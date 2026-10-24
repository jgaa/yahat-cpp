# MCP server

Enable with `-DYAHAT_ENABLE_MCP=ON` (default **OFF**). This automatically enables
JSON-RPC for the build. Existing HTTP handlers and the default configuration are
unchanged. MCP uses Boost.JSON and native `boost::asio::awaitable<T>` handlers.

The endpoint implements **MCP 2026-07-28**: per-request metadata,
`server/discover`, deterministic paginated `tools/list`, and `tools/call`.
Tool calls return request-scoped SSE; discovery/listing return JSON.

**Protocol scope**

- No legacy initialize handshake, session IDs, GET event stream, or JSON-RPC batches are
supported.
- Resources, prompts, subscriptions, MRTR, Tasks, and Apps are not advertised or implemented.

## Registering tools

```cpp
#include "yahat/Mcp.h"

boost::asio::awaitable<std::string> greet(yahat::ToolContext context,
                                         std::string name) {
    context.throwIfCancelled();
    co_return "Hello, " + name;
}

// server is an existing HttpServer with an application authenticator.
auto mcp = std::make_shared<yahat::McpServer>(
    yahat::McpServer::Identity{"myapp", "1.0"});
mcp->add(yahat::makeTool("myapp_greet", "Greet a person", {"name"}, greet));
mcp->mount(server, "/mcp"); // freezes registration and retains shared ownership
```

Register before starting HTTP service. `add()` owns descriptors and metadata;
`freeze()` rejects further mutation and checks help-name collisions. Public tool
names are explicit and independent of server identity. Registration is a runtime
API; it does not require a constexpr registry. Class methods use
`makeTool(name, description, arguments, instance, &Class::method)`. The bound
instance must outlive the registry **and every outstanding coroutine**, including
work finishing after client disconnection. Capturing awaitable lambdas are also
supported, provided their captures remain valid.

## Coroutines, context and cancellation

An optional **first** `ToolContext` parameter is injected, not model supplied.
It contains the authenticated `Auth`, request ID, correlation ID, request metadata,
and Asio executor. `isCancelled()` reads shared atomic state. SSE disconnection
sets it; cancellation is cooperative. Check around waits and loop iterations, or
call `throwIfCancelled()`. An outstanding wait is allowed to finish; cancellation
does not forcibly interrupt threads, roll back work, or create background tasks.
`co_await context.progress(completed, total, message)` emits progress only when
the caller supplies a progress token. Progress values must strictly increase. Progress is marshalled to the invocation
executor; await it before returning. Blocking work still needs an application worker
pool; asynchronous handlers can run concurrently.

## Typed arguments and custom conversion

Arguments use strict name/type validation, with generated schemas for booleans,
bounded integers, finite floating-point values, strings, optionals, vectors,
string-keyed maps, and Boost.JSON values/objects/arrays. Missing required keys,
unknown keys, incompatible types, and integer overflow fail before handler entry.
Integer parameters require JSON integer values, without string coercion or
fractional truncation. Optional arguments accept both missing and null. Errors
identify nested paths using JSON Pointer escaping without echoing input values.
`boost::json::value` accepts any JSON type; `object` and `array` enforce their
container type. Raw JSON internals need application domain validation.

Specialize `yahat::McpConverter<T>` with `decode(value, path)` and `schema()` for
custom argument types. The schema describes the public contract; the converter
must enforce it. This facility is not a general JSON Schema validator. Generated
schemas use JSON Schema 2020-12; application schema/converter customizations must
remain synchronized. Freeze rejects external schema references, other declared
dialects, and `x-mcp-header` annotations, whose transport mapping is not implemented.

## Results and errors

Ordinary return values use `boost::json::value_from`, including its standard
`tag_invoke(value_from_tag, value&, const T&)` customization for domain results.
Strings, numbers, and raw JSON become `structuredContent` plus a text block.
Objects/arrays are inserted as JSON data without double encoding. `void` produces
empty content. Return `ToolResult` for explicit content blocks and optional
structured content. Throw `ToolError` for safe, actionable execution failures;
the executor sets `isError`. Unexpected exceptions are logged without their
message and return a generic failure. Unknown tools and malformed MCP requests
use the JSON-RPC error channel.

## Authentication and HTTP policies

Authentication runs through yahat before MCP handlers. Basic/bearer credential
validation, token acquisition, OAuth discovery/challenges, and domain permissions
belong to the application. A descriptor's `authorize(context)` hook runs before
each tool invocation; it can inspect `context.auth()` (including `Auth::extra`)
and throw `ToolError` to deny. Handlers can perform more granular checks. Never
use clientInfo or metadata as trusted identity. The frozen catalog is identical
for all callers and carries public cache hints; tool names/descriptions/schemas
must be suitable for every authenticated catalog reader. Invocation authorization
is independent. Help checks the target tool's authorization hook.

`mount()` installs an exact POST route, inherits authentication, rejects every
present Origin by default, and permits absent Origin for nonbrowser clients.
Pass `RouteOptions.allowed_origins` to allow explicit origins. CORS is off by
default; applications serving browser clients can explicitly configure preflight
and MCP header permissions through route policies. Bind local services to
loopback explicitly, as in the example. Configure HTTP parser/body/decompression
limits on `HttpConfig` as well as MCP limits: endpoint limits apply after the HTTP
body has been read. Requests must include `Content-Type: application/json`, an
Accept list containing both `application/json` and `text/event-stream`, and the
revision's required routing headers and metadata. Header/body mismatches fail
with HTTP 400 and `HeaderMismatch` (`-32020`).

## Limits and shutdown

`McpServer::Options` bounds input, output, help topics, concurrent calls, and tool
calls per minute. Defaults are 1 MiB input/output, 32 KiB help, 64 active calls,
600 calls/minute, and 64 tools/page. The rate window is global per server; add
principal/domain-specific limits in authorization hooks. Output limits bound
serialized responses, cumulative progress output, and the SSE queue (which reserves
space for a final response); applications must also bound their own
allocations while building results. Tool callbacks must cooperate with shutdown
and cancellation. Stopping the HTTP context does not await arbitrary application
work; manage application resource lifetimes accordingly.

## On-demand help

Optional `<server-name>_help` is enabled by default. Descriptor `help` maps explicit
topic keys to text. With a `help_provider`, the same keys form an allowlist for
lazy application-owned file/embedded-resource loading; keys are never interpreted
as filesystem paths by yahat. Omitting `topic` returns an overview/topic index.
Help responses fail explicitly when oversized; divide large documents into
smaller topics. Set `enable_help=false` to omit the built-in tool.

## Examples and validation

The [MCP example](../examples/mcpserver/main.cpp) demonstrates a typed coroutine,
progress, application bearer authentication, and a raw configuration object with
selective help. Build with examples enabled, set `YAHAT_MCP_EXAMPLE_TOKEN`, then
run `yahat-mcp-example [port]`. The example binds only to 127.0.0.1.

An optional interoperability smoke test uses the official Python SDK (tested with
`mcp==2.3.0`):

```sh
python tests/mcp_client_smoke.py /path/to/build/bin/yahat-mcp-example
```

The C++ tests have no Python dependency. Validate MCP ON/OFF with metrics ON/OFF;
JSON-RPC can also be enabled independently.

For the full local suite, see [MCP conformance testing](mcp-testing.md).
The [yahat-mcp agent skill](../.agents/skills/yahat-mcp/SKILL.md) teaches coding
agents this API; its [installation guide](../.agents/skills/yahat-mcp/references/install.md)
explains how to use it in other projects.
