cmake_minimum_required(VERSION 3.18)

#option(YAHAT_WITH_TESTS "Compile tests" OFF)
option(YAHAT_WITH_EXAMPLES "Compile examples" ON)
option(YAHAT_WITH_LOGFAULT "Use logfault" ON)

if (NOT DEFINED _VERSION)
    set(YAHAT_VERSION 0.0.1)
endif()
add_definitions(-DYAHAT_VERSION=\"${YAHAT_VERSION}\")

project(yahat VERSION ${YAHAT_VERSION} LANGUAGES CXX)
set(YAHAT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH};${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(YAHAT_SILENCE_BOOST_DEPRICATED_MESSAGES "Allows us to avoid warnings (mostly) because asio is not updated between boost versions" ON)
if(YAHAT_SILENCE_BOOST_DEPRICATED_MESSAGES)
    add_definitions(-DBOOST_ALLOW_DEPRECATED_HEADERS=1)
endif()

include(GNUInstallDirs)

if(NOT DEFINED USE_BOOST_VERSION)
    set(USE_BOOST_VERSION 1.75)
endif()

find_package(Boost ${USE_BOOST_VERSION} REQUIRED COMPONENTS
    system
    program_options
    coroutine
    context
    regex
    chrono
    )

add_library(boost INTERFACE IMPORTED)
set_property(TARGET boost PROPERTY
    INTERFACE_INCLUDE_DIRECTORIES ${Boost_INCLUDE_DIR})

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads)

message(STATUS "Using ${CMAKE_CXX_COMPILER}")

include(cmake/3rdparty.cmake)
    
add_library(${PROJECT_NAME}
    include/yahat/logging.h
    src/HttpServer.cpp
    src/logging.cpp
    include/yahat/HttpServer.h
    )

if (YAHAT_WITH_LOGFAULT)
   add_dependencies(${PROJECT_NAME} logfault)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${Boost_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/include
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)


install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    )  # This is for Windows

set_target_properties(${PROJECT_NAME}
    PROPERTIES
    CXX_STANDARD 17
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_compile_definitions(${PROJECT_NAME} PUBLIC -DBOOST_COROUTINES_NO_DEPRECATION_WARNING=1)
target_compile_definitions(${PROJECT_NAME} PUBLIC -DBOOST_COROUTINE_NO_DEPRECATION_WARNING=1)

#if (YAHAT_WITH_TESTS)
#    enable_testing()
#    add_subdirectory(tests)
#endif()

if (YAHAT_WITH_EXAMPLES)
    add_subdirectory(examples)
endif()
