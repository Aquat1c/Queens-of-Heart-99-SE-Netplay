cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED DIST_CONFIG OR NOT DIST_CONFIG STREQUAL "Release")
    message(FATAL_ERROR
        "qoh99_dist is Release-only; invoke the build with --config Release")
endif()

if(DEFINED OPENING_AVI_SOURCE AND NOT "${OPENING_AVI_SOURCE}" STREQUAL ""
   AND (NOT EXISTS "${OPENING_AVI_SOURCE}" OR IS_DIRECTORY "${OPENING_AVI_SOURCE}"))
    message(FATAL_ERROR
        "QOH99_OPENING_AVI names a missing file: '${OPENING_AVI_SOURCE}'. "
        "Produce one with tools/convert_opening_movie.py or leave it empty.")
endif()
set(SHIP_OPENING_AVI OFF)
if(DEFINED OPENING_AVI_SOURCE AND NOT "${OPENING_AVI_SOURCE}" STREQUAL "")
    set(SHIP_OPENING_AVI ON)
endif()
set(SHIP_INDEO_DRIVER OFF)
if(DEFINED INDEO_DRIVER_SOURCE AND NOT "${INDEO_DRIVER_SOURCE}" STREQUAL "")
    if(NOT EXISTS "${INDEO_DRIVER_SOURCE}" OR IS_DIRECTORY "${INDEO_DRIVER_SOURCE}")
        message(FATAL_ERROR
            "QOH99_INDEO_DRIVER names a missing file: '${INDEO_DRIVER_SOURCE}'.")
    endif()
    set(SHIP_INDEO_DRIVER ON)
endif()

if(VERIFY_CONFIGURATION_ONLY)
    return()
endif()

foreach(required_variable
        DIST_DIR
        ZIP_PATH
        CASTER_SOURCE
        NETPLAY_SOURCE
        DDRAW_SOURCE)
    if(NOT DEFINED ${required_variable}
       OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "VerifyDistribution.cmake requires ${required_variable}")
    endif()
endforeach()

set(expected_files
    "README.txt"
    "README_ja.txt"
    "ddraw.dll"
    "manifest.sha256"
    "qoh99_netplay.dll"
    "qoh99caster.exe"
    "launch_wine.sh"
    "defender_exclusions.cmd")
if(SHIP_OPENING_AVI)
    list(APPEND expected_files "SYSTEM/OPENING_LOSSLESS.AVI")
endif()
if(SHIP_INDEO_DRIVER)
    list(APPEND expected_files "ir50_32.dll")
endif()
if(SDL3_SOURCE)
    list(APPEND expected_files "SDL3.dll")
endif()
list(SORT expected_files)

set(expected_pe_runtimes
    "ddraw.dll"
    "qoh99_netplay.dll"
    "qoh99caster.exe")
if(SDL3_SOURCE)
    list(APPEND expected_pe_runtimes "SDL3.dll")
endif()

set(expected_shell_scripts
    "launch_wine.sh")

set(required_wrapper_exports
    "EFZDDRAW_GetNetplayPacingAbi"
    "EFZDDRAW_SetFrameLimit")

set(manifest_name "manifest.sha256")
set(contract_prefix "QOH99_NETPLAY_CONTRACT_")

if(NOT IS_DIRECTORY "${DIST_DIR}")
    message(FATAL_ERROR "Distribution directory does not exist: ${DIST_DIR}")
endif()
if(NOT EXISTS "${ZIP_PATH}" OR IS_DIRECTORY "${ZIP_PATH}")
    message(FATAL_ERROR "Distribution archive does not exist: ${ZIP_PATH}")
endif()

function(qoh99_pe_read path offset size out_variable)
    file(READ "${path}" hex OFFSET ${offset} LIMIT ${size} HEX)
    string(LENGTH "${hex}" hex_length)
    math(EXPR expected_length "${size} * 2")
    if(NOT hex_length EQUAL expected_length)
        set(${out_variable} "" PARENT_SCOPE)
        return()
    endif()
    set(value 0)
    math(EXPR last_byte "${size} - 1")
    foreach(index RANGE ${last_byte})
        math(EXPR from "(${last_byte} - ${index}) * 2")
        string(SUBSTRING "${hex}" ${from} 2 byte)
        math(EXPR value "${value} * 256 + 0x${byte}")
    endforeach()
    set(${out_variable} "${value}" PARENT_SCOPE)
