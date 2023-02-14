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

target_include_directories(${PROJECT_NAME}
    PUBLIC
      $<BUILD_INTERFACE:${Boost_INCLUDE_DIR}>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    INTERFACE
      $<INSTALL_INTERFACE:include>
)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)

# Tell CMake how to install the binary library, and what to EXPORT
install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    )  # This is for Windows

# Tell CMake how to deal with the public API headers
install(DIRECTORY "${CMAKE_SOURCE_DIR}/include/yahat" # source directory
        DESTINATION "include" # target directory
        )


set_target_properties(${PROJECT_NAME}
    PROPERTIES
    CXX_STANDARD 17
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    PUBLIC_HEADER ${CMAKE_CURRENT_SOURCE_DIR}>/include/yahat
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

# Generate and export a configuration file to make the project easily
# importable in other CMake projects
install(EXPORT ${PROJECT_NAME}
  FILE ${PROJECT_NAME}.cmake
  DESTINATION lib/cmake/${PROJECT_NAME}
)

include(CMakePackageConfigHelpers)
# generate the config file that includes the exports
configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.cmake"
  INSTALL_DESTINATION "lib/cmake/${PROJECT_NAME}"
  NO_SET_AND_CHECK_MACRO
  NO_CHECK_REQUIRED_COMPONENTS_MACRO
  )

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.cmake"
  VERSION "${yahat_VERSION_MAJOR}.${yahat_VERSION_MINOR}"
  COMPATIBILITY AnyNewerVersion
)

#export(EXPORT ${PROJECT_NAME}
#  FILE "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.cmake"
#)
