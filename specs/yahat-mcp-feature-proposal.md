# Feature Proposal: MCP Server Support for yahat

**Status:** Implementation brief; decisions agreed with project owner
**Target:** yahat HTTP server library (C++20, Asio coroutines)
**Protocol baseline:** Model Context Protocol (MCP) revision **2026-07-28**
**Scope:** Server-side MCP functionality; not an MCP host or client implementation

## 1. The 1000-meter view

Add a lightweight, standards-compliant **MCP server endpoint** to yahat so an application can expose existing C++20 Asio coroutine functions and class methods as tools usable by AI hosts. The application should primarily declare **what the tools are**; yahat should handle **how MCP clients discover, call, validate, and receive results from them**.

The intended experience is:

1. An application constructs an MCP server, giving it an explicit, stable server identity.
2. It registers runtime tool descriptors through a startup-time `add()` API supporting free functions, composition, and object-bound methods. The registry owns metadata and freezes before serving.
3. Each tool has a **complete, fixed public name**, a concise description, optional longer help/documentation, argument metadata, and a coroutine handler.
4. yahat connects the registry to its existing HTTP routing, JSON processing, authentication, request context, Asio executor, and cancellation facilities.
5. An MCP host discovers a **small, deterministic catalog**, optionally requests deeper help, and invokes selected tools. yahat converts JSON arguments to C++ types, invokes the coroutine, and formats results and errors.

**Core design priorities:** ergonomic typed C++ runtime registration; minimum application boilerplate; native Asio coroutines; compact MCP discovery; progressive, on-demand documentation; a frozen registry and predictable caching; strict, helpful argument validation; transport-independent protocol logic where practical; and alignment with existing yahat internals.

**Not the goal:** create a general agent framework, an MCP host, a comprehensive C++ reflection/serialization framework, dynamic per-session tool catalogs, or a new tool-description protocol. Codex should retain freedom to choose internal class layouts, template machinery, JSON types, memory strategies, and routing integration that fit yahat.

Make it a compile time feature with an option in CMAKE, default OFF. Enable other required feature as needed when enabled (like JSON RPC).

## 2. Design principles and decisions

