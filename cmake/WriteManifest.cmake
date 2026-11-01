cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED DIST_DIR OR "${DIST_DIR}" STREQUAL "")
    message(FATAL_ERROR "WriteManifest.cmake requires -DDIST_DIR=<directory>")
endif()
if(NOT IS_DIRECTORY "${DIST_DIR}")
    message(FATAL_ERROR
        "WriteManifest.cmake: staged directory does not exist: ${DIST_DIR}")
endif()

set(MANIFEST_NAME "manifest.sha256")

file(GLOB_RECURSE staged_files
    LIST_DIRECTORIES false
    RELATIVE "${DIST_DIR}"
    "${DIST_DIR}/*")

set(normalized_files)
foreach(path IN LISTS staged_files)
    string(REPLACE "\\" "/" path "${path}")
    if(path STREQUAL "${MANIFEST_NAME}")
        continue()
    endif()
    list(APPEND normalized_files "${path}")
endforeach()

if(NOT normalized_files)
    message(FATAL_ERROR
        "WriteManifest.cmake: nothing staged below ${DIST_DIR}")
endif()

list(SORT normalized_files)
list(REMOVE_DUPLICATES normalized_files)

set(manifest_content "")
foreach(path IN LISTS normalized_files)
    file(SHA256 "${DIST_DIR}/${path}" file_sha256)
    string(TOLOWER "${file_sha256}" file_sha256)
    string(APPEND manifest_content "${file_sha256}  ${path}\n")
endforeach()

# The staging file must not be named "<output>.tmp": configure_file writes that name itself and the manifest comes out empty.
set(manifest_staging "${DIST_DIR}/${MANIFEST_NAME}.stage")
file(REMOVE "${manifest_staging}")
file(WRITE "${manifest_staging}" "${manifest_content}")
configure_file("${manifest_staging}" "${DIST_DIR}/${MANIFEST_NAME}"
    @ONLY NEWLINE_STYLE LF)
file(REMOVE "${manifest_staging}")

list(LENGTH normalized_files manifest_entries)
message(STATUS
    "Wrote ${DIST_DIR}/${MANIFEST_NAME} (${manifest_entries} entries)")
