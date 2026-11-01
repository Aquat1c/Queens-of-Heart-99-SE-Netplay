#include "netplay/tests/TestHarness.h"

#include "training/TrainingHitboxOverlay.h"

#include <cstdint>

using namespace qtraining;

QNP_TEST(HitboxOrdinarySelectorsAreHitsOnlyWhenLatched) {
    for (uint8_t selector = 0u; selector <= 12u; ++selector) {
        QNP_CHECK(attackRecordKind(selector, true) == HitboxBoxKind::Attack);
        QNP_CHECK(attackRecordKind(selector, false) == HitboxBoxKind::Inert);
    }
    for (const uint8_t selector : {2u, 4u, 8u, 10u}) {
        QNP_CHECK(attackRecordKind(static_cast<uint8_t>(selector), true)
                  == HitboxBoxKind::Attack);
    }
}

QNP_TEST(HitboxUnlatchedAttackGeometryIsInertNotAHitbox) {
    QNP_CHECK(attackRecordKind(0u, false) == HitboxBoxKind::Inert);
    QNP_CHECK(hitboxPaletteFor(HitboxBoxKind::Inert).outlineArgb
              != hitboxPaletteFor(HitboxBoxKind::Attack).outlineArgb);
    QNP_CHECK((hitboxPaletteFor(HitboxBoxKind::Inert).fillArgb >> 24)
              == 0u);
    QNP_CHECK((hitboxPaletteFor(HitboxBoxKind::Attack).fillArgb >> 24)
              == kHitboxFillAlpha);
}

QNP_TEST(HitboxSecondaryCollisionIsItsOwnClashCategory) {
    QNP_CHECK(boxKindForSource(HitboxBoxSource::Secondary, 0u, false)
              == HitboxBoxKind::Clash);
    QNP_CHECK(boxKindForSource(HitboxBoxSource::Secondary, 13u, true)
              == HitboxBoxKind::Clash);
    QNP_CHECK(boxKindForSource(HitboxBoxSource::Secondary, 255u, false)
              == HitboxBoxKind::Clash);
}

QNP_TEST(HitboxReflectAndCaptureSelectorsAreNotHits) {
    QNP_CHECK(attackRecordKind(13u, true) == HitboxBoxKind::Reflect);
    QNP_CHECK(attackRecordKind(14u, true) == HitboxBoxKind::Reflect);
    QNP_CHECK(attackRecordKind(15u, true) == HitboxBoxKind::Capture);
    QNP_CHECK(attackRecordKind(13u, false) == HitboxBoxKind::Reflect);
    QNP_CHECK(attackRecordKind(15u, false) == HitboxBoxKind::Capture);
}

QNP_TEST(HitboxUnknownSelectorStaysVisible) {
    QNP_CHECK(attackRecordKind(16u, true) == HitboxBoxKind::Attack);
    QNP_CHECK(attackRecordKind(255u, true) == HitboxBoxKind::Attack);
}

QNP_TEST(HitboxSourceFamilyDecidesBodyAndHurt) {
    QNP_CHECK(boxKindForSource(HitboxBoxSource::Body, 13u, true)
              == HitboxBoxKind::Collision);
    QNP_CHECK(boxKindForSource(HitboxBoxSource::Hurt, 15u, true)
              == HitboxBoxKind::Hurt);
    QNP_CHECK(boxKindForSource(HitboxBoxSource::Attack, 0u, true)
              == HitboxBoxKind::Attack);
}

QNP_TEST(HitboxSentinelCountsFollowTheEngineRule) {
    QNP_CHECK(hitboxRecordsToDraw(0u) == 0u);
    QNP_CHECK(hitboxRecordsToDraw(1u) == 1u);
    QNP_CHECK(hitboxRecordsToDraw(253u) == 253u);
    QNP_CHECK(hitboxRecordsToDraw(254u) == 0u);
    QNP_CHECK(hitboxRecordsToDraw(255u) == 1u);
}

