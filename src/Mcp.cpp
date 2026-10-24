#include "yahat/Mcp.h"
#include "yahat/logging.h"

#ifdef YAHAT_ENABLE_MCP
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <charconv>

namespace yahat {
namespace {
namespace json = boost::json;
namespace net = boost::asio;
namespace http = boost::beast::http;

struct ActiveCall {
    std::mutex& mutex;
    size_t& count;
    bool acquired = false;
    ~ActiveCall() {
        if (acquired) { std::lock_guard lock{mutex}; --count; }
    }
};

json::object rpcError(json::value id, int code, std::string_view message, std::optional<json::value> data = {}) {
    auto reply = JsonRpc::error(std::move(id), code, message, std::move(data));
    // MCP errors without a readable request ID omit it (unlike generic JSON-RPC).
    if (reply.at("id").is_null()) reply.erase("id");
    return reply;
}

bool validName(std::string_view name) {
    return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
    });
}

// Reject schema features that would advertise transport behavior we do not
// implement. Domain validation remains the converter's responsibility.
void checkSchemaFeatures(const json::value& value, size_t depth = 0) {
    if (depth > 64) throw std::invalid_argument{"MCP schema exceeds nesting limit"};
    if (value.is_bool()) return;
    if (!value.is_object()) throw std::invalid_argument{"Expected a JSON Schema object or boolean"};
    const auto& schema = value.as_object();
    if (schema.contains("x-mcp-header")) throw std::invalid_argument{"MCP parameter header annotations are not supported"};
    if (const auto* dialect = schema.if_contains("$schema"); dialect && *dialect != "https://json-schema.org/draft/2020-12/schema"
        && *dialect != "https://json-schema.org/draft/2020-12/schema#") throw std::invalid_argument{"Only JSON Schema 2020-12 is supported"};
    if (const auto* ref = schema.if_contains("$ref"); ref && (!ref->is_string() || !ref->as_string().starts_with("#"))) {
        throw std::invalid_argument{"External schema references are not supported"};
    }
    for (const auto key : {"properties", "patternProperties", "$defs", "dependentSchemas"}) {
        if (const auto* children = schema.if_contains(key)) {
            if (!children->is_object()) throw std::invalid_argument{"Invalid schema properties"};
            for (const auto& child : children->as_object()) checkSchemaFeatures(child.value(), depth + 1);
        }
    }
    for (const auto key : {"items", "contains", "additionalProperties", "unevaluatedProperties", "propertyNames", "not", "if", "then", "else"}) {
        if (const auto* child = schema.if_contains(key)) checkSchemaFeatures(*child, depth + 1);
    }
    for (const auto key : {"anyOf", "oneOf", "allOf", "prefixItems"}) {
        if (const auto* children = schema.if_contains(key)) {
            if (!children->is_array()) throw std::invalid_argument{"Invalid schema composition"};
            for (const auto& child : children->as_array()) checkSchemaFeatures(child, depth + 1);
        }
    }
}

bool validId(const json::value& id) { return id.is_string() || id.is_int64() || id.is_uint64(); }