endfunction()

function(qoh99_pe_header path out_variable)
    set(${out_variable} "" PARENT_SCOPE)
    file(READ "${path}" dos_magic LIMIT 2 HEX)
    if(NOT dos_magic STREQUAL "4d5a")
        return()
    endif()
    qoh99_pe_read("${path}" 60 4 header_offset)
    if(header_offset STREQUAL "" OR header_offset LESS 64)
        return()
    endif()
    file(READ "${path}" signature OFFSET ${header_offset} LIMIT 4 HEX)
    if(NOT signature STREQUAL "50450000")
        return()
    endif()
    set(${out_variable} "${header_offset}" PARENT_SCOPE)
endfunction()

function(qoh99_pe_read_name path offset out_variable)
    file(READ "${path}" hex OFFSET ${offset} LIMIT 128 HEX)
    string(LENGTH "${hex}" hex_length)
    set(result "")
    if(hex_length LESS 2)
        set(${out_variable} "" PARENT_SCOPE)
        return()
    endif()
    math(EXPR last_byte "${hex_length} / 2 - 1")
    foreach(index RANGE ${last_byte})
        math(EXPR from "${index} * 2")
        string(SUBSTRING "${hex}" ${from} 2 byte)
        math(EXPR code "0x${byte}")
        if(code EQUAL 0)
            break()
        endif()
        if(NOT ((code GREATER 47 AND code LESS 58)
                OR (code GREATER 64 AND code LESS 91)
                OR (code GREATER 96 AND code LESS 123)
                OR code EQUAL 46 OR code EQUAL 64 OR code EQUAL 95))
            set(${out_variable} "" PARENT_SCOPE)
            return()
        endif()
        string(ASCII ${code} character)
        string(APPEND result "${character}")
    endforeach()
    set(${out_variable} "${result}" PARENT_SCOPE)
endfunction()

