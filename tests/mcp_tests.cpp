#include <gtest/gtest.h>
#include "yahat/Mcp.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/beast/core.hpp>
#include <thread>

struct DomainInput { int count; };
struct DomainOutput { int count; };
void tag_invoke(boost::json::value_from_tag, boost::json::value& output, const DomainOutput& value) {
    output = boost::json::object{{"count", value.count}};
}
namespace yahat {
template<> struct McpConverter<DomainInput> {
    static DomainInput decode(const boost::json::value& value, const std::string& path) {
        if (!value.is_object() || !value.as_object().if_contains("count")) throw ArgumentError{path, "object with count", &value};
        auto count = McpConverter<int>::decode(value.as_object().at("count"), path + "/count");
        if (count < 0) throw ToolError{path + "/count: must be nonnegative"};
        return {count};
    }
    static boost::json::object schema() {
        return {{"type", "object"}, {"properties", boost::json::object{{"count", boost::json::object{{"type", "integer"}, {"minimum", 0}}}}},
            {"required", boost::json::array{"count"}}};
    }
};
}

namespace {
using namespace yahat;
namespace net = boost::asio;
namespace json = boost::json;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

json::object request(std::string method, json::object params = {}, json::value id = 1) {
    params["_meta"] = json::object{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
        {"io.modelcontextprotocol/clientCapabilities", json::object{}}};
    return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"method", std::move(method)}, {"params", std::move(params)}};
}
json::object call(std::string name, json::object arguments = {}) { return request("tools/call", {{"name", std::move(name)}, {"arguments", std::move(arguments)}}); }

json::object run(McpServer& server, json::value message, Auth auth = {}) {
    net::io_context ctx;
    auto future = net::co_spawn(ctx, server.dispatch(std::move(message), ToolContext{ctx.get_executor(), std::move(auth)}), net::use_future);
    ctx.run();
    return future.get();
}
net::awaitable<int> sum(int a, int b) { co_return a + b; }
net::awaitable<std::string> echo(std::string text, std::optional<int> count) { co_return text + std::to_string(count.value_or(1)); }

TEST(Mcp, TypedToolsAndFrozenDeterministicCatalog) {
    McpServer::Options options;
    options.page_size = 1;
    options.enable_help = false;
    McpServer server{{"test", "1"}, options};
    server.add(makeTool("z_sum", "Add two integers", {"a", "b"}, sum));
    server.add(makeTool("a_echo", "Echo text", {"text", "count"}, echo));
    server.freeze();
    EXPECT_THROW(server.add(makeTool("late", "Too late", {"a", "b"}, sum)), std::logic_error);
    auto first = run(server, request("tools/list"));
    const auto& body = first.at("result").as_object();
    EXPECT_EQ(body.at("tools").as_array()[0].as_object().at("name"), "a_echo");
    EXPECT_EQ(body.at("cacheScope"), "public");
    EXPECT_EQ(body.at("resultType"), "complete");
    EXPECT_EQ(body.at("tools").as_array()[0].as_object().at("inputSchema").as_object().at("required").as_array().size(), 1);
    const auto second = run(server, request("tools/list", {{"cursor", body.at("nextCursor")}}));
    EXPECT_EQ(second.at("result").as_object().at("tools").as_array()[0].as_object().at("name"), "z_sum");
    EXPECT_FALSE(second.at("result").as_object().contains("nextCursor"));
    EXPECT_TRUE(run(server, request("tools/list", {{"cursor", "bad"}})).contains("error"));
    EXPECT_EQ(run(server, call("z_sum", {{"a", 2}, {"b", 3}})).at("result").as_object().at("structuredContent"), 5);
    EXPECT_EQ(run(server, call("a_echo", {{"text", "x"}})).at("result").as_object().at("structuredContent"), "x1");
    EXPECT_EQ(run(server, call("a_echo", {{"text", "x"}, {"count", nullptr}})).at("result").as_object().at("structuredContent"), "x1");
}