std::optional<json::object> validate(const json::value& message, bool check_version = true) {
    if (!message.is_object()) return rpcError(nullptr, JsonRpc::invalid_request, "Expected one MCP request object");
    const auto& obj = message.as_object();
    const auto* id = obj.if_contains("id");
    const auto* method = obj.if_contains("method");
    const auto* version = obj.if_contains("jsonrpc");
    const auto* params = obj.if_contains("params");
    if (!version || *version != "2.0" || !method || !method->is_string() || (id && !validId(*id))) {
        return rpcError(nullptr, JsonRpc::invalid_request, "Invalid MCP request envelope");
    }
    if (!id) return rpcError(nullptr, JsonRpc::invalid_request, "This server accepts no client notifications");
    if (!params || !params->is_object()) return rpcError(*id, JsonRpc::invalid_params, "Expected params object with request metadata");
    const auto* meta = params->as_object().if_contains("_meta");
    if (!meta || !meta->is_object()) return rpcError(*id, JsonRpc::invalid_params, "Missing request metadata");
    const auto* protocol = meta->as_object().if_contains("io.modelcontextprotocol/protocolVersion");
    const auto* caps = meta->as_object().if_contains("io.modelcontextprotocol/clientCapabilities");
    if (!protocol || !protocol->is_string() || !caps || !caps->is_object()) {
        return rpcError(*id, JsonRpc::invalid_params, "Missing or invalid protocolVersion/clientCapabilities metadata");
    }
    if (const auto* info = meta->as_object().if_contains("io.modelcontextprotocol/clientInfo")) {
        if (!info->is_object() || !info->as_object().if_contains("name") || !info->as_object().at("name").is_string()
            || !info->as_object().if_contains("version") || !info->as_object().at("version").is_string()) {
            return rpcError(*id, JsonRpc::invalid_params, "Invalid clientInfo metadata");
        }
    }
    if (const auto* token = meta->as_object().if_contains("progressToken"); token && !validId(*token)) {
        return rpcError(*id, JsonRpc::invalid_params, "Invalid progress token");
    }
    if (check_version && std::string_view{protocol->as_string()} != McpServer::protocol_version) {
        return rpcError(*id, -32022, "Unsupported protocol version", json::object{
            {"supported", json::array{json::string{McpServer::protocol_version}}}, {"requested", *protocol}});
    }
    return {};
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
bool iequals(std::string_view a, std::string_view b) {
    return boost::beast::iequals({a.data(), a.size()}, {b.data(), b.size()});
}
bool accepts(std::string_view header, std::string_view type) {
    while (!header.empty()) {
        const auto comma = header.find(',');
        auto item = trim(header.substr(0, comma));
        const auto semi = item.find(';');
        const auto media = trim(item.substr(0, semi));
        bool enabled = true;
        while (semi != std::string_view::npos && item.find(';') != std::string_view::npos) {
            item.remove_prefix(item.find(';') + 1);
            const auto part = trim(item.substr(0, item.find(';')));
            const auto eq = part.find('=');
            if (eq != std::string_view::npos && iequals(trim(part.substr(0, eq)), "q")) {
                const auto text = trim(part.substr(eq + 1));
                double quality = 0;
                const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), quality);
                enabled = ec == std::errc{} && end == text.data() + text.size() && quality > 0 && quality <= 1;
            }
        }
        if (iequals(media, type) && enabled) return true;
        if (comma == std::string_view::npos) break;
        header.remove_prefix(comma + 1);
    }
    return false;
}

Response jsonResponse(json::object body, int status = 200) {
    Response reply{status, std::string{http::obsolete_reason(static_cast<http::status>(status))}, json::serialize(body)};
    reply.mime_type = "application/json";
    reply.headers.set(http::field::cache_control, "no-store");
    return reply;
}

// Names generated by this API are ASCII tokens. Decode sentinel headers as the
// transport requires, including encoded ASCII names, with strict canonical base64.
std::optional<std::string> headerName(std::string_view text) {
    if (!(text.starts_with("=?base64?") && text.ends_with("?="))) {
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; })) return {};
        return std::string{text};
    }
    text.remove_prefix(9);
    text.remove_suffix(2);
    if (text.empty() || text.size() % 4) return {};
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    for (size_t i = 0; i < text.size(); i += 4) {
        unsigned value = 0;
        int padding = 0;
        for (size_t j = 0; j < 4; ++j) {
            if (text[i + j] == '=') {
                if (i + 4 != text.size() || j < 2) return {};
                ++padding;
                value <<= 6;
            } else {
                const auto pos = alphabet.find(text[i + j]);
                if (pos == std::string_view::npos || padding) return {};
                value = (value << 6) | static_cast<unsigned>(pos);
            }
        }
        if (padding > 2 || (padding == 2 && (value & 0xffff)) || (padding == 1 && (value & 0xff))) return {};
        decoded.push_back(static_cast<char>(value >> 16));
        if (padding < 2) decoded.push_back(static_cast<char>(value >> 8));
        if (!padding) decoded.push_back(static_cast<char>(value));
    }
    return decoded;
}
}

