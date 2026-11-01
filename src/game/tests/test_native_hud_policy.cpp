#include "TestHarness.h"

#include "game/NativeHudPolicy.h"

using namespace qgame;
using qtraining::TrainingLifecycleState;

namespace {

NativeHudOwnerFacts trainingFacts(
        TrainingLifecycleState state = TrainingLifecycleState::Active) {
    NativeHudOwnerFacts facts{};
    facts.trainingSessionRequested = true;
    facts.trainingLifecycle = state;
    return facts;
}

NativeHudOwnerFacts replayFacts() {
    NativeHudOwnerFacts facts{};
    facts.replayRoleActive = true;
    facts.replayPlaybackActive = true;
    return facts;
}

NativeHudVisibilityProfileV1 hideEverything() {
    return nativeHudPresetProfile(NativeHudPreset::NoNativeUi);
}

}

QNP_TEST(NativeHudOwnerIsTrainingOnlyWhileTrainingOwnsTheStepBoundary) {
    QNP_CHECK(nativeHudResolveOwner(trainingFacts())
              == NativeHudOfflineOwner::Training);
    QNP_CHECK(nativeHudResolveOwner(
                  trainingFacts(TrainingLifecycleState::Reinitializing))
              == NativeHudOfflineOwner::Training);
    QNP_CHECK(nativeHudResolveOwner(
                  trainingFacts(TrainingLifecycleState::MatchInitializing))
              == NativeHudOfflineOwner::Training);

    for (const TrainingLifecycleState state : {
             TrainingLifecycleState::Off,
             TrainingLifecycleState::LaunchRequested,
             TrainingLifecycleState::CharacterSelect,
             TrainingLifecycleState::ReturningToCharacterSelect,
             TrainingLifecycleState::Failed}) {
        QNP_CHECK(nativeHudResolveOwner(trainingFacts(state))
                  == NativeHudOfflineOwner::None);
    }

    NativeHudOwnerFacts unmanaged = trainingFacts();
    unmanaged.trainingSessionRequested = false;
    QNP_CHECK(nativeHudResolveOwner(unmanaged) == NativeHudOfflineOwner::None);
}

QNP_TEST(NativeHudOwnerIsReplayOnlyWhilePlayingBackWithoutTakeover) {
    QNP_CHECK(nativeHudResolveOwner(replayFacts())
              == NativeHudOfflineOwner::Replay);

    NativeHudOwnerFacts browser = replayFacts();
    browser.replayPlaybackActive = false;
    QNP_CHECK(nativeHudResolveOwner(browser) == NativeHudOfflineOwner::None);

    NativeHudOwnerFacts takeover = replayFacts();
    takeover.replayTakeoverActive = true;
    QNP_CHECK(nativeHudResolveOwner(takeover) == NativeHudOfflineOwner::None);
}

QNP_TEST(NativeHudOwnerIsNoneInEveryIneligibleRole) {
    QNP_CHECK(nativeHudResolveOwner(NativeHudOwnerFacts{})
              == NativeHudOfflineOwner::None);

    for (const bool spectator : {false, true}) {
        for (const bool netplay : {false, true}) {
            if (!spectator && !netplay) continue;
            NativeHudOwnerFacts facts = trainingFacts();
            facts.netplayActive = netplay;
            facts.spectatorRole = spectator;
            QNP_CHECK(nativeHudResolveOwner(facts)
                      == NativeHudOfflineOwner::None);
            NativeHudOwnerFacts replaying = replayFacts();
            replaying.netplayActive = netplay;
            replaying.spectatorRole = spectator;
            QNP_CHECK(nativeHudResolveOwner(replaying)
                      == NativeHudOfflineOwner::None);
        }
    }

    NativeHudOwnerFacts both = trainingFacts();
    both.replayRoleActive = true;
    both.replayPlaybackActive = true;
    QNP_CHECK(nativeHudResolveOwner(both) == NativeHudOfflineOwner::None);
}

QNP_TEST(NativeHudHideEverythingHasZeroEffectWithoutAnEligibleOwner) {
    const NativeHudEffectiveMasks masks = nativeHudEffectiveMasks(
        NativeHudOfflineOwner::None, hideEverything());
    QNP_CHECK(masks.globalMask == kNativeHudKnownGlobalMask);
    QNP_CHECK(masks.p1Mask == kNativeHudKnownPlayerMask);
    QNP_CHECK(masks.p2Mask == kNativeHudKnownPlayerMask);
    QNP_CHECK(masks.fullyHonoured());
    QNP_CHECK(!nativeHudScoreSideHidden(masks, 0u));
    QNP_CHECK(!nativeHudBottomTimeHidden(masks));
    QNP_CHECK(!nativeHudScoreSideHidden(masks, 0u));
    QNP_CHECK(!nativeHudScoreSideHidden(masks, 1u));
}

