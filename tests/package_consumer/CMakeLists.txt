cmake_minimum_required(VERSION 3.13)
project(YahatPackageConsumer LANGUAGES CXX)

# No manual dependency discovery or C++ standard: the package must provide them.
find_package(yahat-cpp CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE yahat-cpp::yahat-cpp)
enable_testing()
add_test(NAME ConsumerRuns COMMAND consumer)
