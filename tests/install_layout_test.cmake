foreach(REQUIRED_VARIABLE BUILD_DIR STAGE_DIR)
    if(NOT DEFINED ${REQUIRED_VARIABLE})
        message(FATAL_ERROR "missing ${REQUIRED_VARIABLE}")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE_DIR}")
set(INSTALL_COMMAND
    "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
    --component Server
    --prefix "${STAGE_DIR}"
)
if(CONFIG)
    list(APPEND INSTALL_COMMAND --config "${CONFIG}")
endif()

execute_process(
    COMMAND ${INSTALL_COMMAND}
    RESULT_VARIABLE INSTALL_RESULT
    OUTPUT_VARIABLE INSTALL_OUTPUT
    ERROR_VARIABLE INSTALL_ERROR
)
if(NOT INSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "Server component install failed:\n${INSTALL_OUTPUT}${INSTALL_ERROR}")
endif()

set(EXPECTED_FILES
    "install-node.sh"
    "node${EXECUTABLE_SUFFIX}"
    "node-release.manifest"
    "server${EXECUTABLE_SUFFIX}"
    "systemd/flexedge.service"
)
list(SORT EXPECTED_FILES)

file(GLOB_RECURSE ACTUAL_FILES
    LIST_DIRECTORIES false
    RELATIVE "${STAGE_DIR}"
    "${STAGE_DIR}/*"
)
list(SORT ACTUAL_FILES)
if(NOT ACTUAL_FILES STREQUAL EXPECTED_FILES)
    message(FATAL_ERROR
        "Server component layout mismatch\nexpected: ${EXPECTED_FILES}\nactual: ${ACTUAL_FILES}"
    )
endif()
