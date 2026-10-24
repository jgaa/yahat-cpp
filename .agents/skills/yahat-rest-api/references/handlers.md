# REST handler coroutine patterns

These examples use the public `RequestHandler` interface. They do not change the
library's dispatch model or require an MCP/JSON-RPC feature flag.

## Default: stackful Asio handler

`onReqest` executes on the coroutine stack yahat created with `boost::asio::spawn`.
Pass its existing `yield_context` into async operations; they suspend that
coroutine and return their result when resumed. No additional `spawn` is needed
per handler call.

The timer below illustrates an asynchronous operation; omit artificial delays in
real status endpoints and substitute your async database/network operation.

```cpp
#include "yahat/HttpServer.h"
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <stdexcept>
#include <string_view>

namespace net = boost::asio;

class StatusHandler final : public yahat::RequestHandler {
public:
    yahat::Response onReqest(const yahat::Request& request) override {
        if (!request.yield) {
            throw std::logic_error{"This handler needs an HTTP coroutine"};
        }
        auto yield = *request.yield;
        net::steady_timer timer{yield.get_executor(), std::chrono::milliseconds{1}};
        boost::system::error_code error;
        timer.async_wait(yield[error]);
        if (error) {
            throw boost::system::system_error{error};
        }
        yahat::Response response{200, "OK", std::string_view{R"({"status":"ok"})"}};
        response.mime_type = "application/json";
        return response;
    }
};
```

`async_operation(..., yield)` reports an operation error by throwing;
`async_operation(..., yield[error])` fills the error code for explicit handling.
Choose the application's desired error mapping. Do not add blocking `sleep`,
`future::get`, or a nested `io_context::run` to simulate awaiting work.

Register before starting an existing authenticated server:

```cpp
yahat::RouteOptions options;
options.exact_match = true;
options.allowed_methods = {yahat::Request::Type::GET};
server.addRouteWithOptions("/api/status", std::make_shared<StatusHandler>(), options);
```

The server populates `request.yield` for live calls. For direct unit tests,
invoke the handler from `net::spawn` with a valid yield context and run that
context, or use an actual HTTP fixture. Do not manufacture a dangling pointer to
an already-finished coroutine.

## Optional: reuse a C++20 awaitable through a bridge

Keep the REST override returning `Response`. `co_spawn` accepts the existing
stackful `yield_context` as its completion token, allowing the current HTTP
coroutine to wait without blocking its worker. Yahat's `src/Mcp.cpp` uses this
interop pattern in its HTTP adapter; it is an implementation example, not a
requirement to enable MCP for REST.

```cpp
#include "yahat/HttpServer.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <stdexcept>
#include <string>

namespace net = boost::asio;

net::awaitable<yahat::Response> make_status(std::string account) {
    // A small suspension for this example; real services await their own I/O.
    auto executor = co_await net::this_coro::executor;
    co_await net::post(executor, net::use_awaitable);
    yahat::Response response{200, "OK", boost::json::serialize(
        boost::json::object{{"account", account}, {"status", "ok"}})};
    response.mime_type = "application/json";
    co_return response;
}

class AwaitableStatusHandler final : public yahat::RequestHandler {
public:
    yahat::Response onReqest(const yahat::Request& request) override {
        if (!request.yield) {
            throw std::logic_error{"This handler needs an HTTP coroutine"};
        }
        auto yield = *request.yield;
        return net::co_spawn(yield.get_executor(),
                            make_status(request.auth.account), yield);
    }
};
```

Arguments passed to `make_status` are owned by its coroutine frame. The bridge
awaits completion before the REST handler returns, and exceptions from that
awaitable propagate through the yielding call. Do not immediately invoke a
temporary capturing coroutine lambda: its closure can die while its coroutine
still uses captures. Prefer a named function with by-value parameters, or retain
the callable and all referenced resources for the complete operation.

For either model, a session strand does not serialize a shared handler across
all connections. Synchronize shared state, keep application services alive for
outstanding work, and avoid holding locks while suspending. Cancelling a flag
cannot interrupt an arbitrary operation or roll back completed side effects.