TEST(Mcp, StrictConversionsAndNestedPaths) {
    McpServer server{{"test", "1"}};
    server.add(makeTool("sum", "Add", {"a", "b"}, sum));
    server.add(makeTool("nested", "Nested values", {"items"}, [](std::vector<std::map<std::string, int>> items) -> net::awaitable<size_t> { co_return items.size(); }));
    server.freeze();
    for (const auto& args : std::vector<json::object>{{{"a", 2}}, {{"a", "2"}, {"b", 1}}, {{"a", 2.5}, {"b", 1}},
        {{"a", 2147483648ULL}, {"b", 1}}, {{"a", nullptr}, {"b", 1}}, {{"a", 1}, {"b", 1}, {"extra", true}}}) {
        const auto reply = run(server, call("sum", args));
        ASSERT_TRUE(reply.contains("result"));
        EXPECT_EQ(reply.at("result").as_object().at("isError"), true);
    }
    auto bad = run(server, call("nested", {{"items", json::array{json::object{{"a/b~", "secret-input"}}}}}));
    auto text = json::serialize(bad);
    EXPECT_NE(text.find("/items/0/a~1b~0"), std::string::npos);
    EXPECT_EQ(text.find("secret-input"), std::string::npos);
    const auto ok = run(server, call("nested", {{"items", json::array{json::object{{"x", 4}}}}}));
    EXPECT_EQ(ok.at("result").as_object().at("structuredContent"), 1);
    EXPECT_THROW(makeTool("bad", "Bad", {"a"}, sum), std::invalid_argument);
    EXPECT_THROW(makeTool("bad", "Bad", {"a", "a"}, sum), std::invalid_argument);
}

TEST(Mcp, CustomConversionRawJsonAndScalarTypes) {
    McpServer server{{"test", "1"}};
    server.add(makeTool("custom", "Custom types", {"value"}, [](DomainInput value) -> net::awaitable<DomainOutput> { co_return DomainOutput{value.count}; }));
    server.add(makeTool("raw", "Raw JSON", {"value"}, [](json::value value) -> net::awaitable<json::value> { co_return value; }));
    server.add(makeTool("scalars", "Scalar types", {"enabled", "number", "small"}, [](bool enabled, double number, int8_t small) -> net::awaitable<double> { co_return enabled ? number + small : 0; }));
    server.freeze();
    EXPECT_EQ(run(server, call("custom", {{"value", json::object{{"count", 4}}}})).at("result").as_object().at("structuredContent").as_object().at("count"), 4);
    EXPECT_EQ(run(server, call("custom", {{"value", json::object{{"count", -1}}}})).at("result").as_object().at("isError"), true);
    EXPECT_EQ(run(server, call("raw", {{"value", json::array{1, true, nullptr}}})).at("result").as_object().at("structuredContent"), (json::array{1, true, nullptr}));
    EXPECT_EQ(run(server, call("scalars", {{"enabled", true}, {"number", 1.5}, {"small", 2}})).at("result").as_object().at("structuredContent"), 3.5);
    EXPECT_EQ(run(server, call("scalars", {{"enabled", 1}, {"number", 1.5}, {"small", 2}})).at("result").as_object().at("isError"), true);
}

TEST(Mcp, FreezeRejectsUnsupportedSchemaTransportFeatures) {
    McpServer server{{"test", "1"}};
    auto tool = makeTool("sum", "Add", {"a", "b"}, sum);
    tool.input_schema.at("properties").as_object().at("a").as_object()["x-mcp-header"] = "Tenant";
    server.add(std::move(tool));
    EXPECT_THROW(server.freeze(), std::invalid_argument);
}

TEST(Mcp, ProtocolValidationAndDiscovery) {
    McpServer server{{"test", "1", "Use the test tools"}};
    server.freeze();
    auto discover = run(server, request("server/discover", {}, "discover-id"));
    EXPECT_EQ(discover.at("id"), "discover-id");
    EXPECT_EQ(discover.at("result").as_object().at("supportedVersions").as_array()[0], "2026-07-28");
    EXPECT_EQ(discover.at("result").as_object().at("_meta").as_object().at("io.modelcontextprotocol/serverInfo").as_object().at("name"), "test");
    EXPECT_EQ(run(server, call("missing")).at("error").as_object().at("code"), -32602);
    auto invalid = request("tools/list");
    invalid["id"] = nullptr;
    EXPECT_FALSE(run(server, invalid).contains("id"));
    invalid = request("tools/list");
    invalid.at("params").as_object().erase("_meta");
    EXPECT_EQ(run(server, invalid).at("error").as_object().at("code"), -32602);
    invalid = request("tools/list");
    invalid.at("params").as_object().at("_meta").as_object()["io.modelcontextprotocol/protocolVersion"] = "2025-11-25";
    EXPECT_EQ(run(server, invalid).at("error").as_object().at("code"), -32022);
    EXPECT_TRUE(run(server, json::array{request("tools/list")}).contains("error"));
}

