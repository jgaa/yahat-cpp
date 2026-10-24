# Local MCP validation

From the repository root:

```sh
python3 tests/run_mcp_tests.py
```

The runner configures and builds yahat with MCP enabled, runs the existing GTest
HTTP/JSON-RPC/MCP executables, starts the real C++ coroutine test server on
loopback, verifies discovery readiness, exercises the fixture's typed tools and
help, then runs the official server scenarios. It stops the server on success,
failure, SIGINT and SIGTERM. Nothing is added to CI.

Native validation also installs the library into a temporary build-local prefix,
then configures, builds and runs a separate `find_package(yahat-cpp)` consumer.
It checks exported dependencies, C++20 requirements and (when enabled) a typed
MCP call without borrowing targets from the yahat build tree.

Prerequisites: Linux or another POSIX environment, Python 3.9+, CMake 3.21+,
a C++20 compiler, the normal yahat dependencies (Boost 1.81+, OpenSSL, zlib,
threads), GTest, and Node.js 22+ with npm. Node/npm are only needed for official
tests, never for the library or native tests. The first official run needs
network access to install npm dependencies; later runs can use npm's cache.
No Python packages or global npm installation are needed.

The default build directory is `build/mcp-tests`. To reuse a configured build
with compiler/dependency settings, pass `--build-dir /path/to/build`. The script
turns on `YAHAT_ENABLE_MCP` and `YAHAT_WITH_TESTS`; other cached settings remain
in effect. Build parallelism defaults to two jobs (`--jobs N`).

```sh
# No Node.js tooling or server process required:
python3 tests/run_mcp_tests.py --native-only

# Official tests plus HTTP fixture checks, omitting GTest execution:
python3 tests/run_mcp_tests.py --conformance-only

# One official scenario, with detailed output:
python3 tests/run_mcp_tests.py --conformance-only --scenario server-stateless --verbose
```

`--port PORT` selects a port instead of allocating an ephemeral port. Occupied
ports are rejected. `--executable PATH` overrides the fixture binary; it must
accept `PORT DOCUMENTATION_FILE` and expose the same `conformance` identity.
The runner still configures/builds the fixture to keep the normal setup verified.
Only one runner should use a given build directory at a time.

## Specification and dependency pin

Tests target **MCP 2026-07-28**, the stateless lifecycle with per-request metadata.
They do not require initialization, sessions or removed legacy methods.

The official framework is **@modelcontextprotocol/conformance 0.2.0-alpha.12**
(upstream npm git revision `f44482ba17df816d3176962a11cdf36aec9bda00`). The stable
0.1.16 release does not cover this protocol revision. The version is defined in
root `CMakeLists.txt`; CMake generates `mcp-conformance/package.json` inside the
build directory from `package.json.in`. `package-lock.json` pins transitive
versions and integrity hashes. The runner uses `npm ci --ignore-scripts` in that
build directory. Upgrades require reviewing the lockfile, scenario inventory,
baseline and results together.

The runner inspects the installed CLI and enumerates its server scenarios on
every run, verifies every scenario is classified in `scenarios.json`, checks
revision applicability, and always supplies `--spec-version 2026-07-28`.
New/missing scenarios fail setup rather than silently changing coverage. The
framework's default active suite is intentionally not used. Neither an
all-features requirement score nor full MCP conformance is claimed.