std::string mcp_detail::pointerToken(std::string_view text) {
    std::string result;
    for (const char c : text) {
        if (c == '~') result += "~0";
        else if (c == '/') result += "~1";
        else result += c;
    }
    return result;
}

ArgumentError::ArgumentError(std::string path, std::string expected, const json::value* actual)
    : ToolError{path + ": expected " + expected + "; received " + (actual ? std::string{json::to_string(actual->kind())} : "missing")} {}

ToolContext::ToolContext(net::any_io_executor executor, Auth auth, std::shared_ptr<std::atomic_bool> cancelled)
    : executor_{std::move(executor)}, auth_{std::move(auth)}, cancelled_{std::move(cancelled)} {
    if (!cancelled_) throw std::invalid_argument{"Null cancellation state"};
}
void ToolContext::throwIfCancelled() const { if (isCancelled()) throw ToolError{"Request cancelled"}; }
net::awaitable<void> ToolContext::progress(double completed, std::optional<double> total, std::string message) const {
    throwIfCancelled();
    if (!std::isfinite(completed) || completed < 0 || (total && (!std::isfinite(*total) || *total <= 0 || completed > *total))) {
        throw ToolError{"Invalid progress value"};
    }
    if (progress_) {
        json::object params{{"progress", completed}};
        if (total) params["total"] = *total;
        if (!message.empty()) params["message"] = std::move(message);
        co_await net::co_spawn(executor_, progress_(std::move(params)), net::use_awaitable);
    }
}

ToolResult ToolResult::fromValue(json::value value) {
    ToolResult result;
    result.content.push_back(json::object{{"type", "text"}, {"text", value.is_string() ? value.as_string() : json::string{json::serialize(value)}}});
    result.structured_content = std::move(value);
    return result;
}

McpServer::McpServer(Identity identity) : McpServer{std::move(identity), Options{}} {}
McpServer::McpServer(Identity identity, Options options) : identity_{std::move(identity)}, options_{options} {
    if (!validName(identity_.name) || identity_.version.empty() || identity_.version.size() > 128 || !options_.page_size || !options_.max_input_bytes
        || options_.max_output_bytes < 1024 || options_.max_output_bytes > (std::numeric_limits<size_t>::max() - 1024) / 2 || !options_.max_help_bytes || !options_.max_concurrent_calls || !options_.calls_per_minute) {
        throw std::invalid_argument{"Invalid MCP identity or limits"};
    }
}
void McpServer::add(ToolDescriptor tool) {
    if (frozen_) throw std::logic_error{"MCP registry is frozen"};
    if (!validName(tool.name) || !tool.invoke || tool.description.empty()) throw std::invalid_argument{"Invalid MCP tool descriptor"};
    const auto name = tool.name;
    if (!tools_.emplace(name, std::move(tool)).second) throw std::invalid_argument{"Duplicate MCP tool name"};
}
void McpServer::freeze() {
    if (frozen_) return;
    if (options_.enable_help && (identity_.name.size() + 5 > 128 || tools_.contains(identity_.name + "_help"))) {
        throw std::invalid_argument{"Built-in MCP help name is invalid or collides with a tool"};
    }
    for (const auto& [name, tool] : tools_) {
        if (!tool.input_schema.if_contains("type") || tool.input_schema.at("type") != "object") throw std::invalid_argument{"Tool inputSchema must describe an object"};
        checkSchemaFeatures(tool.input_schema);
        for (const auto& [topic, text] : tool.help) {
            if (topic.empty() || (!tool.help_provider && text.size() > options_.max_help_bytes)) throw std::invalid_argument{"Invalid or oversized help topic"};
        }
    }
    if (options_.enable_help) {
        add(makeTool(identity_.name + "_help", "Get a tool overview and help topics, or retrieve one named topic.",
            {{"tool", "Public tool name; omit to list documented tools"}, {"topic", "Topic key from the tool overview"}},
            [this](ToolContext context, std::optional<std::string> name, std::optional<std::string> topic) -> net::awaitable<json::object> {
                co_return help(std::move(name), std::move(topic), context);
            }));
    }
    catalog_id_ = boost::uuids::to_string(generateUuid());
    frozen_.store(true, std::memory_order_release);
    LOG_INFO << "MCP server " << identity_.name << " frozen with " << tools_.size() << " tools";
}

