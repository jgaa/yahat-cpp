
add_executable(apiserver apiserver.cpp)

target_link_libraries(apiserver
    yahat
    ${Boost_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${ZLIB_LIBRARIES}
    stdc++fs
    ${CMAKE_THREAD_LIBS_INIT}
)

set_target_properties(apiserver
    PROPERTIES
    CXX_STANDARD 17
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
