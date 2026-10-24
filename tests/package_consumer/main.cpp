#include <yahat/Mcp.h>
#ifdef YAHAT_ENABLE_MCP
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <iostream>

boost::asio::awaitable<int> add(int a, int b) { co_return a + b; }

#endif

int main() {
#ifdef YAHAT_ENABLE_MCP
    yahat::McpServer server{{"consumer", "test"}};
    server.add(yahat::makeTool("add", "Add integers", {"a", "b"}, add));
    server.freeze();
    boost::asio::io_context context;
    boost::json::object metadata{{"io.modelcontextprotocol/protocolVersion", boost::json::string{yahat::McpServer::protocol_version}},
        {"io.modelcontextprotocol/clientCapabilities", boost::json::object{}}};
    boost::json::object request{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", boost::json::object{{"name", "add"}, {"arguments", boost::json::object{{"a", 2}, {"b", 3}}}, {"_meta", metadata}}}};
    auto reply = boost::asio::co_spawn(context, server.dispatch(request, yahat::ToolContext{context.get_executor()}), boost::asio::use_future);
    context.run();
    if (reply.get().at("result").as_object().at("structuredContent") != 5) return 1;
    std::cout << "Installed yahat MCP consumer: 2 + 3 = 5\n";
#else
    if (yahat::HttpServer::version().empty()) return 1;
#endif
}
