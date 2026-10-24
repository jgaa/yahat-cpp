---
name: yahat-rest-api
description: Build or extend REST APIs in C++20 applications using yahat, with stackful Boost.Asio request handlers, routing, JSON validation, authentication, and SSE. Use for HTTP application endpoints rather than MCP tool registration or MCP client configuration.
---

# Yahat REST API

Implement the requested endpoints using the consumer project's existing application services and yahat dependency. Preserve its authentication and HTTP contracts.

## Locate the actual API

Resolve these paths against the **yahat dependency source root**, not the consumer root or this skill directory. Read only relevant sections; installed packages provide public headers under their prefix's `include/yahat`.

| Need | Source |
| --- | --- |
| CMake integration and feature switches | `docs/building.md` |
| HTTP setup, route policies, responses and SSE | `docs/http.md` |
| Exact public signatures | `include/yahat/HttpServer.h` |
| Dispatch, URL parsing and exception behavior | `src/HttpServer.cpp` |
| Working API and auth wiring | `examples/apiserver/apiserver.cpp` |
| Multi-endpoint dispatch and SSE | `examples/chatserver/ChatApi.cpp` |
| HTTP regression fixtures | `tests/http_tests.cpp` |

Examples demonstrate wiring; supply real credential checks and keep authorization headers out of logs. Verify APIs against the consumer's revision rather than copying an older example signature.

## Use the correct Asio coroutine model

Implement `yahat::RequestHandler::onReqest(const yahat::Request&) -> yahat::Response`. The spelling **onReqest** is the public virtual API. It returns a response directly; an `awaitable<Response>` override is not compatible.

Yahat already invokes HTTP handlers inside **stackful Boost.Asio `spawn` coroutines**. For asynchronous I/O, obtain `auto yield = *request.yield` and pass `yield` or `yield[error_code]` as the completion token. Construct operations on `yield.get_executor()` so they use the session executor. The authenticator similarly receives `AuthReq::yield` by reference. A synthetic request in a unit test may have a null yield pointer.

Read [handler patterns](references/handlers.md) when implementing async operations or bridging existing C++20 awaitables. Use named awaitable functions with owned parameters and `co_spawn(executor, operation, yield)` when a bridge is needed. Do not use `co_await`/`co_return` inside the REST override, block on a future, or detach work that must finish before the response. Yahat's MCP handlers have a separate awaitable interface; that contract does not apply to REST overrides.

Ordinary local variables survive a yielded operation until the handler returns. Do not let borrowed request references, its yield pointer, query/header/cookie views or buffers outlive their request/coroutine. Background tasks must own their inputs; copying a `Request` does not rebind its internal string views. Shared handler instances can serve concurrent sessions: synchronize application state without holding a mutex across a yielding call. Offload blocking I/O or expensive CPU work using the application's async integration instead of blocking HTTP workers.

## Wire routes and application policy

- Reuse `yahat-cpp::yahat-cpp` and C++20. Plain REST needs neither the MCP nor JSON-RPC feature enabled. Keep `HttpConfig` alive for the entire `HttpServer` lifetime.
- Construct `HttpServer(config, authenticator, optional_branding)` and register shared-owned handlers before starting it. Use `addRouteWithOptions(path, handler, options)` for method restrictions, exact matching and explicit Origin/CORS policies; `addRoute(path, handler)` retains defaults.
- Routes are literal longest prefixes matched at path boundaries, not parameterized REST templates. `exact_match` prevents matching children. `request.target` remains the full path; `request.route` identifies the matched prefix. Dispatch resource IDs/subpaths and validate methods in application code as needed.
- The authenticator runs before the handler. Set `Auth::access`, account and optional application-owned `extra`; use `request.auth` for endpoint/domain authorization. `public_access` deliberately bypasses authentication. A request filter runs **before** auth, so it cannot rely on authenticated identity.
- Configure Origin allowlists and browser preflights deliberately; legacy CORS defaults are permissive. Choose an application unauthorized response/challenge where needed. Configure parser/body/decompression limits and bind local services explicitly to loopback.

## Parse requests and return responses

REST handlers perform their own argument/domain validation; MCP's typed tool converters are not a REST facility. Use Boost.JSON for JSON bodies, validate content type and object/field types before accessing values, and return actionable client errors for malformed input. Query helpers return views and cannot distinguish missing from empty without checking the map; inspect `Request::init` before relying on query decoding or duplicate-key behavior.

Return `Response{status, reason, body}` and set `mime_type` to a stable string literal or other long-lived storage: this field is a `string_view`. Serialize JSON once with `boost::json::serialize`; pass an owned string or explicit `std::string_view` as the body, since bare literals can select ambiguous constructor overloads. Set `generate_status_body = false` for an explicitly empty body; otherwise an empty response uses legacy generated status JSON. Use `Response::headers` for headers, and `close` for connection closure; framing headers are library-managed.

Map expected domain errors to the endpoint's status/body contract. The dispatcher catches thrown `Response` and returns it; unexpected `std::exception` becomes HTTP 500 and its message is logged. Do not put secrets in exception messages or expose raw exception details to clients.

For streaming, follow the existing SSE docs/example: return a response with `setContinuation`, use a separate `SseQueueHandler` per stream, and bound queues. Cancellation is cooperative. Ordinary REST processing has no continuously active disconnect monitor; `isCancelled()` alone cannot guarantee prompt disconnect detection. Give downstream operations their own deadlines/cancellation behavior. SSE's disconnect callback/flag is supported; HTTP I/O timeouts are not universal application-operation deadlines.

## Verify and reuse

Use the project's permitted test workflow. Exercise methods/subpaths, malformed JSON, domain validation, auth failures, empty responses and relevant Origin/header policies. Async tests must actually run an Asio context and demonstrate suspension/resumption; use the library's HTTP fixture when wire behavior matters.

To use this skill in another project, copy the **whole** `yahat-rest-api` folder to that project's `.agents/skills/` and invoke `$yahat-rest-api`. Its relative reference stays bundled; the source-path table still refers to the selected yahat dependency. No repository-specific absolute paths or runtime configuration are required.