TEST(Mcp, BoundMethodsContextAuthorizationAndSafeErrors) {
    struct Application {
        net::awaitable<std::string> who(ToolContext context, std::string prefix) {
            net::steady_timer timer{co_await net::this_coro::executor, 1ms};
            co_await timer.async_wait(net::use_awaitable);
            co_return prefix + context.auth().account;
        }
    } app;
    McpServer server{{"test", "1"}};
    auto tool = makeTool("who", "Caller", {"prefix"}, app, &Application::who);
    tool.authorize = [](const ToolContext& context) { if (!context.auth().access) throw ToolError{"Permission denied"}; };
    server.add(std::move(tool));
    server.add(makeTool("fail", "Private error", {}, []() -> net::awaitable<int> { throw std::runtime_error{"super-secret"}; co_return 0; }));
    server.add(makeTool("domain", "Domain error", {}, []() -> net::awaitable<int> { throw ToolError{"Choose a valid date"}; co_return 0; }));
    server.freeze();
    Auth auth; auth.access = true; auth.account = "alice";
    const auto success = run(server, call("who", {{"prefix", "hello "}}), auth);
    EXPECT_EQ(success.at("result").as_object().at("structuredContent"), "hello alice");
    EXPECT_EQ(run(server, call("who", {{"prefix", "hello "}})).at("result").as_object().at("isError"), true);
    EXPECT_EQ(json::serialize(run(server, call("fail"))).find("super-secret"), std::string::npos);
    EXPECT_NE(json::serialize(run(server, call("domain"))).find("Choose a valid date"), std::string::npos);
}

TEST(Mcp, BoundedLazyHelpAndCollisions) {
    McpServer server{{"test", "1"}};
    auto tool = makeTool("sum", "Add", {"a", "b"}, sum);
    tool.help["examples"] = "";
    int loads = 0;
    tool.help_provider = [&](std::string_view topic) { ++loads; return std::string{topic} + ": two plus three is five"; };
    server.add(std::move(tool));
    server.freeze();
    EXPECT_EQ(loads, 0);
    EXPECT_NE(json::serialize(run(server, call("test_help", {{"tool", "sum"}}))).find("examples"), std::string::npos);
    EXPECT_EQ(loads, 0);
    EXPECT_NE(json::serialize(run(server, call("test_help", {{"tool", "sum"}, {"topic", "examples"}}))).find("two plus three"), std::string::npos);
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(run(server, call("test_help", {{"tool", "sum"}, {"topic", "../../private"}})).at("result").as_object().at("isError"), true);
    EXPECT_EQ(loads, 1);
    McpServer conflict{{"test", "1"}};
    conflict.add(makeTool("test_help", "Conflict", {"a", "b"}, sum));
    EXPECT_THROW(conflict.freeze(), std::invalid_argument);
}

TEST(Mcp, ConcurrencyAndRateBounds) {
    McpServer::Options options;
    options.max_concurrent_calls = 1;
    options.calls_per_minute = 1;
    McpServer server{{"test", "1"}, options};
    server.add(makeTool("slow", "Wait", {}, []() -> net::awaitable<bool> {
        net::steady_timer timer{co_await net::this_coro::executor, 5ms};
        co_await timer.async_wait(net::use_awaitable);
        co_return true;
    }));
    server.freeze();
    net::io_context ctx;
    auto first = net::co_spawn(ctx, server.dispatch(call("slow"), ToolContext{ctx.get_executor()}), net::use_future);
    auto second = net::co_spawn(ctx, server.dispatch(call("slow"), ToolContext{ctx.get_executor()}), net::use_future);
    ctx.run();
    EXPECT_EQ(first.get().at("result").as_object().at("structuredContent"), true);
    EXPECT_EQ(second.get().at("result").as_object().at("isError"), true);
    EXPECT_EQ(run(server, call("slow")).at("result").as_object().at("isError"), true);
}

TEST(Mcp, OutputAndInputBounds) {
    McpServer::Options options;
    options.max_output_bytes = 1024;
    options.max_input_bytes = 1024;
    McpServer server{{"test", "1"}, options};
    server.add(makeTool("large", "Large reply", {}, []() -> net::awaitable<std::string> { co_return std::string(2048, 'x'); }));
    server.freeze();
    auto reply = run(server, call("large"));
    EXPECT_EQ(reply.at("result").as_object().at("isError"), true);
    EXPECT_LT(json::serialize(reply).size(), 1024);
    EXPECT_TRUE(run(server, call("large", {{"too-big", std::string(2048, 'x')}})).contains("error"));
}

