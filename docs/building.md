# Building and using yahat

Yahat uses C++20 and runs on Linux (tested on Ubuntu), macOS and Windows.
You need Boost, OpenSSL, zlib and threads; minimum dependency versions are defined
in the root [CMakeLists.txt](../CMakeLists.txt). GTest is needed when tests are enabled.

yahat-cpp is designed to be easily consumed by other CMake projects using modern CMake practices. You can integrate it using any of the following methods:

## Method 1: Using find_package (after installation)

First, install yahat-cpp:

```bash
git clone https://github.com/jgaa/yahat-cpp.git
cd yahat-cpp
cmake -S . -B build -DYAHAT_WITH_TESTS=OFF -DYAHAT_WITH_EXAMPLES=OFF
cmake --build build --parallel
cmake --install build --prefix "$HOME/.local"
```

Configure your application with `-DCMAKE_PREFIX_PATH="$HOME/.local"`, then use:

```cmake
find_package(yahat-cpp CONFIG REQUIRED)
target_link_libraries(your_target yahat-cpp::yahat-cpp)
```

## Method 2: Using FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(yahat-cpp
  GIT_REPOSITORY https://github.com/jgaa/yahat-cpp.git
  GIT_TAG main  # or a specific tag/commit
)

# Configure yahat before adding the dependency.
# Set YAHAT_ENABLE_MCP ON here if your application needs MCP.
set(YAHAT_WITH_TESTS OFF)
set(YAHAT_WITH_EXAMPLES OFF)

FetchContent_MakeAvailable(yahat-cpp)

target_link_libraries(your_target yahat-cpp::yahat-cpp)
```

## Method 3: Using add_subdirectory

If you have yahat-cpp as a subdirectory in your project:

```cmake
# Configure yahat before adding the dependency.
# Set YAHAT_ENABLE_MCP ON here if your application needs MCP.
set(YAHAT_WITH_TESTS OFF)
set(YAHAT_WITH_EXAMPLES OFF)

add_subdirectory(path/to/yahat-cpp)
target_link_libraries(your_target yahat-cpp::yahat-cpp)
```

## Dependencies

The installed package discovers its dependencies automatically and exports its
C++20 requirement through `yahat-cpp::yahat-cpp`. Building yahat requires these
dependencies (Boost's minimum version is defined in the root CMake file):

```cmake
find_package(Boost REQUIRED COMPONENTS program_options coroutine context chrono json url)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
```

## Feature switches

Set options before configuring a source dependency. For `find_package`, these
options must have been set when the installed library was built; defining a
consumer preprocessor macro does not add a disabled feature to the library.

| CMake option | Default | Documentation |
| --- | --- | --- |
| `YAHAT_ENABLE_METRICS` | ON | [OpenMetrics](metrics.md) |
| `YAHAT_ENABLE_JSON_RPC` | OFF | [JSON-RPC 2.0](json-rpc.md) |
| `YAHAT_ENABLE_MCP` | OFF | [MCP server](mcp.md); also enables JSON-RPC |
| `YAHAT_WITH_TESTS` | ON | GTest tests and installed-package validation |
| `YAHAT_WITH_EXAMPLES` | ON | API, chat and (when enabled) MCP examples |

For example, enable MCP when configuring a standalone yahat checkout:

```sh
cmake -S . -B build -DYAHAT_ENABLE_MCP=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Keep installed headers and the library from the same build. See the
[HTTP setup example](http.md#minimal-server-setup) or the
[complete API server](../examples/apiserver/apiserver.cpp) to get started.
For MCP-specific tests and pinned conformance tooling, see
[local MCP validation](mcp-testing.md).
