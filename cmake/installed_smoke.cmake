if(NOT DEFINED BEACON_BUILD_DIR OR NOT DEFINED BEACON_INSTALL_DIR OR
   NOT DEFINED BEACON_INSTALL_BINDIR)
    message(FATAL_ERROR "installed smoke test requires build, install, and bindir paths")
endif()

file(REMOVE_RECURSE "${BEACON_INSTALL_DIR}")

set(install_command "${CMAKE_COMMAND}" --install "${BEACON_BUILD_DIR}" --prefix "${BEACON_INSTALL_DIR}")
if(DEFINED BEACON_BUILD_CONFIG AND NOT BEACON_BUILD_CONFIG STREQUAL "")
    list(APPEND install_command --config "${BEACON_BUILD_CONFIG}")
endif()
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "install failed (${install_result})\n${install_output}\n${install_error}")
endif()

if(WIN32)
    set(beacon_executable "${BEACON_INSTALL_DIR}/${BEACON_INSTALL_BINDIR}/beacon.exe")
else()
    set(beacon_executable "${BEACON_INSTALL_DIR}/${BEACON_INSTALL_BINDIR}/beacon")
endif()
if(NOT EXISTS "${beacon_executable}")
    message(FATAL_ERROR "installed executable was not found: ${beacon_executable}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=dummy "${beacon_executable}" --smoke-test
    WORKING_DIRECTORY "${BEACON_INSTALL_DIR}/${BEACON_INSTALL_BINDIR}"
    RESULT_VARIABLE smoke_result
    OUTPUT_VARIABLE smoke_output
    ERROR_VARIABLE smoke_error
)
if(NOT smoke_result EQUAL 0)
    message(FATAL_ERROR "installed smoke test failed (${smoke_result})\n${smoke_output}\n${smoke_error}")
endif()
