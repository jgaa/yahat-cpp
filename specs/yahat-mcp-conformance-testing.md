# Task: Add MCP Server Conformance and Integration Tests to yahat

## Objective

The MCP server implementation in yahat is complete. Add a comprehensive, reproducible test setup that validates protocol compliance and the correctness of yahat's C++ MCP API.

Use the official Model Context Protocol conformance framework wherever possible, supplemented by native tests for functionality specific to yahat.

**For now, the tests must run locally through a single Bash or Python script. Do not add CI integration.**

The test runner should be suitable for eventual CI integration without requiring substantial restructuring.

## 1. First examine the existing implementation

Before making changes, inspect the repository to understand:

- The existing build system and test infrastructure.
- The MCP server implementation and supported protocol version.
- How MCP endpoints integrate with yahat's HTTP server.
- The public tool-registration API.
- Argument conversion and validation.
- Asio coroutine-based tool execution.
- Tool result serialization and error handling.
- Authentication and request-context handling.
- The existing examples and test utilities.
- Any existing MCP tests.

Prefer existing project conventions and dependencies.

Do not introduce a new test framework if the existing one can reasonably handle the required tests.

Do not refactor production code unnecessarily. Fix production defects discovered by testing, but keep such changes narrowly scoped and document them.

## 2. Official MCP conformance testing

Integrate the official conformance framework:

https://github.com/modelcontextprotocol/conformance

Relevant documentation:

https://github.com/modelcontextprotocol/conformance/blob/main/README.md

https://github.com/modelcontextprotocol/conformance/blob/main/SDK_INTEGRATION.md

The framework supports testing an independently running MCP server over HTTP.

The basic invocation is:

```bash
npx @modelcontextprotocol/conformance server \
    --url http://127.0.0.1:PORT/mcp
```

The framework also supports selecting individual scenarios, suites, and expected-failure baselines.

### Protocol version

The yahat implementation targets MCP 2026-07-28.

This revision introduces the stateless lifecycle and differs substantially from earlier revisions.

Ensure that the tests validate the protocol version actually implemented by yahat.

In particular:

- Do not incorrectly require legacy initialization behavior.
- Do not mistake unsupported legacy session semantics for defects.
- Use applicable 2026-07-28 scenarios and requirements.
- Inspect the installed runner's capabilities instead of assuming its current suite names or defaults always correspond to the targeted revision.

The conformance framework evolves independently of yahat. Pin a suitable version rather than relying on an unversioned latest release.

### Coverage

Run all applicable official server conformance tests for features implemented by yahat.

Investigate coverage for:

- Stateless MCP request handling.
- JSON-RPC parsing and response semantics.
- HTTP transport requirements.
- Protocol version handling.
- Server discovery.
- Tool discovery.
- Tool invocation.
- Input validation.
- Structured tool results.
- Error handling.
- HTTP headers, content types, and status codes.
- Authentication-related behavior, where applicable.
- Resources, prompts, and extensions, if implemented.

Do not assume that every scenario applies to yahat.

Classify unsupported optional features separately from failed mandatory requirements.

Where the conformance framework requires a specialized server fixture to exercise a scenario, implement the fixture behavior if reasonable and within scope.

Do not silently exclude scenarios merely because they fail.

### Conformance baseline

If known failures remain, support an expected-failures baseline using the framework's built-in mechanism.

However:

- Do not automatically add all failures to the baseline.
- Do not hide mandatory protocol violations.
- Document each expected failure.
- Explain whether it represents an unsupported optional feature, an implementation limitation, or an outstanding defect.
- Keep the normal test runner's final result meaningful.

Unexpected failures must cause a nonzero exit status.

## 3. Dedicated MCP test server

Create or adapt a small executable exposing yahat's MCP implementation.

Use the normal public API that applications are expected to use.

The executable should be representative of real yahat-based applications, not a separate mock MCP implementation.

It should exercise the implemented registration mechanisms and use actual C++20 Asio coroutine handlers.

Include representative tools such as:

