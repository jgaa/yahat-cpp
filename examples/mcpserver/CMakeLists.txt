add_executable(yahat-mcp-example main.cpp)
target_link_libraries(yahat-mcp-example PRIVATE yahat)
set_target_properties(yahat-mcp-example PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
