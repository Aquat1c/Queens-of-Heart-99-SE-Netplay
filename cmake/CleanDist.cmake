if(NOT DEFINED DIST_DIR)
    message(FATAL_ERROR "CleanDist.cmake requires -DDIST_DIR=<path>")
endif()

if(NOT EXISTS "${DIST_DIR}")
    file(MAKE_DIRECTORY "${DIST_DIR}")
    return()
endif()

file(GLOB entries LIST_DIRECTORIES true "${DIST_DIR}/*")
foreach(entry IN LISTS entries)
    file(REMOVE_RECURSE "${entry}")
endforeach()

file(GLOB remaining LIST_DIRECTORIES true "${DIST_DIR}/*")
if(remaining)
    message(FATAL_ERROR
        "Could not empty ${DIST_DIR}; these survived and would be packaged:\n"
        "  ${remaining}\n"
        "Close anything holding them open and build again.")
endif()