QNP_TEST(HitboxCollisionIsWhiteAndClashOwnsYellow) {
    const HitboxPalette collision =
        hitboxPaletteFor(HitboxBoxKind::Collision);
    const HitboxPalette clash = hitboxPaletteFor(HitboxBoxKind::Clash);
    QNP_CHECK(collision.outlineArgb == 0xFFFFFFFFu);
    QNP_CHECK((clash.outlineArgb & 0x00FFFFFFu) == 0x00FFFF28u);
    QNP_CHECK(collision.outlineArgb != clash.outlineArgb);
}

QNP_TEST(HitboxEveryKindIsDistinguishable) {
    uint32_t seen[static_cast<size_t>(HitboxBoxKind::Count)] = {};
    for (size_t i = 0; i < static_cast<size_t>(HitboxBoxKind::Count); ++i) {
        seen[i] = hitboxPaletteFor(static_cast<HitboxBoxKind>(i))
                      .outlineArgb;
    }
    for (size_t i = 0; i < static_cast<size_t>(HitboxBoxKind::Count); ++i) {
        for (size_t j = i + 1; j < static_cast<size_t>(HitboxBoxKind::Count);
             ++j) {
            QNP_CHECK(seen[i] != seen[j]);
        }
    }
}

QNP_TEST(HitboxFillIsTranslucentAndOutlineIsNot) {
    for (size_t i = 0; i < static_cast<size_t>(HitboxBoxKind::Count); ++i) {
        const HitboxBoxKind kind = static_cast<HitboxBoxKind>(i);
        const HitboxPalette p = hitboxPaletteFor(kind);
        QNP_CHECK((p.outlineArgb >> 24) == 255u);
        QNP_CHECK((p.fillArgb >> 24) == hitboxFillAlphaFor(kind));
        QNP_CHECK((p.fillArgb >> 24) < 255u);
        QNP_CHECK((p.fillArgb & 0x00FFFFFFu) == (p.outlineArgb & 0x00FFFFFFu));
    }
}

QNP_TEST(HitboxCommandBudgetIsWholeBoxes) {
    QNP_CHECK(hitboxBoxesThatFit(192) == 96);
    QNP_CHECK(hitboxBoxesThatFit(1) == 0);
    QNP_CHECK(hitboxBoxesThatFit(0) == 0);
    QNP_CHECK(hitboxBoxesThatFit(-4) == 0);
}

QNP_TEST(HitboxModesNeverDrawBothDisplays) {
    QNP_CHECK(hitboxModeWantsCustomOverlay(HitboxDisplayMode::Custom));
    QNP_CHECK(!hitboxModeWantsVanillaFlag(HitboxDisplayMode::Custom));
    QNP_CHECK(hitboxModeWantsVanillaFlag(HitboxDisplayMode::Vanilla));
    QNP_CHECK(!hitboxModeWantsCustomOverlay(HitboxDisplayMode::Vanilla));
    QNP_CHECK(!hitboxModeWantsVanillaFlag(HitboxDisplayMode::Off));
    QNP_CHECK(!hitboxModeWantsCustomOverlay(HitboxDisplayMode::Off));
}

QNP_TEST(HitboxVanillaFlagIsExactlyOneOrZero) {
    QNP_CHECK(hitboxVanillaFlagValue(HitboxDisplayMode::Vanilla) == 1u);
    QNP_CHECK(hitboxVanillaFlagValue(HitboxDisplayMode::Custom) == 0u);
    QNP_CHECK(hitboxVanillaFlagValue(HitboxDisplayMode::Off) == 0u);
}

QNP_TEST(HitboxUnmirroredRectIsActorOriginPlusInclusiveEdges) {
    const HitboxRect r = hitboxResolveRect(1000, 500, 10, -40, 30, 0, false);
    QNP_CHECK(r.left == 1010);
    QNP_CHECK(r.top == 460);
    QNP_CHECK(r.right == 1031);
    QNP_CHECK(r.bottom == 501);
}

