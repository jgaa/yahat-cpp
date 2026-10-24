---
name: yahat-mcp
description: Implement or extend MCP servers in C++20 applications using yahat, including typed coroutine tools, application authentication, and progressive help. Use when integrating yahat's server API, rather than configuring an MCP client.
---

# Yahat MCP

Implement the application's requested MCP integration using its actual yahat dependency. Follow the consumer project's conventions and keep its existing HTTP behavior intact.

## Find the implemented contract

Locate the consumer's actual yahat dependency. Resolve these paths against **that yahat source root**, not the consumer root or skill directory. Read only relevant sections:

| Need | Existing source/documentation |
| --- | --- |
| Setup, transport, auth, limits, help | `docs/mcp.md`; `docs/building.md` for CMake integration |
| Exact public signatures and conversion rules | `include/yahat/Mcp.h`; `include/yahat/HttpServer.h` for HTTP/auth |
| Small working integration | `examples/mcpserver/main.cpp` |
| Custom converters, member methods, ownership and error cases | `tests/mcp_tests.cpp` |
| Lazy external help and explicit content blocks | `tests/mcp_test_server.cpp` |
| Protocol validation and test orchestration | `docs/mcp-testing.md` |

For binary packages, inspect installed `include/yahat/Mcp.h` and generated `include/yahat/config.h`; obtain matching source for examples. Check MCP availability before selecting an [upstream revision](https://github.com/jgaa/yahat-cpp). Headers/implementation take precedence over prose; the feature proposal is not an API reference.

For copying/installing this skill in another project with Codex, read [installation](references/install.md).

## Integrate the server

- Enable `YAHAT_ENABLE_MCP=ON` **before configuring yahat**; it defaults off and implies JSON-RPC. A consumer preprocessor definition cannot enable a prebuilt library: obtain an MCP-enabled package with matching generated headers. Reuse the project's CMake integration and `yahat-cpp::yahat-cpp` target; require C++20.
- Include `yahat/Mcp.h`. Create a shared-owned `McpServer` with `Identity{application_name, application_version}` alongside the existing `HttpServer` and authenticator, as in the working example. Keep `HttpConfig` alive for the HTTP server's lifetime.
- Create descriptors with `makeTool(name, description, arguments, handler)`, then `add(std::move(descriptor))`. Argument names/descriptions correspond to handler parameters in order, excluding an optional **first** injected `ToolContext`. Register before serving; `mount(http, "/mcp", route_options)` freezes the registry and retains shared ownership. Reserve `<server-name>_help` when built-in help is enabled.
- Use `McpServer::Options` for MCP limits and `HttpConfig` for parser/body/decompression limits. Local services should bind explicitly to loopback. The mount inherits auth, defaults CORS off, and rejects present Origins unless allowed through `RouteOptions.allowed_origins`.

Check `McpServer::protocol_version` when choosing clients. Currently: stateless **2026-07-28**, per-request metadata, JSON discovery/listing and SSE calls; no legacy sessions/initialization, resources, prompts, subscriptions, MRTR or tasks. Let the adapter produce protocol envelopes.

## Implement typed tools and results

Handlers return `boost::asio::awaitable<T>` with explicit signatures. Bind members with `makeTool(name, description, arguments, instance, &Class::method)`. Registration is runtime-owned.

Use supported owning parameters: `std::string`, bool, bounded integers, finite floating-point values, `std::optional<T>`, vectors, string-keyed maps, or Boost.JSON values/objects/arrays. Generated wrappers reject missing required or unknown keys, incompatible types and integer overflow **before handler entry**. Integers require JSON integers; optional arguments accept missing/null. Validate domain constraints and raw JSON internals in application code.

For domain inputs, specialize `yahat::McpConverter<T>::decode(value, path)` and `schema()` together; see the custom conversion test. This is not a general JSON Schema validator. Use `ArgumentError` for useful argument paths or `ToolError` for safe, actionable domain failures. Both produce `isError` tool results; unexpected exceptions become generic failures. Unknown tools and malformed requests use JSON-RPC errors.

Ordinary results pass through `boost::json::value_from`, producing structured content plus a text block. Return a Boost.JSON object/array directly instead of serializing it into a string. Customize domain output with Boost.JSON `tag_invoke(value_from_tag, value&, const T&)`, or return `ToolResult` for explicit content blocks. `void` yields empty content.

## Preserve coroutine lifetimes and concurrency

- Prefer arguments and `ToolContext` by value. The typed wrapper owns converted arguments across the awaited call and permits const references to that storage; do not retain such references after the call. Avoid dangling string views, spans or JSON references in deferred work.
- Bound objects, captured references, resources and callback closures must outlive outstanding calls, including calls finishing after disconnect. Do not immediately invoke a temporary capturing coroutine lambda and return its awaitable. `makeTool` stores its callable; external captures still need valid ownership.
- Use Asio operations with `co_await` and `use_awaitable`; obtain the executor from `context.executor()` or `this_coro::executor`. Do not use stackful HTTP `yield_context` in tools. Awaitables are lazy; let yahat await the handler.
- Avoid blocking waits, synchronous slow I/O and CPU-heavy loops on HTTP workers. Use the application's async APIs or worker pool with explicit result/lifetime management. Calls may overlap; protect shared application state without holding a mutex across suspension.
- Cancellation is cooperative: check `context.isCancelled()` or `throwIfCancelled()` around waits and loop iterations. Disconnect does not interrupt an outstanding operation, undo side effects or make borrowed resources safe to destroy. `HttpServer::stop()` does not drain arbitrary application tasks.
- `co_await context.progress(completed, total, message)` before returning. Notifications require a caller progress token, values must increase strictly, and progress consumes the output budget.

## Authorize operations and disclose documentation progressively

Validate basic/bearer credentials in the application authenticator before MCP dispatch. Use `context.auth()` (including application-owned `Auth::extra`) and descriptor `authorize(context)` for permissions; deny with `ToolError`. OAuth/token acquisition and challenges remain application concerns. Client metadata is not identity. The frozen catalog is shared and publicly cacheable; metadata must suit every permitted catalog reader. Help checks target authorization.

Keep descriptions short: identify the operation, essential constraints and when to request help. Put detailed examples, configuration semantics and troubleshooting in descriptor `help[topic]`. The built-in `<server-name>_help` accepts `tool` and optional `topic`; omitting the topic returns an overview/index. A `help_provider` lazily resolves the declared topic keys; when present, it supplies text for every requested known topic rather than falling back to map text. Map topics to application-selected files/resources, never arbitrary client paths. Split documents to respect `max_help_bytes`.

Prefer task-focused tools and compact schemas/results. Offer application-level filtering, field selection or pagination when large outputs warrant them; these are tool design choices, not automatic yahat features. Keep essential safety/domain constraints in schemas/validation, not exclusively in help. Update schemas, validation and help together as the domain evolves. Check that `tools/list` stays small and does not contain full manuals.

## Verify the integration

Use the consumer's permitted build/test workflow. Exercise typed validation before handler entry, authorization, suspension, cancellation, help and private-error redaction. Use a revision-compatible client supplying required metadata/headers. For library diagnosis, follow yahat's conformance README from its source root: that runner tests the library fixture, not the consumer application. Distinguish optional-feature baseline gaps from passing checks.
