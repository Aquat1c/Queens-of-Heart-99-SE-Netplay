cmake_minimum_required(VERSION 3.21)
project(qoh99_re CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(qoh99_re_model STATIC
    src/AddressMap.cpp
    src/MemoryView.cpp
    src/SceneSystem.cpp
    src/MatchSystem.cpp
    src/Fighter.cpp
    src/FighterPool.cpp
    src/CommandHistory.cpp
    src/InputSystem.cpp
    src/VanillaNetProtocol.cpp
    src/DirectPlaySystem.cpp
    src/RngSystem.cpp
    src/ArenaFrame.cpp
    src/EntitySystem.cpp
    src/RenderSystem.cpp
    src/HudSystem.cpp
    src/AudioSystem.cpp
    src/ResourceSystem.cpp
    src/TimingSystem.cpp
    src/GameplayRegionCatalog.cpp
)
target_include_directories(qoh99_re_model PUBLIC include)
if (MSVC)
    set_property(TARGET qoh99_re_model PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(qoh99_re_model PRIVATE /W4)
endif()

option(QOH99_RE_BUILD_TESTS "Build and register the qoh99_re model tests" ON)
if (DEFINED BUILD_TESTING AND NOT BUILD_TESTING)
    set(QOH99_RE_BUILD_TESTS OFF)
endif()

if (QOH99_RE_BUILD_TESTS)
    if (PROJECT_IS_TOP_LEVEL)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    else()
        find_package(Python3 QUIET COMPONENTS Interpreter)
    endif()

    if (Python3_Interpreter_FOUND)
        add_custom_target(qoh99_re_manifest_check ALL
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_model.py --check
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Checking canonical QOH99 manifest and generated model"
            VERBATIM
        )
    else()
        message(WARNING
            "qoh99_re: Python3 interpreter not found — the manifest/scan gates "
            "(qoh99_re_manifest, qoh99_re_scan) are skipped in this build")
    endif()

    add_executable(qoh99_re_tests
        tests/test_main.cpp
        tests/test_addresses.cpp
        tests/test_generated_model.cpp
        tests/test_native_symbol_catalog.cpp
        tests/test_input.cpp
        tests/test_protocol.cpp
        tests/test_state_machine.cpp
        tests/test_vs_formation.cpp
        tests/test_match_flow.cpp
        tests/test_region_catalog.cpp
    )
    if (TARGET qoh99_re_manifest_check)
        add_dependencies(qoh99_re_tests qoh99_re_manifest_check)
    endif()
    target_link_libraries(qoh99_re_tests PRIVATE qoh99_re_model)
    target_include_directories(qoh99_re_tests PRIVATE include tests)
    if (MSVC)
        set_property(TARGET qoh99_re_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(qoh99_re_tests PRIVATE /W4)
    endif()

    enable_testing()
    add_test(NAME qoh99_re_unit COMMAND qoh99_re_tests)
    if (Python3_Interpreter_FOUND)
        add_test(NAME qoh99_re_manifest
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_manifest.py
        )
        set_tests_properties(qoh99_re_manifest PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
