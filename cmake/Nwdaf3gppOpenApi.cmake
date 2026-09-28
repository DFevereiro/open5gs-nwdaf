# H1.7 — official 3GPP Rel-18 OpenAPI artifacts.
#
# Materialises the files listed in 3gpp_openapi_manifest.cmake into
# ${NWDAF_3GPP_OPENAPI_DIR}, from the commit pinned in docs/frozen-standards.md.
# Every file is verified against its SHA-256; a mismatch fails the configure
# rather than silently moving the compliance baseline. The files are
# "All rights reserved" (3GPP Organizational Partners), so they are fetched at
# build time and never committed to this repository.

include(${CMAKE_CURRENT_LIST_DIR}/3gpp_openapi_manifest.cmake)

set(NWDAF_3GPP_OPENAPI_COMMIT d05657604fa16603ff5e0b6190220da2cdd2bf60)
set(NWDAF_3GPP_OPENAPI_URL
    "https://forge.3gpp.org/rep/all/5G_APIs/-/raw/${NWDAF_3GPP_OPENAPI_COMMIT}")
set(NWDAF_3GPP_OPENAPI_SOURCE_DIR "" CACHE PATH
    "Offline copy of the pinned 3GPP OpenAPI files (still hash-verified); empty = download")
set(NWDAF_3GPP_OPENAPI_DIR "${CMAKE_BINARY_DIR}/3gpp-openapi")

function(_nwdaf_fetch_3gpp_file name sha)
    set(dst "${NWDAF_3GPP_OPENAPI_DIR}/${name}")
    if(EXISTS "${dst}")
        file(SHA256 "${dst}" have)
        if(have STREQUAL sha)
            return()
        endif()
    endif()

    if(NWDAF_3GPP_OPENAPI_SOURCE_DIR)
        file(COPY_FILE "${NWDAF_3GPP_OPENAPI_SOURCE_DIR}/${name}" "${dst}")
    else()
        foreach(attempt 1 2 3)
            file(DOWNLOAD "${NWDAF_3GPP_OPENAPI_URL}/${name}" "${dst}"
                 STATUS status TLS_VERIFY ON TIMEOUT 60)
            list(GET status 0 code)
            if(code EQUAL 0)
                break()
            endif()
            message(WARNING "3GPP OpenAPI ${name}: download attempt ${attempt} failed (${status})")
        endforeach()
    endif()

    file(SHA256 "${dst}" have)
    if(NOT have STREQUAL sha)
        file(REMOVE "${dst}")
        message(FATAL_ERROR
            "3GPP OpenAPI ${name}: SHA-256 ${have} does not match the pinned ${sha}. "
            "The compliance baseline (docs/frozen-standards.md) must not move silently.")
    endif()
endfunction()

file(MAKE_DIRECTORY "${NWDAF_3GPP_OPENAPI_DIR}")
list(LENGTH NWDAF_3GPP_OPENAPI_FILES _nwdaf_3gpp_count)
foreach(entry IN LISTS NWDAF_3GPP_OPENAPI_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 sha)
    _nwdaf_fetch_3gpp_file("${name}" "${sha}")
endforeach()
message(STATUS "3GPP Rel-18 OpenAPI: ${_nwdaf_3gpp_count} files verified at "
               "${NWDAF_3GPP_OPENAPI_COMMIT} in ${NWDAF_3GPP_OPENAPI_DIR}")