QNP_TEST(HitboxMirroredRectReflectsAboutActorPlus128) {
    const int32_t actorX = 1000;
    const HitboxRect right =
        hitboxResolveRect(actorX, 500, 10, -40, 30, 0, false);
    const HitboxRect left =
        hitboxResolveRect(actorX, 500, 10, -40, 30, 0, true);
    const int32_t axis = actorX + kHitboxMirrorAxisOffset;
    QNP_CHECK(left.right == 2 * axis - right.left);
    QNP_CHECK(left.left == 2 * axis - right.right);
    QNP_CHECK(left.top == right.top);
    QNP_CHECK(left.bottom == right.bottom);
    QNP_CHECK(left.right - left.left == right.right - right.left);
}

QNP_TEST(HitboxVirtualCanvasIsTheGamesOwn320x240Surface) {
    QNP_CHECK(kHitboxVirtualWidth == 320.0f);
    QNP_CHECK(kHitboxVirtualHeight == 240.0f);
}

QNP_TEST(HitboxScreenMappingMatchesTheEngineTransform) {
    const HitboxCameraV1 camera{96, 8, 3};
    QNP_CHECK(hitboxScreenX(1000, camera) == 904);
    QNP_CHECK(hitboxScreenY(500, camera) == 511);

    const HitboxRect screen =
        hitboxToScreen(HitboxRect{1010, 460, 1031, 501}, camera);
    QNP_CHECK(screen.left == 914);
    QNP_CHECK(screen.top == 471);
    QNP_CHECK(screen.right == 935);
    QNP_CHECK(screen.bottom == 512);
    QNP_CHECK(screen.right - screen.left == 21);
    QNP_CHECK(screen.bottom - screen.top == 41);
}

QNP_TEST(HitboxDegenerateRectsAreRefused) {
    QNP_CHECK(hitboxRectWellFormed(HitboxRect{0, 0, 10, 10}));
    QNP_CHECK(!hitboxRectWellFormed(HitboxRect{10, 0, 10, 10}));
    QNP_CHECK(!hitboxRectWellFormed(HitboxRect{0, 10, 10, 10}));
    QNP_CHECK(!hitboxRectWellFormed(HitboxRect{20, 0, 10, 10}));
}

QNP_TEST(HitboxCullingKeepsBoxesThatStraddleTheEdge) {
    QNP_CHECK(hitboxRectVisible(HitboxRect{100, 100, 140, 140}));
    QNP_CHECK(hitboxRectVisible(HitboxRect{-20, 100, 40, 140}));
    QNP_CHECK(hitboxRectVisible(HitboxRect{300, 100, 360, 140}));
    QNP_CHECK(!hitboxRectVisible(HitboxRect{-4000, 100, -3900, 140}));
    QNP_CHECK(!hitboxRectVisible(HitboxRect{4000, 100, 4100, 140}));
}

QNP_TEST(HitboxAttackSentinelsMeanNoAttackGeometryAtAll) {
    QNP_CHECK(hitboxRecordsToDrawFor(HitboxBoxSource::Attack, 254u) == 0u);
    QNP_CHECK(hitboxRecordsToDrawFor(HitboxBoxSource::Attack, 255u) == 0u);
    QNP_CHECK(hitboxRecordsToDrawFor(HitboxBoxSource::Attack, 0u) == 0u);
    QNP_CHECK(hitboxRecordsToDrawFor(HitboxBoxSource::Attack, 3u) == 3u);
    QNP_CHECK(hitboxRecordsToDrawFor(HitboxBoxSource::Body, 255u) == 1u);
    QNP_CHECK(hitboxRecordsToDrawFor(HitboxBoxSource::Hurt, 255u) == 1u);
    QNP_CHECK(hitboxRecordsToDrawFor(HitboxBoxSource::Secondary, 255u) == 1u);
    QNP_CHECK(hitboxRecordsToDrawFor(HitboxBoxSource::Secondary, 254u) == 0u);
}

