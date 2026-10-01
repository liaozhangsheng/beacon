# Patch build-tree copies; keep submodule checkouts untouched.
function(beacon_patch source output patch)
    configure_file("${PROJECT_SOURCE_DIR}/${source}" "${CMAKE_CURRENT_BINARY_DIR}/thirdparty/${output}" COPYONLY)
    execute_process(
        COMMAND git apply --unidiff-zero "${PROJECT_SOURCE_DIR}/cmake/patches/${patch}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/thirdparty"
        RESULT_VARIABLE result
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot apply ${patch}: ${error}")
    endif()
endfunction()
