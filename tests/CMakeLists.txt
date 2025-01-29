cmake_minimum_required(VERSION 3.14)
project(SummaryTests)

# Enable testing framework
enable_testing()

# Find GoogleTest
find_package(GTest REQUIRED)

# Add test executable
add_executable(summary_tests summary_tests.cpp)

# Link GoogleTest libraries
target_link_libraries(summary_tests PRIVATE GTest::GTest GTest::Main yahat)

# Include directories
include_directories(${GTEST_INCLUDE_DIRS})

# Add the test to CMake's test suite
add_test(NAME SummaryTest COMMAND summary_tests)


# Add test executable
add_executable(histogram_tests histogram_tests.cpp)

# Link GoogleTest libraries
target_link_libraries(histogram_tests PRIVATE GTest::GTest GTest::Main yahat)

# Include directories
include_directories(${GTEST_INCLUDE_DIRS})

# Add the test to CMake's test suite
add_test(NAME HistogramTest COMMAND histogram_tests)
