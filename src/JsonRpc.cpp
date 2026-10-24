#include "yahat/JsonRpc.h"
#include "yahat/logging.h"

#ifdef YAHAT_ENABLE_JSON_RPC
namespace yahat {

JsonRpc::Error::Error(int code, std::string message, std::optional<boost::json::value> data)
    : std::runtime_error{std::move(message)}, code_{code}, data_{std::move(data)} {}

JsonRpc::JsonRpc() : JsonRpc{Options{}} {}
JsonRpc::JsonRpc(Options options) : options_{options} {
    if (options_.max_batch_size == 0) {
        throw std::invalid_argument{"JSON-RPC batch limit must be positive"};
    }
}

void JsonRpc::addMethod(std::string name, method_t handler) {
    if (name.starts_with("rpc.") || !handler) {
        throw std::invalid_argument{"Invalid JSON-RPC method registration"};
    }
    std::unique_lock lock{mutex_};
    if (async_methods_.contains(name)) throw std::invalid_argument{"JSON-RPC method already registered"};
    if (!methods_.emplace(std::move(name), std::move(handler)).second) {
        throw std::invalid_argument{"JSON-RPC method already registered"};
    }
}

bool JsonRpc::removeMethod(std::string_view name) {
    std::unique_lock lock{mutex_};
    const auto it = methods_.find(name);
    if (it == methods_.end()) {
        const auto async_it = async_methods_.find(name);
        if (async_it == async_methods_.end()) return false;
        async_methods_.erase(async_it);
        return true;
    }
    methods_.erase(it);
    return true;
}

boost::json::object JsonRpc::result(boost::json::value id, boost::json::value value) {
    return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(value)}};
}

boost::json::object JsonRpc::error(boost::json::value id, int code, std::string_view message,
                                  std::optional<boost::json::value> data) {
    boost::json::object detail{{"code", code}, {"message", boost::json::string{message}}};
    if (data) detail["data"] = std::move(*data);
    return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"error", std::move(detail)}};
}

std::optional<boost::json::value> JsonRpc::handle(std::string_view json, const Request* request) const {
    boost::system::error_code ec;
    const auto message = boost::json::parse({json.data(), json.size()}, ec);
    if (ec) return error(nullptr, parse_error, "Parse error");
    return dispatch(message, request);
}

std::optional<boost::json::value> JsonRpc::dispatch(const boost::json::value& message,
                                                   const Request* request) const {
    if (!message.is_array()) return dispatchOne(message, request);
    const auto& batch = message.as_array();
    if (!options_.allow_batch || batch.empty() || batch.size() > options_.max_batch_size) {
        return error(nullptr, invalid_request, "Invalid Request");
    }
    boost::json::array responses;
    for (const auto& item : batch) {
        if (auto response = dispatchOne(item, request)) responses.push_back(std::move(*response));
    }
    if (responses.empty()) return {};
    return responses;
}

std::optional<boost::json::value> JsonRpc::dispatchOne(const boost::json::value& message,
                                                      const Request* request) const {
    if (!message.is_object()) return error(nullptr, invalid_request, "Invalid Request");
    const auto& object = message.as_object();
    const auto* version = object.if_contains("jsonrpc");
    const auto* method = object.if_contains("method");
    const auto* id = object.if_contains("id");
    const auto* params = object.if_contains("params");
    if (!version || !version->is_string() || version->as_string() != "2.0"
        || !method || !method->is_string()
        || (id && !id->is_null() && !id->is_string() && !id->is_number())
        || (params && !params->is_object() && !params->is_array())) {
        return error(nullptr, invalid_request, "Invalid Request");
    }
    const Context context{request, id ? *id : boost::json::value{}, !id};
    try {
        method_t callback;
        {
            std::shared_lock lock{mutex_};
            const auto& name = method->as_string();
            const auto it = methods_.find(std::string_view{name.data(), name.size()});
            if (it != methods_.end()) callback = it->second;
        }
        if (!callback) {
            if (!id) return {};
            return error(*id, method_not_found, "Method not found");
        }
        auto value = callback(params ? *params : boost::json::value{}, context);
        if (!id) return {};
        return result(*id, std::move(value));
    } catch (const Error& ex) {
        LOG_DEBUG << "JSON-RPC method returned protocol error " << ex.code();
        if (!id) return {};
        return error(*id, ex.code(), ex.what(), ex.data());
    } catch (...) {
        // Do not disclose exception messages, arguments, or credentials to clients.
        LOG_ERROR << "Unhandled exception in JSON-RPC method";
        if (!id) return {};
        return error(*id, internal_error, "Internal error");
    }
}