QNP_TEST(NativeHudUnqualifiedElementsStayVisibleAndAreReported) {
    for (const NativeHudOfflineOwner owner : {NativeHudOfflineOwner::Training,
                                              NativeHudOfflineOwner::Replay}) {
        const NativeHudEffectiveMasks masks =
            nativeHudEffectiveMasks(owner, hideEverything());
        QNP_CHECK(nativeHudBottomTimeHidden(masks));
        QNP_CHECK(nativeHudScoreSideHidden(masks, 0u));
        QNP_CHECK(nativeHudPortraitLifeSideHidden(masks, 0u));
        QNP_CHECK(nativeHudNameSideHidden(masks, 1u));
        QNP_CHECK(nativeHudRoundWinsSideHidden(masks, 0u));
        QNP_CHECK(nativeHudGuardProducerHidden(masks));
        QNP_CHECK(nativeHudExSideHidden(masks, 0u));
        QNP_CHECK(nativeHudComboSideHidden(masks, 1u));
        QNP_CHECK(nativeHudFpsHidden(masks));
        QNP_CHECK(nativeHudTopRoundTimerHidden(masks));
        QNP_CHECK((masks.globalMask & HudGlobalKo) != 0u);
        QNP_CHECK((masks.globalMask & HudGlobalReady) != 0u);
        QNP_CHECK((masks.globalMask & HudGlobalPauseOverlay) != 0u);
        QNP_CHECK(!masks.fullyHonoured());
        QNP_CHECK((masks.unavailableGlobalMask & HudGlobalKo) != 0u);
        QNP_CHECK((masks.unavailableGlobalMask & HudGlobalBottomElapsedTime)
                  == 0u);
        QNP_CHECK((masks.unavailableGlobalMask & HudGlobalTopRoundTimer)
                  == 0u);
        QNP_CHECK((masks.unavailableP1Mask & HudPlayerScoreDigits) == 0u);
        QNP_CHECK((masks.unavailableP1Mask & HudPlayerExStocks) == 0u);
    }

    const NativeHudEffectiveMasks full = nativeHudEffectiveMasks(
        NativeHudOfflineOwner::Training,
        nativeHudPresetProfile(NativeHudPreset::Full));
    QNP_CHECK(full.fullyHonoured());
    QNP_CHECK(!nativeHudBottomTimeHidden(full));
}

QNP_TEST(NativeHudAvailabilityIsASubsetOfTheDeclaredBits) {
    QNP_CHECK((kNativeHudAvailableGlobalMask & ~kNativeHudKnownGlobalMask)
              == 0u);
    QNP_CHECK((kNativeHudAvailablePlayerMask & ~kNativeHudKnownPlayerMask)
              == 0u);
    QNP_CHECK(nativeHudGlobalMaskWellFormed(kNativeHudKnownGlobalMask));
    QNP_CHECK(nativeHudPlayerMaskWellFormed(kNativeHudKnownPlayerMask));
    QNP_CHECK(!nativeHudGlobalMaskWellFormed(kNativeHudKnownGlobalMask + 1u));
    QNP_CHECK(!nativeHudPlayerMaskWellFormed(kNativeHudKnownPlayerMask + 1u));
}

QNP_TEST(NativeHudScoreHidesExactlyTheSideThatAskedToBeHidden) {
    NativeHudVisibilityProfileV1 p1Only{};
    p1Only.p1VisibleMask &= ~(HudPlayerScoreLabel | HudPlayerScoreDigits);
    const NativeHudEffectiveMasks partial = nativeHudEffectiveMasks(
        NativeHudOfflineOwner::Training, p1Only);
    QNP_CHECK(nativeHudScoreSideHidden(partial, 0u));
    QNP_CHECK(!nativeHudScoreSideHidden(partial, 1u));

    NativeHudVisibilityProfileV1 digitsOnly{};
    digitsOnly.p1VisibleMask &= ~HudPlayerScoreDigits;
    const NativeHudEffectiveMasks half = nativeHudEffectiveMasks(
        NativeHudOfflineOwner::Training, digitsOnly);
    QNP_CHECK(!nativeHudScoreSideHidden(half, 0u));

    NativeHudVisibilityProfileV1 both = p1Only;
    both.p2VisibleMask &= ~(HudPlayerScoreLabel | HudPlayerScoreDigits);
    const NativeHudEffectiveMasks all = nativeHudEffectiveMasks(
        NativeHudOfflineOwner::Training, both);
    QNP_CHECK(nativeHudScoreSideHidden(all, 0u));
    QNP_CHECK(nativeHudScoreSideHidden(all, 1u));
}

