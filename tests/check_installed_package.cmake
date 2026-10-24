# Exercise the installed public package from a separate, clean CMake project.
file(REMOVE_RECURSE "${YAHAT_CHECK_DIR}")
file(MAKE_DIRECTORY "${YAHAT_CHECK_DIR}")

function(run_step name)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    file(WRITE "${YAHAT_CHECK_DIR}/${name}.log" "${output}\n${errors}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Installed package ${name} failed (${result}):\n${output}\n${errors}\nLogs: ${YAHAT_CHECK_DIR}")
    endif()
endfunction()

run_step(install "${CMAKE_COMMAND}"
    "-DCMAKE_INSTALL_PREFIX=${YAHAT_CHECK_DIR}/prefix"
    "-DCMAKE_INSTALL_CONFIG_NAME=${YAHAT_CONFIG}"
    -P "${YAHAT_BUILD_DIR}/cmake_install.cmake")
set(toolchain_args)
set(build_config_args)
set(test_config_args)
if(YAHAT_TOOLCHAIN_FILE)
    list(APPEND toolchain_args "-DCMAKE_TOOLCHAIN_FILE=${YAHAT_TOOLCHAIN_FILE}")
endif()
# Single-config generators may have no CMAKE_BUILD_TYPE. Function argument
# expansion drops empty list elements, so '--config ""' becomes a bare flag.
if(NOT "${YAHAT_CONFIG}" STREQUAL "")
    list(APPEND build_config_args --config "${YAHAT_CONFIG}")
    list(APPEND test_config_args -C "${YAHAT_CONFIG}")
endif()
run_step(configure "${CMAKE_COMMAND}"
    -S "${YAHAT_CONSUMER_SOURCE_DIR}" -B "${YAHAT_CHECK_DIR}/consumer-build"
    -G "${YAHAT_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${YAHAT_CHECK_DIR}/prefix"
    "-DCMAKE_CXX_COMPILER=${YAHAT_CXX_COMPILER}"
    "-DCMAKE_BUILD_TYPE=${YAHAT_CONFIG}" ${toolchain_args})
run_step(build "${CMAKE_COMMAND}" --build "${YAHAT_CHECK_DIR}/consumer-build"
    ${build_config_args})
run_step(run "${CMAKE_COMMAND}" -E chdir "${YAHAT_CHECK_DIR}/consumer-build"
    "${CMAKE_CTEST_COMMAND}" ${test_config_args} --output-on-failure)
message(STATUS "Installed package consumer passed; logs: ${YAHAT_CHECK_DIR}")