| Area | Decision / direction |
| --- | --- |
| Protocol | Target MCP `2026-07-28` stateless core; do not inadvertently implement legacy session semantics. |
| Transport | Streamable HTTP over yahat is the first delivery mechanism. |
| Handlers | Native **C++20 Asio coroutines**, returning `asio::awaitable<T>` (or yahat's equivalent alias). No sync-first wrapper API. |
| Registration | Runtime descriptors and startup-time `add()`; freeze before serving requests. No constexpr requirement. |
| Public names | Full explicit names such as `nextapp_list_actions`, not inferred or automatically prefixed. |
| Server identity | Explicit, separate from public tool names. May be used to name built-in support tools. |
| Built-in help | Prefer one optional `<server-id>_help` tool rather than many introspection tools. |
| Descriptions/help | Keep as text or resource references, not mandatory pre-parsed JSON objects. |
| Documentation | Short discovery description; deeper structured help or topic docs on demand. |
| Schemas | Generate practical basic schemas from supported C++ types; allow explicit overrides and more detailed server-side validation. |
| Authorization | Evaluate independently on each request and each tool call; do not infer permissions from a connection/session. |
| Error handling | Centralize malformed arguments, type mismatches, conversions, exceptions, and MCP error mapping. |
| Flexibility | Make reasonable defaults easy, with escape hatches for raw JSON, advanced results, and custom conversion. |
| Scope control | Implement core tools well before resources, prompts, tasks, apps, or elaborate runtime registry mutation. |

## 3. User experience and public API requirements

### 3.1 The common case must be concise

A tool should normally be an ordinary application function or class method whose return type is an Asio awaitable. The registry declaration should associate a public name, short description, optional metadata, named arguments, and its handler. Most tool implementations should **not** have to parse JSON, build JSON-RPC envelopes, format MCP content blocks, launch coroutines explicitly, or handcraft missing-field errors.

### 3.2 Runtime descriptors

Use a uniform runtime descriptor with owned metadata and an erased awaitable invocation callback. The application registers typed free functions or bound methods through a concise factory and freezes the registry before serving. A constexpr homogeneous array is not required. Check handler return types and supported conversions at compile time where practical; check argument-name counts and collisions during registration.

### 3.3 Free functions, methods, and request context

Support free/static functions. Support class methods bound to application-owned instances, at least through startup-time registration. Clearly document lifetime obligations for non-owning instance references; optional owning/capturing callbacks can be considered as a separate convenience API if justified.

Permit an optional injected, request-scoped context parameter, which is **not** part of the model-supplied JSON arguments. The context provides the authenticated principal and permissions as exposed by yahat, cooperative cancellation information (application deadlines may be added later), request correlation or tracing information, and useful executor/request facilities without coupling handlers to raw HTTP details unnecessarily.

### 3.4 Configuration and extensibility

The server should accept its identity and registry at construction. A startup-oriented `add()` or equivalent should support modular application registration. After a `freeze()` phase (explicit or implicit before starting service), reject mutations and validate names, collisions, metadata, and schemas. Avoid requiring mutable registries while requests are active.

## 4. Tool identity, discovery, and built-in naming

### 4.1 Tool names are stable API identifiers

Require explicit complete public names, such as `nextapp_list_actions` and `nextapp_execute_action`. Do **not** automatically prepend a prefix, deduce a prefix from the first registered tool, or run search-and-replace on documentation. Prefix inference is ambiguous with underscores, reordered/empty registries, or mixed subsystems; renaming the MCP server should not implicitly rename tools. Keep documentation resource identifiers independent of tool names.

The server identity, for example `nextapp`, is separate from tool names. If a help tool is enabled, derive its default name from an explicit, validated server identifier (e.g., `nextapp_help`), optionally allowing override or disabling. Detect name collisions on freezing. Do not automatically generate other support tools without a real need.

### 4.2 Frozen, deterministic discovery

Return tools in a deterministic order, ideally sorted or otherwise consistently ordered independently of registration order. Build reusable metadata/lookup structures at freeze time where this benefits yahat. Honor `tools/list` pagination and the 2026-07-28 result shape and cache-hint semantics. For a globally identical frozen tool list, sensible positive `ttlMs` and `cacheScope: public` may be appropriate; if caller-dependent, prefer private caching with correct partitioning. Never publicly cache authorization-dependent lists.

**Important protocol nuance:** a stateless MCP transport does not require a static tool list. The specification permits a catalog to change over time or vary with per-request authorization, but disallows connection-history-dependent behavior. For the initial yahat implementation, prefer a fixed catalog, and enforce permissions at invocation. Whether unauthorized tools are advertised is an explicit server policy/security choice, not an assumption that everyone may invoke them.

## 5. Native Asio coroutine execution

All normal handlers use C++20 Asio coroutine return types. The MCP invocation path should parse and validate synchronously where appropriate, then `co_await` the actual application handler on the correct yahat/Asio executor, then serialize the result. JSON parsing and type conversion should not themselves become coroutines merely for uniformity.

The design must account for:

- **Lifetimes across suspension:** parsed/converted arguments must remain valid through the coroutine; prefer owned values for ordinary arguments. Views into request buffers must be an explicit opt-in with proven lifetime guarantees.
- **Context lifetime:** request context and any bound method instance must remain valid until completion.
- **Cooperative cancellation:** provide shared atomic cooperative cancellation in the injected context. Handlers check before work, in loops, and around asynchronous waits. Do not forcibly interrupt a busy thread; an outstanding wait may finish before cancellation is observed. Disconnection is cooperative cancellation, not guaranteed rollback of external side effects.
- **Concurrency:** multiple requests can call the same tool concurrently; tool implementation owners must manage application-state synchronization. Do not add a global dispatcher lock.
- **Exceptions:** catch at the invocation boundary, log or trace internally, map to safe MCP responses, and avoid returning secrets or stack traces.
- **Long-running operations:** an awaitable handler is not itself MCP Tasks support. Task handles and polling are an optional future protocol extension.

## 6. Typed JSON argument conversion

The strongest usability requirement is that applications receive normal typed C++ parameters while yahat centrally handles absent/wrongly typed values.

### 6.1 Initial useful type coverage

Support ordinary booleans, bounded signed integers, floating-point values, strings, optional values, arrays/vectors, and a suitable map/object form as supported by yahat's existing JSON system. Provide a raw JSON argument escape hatch for complex or open-ended documents, plus a documented extension mechanism for user-defined types. Prefer leveraging existing yahat JSON facilities rather than creating a separate serialization framework.

For complex domain objects, application authors may choose either a custom converter to a typed structure or a raw JSON value plus domain validation. Avoid requiring every deeply nested JSON configuration to become a reflected C++ class.

### 6.2 Strict conversions and validation

By default, do **not** coerce numeric strings into numbers, silently truncate fractional numbers to integers, accept out-of-range integers, or quietly discard unsupported JSON types. Define an explicit policy for missing vs. `null`: missing required arguments fail; optional parameters may treat missing and explicit null as empty if documented. Detect invalid argument containers, missing required keys, wrong types, invalid values, and conversion failures. Decide and document handling of unknown fields (strict by default is worth considering; make an escape hatch for extensible objects).

Error information should include the argument path (preferably JSON Pointer-compatible for nested values), expected and actual types, a concise human-readable explanation, and optional actionable remediation or help-topic reference. Avoid dumping an entire schema or user-supplied secret into error text.

### 6.3 Schema generation and independent server validation

Validate argument names, required/optional counts, compatible types, numeric ranges, and nested containers directly from the typed registration. Reject unknown arguments by default. No general JSON Schema validation engine is required. Custom converters enforce domain constraints and supply matching public schemas.

Generate a useful `inputSchema` from argument names and supported C++ types, including required/optional status and basic constraints where expressible. Allow explicit schemas and custom validation when the generated schema cannot capture domain rules. Make it possible to expose a **compact public schema** while validating against a more detailed internal contract for very complex JSON payloads.

This separation is intentional but has a cost: clients cannot prevalidate constraints that the public schema omits. Therefore, validation errors must be precise and repairable. Do not automatically reduce all schemas to opaque objects: ordinary small tools should publish complete, helpful schemas with no extra help round trip. `outputSchema`, if provided, should be useful and reasonably sized; complex output documents can be described on demand where permitted by the protocol.

## 7. Progressive documentation and token efficiency

### 7.1 Problem statement

A naive MCP host may place every advertised tool definition directly into the LLM context. Large nested schemas, exhaustive field descriptions, and long examples can consume substantial context even when most tools are never invoked. Host-side progressive discovery is helpful, but yahat's server should also be efficient with naive hosts.

### 7.2 Three disclosure levels

1. **Discovery:** concise tool name, purpose, minimal useful argument contract, and a cue to retrieve detailed help when needed.
2. **Understanding:** topic-based help with detailed semantics, schemas, examples, configuration constraints, and error explanations, requested only when relevant.
3. **Execution:** real inputs and bounded results, with precise failures that support retries.

Do not replace one context explosion with a help tool that returns all documentation in one call. Prefer a summary/index response, topic retrieval, and optional path-level navigation for nested JSON structures. Make help output bounded and offer explicit continuation or topic selection instead of silently truncating schemas.

### 7.3 Built-in help tool

Provide an **optional** conventional MCP tool, named by default `<server-id>_help`, which accepts a target public tool name and optionally a topic/path. Its initial response should normally provide the tool overview and available help topics; subsequent requests can provide a specific topic, selected schema subtree, constraints, or examples. Help should clearly distinguish standard MCP capabilities from yahat's own conventions. Consider exposing documentation as MCP resources later, reusing the same underlying provider; do not rely on hosts exposing resources to models.

### 7.4 Skills are complementary, not required

Skills describe **how to accomplish workflows** using multiple operations; help documents **what individual interfaces mean**. Do not make skills support a requirement for correct tool use. A skill can point to help topics when an environment supports skills, but any compliant MCP tool caller should be able to retrieve necessary information through standard `tools/call`.

### 7.5 Documentation storage

Allow concise description and help references to be `std::string_view` or equivalent lightweight text-oriented metadata. Documentation may be inline text, Markdown or JSON text files loaded from a directory, or blobs from an application embedded-resource repository. The proposal does **not** require descriptions/help to be stored as JSON DOM values. Distinguish literal text from a file path or resource key explicitly in metadata/provider configuration. Manage lifetime/ownership: a non-owning string view must remain valid; dynamically loaded content needs owned backing storage. Load and parse extended documentation lazily when beneficial, subject to size limits.

### 7.6 Measure effectiveness

Evaluate **both** tokens used for `tools/list` and **end-to-end task cost** (discovery + help + calls + recovery). Also measure validation retries, latency, size of returned content, and prompt-cache stability. Short descriptions that trigger many failed calls or repeated help lookups are not automatically better.

## 8. MCP protocol surface and transport

Implement against the published `2026-07-28` revision, rather than relying on tutorials written for the legacy session-based era. Keep protocol parsing and execution conceptually separate from yahat's HTTP transport glue, while avoiding unnecessary abstraction layers.

Initial mandatory/priority items:

- UTF-8 JSON-RPC 2.0 framing and correct request IDs, notification and error behavior.
- MCP revision-specific request metadata, protocol version checks, and **`server/discover`** support as required by this revision.
- The `tools` capability, `tools/list` (including pagination and relevant cache hints), and `tools/call` with the 2026 response format.
- Current Streamable HTTP binding: one configured HTTP POST endpoint, one message per POST, required MCP routing/metadata headers including `Mcp-Method` and `Mcp-Name`, and reject header/body mismatches as specified.
- Correct HTTP responses, MIME/Accept handling, and JSON request/response handling. Evaluate request-scoped SSE support against the mandatory server-side behavior and selected supported features; **do not** create persistent session/SSE architecture simply because earlier MCP versions used it.
- Correct tool result content and structured content where supported; server identity and capability metadata in their current spec-defined locations.
- Request-body limits, input validation, origin checks, authentication integration, and controlled failure paths.

The 2026 revision removes the old mandatory `initialize`/`initialized` handshake and protocol session IDs. Any application state that persists between calls should use explicit identifiers/handles and authorization checks, not a hidden MCP pseudo-session.

### 8.1 Error taxonomy

Distinguish **protocol errors** (malformed JSON-RPC/MCP envelopes, unknown tool, invalid method, unsupported protocol) from **tool execution errors** (typed conversion/validation failures, domain constraints, API failures). Use the standard JSON-RPC error channel for the former and tool results with `isError` for actionable execution failures, following the spec. Do not turn every exception into HTTP 500, nor report an unknown tool as a successful tool result.

### 8.2 Security and isolation

Use yahat's existing authentication hooks. Treat the authenticated principal and permissions as **request-scoped**; reauthorize each invocation, including operations using server-generated state handles. Validate HTTP `Origin` per the transport rules (including explicit behavior when absent), use safe local binding defaults, and avoid leaking secrets through logs, tool errors, or help documentation. Apply request/tool rate limits and payload/output size bounds. Sanitize output where appropriate, and expose annotations about potentially destructive tools when supported. Tool annotations are hints, not security controls.

## 9. Registry lifecycle and implementation freedom

Expected lifecycle: construct server with identity and static descriptors; optionally register additional application modules and bind method instances; verify/freeze; mount into yahat routing; serve concurrent requests. Freeze should catch duplicate/invalid names and built-in collisions, invalid metadata, unsupported handler signatures and schemas, and incompatible configuration. It may precompute lookup tables, schemas, catalog serialization, and a registry fingerprint if useful.

The proposal deliberately does not dictate:

- exact C++ class names, namespace layout, template signatures, or compile-time string technique;
- whether the uniform callable internally uses function pointers, an erased invoke thunk, or another allocation-conscious representation;
- choice of JSON DOM/parser, schema validator, memory arena, or string storage;
- whether `freeze` is an explicit public call or an automatic startup phase;
- exact resource repository integration; or
- internal coroutine spawning/dispatch plumbing.

Those should be decided by Codex based on the real yahat codebase and conventions.

## 10. Phased implementation proposal

**Phase A — Small working vertical slice.** A single MCP HTTP endpoint serving stateless discovery and a frozen list of simple tools; typed free-function Asio coroutine invocation; basic C++/JSON conversions; correct result and error envelopes; protocol metadata and transport validation; simple test application.

**Phase B — Production-worthy developer API.** Member-function binding, context injection, richer converters and custom conversion hooks, robust validation diagnostics, request cancellation, security integration, deterministic cacheable catalogs, and startup-time modular registration.

**Phase C — Progressive help.** Documentation sources/providers (inline/file/embedded), topic index and selective help, optional built-in help tool, schema-depth/size controls, precise help references in validation errors, documentation bounds and caching.

**Phase D — Interoperability and optional extensions.** Additional MCP resources/prompts only when justified by projects; additional request-scoped SSE features; then assess MRTR, Tasks, MCP Apps, subscription handling, and other extensions individually. Avoid accidentally advertising capabilities that are not implemented.

A phase may be folded into another if yahat already provides most of the infrastructure; the sequencing expresses priorities, not a mandated sprint plan.

## 11. Acceptance criteria and verification

- A yahat application can expose a free-function Asio coroutine as a typed MCP tool with minimal registration metadata and no manual JSON-RPC construction.
- An application can expose an instance method and obtain request-scoped context; ownership and lifetime are documented.
- A uniform runtime registry owns metadata and accepts typed free functions and bound methods without a constexpr requirement.
- A startup-time `add()` path works, and registration is immutable once serving begins.
- Tool names are complete and fixed; server identity is independent; optional `<server-id>_help` is collision checked.
- Basic C++ argument types round-trip accurately; missing, null, wrong type, numeric overflow, malformed JSON, and nested-validation failures produce deterministic, useful errors.
- Public schemas are correct for simple tools; deeper documentation can be retrieved selectively for complex tools without dumping the entire configuration schema into `tools/list`.
- MCP 2026-07-28 discovery, tool listing, invocation, transport headers, and response metadata pass protocol-level tests and interoperate with at least one independent current MCP client/inspector.
- HTTP origin/auth/security behavior is verified; different authenticated callers cannot bypass per-request authorization; concurrent requests and cancellation are tested.
- Frozen catalog results are stable, ordered, and safely cacheable according to their scope.
- Integration tests cover coroutine suspension, exception paths, instance lifetime, oversized input/output, help topic traversal, and accidental information disclosure.
- Documentation includes at least one straightforward typed tool and one complex JSON/configuration tool with progressive help, **without prescribing library internals as sample implementation code**.

## 12. Explicit non-goals / deferred decisions

- Implementing an MCP host or orchestrating LLM tool selection.
- Assuming MCP hosts support progressive discovery, skills, or resources.
- Supporting historic MCP revisions or sessionful compatibility unless a concrete integration needs it.
- Mutable, user/session-specific runtime tool registration as the initial model.
- Automatic namespace inference or documentation rewriting.
- A general-purpose C++ reflection system; exhaustive JSON Schema synthesis for arbitrary domain classes.
- Persistent background jobs merely because a coroutine handler suspends; MCP Tasks are separate.
- Full MCP Apps UI, resources/prompts workflows, MRTR, or advanced extensions in the first release.
- A single universal rule for how much schema to disclose: compactness must be balanced with correctness and usability.

## 13. Questions for implementation discovery (not blockers)

These are decisions Codex should resolve by inspecting yahat rather than asking for new product requirements upfront:

1. Which Asio namespace/awaitable aliases, executors, and cancellation primitives does yahat already expose?
2. Which JSON types and serializers can be reused for schema generation, conversion, and structured results?
3. What route-handler lifecycle guarantees are available, and how is request authentication represented?
4. Where are application embedded resources and text documentation conventionally stored?
5. How should tools return user-visible text, structured JSON, or explicit tool errors without forcing callers to learn MCP internals?
6. Which server-side requirements need SSE, subscription, or other capability handling for full advertised conformance, and which can be safely excluded from the initial capability set?

## Agreed implementation decisions

- Native Asio awaitable tool handlers; preserve existing stackful HTTP handlers and synchronous JSON-RPC APIs. Add asynchronous JSON-RPC dispatch without changing existing callback signatures.
- Optional first injected tool context with owned identity/metadata and shared atomic cancellation; SSE disconnects set the flag. Progress is emitted on the invocation executor. Cancellation callbacks and forced cancellation of outstanding waits are deferred.
- Runtime descriptors; no constexpr registry requirement. Bound application instances must outlive outstanding invocations.
- Strict typed argument conversion and name/count validation; optional missing/null semantics; custom converters plus raw JSON for domain documents. No general JSON Schema validation dependency.
- Application authentication remains before MCP handler invocation. Application authorization runs on every call, through a hook or handler context. Full OAuth discovery/token infrastructure is application-owned.
- Ordinary and custom Boost.JSON-convertible results become MCP result data. Raw JSON objects/arrays remain JSON data. Explicit content blocks have an advanced result escape hatch.
- Explicit safe tool exceptions become `isError` results, protocol errors use JSON-RPC errors, and unexpected exception details remain private.
- Request-scoped SSE, authorization, cancellation, and input/output limits are included in the first implementation. Help topics are bounded, selected explicitly, and may be loaded lazily through an application provider.

## 14. Protocol references

These links are reference material, **not** instructions to copy an SDK architecture:

- MCP tools, schemas, authorization-dependent listing, and error model: https://modelcontextprotocol.io/specification/2026-07-28/server/tools
- Streamable HTTP binding: https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- Transport concepts: https://modelcontextprotocol.io/specification/2026-07-28/basic/transports
- Client-side progressive discovery and programmatic calls (motivation, not a server requirement): https://modelcontextprotocol.io/docs/2026-07-28/develop/clients/client-best-practices
- Revision overview: https://blog.modelcontextprotocol.io/posts/2026-07-28/

---

**Implementation brief:** Build a small, native-C++20/Asio MCP server facility for yahat with a stable frozen tool registry, typed coroutine invocation, central JSON conversion and errors, safe per-request context, and compact discovery complemented by optional on-demand help. Favor yahat's existing patterns over literal API designs in this proposal.
