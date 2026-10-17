foreach(REQUIRED_VARIABLE BUILD_DIR STAGE_DIR EXPECT_WEB)
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
    "xdb_lookup${EXECUTABLE_SUFFIX}"
)
if(EXPECT_WEB)
    if(NOT EXISTS "${BUILD_DIR}/web/index.html")
        message(FATAL_ERROR "frontend entry is missing")
    endif()
    foreach(EXTENSION js css woff2)
        file(GLOB ASSETS "${BUILD_DIR}/web/assets/*.${EXTENSION}")
        if(NOT ASSETS)
            message(FATAL_ERROR "frontend ${EXTENSION} assets are missing")
        endif()
    endforeach()
    file(GLOB_RECURSE WEB_FILES
        LIST_DIRECTORIES false
        RELATIVE "${BUILD_DIR}"
        "${BUILD_DIR}/web/*"
    )
    foreach(WEB_FILE IN LISTS WEB_FILES)
        string(TOLOWER "${WEB_FILE}" LOWER_WEB_FILE)
        if(NOT WEB_FILE STREQUAL LOWER_WEB_FILE)
            message(FATAL_ERROR "frontend filename must be lowercase: ${WEB_FILE}")
        endif()
        file(SHA256 "${BUILD_DIR}/${WEB_FILE}" SOURCE_HASH)
        file(SHA256 "${STAGE_DIR}/${WEB_FILE}" INSTALLED_HASH)
        if(NOT SOURCE_HASH STREQUAL INSTALLED_HASH)
            message(FATAL_ERROR "installed frontend asset differs: ${WEB_FILE}")
        endif()
    endforeach()
    list(APPEND EXPECTED_FILES ${WEB_FILES})
endif()
if(EXPECT_XDB_V4)
    list(APPEND EXPECTED_FILES "geo/ip2region_v4.xdb")
endif()
if(EXPECT_XDB_V6)
    list(APPEND EXPECTED_FILES "geo/ip2region_v6.xdb")
endif()
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