QNP_TEST(HitboxContactFlagsAreIndependentBits) {
    QNP_CHECK(hitboxAttackActive(0x0001u));
    QNP_CHECK(hitboxContactHit(0x0010u));
    QNP_CHECK(hitboxContactClash(0x0020u));
    QNP_CHECK(hitboxContactGuard(0x0040u));
    QNP_CHECK(!hitboxContactHit(0x0020u));
    QNP_CHECK(!hitboxContactGuard(0x0020u));
    QNP_CHECK(!hitboxContactClash(0x0010u));
    QNP_CHECK(!hitboxContactClash(0u));
    QNP_CHECK(hitboxContactClash(0x0021u));
    QNP_CHECK(hitboxAttackActive(0x0021u));
}

QNP_TEST(HitboxModeParsingRejectsOutOfRangeValues) {
    QNP_CHECK(hitboxDisplayModeValid(0u));
    QNP_CHECK(hitboxDisplayModeValid(1u));
    QNP_CHECK(hitboxDisplayModeValid(2u));
    QNP_CHECK(!hitboxDisplayModeValid(3u));
    QNP_CHECK(!hitboxDisplayModeValid(0xFFFFFFFFu));
    QNP_CHECK(hitboxDisplayModeFrom(1u) == HitboxDisplayMode::Custom);
    QNP_CHECK(hitboxDisplayModeFrom(2u) == HitboxDisplayMode::Vanilla);
    QNP_CHECK(hitboxDisplayModeFrom(3u) == HitboxDisplayMode::Off);
}

QNP_TEST(HitboxDefaultRendererIsOurs) {
    QNP_CHECK(kHitboxDefaultRenderer == HitboxDisplayMode::Custom);
    QNP_CHECK(hitboxRendererPreference(HitboxDisplayMode::Off)
              == HitboxDisplayMode::Custom);
    QNP_CHECK(hitboxRendererPreference(HitboxDisplayMode::Custom)
              == HitboxDisplayMode::Custom);
    QNP_CHECK(hitboxRendererPreference(HitboxDisplayMode::Vanilla)
              == HitboxDisplayMode::Vanilla);
}

QNP_TEST(HitboxToggleIsOnOffAndRemembersTheRenderer) {
    for (const HitboxDisplayMode preferred :
         {HitboxDisplayMode::Custom, HitboxDisplayMode::Vanilla}) {
        const HitboxDisplayMode on =
            hitboxToggled(HitboxDisplayMode::Off, preferred);
        QNP_CHECK(on == preferred);
        QNP_CHECK(hitboxToggled(on, preferred) == HitboxDisplayMode::Off);
    }
    QNP_CHECK(hitboxToggled(HitboxDisplayMode::Vanilla,
                            HitboxDisplayMode::Custom)
              == HitboxDisplayMode::Off);
}

QNP_TEST(HitboxSnapshotAppendCarriesScreenRectsAndClassification) {
    HitboxSnapshotV1 snapshot{};
    QNP_CHECK(hitboxSnapshotAppend(snapshot, HitboxRect{10, 20, 40, 60},
                                   HitboxBoxKind::Attack, 1u));
    QNP_CHECK(snapshot.boxCount == 1u);
    QNP_CHECK(snapshot.boxes[0].x == 10);
    QNP_CHECK(snapshot.boxes[0].y == 20);
    QNP_CHECK(snapshot.boxes[0].w == 30);
    QNP_CHECK(snapshot.boxes[0].h == 40);
    QNP_CHECK(snapshot.boxes[0].kind
              == static_cast<uint8_t>(HitboxBoxKind::Attack));
    QNP_CHECK(snapshot.boxes[0].owner == 1u);
    QNP_CHECK(snapshot.droppedBoxes == 0u);
}

