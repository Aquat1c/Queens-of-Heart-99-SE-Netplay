cmake_minimum_required(VERSION 3.15)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(qoh99_ddraw_wrapper LANGUAGES C CXX)
include(CTest)

option(QOH99_DDRAW_FORCE_LOGGING
    "Diagnostic builds only: default the wrapper log to VERBOSE with no env \
var required. An explicit QOH99_WRAPPER_LOG still overrides it either way."
    OFF)

set(EFZ_IMGUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui"
    CACHE PATH "Dear ImGui source dir")
if(NOT EXISTS "${EFZ_IMGUI_SOURCE_DIR}/imgui.h")
    message(FATAL_ERROR "Dear ImGui reference sources were not found at ${EFZ_IMGUI_SOURCE_DIR}")
endif()
if(NOT EXISTS "${EFZ_IMGUI_SOURCE_DIR}/LICENSE.txt")
    message(FATAL_ERROR "Dear ImGui license was not found at ${EFZ_IMGUI_SOURCE_DIR}/LICENSE.txt")
endif()

set(QOH99_IMGUI_EXPECTED_VERSION_NUM 19191)
set(QOH99_IMGUI_EXPECTED_VERSION "1.91.9b")
file(STRINGS "${EFZ_IMGUI_SOURCE_DIR}/imgui.h" QOH99_IMGUI_VERSION_LINE
     REGEX "^#define[ \t]+IMGUI_VERSION_NUM[ \t]+[0-9]+")
if(NOT QOH99_IMGUI_VERSION_LINE)
    message(FATAL_ERROR
        "Could not read IMGUI_VERSION_NUM from ${EFZ_IMGUI_SOURCE_DIR}/imgui.h; "
        "expected Dear ImGui ${QOH99_IMGUI_EXPECTED_VERSION} "
        "(IMGUI_VERSION_NUM ${QOH99_IMGUI_EXPECTED_VERSION_NUM}).")
endif()
string(REGEX MATCH "[0-9]+" QOH99_IMGUI_VERSION_NUM "${QOH99_IMGUI_VERSION_LINE}")
if(NOT QOH99_IMGUI_VERSION_NUM STREQUAL "${QOH99_IMGUI_EXPECTED_VERSION_NUM}")
    message(FATAL_ERROR
        "Dear ImGui at ${EFZ_IMGUI_SOURCE_DIR} is IMGUI_VERSION_NUM "
        "${QOH99_IMGUI_VERSION_NUM}, but this wrapper is pinned to "
        "${QOH99_IMGUI_EXPECTED_VERSION_NUM} (Dear ImGui "
        "${QOH99_IMGUI_EXPECTED_VERSION}). Restore the vendored revision "
        "instead of building against a different one.")
endif()