TEST(Mcp, InvalidConversionsNeverEnterApplicationCode) {
    int entered = 0;
    McpServer server{{"test", "1"}};
    server.add(makeTool("checked", "Validate before invocation", {"small", "enabled", "number", "text", "items"},
        [&](uint8_t small, bool enabled, float number, std::string text, std::vector<int> items) -> net::awaitable<bool> {
            ++entered;
            co_return small == 2 && enabled && number == 3.5f && text == "ok" && items == std::vector<int>{1};
        }));
    server.freeze();
    const json::object valid{{"small", 2}, {"enabled", true}, {"number", 3.5}, {"text", "ok"}, {"items", json::array{1}}};
    for (const auto& [key, value] : std::vector<std::pair<std::string, json::value>>{
        {"small", -1}, {"small", 256}, {"small", 2.0}, {"enabled", "true"},
        {"number", "3.5"}, {"number", 1e100}, {"text", 4}, {"items", json::array{1, false}}, {"extra", true}}) {
        auto args = valid;
        args[key] = value;
        auto reply = run(server, call("checked", args));
        EXPECT_EQ(reply.at("result").as_object().at("isError"), true);
        EXPECT_NE(json::serialize(reply).find("/" + key), std::string::npos);
    }
    auto missing = valid;
    missing.erase("text");
    EXPECT_EQ(run(server, call("checked", missing)).at("result").as_object().at("isError"), true);
    EXPECT_EQ(entered, 0);
    EXPECT_EQ(run(server, call("checked", valid)).at("result").as_object().at("structuredContent"), true);
    EXPECT_EQ(entered, 1);
    EXPECT_EQ(McpConverter<double>::decode(json::value{3}, "/number"), 3.0);
    EXPECT_THROW(McpConverter<float>::decode(json::value{std::numeric_limits<double>::infinity()}, "/number"), ArgumentError);
    EXPECT_THROW(McpConverter<uint64_t>::decode(json::value{-1}, "/number"), ArgumentError);
    EXPECT_EQ(McpConverter<uint64_t>::decode(json::value{UINT64_MAX}, "/number"), UINT64_MAX);
}

TEST(Mcp, RegistrationRejectsInvalidDescriptorsAndDuplicateNames) {
    McpServer server{{"test", "1"}};
    server.add(makeTool("sum", "Add", {"a", "b"}, sum));
    EXPECT_THROW(server.add(makeTool("sum", "Add again", {"a", "b"}, sum)), std::invalid_argument);
    for (const auto& name : {"", "bad name", "bad/name"}) {
        EXPECT_THROW(server.add(makeTool(name, "Invalid name", {"a", "b"}, sum)), std::invalid_argument);
    }
    auto invalid = makeTool("empty", "", {"a", "b"}, sum);
    EXPECT_THROW(server.add(invalid), std::invalid_argument);
    invalid.description = "No handler";
    invalid.invoke = {};
    EXPECT_THROW(server.add(invalid), std::invalid_argument);
    McpServer bad_schema{{"test", "1"}};
    auto schema = makeTool("bad", "Bad schema", {"a", "b"}, sum);
    schema.input_schema = {{"type", "array"}};
    bad_schema.add(std::move(schema));
    EXPECT_THROW(bad_schema.freeze(), std::invalid_argument);
    server.freeze();
    EXPECT_NO_THROW(server.freeze());
}

TEST(Mcp, ConcurrentSuspendedCallsKeepAuthenticationAndMetadataIsolated) {
    net::io_context ctx;
    net::steady_timer barrier{ctx, std::chrono::steady_clock::time_point::max()};
    int entered = 0;
    McpServer server{{"test", "1"}};
    auto tool = makeTool("identity", "Read owned request state after both calls enter", {"text"},
        [&](const ToolContext& context, const std::string& text) -> net::awaitable<json::object> {
            boost::system::error_code ec;
            if (++entered == 1) co_await barrier.async_wait(net::redirect_error(net::use_awaitable, ec));
            else barrier.cancel();
            // Another suspension ensures the second call also retains its state.
            co_await net::post(context.executor(), net::use_awaitable);
            co_return json::object{{"account", context.auth().account}, {"text", text},
                {"id", context.request_id}, {"tenant", context.metadata.at("tenant")}, {"cancelled", context.isCancelled()}};
        });
    tool.authorize = [](const ToolContext& context) { if (!context.auth().access) throw ToolError{"Denied"}; };
    server.add(std::move(tool));
    server.freeze();
    Auth alice; alice.access = true; alice.account = "alice";
    Auth bob; bob.access = true; bob.account = "bob";
    auto first = call("identity", {{"text", "first"}});
    first["id"] = "alice-id";
    first.at("params").as_object().at("_meta").as_object()["tenant"] = "alice-tenant";
    auto second = call("identity", {{"text", "second"}});
    second["id"] = "bob-id";
    second.at("params").as_object().at("_meta").as_object()["tenant"] = "bob-tenant";
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    auto a = net::co_spawn(ctx, server.dispatch(first, ToolContext{ctx.get_executor(), alice, cancellation}), net::use_future);
    auto b = net::co_spawn(ctx, server.dispatch(second, ToolContext{ctx.get_executor(), bob}), net::use_future);
    alice.account = "mutated";
    first.clear();
    // A watchdog is a failure bound, not synchronization for the test.
    net::steady_timer watchdog{ctx, 2s};
    watchdog.async_wait([&](auto ec) { if (!ec) { ADD_FAILURE() << "Coroutine barrier timed out"; barrier.cancel(); } });
    while (ctx.run_one() && (a.wait_for(0ms) != std::future_status::ready || b.wait_for(0ms) != std::future_status::ready)) {}
    watchdog.cancel();
    ctx.run();
    EXPECT_EQ(entered, 2);
    EXPECT_EQ(a.get().at("result").as_object().at("structuredContent"),
        (json::object{{"account", "alice"}, {"text", "first"}, {"id", "alice-id"}, {"tenant", "alice-tenant"}, {"cancelled", false}}));
    EXPECT_EQ(b.get().at("result").as_object().at("structuredContent"),
        (json::object{{"account", "bob"}, {"text", "second"}, {"id", "bob-id"}, {"tenant", "bob-tenant"}, {"cancelled", false}}));
}