json::object McpServer::result(json::value id, json::object body) const {
    body["resultType"] = "complete";
    body["_meta"] = json::object{{"io.modelcontextprotocol/serverInfo", json::object{{"name", identity_.name}, {"version", identity_.version}}}};
    return JsonRpc::result(std::move(id), std::move(body));
}

json::object McpServer::help(std::optional<std::string> name, std::optional<std::string> topic, const ToolContext& context) const {
    json::object reply;
    if (!name) {
        if (topic) throw ToolError{"Select a tool before selecting a help topic"};
        json::array names;
        for (const auto& [key, tool] : tools_) {
            try { if (tool.authorize) tool.authorize(context); }
            catch (const ToolError&) { continue; }
            if (!tool.help.empty()) names.push_back(json::string{key});
        }
        reply["tools"] = std::move(names);
    } else {
        const auto it = tools_.find(*name);
        if (it == tools_.end()) throw ToolError{"Unknown help target"};
        const auto& tool = it->second;
        if (tool.authorize) tool.authorize(context);
        reply["tool"] = *name;
        if (!topic) {
            reply["description"] = tool.description;
            json::array topics;
            for (const auto& [key, unused] : tool.help) topics.push_back(json::string{key});
            reply["topics"] = std::move(topics);
        } else {
            const auto doc = tool.help.find(*topic);
            if (doc == tool.help.end()) throw ToolError{"Unknown help topic; request the tool overview for available topics"};
            auto text = tool.help_provider ? tool.help_provider(*topic) : doc->second;
            if (text.size() > options_.max_help_bytes) throw ToolError{"Help topic exceeds the configured limit; split it into smaller topics"};
            reply["topic"] = *topic;
            reply["text"] = std::move(text);
        }
    }
    if (json::serialize(reply).size() > options_.max_help_bytes) throw ToolError{"Help index exceeds the configured limit; select a specific tool or topic"};
    return reply;
}

