cmake_minimum_required(VERSION 3.14)
project(UnitTests)

# Enable testing framework
enable_testing()

# Find GoogleTest
find_package(GTest REQUIRED)

# Macro to simplify test addition
macro(AddTest CLASS_NAME BASE_NAME)
    add_executable(${BASE_NAME}_tests ${BASE_NAME}.cpp)
    target_link_libraries(${BASE_NAME}_tests PRIVATE GTest::GTest GTest::Main yahat)
    include_directories(${GTEST_INCLUDE_DIRS})
    add_test(NAME ${CLASS_NAME}Test COMMAND ${BASE_NAME}_tests)
endmacro()

AddTest(Http http_tests)
add_test(NAME InstalledPackageTest COMMAND "${CMAKE_COMMAND}"
    "-DYAHAT_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DYAHAT_CHECK_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-package-check"
    "-DYAHAT_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_consumer"
    "-DYAHAT_CONFIG=$<CONFIG>"
    "-DYAHAT_GENERATOR=${CMAKE_GENERATOR}"
    "-DYAHAT_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DYAHAT_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_installed_package.cmake")
if (YAHAT_ENABLE_JSON_RPC)
    AddTest(JsonRpc jsonrpc_tests)
endif()

if (YAHAT_ENABLE_MCP)
    AddTest(Mcp mcp_tests)
    configure_file(mcp_conformance/package.json.in
        "${CMAKE_BINARY_DIR}/mcp-conformance/package.json" @ONLY)
    configure_file(mcp_conformance/package-lock.json
        "${CMAKE_BINARY_DIR}/mcp-conformance/package-lock.json" COPYONLY)
    add_executable(yahat-mcp-test-server mcp_test_server.cpp)
    target_link_libraries(yahat-mcp-test-server PRIVATE yahat)
    set_target_properties(yahat-mcp-test-server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if (YAHAT_ENABLE_METRICS)
    AddTest(Summary summary_tests)
    AddTest(Histogram histogram_tests)
    AddTest(Stateset stateset_tests)
    AddTest(Counter counter_tests)
    AddTest(Gauge gauge_tests)
    AddTest(Info info_tests)
endif()
