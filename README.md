# yahat-cpp

Yet Another HTTP API Thing — a small HTTP server you can embed in a C++20 app.
I kept writing the same bits for REST APIs and microservices, so I put them in a
library. Bring your handlers and your application's authentication.

It does HTTP/HTTPS and Server-Sent Events, with optional OpenMetrics, JSON-RPC
and MCP support. It runs on Linux, macOS and Windows. Still beta.

[![Build Status - Ubuntu](https://github.com/jgaa/yahat-cpp/actions/workflows/ubuntu_build.yaml/badge.svg?label=Ubuntu%20Build)](https://github.com/jgaa/yahat-cpp/actions/workflows/ubuntu_build.yaml)
[![Build Status - macOS](https://github.com/jgaa/yahat-cpp/actions/workflows/macos_build.yaml/badge.svg?label=macOS%20Build)](https://github.com/jgaa/yahat-cpp/actions/workflows/macos_build.yaml)
[![Build Status - Windows](https://github.com/jgaa/yahat-cpp/actions/workflows/windows_build.yaml/badge.svg?label=Windows%20Build)](https://github.com/jgaa/yahat-cpp/actions/workflows/windows_build.yaml)

| Looking for… | Start here |
| --- | --- |
| Building, dependencies and CMake integration | [Getting started](docs/building.md) |
| Publishing a version and understanding build triggers | [Releases and workflows](docs/releasing.md) |
| Routes, authentication, HTTPS and SSE | [HTTP server](docs/http.md) |
| Application metrics and `/metrics` | [OpenMetrics](docs/metrics.md) |
| JSON-RPC calls, notifications and coroutines | [JSON-RPC 2.0](docs/json-rpc.md) |
| Typed MCP tools, cancellation and on-demand help | [MCP server](docs/mcp.md) |
| A little chat app and its Docker image | [Yahat Chat](docs/chat.md) |
| MCP integration and official conformance tests | [Local MCP validation](docs/mcp-testing.md) |
| Helping a coding agent build REST endpoints | [The yahat-rest-api skill](.agents/skills/yahat-rest-api/SKILL.md) |
| Helping a coding agent use yahat in another project | [The yahat-mcp skill](.agents/skills/yahat-mcp/SKILL.md) and [installation](.agents/skills/yahat-mcp/references/install.md) |

Want working code? Have a look at the [API](examples/apiserver/apiserver.cpp),
[chat](examples/chatserver/) and [MCP](examples/mcpserver/main.cpp) examples.
