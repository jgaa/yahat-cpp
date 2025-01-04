
project(yahatchat VERSION ${YAHAT_VERSION} LANGUAGES CXX)

add_executable(${PROJECT_NAME} main.cpp
    ChatMgr.h ChatMgr.cpp
    ChatApi.h ChatApi.cpp
    WebApp.h WebApp.cpp)

target_link_libraries(${PROJECT_NAME}
    yahat
    # ${Boost_LIBRARIES}
    # ${OPENSSL_LIBRARIES}
    # ${ZLIB_LIBRARIES}
    # ${CMAKE_THREAD_LIBS_INIT}
)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