Upstream references:
[README](https://github.com/modelcontextprotocol/conformance/blob/f44482ba17df816d3176962a11cdf36aec9bda00/README.md),
[SDK integration](https://github.com/modelcontextprotocol/conformance/blob/f44482ba17df816d3176962a11cdf36aec9bda00/SDK_INTEGRATION.md).

## Coverage and interpretation

The official selection covers stateless discovery/metadata/errors, tool listing
and deterministic ordering, text/image/audio/embedded/mixed tool content, tool
errors, progress, JSON Schema keyword preservation, concurrent HTTP requests,
origin protection, cache hints, and routing-header validation.

Native GTests cover C++ conversion hooks and numeric bounds, rejection before
application invocation, runtime descriptor validation, frozen registries,
member coroutines, owned arguments/context, concurrent authenticated contexts,
exceptions before/after suspension, cancellation/disconnection, output limits,
and progressive help. Registration is runtime-owned; constexpr/static
registration is not a separate API. HTTP fixture checks additionally exercise
nested typed input, structured output, optional/missing/null arguments, member
calls, and loading an application-selected external help file, including when
that file disappears. Discovery byte sizes are recorded to detect accidental
help-document expansion.

`ToolResult` supports content blocks without requiring the server to implement
resource listing/reading. Schema preservation tests do not claim a general
JSON Schema validator: domain converters/handlers enforce application schemas.
Authentication tests exercise application auth and tool authorization hooks;
this fixture does not implement an OAuth authorization server.

`scenarios.json` records every excluded scenario and its reason: legacy-only
features, unadvertised resources/prompts/completion, MRTR, parameter-header
annotations, and optional extensions including tasks/skills. Unknown exclusions
must be reviewed when updating the pin.

## Expected failures and known coverage gaps

`baseline.yml` uses the framework's **per-check** expected-failure mechanism.
Unexpected failures and stale baseline entries return nonzero. No entire
scenario is baselined; no known mandatory violation is excused.

| Scenario / check suffix | Classification and reason |
| --- | --- |
| `server-stateless:sep-2575-server-rejects-undeclared-capability` | Optional-feature fixture gap: diagnostic sampling tool is absent because client-dependent MRTR is unsupported. |
| `server-stateless:sep-2575-missing-capability-http-400` | Same gap; HTTP status for the unavailable capability-dependent operation cannot be exercised. |
| `server-stateless:sep-2575-http-server-no-independent-requests-on-stream` | Optional-feature fixture gap: runner requires `test_streaming_elicitation`; yahat does not implement elicitation/MRTR. Ordinary SSE calls/progress/disconnects are tested separately. |
| `caching:sep-2549-prompts-list-caching-hints` | Unsupported optional prompts; runner calls the unadvertised method unconditionally. |
| `caching:sep-2549-resources-list-caching-hints` | Unsupported optional resources; same unconditional probe. |
| `caching:sep-2549-resources-templates-list-caching-hints` | Unsupported optional resource templates; same unconditional probe. |

These six checks remain visible as failures in upstream `checks.json` and as
expected failures in the console/summary. Other stateless and caching checks
remain enforced. Upstream also reports skipped checks for unadvertised
subscriptions/list changes and resource reading. Its multiple-stream scenario
uses `tools/list`, which yahat legitimately answers with JSON; its SSE-only
functional check is informational. Native tests exercise real simultaneous
SSE tool calls. This is a coverage distinction, not evidence of SSE conformance
from the official scenario alone.

Two narrow production fixes were found by the first official run: routing
header mismatches now take precedence over unsupported-body-version errors,
and SSE responses send `Connection: close` because their continuation closes
the socket. The latter prevents clients from trying to reuse the closing
connection. Both have native regressions and official scenario coverage.

## Results and reproduction

Every run preserves logs in a unique ignored `tests/mcp-results/TIMESTAMP-*`
directory, printed at exit. Nothing generated is committed.

- `summary.json`: final exit status, native/fixture results, framework version,
  per-scenario check counts, failed/skipped check details and exclusion reasons.
- `native.xml` and `native.log`: CTest results with individual GTest output.
- `fixture-checks.json`: HTTP fixture results and documentation/discovery sizes.
- `server.log`, `configure.log`, `build.log`, `npm.log`: setup and server diagnostics.
- `scenarios.log`, `runner-help.log`: installed runner capabilities.
- Each scenario directory contains `runner.log` and the upstream `checks.json`
  and request/response artifacts, preserved unchanged.

A successful exit means all native/fixture checks and every **unbaselined**
selected official check passed, not that every optional feature exists.
To reproduce a failure, rerun with its `--scenario NAME --verbose`; inspect the
check's error message and HTTP transcript. For example:

```sh
python3 tests/run_mcp_tests.py --conformance-only --scenario http-header-validation --verbose
python3 tests/run_mcp_tests.py --conformance-only --scenario caching --verbose
```

Missing dependencies, failed builds, crashes, readiness failures, missing check
artifacts, timeouts, unexpected failures and stale baseline entries all return
nonzero. SIGINT/SIGTERM return 130 after cleaning up process groups.