set(QOH99_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/platform")
if(NOT EXISTS "${QOH99_PLATFORM_DIR}/PeImage.cpp"
   OR NOT EXISTS "${QOH99_PLATFORM_DIR}/WidePath.cpp")
    message(FATAL_ERROR
        "The shared PE/IAT implementation was not found under "
        "${QOH99_PLATFORM_DIR} (PeImage.cpp + WidePath.cpp). The wrapper must "
        "not grow a second parser: restore "
        "src/platform/{PeImage,WidePath}.{h,cpp}.")
endif()

add_library(ddraw SHARED
    ddraw.c
    ddraw_log.cpp
    ddraw_pe_shim.cpp
    "${QOH99_PLATFORM_DIR}/PeImage.cpp"
    "${QOH99_PLATFORM_DIR}/WidePath.cpp"
    surface_ops.c
    surface_gdi_bridge.c
    qoh99_instrument.c
    qoh99_movie.cpp
    qoh99_movie_policy.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/game/OpeningMovieCodec.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz/miniz.c"
    render_dx9_imgui.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/input/KeyboardScanCodec.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/training/TrainingActionCatalog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/i18n/Tr.cpp"
    ddraw.def
    "${EFZ_IMGUI_SOURCE_DIR}/imgui.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/backends/imgui_impl_dx9.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
)

target_include_directories(ddraw PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz"
    "${EFZ_IMGUI_SOURCE_DIR}"
    "${EFZ_IMGUI_SOURCE_DIR}/backends"
)

target_compile_features(ddraw PRIVATE c_std_11 cxx_std_17)

target_compile_definitions(ddraw PRIVATE
    UNICODE
    _UNICODE
)
if(QOH99_DDRAW_FORCE_LOGGING)
    message(STATUS
        "ddraw: QOH99_DDRAW_FORCE_LOGGING is ON - this DLL defaults to "
        "VERBOSE logging with no environment variable required. Diagnostic "
        "builds only; do not ship this to players as the normal package.")
    target_compile_definitions(ddraw PRIVATE
        QOH99_WLOG_FORCE_LEVEL=QOH99_WLOG_VERBOSE)
endif()

if(MSVC)
    set_property(TARGET ddraw PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(ddraw PRIVATE
        /W3
        /GS
        /wd4100
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Debug>:/Zi>
        $<$<CONFIG:Release>:/O2>
    )
    target_link_libraries(ddraw PRIVATE
        kernel32
        user32
        gdi32
        d3d9
        dwmapi
        imm32
        winmm
        ole32
        oleaut32
        strmiids
    )
    target_link_options(ddraw PRIVATE
        /MACHINE:X86
        /DYNAMICBASE
        /NXCOMPAT
        /SUBSYSTEM:WINDOWS,6.0
        $<$<CONFIG:Debug>:/DEBUG>
    )
else()
    target_compile_options(ddraw PRIVATE
        -Wall
        -Wno-unused-parameter
        -m32
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Release>:-O2>
    )
    target_link_options(ddraw PRIVATE
        -m32
        -static
        -static-libgcc
        -Wl,--kill-at
        -Wl,--enable-stdcall-fixup
    )
    target_link_libraries(ddraw PRIVATE
        kernel32
        user32
        gdi32
        d3d9
        dwmapi
        imm32
        winmm
        ole32
        oleaut32
        strmiids
    )
endif()

set_target_properties(ddraw PROPERTIES
    OUTPUT_NAME   "ddraw"
    PREFIX        ""
    SUFFIX        ".dll"
)

if(BUILD_TESTING)
    add_executable(surface_ops_tests
        surface_ops.c
        tests/surface_ops_tests.c
    )
    target_include_directories(surface_ops_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(surface_ops_tests PRIVATE c_std_11)
    if(MSVC)
        set_property(TARGET surface_ops_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        target_compile_options(surface_ops_tests PRIVATE /W4)
    else()
        target_compile_options(surface_ops_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME surface_ops COMMAND surface_ops_tests)

    add_library(presentation_disposition_stub SHARED
        tests/presentation_disposition_stub.c)
    set_target_properties(presentation_disposition_stub PROPERTIES
        OUTPUT_NAME qoh99_netplay
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_stubs"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_stubs")
    add_executable(hidden_resource_fill_tests tests/hidden_resource_fill_tests.c)
    add_dependencies(hidden_resource_fill_tests ddraw presentation_disposition_stub)
    add_test(NAME hidden_resource_fill COMMAND hidden_resource_fill_tests
        "$<TARGET_FILE:ddraw>" "$<TARGET_FILE:presentation_disposition_stub>")

    add_executable(surface_gdi_bridge_tests
        surface_ops.c
        surface_gdi_bridge.c
        tests/surface_gdi_bridge_tests.c
    )
    target_include_directories(surface_gdi_bridge_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(surface_gdi_bridge_tests PRIVATE c_std_11)
    if(MSVC)
        set_property(TARGET surface_gdi_bridge_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        target_compile_options(surface_gdi_bridge_tests PRIVATE /W4)
    else()
        target_compile_options(surface_gdi_bridge_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    target_link_libraries(surface_gdi_bridge_tests PRIVATE gdi32 user32)
    add_test(NAME surface_gdi_bridge COMMAND surface_gdi_bridge_tests)

    add_executable(movie_policy_tests
        qoh99_movie_policy.c
        tests/movie_policy_tests.c
    )
    target_include_directories(movie_policy_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(movie_policy_tests PRIVATE c_std_11)
    if(MSVC)
        set_property(TARGET movie_policy_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        target_compile_options(movie_policy_tests PRIVATE /W4)
    else()
        target_compile_options(movie_policy_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME movie_policy COMMAND movie_policy_tests)

    add_executable(presentation_policy_tests
        tests/presentation_policy_tests.c
    )
    target_include_directories(presentation_policy_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(presentation_policy_tests PRIVATE c_std_11)
    if(MSVC)
        set_property(TARGET presentation_policy_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        target_compile_options(presentation_policy_tests PRIVATE /W4)
    else()
        target_compile_options(presentation_policy_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME presentation_policy COMMAND presentation_policy_tests)

    add_executable(input_display_layout_tests
        tests/input_display_layout_tests.cpp
    )
    target_include_directories(input_display_layout_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(input_display_layout_tests PRIVATE cxx_std_17)
    if(MSVC)
        set_property(TARGET input_display_layout_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        target_compile_options(input_display_layout_tests PRIVATE /W4)
    else()
        target_compile_options(input_display_layout_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME input_display_layout COMMAND input_display_layout_tests)

    add_executable(replay_hud_tests tests/replay_hud_tests.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/input/KeyboardScanCodec.cpp")
    target_include_directories(replay_hud_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_features(replay_hud_tests PRIVATE cxx_std_17)
    if(MSVC)
        set_property(TARGET replay_hud_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME replay_hud COMMAND replay_hud_tests)

    add_executable(pe_parser_tests
        tests/pe_parser_tests.c
        ddraw_pe_shim.cpp
        "${QOH99_PLATFORM_DIR}/PeImage.cpp"
        "${QOH99_PLATFORM_DIR}/WidePath.cpp"
    )
    target_include_directories(pe_parser_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_compile_features(pe_parser_tests PRIVATE c_std_11 cxx_std_17)
    if(MSVC)
        set_property(TARGET pe_parser_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        target_compile_options(pe_parser_tests PRIVATE /W3 /wd4100)
    else()
        target_compile_options(pe_parser_tests PRIVATE -Wall -Wno-unused-parameter)
    endif()
    target_link_libraries(pe_parser_tests PRIVATE kernel32)
    add_test(NAME pe_parser COMMAND pe_parser_tests)
endif()

install(TARGETS ddraw
    RUNTIME DESTINATION .
    COMPONENT Qoh99Distribution)