TEST(Mcp, FailuresAfterSuspensionAndVoidAndExplicitContentResults) {
    McpServer server{{"test", "1"}};
    server.add(makeTool("domain", "Domain failure after suspension", {}, [](ToolContext context) -> net::awaitable<void> {
        co_await net::post(context.executor(), net::use_awaitable);
        throw ToolError{"Visible domain failure"};
    }));
    server.add(makeTool("internal", "Internal failure after suspension", {}, [](ToolContext context) -> net::awaitable<void> {
        co_await net::post(context.executor(), net::use_awaitable);
        throw std::runtime_error{"private database password"};
    }));
    server.add(makeTool("nothing", "Void completion", {}, []() -> net::awaitable<void> { co_return; }));
    server.add(makeTool("explicit", "Explicit content", {}, []() -> net::awaitable<ToolResult> {
        co_return ToolResult{{json::object{{"type", "text"}, {"text", "custom"}}}, json::object{{"value", 42}}, false};
    }));
    server.freeze();
    auto domain = run(server, call("domain"));
    EXPECT_FALSE(domain.contains("error"));
    EXPECT_EQ(domain.at("result").as_object().at("isError"), true);
    EXPECT_NE(json::serialize(domain).find("Visible domain failure"), std::string::npos);
    auto internal = run(server, call("internal"));
    EXPECT_EQ(internal.at("result").as_object().at("isError"), true);
    EXPECT_EQ(json::serialize(internal).find("private database password"), std::string::npos);
    EXPECT_TRUE(run(server, call("nothing")).at("result").as_object().at("content").as_array().empty());
    EXPECT_EQ(run(server, call("explicit")).at("result").as_object().at("structuredContent"), (json::object{{"value", 42}}));
}

TEST(Mcp, ProgressiveHelpStaysOutOfDiscoveryAndHandlesMissingDocuments) {
    McpServer server{{"test", "1"}};
    auto embedded = makeTool("embedded", "Short description; use test_help for details.", {}, []() -> net::awaitable<bool> { co_return true; });
    const std::string document = "LARGE_HELP_DOCUMENT " + std::string(16000, 'x');
    embedded.help["manual"] = document;
    server.add(std::move(embedded));
    auto missing = makeTool("missing", "Unavailable help", {}, []() -> net::awaitable<void> { co_return; });
    missing.help["manual"] = "";
    missing.help_provider = [](std::string_view) -> std::string { throw ToolError{"Documentation unavailable"}; };
    server.add(std::move(missing));
    server.freeze();
    const auto listing = json::serialize(run(server, request("tools/list")));
    EXPECT_LT(listing.size(), document.size());
    EXPECT_EQ(listing.find("LARGE_HELP_DOCUMENT"), std::string::npos);
    EXPECT_NE(listing.find("test_help"), std::string::npos);
    auto help = run(server, call("test_help", {{"tool", "embedded"}, {"topic", "manual"}}));
    EXPECT_EQ(std::string{help.at("result").as_object().at("structuredContent").as_object().at("text").as_string()}, document);
    auto unknown = run(server, call("test_help", {{"tool", "does_not_exist"}}));
    EXPECT_EQ(unknown.at("result").as_object().at("isError"), true);
    EXPECT_NE(json::serialize(unknown).find("Unknown help target"), std::string::npos);
    EXPECT_EQ(run(server, call("test_help", {{"tool", "missing"}, {"topic", "manual"}})).at("result").as_object().at("isError"), true);
    EXPECT_EQ(run(server, call("test_help", {{"tool", "embedded"}, {"topic", "unknown"}})).at("result").as_object().at("isError"), true);
    EXPECT_TRUE(run(server, call("test_help", {{"tool", "test_help"}})).at("result").as_object().at("structuredContent").as_object().at("topics").as_array().empty());
}

