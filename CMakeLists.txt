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

project(flexedge_service VERSION 0.3.33 LANGUAGES CXX)

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(FLEXEDGE_BUILD_WEB "Build the frontend as part of the service build" ON)

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
    GIT_TAG 8a4a02eb5559213fb012c3a2c4c34c9b6535dfe7 # main, 2026-08-31
)
FetchContent_MakeAvailable(ruvia)
find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)
find_package(Protobuf CONFIG REQUIRED)
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
target_compile_definitions(flexedge_service PRIVATE FLEXEDGE_NODE_VERSION="${PROJECT_VERSION}")

target_link_libraries(flexedge_service
    PRIVATE
        flexedge_node_protocol
        flexedge_log_ingest_protocol
        ruvia::web
        OpenSSL::Crypto
        ZLIB::ZLIB
)

add_executable(flexedge_node
    node/main.cpp
)
target_include_directories(flexedge_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(flexedge_node PRIVATE
    FLEXEDGE_NODE_VERSION="${PROJECT_VERSION}"
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
target_link_libraries(flexedge_node
    PRIVATE flexedge_node_protocol ruvia::web OpenSSL::SSL OpenSSL::Crypto
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
    add_executable(flexedge_architecture_test tests/architecture_test.cpp)
    target_include_directories(flexedge_architecture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(flexedge_architecture_test
        PRIVATE FLEXEDGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(flexedge_architecture_test
        PRIVATE flexedge_node_protocol ruvia::web OpenSSL::Crypto
    )
    if(MSVC)
        target_compile_options(flexedge_architecture_test PRIVATE /W4 /permissive- /utf-8 /wd4819)
    else()
        target_compile_options(flexedge_architecture_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME flexedge_architecture COMMAND flexedge_architecture_test)

    add_executable(flexedge_node_runtime_test tests/node_runtime_test.cpp)
    target_include_directories(flexedge_node_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(flexedge_node_runtime_test
        PRIVATE flexedge_node_protocol ruvia::core ruvia::http OpenSSL::SSL OpenSSL::Crypto
    )
    if(MSVC)
        target_compile_options(flexedge_node_runtime_test PRIVATE /W4 /bigobj /permissive- /utf-8 /wd4819)
    else()
        target_compile_options(flexedge_node_runtime_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME flexedge_node_runtime COMMAND flexedge_node_runtime_test)

    add_executable(flexedge_node_release_test tests/node_release_test.cpp)
    target_include_directories(flexedge_node_release_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(flexedge_node_release_test PRIVATE OpenSSL::Crypto)
    if(MSVC)
        target_compile_options(flexedge_node_release_test PRIVATE /W4 /permissive- /utf-8 /wd4819)
    else()
        target_compile_options(flexedge_node_release_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME flexedge_node_release COMMAND flexedge_node_release_test)

    add_executable(flexedge_control_stream_test tests/control_stream_test.cpp)
    target_include_directories(flexedge_control_stream_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(flexedge_control_stream_test
        PRIVATE flexedge_node_protocol ruvia::web
    )
    if(MSVC)
        target_compile_options(flexedge_control_stream_test PRIVATE /W4 /permissive- /utf-8 /wd4819)
    else()
        target_compile_options(flexedge_control_stream_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME flexedge_control_stream COMMAND flexedge_control_stream_test)

    add_executable(flexedge_log_ingest_test tests/log_ingest_test.cpp)
    target_include_directories(flexedge_log_ingest_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(flexedge_log_ingest_test
        PRIVATE flexedge_log_ingest_protocol ruvia::web
    )
    if(MSVC)
        target_compile_options(flexedge_log_ingest_test PRIVATE /W4 /permissive- /utf-8 /wd4819)
    else()
        target_compile_options(flexedge_log_ingest_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME flexedge_log_ingest COMMAND flexedge_log_ingest_test)

    add_executable(flexedge_log_fanout_test tests/log_fanout_test.cpp)
    target_include_directories(flexedge_log_fanout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(flexedge_log_fanout_test PRIVATE ruvia::web)
    if(MSVC)
        target_compile_options(flexedge_log_fanout_test PRIVATE /W4 /permissive- /utf-8 /wd4819)
    else()
        target_compile_options(flexedge_log_fanout_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME flexedge_log_fanout COMMAND flexedge_log_fanout_test)

    add_executable(flexedge_logging_test tests/logging_test.cpp)
    target_include_directories(flexedge_logging_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(flexedge_logging_test PRIVATE Threads::Threads ruvia::web)
    if(MSVC)
        target_compile_options(flexedge_logging_test PRIVATE /W4 /permissive- /utf-8 /wd4819)
    else()
        target_compile_options(flexedge_logging_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME flexedge_logging COMMAND flexedge_logging_test)
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

install(TARGETS flexedge_service flexedge_node
    RUNTIME DESTINATION .
    COMPONENT Server
)
install(FILES "${FLEXEDGE_NODE_RELEASE_MANIFEST}"
    DESTINATION .
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
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_layout_test.cmake"
    )
    if(UNIX)
        find_program(BASH_EXECUTABLE bash REQUIRED)
        add_test(NAME flexedge_node_installer_syntax
            COMMAND "${BASH_EXECUTABLE}" -n
                "${CMAKE_CURRENT_SOURCE_DIR}/node/install_node.sh"
        )
    endif()
endif()