void JsonRpc::addAsyncMethod(std::string name, async_method_t handler) {
    if (name.starts_with("rpc.") || !handler) throw std::invalid_argument{"Invalid JSON-RPC method registration"};
    std::unique_lock lock{mutex_};
    if (methods_.contains(name) || !async_methods_.emplace(std::move(name), std::move(handler)).second) {
        throw std::invalid_argument{"JSON-RPC method already registered"};
    }
}

boost::asio::awaitable<std::optional<boost::json::value>> JsonRpc::dispatchAsync(
    boost::json::value message, std::shared_ptr<const Request> request) const {
    if (!message.is_array()) co_return co_await dispatchOneAsync(std::move(message), std::move(request));
    const auto& batch = message.as_array();
    if (!options_.allow_batch || batch.empty() || batch.size() > options_.max_batch_size) {
        co_return error(nullptr, invalid_request, "Invalid Request");
    }
    boost::json::array responses;
    for (const auto& item : batch) {
        if (auto response = co_await dispatchOneAsync(item, request)) responses.push_back(std::move(*response));
    }
    if (responses.empty()) co_return std::nullopt;
    co_return responses;
}

boost::asio::awaitable<std::optional<boost::json::value>> JsonRpc::dispatchOneAsync(
    boost::json::value message, std::shared_ptr<const Request> request) const {
    if (!message.is_object()) co_return error(nullptr, invalid_request, "Invalid Request");
    const auto& object = message.as_object();
    const auto* version = object.if_contains("jsonrpc");
    const auto* method = object.if_contains("method");
    const auto* id = object.if_contains("id");
    const auto* params = object.if_contains("params");
    if (!version || *version != "2.0" || !method || !method->is_string()
        || (id && !id->is_null() && !id->is_string() && !id->is_number())
        || (params && !params->is_object() && !params->is_array())) {
        co_return error(nullptr, invalid_request, "Invalid Request");
    }
    async_method_t callback;
    {
        std::shared_lock lock{mutex_};
        const auto it = async_methods_.find(std::string_view{method->as_string()});
        if (it != async_methods_.end()) callback = it->second;
    }
    if (!callback) co_return dispatchOne(message, request.get());
    try {
        const Context context{request.get(), id ? *id : boost::json::value{}, !id};
        auto value = co_await callback(params ? *params : boost::json::value{}, context);
        if (!id) co_return std::nullopt;
        co_return result(*id, std::move(value));
    } catch (const Error& ex) {
        LOG_DEBUG << "JSON-RPC async method returned protocol error " << ex.code();
        if (!id) co_return std::nullopt;
        co_return error(*id, ex.code(), ex.what(), ex.data());
    } catch (...) {
        LOG_ERROR << "Unhandled exception in JSON-RPC async method";
        if (!id) co_return std::nullopt;
        co_return error(*id, internal_error, "Internal error");
    }
}

JsonRpcHandler::JsonRpcHandler(std::shared_ptr<JsonRpc> dispatcher)
    : dispatcher_{std::move(dispatcher)} {
    if (!dispatcher_) throw std::invalid_argument{"JSON-RPC dispatcher cannot be null"};
}

Response JsonRpcHandler::onReqest(const Request& request) {
    if (request.type != Request::Type::POST) {
        Response response{405, "Method Not Allowed"};
        response.headers.set(boost::beast::http::field::allow, "POST");
        return response;
    }
    auto media_type = request.getHeader("Content-Type");
    media_type = media_type.substr(0, media_type.find(';'));
    while (!media_type.empty() && (media_type.back() == ' ' || media_type.back() == '\t')) {
        media_type.remove_suffix(1);
    }
    while (!media_type.empty() && (media_type.front() == ' ' || media_type.front() == '\t')) {
        media_type.remove_prefix(1);
    }
    if (!boost::beast::iequals({media_type.data(), media_type.size()}, "application/json")) {
        return {415, "Unsupported Media Type"};
    }
    if (auto value = dispatcher_->handle(request.body, &request)) {
        Response response{200, "OK", boost::json::serialize(*value)};
        response.mime_type = "application/json";
        return response;
    }
    Response response{202, "Accepted"};
    response.generate_status_body = false;
    return response;
}

} // namespace yahat
#endif