class McpHttp : public ::testing::Test {
protected:
    void SetUp() override {
        net::io_context probe;
        tcp::acceptor acceptor{probe, {net::ip::make_address("127.0.0.1"), 0}};
        port_ = acceptor.local_endpoint().port();
        auto& config = config_;
        config.http_endpoint = "127.0.0.1";
        config.http_port = std::to_string(port_);
        config.num_http_threads = 2;
        config.max_request_body_size = 2048;
        config.max_request_header_size = 16 * 1024;
#ifdef YAHAT_ENABLE_METRICS
        config.enable_metrics = false;
#endif
        http_ = std::make_unique<HttpServer>(config, [](const AuthReq& req) {
            Auth auth; auth.access = req.auth_header == "Bearer good" || req.auth_header == "Bearer bob";
            auth.account = req.auth_header == "Bearer good" ? "alice" : auth.access ? "bob" : ""; return auth;
        });
        mcp_ = std::make_shared<McpServer>(McpServer::Identity{"test", "1"});
        mcp_->add(makeTool("sum", "Add", {"a", "b"}, sum));
    }
    void TearDown() override { if (started_) http_->stop(); }
    void start() {
        RouteOptions options;
        options.allowed_origins = std::vector<std::string>{"https://trusted.test"};
        mcp_->mount(*http_, "/mcp", options);
        http_->start(); started_ = true;
    }
    http::request<http::string_body> wire(json::object message) {
        http::request<http::string_body> req{http::verb::post, "/mcp", 11};
        req.set(http::field::host, "127.0.0.1");
        req.set(http::field::authorization, "Bearer good");
        req.set(http::field::content_type, "application/json");
        req.set(http::field::accept, "application/json, text/event-stream");
        req.set("MCP-Protocol-Version", "2026-07-28");
        req.set("Mcp-Method", message.at("method").as_string());
        if (const auto* name = message.at("params").as_object().if_contains("name")) req.set("Mcp-Name", name->as_string());
        req.body() = json::serialize(message);
        req.keep_alive(false);
        req.prepare_payload();
        return req;
    }
    void connect(boost::beast::tcp_stream& stream, net::yield_context yield) {
        boost::system::error_code ec;
        for (int i = 0; i < 100; ++i) {
            stream.expires_after(2s);
            stream.async_connect({net::ip::make_address("127.0.0.1"), port_}, yield[ec]);
            if (!ec) return;
            stream.socket().close();
            net::steady_timer timer{stream.get_executor(), 10ms}; timer.async_wait(yield);
        }
        throw boost::system::system_error{ec};
    }
    http::response<http::string_body> exchange(http::request<http::string_body> req) {
        net::io_context ctx;
        boost::beast::tcp_stream stream{ctx};
        http::response<http::string_body> response;
        std::exception_ptr failure;
        net::spawn(ctx, [&](net::yield_context yield) {
            connect(stream, yield);
            stream.expires_after(3s);
            http::async_write(stream, req, yield);
            boost::beast::flat_buffer buffer;
            http::async_read(stream, buffer, response, yield);
        }, [&](std::exception_ptr ex) { failure = ex; });
        ctx.run();
        if (failure) std::rethrow_exception(failure);
        return response;
    }
    HttpConfig config_;
    std::unique_ptr<HttpServer> http_;
    std::shared_ptr<McpServer> mcp_;
    unsigned short port_ = 0;
    bool started_ = false;
};

