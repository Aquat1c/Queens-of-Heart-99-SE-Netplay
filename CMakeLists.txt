cmake_minimum_required(VERSION 3.15)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(qoh99_netplay LANGUAGES C CXX)
enable_testing()

add_executable(qoh99_log_tests src/tests/LogTests.cpp
    src/CrashDump.cpp src/CrashContext.cpp ddraw_wrapper/ddraw_log.cpp)
target_include_directories(qoh99_log_tests PRIVATE src)
target_link_libraries(qoh99_log_tests PRIVATE qoh99_platform)
if(MSVC)
    set_property(TARGET qoh99_log_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_compile_features(qoh99_log_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_log_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME qoh99_logging COMMAND qoh99_log_tests)
set_tests_properties(qoh99_logging PROPERTIES TIMEOUT 30)

option(QOH99_BUILD_DDRAW_WRAPPER
    "Build the QOH99 DirectDraw wrapper with the netplay release" ON)
if(QOH99_BUILD_DDRAW_WRAPPER)
    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/ddraw_wrapper"
        "${CMAKE_BINARY_DIR}/ddraw_wrapper")
endif()

set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook"
    CACHE PATH "MinHook source dir")
if(NOT EXISTS "${MINHOOK_DIR}/include/MinHook.h")
    message(FATAL_ERROR "MinHook not found at ${MINHOOK_DIR}")
endif()
if(NOT EXISTS "${MINHOOK_DIR}/COPYING.txt")
    message(FATAL_ERROR "MinHook license not found at ${MINHOOK_DIR}/COPYING.txt")
endif()

set(MINHOOK_SOURCES
    "${MINHOOK_DIR}/src/buffer.cc"
    "${MINHOOK_DIR}/src/export.cc"
    "${MINHOOK_DIR}/src/hook.cc"
    "${MINHOOK_DIR}/src/thread.cc"
    "${MINHOOK_DIR}/src/trampoline.cc"
    "${MINHOOK_DIR}/src/hde32/hde32.c"
)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/qoh99_re" qoh99_re_model_build)
set(QOH99_RE_INC "${CMAKE_CURRENT_SOURCE_DIR}/qoh99_re/include")