QNP_TEST(HitboxSnapshotSkipsUndrawableBoxesWithoutCountingThemDropped) {
    HitboxSnapshotV1 snapshot{};
    QNP_CHECK(!hitboxSnapshotAppend(snapshot, HitboxRect{10, 20, 10, 60},
                                    HitboxBoxKind::Hurt, 0u));
    QNP_CHECK(!hitboxSnapshotAppend(snapshot, HitboxRect{9000, 20, 9100, 60},
                                    HitboxBoxKind::Hurt, 0u));
    QNP_CHECK(snapshot.boxCount == 0u);
    QNP_CHECK(snapshot.droppedBoxes == 0u);
}

QNP_TEST(HitboxSnapshotReportsOverflowInsteadOfTruncatingSilently) {
    HitboxSnapshotV1 snapshot{};
    for (uint32_t i = 0; i < kHitboxSnapshotMaxBoxes + 5u; ++i) {
        (void)hitboxSnapshotAppend(snapshot, HitboxRect{1, 1, 20, 20},
                                   HitboxBoxKind::Hurt, 0u);
    }
    QNP_CHECK(snapshot.boxCount == kHitboxSnapshotMaxBoxes);
    QNP_CHECK(snapshot.droppedBoxes == 5u);
}

QNP_TEST(HitboxSnapshotNeverPromisesMoreThanTheFrameHolds) {
    QNP_CHECK(static_cast<int>(kHitboxSnapshotMaxBoxes)
                  * kHitboxCommandsPerBox
              <= 192);
    QNP_CHECK(hitboxBoxesThatFit(192)
              >= static_cast<int>(kHitboxSnapshotMaxBoxes));
}

QNP_TEST(HitboxClashPointIsTheCentreOfTheGeometryThatTraded) {
    HitboxSnapshotV1 snapshot{};
    QNP_CHECK(snapshot.clashPointValid == 0u);

    hitboxNoteClashPoint(snapshot, HitboxRect{100, 50, 140, 90});
    QNP_CHECK(snapshot.clashPointValid == 1u);
    QNP_CHECK(snapshot.clashX == 120);
    QNP_CHECK(snapshot.clashY == 70);

    hitboxNoteClashPoint(snapshot, HitboxRect{160, 50, 200, 90});
    QNP_CHECK(snapshot.clashX == 150);
    QNP_CHECK(snapshot.clashY == 70);
}

QNP_TEST(HitboxClashPointLivesInTheSameSpaceAsTheBoxes) {
    const HitboxCameraV1 camera{96, 8, 3};
    const HitboxRect world{1000, 400, 1040, 440};
    const HitboxRect screen = hitboxToScreen(world, camera);

    HitboxSnapshotV1 snapshot{};
    hitboxNoteClashPoint(snapshot, screen);
    QNP_CHECK(snapshot.clashX == hitboxScreenX(1020, camera));
    QNP_CHECK(snapshot.clashY == hitboxScreenY(420, camera));
    QNP_CHECK(snapshot.clashX != 1020);
}

QNP_TEST(HitboxDrawOrderPutsAttackOnTopAndCollisionBeneath) {
    QNP_CHECK(hitboxDrawRank(HitboxBoxKind::Collision)
              < hitboxDrawRank(HitboxBoxKind::Hurt));
    QNP_CHECK(hitboxDrawRank(HitboxBoxKind::Hurt)
              < hitboxDrawRank(HitboxBoxKind::Clash));
    QNP_CHECK(hitboxDrawRank(HitboxBoxKind::Clash)
              < hitboxDrawRank(HitboxBoxKind::Attack));
    for (size_t i = 0; i < static_cast<size_t>(HitboxBoxKind::Count); ++i) {
        const HitboxBoxKind kind = static_cast<HitboxBoxKind>(i);
        if (kind == HitboxBoxKind::Attack) continue;
        QNP_CHECK(hitboxDrawRank(kind) < hitboxDrawRank(HitboxBoxKind::Attack));
    }
}