function(qoh99_pe_rva_to_offset path header rva out_variable)
    set(${out_variable} "" PARENT_SCOPE)
    math(EXPR section_count_offset "${header} + 6")
    math(EXPR optional_size_offset "${header} + 20")
    qoh99_pe_read("${path}" ${section_count_offset} 2 section_count)
    qoh99_pe_read("${path}" ${optional_size_offset} 2 optional_size)
    if(section_count STREQUAL "" OR optional_size STREQUAL ""
       OR section_count EQUAL 0)
        return()
    endif()
    math(EXPR table "${header} + 24 + ${optional_size}")
    math(EXPR last_section "${section_count} - 1")
    foreach(index RANGE ${last_section})
        math(EXPR entry "${table} + ${index} * 40")
        math(EXPR virtual_size_offset "${entry} + 8")
        math(EXPR virtual_address_offset "${entry} + 12")
        math(EXPR raw_size_offset "${entry} + 16")
        math(EXPR raw_pointer_offset "${entry} + 20")
        qoh99_pe_read("${path}" ${virtual_size_offset} 4 virtual_size)
        qoh99_pe_read("${path}" ${virtual_address_offset} 4 virtual_address)
        qoh99_pe_read("${path}" ${raw_size_offset} 4 raw_size)
        qoh99_pe_read("${path}" ${raw_pointer_offset} 4 raw_pointer)
        if(virtual_address STREQUAL "" OR raw_pointer STREQUAL "")
            return()
        endif()
        set(span "${raw_size}")
        if(span EQUAL 0)
            set(span "${virtual_size}")
        endif()
        math(EXPR section_end "${virtual_address} + ${span}")
        if(NOT rva LESS virtual_address AND rva LESS section_end)
            math(EXPR file_offset "${raw_pointer} + ${rva} - ${virtual_address}")
            set(${out_variable} "${file_offset}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

function(qoh99_pe_named_exports path out_variable)
    set(${out_variable} "" PARENT_SCOPE)
    qoh99_pe_header("${path}" header)
    if(header STREQUAL "")
        return()
    endif()
    math(EXPR directory_offset "${header} + 24 + 96")
    qoh99_pe_read("${path}" ${directory_offset} 4 export_rva)
    if(export_rva STREQUAL "" OR export_rva EQUAL 0)
        return()
    endif()
    qoh99_pe_rva_to_offset("${path}" ${header} ${export_rva} directory)
    if(directory STREQUAL "")
        return()
    endif()
    math(EXPR ordinal_base_offset "${directory} + 16")
    math(EXPR name_count_offset "${directory} + 24")
    math(EXPR name_table_offset "${directory} + 32")
    math(EXPR ordinal_table_offset "${directory} + 36")
    qoh99_pe_read("${path}" ${ordinal_base_offset} 4 ordinal_base)
    qoh99_pe_read("${path}" ${name_count_offset} 4 name_count)
    qoh99_pe_read("${path}" ${name_table_offset} 4 name_table_rva)
    qoh99_pe_read("${path}" ${ordinal_table_offset} 4 ordinal_table_rva)
    if(name_count STREQUAL "" OR name_count EQUAL 0)
        return()
    endif()
    if(name_count GREATER 8192)
        message(FATAL_ERROR
            "${path} declares ${name_count} export names; refusing to walk a "
            "malformed export directory")
    endif()
    qoh99_pe_rva_to_offset("${path}" ${header} ${name_table_rva} name_table)
    qoh99_pe_rva_to_offset("${path}" ${header} ${ordinal_table_rva}
                           ordinal_table)
    if(name_table STREQUAL "" OR ordinal_table STREQUAL "")
        return()
    endif()
    set(exports)
    math(EXPR last_name "${name_count} - 1")
    foreach(index RANGE ${last_name})
        math(EXPR name_entry "${name_table} + ${index} * 4")
        math(EXPR ordinal_entry "${ordinal_table} + ${index} * 2")
        qoh99_pe_read("${path}" ${name_entry} 4 name_rva)
        qoh99_pe_read("${path}" ${ordinal_entry} 2 ordinal_index)
        if(name_rva STREQUAL "" OR ordinal_index STREQUAL "")
            continue()
        endif()
        qoh99_pe_rva_to_offset("${path}" ${header} ${name_rva} name_offset)
        if(name_offset STREQUAL "")
            continue()
        endif()
        qoh99_pe_read_name("${path}" ${name_offset} export_name)
        if(export_name STREQUAL "")
            continue()
        endif()
        math(EXPR ordinal "${ordinal_base} + ${ordinal_index}")
        list(APPEND exports "${ordinal} ${export_name}")
    endforeach()
    set(${out_variable} "${exports}" PARENT_SCOPE)
endfunction()

function(qoh99_file_bytes path out_variable)
    file(READ "${path}" file_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\1;" file_bytes "${file_hex}")
    set(${out_variable} "${file_bytes}" PARENT_SCOPE)
endfunction()

function(qoh99_build_contract path out_variable)
    set(${out_variable} "" PARENT_SCOPE)
    file(STRINGS "${path}" candidates
        REGEX "${contract_prefix}[A-Za-z0-9_]+"
        LIMIT_COUNT 4)
    foreach(line IN LISTS candidates)
        string(REGEX MATCH "${contract_prefix}[A-Za-z0-9_]+" marker "${line}")
        if(NOT marker STREQUAL "")
            set(${out_variable} "${marker}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

file(GLOB_RECURSE staged_files
    LIST_DIRECTORIES false
    RELATIVE "${DIST_DIR}"
    "${DIST_DIR}/*")
set(normalized_staged_files)
foreach(path IN LISTS staged_files)
    string(REPLACE "\\" "/" path "${path}")
    list(APPEND normalized_staged_files "${path}")
endforeach()
list(SORT normalized_staged_files)

if(NOT "${normalized_staged_files}" STREQUAL "${expected_files}")
    message(FATAL_ERROR
        "Staged distribution manifest mismatch.\n"
        "Expected: ${expected_files}\n"
        "Actual:   ${normalized_staged_files}")
endif()

function(qoh99_require_no_case_collision label)
    set(seen)
    foreach(path IN LISTS ARGN)
        string(TOLOWER "${path}" folded)
        if(folded IN_LIST seen)
            message(FATAL_ERROR
                "${label} contains paths that differ only by case: ${path}\n"
                "A case-insensitive host would overwrite one with the other.")
        endif()
        list(APPEND seen "${folded}")
    endforeach()
endfunction()

qoh99_require_no_case_collision(
    "Staged distribution" ${normalized_staged_files})

function(require_identical_binary staged_name source_path)
    set(staged_path "${DIST_DIR}/${staged_name}")
    if(NOT EXISTS "${source_path}" OR IS_DIRECTORY "${source_path}")
        message(FATAL_ERROR
            "Built source artifact is missing: ${source_path}")
    endif()
    if(NOT EXISTS "${staged_path}" OR IS_DIRECTORY "${staged_path}")
        message(FATAL_ERROR
            "Staged runtime artifact is missing: ${staged_path}")
    endif()
    file(SHA256 "${source_path}" source_sha256)
    file(SHA256 "${staged_path}" staged_sha256)
    if(NOT source_sha256 STREQUAL staged_sha256)
        message(FATAL_ERROR
            "${staged_name} does not match its canonical build target "
            "(source=${source_sha256}, staged=${staged_sha256})")
    endif()
endfunction()

require_identical_binary("qoh99caster.exe" "${CASTER_SOURCE}")
require_identical_binary("qoh99_netplay.dll" "${NETPLAY_SOURCE}")
require_identical_binary("ddraw.dll" "${DDRAW_SOURCE}")
if(SHIP_OPENING_AVI)
require_identical_binary("SYSTEM/OPENING_LOSSLESS.AVI" "${OPENING_AVI_SOURCE}")

set(movie_name "SYSTEM/OPENING_LOSSLESS.AVI")
file(READ "${DIST_DIR}/${movie_name}" movie_head LIMIT 4096 HEX)
string(SUBSTRING "${movie_head}" 0 8 movie_riff)
string(SUBSTRING "${movie_head}" 16 8 movie_form)
if(NOT movie_riff STREQUAL "52494646" OR NOT movie_form STREQUAL "41564920")
    message(FATAL_ERROR "${movie_name} is not a RIFF AVI file")
endif()
string(FIND "${movie_head}" "7669647351393956" movie_q99v)
string(FIND "${movie_head}" "7669647349563530" movie_indeo)
if(NOT movie_indeo EQUAL -1)
    message(FATAL_ERROR
        "${movie_name} is the Indeo 5 (IV50) original; the package must ship "
        "the Q99V conversion (tools/convert_opening_movie.py, see "
        "qoh99_re/docs/subsystems/opening_movie.md)")
endif()
if(movie_q99v EQUAL -1)
    message(FATAL_ERROR
        "${movie_name} does not declare a Q99V video stream")
endif()
message(STATUS "  ${movie_name} is a Q99V (lossless) AVI")
endif()

if(SHIP_INDEO_DRIVER)
    require_identical_binary("ir50_32.dll" "${INDEO_DRIVER_SOURCE}")
    file(SIZE "${DIST_DIR}/ir50_32.dll" indeo_size)
    if(indeo_size LESS 500000)
        message(FATAL_ERROR
            "ir50_32.dll is ${indeo_size} bytes: that is the Windows forwarding "
            "shim, not the 755 KB Intel Indeo 5.10 driver. It cannot decode on "
            "its own; ship the full driver or leave QOH99_INDEO_DRIVER empty.")
    endif()
    file(STRINGS "${DIST_DIR}/ir50_32.dll" indeo_strings REGEX "Indeo" LIMIT_COUNT 1)
    if("${indeo_strings}" STREQUAL "")
        message(FATAL_ERROR "ir50_32.dll does not identify itself as an Indeo driver")
    endif()
    message(STATUS "  ir50_32.dll is the Intel Indeo 5.10 driver (${indeo_size} bytes)")
endif()
if(SDL3_SOURCE)
    require_identical_binary("SDL3.dll" "${SDL3_SOURCE}")
    if(SDL3_EXPECTED_SHA256)
        file(SHA256 "${DIST_DIR}/SDL3.dll" staged_sdl3_sha256)
        if(NOT staged_sdl3_sha256 STREQUAL SDL3_EXPECTED_SHA256)
            message(FATAL_ERROR
                "shipped SDL3.dll is not the pinned artifact "
                "(expected=${SDL3_EXPECTED_SHA256} "
                "staged=${staged_sdl3_sha256})")
        endif()
    endif()
endif()

foreach(runtime IN LISTS expected_pe_runtimes)
    set(runtime_path "${DIST_DIR}/${runtime}")
    file(READ "${runtime_path}" dos_magic LIMIT 2 HEX)
    if(NOT dos_magic STREQUAL "4d5a")
        message(FATAL_ERROR "${runtime} is not a PE image (no MZ header)")
    endif()
    qoh99_pe_read("${runtime_path}" 60 4 pe_offset)
    if(pe_offset STREQUAL "" OR pe_offset LESS 64)
        message(FATAL_ERROR "${runtime} has an invalid DOS e_lfanew")
    endif()
    file(READ "${runtime_path}" pe_signature OFFSET ${pe_offset} LIMIT 4 HEX)
    if(NOT pe_signature STREQUAL "50450000")
        message(FATAL_ERROR
            "${runtime} has no PE signature at e_lfanew=${pe_offset}")
    endif()
    math(EXPR machine_offset "${pe_offset} + 4")
    file(READ "${runtime_path}" machine OFFSET ${machine_offset} LIMIT 2 HEX)
    if(NOT machine STREQUAL "4c01")
        message(FATAL_ERROR
            "${runtime} COFF Machine is not 014c/i386 (little-endian bytes "
            "were ${machine}); the game process is 32-bit")
    endif()
    math(EXPR magic_offset "${pe_offset} + 24")
    file(READ "${runtime_path}" optional_magic
        OFFSET ${magic_offset} LIMIT 2 HEX)
    if(optional_magic STREQUAL "0b02")
        message(FATAL_ERROR "${runtime} is PE32+ (64-bit); PE32 is required")
    endif()
    if(NOT optional_magic STREQUAL "0b01")
        message(FATAL_ERROR
            "${runtime} has an unknown optional-header magic "
            "(little-endian bytes ${optional_magic})")
    endif()
endforeach()

qoh99_pe_named_exports("${DIST_DIR}/ddraw.dll" wrapper_exports)
if(NOT wrapper_exports)
    message(FATAL_ERROR
        "The staged ddraw.dll has no readable export directory; the netplay "
        "pacing ABI cannot be verified")
endif()

set(wrapper_def "${CMAKE_CURRENT_LIST_DIR}/../ddraw_wrapper/ddraw.def")
set(def_available FALSE)
if(EXISTS "${wrapper_def}")
    set(def_available TRUE)
    file(STRINGS "${wrapper_def}" wrapper_def_lines)
endif()

foreach(required_export IN LISTS required_wrapper_exports)
    set(found_ordinal "")
    foreach(entry IN LISTS wrapper_exports)
        if(entry MATCHES "^([0-9]+) ${required_export}$")
            set(found_ordinal "${CMAKE_MATCH_1}")
            break()
        endif()
    endforeach()
    if(found_ordinal STREQUAL "")
        message(FATAL_ERROR
            "Staged ddraw.dll does not export ${required_export}. Wine's "
            "builtin DirectDraw is not a substitute: src/GameHooks.cpp "
            "refuses to start netplay without this ABI.")
    endif()
    if(def_available)
        set(declared_ordinal "")
        foreach(line IN LISTS wrapper_def_lines)
            if(line MATCHES "^[ \t]*${required_export}[ \t]+@([0-9]+)")
                set(declared_ordinal "${CMAKE_MATCH_1}")
                break()
            endif()
        endforeach()
        if(declared_ordinal STREQUAL "")
            message(FATAL_ERROR
                "ddraw.def no longer declares an ordinal for "
                "${required_export}")
        endif()
        if(NOT declared_ordinal EQUAL found_ordinal)
            message(FATAL_ERROR
                "${required_export} is exported at ordinal ${found_ordinal} "
                "but ddraw.def declares @${declared_ordinal}")
        endif()
    endif()
    message(STATUS
        "  ddraw.dll exports ${required_export} at ordinal ${found_ordinal}")
endforeach()

qoh99_build_contract("${DIST_DIR}/qoh99caster.exe" caster_contract)
qoh99_build_contract("${DIST_DIR}/qoh99_netplay.dll" netplay_contract)
if(caster_contract STREQUAL "")
    message(FATAL_ERROR
        "Staged qoh99caster.exe carries no ${contract_prefix}* marker")
endif()
if(netplay_contract STREQUAL "")
    message(FATAL_ERROR
        "Staged qoh99_netplay.dll carries no ${contract_prefix}* marker")
endif()
if(NOT caster_contract STREQUAL netplay_contract)
    message(FATAL_ERROR
        "The staged caster and netplay DLL are from different builds.\n"
        "caster: ${caster_contract}\n"
        "dll   : ${netplay_contract}")
endif()

foreach(script IN LISTS expected_shell_scripts)
    set(script_path "${DIST_DIR}/${script}")
    qoh99_file_bytes("${script_path}" script_bytes)
    list(FIND script_bytes "0d" carriage_return)
    if(NOT carriage_return EQUAL -1)
        message(FATAL_ERROR
            "${script} contains a carriage return at byte ${carriage_return}; "
            "shipped shell scripts must be LF-only or Linux answers with "
            "\"/usr/bin/env: 'bash\\r': No such file or directory\"")
    endif()
    list(FIND script_bytes "00" embedded_nul)
    if(NOT embedded_nul EQUAL -1)
        message(FATAL_ERROR
            "${script} contains a NUL byte at ${embedded_nul}; it is not a "
            "text script")
    endif()
    file(READ "${script_path}" script_text)
    string(REGEX MATCH "^[^\n]*" shebang "${script_text}")
    if(NOT shebang STREQUAL "#!/usr/bin/env bash")
        message(FATAL_ERROR
            "${script} must start with '#!/usr/bin/env bash' (found "
            "'${shebang}')")
    endif()
    if(NOT script_text MATCHES "set -euo pipefail")
        message(FATAL_ERROR
            "${script} does not enable 'set -euo pipefail'")
    endif()
endforeach()

set(manifest_path "${DIST_DIR}/${manifest_name}")
qoh99_file_bytes("${manifest_path}" manifest_bytes)
list(FIND manifest_bytes "0d" manifest_carriage_return)
if(NOT manifest_carriage_return EQUAL -1)
    message(FATAL_ERROR
        "${manifest_name} contains CRLF; `sha256sum -c` on Linux would look "
        "for path names ending in a carriage return")
endif()
file(READ "${manifest_path}" manifest_text)
string(REGEX REPLACE "\n+$" "" manifest_text "${manifest_text}")
if(manifest_text STREQUAL "")
    message(FATAL_ERROR "${manifest_name} is empty")
endif()
string(REPLACE "\n" ";" manifest_lines "${manifest_text}")

set(manifest_paths)
foreach(line IN LISTS manifest_lines)
    if(NOT line MATCHES "^([0-9a-f]+)  (.+)$")
        message(FATAL_ERROR
            "${manifest_name} has a line that is not `<sha256>  <path>`: "
            "${line}")
    endif()
    set(listed_hash "${CMAKE_MATCH_1}")
    set(listed_path "${CMAKE_MATCH_2}")
    string(LENGTH "${listed_hash}" listed_hash_length)
    if(NOT listed_hash_length EQUAL 64)
        message(FATAL_ERROR
            "${manifest_name}: ${listed_path} has a ${listed_hash_length}-"
            "character hash; SHA-256 is 64")
    endif()
    if(NOT EXISTS "${DIST_DIR}/${listed_path}")
        message(FATAL_ERROR
            "${manifest_name} lists a file that is not staged: ${listed_path}")
    endif()
    file(SHA256 "${DIST_DIR}/${listed_path}" actual_hash)
    string(TOLOWER "${actual_hash}" actual_hash)
    if(NOT actual_hash STREQUAL listed_hash)
        message(FATAL_ERROR
            "${manifest_name} hash mismatch for ${listed_path} "
            "(listed=${listed_hash} actual=${actual_hash})")
    endif()
    list(APPEND manifest_paths "${listed_path}")
endforeach()
list(SORT manifest_paths)

set(expected_manifest_paths ${expected_files})
list(REMOVE_ITEM expected_manifest_paths "${manifest_name}")
list(SORT expected_manifest_paths)
if(NOT "${manifest_paths}" STREQUAL "${expected_manifest_paths}")
    message(FATAL_ERROR
        "${manifest_name} does not list exactly the staged files.\n"
        "Expected: ${expected_manifest_paths}\n"
        "Listed:   ${manifest_paths}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${ZIP_PATH}"
    RESULT_VARIABLE zip_list_result
    OUTPUT_VARIABLE zip_list_output
    ERROR_VARIABLE zip_list_error)
if(NOT zip_list_result EQUAL 0)
    message(FATAL_ERROR
        "Could not list distribution archive ${ZIP_PATH}: ${zip_list_error}")
endif()

string(REPLACE "\r\n" "\n" zip_list_output "${zip_list_output}")
string(REPLACE "\r" "\n" zip_list_output "${zip_list_output}")
string(REGEX REPLACE "\n+$" "" zip_list_output "${zip_list_output}")
if(zip_list_output STREQUAL "")
    message(FATAL_ERROR "Distribution archive is empty: ${ZIP_PATH}")
endif()
string(REPLACE "\n" ";" zip_entries "${zip_list_output}")

set(normalized_zip_files)
foreach(path IN LISTS zip_entries)
    string(REPLACE "\\" "/" path "${path}")
    string(REGEX REPLACE "^\\./" "" path "${path}")
    if(path MATCHES "/$")
        continue()
    endif()
    if(path MATCHES "^/" OR path MATCHES "^[A-Za-z]:"
       OR path MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR
            "Unsafe path in distribution archive: ${path}")
    endif()
    list(APPEND normalized_zip_files "${path}")
endforeach()
list(SORT normalized_zip_files)

if(NOT "${normalized_zip_files}" STREQUAL "${expected_files}")
    message(FATAL_ERROR
        "ZIP distribution manifest mismatch.\n"
        "Expected: ${expected_files}\n"
        "Actual:   ${normalized_zip_files}")
endif()

qoh99_require_no_case_collision(
    "Distribution archive" ${normalized_zip_files})

set(relay_binary_pattern "qoh99relay")

foreach(path IN LISTS normalized_staged_files)
    get_filename_component(staged_name "${path}" NAME)
    if(staged_name MATCHES "${relay_binary_pattern}")
        message(FATAL_ERROR
            "The distribution still contains a standalone relay artifact: "
            "${path}. The relay is `qoh99caster relay`.")
    endif()
endforeach()

set(relay_scanned_files ${expected_shell_scripts}
    "README.txt" "README_ja.txt")
foreach(path IN LISTS relay_scanned_files)
    file(READ "${DIST_DIR}/${path}" scanned_text)
    if(scanned_text MATCHES "${relay_binary_pattern}")
        message(FATAL_ERROR
            "Packaged file ${path} still references '${relay_binary_pattern}'. "
            "Use `qoh99caster relay [--bind IP] [--port N]` instead.")
    endif()
endforeach()

file(GLOB source_scripts "${CMAKE_CURRENT_LIST_DIR}/../scripts/*.sh")
foreach(path IN LISTS source_scripts)
    file(READ "${path}" scanned_text)
    if(scanned_text MATCHES "${relay_binary_pattern}")
        message(FATAL_ERROR
            "Source script ${path} still references "
            "'${relay_binary_pattern}'.")
    endif()
endforeach()

message(STATUS
    "Verified QOH99 distribution: ${DIST_DIR} and ${ZIP_PATH}")
message(STATUS
    "  build contract: ${caster_contract}")