TEST_F(McpHttp, DiscoveryHeadersOriginsAuthenticationAndSseResult) {
    start();
    auto req = wire(request("server/discover"));
    auto reply = exchange(req);
    EXPECT_EQ(reply.result_int(), 200);
    EXPECT_EQ(reply[http::field::content_type], "application/json");
    EXPECT_TRUE(json::parse(reply.body()).as_object().contains("result"));
    req.erase(http::field::authorization);
    EXPECT_EQ(exchange(req).result_int(), 401);
    req.set(http::field::authorization, "Bearer good");
    req.set(http::field::origin, "https://evil.test");
    EXPECT_EQ(exchange(req).result_int(), 403);
    req.erase(http::field::origin);
    req.set("Mcp-Method", "tools/call");
    reply = exchange(req);
    EXPECT_EQ(reply.result_int(), 400);
    EXPECT_EQ(json::parse(reply.body()).as_object().at("error").as_object().at("code"), -32020);
    req = wire(call("sum", {{"a", 4}, {"b", 5}}));
    req.set("Mcp-Name", "=?base64?c3Vt?=");
    reply = exchange(req);
    EXPECT_EQ(reply.result_int(), 200);
    EXPECT_EQ(reply[http::field::content_type], "text/event-stream");
    EXPECT_NE(reply.body().find("\"structuredContent\":9"), std::string::npos);
    EXPECT_TRUE(reply.chunked());
    EXPECT_FALSE(reply.keep_alive());
    req.insert("Mcp-Name", "sum");
    EXPECT_EQ(exchange(req).result_int(), 400);
    req = wire(request("server/discover"));
    auto unsupported = request("server/discover");
    unsupported.at("params").as_object().at("_meta").as_object()["io.modelcontextprotocol/protocolVersion"] = "v999.0.0";
    req.body() = json::serialize(unsupported); req.prepare_payload();
    // The supported header disagrees with the body: report HeaderMismatch.
    reply = exchange(req);
    EXPECT_EQ(reply.result_int(), 400);
    EXPECT_EQ(json::parse(reply.body()).as_object().at("error").as_object().at("code"), -32020);
    req.set("MCP-Protocol-Version", "v999.0.0");
    reply = exchange(req);
    EXPECT_EQ(json::parse(reply.body()).as_object().at("error").as_object().at("code"), -32022);
    req = wire(request("tools/list")); req.method(http::verb::get);
    EXPECT_EQ(exchange(req).result_int(), 405);
    req = wire(request("tools/list")); req.set(http::field::accept, "application/json, text/event-stream;q=0");
    EXPECT_EQ(exchange(req).result_int(), 406);
    req = wire(request("tools/list")); req.set("Mcp-Method", "unknown");
    auto unknown = request("unknown"); req.body() = json::serialize(unknown); req.prepare_payload();
    EXPECT_EQ(exchange(req).result_int(), 404);
}

TEST_F(McpHttp, MalformedEnvelopesAndTransportLimits) {
    start();
    auto req = wire(request("tools/list"));
    req.body() = "{"; req.prepare_payload();
    auto reply = exchange(req);
    EXPECT_EQ(reply.result_int(), 400);
    EXPECT_EQ(json::parse(reply.body()).as_object().at("error").as_object().at("code"), -32700);
    EXPECT_FALSE(json::parse(reply.body()).as_object().contains("id"));
    for (const auto& body : {json::value{json::array{request("tools/list")}}, json::value{nullptr}, json::value{42}}) {
        req = wire(request("tools/list"));
        req.body() = json::serialize(body); req.prepare_payload();
        reply = exchange(req);
        EXPECT_EQ(reply.result_int(), 400);
        EXPECT_EQ(json::parse(reply.body()).as_object().at("error").as_object().at("code"), -32600);
    }
    auto notification = request("tools/list"); notification.erase("id");
    EXPECT_EQ(exchange(wire(notification)).result_int(), 400);
    req = wire(request("tools/list")); req.set(http::field::content_type, "text/plain");
    EXPECT_EQ(exchange(req).result_int(), 415);
    req = wire(request("tools/list")); req.insert("MCP-Protocol-Version", "2026-07-28");
    EXPECT_EQ(exchange(req).result_int(), 400);
    req = wire(call("missing"));
    reply = exchange(req);
    EXPECT_EQ(reply.result_int(), 400);
    EXPECT_EQ(json::parse(reply.body()).as_object().at("error").as_object().at("code"), -32602);
    req = wire(call("sum", {{"a", "bad"}, {"b", 2}}));
    reply = exchange(req);
    EXPECT_EQ(reply.result_int(), 200);
    EXPECT_NE(reply.body().find("\"isError\":true"), std::string::npos);
    req = wire(request("tools/list")); req.body() = std::string(4096, 'x'); req.prepare_payload();
    EXPECT_EQ(exchange(req).result_int(), 413);
}

TEST_F(McpHttp, ConcurrentAuthenticationContextsRemainIsolatedAcrossSuspension) {
    auto tool = makeTool("who", "Read the calling principal", {}, [](ToolContext context) -> net::awaitable<json::object> {
        const auto before = context.auth().account;
        co_await net::post(context.executor(), net::use_awaitable);
        co_return json::object{{"before", before}, {"after", context.auth().account}, {"correlation", context.correlation_id}};
    });
    mcp_->add(std::move(tool));
    start();
    std::promise<void> ready;
    auto gate = ready.get_future().share();
    auto alice = std::async(std::launch::async, [&] { gate.wait(); return exchange(wire(call("who"))); });
    auto bob = std::async(std::launch::async, [&] {
        gate.wait(); auto req = wire(call("who")); req.set(http::field::authorization, "Bearer bob"); return exchange(req);
    });
    ready.set_value();
    const auto first = alice.get();
    const auto second = bob.get();
    EXPECT_EQ(first.result_int(), 200);
    EXPECT_EQ(second.result_int(), 200);
    EXPECT_NE(first.body().find("\"before\":\"alice\",\"after\":\"alice\""), std::string::npos);
    EXPECT_NE(second.body().find("\"before\":\"bob\",\"after\":\"bob\""), std::string::npos);
    EXPECT_EQ(first.body().find("bob"), std::string::npos);
    EXPECT_EQ(second.body().find("alice"), std::string::npos);
    EXPECT_NE(first.body(), second.body());
}

