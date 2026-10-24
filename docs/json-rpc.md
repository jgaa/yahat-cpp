# JSON-RPC 2.0

Enable with `-DYAHAT_ENABLE_JSON_RPC=ON` (default **OFF**). This adds a generic
Boost.JSON JSON-RPC dispatcher and a POST/JSON HTTP adapter.

```cpp
#include "yahat/JsonRpc.h"

auto rpc = std::make_shared<yahat::JsonRpc>();
rpc->addMethod("echo", [](const boost::json::value& params,
                         const yahat::JsonRpc::Context& context) {
    return params;
});

yahat::RouteOptions options;
options.exact_match = true;
options.auto_handle_cors = false;
options.allowed_origins = std::vector<std::string>{"https://app.example.com"};
server.addRouteWithOptions("/rpc", std::make_shared<yahat::JsonRpcHandler>(rpc),
                           std::move(options));
```

## HTTP transport and synchronous dispatch

The HTTP adapter accepts POST with `Content-Type: application/json`, returns JSON
responses with HTTP 200, and acknowledges notifications with an empty HTTP 202.
Applications must explicitly configure authentication and Origin policies.

`JsonRpc::handle(json, request)` parses and dispatches text; `dispatch(value, request)`
accepts parsed messages. Both return `std::nullopt` for notifications that require
no response. Callbacks receive params (null when omitted) and a context containing
the request ID, notification flag, and optional HTTP request/identity. Validate
method parameters in the callback and throw `JsonRpc::Error` for explicit errors,
for example `JsonRpc::Error{JsonRpc::invalid_params, "Missing name"}`. Unexpected
exceptions produce a generic internal error without disclosing exception details.
Callbacks run without holding the registry lock and must be thread safe.

## Batches

JSON-RPC batches are supported with a default maximum of 128 messages. For a
transport that prohibits batches, construct
`JsonRpc{JsonRpc::Options{false, 128}}`. The dispatcher supports server-side calls
and notifications; incoming response correlation and protocol-specific schema
validation belong in the layer above it.

## Testing

With tests enabled, HTTP regression tests are always included; JSON-RPC tests are
included when its feature is enabled. Suggested validation configurations are
JSON-RPC ON/OFF combined with metrics ON/OFF, followed by CTest in each build.

## Coroutine dispatch

For generic JSON-RPC integrations, `addAsyncMethod()` and `dispatchAsync()` provide
an additive awaitable API. It owns messages and retains an optional shared HTTP
request through suspension. The synchronous API and `JsonRpcHandler` retain their
existing behavior. Invoke async dispatch from an Asio coroutine; synchronous
callbacks reached through this API must not use a stackful request yield pointer.
The [MCP server](mcp.md) has its own revision-specific execution/transport policy
and reuses the JSON-RPC envelope helpers.

See [HTTP route policies](http.md#route-policies-authentication-and-cors) for authentication and Origin configuration.
