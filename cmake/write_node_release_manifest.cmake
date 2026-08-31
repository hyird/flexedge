foreach(REQUIRED_VARIABLE INPUT OUTPUT VERSION)
    if(NOT DEFINED ${REQUIRED_VARIABLE} OR "${${REQUIRED_VARIABLE}}" STREQUAL "")
        message(FATAL_ERROR "missing ${REQUIRED_VARIABLE}")
    endif()
endforeach()

if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "node release binary is unavailable")
endif()

file(SHA256 "${INPUT}" DIGEST)
set(TEMPORARY_OUTPUT "${OUTPUT}.tmp")
file(WRITE "${TEMPORARY_OUTPUT}"
    "flexedge-node-release-v1\nversion=${VERSION}\nsha256=${DIGEST}\n"
)
file(RENAME "${TEMPORARY_OUTPUT}" "${OUTPUT}")