add_library(qoh99_platform STATIC
    src/platform/WidePath.cpp
    src/platform/CompatEnv.cpp
    src/platform/PeImage.cpp
    src/platform/WineSocketRepair.cpp
    src/platform/CompatPreamble.cpp
)
target_include_directories(qoh99_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_platform PUBLIC cxx_std_17)
target_compile_definitions(qoh99_platform PUBLIC _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_platform PUBLIC ws2_32)
if(MSVC)
    set_property(TARGET qoh99_platform PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(qoh99_replay STATIC
    src/replay/ReplayIntegrity.cpp
    src/replay/ReplayCodec.cpp
    src/replay/ReplayCodecV2.cpp
    src/replay/ReplaySimulationContractV2.cpp
    src/replay/ReplayCompatibilityIdentityV3.cpp
    src/replay/CharacterCatalog.cpp
    src/replay/StageCatalog.cpp
    src/replay/ReplayTimeline.cpp
    src/replay/ConfirmedTimeline.cpp
    src/replay/GameEpochTracker.cpp
    src/replay/ReplayRecordingCoordinator.cpp
    src/replay/ReplayPublisherQueueV2.cpp
    src/replay/ReplayRecorderQueue.cpp
    src/replay/ReplayPlaybackController.cpp
    src/replay/ReplayPlaybackSessionV2.cpp
    src/replay/ReplayCheckpoint.cpp
    src/replay/ReplayTakeoverController.cpp
)
target_include_directories(qoh99_replay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_replay PUBLIC cxx_std_17)
target_compile_definitions(qoh99_replay PUBLIC _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET qoh99_replay PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(qoh99_content_identity STATIC
    src/content/SimulationContentIdentity.cpp
)
target_include_directories(qoh99_content_identity PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_content_identity PUBLIC cxx_std_17)
target_compile_definitions(qoh99_content_identity PUBLIC _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_content_identity PUBLIC qoh99_replay)
if(MSVC)
    set_property(TARGET qoh99_content_identity PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(qoh99net STATIC
    src/net/Sockets.cpp
    src/net/Reliable.cpp
    src/net/ControlLink.cpp
    src/net/RelayProtocol.cpp
    src/net/RelayClient.cpp
    src/net/Autopunch.cpp
    src/net/AutopunchSession.cpp
    src/net/AutopunchPeerSet.cpp
    src/net/PortMapper.cpp
    src/net/SecureRandom.cpp
)
target_include_directories(qoh99net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99net PUBLIC cxx_std_17)
target_compile_definitions(qoh99net PUBLIC _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET qoh99net PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(qoh99_netplay_core STATIC
    src/netplay/WireProtocol.cpp
    src/netplay/NetplayCore.cpp
    src/netplay/RollbackSession.cpp
)
target_include_directories(qoh99_netplay_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_netplay_core PUBLIC cxx_std_17)
target_compile_definitions(qoh99_netplay_core PUBLIC _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET qoh99_netplay_core PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(qoh99_spectator STATIC
    src/spectator/SpectatorProtocol.cpp
    src/spectator/SpectatorProtocolV4.cpp
    src/spectator/SpectatorRelayAdapter.cpp
    src/spectator/SpectatorArchive.cpp
    src/spectator/SpectatorConnectedArchiveV4.cpp
    src/spectator/SpectatorSession.cpp
    src/spectator/SpectatorConnectedSessionV4.cpp
    src/spectator/SpectatorCheckpoint.cpp
    src/spectator/SpectatorGrant.cpp
    src/spectator/TopologyGrant.cpp
    src/spectator/TopologyRelayAdapter.cpp
    src/spectator/SpectatorSetArchive.cpp
    src/spectator/ResilientSpectatorDistribution.cpp
    src/spectator/ResilientSpectatorProtocol.cpp
)
target_include_directories(qoh99_spectator PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_spectator PUBLIC cxx_std_17)
target_compile_definitions(qoh99_spectator PUBLIC
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_spectator PUBLIC qoh99_replay qoh99net)
if(MSVC)
    set_property(TARGET qoh99_spectator PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(qoh99_training STATIC
    src/training/TrainingCommandQueue.cpp
    src/training/TrainingStatusPublisher.cpp
    src/training/TrainingInput.cpp
    src/training/TrainingMacros.cpp
    src/training/TrainingDummy.cpp
    src/training/TrainingPositions.cpp
    src/training/TrainingRecovery.cpp
    src/training/TrainingResources.cpp
    src/training/TrainingCombatTools.cpp
    src/training/TrainingActionCatalog.cpp
    src/input/InputDisplayPublisher.cpp
    src/training/TrainingHitboxPublisher.cpp
)
target_include_directories(qoh99_training PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_training PUBLIC cxx_std_17)
target_compile_definitions(qoh99_training PUBLIC _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET qoh99_training PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(qoh99_training_tests
    src/training/tests/test_training_main.cpp
    src/training/tests/test_training_core.cpp
    src/training/tests/test_training_combat_tools.cpp
    src/training/tests/test_training_action_catalog.cpp
    src/training/tests/test_training_contact_policies.cpp
    src/training/tests/test_training_hitbox_overlay.cpp
)
find_package(Threads REQUIRED)
target_include_directories(qoh99_training_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_training_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_training_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_training_tests PRIVATE
    qoh99_training Threads::Threads)
if(MSVC)
    set_property(TARGET qoh99_training_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_training_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_training_core COMMAND qoh99_training_tests)

add_library(qoh99_netplay SHARED
    src/DllMain.cpp
    src/GameHooks.cpp
    src/client/BuildIdentity.cpp
    src/training/NativeRecoveryQualification.cpp
    src/RollbackManager.cpp
    src/CrashContext.cpp
    src/CrashDump.cpp
    src/game/DPlayTransport.cpp
    src/game/DirectBoot.cpp
    src/qoh99/qoh99_probe.cpp
    src/game/replay/ReplayRuntime.cpp
    src/game/replay/ReplayTimelineSource.cpp
    src/game/replay/ReplayIpcWorker.cpp
    src/game/spectator/SpectatorPublisher.cpp
    src/game/spectator/SpectatorPublisherV4.cpp
    src/game/spectator/SpectatorReplayPublisherV4.cpp
    src/client/spectator/ResilientTopologyClient.cpp
    src/client/spectator/ResilientTopologyClientRuntime.cpp
    src/client/spectator/TopologyAuthoritySignature.cpp
    src/client/spectator/ResilientSpectatorNodeRuntime.cpp
    ${MINHOOK_SOURCES}
)

target_include_directories(qoh99_netplay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${QOH99_RE_INC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/ddraw_wrapper"
    "${MINHOOK_DIR}/include"
    "${MINHOOK_DIR}/src"
    "${MINHOOK_DIR}/src/hde32"
)

target_compile_features(qoh99_netplay PRIVATE cxx_std_17)

target_compile_definitions(qoh99_netplay PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    set_property(TARGET qoh99_netplay PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(qoh99_netplay PRIVATE
        /W3
        /wd4100
        $<$<CONFIG:Debug>:/Od> $<$<CONFIG:Debug>:/Zi>
        $<$<CONFIG:Release>:/O2>
    )
    target_link_options(qoh99_netplay PRIVATE
        /MACHINE:X86 /DYNAMICBASE /NXCOMPAT
        "/MAP:$<TARGET_FILE_DIR:qoh99_netplay>/qoh99_netplay.map"
        $<$<CONFIG:Debug>:/DEBUG>
    )
    # dbghelp must stay dynamically loaded, never linked: a stub DbgHelp must not stop the DLL from loading.
    target_link_libraries(qoh99_netplay PRIVATE
        qoh99_re_model
        qoh99_platform
        qoh99_netplay_core
        qoh99_spectator
        qoh99_replay
        qoh99_content_identity
        qoh99_audio
        qoh99_input
        qoh99_training
        kernel32 user32 ws2_32 ole32 winmm dinput8 dxguid bcrypt)
else()
    target_compile_options(qoh99_netplay PRIVATE
        -m32 -Wall -Wno-unused-parameter
        $<$<CONFIG:Debug>:-O0> $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Release>:-O2>
    )
    target_link_options(qoh99_netplay PRIVATE -m32 -static -static-libgcc -static-libstdc++)
    target_link_libraries(qoh99_netplay PRIVATE
        qoh99_re_model qoh99_platform qoh99_netplay_core qoh99_spectator
        qoh99_replay qoh99_audio qoh99_input qoh99_training
        qoh99_content_identity
        kernel32 user32 ws2_32 ole32 winmm dinput8 dxguid bcrypt)
endif()

set_target_properties(qoh99_netplay PROPERTIES
    OUTPUT_NAME "qoh99_netplay" PREFIX "" SUFFIX ".dll")

add_executable(qoh99_opening_movie_tests
    src/replay/tests/test_replay_main.cpp
    src/game/tests/test_opening_movie.cpp
    src/game/tests/test_game_error_journal.cpp
    src/game/OpeningMovieCodec.cpp
    third_party/miniz/miniz.c
)
target_include_directories(qoh99_opening_movie_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz")
target_compile_features(qoh99_opening_movie_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_opening_movie_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_opening_movie_tests PRIVATE
    vfw32 winmm ole32 oleaut32 strmiids)
add_test(NAME qoh99_opening_movie COMMAND qoh99_opening_movie_tests)
set_tests_properties(qoh99_opening_movie PROPERTIES TIMEOUT 120)

add_executable(qoh99caster
    src/client/Main.cpp
    src/CrashDump.cpp
    src/CrashContext.cpp
    src/i18n/Tr.cpp
    src/client/AntivirusPreflight.cpp
    src/client/SessionLogArchive.cpp
    third_party/miniz/miniz.c
    src/client/Launcher.cpp
    src/client/BuildIdentity.cpp
    src/client/PublicIp.cpp
    src/client/ConsoleUi.cpp
    src/client/SdlUi.cpp
    src/client/RawInputCapture.cpp
    src/client/replay/ReplayParser.cpp
    src/client/replay/ReplayParserV2.cpp
    src/client/replay/ReplayWriter.cpp
    src/client/replay/ReplayWriterV2.cpp
    src/client/replay/ReplayCompatibility.cpp
    src/client/replay/ReplayCompatibilityV2.cpp
    src/client/replay/ReplayProductV2.cpp
    src/client/replay/ReplayCatalog.cpp
    src/client/replay/ReplaySetNaming.cpp
    src/client/replay/ReplayBattleLog.cpp
    src/client/replay/ReplayBattleLogFile.cpp
    src/client/replay/ReplayBattleStats.cpp
    src/client/replay/ReplayAnnotations.cpp
    src/client/replay/ReplayBrowserModel.cpp
    src/client/replay/ReplayBroker.cpp
    src/client/replay/ReplayBrokerV2.cpp
    src/client/replay/ReplayRecorderHost.cpp
    src/client/spectator/SpectatorSidecar.cpp
    src/client/spectator/ResilientSpectatorRuntime.cpp
    src/client/spectator/ResilientTopologyAuthority.cpp
    src/client/spectator/TopologyAuthoritySignature.cpp
    src/client/spectator/ResilientTopologyClient.cpp
    src/client/spectator/ReleasedTopologyControlV2.cpp
    src/client/spectator/ReleasedClientRootControl.cpp
    src/client/spectator/ResilientSpectatorNodeRuntime.cpp
    src/client/spectator/SpectatorRoute.cpp
    src/relay/RelayService.cpp
    src/relay/RelayServerCore.cpp
    src/relay/RelayServiceVerb.cpp
    src/tests/Tests.cpp
)
target_include_directories(qoh99caster PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${QOH99_RE_INC}"
)
target_compile_features(qoh99caster PRIVATE cxx_std_17)
target_compile_definitions(qoh99caster PRIVATE _CRT_SECURE_NO_WARNINGS)

set(QOH99_RES_OPPONENT_JOINED_SOUND 201)
set(QOH99_OPPONENT_JOINED_SOUND
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/opponent_joined.wav")
configure_file(src/client/CasterResources.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/CasterResources.rc" @ONLY)
target_sources(qoh99caster PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/CasterResources.rc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${QOH99_OPPONENT_JOINED_SOUND}")
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/generated/CasterResources.rc"
    PROPERTIES OBJECT_DEPENDS "${QOH99_OPPONENT_JOINED_SOUND}")
target_compile_definitions(qoh99caster PRIVATE
    QOH99_RES_OPPONENT_JOINED_SOUND=${QOH99_RES_OPPONENT_JOINED_SOUND})
target_link_libraries(qoh99caster PRIVATE
    qoh99net
    qoh99_platform
    qoh99_netplay_core
    qoh99_spectator
    qoh99_replay
    qoh99_content_identity
    qoh99_input
    ws2_32
    bcrypt
    wininet
    winmm
)
if(MSVC)
    set_property(TARGET qoh99caster PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99caster PRIVATE /MACHINE:X86)
else()
    target_compile_options(qoh99caster PRIVATE -m32)
    target_link_options(qoh99caster PRIVATE -m32 -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME qoh99_caster_selftests COMMAND qoh99caster --tests)
set_tests_properties(qoh99_caster_selftests PROPERTIES TIMEOUT 120)

add_executable(qoh99_runtime_profile_tests
    src/runtime/tests/test_runtime_profiles.cpp
    src/client/BuildIdentity.cpp
    src/training/NativeRecoveryQualification.cpp
)
target_include_directories(qoh99_runtime_profile_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${QOH99_RE_INC}"
)
target_compile_features(qoh99_runtime_profile_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_runtime_profile_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(qoh99_runtime_profile_tests PRIVATE
    qoh99_content_identity bcrypt)
if(MSVC)
    set_property(TARGET qoh99_runtime_profile_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_runtime_profile_tests PRIVATE /MACHINE:X86)
else()
    target_compile_options(qoh99_runtime_profile_tests PRIVATE -m32)
    target_link_options(qoh99_runtime_profile_tests PRIVATE
        -m32 -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME qoh99_runtime_profiles COMMAND qoh99_runtime_profile_tests)

add_executable(qoh99_content_identity_diagnostic
    tools/ContentIdentityDiagnostic.cpp
    src/client/BuildIdentity.cpp
)
target_include_directories(qoh99_content_identity_diagnostic PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${QOH99_RE_INC}"
)
target_compile_features(qoh99_content_identity_diagnostic PRIVATE cxx_std_17)
target_compile_definitions(qoh99_content_identity_diagnostic PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_content_identity_diagnostic PRIVATE
    qoh99_content_identity qoh99_platform bcrypt)
if(MSVC)
    set_property(TARGET qoh99_content_identity_diagnostic PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_content_identity_diagnostic PRIVATE /MACHINE:X86)
endif()

option(QOH99_BUILD_LEGACY_RELAY_SHIM
    "Build the deprecated standalone qoh99relay.exe forwarding shim" OFF)
if(QOH99_BUILD_LEGACY_RELAY_SHIM)
    add_executable(qoh99relay
        src/relay/RelayServer.cpp
        src/relay/RelayService.cpp
        src/relay/RelayServerCore.cpp
        src/relay/RelayServiceVerb.cpp
    )
    target_include_directories(qoh99relay PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(qoh99relay PRIVATE cxx_std_17)
    target_compile_definitions(qoh99relay PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(qoh99relay PRIVATE
        qoh99net
        qoh99_platform
        qoh99_netplay_core
        qoh99_spectator
        qoh99_replay
        ws2_32
        bcrypt
    )
    if(MSVC)
        set_property(TARGET qoh99relay PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(qoh99relay PRIVATE /W4 /permissive-)
        target_link_options(qoh99relay PRIVATE /MACHINE:X86)
    endif()
endif()

add_library(qoh99_audio STATIC
    src/audio/AudioTimeline.cpp
    src/audio/AudioReconciler.cpp
    src/audio/WaveMetadata.cpp
    src/audio/QohAudioRuntime.cpp
)
target_include_directories(qoh99_audio PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_audio PUBLIC cxx_std_17)
target_compile_definitions(qoh99_audio PUBLIC _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET qoh99_audio PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(QOH99_SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL3"
    CACHE PATH
    "Directory of the pinned SDL3 x86 artifact (include/ + lib/x86)")
set(QOH99_SDL3_DLL_SHA256
    "c68977dfc08f64b7bc845d04bbb01eb534f4ad52f80d7fa126fb4731185bcb63"
    CACHE STRING "Expected SHA-256 of the pinned SDL3.dll (3.2.14 x86)")
set(QOH99_INPUT_SDL_SOURCES "")
set(QOH99_SDL3_DLL "")
if(QOH99_SDL3_DIR AND EXISTS "${QOH99_SDL3_DIR}/lib/x86/SDL3.dll")
    set(QOH99_SDL3_DLL "${QOH99_SDL3_DIR}/lib/x86/SDL3.dll")
    set(QOH99_SDL3_IMPLIB "${QOH99_SDL3_DIR}/lib/x86/SDL3.lib")
elseif(QOH99_SDL3_DIR AND EXISTS "${QOH99_SDL3_DIR}/SDL3.dll")
    set(QOH99_SDL3_DLL "${QOH99_SDL3_DIR}/SDL3.dll")
    set(QOH99_SDL3_IMPLIB "${QOH99_SDL3_DIR}/SDL3.lib")
elseif(QOH99_SDL3_DIR)
    message(FATAL_ERROR
        "QOH99_SDL3_DIR=${QOH99_SDL3_DIR} has no SDL3.dll "
        "(expected lib/x86/SDL3.dll or SDL3.dll)")
endif()
if(QOH99_SDL3_DLL)
    if(NOT QOH99_SDL3_DLL_SHA256)
        message(FATAL_ERROR
            "QOH99_SDL3_DLL_SHA256 is required with QOH99_SDL3_DIR "
            "(the pin never floats)")
    endif()
    file(SHA256 "${QOH99_SDL3_DLL}" QOH99_SDL3_ACTUAL_SHA256)
    if(NOT QOH99_SDL3_ACTUAL_SHA256 STREQUAL QOH99_SDL3_DLL_SHA256)
        message(FATAL_ERROR
            "pinned SDL3.dll hash mismatch: expected "
            "${QOH99_SDL3_DLL_SHA256} got ${QOH99_SDL3_ACTUAL_SHA256}")
    endif()
    list(APPEND QOH99_INPUT_SDL_SOURCES src/input/SdlControllerBackend.cpp)
    message(STATUS "SDL3 controller backend pinned: ${QOH99_SDL3_DLL}")
endif()
add_library(qoh99_input STATIC
    src/input/BindingEvaluator.cpp
    src/input/ModeControlEvaluator.cpp
    src/input/ModeControlPolicy.cpp
    src/input/InputProfile.cpp
    src/input/InputCompiler.cpp
    src/input/DeviceIdentity.cpp
    src/input/KeyboardScanCodec.cpp
    src/input/InputWorkers.cpp
    src/input/DirectInput8KeyboardBackend.cpp
    src/input/DirectInput8ControllerBackend.cpp
    src/input/SdlLifecycle.cpp
    src/input/ControllerPreview.cpp
    ${QOH99_INPUT_SDL_SOURCES}
)
target_link_libraries(qoh99_input PUBLIC dinput8 dxguid)
if(QOH99_SDL3_DLL)
    target_include_directories(qoh99_input PUBLIC
        "${QOH99_SDL3_DIR}/include")
    target_compile_definitions(qoh99_input PUBLIC QOH99_WITH_SDL3)
    # Delay-loaded on purpose: a static import of a missing or stale SDL3.dll terminates the process before main().
    target_link_libraries(qoh99_input PUBLIC "${QOH99_SDL3_IMPLIB}"
        delayimp)
    target_link_options(qoh99_input INTERFACE "/DELAYLOAD:SDL3.dll")
endif()
target_include_directories(qoh99_input PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_input PUBLIC cxx_std_17)
target_compile_definitions(qoh99_input PUBLIC _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET qoh99_input PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(qoh99_network_tests
    src/netplay/tests/test_main.cpp
    src/netplay/tests/test_zero_timeout.cpp
    src/netplay/tests/test_wire.cpp
    src/netplay/tests/test_core.cpp
    src/netplay/tests/test_rematch_prompt.cpp
    src/netplay/tests/test_rematch_theater.cpp
    src/netplay/tests/test_rollback_session.cpp
    src/netplay/tests/test_pacing_budget.cpp
    src/netplay/tests/test_route_metrics.cpp
    src/netplay/tests/test_schedule_policy.cpp
    src/netplay/tests/test_run_state.cpp
    src/netplay/tests/test_route_selection.cpp
    src/netplay/tests/test_route_candidates.cpp
    src/netplay/tests/test_build_identity.cpp
    src/netplay/tests/test_build_signatures.cpp
    src/netplay/tests/test_udp_loopback.cpp
    src/netplay/tests/test_relay_protocol.cpp
    src/netplay/tests/test_relay_client.cpp
    src/netplay/tests/test_relay_route.cpp
    src/netplay/tests/test_runtime_session_ui.cpp
    src/game/tests/test_direct_boot_training.cpp
    src/game/tests/test_native_hud_policy.cpp
    src/game/tests/test_state_proof_digest.cpp
    src/relay/tests/test_relay_server_core.cpp
    src/relay/tests/test_relay_service.cpp
    src/relay/RelayServerCore.cpp
    src/relay/RelayService.cpp
    src/relay/RelayServiceVerb.cpp
    src/client/BuildIdentity.cpp
    src/i18n/Tr.cpp
    src/client/ConsoleUi.cpp
    src/client/tests/test_launcher_paths.cpp
    src/client/tests/test_fcommand_text.cpp
    src/client/tests/test_sdl_ui_model.cpp
    src/client/spectator/tests/test_spectator_sidecar.cpp
    src/client/spectator/tests/test_spectator_route.cpp
    src/client/spectator/tests/test_resilient_spectator_runtime.cpp
    src/spectator/tests/test_spectator_v4.cpp
    src/client/spectator/SpectatorSidecar.cpp
    src/client/spectator/ResilientSpectatorRuntime.cpp
    src/client/spectator/ResilientTopologyAuthority.cpp
    src/client/spectator/TopologyAuthoritySignature.cpp
    src/client/spectator/ResilientSpectatorNodeRuntime.cpp
    src/client/spectator/SpectatorReplaySaveAdapter.cpp
    src/client/spectator/SpectatorRoute.cpp
    src/client/SdlUi.cpp
    src/client/RawInputCapture.cpp
    src/net/tests/test_addressing.cpp
    src/net/tests/test_autopunch.cpp
    src/net/tests/test_control_link.cpp
    src/net/tests/test_port_mapper.cpp
    src/net/Autopunch.cpp
    src/net/AutopunchSession.cpp
    src/net/AutopunchPeerSet.cpp
    src/net/PortMapper.cpp
    src/game/replay/tests/test_replay_runtime.cpp
    src/game/replay/tests/test_replay_ipc_worker.cpp
    src/game/replay/tests/test_replay_playlist_runtime.cpp
    src/game/replay/ReplayRuntime.cpp
    src/game/replay/ReplayTimelineSource.cpp
    src/game/replay/ReplayIpcWorker.cpp
    src/game/spectator/SpectatorPublisher.cpp
    src/game/spectator/SpectatorPublisherV4.cpp
    src/game/spectator/SpectatorReplayPublisherV4.cpp
    src/platform/tests/test_platform.cpp
    src/audio/tests/test_wave_metadata.cpp
    src/audio/tests/test_audio_timeline.cpp
    src/audio/tests/test_audio_reconciler.cpp
    src/audio/tests/test_bgm_boundary_guard.cpp
    src/audio/tests/test_qoh_audio_runtime.cpp
    src/input/tests/test_actions.cpp
    src/input/tests/test_profile.cpp
    src/input/tests/test_bindings.cpp
    src/input/tests/test_identity.cpp
    src/input/tests/test_keyboard_scan_codec.cpp
    src/input/tests/test_worker_snapshot.cpp
    src/input/tests/test_session_controls.cpp
)
target_include_directories(qoh99_network_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests"
    "${QOH99_RE_INC}"
)
target_compile_features(qoh99_network_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_network_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    QOH99_BUILT_NETPLAY_DLL="$<TARGET_FILE:qoh99_netplay>"
)
add_dependencies(qoh99_network_tests qoh99_netplay)
if(TARGET ddraw)
    add_dependencies(qoh99_network_tests ddraw)
endif()
target_link_libraries(qoh99_network_tests PRIVATE
    qoh99_netplay_core
    qoh99_platform
    qoh99_spectator
    qoh99_replay
    qoh99_content_identity
    qoh99_audio
    qoh99_input
    qoh99_training
    qoh99net
    ws2_32
    bcrypt
)
if(MSVC)
    set_property(TARGET qoh99_network_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_network_tests PRIVATE /MACHINE:X86)
else()
    target_compile_options(qoh99_network_tests PRIVATE -m32)
    target_link_options(qoh99_network_tests PRIVATE
        -m32 -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME qoh99_network_p0 COMMAND qoh99_network_tests)

add_executable(qoh99_crash_context_tests
    src/crash/tests/test_crash_context.cpp
    src/CrashContext.cpp
)
target_include_directories(qoh99_crash_context_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(qoh99_crash_context_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_crash_context_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
if(MSVC)
    set_property(TARGET qoh99_crash_context_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_crash_context_tests PRIVATE /MACHINE:X86)
else()
    target_compile_options(qoh99_crash_context_tests PRIVATE -m32)
    target_link_options(qoh99_crash_context_tests PRIVATE
        -m32 -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME qoh99_crash_context COMMAND qoh99_crash_context_tests)

add_executable(qoh99_replay_core_tests
    src/replay/tests/test_replay_main.cpp
    src/replay/tests/test_replay_codec.cpp
    src/replay/tests/test_replay_v2_contract.cpp
    src/replay/tests/test_replay_simulation_contract_v2.cpp
    src/replay/tests/test_replay_compatibility_identity_v3.cpp
    src/replay/tests/test_character_catalog.cpp
    src/replay/tests/test_replay_timeline.cpp
    src/replay/tests/test_replay_recording_coordinator.cpp
    src/replay/tests/test_replay_publisher_queue_v2.cpp
    src/replay/tests/test_replay_recorder_queue.cpp
    src/replay/tests/test_replay_playback.cpp
    src/replay/tests/test_replay_playback_v2.cpp
    src/replay/tests/test_replay_seek_cache.cpp
    src/replay/tests/test_replay_checkpoint.cpp
    src/replay/tests/test_replay_takeover.cpp
    src/client/replay/tests/test_replay_parser.cpp
    src/client/replay/tests/test_replay_writer_recovery.cpp
    src/client/replay/tests/test_replay_compatibility.cpp
    src/client/replay/tests/test_replay_catalog.cpp
    src/client/replay/tests/test_replay_set_naming.cpp
    src/client/replay/tests/test_replay_battle_log.cpp
    src/client/replay/tests/test_replay_battle_log_file.cpp
    src/client/replay/tests/test_replay_battle_stats.cpp
    src/client/replay/tests/test_replay_browser_model.cpp
    src/client/replay/tests/test_replay_incomplete_playback.cpp
    src/client/replay/tests/test_replay_fuzz.cpp
    src/client/replay/tests/test_replay_storage_v2.cpp
    src/client/replay/tests/test_replay_broker_v2.cpp
    src/client/replay/ReplayParser.cpp
    src/client/replay/ReplayParserV2.cpp
    src/client/replay/ReplayWriter.cpp
    src/client/replay/ReplayWriterV2.cpp
    src/client/replay/ReplayRecovery.cpp
    src/client/replay/ReplayRecoveryV2.cpp
    src/client/replay/ReplayCompatibility.cpp
    src/client/replay/ReplayCompatibilityV2.cpp
    src/client/replay/ReplayProductV2.cpp
    src/client/replay/ReplayCatalog.cpp
    src/client/replay/ReplaySetNaming.cpp
    src/client/replay/ReplayBattleLog.cpp
    src/client/replay/ReplayBattleLogFile.cpp
    src/client/replay/ReplayBattleStats.cpp
    src/client/replay/ReplayAnnotations.cpp
    src/client/replay/ReplayBrowserModel.cpp
    src/client/replay/ReplayBroker.cpp
    src/client/replay/ReplayBrokerV2.cpp
    src/client/replay/ReplayRecorderHost.cpp
)
target_include_directories(qoh99_replay_core_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests"
)
target_compile_features(qoh99_replay_core_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_replay_core_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_replay_core_tests PRIVATE
    qoh99_replay qoh99_platform)
if(MSVC)
    set_property(TARGET qoh99_replay_core_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_replay_core_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_replay_core COMMAND qoh99_replay_core_tests)
set_tests_properties(qoh99_replay_core PROPERTIES TIMEOUT 300)

add_executable(qoh99_content_identity_tests
    src/replay/tests/test_replay_main.cpp
    src/content/tests/test_simulation_content_identity.cpp
    src/client/BuildIdentity.cpp
)
target_include_directories(qoh99_content_identity_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests"
    "${QOH99_RE_INC}")
target_compile_features(qoh99_content_identity_tests PRIVATE cxx_std_17)
set(QOH99_NOVEMBER_GAME_ROOT
    ""
    CACHE PATH "Complete November installation for content identity tests")
set(QOH99_DECEMBER_GAME_ROOT ""
    CACHE PATH "Complete December installation for content identity tests")
target_compile_definitions(qoh99_content_identity_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    QOH99_NOVEMBER_GAME_ROOT="${QOH99_NOVEMBER_GAME_ROOT}"
    QOH99_DECEMBER_GAME_ROOT="${QOH99_DECEMBER_GAME_ROOT}")
target_link_libraries(qoh99_content_identity_tests PRIVATE
    qoh99_content_identity qoh99_replay bcrypt)
if(MSVC)
    set_property(TARGET qoh99_content_identity_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_content_identity_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_content_identity COMMAND qoh99_content_identity_tests)
set_tests_properties(qoh99_content_identity PROPERTIES TIMEOUT 180)

if(WIN32)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME qoh99_local_autopunch_semantics
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests/test_local_autopunch_relay.py")
        set_tests_properties(qoh99_local_autopunch_semantics PROPERTIES TIMEOUT 30)
        add_test(
            NAME qoh99_live_e2e_lifecycle
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests/test_live_game_e2e_lifecycle.py")
        set_tests_properties(qoh99_live_e2e_lifecycle PROPERTIES TIMEOUT 30)
        add_test(
            NAME qoh99_relay_live_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/relay/tests/relay_live_smoke.py"
                "$<TARGET_FILE:qoh99caster>")
        set_tests_properties(qoh99_relay_live_smoke PROPERTIES TIMEOUT 30)
        set(QOH99_AUTO_ROUTE_GAME_EXE "" CACHE FILEPATH
            "Explicit full-install qoh99.exe for qoh99_auto_route_smoke")
        add_test(
            NAME qoh99_auto_route_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/relay/tests/auto_route_smoke.py"
                "$<TARGET_FILE:qoh99caster>"
                "$<TARGET_FILE:qoh99caster>"
                "${QOH99_AUTO_ROUTE_GAME_EXE}"
                "$<TARGET_FILE:qoh99_netplay>")
        set_tests_properties(qoh99_auto_route_smoke PROPERTIES TIMEOUT 240)

        set(QOH99_LIVE_GAME_EXE
            ""
            CACHE FILEPATH "qoh99.exe in a complete game install for live E2E tests")
        set(QOH99_LIVE_GAME_E2E_OUTPUT_ROOT
            "${CMAKE_BINARY_DIR}/live_game_e2e"
            CACHE PATH "Marker-owned output root for live game E2E artifacts")
        option(QOH99_ENABLE_LIVE_GAME_E2E
            "Register the two-process live game regressions" ON)
        get_filename_component(
            QOH99_LIVE_GAME_DIR "${QOH99_LIVE_GAME_EXE}" DIRECTORY)
        if(QOH99_ENABLE_LIVE_GAME_E2E
           AND TARGET ddraw
           AND EXISTS "${QOH99_LIVE_GAME_EXE}"
           AND EXISTS "${QOH99_LIVE_GAME_DIR}/System/QOHcnf.key")
            add_executable(qoh99_e2e_agent
                src/netplay/tests/agent/qoh99_e2e_agent.cpp)
            target_compile_features(qoh99_e2e_agent PRIVATE cxx_std_17)
            execute_process(
                COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE QOH99_AGENT_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(NOT QOH99_AGENT_GIT_HASH)
                set(QOH99_AGENT_GIT_HASH "no-git")
            endif()
            target_compile_definitions(qoh99_e2e_agent PRIVATE
                QOH99_AGENT_GIT_HASH="${QOH99_AGENT_GIT_HASH}"
                _CRT_SECURE_NO_WARNINGS
                WIN32_LEAN_AND_MEAN
                NOMINMAX
                UNICODE
                _UNICODE)
            target_link_libraries(qoh99_e2e_agent PRIVATE ws2_32)
            if(MSVC)
                set_property(TARGET qoh99_e2e_agent PROPERTY
                    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                target_link_options(qoh99_e2e_agent PRIVATE
                    /MACHINE:X86 /ENTRY:wmainCRTStartup)
            endif()

            if(Python3_Interpreter_FOUND)
                add_test(
                    NAME qoh99_agent_protocol
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests/test_e2e_agent_protocol.py")
                set_tests_properties(qoh99_agent_protocol PROPERTIES
                    TIMEOUT 180
                    ENVIRONMENT
                        "QOH99_E2E_AGENT=$<TARGET_FILE:qoh99_e2e_agent>;QOH99_CASTER=$<TARGET_FILE:qoh99caster>")
            endif()

            function(qoh99_add_live_game_test test_name route variant)
                set(spectator_args)
                set(extra_args)
                foreach(optional_arg IN LISTS ARGN)
                    if(optional_arg STREQUAL "spectator")
                        list(APPEND spectator_args --spectator)
                    else()
                        list(APPEND extra_args "${optional_arg}")
                    endif()
                endforeach()
                add_test(
                    NAME "${test_name}"
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests/live_game_e2e.py"
                        --route "${route}"
                        --config-variant "${variant}"
                        --caster "$<TARGET_FILE:qoh99caster>"
                        --relay "$<TARGET_FILE:qoh99caster>"
                        --game "${QOH99_LIVE_GAME_EXE}"
                        --dll "$<TARGET_FILE:qoh99_netplay>"
                        --wrapper "$<TARGET_FILE:ddraw>"
                        --dll-map "$<TARGET_FILE_DIR:qoh99_netplay>/qoh99_netplay.map"
                        --output-root "${QOH99_LIVE_GAME_E2E_OUTPUT_ROOT}"
                        --gameplay-ticks 300
                        --prune-on-pass
                        ${spectator_args}
                        ${extra_args})
                set_tests_properties("${test_name}" PROPERTIES
                    TIMEOUT 300
                    RUN_SERIAL TRUE
                    RESOURCE_LOCK qoh99_live_game
                    LABELS "live;e2e;game")
            endfunction()

            set(QOH99_LIVE_GAME_REMOTE_HOST "" CACHE STRING
                "ssh target hosting the remote role of the distributed live game lanes")
            set(QOH99_LIVE_GAME_CONTROLLER_ADDRESS "" CACHE STRING
                "This machine's LAN address as the remote peer must reach it")
            set(QOH99_LIVE_GAME_REMOTE_MIN_FPS "50" CACHE STRING
                "Log-derived visible-cadence floor for the remote role")
            if(QOH99_LIVE_GAME_REMOTE_HOST AND QOH99_LIVE_GAME_CONTROLLER_ADDRESS)
                function(qoh99_add_distributed_live_game_test
                         test_name route game_port relay_port)
                    add_test(
                        NAME "${test_name}"
                        COMMAND "${Python3_EXECUTABLE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests/live_game_e2e.py"
                            --route "${route}"
                            --config-variant identical
                            --caster "$<TARGET_FILE:qoh99caster>"
                            --relay "$<TARGET_FILE:qoh99caster>"
                            --game "${QOH99_LIVE_GAME_EXE}"
                            --dll "$<TARGET_FILE:qoh99_netplay>"
                            --wrapper "$<TARGET_FILE:ddraw>"
                            --dll-map "$<TARGET_FILE_DIR:qoh99_netplay>/qoh99_netplay.map"
                            --agent "$<TARGET_FILE:qoh99_e2e_agent>"
                            --output-root "${QOH99_LIVE_GAME_E2E_OUTPUT_ROOT}"
                            --gameplay-ticks 300
                            --prune-on-pass
                            --role-host "client=${QOH99_LIVE_GAME_REMOTE_HOST}"
                            --controller-address "${QOH99_LIVE_GAME_CONTROLLER_ADDRESS}"
                            --game-port "${game_port}"
                            --relay-port "${relay_port}"
                            --full-match-rematch
                            --visible-games
                            --network-profile stable-120
                            --host-delay 2 --host-rollback 8
                            --client-delay 2 --client-rollback 8
                            --returned-charselect-idle-ticks 180
                            --minimum-canonical-fps 58
                            --remote-minimum-canonical-fps
                                "${QOH99_LIVE_GAME_REMOTE_MIN_FPS}"
                            --remote-barrier-slack-ms 4000
                            --route-p50-agreement-us 3000
                            --evidence-staleness-budget 5.0)
                    set_tests_properties("${test_name}" PROPERTIES
                        TIMEOUT 1800
                        RUN_SERIAL TRUE
                        RESOURCE_LOCK qoh99_live_game
                        LABELS "live;e2e;game;distributed")
                endfunction()

                qoh99_add_distributed_live_game_test(
                    qoh99_game_e2e_dist_120_d2_r8_rematch_idle
                    punched-direct 46200 46400)
                qoh99_add_distributed_live_game_test(
                    qoh99_game_e2e_dist_relay_120_d2_r8_rematch_idle
                    relay 46210 46410)
            endif()

            add_test(
                NAME qoh99_training_live_e2e
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/netplay/tests/live_training_e2e.py"
                    --caster "$<TARGET_FILE:qoh99caster>"
                    --game "${QOH99_LIVE_GAME_EXE}"
                    --dll "$<TARGET_FILE:qoh99_netplay>"
                    --wrapper "$<TARGET_FILE:ddraw>"
                    --dll-map "$<TARGET_FILE_DIR:qoh99_netplay>/qoh99_netplay.map"
                    --output-root "${CMAKE_BINARY_DIR}/live_training_e2e"
                    --admitted-steps 480
                    --prune-game-clone-on-pass)
            set_tests_properties(qoh99_training_live_e2e PROPERTIES
                TIMEOUT 300
                RUN_SERIAL TRUE
                RESOURCE_LOCK qoh99_live_game
                CONFIGURATIONS Release
                LABELS "live;e2e;game;training")

            qoh99_add_live_game_test(
                qoh99_game_e2e_punched_direct punched-direct identical)
            qoh99_add_live_game_test(
                qoh99_game_e2e_relay relay identical)
            qoh99_add_live_game_test(
                qoh99_game_e2e_punched_direct_config_stress
                punched-direct config-stress)
            qoh99_add_live_game_test(
                qoh99_game_e2e_punched_direct_inactive_p4_hash_isolation
                punched-direct identical
                --layout-command-mutation inactive-p4)
            qoh99_add_live_game_test(
                qoh99_game_e2e_punched_direct_active_p2_hash_guard
                punched-direct identical
                --layout-command-mutation active-p2)
            qoh99_add_live_game_test(
                qoh99_game_e2e_relay_spectator_inactive_p4_hash_isolation
                relay identical spectator
                --layout-command-mutation inactive-p4)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_forced_direct_authority
                relay identical spectator
                --spectator-route direct
                --spectator-input-mash)
            qoh99_add_live_game_test(
                qoh99_game_e2e_released_topology_direct
                relay identical spectator
                --spectator-route direct
                --spectator-topology
                --spectator-input-mash)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_forced_punch
                relay identical spectator
                --spectator-route punch)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_forced_relay
                relay identical spectator
                --spectator-route relay)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_two_simultaneous_viewers
                relay identical spectator
                --spectator-count 2)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_mixed_direct_punch
                relay identical spectator
                --spectator-count 2
                --spectator-routes direct,punch)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_two_forced_punch
                relay identical spectator
                --spectator-count 2
                --spectator-routes punch,punch)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_active_catchup
                relay identical spectator
                --spectator-count 2
                --spectator-route direct
                --spectator-active-catchup-pause-seconds 12
                --gameplay-ticks 3000)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_sibling_disconnect
                relay identical spectator
                --spectator-count 2
                --spectator-routes direct,punch
                --spectator-sibling-disconnect
                --gameplay-ticks 600)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_forced_direct_capacity_readmit
                relay identical spectator
                --spectator-route direct
                --spectator-capacity-readmit)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_forced_relay_capacity_readmit
                relay identical spectator
                --spectator-route relay
                --spectator-capacity-readmit)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_midgame_late_join
                relay identical spectator
                --spectator-join-after-seconds 4
                --gameplay-ticks 600)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_set_seek_latest
                relay identical spectator
                --spectator-join-after-seconds 6
                --spectator-latest
                --gameplay-ticks 900)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_f1_return_survival
                relay identical spectator
                --f1-return-at-frame 900
                --gameplay-ticks 600)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_unavailable_does_not_harm_players
                relay identical
                --spectator-negative unavailable)
            qoh99_add_live_game_test(
                qoh99_game_e2e_spectator_retained_late_join_after_source_exit
                relay identical spectator
                --spectator-postgame-retain-ms 30000)
            set_tests_properties(
                qoh99_game_e2e_spectator_forced_direct_authority
                qoh99_game_e2e_released_topology_direct
                qoh99_game_e2e_spectator_forced_punch
                qoh99_game_e2e_spectator_forced_relay
                qoh99_game_e2e_spectator_two_simultaneous_viewers
                qoh99_game_e2e_spectator_mixed_direct_punch
                qoh99_game_e2e_spectator_two_forced_punch
                qoh99_game_e2e_spectator_active_catchup
                qoh99_game_e2e_spectator_sibling_disconnect
                qoh99_game_e2e_spectator_forced_direct_capacity_readmit
                qoh99_game_e2e_spectator_forced_relay_capacity_readmit
                qoh99_game_e2e_spectator_midgame_late_join
                qoh99_game_e2e_spectator_set_seek_latest
                qoh99_game_e2e_spectator_unavailable_does_not_harm_players
                qoh99_game_e2e_spectator_retained_late_join_after_source_exit
                PROPERTIES
                    TIMEOUT 420
                    LABELS "live;e2e;game;spectator;caster")
            qoh99_add_live_game_test(
                qoh99_game_e2e_punched_direct_spectator_impairment_stats
                punched-direct identical spectator
                --network-profile stable-low
                --network-seed 0x51A7E5EE)
            qoh99_add_live_game_test(
                qoh99_game_e2e_runtime_delay_stable_low
                relay identical spectator
                --full-match-rematch
                --force-ko-after-frames 180
                --host-delay 1 --host-rollback 6
                --client-delay 2 --client-rollback 8
                --host-delay-script 30:4,120:1
                --client-delay-script 45:0,135:3
                --network-profile stable-low
                --network-seed 0xD31A651B
                --minimum-canonical-fps 59)
            qoh99_add_live_game_test(
                qoh99_game_e2e_runtime_delay_names
                relay identical spectator
                --full-match-rematch
                --force-ko-after-frames 180
                --host-delay 1 --host-rollback 8
                --client-delay 2 --client-rollback 10
                --host-delay-script 30:4,120:1
                --client-delay-script 45:0,135:3
                --network-profile jitter-low
                --network-seed 0xD31A6518
                --minimum-canonical-fps 59)
            qoh99_add_live_game_test(
                qoh99_game_e2e_runtime_delay_stable_high
                relay identical spectator
                --full-match-rematch
                --force-ko-after-frames 180
                --host-delay 7 --host-rollback 12
                --client-delay 8 --client-rollback 15
                --host-delay-script 30:10,120:7
                --client-delay-script 45:5,135:9
                --network-profile stable-high
                --network-seed 0xD31A6519
                --minimum-canonical-fps 59)
            qoh99_add_live_game_test(
                qoh99_game_e2e_asymmetric_character_voices
                relay identical
                --full-match-rematch
                --require-character-switch
                --game-count 2
                --force-ko-after-frames 180
                --host-delay 2 --host-rollback 8
                --client-delay 2 --client-rollback 8
                --network-profile stable-120
                --network-seed 0xD31A6527
                --split-game-affinity
                --strip-character-voices client
                --p1-character-cursor 1 --p2-character-cursor 13)
            set_tests_properties(
                qoh99_game_e2e_asymmetric_character_voices
                PROPERTIES TIMEOUT 600)
            qoh99_add_live_game_test(
                qoh99_game_e2e_cserika_aliased_framedata
                relay identical
                --full-match-rematch
                --require-character-switch
                --game-count 2
                --force-ko-after-frames 180
                --host-delay 2 --host-rollback 8
                --client-delay 2 --client-rollback 8
                --network-profile stable-120
                --network-seed 0xD31A6528
                --split-game-affinity
                --p1-character-cursor 10 --p1-character-alternate 1
                --p2-character-cursor 13)
            set_tests_properties(
                qoh99_game_e2e_cserika_aliased_framedata
                PROPERTIES TIMEOUT 600)
            qoh99_add_live_game_test(
                qoh99_game_e2e_punched_direct_five_games_char_switch
                punched-direct identical spectator
                --full-match-rematch
                --game-count 5
                --require-character-switch
                --force-ko-after-frames 180
                --host-delay 2 --host-rollback 8
                --client-delay 2 --client-rollback 8
                --network-profile stable-120
                --network-seed 0xD31A6522
                --split-game-affinity
                --minimum-canonical-fps 59)
            set_tests_properties(
                qoh99_game_e2e_punched_direct_five_games_char_switch
                PROPERTIES TIMEOUT 900)
            qoh99_add_live_game_test(
                qoh99_game_e2e_stable_120_d2_r8_rematch_idle
                relay identical
                --full-match-rematch
                --force-ko-after-frames 180
                --host-delay 2 --host-rollback 8
                --client-delay 2 --client-rollback 8
                --network-profile stable-120
                --network-seed 0xD31A6520
                --returned-charselect-idle-ticks 180
                --split-game-affinity
                --minimum-canonical-fps 59)
            qoh99_add_live_game_test(
                qoh99_game_e2e_stable_120_d2_r8_spectator_rematch_idle
                relay identical spectator
                --full-match-rematch
                --force-ko-after-frames 180
                --host-delay 2 --host-rollback 8
                --client-delay 2 --client-rollback 8
                --network-profile stable-120
                --network-seed 0xD31A6521
                --returned-charselect-idle-ticks 180
                --split-game-affinity
                --minimum-canonical-fps 59)
            qoh99_add_live_game_test(
                qoh99_game_e2e_stable_250_d2_r12
                relay identical
                --full-match-rematch
                --force-ko-after-frames 180
                --host-delay 2 --host-rollback 12
                --client-delay 2 --client-rollback 12
                --network-profile stable-250
                --network-seed 0xD31A6523
                --split-game-affinity
                --minimum-canonical-fps 59)
            qoh99_add_live_game_test(
                qoh99_game_e2e_stable_300_d2_r8_underbuffered_control
                relay identical
                --host-delay 2 --host-rollback 8
                --client-delay 2 --client-rollback 8
                --network-profile stable-300
                --network-seed 0xD31A6524
                --gameplay-ticks 600
                --split-game-affinity
                --expect-underbuffered)
            qoh99_add_live_game_test(
                qoh99_game_e2e_asym_updown
                relay identical
                --host-delay 2 --host-rollback 8
                --client-delay 2 --client-rollback 8
                --network-profile asym-updown
                --network-seed 0xD31A6525
                --gameplay-ticks 600
                --split-game-affinity
                --expect-asymmetric-path updown)
            qoh99_add_live_game_test(
                qoh99_game_e2e_asym_downup
                relay identical
                --host-delay 2 --host-rollback 8
                --client-delay 2 --client-rollback 8
                --network-profile asym-downup
                --network-seed 0xD31A6526
                --gameplay-ticks 600
                --split-game-affinity
                --expect-asymmetric-path downup)
            qoh99_add_live_game_test(
                qoh99_game_e2e_runtime_delay_jitter_high
                relay identical spectator
                --full-match-rematch
                --force-ko-after-frames 180
                --host-delay 7 --host-rollback 12
                --client-delay 9 --client-rollback 15
                --host-delay-script 30:10,120:7
                --client-delay-script 45:6,135:10
                --network-profile jitter-high
                --network-seed 0xD31A651C
                --minimum-canonical-fps 59)
            qoh99_add_live_game_test(
                qoh99_game_e2e_runtime_delay_packet_loss
                relay identical spectator
                --full-match-rematch
                --force-ko-after-frames 180
                --host-delay 2 --host-rollback 12
                --client-delay 4 --client-rollback 15
                --host-delay-script 30:5,120:2
                --client-delay-script 45:2,135:5
                --network-profile packet-loss
                --network-seed 0xD31A651A
                --minimum-canonical-fps 59)
            set_tests_properties(
                qoh99_game_e2e_runtime_delay_stable_low
                qoh99_game_e2e_runtime_delay_names
                qoh99_game_e2e_runtime_delay_stable_high
                qoh99_game_e2e_stable_120_d2_r8_rematch_idle
                qoh99_game_e2e_stable_120_d2_r8_spectator_rematch_idle
                qoh99_game_e2e_stable_250_d2_r12
                qoh99_game_e2e_stable_300_d2_r8_underbuffered_control
                qoh99_game_e2e_asym_updown
                qoh99_game_e2e_asym_downup
                qoh99_game_e2e_runtime_delay_jitter_high
                qoh99_game_e2e_runtime_delay_packet_loss
                PROPERTIES TIMEOUT 600)
            if(EXISTS "${QOH99_LIVE_GAME_DIR}/System/QOHcnf.key.bak")
                qoh99_add_live_game_test(
                    qoh99_game_e2e_punched_direct_config_isolation
                    punched-direct bak)
                qoh99_add_live_game_test(
                    qoh99_game_e2e_relay_config_isolation relay bak)
                qoh99_add_live_game_test(
                    qoh99_game_e2e_spectator_punched_direct_config_isolation
                    punched-direct bak spectator)
            endif()
        else()
            message(STATUS
                "Live qoh99 game E2E tests disabled or complete install not found")
        endif()
    endif()
endif()

add_executable(qoh99_spectator_tests
    src/spectator/tests/test_spectator.cpp
)
target_include_directories(qoh99_spectator_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_spectator_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_spectator_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_spectator_tests PRIVATE
    qoh99_spectator
    qoh99_netplay_core)
if(MSVC)
    set_property(TARGET qoh99_spectator_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_spectator_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_spectator_stack COMMAND qoh99_spectator_tests)

add_executable(qoh99_resilient_spectator_tests
    src/spectator/tests/test_resilient_spectator.cpp
)
target_include_directories(qoh99_resilient_spectator_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_spectator_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_spectator_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_resilient_spectator_tests PRIVATE
    qoh99_spectator)
if(MSVC)
    set_property(TARGET qoh99_resilient_spectator_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_spectator_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_spectator
         COMMAND qoh99_resilient_spectator_tests)

add_executable(qoh99_resilient_spectator_process_tests
    src/spectator/tests/test_resilient_spectator_process.cpp
)
target_include_directories(qoh99_resilient_spectator_process_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_spectator_process_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_spectator_process_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_resilient_spectator_process_tests PRIVATE
    qoh99_spectator)
if(MSVC)
    set_property(TARGET qoh99_resilient_spectator_process_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_spectator_process_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_spectator_process
         COMMAND qoh99_resilient_spectator_process_tests)

add_executable(qoh99_resilient_spectator_local_process_tests
    src/client/spectator/tests/test_resilient_spectator_local_process.cpp
    src/client/spectator/ResilientSpectatorRuntime.cpp
)
target_include_directories(qoh99_resilient_spectator_local_process_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_spectator_local_process_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_spectator_local_process_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_resilient_spectator_local_process_tests PRIVATE
    qoh99_spectator
    ws2_32)
if(MSVC)
    set_property(TARGET qoh99_resilient_spectator_local_process_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_spectator_local_process_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_spectator_local_process
         COMMAND qoh99_resilient_spectator_local_process_tests)
set_tests_properties(qoh99_resilient_spectator_local_process PROPERTIES TIMEOUT 30)

add_executable(qoh99_resilient_topology_authority_tests
    src/client/spectator/tests/test_resilient_topology_authority.cpp
    src/client/spectator/ResilientTopologyAuthority.cpp
    src/client/spectator/TopologyAuthoritySignature.cpp
    src/client/spectator/ResilientSpectatorRuntime.cpp
)

add_executable(qoh99_released_client_root_control_tests
    src/client/spectator/tests/test_released_client_root_control.cpp
    src/client/spectator/ReleasedClientRootControl.cpp
)
target_include_directories(qoh99_released_client_root_control_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_released_client_root_control_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_released_client_root_control_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_released_client_root_control_tests PRIVATE
    qoh99_spectator
    ws2_32
    bcrypt)
if(MSVC)
    set_property(TARGET qoh99_released_client_root_control_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_released_client_root_control_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_released_client_root_control
         COMMAND qoh99_released_client_root_control_tests)
set_tests_properties(qoh99_released_client_root_control PROPERTIES TIMEOUT 30)
target_include_directories(qoh99_resilient_topology_authority_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_topology_authority_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_topology_authority_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_resilient_topology_authority_tests PRIVATE
    qoh99_spectator
    ws2_32
    bcrypt)
if(MSVC)
    set_property(TARGET qoh99_resilient_topology_authority_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_topology_authority_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_topology_authority
         COMMAND qoh99_resilient_topology_authority_tests)
set_tests_properties(qoh99_resilient_topology_authority PROPERTIES TIMEOUT 30)

add_executable(qoh99_topology_authority_signature_tests
    src/client/spectator/tests/test_topology_authority_signature.cpp
    src/client/spectator/TopologyAuthoritySignature.cpp
)
target_include_directories(qoh99_topology_authority_signature_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_topology_authority_signature_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_topology_authority_signature_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_topology_authority_signature_tests PRIVATE bcrypt)
if(MSVC)
    set_property(TARGET qoh99_topology_authority_signature_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_topology_authority_signature_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_topology_authority_signature
         COMMAND qoh99_topology_authority_signature_tests)
set_tests_properties(qoh99_topology_authority_signature PROPERTIES TIMEOUT 30)

add_executable(qoh99_resilient_topology_client_tests
    src/client/spectator/tests/test_resilient_topology_client.cpp
    src/client/spectator/ResilientTopologyClient.cpp
)
target_include_directories(qoh99_resilient_topology_client_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_topology_client_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_topology_client_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_resilient_topology_client_tests PRIVATE
    qoh99_spectator)
if(MSVC)
    set_property(TARGET qoh99_resilient_topology_client_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_topology_client_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_topology_client
         COMMAND qoh99_resilient_topology_client_tests)
set_tests_properties(qoh99_resilient_topology_client PROPERTIES TIMEOUT 30)

add_executable(qoh99_resilient_topology_authority_process_tests
    src/client/spectator/tests/test_resilient_topology_authority_process.cpp
    src/client/spectator/SpectatorSidecar.cpp
    src/client/spectator/ResilientSpectatorRuntime.cpp
    src/client/spectator/ResilientTopologyAuthority.cpp
    src/client/spectator/TopologyAuthoritySignature.cpp
    src/client/spectator/SpectatorRoute.cpp
)
target_include_directories(qoh99_resilient_topology_authority_process_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_topology_authority_process_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_topology_authority_process_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_resilient_topology_authority_process_tests PRIVATE
    qoh99net
    qoh99_platform
    qoh99_netplay_core
    qoh99_spectator
    qoh99_replay
    qoh99_content_identity
    ws2_32
    bcrypt)
if(MSVC)
    set_property(TARGET qoh99_resilient_topology_authority_process_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_topology_authority_process_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_topology_authority_process
         COMMAND qoh99_resilient_topology_authority_process_tests)
set_tests_properties(qoh99_resilient_topology_authority_process PROPERTIES TIMEOUT 60)

add_executable(qoh99_resilient_topology_client_process_tests
    src/client/spectator/tests/test_resilient_topology_client_process.cpp
    src/client/spectator/ReleasedClientRootControl.cpp
    src/client/spectator/SpectatorSidecar.cpp
    src/client/spectator/ResilientSpectatorRuntime.cpp
    src/client/spectator/ResilientTopologyAuthority.cpp
    src/client/spectator/TopologyAuthoritySignature.cpp
    src/client/spectator/ResilientTopologyClient.cpp
    src/client/spectator/ResilientTopologyClientRuntime.cpp
    src/client/spectator/ResilientSpectatorNodeRuntime.cpp
    src/client/spectator/ReleasedTopologyControlV2.cpp
    src/client/spectator/SpectatorRoute.cpp
    src/relay/RelayServerCore.cpp
    src/relay/RelayService.cpp
)
target_include_directories(qoh99_resilient_topology_client_process_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_topology_client_process_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_topology_client_process_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_resilient_topology_client_process_tests PRIVATE
    qoh99net
    qoh99_platform
    qoh99_netplay_core
    qoh99_spectator
    qoh99_replay
    qoh99_content_identity
    ws2_32
    bcrypt)
if(MSVC)
    set_property(TARGET qoh99_resilient_topology_client_process_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_topology_client_process_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_topology_client_process
         COMMAND qoh99_resilient_topology_client_process_tests)
set_tests_properties(qoh99_resilient_topology_client_process PROPERTIES TIMEOUT 60)
add_test(NAME qoh99_resilient_topology_client_relay_qsr_process
         COMMAND qoh99_resilient_topology_client_process_tests
                 --relay-qsr-validation)
set_tests_properties(qoh99_resilient_topology_client_relay_qsr_process
                     PROPERTIES TIMEOUT 60)

add_test(NAME qoh99_resilient_topology_relay_data_binding
         COMMAND qoh99_resilient_topology_client_process_tests
                 --relay-data-binding-validation)
set_tests_properties(qoh99_resilient_topology_relay_data_binding
                     PROPERTIES TIMEOUT 45)

add_test(NAME qoh99_resilient_topology_paired_activation
         COMMAND qoh99_resilient_topology_client_process_tests
                 --paired-activation-validation)
set_tests_properties(qoh99_resilient_topology_paired_activation
                     PROPERTIES TIMEOUT 60)

add_test(NAME qoh99_client_root_qualification
         COMMAND qoh99_resilient_topology_client_process_tests
                 --client-root-qualification)
set_tests_properties(qoh99_client_root_qualification PROPERTIES TIMEOUT 60)

add_test(NAME qoh99_resilient_topology_relay_process
         COMMAND qoh99_resilient_topology_client_process_tests
                 --phase-c-relay-process)
set_tests_properties(qoh99_resilient_topology_relay_process
                     PROPERTIES TIMEOUT 60)

add_test(NAME qoh99_resilient_topology_authority_state_validation
         COMMAND qoh99_resilient_topology_client_process_tests
                 --authority-state-validation)
set_tests_properties(qoh99_resilient_topology_authority_state_validation
                     PROPERTIES TIMEOUT 45)

function(add_resilient_topology_b10_process number label)
    add_test(NAME qoh99_resilient_topology_b10_${number}_${label}
             COMMAND qoh99_resilient_topology_client_process_tests
                     --b10-case ${number})
    set_tests_properties(qoh99_resilient_topology_b10_${number}_${label}
                         PROPERTIES TIMEOUT 35)
endfunction()

function(add_resilient_topology_b10_core number label)
    add_test(NAME qoh99_resilient_topology_b10_${number}_${label}
             COMMAND qoh99_resilient_topology_client_tests
                     --b10-case ${number})
    set_tests_properties(qoh99_resilient_topology_b10_${number}_${label}
                         PROPERTIES TIMEOUT 30)
endfunction()

add_resilient_topology_b10_process(01 credential_expired_before_qsr)
add_resilient_topology_b10_process(02 expires_before_qsr_admission)
add_resilient_topology_b10_process(03 expires_after_attachment)
add_resilient_topology_b10_process(04 proof_response_authority_generation)
add_resilient_topology_b10_core(05 lease_authority_generation)
add_resilient_topology_b10_core(06 parent_commit_authority_generation)
add_resilient_topology_b10_process(07 stale_authority_generation)
add_resilient_topology_b10_process(08 wrong_session)
add_resilient_topology_b10_process(09 wrong_epoch)
add_resilient_topology_b10_process(10 wrong_compatibility_identity)
add_resilient_topology_b10_process(11 wrong_stream_root)
add_resilient_topology_b10_process(12 wrong_source_generation)
add_resilient_topology_b10_process(13 wrong_root_node)
add_resilient_topology_b10_process(14 wrong_child_node)
add_resilient_topology_b10_process(15 wrong_viewer)
add_resilient_topology_b10_process(16 wrong_viewer_generation)
add_resilient_topology_b10_process(17 wrong_qsp_endpoint)
add_resilient_topology_b10_process(18 wrong_qsp_route_generation)
add_resilient_topology_b10_process(19 wrong_qsr_endpoint)
add_resilient_topology_b10_process(20 wrong_qsr_binding_generation)
add_resilient_topology_b10_process(21 wrong_request_nonce)
add_resilient_topology_b10_process(22 wrong_transcript_cursor)
add_resilient_topology_b10_process(23 wrong_response_tag)
add_resilient_topology_b10_process(24 truncated_response)
add_resilient_topology_b10_process(25 oversized_response)
add_resilient_topology_b10_process(26 unsupported_wire_version)
add_resilient_topology_b10_process(27 duplicate_success_response)
add_resilient_topology_b10_process(28 prior_session_response_replay)
add_resilient_topology_b10_process(29 qsp_qsr_local_port_collision)
add_resilient_topology_b10_process(30 qsp_qsr_endpoint_collision)
add_resilient_topology_b10_core(31 qsp_to_qsr_lane)
add_resilient_topology_b10_core(32 qsr_to_qsp_lane)
add_resilient_topology_b10_core(33 viewer_grant_as_qta_credential)
add_resilient_topology_b10_process(34 punched_qsp_not_qsr_proof)
add_resilient_topology_b10_process(35 relay_route_not_qta_proof)
add_resilient_topology_b10_core(36 exact_qsr_timeout)
add_resilient_topology_b10_process(37 one_lost_response_retry)
add_resilient_topology_b10_process(38 all_responses_lost)
add_resilient_topology_b10_process(39 abandoned_link_late_response)
add_resilient_topology_b10_process(40 retry_changed_route_generation)
add_resilient_topology_b10_process(41 retry_changed_viewer_generation)
add_resilient_topology_b10_process(42 retry_changed_authority_generation)
add_resilient_topology_b10_core(43 retry_budget_exhausted)
add_resilient_topology_b10_core(44 stop_while_retry_pending)
add_resilient_topology_b10_process(45 revocation_during_qsr_retry)
add_resilient_topology_b10_process(46 unexpected_relay_qsr_offer)

add_executable(qoh99_resilient_spectator_node_runtime_tests
    src/client/spectator/tests/test_resilient_spectator_node_runtime.cpp
    src/client/spectator/ResilientSpectatorNodeRuntime.cpp
)
target_include_directories(qoh99_resilient_spectator_node_runtime_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_spectator_node_runtime_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_spectator_node_runtime_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    QOH99_NODE_RUNTIME_TEST_SEAM)
target_link_libraries(qoh99_resilient_spectator_node_runtime_tests PRIVATE
    qoh99_spectator)
if(MSVC)
    set_property(TARGET qoh99_resilient_spectator_node_runtime_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_spectator_node_runtime_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_spectator_node_runtime
         COMMAND qoh99_resilient_spectator_node_runtime_tests)
set_tests_properties(qoh99_resilient_spectator_node_runtime PROPERTIES TIMEOUT 60)

add_executable(qoh99_resilient_spectator_qsp_local_process_tests
    src/client/spectator/tests/test_resilient_spectator_qsp_local_process.cpp
    src/client/spectator/ResilientSpectatorNodeRuntime.cpp
)
target_include_directories(qoh99_resilient_spectator_qsp_local_process_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_resilient_spectator_qsp_local_process_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_resilient_spectator_qsp_local_process_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    QOH99_NODE_RUNTIME_TEST_SEAM)
target_link_libraries(qoh99_resilient_spectator_qsp_local_process_tests PRIVATE
    qoh99_spectator
    ws2_32)
if(MSVC)
    set_property(TARGET qoh99_resilient_spectator_qsp_local_process_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_resilient_spectator_qsp_local_process_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_resilient_spectator_qsp_local_process
         COMMAND qoh99_resilient_spectator_qsp_local_process_tests)
set_tests_properties(qoh99_resilient_spectator_qsp_local_process PROPERTIES TIMEOUT 60)

add_executable(qoh99_spectator_production_tests
    src/spectator/tests/test_spectator_production.cpp
)
target_include_directories(qoh99_spectator_production_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_spectator_production_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_spectator_production_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_spectator_production_tests PRIVATE
    qoh99_spectator qoh99_replay)
if(MSVC)
    set_property(TARGET qoh99_spectator_production_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_spectator_production_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_spectator_production
         COMMAND qoh99_spectator_production_tests)
set_tests_properties(qoh99_spectator_production PROPERTIES TIMEOUT 600)

add_executable(qoh99_soak_tests
    src/netplay/tests/soak/DeterministicNetwork.cpp
    src/netplay/tests/soak/ProductionStackSoak.cpp
    src/netplay/tests/soak/ToySimulation.cpp
    src/netplay/tests/soak/SoakRunner.cpp
    src/netplay/tests/soak/soak_main.cpp
)
target_include_directories(qoh99_soak_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(qoh99_soak_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_soak_tests PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qoh99_soak_tests PRIVATE
    qoh99_netplay_core
    qoh99_spectator)
if(MSVC)
    set_property(TARGET qoh99_soak_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_soak_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_soak_quick COMMAND qoh99_soak_tests --quick)
set_tests_properties(qoh99_soak_quick PROPERTIES TIMEOUT 600)
add_custom_target(qoh99_soak_full
    COMMAND qoh99_soak_tests --full
    DEPENDS qoh99_soak_tests
    USES_TERMINAL)
add_custom_target(qoh99_soak_nightly
    COMMAND qoh99_soak_tests --nightly
    DEPENDS qoh99_soak_tests
    USES_TERMINAL)

add_library(qoh99_rollback STATIC
    src/rollback/SavestateProfile.cpp
    src/rollback/SyncHash.cpp
    src/rollback/RollbackRuntime.cpp
)
target_include_directories(qoh99_rollback PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(qoh99_rollback PUBLIC qoh99_re_model)
target_compile_features(qoh99_rollback PUBLIC cxx_std_17)
target_compile_definitions(qoh99_rollback PUBLIC _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET qoh99_rollback PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(qoh99_rollback_tests
    src/rollback/tests/test_rollback_main.cpp
    src/rollback/tests/test_savestate.cpp
    src/rollback/tests/test_hash_and_runtime.cpp
)
target_include_directories(qoh99_rollback_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/qoh99_re/tests")
target_link_libraries(qoh99_rollback_tests PRIVATE qoh99_rollback)
if(MSVC)
    set_property(TARGET qoh99_rollback_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_rollback_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_rollback_layer COMMAND qoh99_rollback_tests)

add_executable(qoh99_production_tests
    src/tests/production/test_production_main.cpp
    src/tests/production/test_production_addresses.cpp
    src/tests/production/test_production_regions.cpp
    src/tests/production/test_production_phase.cpp
    src/tests/production/test_production_frame_step.cpp
    src/tests/production/test_production_parity.cpp
    src/tests/production/test_production_ai.cpp
    src/tests/production/test_production_native_ko.cpp
    src/tests/production/test_production_training.cpp
    src/tests/production/test_production_direct_boot.cpp
    src/rollback/SavestateProfile.cpp
    src/rollback/SyncHash.cpp
)
target_include_directories(qoh99_production_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rollback"
    "${CMAKE_CURRENT_SOURCE_DIR}/ddraw_wrapper"
    "${QOH99_RE_INC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/qoh99_re/tests")
target_link_libraries(qoh99_production_tests PRIVATE qoh99_re_model)
target_compile_features(qoh99_production_tests PRIVATE cxx_std_17)
target_compile_definitions(qoh99_production_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    set_property(TARGET qoh99_production_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(qoh99_production_tests PRIVATE /MACHINE:X86)
endif()
add_test(NAME qoh99_production_surface COMMAND qoh99_production_tests)

set(QOH99_DEPLOY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Directory that receives the playable netplay artifacts")
set(_qoh99_deploy_files
    "$<TARGET_FILE:qoh99caster>"
    "$<TARGET_FILE:qoh99_netplay>")
if(TARGET ddraw)
    list(APPEND _qoh99_deploy_files "$<TARGET_FILE:ddraw>")
endif()
if(QOH99_SDL3_DLL)
    list(APPEND _qoh99_deploy_files "${QOH99_SDL3_DLL}")
endif()
add_custom_target(qoh99_deploy
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${_qoh99_deploy_files} "${QOH99_DEPLOY_ROOT}/"
    COMMENT "Deploying netplay runtime to ${QOH99_DEPLOY_ROOT}")
add_dependencies(qoh99_deploy qoh99caster qoh99_netplay)
if(TARGET ddraw)
    add_dependencies(qoh99_deploy ddraw)
endif()

install(TARGETS qoh99_netplay qoh99caster
    RUNTIME DESTINATION .
    COMPONENT Qoh99Distribution)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION .
    RENAME README.txt
    COMPONENT Qoh99Distribution)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README_ja.md"
    DESTINATION .
    RENAME README_ja.txt
    COMPONENT Qoh99Distribution)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/defender_exclusions.cmd"
    DESTINATION .
    COMPONENT Qoh99Distribution)
if(QOH99_SDL3_DLL)
    install(FILES "${QOH99_SDL3_DLL}"
        DESTINATION .
        COMPONENT Qoh99Distribution)
endif()
set(QOH99_OPENING_AVI ""
    CACHE FILEPATH
    "Optional lossless Q99V copy of the opening movie, shipped as SYSTEM/OPENING_LOSSLESS.AVI when set")
set(QOH99_DIST_MOVIE_FILES "")
if(NOT "${QOH99_OPENING_AVI}" STREQUAL "")
    if(NOT EXISTS "${QOH99_OPENING_AVI}")
        message(FATAL_ERROR "QOH99_OPENING_AVI is set but does not exist: ${QOH99_OPENING_AVI}")
    endif()
    install(FILES "${QOH99_OPENING_AVI}"
        DESTINATION SYSTEM
        RENAME OPENING_LOSSLESS.AVI
        COMPONENT Qoh99Distribution)
    set(QOH99_DIST_MOVIE_FILES SYSTEM/OPENING_LOSSLESS.AVI)
endif()

set(QOH99_INDEO_DRIVER
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/ir50_32.dll"
    CACHE FILEPATH
    "Intel Indeo 5.10 VfW driver shipped beside the game as ir50_32.dll (empty = not shipped)")
set(QOH99_DIST_INDEO_FILES "")
if(NOT "${QOH99_INDEO_DRIVER}" STREQUAL "")
    if(NOT EXISTS "${QOH99_INDEO_DRIVER}")
        message(FATAL_ERROR "QOH99_INDEO_DRIVER is set but does not exist: ${QOH99_INDEO_DRIVER}")
    endif()
    install(FILES "${QOH99_INDEO_DRIVER}"
        DESTINATION .
        RENAME ir50_32.dll
        COMPONENT Qoh99Distribution)
    set(QOH99_DIST_INDEO_FILES ir50_32.dll)
endif()

install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/launch_wine.sh"
    DESTINATION .
    PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
    COMPONENT Qoh99Distribution)
if(TARGET ddraw)
    set(QOH99_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist")
    set(QOH99_DIST_ZIP "${CMAKE_CURRENT_SOURCE_DIR}/qoh99-netplay-win32.zip")
    set(QOH99_DIST_VERIFY
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyDistribution.cmake")

    add_custom_target(qoh99_dist
        COMMAND "${CMAKE_COMMAND}"
            "-DDIST_CONFIG=$<CONFIG>"
            "-DOPENING_AVI_SOURCE=${QOH99_OPENING_AVI}"
            "-DINDEO_DRIVER_SOURCE=${QOH99_INDEO_DRIVER}"
            -DVERIFY_CONFIGURATION_ONLY=ON
            -P "${QOH99_DIST_VERIFY}"
        COMMAND "${CMAKE_COMMAND}"
            "-DDIST_DIR=${QOH99_DIST_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CleanDist.cmake"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${QOH99_DIST_ZIP}"
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
            --config "$<CONFIG>"
            --prefix "${QOH99_DIST_DIR}"
            --component Qoh99Distribution
        COMMAND "${CMAKE_COMMAND}"
            "-DDIST_DIR=${QOH99_DIST_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteManifest.cmake"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${QOH99_DIST_DIR}"
            "${CMAKE_COMMAND}" -E tar cf "${QOH99_DIST_ZIP}"
            --format=zip --
            qoh99caster.exe
            qoh99_netplay.dll
            ddraw.dll
            README.txt
            README_ja.txt
            defender_exclusions.cmd
            SDL3.dll
            launch_wine.sh
            ${QOH99_DIST_MOVIE_FILES}
            ${QOH99_DIST_INDEO_FILES}
            manifest.sha256
        COMMAND "${CMAKE_COMMAND}"
            "-DDIST_CONFIG=$<CONFIG>"
            "-DDIST_DIR=${QOH99_DIST_DIR}"
            "-DZIP_PATH=${QOH99_DIST_ZIP}"
            "-DCASTER_SOURCE=$<TARGET_FILE:qoh99caster>"
            "-DNETPLAY_SOURCE=$<TARGET_FILE:qoh99_netplay>"
            "-DDDRAW_SOURCE=$<TARGET_FILE:ddraw>"
            "-DSDL3_SOURCE=${QOH99_SDL3_DLL}"
            "-DSDL3_EXPECTED_SHA256=${QOH99_SDL3_DLL_SHA256}"
            "-DOPENING_AVI_SOURCE=${QOH99_OPENING_AVI}"
            "-DINDEO_DRIVER_SOURCE=${QOH99_INDEO_DRIVER}"
            -P "${QOH99_DIST_VERIFY}"
        DEPENDS
            qoh99caster
            qoh99_netplay
            ddraw
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/README_ja.md"
            "${QOH99_DIST_VERIFY}"
        COMMENT
            "Creating verified QOH99 netplay bundle in dist"
        VERBATIM
        USES_TERMINAL)
else()
    message(STATUS
        "qoh99_dist is unavailable because QOH99_BUILD_DDRAW_WRAPPER=OFF")
endif()
