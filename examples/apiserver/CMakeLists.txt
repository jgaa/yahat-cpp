
project(apiserver VERSION ${YAHAT_VERSION} LANGUAGES CXX)

add_executable(${PROJECT_NAME} apiserver.cpp)

target_link_libraries(${PROJECT_NAME}
    yahat
    ${Boost_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${ZLIB_LIBRARIES}
    stdc++fs
    ${CMAKE_THREAD_LIBS_INIT}
)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