QNP_TEST(NativeHudPresetsExpandAndRoundTrip) {
    for (const NativeHudPreset preset : {NativeHudPreset::Full,
                                         NativeHudPreset::Minimal,
                                         NativeHudPreset::Clean,
                                         NativeHudPreset::NoNativeUi}) {
        const NativeHudVisibilityProfileV1 profile =
            nativeHudPresetProfile(preset);
        QNP_CHECK(nativeHudProfileWellFormed(profile));
        QNP_CHECK(nativeHudPresetOf(profile) == preset);
    }

    const NativeHudVisibilityProfileV1 clean =
        nativeHudPresetProfile(NativeHudPreset::Clean);
    QNP_CHECK(clean.globalVisibleMask == kNativeHudAnnouncementMask);
    QNP_CHECK((clean.globalVisibleMask & HudGlobalKo) != 0u);
    QNP_CHECK(clean.p1VisibleMask == 0u && clean.p2VisibleMask == 0u);
    QNP_CHECK((nativeHudPresetProfile(NativeHudPreset::NoNativeUi)
                   .globalVisibleMask
               & HudGlobalKo) == 0u);

    const NativeHudVisibilityProfileV1 minimal =
        nativeHudPresetProfile(NativeHudPreset::Minimal);
    QNP_CHECK((minimal.p1VisibleMask & HudPlayerLifeCurrentFill) != 0u);
    QNP_CHECK((minimal.p1VisibleMask & HudPlayerExStocks) != 0u);
    QNP_CHECK((minimal.p1VisibleMask & HudPlayerPortrait) == 0u);
    QNP_CHECK((minimal.p1VisibleMask & HudPlayerCharacterName) == 0u);
    QNP_CHECK((minimal.p1VisibleMask & HudPlayerScoreDigits) == 0u);

    NativeHudVisibilityProfileV1 odd{};
    odd.p2VisibleMask &= ~HudPlayerPortrait;
    QNP_CHECK(nativeHudPresetOf(odd) == NativeHudPreset::Custom);
}

QNP_TEST(NativeHudTrainingAndReplayProfilesAreIndependent) {
    OfflineNativeHudPreferencesV1 prefs{};
    prefs.training = hideEverything();
    prefs.replay = nativeHudPresetProfile(NativeHudPreset::Clean);

    QNP_CHECK(nativeHudBottomTimeHidden(nativeHudEffectiveMasks(
        NativeHudOfflineOwner::Training, prefs.training)));
    QNP_CHECK(nativeHudBottomTimeHidden(nativeHudEffectiveMasks(
        NativeHudOfflineOwner::Replay, prefs.replay)));
    QNP_CHECK((prefs.training.globalVisibleMask & HudGlobalKo) == 0u);
    QNP_CHECK((prefs.replay.globalVisibleMask & HudGlobalKo) != 0u);

    NativeHudVisibilityProfileV1 p2Only{};
    p2Only.p1VisibleMask = 0u;
    const NativeHudEffectiveMasks masks =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, p2Only);
    QNP_CHECK(nativeHudScoreSideHidden(masks, 0u));
    QNP_CHECK(!nativeHudScoreSideHidden(masks, 1u));
}

QNP_TEST(NativeHudLegacyMigrationTouchesOnlyItsOwnTwoElements) {
    const NativeHudVisibilityProfileV1 neither =
        nativeHudProfileFromLegacy(false, false);
    QNP_CHECK(nativeHudPresetOf(neither) == NativeHudPreset::Full);

    const NativeHudVisibilityProfileV1 score =
        nativeHudProfileFromLegacy(true, false);
    QNP_CHECK((score.p1VisibleMask & HudPlayerScoreDigits) == 0u);
    QNP_CHECK((score.p2VisibleMask & HudPlayerScoreLabel) == 0u);
    QNP_CHECK((score.p1VisibleMask & HudPlayerPortrait) != 0u);
    QNP_CHECK(score.globalVisibleMask == kNativeHudKnownGlobalMask);

    const NativeHudVisibilityProfileV1 timer =
        nativeHudProfileFromLegacy(false, true);
    QNP_CHECK((timer.globalVisibleMask & HudGlobalBottomElapsedTime) == 0u);
    QNP_CHECK((timer.globalVisibleMask & HudGlobalTopRoundTimer) != 0u);
    QNP_CHECK(timer.p1VisibleMask == kNativeHudKnownPlayerMask);
}