TEST_F(McpHttp, ProgressAndOwnedArgumentsAcrossSuspension) {
    mcp_->add(makeTool("progress", "Report progress", {"text"}, [](ToolContext context, std::string text) -> net::awaitable<std::string> {
        co_await context.progress(0, 1, "starting");
        net::steady_timer timer{co_await net::this_coro::executor, 2ms};
        co_await timer.async_wait(net::use_awaitable);
        co_await context.progress(1, 1);
        co_return text + context.auth().account;
    }));
    start();
    auto message = call("progress", {{"text", "hello "}});
    message.at("params").as_object().at("_meta").as_object()["progressToken"] = "p1";
    const auto reply = exchange(wire(message));
    EXPECT_EQ(reply.result_int(), 200);
    EXPECT_NE(reply.body().find("notifications/progress"), std::string::npos);
    EXPECT_NE(reply.body().find("\"progressToken\":\"p1\""), std::string::npos);
    EXPECT_NE(reply.body().find("hello alice"), std::string::npos);
}

TEST_F(McpHttp, ConcurrentRequestsDoNotHoldADispatcherLock) {
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    mcp_->add(makeTool("slow", "Wait for another request", {}, [&](ToolContext context) -> net::awaitable<bool> {
        entered = true;
        net::steady_timer timer{co_await net::this_coro::executor};
        for (int i = 0; i < 200 && !release && !context.isCancelled(); ++i) {
            timer.expires_after(5ms); co_await timer.async_wait(net::use_awaitable);
        }
        co_return release.load();
    }));
    start();
    auto slow = std::async(std::launch::async, [&] { return exchange(wire(call("slow"))); });
    for (int i = 0; i < 200 && !entered; ++i) std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(entered);
    const auto fast = exchange(wire(call("sum", {{"a", 1}, {"b", 2}})));
    EXPECT_NE(fast.body().find("\"structuredContent\":3"), std::string::npos);
    EXPECT_EQ(slow.wait_for(0ms), std::future_status::timeout);
    release = true;
    EXPECT_NE(slow.get().body().find("\"structuredContent\":true"), std::string::npos);
}

TEST_F(McpHttp, ProgressBudgetReservesTheFinalErrorResponse) {
    McpServer::Options options;
    options.max_output_bytes = 1024;
    mcp_ = std::make_shared<McpServer>(McpServer::Identity{"test", "1"}, options);
    mcp_->add(makeTool("noisy", "Report excessive progress", {}, [](ToolContext context) -> net::awaitable<bool> {
        co_await context.progress(1, 3, std::string(700, 'x'));
        co_await context.progress(2, 3, std::string(700, 'y'));
        co_return true;
    }));
    start();
    auto message = call("noisy");
    message.at("params").as_object().at("_meta").as_object()["progressToken"] = 1;
    const auto reply = exchange(wire(message));
    EXPECT_NE(reply.body().find("notifications/progress"), std::string::npos);
    EXPECT_NE(reply.body().find("\"isError\":true"), std::string::npos);
    EXPECT_NE(reply.body().find("Progress output limit reached"), std::string::npos);
}

TEST_F(McpHttp, DisconnectSetsCooperativeCancellationFlag) {
    std::promise<bool> finished;
    auto observed = finished.get_future();
    mcp_->add(makeTool("wait", "Wait until disconnected", {}, [&finished](ToolContext context) -> net::awaitable<bool> {
        net::steady_timer timer{co_await net::this_coro::executor};
        for (int i = 0; i < 200 && !context.isCancelled(); ++i) {
            timer.expires_after(5ms); co_await timer.async_wait(net::use_awaitable);
        }
        finished.set_value(context.isCancelled());
        co_return true;
    }));
    start();
    net::io_context ctx;
    boost::beast::tcp_stream stream{ctx};
    auto req = wire(call("wait"));
    std::exception_ptr failure;
    net::spawn(ctx, [&](net::yield_context yield) {
        connect(stream, yield);
        http::async_write(stream, req, yield);
        boost::beast::flat_buffer buffer;
        http::response_parser<http::empty_body> parser;
        http::async_read_header(stream, buffer, parser, yield);
        EXPECT_EQ(parser.get()[http::field::content_type], "text/event-stream");
        stream.socket().close();
    }, [&](std::exception_ptr ex) { failure = ex; });
    ctx.run();
    if (failure) std::rethrow_exception(failure);
    ASSERT_EQ(observed.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(observed.get());
}
}
