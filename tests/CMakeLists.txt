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

if (YAHAT_ENABLE_METRICS)
    AddTest(Summary summary_tests)
    AddTest(Histogram histogram_tests)
    AddTest(Stateset stateset_tests)
    AddTest(Counter counter_tests)
    AddTest(Gauge gauge_tests)
    AddTest(Info info_tests)
endif()
