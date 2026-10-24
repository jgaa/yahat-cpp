#include "yahat/Mcp.h"
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <iostream>

using namespace yahat;
namespace net = boost::asio;
namespace json = boost::json;

net::awaitable<std::string> greet(ToolContext context, std::string name) {
    context.throwIfCancelled();
    co_await context.progress(0, 1, "Preparing greeting");
    net::steady_timer pause{co_await net::this_coro::executor, std::chrono::milliseconds{10}};
    co_await pause.async_wait(net::use_awaitable);
    context.throwIfCancelled();
    co_return "Hello, " + name;
}

net::awaitable<json::object> inspectConfig(json::object config) {
    const auto* mode = config.if_contains("mode");
    if (!mode || !mode->is_string() || (mode->as_string() != "fast" && mode->as_string() != "safe")) {
        throw ToolError{"/config/mode: expected fast or safe; see demo_help topic configuration"};
    }
    co_return json::object{{"accepted", true}, {"mode", *mode}};
}

int main(int argc, char** argv) {
    // Demonstrates application-owned authentication. Do not log the token.
    const char* token = std::getenv("YAHAT_MCP_EXAMPLE_TOKEN");
    if (!token || !*token) {
        std::cerr << "Set YAHAT_MCP_EXAMPLE_TOKEN before starting the example\n";
        return 1;
    }
    HttpConfig config;
    config.http_endpoint = "127.0.0.1";
    config.http_port = argc > 1 ? argv[1] : "8080";
    config.max_request_body_size = 1024 * 1024;
    config.max_request_header_size = 16 * 1024;
    config.enable_http_basic_auth = false;
    HttpServer http{config, [expected = std::string{"Bearer "} + token](const AuthReq& req) {
        Auth auth;
        auth.access = req.auth_header == expected;
        if (auth.access) auth.account = "example-user";
        return auth;
    }};
    auto mcp = std::make_shared<McpServer>(McpServer::Identity{"demo", std::string{HttpServer::version()}});
    mcp->add(makeTool("demo_greet", "Greet a person by name.", {{"name", "Person to greet"}}, greet));
    auto inspect = makeTool("demo_inspect_config", "Validate configuration; use demo_help for configuration details.", {"config"}, inspectConfig);
    inspect.help["configuration"] = "config.mode is required and must be fast or safe. Example: {\"config\":{\"mode\":\"safe\"}}. Other configuration keys are ignored by this example.";
    mcp->add(std::move(inspect));
    mcp->mount(http);
    std::cout << "MCP endpoint: http://127.0.0.1:" << config.http_port << "/mcp\n";
    http.run();
}
