project(unittests LANGUAGES CXX)


####### metrics_tests
if (YAHAT_ENABLE_METRICS)

add_executable(metrics_tests
    metrics_tests.cpp
    )

#set_property(TARGET metrics_tests PROPERTY CXX_STANDARD 20)

add_dependencies(metrics_tests
    yahat
    ${DEPENDS_GTEST}
    )

target_include_directories(metrics_tests
    PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}
    $<BUILD_INTERFACE:${NSBLAST_ROOT}/include
    $<BUILD_INTERFACE:${NSBLAST_ROOT}/src/lib
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    )

target_link_libraries(metrics_tests
    ${GTEST_LIBRARIES}
    yahat
)

add_test(NAME metrics_tests COMMAND metrics_tests)

endif()