| Tool | Purpose |
|---|---|
| Echo | String argument conversion and response serialization |
| Add | Numeric conversion and result serialization |
| Optional arguments | Missing, null, and supplied values |
| Structured input | Nested JSON objects and arrays |
| Structured output | Complex result serialization |
| Error | Application-level tool errors |
| Async operation | Coroutine suspension and resumption |
| Member-function tool | Invocation of a class method |
| Help | Built-in progressive documentation functionality |

Adapt the tool set to the actual yahat API.

Avoid implementing unsupported MCP features solely to make the tests pass.

The test executable should:

- Accept a configurable listening port.
- Bind to loopback by default.
- Expose a predictable MCP endpoint.
- Provide a deterministic set of registered tools.
- Avoid dependencies on external services.
- Shut down cleanly.
- Produce useful diagnostics.

Use temporary or ephemeral ports where practical.

Tests should not interfere with a production server or require administrator privileges.

## 4. Native C++ tests

Add focused tests for behavior that the protocol conformance framework cannot adequately verify.

Use the existing C++ test framework if available.

### 4.1 Argument conversion

Test supported C++ argument types.

Include:

- Required arguments.
- Missing required arguments.
- Incorrect JSON types.
- Optional arguments.
- Explicit JSON null.
- Integer range violations.
- Floating-point conversion.
- Boolean conversion.
- Strings.
- Arrays and nested objects.
- Unknown properties, according to the implementation's documented policy.
- Custom conversion hooks, if supported.

Verify that invalid input does not invoke the application handler.

Check that errors contain useful information identifying the invalid argument.

### 4.2 Tool registration

Test:

- Static tool registration.
- Runtime registration, if implemented.
- Duplicate tool names.
- Invalid tool descriptors.
- Tool lookup.
- Deterministic discovery ordering.
- Registry immutability after initialization, if implemented.
- Built-in support-tool naming.
- Collision detection between application and built-in tools.

### 4.3 Coroutine execution

All MCP tool handlers use C++20 Asio coroutines.

Test:

- Successful coroutine completion.
- Suspension and resumption.
- Concurrent tool invocations.
- Exception propagation and conversion into MCP errors.
- Handler failures after suspension.
- Member-function coroutine handlers.
- Request-context lifetime across suspension.
- Cancellation behavior, where supported.

Use deterministic synchronization when possible.

Avoid timing-sensitive tests relying on arbitrary sleeps.

### 4.4 Protocol error semantics

Verify the distinction between:

- Malformed JSON-RPC requests.
- Unknown methods.
- Unknown tools.
- Invalid tool arguments.
- Application-level tool failures.
- Internal server failures.

Recoverable tool execution errors should use MCP tool-error semantics where required.

Protocol errors should use the appropriate JSON-RPC error mechanism.

### 4.5 Security and concurrency

Where supported, test:

- Concurrent requests with distinct authentication contexts.
- Request-context isolation.
- Authorization failures.
- HTTP origin validation.
- Invalid protocol headers.
- Malformed and oversized requests.
- Cancellation or disconnection during tool execution.

Tests must not depend on persistent session state unless the implemented protocol feature explicitly requires it.

## 5. Progressive documentation and help tests

Yahat's MCP design emphasizes small tool descriptions and on-demand documentation.

Test this explicitly.

Verify that:

- `tools/list` does not unnecessarily include complete help documents.
- The built-in help tool is correctly registered.
- Help for a known tool can be retrieved.
- Unknown tool names produce meaningful errors.
- Unknown help topics are handled correctly.
- External documentation resources can be loaded.
- Embedded documentation works, if supported.
- Missing documentation is handled gracefully.
- Large documentation does not unintentionally inflate tool discovery output.
- Tool metadata and documentation references remain consistent.

Avoid exact token-count assertions.

Instead, test observable properties, such as the absence of large help documents in discovery responses.

Where useful, record the byte sizes of tool discovery responses.

The purpose is to detect accidental regressions in yahat's progressive-disclosure design.

## 6. Test runner script

Provide one simple entry point, preferably:

`tests/run_mcp_tests.sh`

or:

`tests/run_mcp_tests.py`

Choose whichever best fits the existing project.

The script should perform the necessary orchestration:

