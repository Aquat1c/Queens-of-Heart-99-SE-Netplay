#pragma once

#include <cstdint>

#include "training/TrainingTypes.h"

namespace qgame {

enum class NativeHudOfflineOwner : uint8_t {
    None = 0,
    Training,
    Replay,
};

// Append-only: the values ride local IPC and the wrapper status word. A set bit means VISIBLE.
enum NativeHudGlobalElement : uint32_t {
    HudGlobalTopRoundTimer       = 1u << 0,
    HudGlobalBottomElapsedTime   = 1u << 1,
    HudGlobalRoundWord           = 1u << 2,
    HudGlobalRoundNumber         = 1u << 3,
    HudGlobalReady               = 1u << 4,
    HudGlobalGo                  = 1u << 5,
    HudGlobalSlash               = 1u << 6,
    HudGlobalKo                  = 1u << 7,
    HudGlobalDoubleKo            = 1u << 8,
    HudGlobalTimeUpResult        = 1u << 9,
    HudGlobalGuardCrashBanner    = 1u << 10,
    HudGlobalGuardCancelBanner   = 1u << 11,
    HudGlobalPauseOverlay        = 1u << 12,
    HudGlobalNativeFpsReadout    = 1u << 13,
    HudGlobalNativeDebugReadouts = 1u << 14,
};

enum NativeHudPlayerElement : uint32_t {
    HudPlayerScoreLabel          = 1u << 0,
    HudPlayerScoreDigits         = 1u << 1,
    HudPlayerPortrait            = 1u << 2,
    HudPlayerLifeFrame           = 1u << 3,
    HudPlayerLifeCurrentFill     = 1u << 4,
    HudPlayerLifePendingOverlay  = 1u << 5,
    HudPlayerGuardLabelAndDigits = 1u << 6,
    HudPlayerGuardBar            = 1u << 7,
    HudPlayerExStocks            = 1u << 8,
    HudPlayerExFractionBar       = 1u << 9,
    HudPlayerExSpendAnimation    = 1u << 10,
    HudPlayerCharacterName       = 1u << 11,
    HudPlayerRoundWinMarkers     = 1u << 12,
    HudPlayerComboHitCount       = 1u << 13,
    HudPlayerComboValidityBanner = 1u << 14,
    HudPlayerComboDamage         = 1u << 15,
    HudPlayerComboReactionLabel  = 1u << 16,
};

inline constexpr uint32_t kNativeHudKnownGlobalMask = 0x00007FFFu;
inline constexpr uint32_t kNativeHudKnownPlayerMask = 0x0001FFFFu;

inline constexpr uint32_t kNativeHudGroupScore =
    HudPlayerScoreLabel | HudPlayerScoreDigits;
inline constexpr uint32_t kNativeHudGroupPortraitLife =
    HudPlayerPortrait | HudPlayerLifeFrame | HudPlayerLifeCurrentFill
    | HudPlayerLifePendingOverlay;
inline constexpr uint32_t kNativeHudGroupName = HudPlayerCharacterName;
inline constexpr uint32_t kNativeHudGroupRoundWins = HudPlayerRoundWinMarkers;
inline constexpr uint32_t kNativeHudGroupGuard =
    HudPlayerGuardLabelAndDigits | HudPlayerGuardBar;
inline constexpr uint32_t kNativeHudGroupEx =
    HudPlayerExStocks | HudPlayerExFractionBar | HudPlayerExSpendAnimation;
inline constexpr uint32_t kNativeHudGroupCombo =
    HudPlayerComboHitCount | HudPlayerComboValidityBanner
    | HudPlayerComboDamage | HudPlayerComboReactionLabel;

inline constexpr uint32_t kNativeHudAvailableGlobalMask =
    HudGlobalBottomElapsedTime | HudGlobalTopRoundTimer
    | HudGlobalNativeFpsReadout;
inline constexpr uint32_t kNativeHudAvailablePlayerMask =
    kNativeHudGroupScore | kNativeHudGroupPortraitLife | kNativeHudGroupName
    | kNativeHudGroupRoundWins | kNativeHudGroupGuard | kNativeHudGroupEx
    | kNativeHudGroupCombo;

constexpr bool nativeHudGlobalMaskWellFormed(uint32_t mask) {
    return (mask & ~kNativeHudKnownGlobalMask) == 0u;
}

constexpr bool nativeHudPlayerMaskWellFormed(uint32_t mask) {
    return (mask & ~kNativeHudKnownPlayerMask) == 0u;
}

struct NativeHudVisibilityProfileV1 {
    uint32_t globalVisibleMask = kNativeHudKnownGlobalMask;
    uint32_t p1VisibleMask = kNativeHudKnownPlayerMask;
    uint32_t p2VisibleMask = kNativeHudKnownPlayerMask;
};

struct OfflineNativeHudPreferencesV1 {
    NativeHudVisibilityProfileV1 training{};
    NativeHudVisibilityProfileV1 replay{};
};

constexpr bool nativeHudProfileWellFormed(
        const NativeHudVisibilityProfileV1& profile) {
    return nativeHudGlobalMaskWellFormed(profile.globalVisibleMask)
        && nativeHudPlayerMaskWellFormed(profile.p1VisibleMask)
        && nativeHudPlayerMaskWellFormed(profile.p2VisibleMask);
}

enum class NativeHudPreset : uint32_t {
    Full = 0,
    Minimal = 1,
    Clean = 2,
    NoNativeUi = 3,
    Custom = 4,
};

inline constexpr uint32_t kNativeHudAnnouncementMask =
    HudGlobalRoundWord | HudGlobalRoundNumber | HudGlobalReady | HudGlobalGo
    | HudGlobalSlash | HudGlobalKo | HudGlobalDoubleKo
    | HudGlobalTimeUpResult;

inline constexpr uint32_t kNativeHudEssentialAnnouncementMask =
    HudGlobalRoundWord | HudGlobalReady | HudGlobalGo | HudGlobalSlash
    | HudGlobalKo;

inline constexpr uint32_t kNativeHudPersistentPlayerMask =
    HudPlayerScoreLabel | HudPlayerScoreDigits | HudPlayerPortrait
    | HudPlayerLifeFrame | HudPlayerLifeCurrentFill
    | HudPlayerLifePendingOverlay | HudPlayerGuardLabelAndDigits
    | HudPlayerGuardBar | HudPlayerExStocks | HudPlayerExFractionBar
    | HudPlayerExSpendAnimation | HudPlayerCharacterName
    | HudPlayerRoundWinMarkers;

inline constexpr uint32_t kNativeHudMinimalPlayerMask =
    HudPlayerLifeFrame | HudPlayerLifeCurrentFill
    | HudPlayerLifePendingOverlay | HudPlayerExStocks
    | HudPlayerExFractionBar | HudPlayerExSpendAnimation;

constexpr NativeHudVisibilityProfileV1 nativeHudPresetProfile(
        NativeHudPreset preset) {
    NativeHudVisibilityProfileV1 profile{};
    switch (preset) {
        case NativeHudPreset::Minimal:
            profile.globalVisibleMask = HudGlobalTopRoundTimer
                | kNativeHudEssentialAnnouncementMask;
            profile.p1VisibleMask = kNativeHudMinimalPlayerMask;
            profile.p2VisibleMask = kNativeHudMinimalPlayerMask;
            break;
        case NativeHudPreset::Clean:
            profile.globalVisibleMask = kNativeHudAnnouncementMask;
            profile.p1VisibleMask = 0u;
            profile.p2VisibleMask = 0u;
            break;
        case NativeHudPreset::NoNativeUi:
            profile.globalVisibleMask = 0u;
            profile.p1VisibleMask = 0u;
            profile.p2VisibleMask = 0u;
            break;
        case NativeHudPreset::Full:
        case NativeHudPreset::Custom:
        default:
            break;
    }
    return profile;
}

constexpr bool nativeHudProfilesEqual(
        const NativeHudVisibilityProfileV1& left,
        const NativeHudVisibilityProfileV1& right) {
    return left.globalVisibleMask == right.globalVisibleMask
        && left.p1VisibleMask == right.p1VisibleMask
        && left.p2VisibleMask == right.p2VisibleMask;
}

constexpr NativeHudPreset nativeHudPresetOf(
        const NativeHudVisibilityProfileV1& profile) {
    return nativeHudProfilesEqual(
               profile, nativeHudPresetProfile(NativeHudPreset::Full))
        ? NativeHudPreset::Full
        : nativeHudProfilesEqual(
              profile, nativeHudPresetProfile(NativeHudPreset::Minimal))
            ? NativeHudPreset::Minimal
            : nativeHudProfilesEqual(
                  profile, nativeHudPresetProfile(NativeHudPreset::Clean))
                ? NativeHudPreset::Clean
                : nativeHudProfilesEqual(
                      profile,
                      nativeHudPresetProfile(NativeHudPreset::NoNativeUi))
                    ? NativeHudPreset::NoNativeUi
                    : NativeHudPreset::Custom;
}

struct NativeHudOwnerFacts {
    bool trainingSessionRequested = false;
    qtraining::TrainingLifecycleState trainingLifecycle =
        qtraining::TrainingLifecycleState::Off;
    bool replayRoleActive = false;
    bool replayPlaybackActive = false;
    bool replayTakeoverActive = false;
    bool netplayActive = false;
    bool spectatorRole = false;
};

constexpr bool nativeHudTrainingLifecycleOwns(
        qtraining::TrainingLifecycleState state) {
    return state == qtraining::TrainingLifecycleState::MatchInitializing
        || state == qtraining::TrainingLifecycleState::Active
        || state == qtraining::TrainingLifecycleState::Reinitializing;
}

constexpr NativeHudOfflineOwner nativeHudResolveOwner(
        const NativeHudOwnerFacts& facts) {
    if (facts.netplayActive || facts.spectatorRole)
        return NativeHudOfflineOwner::None;

    const bool training = facts.trainingSessionRequested
        && nativeHudTrainingLifecycleOwns(facts.trainingLifecycle);
    const bool replay = facts.replayRoleActive
        && facts.replayPlaybackActive
        && !facts.replayTakeoverActive;

    if (training && replay) return NativeHudOfflineOwner::None;
    if (training) return NativeHudOfflineOwner::Training;
    if (replay) return NativeHudOfflineOwner::Replay;
    return NativeHudOfflineOwner::None;
}

struct NativeHudEffectiveMasks {
    uint32_t globalMask = kNativeHudKnownGlobalMask;
    uint32_t p1Mask = kNativeHudKnownPlayerMask;
    uint32_t p2Mask = kNativeHudKnownPlayerMask;
    uint32_t unavailableGlobalMask = 0u;
    uint32_t unavailableP1Mask = 0u;
    uint32_t unavailableP2Mask = 0u;