net::awaitable<json::object> McpServer::dispatch(json::value message, ToolContext context) const {
    if (!frozen_.load(std::memory_order_acquire)) throw std::logic_error{"Freeze the MCP registry before dispatch"};
    if (json::serialize(message).size() > options_.max_input_bytes) co_return rpcError(nullptr, JsonRpc::invalid_request, "Request exceeds input limit");
    if (auto error = validate(message)) co_return std::move(*error);
    auto& obj = message.as_object();
    const auto id = obj.at("id");
    const auto& method = obj.at("method").as_string();
    const auto& params = obj.at("params").as_object();
    context.request_id = id;
    context.metadata = params.at("_meta").as_object();
    auto bounded = [&](json::object response) {
        if (json::serialize(response).size() > options_.max_output_bytes) return rpcError(id, JsonRpc::internal_error, "Response exceeds output limit");
        return response;
    };
    if (method == "server/discover") {
        json::object body{{"supportedVersions", json::array{json::string{protocol_version}}}, {"capabilities", json::object{{"tools", json::object{}}}},
            {"ttlMs", options_.catalog_ttl_ms}, {"cacheScope", "public"}};
        if (!identity_.instructions.empty()) body["instructions"] = identity_.instructions;
        co_return bounded(result(id, std::move(body)));
    }
    if (method == "tools/list") {
        size_t offset = 0;
        if (const auto* cursor = params.if_contains("cursor")) {
            if (!cursor->is_string()) co_return rpcError(id, JsonRpc::invalid_params, "Invalid tools cursor");
            const std::string_view text{cursor->as_string()};
            const auto prefix = catalog_id_ + ":";
            if (!text.starts_with(prefix)) co_return rpcError(id, JsonRpc::invalid_params, "Stale or invalid tools cursor");
            const auto suffix = text.substr(prefix.size());
            const auto [end, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), offset);
            if (ec != std::errc{} || end != suffix.data() + suffix.size() || offset >= tools_.size()) co_return rpcError(id, JsonRpc::invalid_params, "Invalid tools cursor");
        }
        json::array list;
        auto it = tools_.begin();
        std::advance(it, offset);
        for (; it != tools_.end() && list.size() < options_.page_size; ++it) {
            const auto& tool = it->second;
            json::object item{{"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.input_schema}};
            if (!tool.annotations.empty()) item["annotations"] = tool.annotations;
            list.push_back(std::move(item));
        }
        json::object body{{"tools", std::move(list)}, {"ttlMs", options_.catalog_ttl_ms}, {"cacheScope", "public"}};
        if (it != tools_.end()) body["nextCursor"] = catalog_id_ + ":" + std::to_string(offset + options_.page_size);
        co_return bounded(result(id, std::move(body)));
    }
    if (method != "tools/call") co_return rpcError(id, JsonRpc::method_not_found, "Method not found; supported MCP version: 2026-07-28");
    const auto* name = params.if_contains("name");
    const auto* arguments = params.if_contains("arguments");
    if (!name || !name->is_string() || (arguments && !arguments->is_object())) co_return rpcError(id, JsonRpc::invalid_params, "Expected tool name and arguments object");
    const auto it = tools_.find(std::string_view{name->as_string()});
    if (it == tools_.end()) co_return rpcError(id, JsonRpc::invalid_params, "Unknown tool");
    const auto& tool = it->second;
    // No lock is held while validating or executing application code.
    ActiveCall active_call{limits_mutex_, active_calls_};
    ToolResult tool_result;
    try {
        context.throwIfCancelled();
        if (tool.authorize) tool.authorize(context);
        {
            std::lock_guard lock{limits_mutex_};
            const auto now = std::chrono::steady_clock::now();
            if (now - window_start_ >= std::chrono::minutes{1}) { window_start_ = now; window_calls_ = 0; }
            if (active_calls_ >= options_.max_concurrent_calls || window_calls_ >= options_.calls_per_minute) throw ToolError{"Tool call limit reached; retry later"};
            ++active_calls_;
            ++window_calls_;
            active_call.acquired = true;
        }
        LOG_DEBUG << "Invoking MCP tool " << tool.name;
        tool_result = co_await tool.invoke(arguments ? arguments->as_object() : json::object{}, context);
        LOG_DEBUG << "MCP tool completed: " << tool.name;
    } catch (const ToolError& ex) {
        tool_result.content = {json::object{{"type", "text"}, {"text", ex.what()}}};
        tool_result.is_error = true;
        LOG_DEBUG << "MCP tool returned an execution error: " << tool.name;
    } catch (...) {
        LOG_ERROR << "Unhandled exception in MCP tool " << tool.name;
        tool_result.content = {json::object{{"type", "text"}, {"text", "Internal tool error"}}};
        tool_result.is_error = true;
    }
    json::object body{{"content", std::move(tool_result.content)}};
    if (tool_result.structured_content) body["structuredContent"] = std::move(*tool_result.structured_content);
    if (tool_result.is_error) body["isError"] = true;
    auto response = result(id, std::move(body));
    if (json::serialize(response).size() > options_.max_output_bytes) {
        response = result(id, {{"isError", true}, {"content", json::array{json::object{{"type", "text"}, {"text", "Tool result exceeds output limit; request a smaller result"}}}}});
    }
    co_return response;
}

class McpServer::Handler : public RequestHandler {
public:
    Handler(HttpServer& http, std::shared_ptr<McpServer> owner) : http_{http}, owner_{std::move(owner)} {}
    Response onReqest(const Request& request) override;
private:
    class Call : public SseQueueHandler, public std::enable_shared_from_this<Call> {
    public:
        Call(HttpServer& http, std::shared_ptr<McpServer> owner, json::value message, ToolContext context)
            : SseQueueHandler{http}, owner_{std::move(owner)}, message_{std::move(message)}, context_{std::move(context)} {
            // Reserve room for the final response even after the progress budget is exhausted.
            setQueueLimit(2 * owner_->options_.max_output_bytes + 1024);
            setHeartbeatInterval(std::chrono::seconds{15});
            headers.set("X-Accel-Buffering", "no");
            headers.set(http::field::cache_control, "no-store");
        }
    protected:
        Response proceeding() override {
            auto self = shared_from_this();
            if (const auto* token = message_.as_object().at("params").as_object().at("_meta").as_object().if_contains("progressToken")) {
                const auto progress_token = *token;
                std::weak_ptr<Call> weak = self;
                context_.progress_ = [weak, progress_token](json::object params) -> net::awaitable<void> {
                    if (auto call = weak.lock(); call && !call->context_.isCancelled()) {
                        const auto completed = params.at("progress").as_double();
                        if (call->last_progress_ && completed <= *call->last_progress_) throw ToolError{"Progress must increase with each notification"};
                        params["progressToken"] = progress_token;
                        auto text = json::serialize(json::object{{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", std::move(params)}});
                        const auto bytes = text.size() + 23;
                        if (bytes > call->owner_->options_.max_output_bytes - call->progress_bytes_) throw ToolError{"Progress output limit reached"};
                        call->progress_bytes_ += bytes;
                        call->last_progress_ = completed;
                        call->sendSse("message", text);
                        co_await net::post(call->context_.executor(), net::use_awaitable);
                    }
                    co_return;
                };
            }
            // Defer until the queue initializes the SSE headers and disconnect monitor.
            net::post(getYield().get_executor(), [self] {
                net::co_spawn(self->context_.executor(), self->owner_->dispatch(std::move(self->message_), self->context_),
                    [self](std::exception_ptr failure, json::object response) {
                        if (!self->context_.isCancelled()) {
                            try {
                                if (failure) response = rpcError(self->context_.request_id, JsonRpc::internal_error, "Internal error");
                                self->sendSse("message", json::serialize(response));
                            } catch (...) { LOG_ERROR << "MCP response stream exceeded its queue limit"; }
                        }
                        self->finishSse();
                    });
            });
            return SseQueueHandler::proceeding();
        }
    private:
        std::shared_ptr<McpServer> owner_;
        json::value message_;
        ToolContext context_;
        size_t progress_bytes_ = 0;
        std::optional<double> last_progress_;
    };
    HttpServer& http_;
    std::shared_ptr<McpServer> owner_;
};

Response McpServer::Handler::onReqest(const Request& request) {
    if (request.type != Request::Type::POST) {
        Response reply{405, "Method Not Allowed"}; reply.headers.set(http::field::allow, "POST"); return reply;
    }
    if (request.body.size() > owner_->options_.max_input_bytes) return jsonResponse(rpcError(nullptr, JsonRpc::invalid_request, "Request exceeds input limit"), 413);
    auto content_type = request.getHeader("Content-Type");
    if (request.headers.count(http::field::content_type) != 1 || !iequals(trim(content_type.substr(0, content_type.find(';'))), "application/json")) return {415, "Unsupported Media Type"};
    std::string accept;
    for (const auto& field : request.headers) {
        if (field.name() == http::field::accept) { if (!accept.empty()) accept += ","; accept.append(field.value().data(), field.value().size()); }
    }
    if (!accepts(accept, "application/json") || !accepts(accept, "text/event-stream")) return {406, "Not Acceptable"};
    boost::system::error_code ec;
    auto message = json::parse(request.body, ec);
    if (ec) return jsonResponse(rpcError(nullptr, JsonRpc::parse_error, "Parse error"), 400);
    // Validate the envelope first, then mirrored routing headers, then version
    // support. A header/body mismatch must be reported even for an unsupported
    // body version (the headers may route requests before dispatch).
    if (auto error = validate(message, false)) return jsonResponse(std::move(*error), 400);
    const auto& obj = message.as_object();
    const auto& params = obj.at("params").as_object();
    const auto& id = obj.at("id");
    const auto& method = obj.at("method").as_string();
    const auto mismatch = [&] { return jsonResponse(rpcError(id, -32020, "Missing, malformed, or mismatched MCP routing headers"), 400); };
    if (request.headers.count("MCP-Protocol-Version") != 1 || request.headers.count("Mcp-Method") != 1
        || request.getHeader("MCP-Protocol-Version") != std::string_view{params.at("_meta").as_object().at("io.modelcontextprotocol/protocolVersion").as_string()}
        || request.getHeader("Mcp-Method") != std::string_view{method}) return mismatch();
    if (method == "tools/call") {
        const auto* name = params.if_contains("name");
        const auto decoded = headerName(request.getHeader("Mcp-Name"));
        if (request.headers.count("Mcp-Name") != 1 || !decoded || !name || !name->is_string() || *decoded != name->as_string()) return mismatch();
    }
    if (auto error = validate(message)) return jsonResponse(std::move(*error), 400);
    if (method == "tools/call") {
        const auto* name = params.if_contains("name");
        if (!owner_->tools_.contains(std::string_view{name->as_string()})) return jsonResponse(rpcError(id, JsonRpc::invalid_params, "Unknown tool"), 400);
        if (const auto* args = params.if_contains("arguments"); args && !args->is_object()) return jsonResponse(rpcError(id, JsonRpc::invalid_params, "Expected arguments object"), 400);
    }
    ToolContext context{request.yield ? request.yield->get_executor() : http_.getCtx().get_executor(), request.auth, request.cancelled};
    context.request_id = id;
    context.correlation_id = boost::uuids::to_string(request.uuid);
    if (method == "tools/call") {
        Response response;
        response.setContinuation(std::make_shared<Call>(http_, owner_, std::move(message), std::move(context)));
        return response;
    }
    if (!request.yield) throw std::logic_error{"MCP HTTP handler requires an HTTP coroutine"};
    const auto executor = context.executor();
    auto response = net::co_spawn(executor, owner_->dispatch(std::move(message), std::move(context)), *request.yield);
    const auto* error = response.if_contains("error");
    const int status = error ? (error->as_object().at("code") == JsonRpc::method_not_found ? 404 : 400) : 200;
    return jsonResponse(std::move(response), status);
}

void McpServer::mount(HttpServer& http_server, std::string path, RouteOptions options) {
    auto self = shared_from_this();
    freeze();
    if (path.empty() || path.front() != '/' || path.find_first_of("?#") != std::string::npos) throw std::invalid_argument{"Invalid MCP endpoint path"};
    options.exact_match = true;
    options.allowed_methods = {Request::Type::POST};
    if (!options.allowed_origins) options.allowed_origins = std::vector<std::string>{};
    // Applications may configure CORS/preflight explicitly through RouteOptions.
    if (!options.auto_handle_cors) options.auto_handle_cors = false;
    http_server.addRouteWithOptions(path, std::make_shared<Handler>(http_server, std::move(self)), options);
    LOG_INFO << "Mounted MCP server " << identity_.name << " at " << path;
}

} // namespace yahat
#endif