1. Check dependencies.
2. Locate or build the MCP test executable.
3. Start the test server.
4. Wait until the server is ready.
5. Run native tests.
6. Run applicable official MCP conformance tests.
7. Collect results and logs.
8. Shut down the server.
9. Print a concise summary.
10. Return an appropriate exit status.

A developer should be able to execute the complete suite with one command from the repository root.

### Runner requirements

- No manual server startup.
- No hard-coded machine-specific paths.
- Configurable port and executable location if useful.
- Reliable startup readiness detection.
- Cleanup on success, failure, and interruption.
- Proper signal handling.
- No orphaned server processes.
- Nonzero exit status on unexpected failures.
- Useful diagnostics when the server crashes.
- Clear handling of missing dependencies.
- Support for rerunning tests without manual cleanup.

Avoid treating an arbitrary successful HTTP response as proof that the MCP endpoint is ready. Use an appropriate readiness check.

### Useful options

Consider supporting:

- Run all tests.
- Run native tests only.
- Run conformance tests only.
- Run one conformance scenario.
- Verbose output.
- Preserve test logs.
- Specify an existing build directory.
- Override the listening port.

Keep the CLI simple.

Do not add options unless they are useful.

### Dependencies

The official conformance framework requires Node.js tooling.

Do not make Node.js a dependency of the yahat library itself.

Keep it isolated to the test infrastructure.

If packages must be installed, use a reproducible dependency strategy with pinned versions.

Avoid unnecessary global npm installations.

## 7. Reporting

The test runner should produce a concise summary showing:

- Native test results.
- Official conformance results.
- Passed scenarios.
- Failed scenarios.
- Expected failures.
- Skipped or inapplicable scenarios.
- Location of detailed logs.

Preserve sufficient information to diagnose failures without rerunning the entire suite.

If feasible, organize test output under a dedicated directory that can later be archived by CI.

Do not commit generated logs or temporary artifacts.

## 8. Documentation

Add a short README explaining:

- Prerequisites.
- How to run all MCP tests.
- How to run individual scenarios.
- How to run only native tests.
- How to inspect failures.
- Which MCP specification revision is targeted.
- Which official conformance framework version is used.
- Any known limitations.

Document the distinction between protocol conformance and yahat-specific implementation tests.

The setup should be understandable to a developer unfamiliar with the conformance framework.

## 9. Implementation principles

Follow these priorities:

**Use official tests first.** Do not duplicate MCP conformance scenarios unnecessarily.

**Use yahat's real public API.** The test server should exercise the implementation applications actually use.

**Prefer existing infrastructure.** Reuse the build system, test framework, and Asio integration.

**Keep dependencies isolated.** MCP conformance tooling must not affect production builds.

**Keep tests deterministic.** Avoid timing-dependent assumptions, shared global state, and external services.

**Make failures visible.** Do not silently skip failing tests, suppress exit statuses, or manufacture passing results.

**Allow implementation freedom.** Select the file organization and implementation details that best fit the repository.

**Do not add CI configuration yet.** The local script is the entry point for now.

## 10. Acceptance criteria

The task is complete when:

1. A developer can execute one script to run the MCP test suite.
2. The script starts and stops the test server automatically.
3. The official conformance framework tests the real yahat MCP endpoint.
4. Tests target the intended MCP protocol revision.
5. Native tests cover typed argument conversion, errors, registration, and coroutine execution.
6. Progressive help/documentation behavior is tested.
7. Unexpected failures produce a nonzero exit code.
8. Test logs and results are accessible.
9. All test dependencies are documented.
10. No CI configuration has been introduced.

## 11. Final deliverable

Implement the tests and runner in the repository.

Then:

- Execute the complete test script.
- Fix test infrastructure problems.
- Fix straightforward production defects discovered during testing when safe and appropriate.
- Clearly report any remaining protocol failures.
- Summarize the files added or modified.
- State which conformance scenarios pass.
- State which scenarios remain unsupported or fail.
- Explain how to reproduce failures.

Do not report tests as passing unless they were actually executed successfully.

If execution is blocked by environment limitations, report the blocker and provide the exact command needed to run the tests locally.

The ultimate goal is a **repeatable, trustworthy local MCP server validation suite** that can later be integrated into CI with minimal changes.