QNP_TEST(NativeHudWholeProducerGroupsHideTogetherOrNotAtAll) {
    NativeHudVisibilityProfileV1 lifeOnly{};
    lifeOnly.p1VisibleMask &= ~HudPlayerLifeCurrentFill;
    const NativeHudEffectiveMasks partial =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, lifeOnly);
    QNP_CHECK(!nativeHudPortraitLifeSideHidden(partial, 0u));
    QNP_CHECK((partial.unavailableP1Mask & HudPlayerLifeCurrentFill) != 0u);
    QNP_CHECK((partial.p1Mask & HudPlayerLifeCurrentFill) != 0u);

    NativeHudVisibilityProfileV1 whole{};
    whole.p1VisibleMask &= ~kNativeHudGroupPortraitLife;
    const NativeHudEffectiveMasks hidden =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, whole);
    QNP_CHECK(nativeHudPortraitLifeSideHidden(hidden, 0u));
    QNP_CHECK(!nativeHudPortraitLifeSideHidden(hidden, 1u));
    QNP_CHECK((hidden.unavailableP1Mask & kNativeHudGroupPortraitLife) == 0u);

    NativeHudVisibilityProfileV1 names{};
    names.p2VisibleMask &= ~HudPlayerCharacterName;
    const NativeHudEffectiveMasks nameMasks =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, names);
    QNP_CHECK(nativeHudNameSideHidden(nameMasks, 1u));
    QNP_CHECK(!nativeHudNameSideHidden(nameMasks, 0u));
    QNP_CHECK(nameMasks.fullyHonoured());

    NativeHudVisibilityProfileV1 wins{};
    wins.p1VisibleMask &= ~HudPlayerRoundWinMarkers;
    const NativeHudEffectiveMasks winMasks =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, wins);
    QNP_CHECK(nativeHudRoundWinsSideHidden(winMasks, 0u));
    QNP_CHECK(!nativeHudRoundWinsSideHidden(winMasks, 1u));
}

QNP_TEST(NativeHudGuardNeedsBothSidesBecauseOneCallDrawsThem) {
    NativeHudVisibilityProfileV1 p1Only{};
    p1Only.p1VisibleMask &= ~kNativeHudGroupGuard;
    const NativeHudEffectiveMasks partial =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, p1Only);
    QNP_CHECK(!nativeHudGuardProducerHidden(partial));
    QNP_CHECK((partial.p1Mask & kNativeHudGroupGuard) != 0u);
    QNP_CHECK((partial.unavailableP1Mask & kNativeHudGroupGuard) != 0u);
    QNP_CHECK((partial.unavailableP2Mask & kNativeHudGroupGuard) == 0u);

    NativeHudVisibilityProfileV1 both = p1Only;
    both.p2VisibleMask &= ~kNativeHudGroupGuard;
    const NativeHudEffectiveMasks hidden =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, both);
    QNP_CHECK(nativeHudGuardProducerHidden(hidden));
    QNP_CHECK((hidden.unavailableP1Mask & kNativeHudGroupGuard) == 0u);
    QNP_CHECK((hidden.unavailableP2Mask & kNativeHudGroupGuard) == 0u);
    QNP_CHECK(hidden.fullyHonoured());
}

QNP_TEST(NativeHudStatefulProducersAreWholeGroupsAndAvailable) {
    const uint32_t stateful = kNativeHudGroupEx | kNativeHudGroupCombo;
    QNP_CHECK((kNativeHudAvailablePlayerMask & stateful) == stateful);

    NativeHudVisibilityProfileV1 halfEx{};
    halfEx.p1VisibleMask &= ~HudPlayerExStocks;
    const NativeHudEffectiveMasks partial =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, halfEx);
    QNP_CHECK(!nativeHudExSideHidden(partial, 0u));
    QNP_CHECK((partial.unavailableP1Mask & HudPlayerExStocks) != 0u);

    NativeHudVisibilityProfileV1 wholeEx{};
    wholeEx.p2VisibleMask &= ~kNativeHudGroupEx;
    const NativeHudEffectiveMasks exHidden =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, wholeEx);
    QNP_CHECK(nativeHudExSideHidden(exHidden, 1u));
    QNP_CHECK(!nativeHudExSideHidden(exHidden, 0u));
    QNP_CHECK(exHidden.fullyHonoured());

    NativeHudVisibilityProfileV1 fpsOnly{};
    fpsOnly.globalVisibleMask &= ~HudGlobalNativeFpsReadout;
    const NativeHudEffectiveMasks fps =
        nativeHudEffectiveMasks(NativeHudOfflineOwner::Training, fpsOnly);
    QNP_CHECK(nativeHudFpsHidden(fps));
    QNP_CHECK(!nativeHudTopRoundTimerHidden(fps));
    QNP_CHECK(fps.fullyHonoured());
}
