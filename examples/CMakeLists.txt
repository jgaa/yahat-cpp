
add_subdirectory(apiserver)
#add_subdirectory(cmake_import)
add_subdirectory(chatserver)

if (YAHAT_ENABLE_MCP)
    add_subdirectory(mcpserver)
endif()
