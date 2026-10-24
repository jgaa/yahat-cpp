# HTTP, HTTPS and Server-Sent Events

Start with [building and integration](building.md). The
[API server example](../examples/apiserver/apiserver.cpp) shows a complete
request handler and application authentication. Configure `http_tls_key` and
`http_tls_cert` on `HttpConfig` to serve HTTPS.

## Minimal server setup

```cpp
#include "yahat/HttpServer.h"
#include <iostream>

int main() {
    yahat::HttpConfig config;
    config.http_endpoint = "127.0.0.1";
    config.http_port = "8080";

    // Local example: permit requests without checking credentials.
    yahat::HttpServer server{config, [](const yahat::AuthReq&) {
        yahat::Auth auth;
        auth.access = true;
        return auth;
    }};
    // Add your request handlers here
    server.start().wait();

    return 0;
}
```

Existing routes and response constructors retain their defaults. New route policies
are opt-in and must be configured before starting the server. These additions are
source compatible; rebuild applications against the matching library and headers.

## Requests and responses

`Request::headers` owns all HTTP headers, including repeated fields.
`Request::getHeader(name)` returns the first matching value, ignoring case; use
`headers.count()` or `headers.equal_range()` when duplicates must be rejected.
The authenticator also has access to these headers through `AuthReq::req`.

`Response::headers` supplies custom response headers, including repeated fields.
Application headers override generated values. Invalid header names/values and
server-managed framing headers (`Content-Length`, `Transfer-Encoding`,
`Connection`, `Keep-Alive`, `Trailer`, `Upgrade`) are rejected. Set `Response::close`
to close a connection. Set `generate_status_body = false` to send an explicitly
empty body instead of the legacy generated status JSON.

## Route policies, authentication and CORS

```cpp
yahat::RouteOptions options;
options.exact_match = true;
options.auto_handle_cors = false; // The handler/filter manages preflights if needed.
options.allowed_origins = std::vector<std::string>{"https://app.example.com"};
options.allowed_methods = {yahat::Request::Type::POST};
options.unauthorized_response = [](const yahat::Request&) {
    yahat::Response response{401, "Unauthorized"};
    response.headers.set("WWW-Authenticate", "Bearer");
    return response;
};
server.addRouteWithOptions("/protocol", handler, std::move(options));
```

Policies use the same longest-prefix selection as handlers. `exact_match` prevents
matching child paths. `allowed_origins` checks every request, including OPTIONS,
before authentication or automatic CORS handling. An unset allowlist preserves
legacy behavior; an empty allowlist rejects any present Origin. Requests without
Origin remain allowed, and duplicate Origin headers are rejected when an allowlist
is configured. With automatic CORS enabled and an allowlist configured, a validated
Origin is echoed with `Vary: Origin` instead of the legacy wildcard.

`request_filter` can return an early response or `std::nullopt` to continue. It runs
before authentication and can implement protocol-header validation or a custom
OPTIONS response with `Access-Control-Allow-Headers` and
`Access-Control-Expose-Headers`. Set `public_access = true` only for deliberately
public endpoints, such as authorization discovery metadata. All other routes keep
the existing authenticator behavior. `Response::auto_handle_cors` can override the
route/server CORS setting for an individual response. Policy and authentication
callbacks must be thread safe.

## Request limits

`HttpConfig::max_request_body_size` and `max_request_header_size` optionally
configure Beast's parser limits. Leaving them unset preserves Beast's defaults.
The separate `max_decompressed_size` limit still applies to decompressed bodies.

## Server-Sent Events

SSE responses work for POST as well as GET. Custom response headers and cookies
are passed to `SseHandler`; stream-specific headers can also be set directly on
`SseHandler::headers` before starting the continuation. Each network session runs
on an Asio strand. `SseQueueHandler::sendSse()` and queue controls support calls
from other threads. `setQueueLimit(bytes)` bounds queued data and throws
`std::length_error` on overflow; zero keeps the existing unlimited queue.
`setHeartbeatInterval(seconds)` enables SSE comment keepalives; zero disables them.
`closeSse()` stops immediately, while `finishSse()` drains queued events and sends
the final HTTP chunk. Messages submitted after either close operation are ignored.
Use a separate handler for each response stream.

Idle SSE disconnects now invoke `Request::notify_connection_closed` once and mark
`Request::isCancelled()`. This detects transport loss; the protocol layer decides
whether that means cancellation for its supported protocol version. It does not
detect disconnects while an ordinary synchronous JSON handler is executing.
Custom `Continuation::Stream` implementations remain source compatible; override
`finish_stream()` to support graceful chunked termination. Custom continuations
can override `prepareResponse()` to consume HTTP response metadata.
