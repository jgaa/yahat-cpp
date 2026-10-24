// Real public-API fixture for the official MCP conformance runner.
#include "yahat/Mcp.h"
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fstream>
#include <iostream>

namespace net = boost::asio;
namespace json = boost::json;
using namespace yahat;

namespace {
constexpr std::string_view pixel = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aV1cAAAAASUVORK5CYII=";
constexpr std::string_view audio = "UklGRiYAAABXQVZFZm10IBAAAAABAAEARKwAAESsAAABAAgAZGF0YQIAAACAgA==";
ToolResult blocks(json::array content) { return {std::move(content), {}, false}; }
struct Application {
    net::awaitable<std::string> member(ToolContext context, std::string text) {
        co_await net::post(context.executor(), net::use_awaitable);
        co_return context.auth().account + ":" + text;
    }
};
}

int main(int argc, char** argv) try {
    if (argc != 3) { std::cerr << "Usage: yahat-mcp-test-server PORT DOCUMENTATION_FILE\n"; return 2; }
    const auto port = std::stoi(argv[1]);
    if (port < 1 || port > 65535) throw std::invalid_argument{"Invalid port"};
    const std::string help_path = argv[2];
    HttpConfig config;
    config.http_endpoint = "127.0.0.1";
    config.http_port = std::to_string(port);
    config.num_http_threads = 2;
    config.max_request_body_size = 1024 * 1024;
    config.max_request_header_size = 16 * 1024;
    config.enable_http_basic_auth = false;
#ifdef YAHAT_ENABLE_METRICS
    config.enable_metrics = false;
#endif
    // Public loopback fixture: production authentication remains application-owned.
    HttpServer http{config, [](const AuthReq&) { Auth auth; auth.access = true; auth.account = "fixture"; return auth; }};
    Application application; // Outlives the registry and outstanding member calls.
    auto mcp = std::make_shared<McpServer>(McpServer::Identity{"conformance", std::string{HttpServer::version()}});
    mcp->add(makeTool("echo", "Echo a string; use conformance_help for details.", {"text"}, [](std::string text) -> net::awaitable<std::string> { co_return text; }));
    mcp->add(makeTool("add", "Add numbers.", {"a", "b"}, [](double a, double b) -> net::awaitable<double> { co_return a + b; }));
    mcp->add(makeTool("optional", "Echo an optional integer.", {"value"}, [](std::optional<int> value) -> net::awaitable<json::value> { co_return value ? json::value{*value} : json::value{nullptr}; }));
    auto structured = makeTool("structured", "Echo nested JSON; use conformance_help for details.", {"items"},
        [](std::vector<std::map<std::string, int>> items) -> net::awaitable<json::object> { co_return json::object{{"items", json::value_from(items)}, {"count", items.size()}}; });
    structured.help["embedded"] = "Input is an array of objects with integer values.";
    structured.help["external"] = "";
    structured.help_provider = [help_path](std::string_view topic) {
        if (topic == "embedded") return std::string{"Input is an array of objects with integer values."};
        // Only a fixed, application-selected path is opened; topic is never a path.
        std::ifstream input{help_path};
        if (!input) throw ToolError{"Documentation is unavailable"};
        return std::string{std::istreambuf_iterator<char>{input}, {}};
    };
    mcp->add(std::move(structured));
    mcp->add(makeTool("member", "Call a member coroutine.", {"text"}, application, &Application::member));
    mcp->add(makeTool("test_simple_text", "Return text.", {}, []() -> net::awaitable<std::string> { co_return "This is a simple text response for testing."; }));
    mcp->add(makeTool("test_error_handling", "Return a tool error.", {}, []() -> net::awaitable<void> { throw ToolError{"This tool intentionally returns an error for testing"}; co_return; }));
    mcp->add(makeTool("test_tool_with_progress", "Suspend and report progress.", {}, [](ToolContext context) -> net::awaitable<std::string> {
        for (const auto completed : {0, 50, 100}) {
            co_await context.progress(completed, 100);
            co_await net::post(context.executor(), net::use_awaitable);
            context.throwIfCancelled();
        }
        co_return "Completed";
    }));
    mcp->add(makeTool("test_logging_tool", "Complete without protocol log notifications.", {}, []() -> net::awaitable<bool> { co_return true; }));
    mcp->add(makeTool("test_image_content", "Return an image block.", {}, []() -> net::awaitable<ToolResult> {
        co_return blocks({json::object{{"type", "image"}, {"data", pixel}, {"mimeType", "image/png"}}});
    }));
    mcp->add(makeTool("test_audio_content", "Return an audio block.", {}, []() -> net::awaitable<ToolResult> {
        co_return blocks({json::object{{"type", "audio"}, {"data", audio}, {"mimeType", "audio/wav"}}});
    }));
    mcp->add(makeTool("test_embedded_resource", "Return an embedded resource block.", {}, []() -> net::awaitable<ToolResult> {
        co_return blocks({json::object{{"type", "resource"}, {"resource", json::object{{"uri", "test://embedded-resource"}, {"mimeType", "text/plain"}, {"text", "This is an embedded resource content."}}}}});
    }));
    mcp->add(makeTool("test_multiple_content_types", "Return mixed content blocks.", {}, []() -> net::awaitable<ToolResult> {
        co_return blocks({json::object{{"type", "text"}, {"text", "Multiple content types test:"}},
            json::object{{"type", "image"}, {"data", pixel}, {"mimeType", "image/png"}},
            json::object{{"type", "resource"}, {"resource", json::object{{"uri", "test://mixed-content-resource"}, {"mimeType", "application/json"}, {"text", "{\"test\":\"data\",\"value\":123}"}}}}});
    }));
    // The official keyword-preservation fixture uses a runtime descriptor. Its
    // validator belongs to the application, as for any custom converter/schema.
    ToolDescriptor schema;
    schema.name = "json_schema_2020_12_tool";
    schema.description = "Tool with JSON Schema 2020-12 features";
    schema.input_schema = json::parse(R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","$defs":{"address":{"$anchor":"addressDef","type":"object","properties":{"street":{"type":"string"},"city":{"type":"string"}}}},"properties":{"name":{"type":"string"},"address":{"$ref":"#/$defs/address"},"contactMethod":{"type":"string","enum":["phone","email"]},"phone":{"type":"string"},"email":{"type":"string"}},"allOf":[{"anyOf":[{"required":["phone"]},{"required":["email"]}]}],"if":{"properties":{"contactMethod":{"const":"phone"}},"required":["contactMethod"]},"then":{"required":["phone"]},"else":{"required":["email"]},"additionalProperties":false})").as_object();
    schema.invoke = [](json::object args, ToolContext) -> net::awaitable<ToolResult> {
        for (const auto& entry : args) {
            if (entry.key() == "address") {
                if (!entry.value().is_object()) throw ToolError{"/address: expected object"};
                for (const auto key : {"street", "city"}) {
                    const auto* value = entry.value().as_object().if_contains(key);
                    if (value && !value->is_string()) throw ToolError{"/address: expected string fields"};
                }
            } else if (entry.key() != "name" && entry.key() != "contactMethod" && entry.key() != "phone" && entry.key() != "email") {
                throw ToolError{"Unknown argument"};
            } else if (!entry.value().is_string()) throw ToolError{"Expected string argument"};
        }
        const auto* method = args.if_contains("contactMethod");
        if (method && *method != "phone" && *method != "email") throw ToolError{"Invalid contactMethod"};
        if (!args.contains(method && *method == "phone" ? "phone" : "email")) throw ToolError{"Required contact detail missing"};
        co_return ToolResult::fromValue(args);
    };
    mcp->add(std::move(schema));
    RouteOptions options;
    options.allowed_origins = std::vector<std::string>{"http://127.0.0.1:" + config.http_port};
    mcp->mount(http, "/mcp", options);
    // Signal handling runs on a separate context so stop() can join HTTP workers.
    net::io_context signals_ctx;
    net::signal_set signals{signals_ctx, SIGINT, SIGTERM};
    signals.async_wait([&](auto error, int) { if (!error) http.stop(); });
    auto done = http.start();
    std::cout << "MCP test endpoint: http://127.0.0.1:" << port << "/mcp" << std::endl;
    signals_ctx.run();
    done.get();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "MCP fixture: " << error.what() << '\n';
    return 1;
}
