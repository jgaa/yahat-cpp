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
if(YAHAT_TOOLCHAIN_FILE)
    list(APPEND toolchain_args "-DCMAKE_TOOLCHAIN_FILE=${YAHAT_TOOLCHAIN_FILE}")
endif()
run_step(configure "${CMAKE_COMMAND}"
    -S "${YAHAT_CONSUMER_SOURCE_DIR}" -B "${YAHAT_CHECK_DIR}/consumer-build"
    -G "${YAHAT_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${YAHAT_CHECK_DIR}/prefix"
    "-DCMAKE_CXX_COMPILER=${YAHAT_CXX_COMPILER}"
    "-DCMAKE_BUILD_TYPE=${YAHAT_CONFIG}" ${toolchain_args})
run_step(build "${CMAKE_COMMAND}" --build "${YAHAT_CHECK_DIR}/consumer-build"
    --config "${YAHAT_CONFIG}")
run_step(run "${CMAKE_COMMAND}" -E chdir "${YAHAT_CHECK_DIR}/consumer-build"
    "${CMAKE_CTEST_COMMAND}" -C "${YAHAT_CONFIG}" --output-on-failure)
message(STATUS "Installed package consumer passed; logs: ${YAHAT_CHECK_DIR}")