    constexpr bool fullyHonoured() const {
        return unavailableGlobalMask == 0u && unavailableP1Mask == 0u
            && unavailableP2Mask == 0u;
    }
};

constexpr uint32_t nativeHudGroupHiddenBits(uint32_t group,
                                            uint32_t visibleMask) {
    return (visibleMask & group) == 0u ? group : 0u;
}

constexpr uint32_t nativeHudHiddenPlayerBits(uint32_t visibleMask,
                                             uint32_t guardVisibleEitherSide) {
    return nativeHudGroupHiddenBits(kNativeHudGroupScore, visibleMask)
        | nativeHudGroupHiddenBits(kNativeHudGroupPortraitLife, visibleMask)
        | nativeHudGroupHiddenBits(kNativeHudGroupName, visibleMask)
        | nativeHudGroupHiddenBits(kNativeHudGroupRoundWins, visibleMask)
        | nativeHudGroupHiddenBits(kNativeHudGroupEx, visibleMask)
        | nativeHudGroupHiddenBits(kNativeHudGroupCombo, visibleMask)
        | nativeHudGroupHiddenBits(kNativeHudGroupGuard,
                                   visibleMask | guardVisibleEitherSide);
}

constexpr NativeHudEffectiveMasks nativeHudEffectiveMasks(
        NativeHudOfflineOwner owner,
        const NativeHudVisibilityProfileV1& profile) {
    NativeHudEffectiveMasks out{};
    if (owner == NativeHudOfflineOwner::None) return out;

    const uint32_t guardEither =
        (profile.p1VisibleMask | profile.p2VisibleMask) & kNativeHudGroupGuard;
    const uint32_t hiddenGlobal =
        nativeHudGroupHiddenBits(HudGlobalBottomElapsedTime,
                                 profile.globalVisibleMask)
        | nativeHudGroupHiddenBits(HudGlobalTopRoundTimer,
                                   profile.globalVisibleMask)
        | nativeHudGroupHiddenBits(HudGlobalNativeFpsReadout,
                                   profile.globalVisibleMask);
    const uint32_t hiddenP1 =
        nativeHudHiddenPlayerBits(profile.p1VisibleMask, guardEither);
    const uint32_t hiddenP2 =
        nativeHudHiddenPlayerBits(profile.p2VisibleMask, guardEither);

    out.globalMask = kNativeHudKnownGlobalMask & ~hiddenGlobal;
    out.p1Mask = kNativeHudKnownPlayerMask & ~hiddenP1;
    out.p2Mask = kNativeHudKnownPlayerMask & ~hiddenP2;
    out.unavailableGlobalMask =
        ~profile.globalVisibleMask & kNativeHudKnownGlobalMask & ~hiddenGlobal;
    out.unavailableP1Mask =
        ~profile.p1VisibleMask & kNativeHudKnownPlayerMask & ~hiddenP1;
    out.unavailableP2Mask =
        ~profile.p2VisibleMask & kNativeHudKnownPlayerMask & ~hiddenP2;
    return out;
}

constexpr bool nativeHudScoreSideHidden(const NativeHudEffectiveMasks& masks,
                                        uint8_t player) {
    const uint32_t mask = player == 0u ? masks.p1Mask : masks.p2Mask;
    return (mask & kNativeHudGroupScore) == 0u;
}

constexpr bool nativeHudNameSideHidden(const NativeHudEffectiveMasks& masks,
                                       uint8_t player) {
    const uint32_t mask = player == 0u ? masks.p1Mask : masks.p2Mask;
    return (mask & kNativeHudGroupName) == 0u;
}

constexpr bool nativeHudRoundWinsSideHidden(
        const NativeHudEffectiveMasks& masks, uint8_t player) {
    const uint32_t mask = player == 0u ? masks.p1Mask : masks.p2Mask;
    return (mask & kNativeHudGroupRoundWins) == 0u;
}

constexpr bool nativeHudPortraitLifeSideHidden(
        const NativeHudEffectiveMasks& masks, uint8_t player) {
    const uint32_t mask = player == 0u ? masks.p1Mask : masks.p2Mask;
    return (mask & kNativeHudGroupPortraitLife) == 0u;
}

constexpr bool nativeHudExSideHidden(const NativeHudEffectiveMasks& masks,
                                     uint8_t player) {
    const uint32_t mask = player == 0u ? masks.p1Mask : masks.p2Mask;
    return (mask & kNativeHudGroupEx) == 0u;
}

constexpr bool nativeHudComboSideHidden(const NativeHudEffectiveMasks& masks,
                                        uint8_t player) {
    const uint32_t mask = player == 0u ? masks.p1Mask : masks.p2Mask;
    return (mask & kNativeHudGroupCombo) == 0u;
}

constexpr bool nativeHudFpsHidden(const NativeHudEffectiveMasks& masks) {
    return (masks.globalMask & HudGlobalNativeFpsReadout) == 0u;
}

constexpr bool nativeHudTopRoundTimerHidden(
        const NativeHudEffectiveMasks& masks) {
    return (masks.globalMask & HudGlobalTopRoundTimer) == 0u;
}

constexpr bool nativeHudGuardProducerHidden(
        const NativeHudEffectiveMasks& masks) {
    return ((masks.p1Mask | masks.p2Mask) & kNativeHudGroupGuard) == 0u;
}

constexpr bool nativeHudBottomTimeHidden(
        const NativeHudEffectiveMasks& masks) {
    return (masks.globalMask & HudGlobalBottomElapsedTime) == 0u;
}

constexpr NativeHudVisibilityProfileV1 nativeHudProfileFromLegacy(
        bool hideNativeScore, bool hideNativeTimer) {
    NativeHudVisibilityProfileV1 profile{};
    if (hideNativeScore) {
        profile.p1VisibleMask &=
            ~(HudPlayerScoreLabel | HudPlayerScoreDigits);
        profile.p2VisibleMask &=
            ~(HudPlayerScoreLabel | HudPlayerScoreDigits);
    }
    if (hideNativeTimer)
        profile.globalVisibleMask &= ~HudGlobalBottomElapsedTime;
    return profile;
}

}
