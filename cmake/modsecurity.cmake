# Fixed, deliberately small libmodsecurity build. Optional persistent collections,
# Lua, remote rules, GeoIP and file inspection integrations are not enabled.
FetchContent_Declare(modsecurity
    GIT_REPOSITORY https://github.com/owasp-modsecurity/ModSecurity.git
    GIT_TAG 1925753989ccce977cdaae417b55c9726c7cf02c
    GIT_SUBMODULES others/libinjection others/mbedtls
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_Declare(crs
    GIT_REPOSITORY https://github.com/coreruleset/coreruleset.git
    GIT_TAG aabf675fcfc5e489b424b844e6c9f1b39802df69
    GIT_SUBMODULES "")
FetchContent_MakeAvailable(modsecurity crs)

find_package(pcre2 CONFIG REQUIRED)
find_package(LibXml2 REQUIRED)
find_path(FLEXEDGE_YAJL_INCLUDE yajl/yajl_parse.h REQUIRED)
find_library(FLEXEDGE_YAJL_LIBRARY NAMES yajl_s yajl REQUIRED)
if(WIN32)
    find_package(Poco CONFIG REQUIRED COMPONENTS Foundation)
endif()

set(PACKAGE_NAME modsecurity)
set(PACKAGE_VERSION 3.0.17)
set(PACKAGE_STRING "modsecurity 3.0.17")
set(PACKAGE_TARNAME modsecurity)
set(HAVE_YAJL 1)
set(HAVE_LIBXML2 1)
include(CheckIncludeFile)
include(CheckIncludeFiles)
include("${modsecurity_SOURCE_DIR}/build/win32/ConfigureChecks.cmake")
file(MAKE_DIRECTORY "${modsecurity_BINARY_DIR}/src")
configure_file("${modsecurity_SOURCE_DIR}/build/win32/config.h.cmake"
    "${modsecurity_BINARY_DIR}/src/config.h")

set(_crypto "${modsecurity_SOURCE_DIR}/others/mbedtls/tf-psa-crypto")
set(_mbed "${modsecurity_SOURCE_DIR}/others/mbedtls")
add_library(flexedge_modsecurity_crypto STATIC
    "${_crypto}/utilities/base64.c" "${_crypto}/utilities/constant_time.c"
    "${_crypto}/platform/platform_util.c" "${_crypto}/extras/md.c"
    "${_crypto}/drivers/builtin/src/md5.c" "${_crypto}/drivers/builtin/src/sha1.c"
    "${_crypto}/drivers/builtin/src/sha256.c" "${_crypto}/drivers/builtin/src/sha512.c"
    "${_crypto}/drivers/builtin/src/sha3.c" "${_crypto}/drivers/builtin/src/ripemd160.c"
    "${_crypto}/drivers/builtin/src/psa_util_internal.c")
target_include_directories(flexedge_modsecurity_crypto PRIVATE
    "${_mbed}/include" "${_crypto}/include" "${_crypto}/core" "${_crypto}/extras"
    "${_crypto}/library" "${_crypto}/utilities" "${_crypto}/drivers/builtin/include"
    "${_crypto}/drivers/builtin/src")
target_compile_definitions(flexedge_modsecurity_crypto PRIVATE MBEDTLS_CONFIG_FILE="mbedtls/mbedtls_config.h")
add_library(flexedge_libinjection STATIC
    "${modsecurity_SOURCE_DIR}/others/libinjection/src/libinjection_sqli.c"
    "${modsecurity_SOURCE_DIR}/others/libinjection/src/libinjection_xss.c"
    "${modsecurity_SOURCE_DIR}/others/libinjection/src/libinjection_html5.c")
target_compile_definitions(flexedge_libinjection PRIVATE LIBINJECTION_VERSION="4.0.0")
file(GLOB_RECURSE _modsecurity_sources CONFIGURE_DEPENDS "${modsecurity_SOURCE_DIR}/src/*.cc")
add_library(flexedge_modsecurity STATIC ${_modsecurity_sources})
target_include_directories(flexedge_modsecurity SYSTEM PUBLIC "${modsecurity_SOURCE_DIR}/headers")
target_include_directories(flexedge_modsecurity PRIVATE
    "${modsecurity_BINARY_DIR}" "${modsecurity_SOURCE_DIR}" "${modsecurity_SOURCE_DIR}/others"
    "${_mbed}/include" "${_crypto}/include" "${_crypto}/drivers/builtin/include" "${FLEXEDGE_YAJL_INCLUDE}")
target_compile_definitions(flexedge_modsecurity PRIVATE WITH_PCRE2 WITH_YAJL WITH_LIBXML2
    MBEDTLS_CONFIG_FILE="mbedtls/mbedtls_config.h")
target_link_libraries(flexedge_modsecurity PRIVATE PCRE2::8BIT LibXml2::LibXml2
    flexedge_libinjection flexedge_modsecurity_crypto "${FLEXEDGE_YAJL_LIBRARY}")
if(WIN32)
    target_compile_definitions(flexedge_modsecurity PRIVATE WIN32 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(flexedge_modsecurity PRIVATE Poco::Foundation iphlpapi)
    # Poco exports UNICODE, but upstream libmodsecurity uses narrow Win32 APIs.
    target_compile_options(flexedge_modsecurity PRIVATE /Zc:preprocessor /permissive- /UUNICODE /U_UNICODE)
endif()
set_target_properties(flexedge_modsecurity flexedge_modsecurity_crypto flexedge_libinjection
    PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# Embed exact asset bytes: installation and self-update transport only a node binary.
# Small separate literals also avoid MSVC's 64KiB concatenated-string limit.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_crs_catalog.py"
    --source "${crs_SOURCE_DIR}/rules" --output "${CMAKE_BINARY_DIR}/generated/security_catalog.h"
    COMMAND_ERROR_IS_FATAL ANY)
set(_asset_header "${CMAKE_BINARY_DIR}/generated/security_assets.h.in")
file(WRITE "${_asset_header}" "#pragma once\n#include <span>\n#include <string_view>\nnamespace flexedge::node {\nstruct security_asset { std::string_view name_, digest_; std::span<const std::string_view> chunks_; };\n")
file(GLOB _assets "${crs_SOURCE_DIR}/rules/*.conf" "${crs_SOURCE_DIR}/rules/*.data")
list(APPEND _assets "${modsecurity_SOURCE_DIR}/unicode.mapping" "${modsecurity_SOURCE_DIR}/LICENSE" "${crs_SOURCE_DIR}/LICENSE")
set(_asset_index 0)
set(_asset_table "")
set(_asset_digests "")
foreach(_asset IN LISTS _assets)
    file(SHA256 "${_asset}" _digest)
    get_filename_component(_name "${_asset}" NAME)
    if(_asset STREQUAL "${modsecurity_SOURCE_DIR}/LICENSE")
        set(_name "LICENSE.ModSecurity")
    elseif(_asset STREQUAL "${crs_SOURCE_DIR}/LICENSE")
        set(_name "LICENSE.CRS")
    endif()
    file(READ "${_asset}" _hex HEX)
    string(LENGTH "${_hex}" _length)
    file(APPEND "${_asset_header}" "inline constexpr std::string_view security_chunks_${_asset_index}[] = {\n")
    set(_offset 0)
    while(_offset LESS _length)
        math(EXPR _remaining "${_length}-${_offset}")
        if(_remaining GREATER 8192)
            set(_remaining 8192)
        endif()
        string(SUBSTRING "${_hex}" ${_offset} ${_remaining} _chunk)
        string(REGEX REPLACE "(..)" "\\\\x\\1" _escaped "${_chunk}")
        math(EXPR _bytes "${_remaining}/2")
        file(APPEND "${_asset_header}" "std::string_view{\"${_escaped}\",${_bytes}},\n")
        math(EXPR _offset "${_offset}+${_remaining}")
    endwhile()
    file(APPEND "${_asset_header}" "};\n")
    string(APPEND _asset_table "{\"${_name}\",\"${_digest}\",security_chunks_${_asset_index}},\n")
    string(APPEND _asset_digests "${_name}:${_digest}\n")
    math(EXPR _asset_index "${_asset_index}+1")
endforeach()
string(SHA256 _asset_version "${_asset_digests}")
file(APPEND "${_asset_header}" "inline constexpr security_asset security_assets[] = {\n${_asset_table}};\ninline constexpr std::string_view security_assets_version = \"${_asset_version}\";\n}\n")
configure_file("${_asset_header}" "${CMAKE_BINARY_DIR}/generated/security_assets.h" COPYONLY)
add_library(flexedge_security STATIC node/data/managed_security.cpp node/proto/security_validation.cpp)
target_include_directories(flexedge_security PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(flexedge_security PRIVATE flexedge_modsecurity OpenSSL::Crypto flexedge_node_protocol re2::re2 asio::asio)
if(MSVC)
    target_compile_options(flexedge_security PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(flexedge_security PRIVATE _WIN32_WINNT=0x0A00)
else()
    target_compile_options(flexedge_security PRIVATE -Wall -Wextra -Wpedantic)
endif()
