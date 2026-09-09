cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Auto-detect vcpkg via VCPKG_ROOT when no toolchain file is given explicitly.
# Must happen before the first project() call.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _vcpkg_root)
    if(EXISTS "${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "vcpkg toolchain file (auto-detected from VCPKG_ROOT)")
        message(STATUS "vcpkg toolchain auto-detected: ${CMAKE_TOOLCHAIN_FILE}")
    else()
        message(WARNING "VCPKG_ROOT is set but vcpkg.cmake not found under it: ${_vcpkg_root}")
    endif()
    unset(_vcpkg_root)
endif()

project(flexedge_service VERSION 0.3.39 LANGUAGES CXX)

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(FLEXEDGE_BUILD_WEB "Build the frontend as part of the service build" ON)
set(FLEXEDGE_XDB_V4_FILE "" CACHE FILEPATH
    "Optional IPv4 ip2region XDB file to install as geo/ip2region_v4.xdb")
set(FLEXEDGE_XDB_V6_FILE "" CACHE FILEPATH
    "Optional IPv6 ip2region XDB file to install as geo/ip2region_v6.xdb")

# Ruvia exposes three targets (core/http/web); we consume ruvia::web.
set(RUVIA_BUILD_CORE ON CACHE BOOL "" FORCE)
set(RUVIA_BUILD_HTTP ON CACHE BOOL "" FORCE)
set(RUVIA_BUILD_WEB ON CACHE BOOL "" FORCE)
set(RUVIA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RUVIA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RUVIA_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_MARIADB OFF CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_POSTGRESQL ON CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_JWT OFF CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_REDIS ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    ruvia
    GIT_REPOSITORY https://github.com/hyird/Ruvia.git
    GIT_TAG d7fa8d6dd14be4de204172de9bca0b76f7a3643f # main, 2026-09-09
)
FetchContent_MakeAvailable(ruvia)
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)
find_package(Protobuf CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(FLEXEDGE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${FLEXEDGE_GENERATED_DIR}")

set(FLEXEDGE_NODE_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/node/proto/edge_control.proto")
add_library(flexedge_node_protocol STATIC "${FLEXEDGE_NODE_PROTO}")
protobuf_generate(
    TARGET flexedge_node_protocol
    LANGUAGE cpp
    PROTOC_OUT_DIR "${FLEXEDGE_GENERATED_DIR}"
)
target_include_directories(flexedge_node_protocol PUBLIC "${FLEXEDGE_GENERATED_DIR}")
target_link_libraries(flexedge_node_protocol PUBLIC protobuf::libprotobuf)

add_library(flexedge_route_policy INTERFACE)
target_include_directories(flexedge_route_policy INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flexedge_route_policy INTERFACE re2::re2)

add_library(flexedge_node_version INTERFACE)
target_compile_definitions(flexedge_node_version INTERFACE
    FLEXEDGE_NODE_VERSION="${PROJECT_VERSION}"
)

set(FLEXEDGE_LOG_INGEST_PROTO
    "${CMAKE_CURRENT_SOURCE_DIR}/service/features/log_ingest/log_envelope.proto")
add_library(flexedge_log_ingest_protocol STATIC "${FLEXEDGE_LOG_INGEST_PROTO}")
protobuf_generate(
    TARGET flexedge_log_ingest_protocol
    LANGUAGE cpp
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PROTOC_OUT_DIR "${FLEXEDGE_GENERATED_DIR}"
)
target_include_directories(flexedge_log_ingest_protocol
    PUBLIC "${FLEXEDGE_GENERATED_DIR}")
target_link_libraries(flexedge_log_ingest_protocol
    PUBLIC flexedge_node_protocol protobuf::libprotobuf)

if(FLEXEDGE_BUILD_WEB)
    find_program(BUN_EXECUTABLE NAMES bun bun.exe)
    if(NOT BUN_EXECUTABLE)
        message(FATAL_ERROR "Bun is required to build the frontend")
    endif()

    set(FLEXEDGE_WEB_OUTPUT_DIR "${CMAKE_BINARY_DIR}/web")
    message(STATUS "Using Bun: ${BUN_EXECUTABLE}")

    add_custom_target(flexedge_web ALL
        COMMAND "${CMAKE_COMMAND}" -E env
            NO_COLOR=1
            FORCE_COLOR=0
            "${BUN_EXECUTABLE}" run build --
                --clearScreen false
                --logLevel warn
                --outDir "${FLEXEDGE_WEB_OUTPUT_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        BYPRODUCTS "${FLEXEDGE_WEB_OUTPUT_DIR}/index.html"
        COMMENT "Building frontend"
        VERBATIM
    )
endif()

# Domain controllers are currently header-only. Feature .cpp files must be listed explicitly here.
add_executable(flexedge_service
    service/server.cpp
    service/features/background/worker_pool.cpp
)

target_include_directories(flexedge_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(flexedge_service
    PRIVATE
        flexedge_node_protocol
        flexedge_log_ingest_protocol
        flexedge_route_policy
        ruvia::web
        asio::asio
        OpenSSL::Crypto
        ZLIB::ZLIB
)

add_executable(xdb_lookup tools/xdb_lookup.cpp)
target_include_directories(xdb_lookup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xdb_lookup PRIVATE asio::asio)
if(MSVC)
    target_compile_options(xdb_lookup PRIVATE /W4 /permissive- /utf-8 /wd4819)
else()
    target_compile_options(xdb_lookup PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(flexedge_node
    node/main.cpp
)
target_include_directories(flexedge_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(flexedge_node PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
target_link_libraries(flexedge_node
    PRIVATE flexedge_node_protocol flexedge_node_version flexedge_route_policy ruvia::web OpenSSL::SSL OpenSSL::Crypto
)
set(FLEXEDGE_NODE_RELEASE_MANIFEST "${CMAKE_BINARY_DIR}/node-release.manifest")
add_custom_command(TARGET flexedge_node POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=$<TARGET_FILE:flexedge_node>"
        "-DOUTPUT=${FLEXEDGE_NODE_RELEASE_MANIFEST}"
        "-DVERSION=${PROJECT_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_node_release_manifest.cmake"
    VERBATIM
)

if(MSVC)
    target_compile_options(flexedge_node PRIVATE /W4 /bigobj /permissive- /utf-8 /wd4819)
else()
    target_compile_options(flexedge_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(flexedge_service PRIVATE crypt32)
endif()

if(FLEXEDGE_BUILD_WEB)
    add_dependencies(flexedge_service flexedge_web)
endif()

if(MSVC)
    target_compile_options(flexedge_service PRIVATE /W4 /bigobj /permissive- /Zc:preprocessor /utf-8 /wd4819)
else()
    target_compile_options(flexedge_service PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    # Compile-only boundary check: no Ruvia, database, or protocol dependency
    # may be needed to consume the plain domain data structures.
    add_library(flexedge_domain_models_compile_test OBJECT
        tests/domain_models_compile_test.cpp
    )
    target_include_directories(flexedge_domain_models_compile_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    if(MSVC)
        target_compile_options(flexedge_domain_models_compile_test PRIVATE /utf-8)
    endif()

    # Each test owns its dependencies; compilation policy is shared.
    function(flexedge_add_test name)
        cmake_parse_arguments(TEST "BIG_OBJECT" "" "SOURCES;LIBRARIES;DEFINITIONS" ${ARGN})
        set(target "flexedge_${name}_test")
        add_executable(${target} ${TEST_SOURCES})
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(${target} PRIVATE ${TEST_LIBRARIES})
        target_compile_definitions(${target} PRIVATE ${TEST_DEFINITIONS})
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /wd4819)
            if(TEST_BIG_OBJECT)
                target_compile_options(${target} PRIVATE /bigobj)
            endif()
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME flexedge_${name} COMMAND ${target})
    endfunction()

    flexedge_add_test(dns_reconciliation
        SOURCES tests/dns_reconciliation_test.cpp
    )
    flexedge_add_test(credential_crypto
        SOURCES tests/credential_crypto_test.cpp
        LIBRARIES OpenSSL::Crypto
    )
    flexedge_add_test(duration
        SOURCES tests/duration_test.cpp
    )
    flexedge_add_test(geoip_location
        SOURCES tests/geoip_location_test.cpp
    )
    flexedge_add_test(uuid_collection_validation
        SOURCES tests/uuid_collection_validation_test.cpp
        LIBRARIES ruvia::web
    )
    flexedge_add_test(node_endpoint_validation
        SOURCES tests/node_endpoint_validation_test.cpp
        LIBRARIES ruvia::web
    )
    flexedge_add_test(endpoint_claim
        SOURCES tests/endpoint_claim_test.cpp
        LIBRARIES ruvia::web
    )
    set_tests_properties(flexedge_endpoint_claim PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(website_relations
        SOURCES tests/website_relations_test.cpp
        LIBRARIES ruvia::web
    )
    flexedge_add_test(website_domain_claim
        SOURCES tests/website_domain_claim_test.cpp
    )
    set_tests_properties(flexedge_website_relations PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(website_reference_lock
        SOURCES tests/website_reference_lock_test.cpp
        LIBRARIES ruvia::web
    )
    set_tests_properties(flexedge_website_reference_lock PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(website_aggregate
        SOURCES tests/website_aggregate_test.cpp
        LIBRARIES ruvia::web
    )
    set_tests_properties(flexedge_website_aggregate PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(deployment_source
        SOURCES tests/deployment_source_test.cpp
        LIBRARIES ruvia::web
    )
    set_tests_properties(flexedge_deployment_source PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(delivery_object
        SOURCES tests/delivery_object_test.cpp
        LIBRARIES flexedge_node_protocol ruvia::web OpenSSL::Crypto
    )
    set_tests_properties(flexedge_delivery_object PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(cluster_release_store
        SOURCES tests/cluster_release_store_test.cpp
        LIBRARIES ruvia::web
    )
    set_tests_properties(flexedge_cluster_release_store PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(cluster_release_lock
        SOURCES tests/cluster_release_lock_test.cpp
        LIBRARIES ruvia::web
    )
    set_tests_properties(flexedge_cluster_release_lock PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(node_target_store
        SOURCES tests/node_target_store_test.cpp
        LIBRARIES ruvia::web
    )
    set_tests_properties(flexedge_node_target_store PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(heartbeat_store
        SOURCES tests/heartbeat_store_test.cpp
        LIBRARIES ruvia::web
    )
    set_tests_properties(flexedge_heartbeat_store PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    flexedge_add_test(auth_configuration
        SOURCES tests/auth_configuration_test.cpp
        LIBRARIES ruvia::web
    )
    flexedge_add_test(runtime_secrets
        SOURCES tests/runtime_secrets_test.cpp
        LIBRARIES ruvia::web OpenSSL::Crypto
    )
    flexedge_add_test(secure_file
        SOURCES tests/secure_file_test.cpp
        LIBRARIES Threads::Threads
    )
    flexedge_add_test(loop_shutdown
        SOURCES tests/loop_shutdown_test.cpp
        LIBRARIES ruvia::core
    )
    flexedge_add_test(log_notification
        SOURCES tests/log_notification_test.cpp
        LIBRARIES flexedge_node_protocol ruvia::core OpenSSL::Crypto
    )
    flexedge_add_test(architecture
        SOURCES tests/architecture_test.cpp service/features/background/worker_pool.cpp
        LIBRARIES flexedge_node_protocol flexedge_route_policy ruvia::web asio::asio OpenSSL::Crypto
        DEFINITIONS FLEXEDGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    flexedge_add_test(origin_health
        SOURCES tests/origin_health_test.cpp
        LIBRARIES Threads::Threads
    )
    flexedge_add_test(node_runtime BIG_OBJECT
        SOURCES tests/node_runtime_test.cpp
        LIBRARIES flexedge_node_protocol flexedge_route_policy ruvia::core ruvia::http OpenSSL::SSL OpenSSL::Crypto
    )
    flexedge_add_test(config_activation BIG_OBJECT
        SOURCES tests/config_activation_test.cpp
        LIBRARIES flexedge_node_protocol flexedge_route_policy ruvia::core ruvia::http OpenSSL::SSL OpenSSL::Crypto
    )
    flexedge_add_test(node_release
        SOURCES tests/node_release_test.cpp
        LIBRARIES OpenSSL::Crypto Threads::Threads
    )
    flexedge_add_test(control_stream
        SOURCES tests/control_stream_test.cpp
        LIBRARIES flexedge_node_protocol flexedge_node_version ruvia::web
    )
    flexedge_add_test(agent_protocol
        SOURCES tests/agent_protocol_test.cpp
        LIBRARIES flexedge_node_protocol OpenSSL::Crypto
    )
    flexedge_add_test(log_ingest
        SOURCES tests/log_ingest_test.cpp
        LIBRARIES flexedge_log_ingest_protocol
    )
    flexedge_add_test(log_fanout
        SOURCES tests/log_fanout_test.cpp
        LIBRARIES ruvia::web
    )
    flexedge_add_test(live_resource
        SOURCES tests/live_resource_test.cpp
        LIBRARIES ruvia::web
    )
    flexedge_add_test(logging
        SOURCES tests/logging_test.cpp
        LIBRARIES Threads::Threads ruvia::web
    )
endif()

set(FLEXEDGE_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}")

set_target_properties(flexedge_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${FLEXEDGE_RUNTIME_OUTPUT_DIR}"
    OUTPUT_NAME "server"
)

set_target_properties(flexedge_node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${FLEXEDGE_RUNTIME_OUTPUT_DIR}"
    OUTPUT_NAME "node"
)

if(CMAKE_CONFIGURATION_TYPES)
    foreach(OUTPUT_CONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${OUTPUT_CONFIG}" OUTPUT_CONFIG_UPPER)
        set_target_properties(flexedge_service PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${OUTPUT_CONFIG_UPPER}" "${FLEXEDGE_RUNTIME_OUTPUT_DIR}"
        )
        set_target_properties(flexedge_node PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${OUTPUT_CONFIG_UPPER}" "${FLEXEDGE_RUNTIME_OUTPUT_DIR}"
        )
    endforeach()
endif()

install(TARGETS flexedge_service flexedge_node xdb_lookup
    RUNTIME DESTINATION .
    COMPONENT Server
)
install(FILES "${FLEXEDGE_NODE_RELEASE_MANIFEST}"
    DESTINATION .
    COMPONENT Server
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/route-engine-notices.md"
    DESTINATION licenses
    COMPONENT Server
)
if(FLEXEDGE_BUILD_WEB)
    install(FILES "${FLEXEDGE_WEB_OUTPUT_DIR}/index.html"
        DESTINATION web
        COMPONENT Server
    )
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/service/config/flexedge.service"
    DESTINATION systemd
    COMPONENT Server
)
if(FLEXEDGE_XDB_V4_FILE)
    if(NOT EXISTS "${FLEXEDGE_XDB_V4_FILE}")
        message(FATAL_ERROR "FLEXEDGE_XDB_V4_FILE does not exist: ${FLEXEDGE_XDB_V4_FILE}")
    endif()
    install(FILES "${FLEXEDGE_XDB_V4_FILE}"
        DESTINATION geo
        RENAME ip2region_v4.xdb
        COMPONENT Server
    )
endif()
if(FLEXEDGE_XDB_V6_FILE)
    if(NOT EXISTS "${FLEXEDGE_XDB_V6_FILE}")
        message(FATAL_ERROR "FLEXEDGE_XDB_V6_FILE does not exist: ${FLEXEDGE_XDB_V6_FILE}")
    endif()
    install(FILES "${FLEXEDGE_XDB_V6_FILE}"
        DESTINATION geo
        RENAME ip2region_v6.xdb
        COMPONENT Server
    )
endif()
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/node/install_node.sh"
    DESTINATION .
    RENAME install-node.sh
    COMPONENT Server
)

if(BUILD_TESTING)
    add_test(NAME flexedge_install_layout
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSTAGE_DIR=${CMAKE_BINARY_DIR}/test-install-layout"
            "-DCONFIG=$<CONFIG>"
            "-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            "-DEXPECT_WEB=${FLEXEDGE_BUILD_WEB}"
            "-DEXPECT_XDB_V4=$<BOOL:${FLEXEDGE_XDB_V4_FILE}>"
            "-DEXPECT_XDB_V6=$<BOOL:${FLEXEDGE_XDB_V6_FILE}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_layout_test.cmake"
    )
    if(UNIX)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME flexedge_xdb_lookup
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/xdb_lookup_test.py"
                "$<TARGET_FILE:xdb_lookup>"
        )
        set_tests_properties(flexedge_xdb_lookup PROPERTIES TIMEOUT 30)
        add_test(NAME flexedge_node_shutdown
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/node_shutdown_smoke.py"
                "$<TARGET_FILE:flexedge_node>"
        )
        set_tests_properties(flexedge_node_shutdown PROPERTIES TIMEOUT 60)
        add_test(NAME flexedge_node_installer_validation
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/node_installer_validation_test.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/node/install_node.sh"
        )
        set_tests_properties(flexedge_node_installer_validation PROPERTIES TIMEOUT 30)
        find_program(BASH_EXECUTABLE bash REQUIRED)
        add_test(NAME flexedge_node_installer_syntax
            COMMAND "${BASH_EXECUTABLE}" -n
                "${CMAKE_CURRENT_SOURCE_DIR}/node/install_node.sh"
        )
    endif()
endif()
