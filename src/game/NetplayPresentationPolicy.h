#pragma once

#include <cstddef>
#include <cstdint>

namespace qgame {

enum class NativeTickVisibility {
    VisibleCanonical,
    Historical,
    HiddenNewForward,
};

constexpr NativeTickVisibility rollbackCorrectionTickVisibility() {
    return NativeTickVisibility::Historical;
}

constexpr NativeTickVisibility playerCatchupTickVisibility(
    const size_t tickIndex, const size_t lastIndex) {
    return tickIndex < lastIndex
        ? NativeTickVisibility::HiddenNewForward
        : NativeTickVisibility::VisibleCanonical;
}

constexpr bool roundIntroRotationPresentationCritical(
    const uint32_t phase, const uint32_t timer) {
    return phase == 6u
        || (phase == 5u && timer > 60u);
}

constexpr bool opportunityStarvedPresentation(const uint32_t framesExecuted,
                                              const uint32_t framesPresented) {
    return framesExecuted > 0u && framesPresented == 0u;
}

constexpr bool deterministicPresentationClockStarted(
    const bool netRunning, const bool spectatorMode,
    const bool spectatorPlaybackStarted) {
    return netRunning
        && (!spectatorMode || spectatorPlaybackStarted);
}

constexpr NativeTickVisibility spectatorCatchupTickVisibility(
    const size_t tickIndex, const size_t tickBudget) {
    return tickBudget > 1u && tickIndex < tickBudget - 1u
        ? NativeTickVisibility::Historical
        : NativeTickVisibility::VisibleCanonical;
}

constexpr NativeTickVisibility spectatorCatchupExecutedTickVisibility(
    const size_t tickIndex, const size_t tickBudget,
    const bool forcedClosingTick) {
    return forcedClosingTick
        ? NativeTickVisibility::VisibleCanonical
        : spectatorCatchupTickVisibility(tickIndex, tickBudget);
}

constexpr bool spectatorCatchupTickDraws() { return true; }

constexpr uintptr_t kQohFpsLastSampleMs = 0x4AA448u;
constexpr uintptr_t kQohPublishedFps = 0x4AA44Cu;
constexpr uintptr_t kQohCompletedNativeTickCount = 0x4AB778u;

struct QohNativeFpsAccounting {
    uint32_t lastSampleMs = 0;
    uint32_t publishedFps = 0;
    uint32_t completedNativeTicks = 0;
};

template <typename ReadDword>
QohNativeFpsAccounting captureQohNativeFpsAccounting(
    ReadDword&& readDword) {
    QohNativeFpsAccounting state{};
    state.lastSampleMs = readDword(kQohFpsLastSampleMs);
    state.publishedFps = readDword(kQohPublishedFps);
    state.completedNativeTicks =
        readDword(kQohCompletedNativeTickCount);
    return state;
}

template <typename WriteDword>
void restoreQohNativeFpsAccounting(
    const QohNativeFpsAccounting& state, WriteDword&& writeDword) {
    writeDword(kQohFpsLastSampleMs, state.lastSampleMs);
    writeDword(kQohPublishedFps, state.publishedFps);
    writeDword(
        kQohCompletedNativeTickCount, state.completedNativeTicks);
}

}
