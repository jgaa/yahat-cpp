cmake_minimum_required(VERSION 3.18)

project(cmake-import LANGUAGES CXX)

# Required why??
include(GNUInstallDirs)

# We allow the user to ovberide our BOOST version requirement.
if(NOT DEFINED USE_BOOST_VERSION)
    set(USE_BOOST_VERSION 1.75)
endif()

# Find the required boost libraries
# We require `program_options` here.
# The others are needed in order to link with the yahat library.
find_package(Boost ${USE_BOOST_VERSION} REQUIRED COMPONENTS
    system
    program_options
    coroutine
    context
    chrono
    )

# Allow us to depend on boost
add_library(boost INTERFACE IMPORTED)

# ???
set_property(TARGET boost PROPERTY
    INTERFACE_INCLUDE_DIRECTORIES ${Boost_INCLUDE_DIR})

# Find packages we need to link with when we use yahat
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads)


# Where we store the cmake spells to import yahat to our project as a library
include(cmake/3rdparty.cmake)

# Normal CMAKE declaration of the binary we require
add_executable(${PROJECT_NAME} main.cpp)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    yahat
    ${Boost_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${ZLIB_LIBRARIES}
    stdc++fs
    ${CMAKE_THREAD_LIBS_INIT}
)

# Requirements for the project, like using C++17 and where
# to save the binaries.
set_target_properties(${PROJECT_NAME}
    PROPERTIES
    CXX_STANDARD 17
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
