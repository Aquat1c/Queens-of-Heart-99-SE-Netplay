#pragma once

#include <cstdint>

namespace qtraining {

// Body, hurt and secondary ranges live in the 8-byte array at actor+32 (sub_462AC0); attack records are the 24-byte array at actor+40 (sub_462B70).
enum class HitboxBoxSource : uint8_t {
    Body      = 0,  // frame +0x17 offset, +0x18 count
    Hurt      = 1,  // frame +0x19 offset, +0x1A count
    Secondary = 2,  // frame +0x1D offset, +0x1E count -- never drawn by F7
    Attack    = 3,  // frame +0x1F offset, +0x20 count
    Count     = 4,
};

enum class HitboxBoxKind : uint8_t {
    Collision = 0,
    Hurt      = 1,
    Attack    = 2,
    Inert     = 3,
    Clash     = 4,
    Reflect   = 5,
    Capture   = 6,
    Count     = 7,
};

// Reaction selector at attack-record +18: 13/14 route to sub_464CC0, 15 to sub_464AA0.
constexpr HitboxBoxKind attackRecordKind(const uint8_t reactionSelector,
                                         const bool attackLatchSet) {
    return reactionSelector == 15u ? HitboxBoxKind::Capture
         : (reactionSelector == 13u || reactionSelector == 14u)
               ? HitboxBoxKind::Reflect
         : attackLatchSet ? HitboxBoxKind::Attack
                          : HitboxBoxKind::Inert;
}

constexpr HitboxBoxKind boxKindForSource(const HitboxBoxSource source,
                                         const uint8_t reactionSelector,
                                         const bool attackLatchSet) {
    return source == HitboxBoxSource::Body      ? HitboxBoxKind::Collision
         : source == HitboxBoxSource::Hurt      ? HitboxBoxKind::Hurt
         : source == HitboxBoxSource::Secondary ? HitboxBoxKind::Clash
         : attackRecordKind(reactionSelector, attackLatchSet);
}

// Count sentinels: 0 none, 1..253 literal, 254 explicit no-collision, 255 a one-record probe.
constexpr uint8_t hitboxRecordsToDraw(const uint8_t authoredCount) {
    return authoredCount == 0u    ? 0u
         : authoredCount == 254u  ? 0u
         : authoredCount == 255u  ? 1u
                                  : authoredCount;
}

// Attack count (frame +0x20) of 0 or >= 254 means no attack geometry: sub_467EA0 resolves the attacker through the secondary list.
constexpr uint8_t hitboxRecordsToDrawFor(const HitboxBoxSource source,
                                         const uint8_t authoredCount) {
    return (source == HitboxBoxSource::Attack && authoredCount >= 254u)
        ? 0u
        : hitboxRecordsToDraw(authoredCount);
}

// 0xAARRGGBB, matching the wrapper's overlay command colour word.
struct HitboxPalette {
    uint32_t outlineArgb = 0u;
    uint32_t fillArgb = 0u;
};

constexpr uint8_t kHitboxFillAlpha = 63u;
constexpr uint8_t kHitboxInertFillAlpha = 0u;

constexpr uint32_t hitboxArgb(const uint32_t rgb, const uint8_t alpha) {
    return (rgb & 0x00FFFFFFu) | (static_cast<uint32_t>(alpha) << 24);
}

constexpr uint32_t kHitboxRgbCollision = 0x00FFFFFFu;
constexpr uint32_t kHitboxRgbHurt      = 0x0000DC00u;
constexpr uint32_t kHitboxRgbAttack    = 0x00FF2828u;
constexpr uint32_t kHitboxRgbInert     = 0x00803030u;
constexpr uint32_t kHitboxRgbClash     = 0x00FFFF28u;
constexpr uint32_t kHitboxRgbReflect   = 0x0028C8FFu;
constexpr uint32_t kHitboxRgbCapture   = 0x00FF28FFu;

constexpr uint32_t hitboxBaseRgb(const HitboxBoxKind kind) {
    return kind == HitboxBoxKind::Collision ? kHitboxRgbCollision
         : kind == HitboxBoxKind::Hurt      ? kHitboxRgbHurt
         : kind == HitboxBoxKind::Inert     ? kHitboxRgbInert
         : kind == HitboxBoxKind::Clash     ? kHitboxRgbClash
         : kind == HitboxBoxKind::Reflect   ? kHitboxRgbReflect
         : kind == HitboxBoxKind::Capture   ? kHitboxRgbCapture
                                            : kHitboxRgbAttack;
}

constexpr uint8_t hitboxFillAlphaFor(const HitboxBoxKind kind) {
    return kind == HitboxBoxKind::Inert ? kHitboxInertFillAlpha
                                        : kHitboxFillAlpha;
}

constexpr HitboxPalette hitboxPaletteFor(const HitboxBoxKind kind) {
    const uint32_t rgb = hitboxBaseRgb(kind);
    return HitboxPalette{hitboxArgb(rgb, 255u),
                         hitboxArgb(rgb, hitboxFillAlphaFor(kind))};
}

constexpr int kHitboxCommandsPerBox = 2;

constexpr int hitboxBoxesThatFit(const int commandCapacity) {
    return commandCapacity <= 0 ? 0 : commandCapacity / kHitboxCommandsPerBox;
}

struct HitboxRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};

constexpr int32_t kHitboxMirrorAxisOffset = 128;

constexpr HitboxRect hitboxResolveRect(const int32_t actorX,
                                       const int32_t actorY,
                                       const int16_t left,
                                       const int16_t top,
                                       const int16_t right,
                                       const int16_t bottom,
                                       const bool facingLeft) {
    return facingLeft
        ? HitboxRect{actorX - right + 255, actorY + top,
                     actorX - left + 256,  actorY + bottom + 1}
        : HitboxRect{actorX + left,        actorY + top,
                     actorX + right + 1,   actorY + bottom + 1};
}

// Overlay coordinates are in the game's 320x240 surface space, not a 640x480 canvas.
constexpr float kHitboxVirtualWidth = 320.0f;
constexpr float kHitboxVirtualHeight = 240.0f;

// dword_626E80 (init 96) and dword_626E84 (init 0) are the camera scroll; flt_87D97C is the float Y shift the sprite path also adds.
struct HitboxCameraV1 {
    int32_t scrollX = 0;
    int32_t scrollY = 0;
    int32_t groundOffset = 0;
};

constexpr int32_t hitboxScreenX(const int32_t worldX,
                                const HitboxCameraV1& camera) {
    return worldX - camera.scrollX;
}

constexpr int32_t hitboxScreenY(const int32_t worldY,
                                const HitboxCameraV1& camera) {
    return worldY + camera.scrollY + camera.groundOffset;
}

constexpr HitboxRect hitboxToScreen(const HitboxRect& world,
                                    const HitboxCameraV1& camera) {
    return HitboxRect{hitboxScreenX(world.left, camera),
                      hitboxScreenY(world.top, camera),
                      hitboxScreenX(world.right, camera),
                      hitboxScreenY(world.bottom, camera)};
}

constexpr int32_t kHitboxCullMargin = 64;

constexpr bool hitboxRectVisible(const HitboxRect& screen) {
    return screen.right > -kHitboxCullMargin
        && screen.left < static_cast<int32_t>(kHitboxVirtualWidth)
                             + kHitboxCullMargin
        && screen.bottom > -kHitboxCullMargin
        && screen.top < static_cast<int32_t>(kHitboxVirtualHeight)
                            + kHitboxCullMargin;
}

constexpr bool hitboxRectWellFormed(const HitboxRect& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

// actor+160: bit 0 is the active latch (set from frame byte +0x0E bit 0x10); 0x20 is set by sub_41E120 on both actors on a clash.
constexpr uint16_t kHitboxContactAttackActive = 0x0001u;
constexpr uint16_t kHitboxContactHit          = 0x0010u;
constexpr uint16_t kHitboxContactClash        = 0x0020u;
constexpr uint16_t kHitboxContactGuard        = 0x0040u;

constexpr bool hitboxAttackActive(const uint16_t contactFlags) {
    return (contactFlags & kHitboxContactAttackActive) != 0u;
}
constexpr bool hitboxContactHit(const uint16_t contactFlags) {
    return (contactFlags & kHitboxContactHit) != 0u;
}
constexpr bool hitboxContactClash(const uint16_t contactFlags) {
    return (contactFlags & kHitboxContactClash) != 0u;
}
constexpr bool hitboxContactGuard(const uint16_t contactFlags) {
    return (contactFlags & kHitboxContactGuard) != 0u;
}

enum class HitboxDisplayMode : uint8_t {
    Off     = 0,
    Custom  = 1,
    Vanilla = 2,  // QOH's own renderer via byte 0x4AA424
};

constexpr uint8_t kHitboxDisplayModeMax = 2u;

constexpr bool hitboxDisplayModeValid(const uint32_t value) {
    return value <= kHitboxDisplayModeMax;
}

constexpr HitboxDisplayMode hitboxDisplayModeFrom(const uint32_t value) {
    return hitboxDisplayModeValid(value)
        ? static_cast<HitboxDisplayMode>(value)
        : HitboxDisplayMode::Off;
}

constexpr HitboxDisplayMode kHitboxDefaultRenderer = HitboxDisplayMode::Custom;

constexpr HitboxDisplayMode hitboxRendererPreference(
        const HitboxDisplayMode preferred) {
    return preferred == HitboxDisplayMode::Vanilla ? HitboxDisplayMode::Vanilla
                                                   : kHitboxDefaultRenderer;
}

constexpr HitboxDisplayMode hitboxToggled(const HitboxDisplayMode current,
                                          const HitboxDisplayMode preferred) {
    return current == HitboxDisplayMode::Off
        ? hitboxRendererPreference(preferred)
        : HitboxDisplayMode::Off;
}

constexpr bool hitboxModeWantsVanillaFlag(const HitboxDisplayMode mode) {
    return mode == HitboxDisplayMode::Vanilla;
}

constexpr bool hitboxModeWantsCustomOverlay(const HitboxDisplayMode mode) {
    return mode == HitboxDisplayMode::Custom;
}

// QOH gates on == 1 exactly, so any other non-zero value disables its display.
constexpr uint8_t hitboxVanillaFlagValue(const HitboxDisplayMode mode) {
    return hitboxModeWantsVanillaFlag(mode) ? 1u : 0u;
}

static_assert(!(hitboxModeWantsVanillaFlag(HitboxDisplayMode::Custom)
                && hitboxModeWantsCustomOverlay(HitboxDisplayMode::Custom)),
              "custom mode must not raise QOH's own display flag");
static_assert(!(hitboxModeWantsVanillaFlag(HitboxDisplayMode::Vanilla)
                && hitboxModeWantsCustomOverlay(HitboxDisplayMode::Vanilla)),
              "vanilla mode must not also publish our overlay");

constexpr uint32_t kHitboxApiVersion = 1u;
constexpr uint32_t kHitboxSnapshotMaxBoxes = 88u;

constexpr uint8_t kHitboxOwnerPool = 0xFEu;
constexpr uint8_t kHitboxOwnerUnknown = 0xFFu;

struct HitboxBoxV1 {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    uint8_t kind = 0;
    uint8_t owner = kHitboxOwnerUnknown;
    uint8_t reserved0 = 0;
    uint8_t reserved1 = 0;
};

static_assert(sizeof(HitboxBoxV1) == 12u,
              "hitbox box must stay a 12-byte POD for the seqlock word array");

struct HitboxSnapshotV1 {
    uint32_t apiVersion = kHitboxApiVersion;
    uint32_t structSize = 0u;
    uint32_t boxCapacity = kHitboxSnapshotMaxBoxes;
    uint32_t mode = static_cast<uint32_t>(HitboxDisplayMode::Off);
    uint32_t visible = 0u;
    uint32_t boxCount = 0u;
    uint32_t droppedBoxes = 0u;
    uint32_t clashMask = 0u;
    uint32_t hitMask = 0u;
    uint32_t guardMask = 0u;
    uint32_t activeMask = 0u;
    uint32_t frameSerial = 0u;
    int32_t clashX = 0;
    int32_t clashY = 0;
    uint32_t clashPointValid = 0u;
    uint32_t reserved = 0u;
    HitboxBoxV1 boxes[kHitboxSnapshotMaxBoxes] = {};
};

static_assert(sizeof(HitboxSnapshotV1) % sizeof(uint32_t) == 0u,
              "hitbox snapshot must be a u32 word sequence");

static_assert(kHitboxSnapshotMaxBoxes * kHitboxCommandsPerBox <= 192,
              "snapshot cannot promise more boxes than the overlay frame holds");

constexpr bool hitboxSnapshotHasRoom(const HitboxSnapshotV1& snapshot) {
    return snapshot.boxCount < kHitboxSnapshotMaxBoxes;
}

constexpr int16_t hitboxClampCoord(const int32_t value) {
    return value < -32768 ? static_cast<int16_t>(-32768)
         : value > 32767  ? static_cast<int16_t>(32767)
                          : static_cast<int16_t>(value);
}

inline bool hitboxSnapshotAppend(HitboxSnapshotV1& snapshot,
                                 const HitboxRect& screen,
                                 const HitboxBoxKind kind,
                                 const uint8_t owner) {
    if (!hitboxRectWellFormed(screen) || !hitboxRectVisible(screen))
        return false;
    if (!hitboxSnapshotHasRoom(snapshot)) {
        ++snapshot.droppedBoxes;
        return false;
    }
    HitboxBoxV1& box = snapshot.boxes[snapshot.boxCount++];
    box.x = hitboxClampCoord(screen.left);
    box.y = hitboxClampCoord(screen.top);
    box.w = hitboxClampCoord(screen.right - screen.left);
    box.h = hitboxClampCoord(screen.bottom - screen.top);
    box.kind = static_cast<uint8_t>(kind);
    box.owner = owner;
    box.reserved0 = 0u;
    box.reserved1 = 0u;
    return true;
}

inline void hitboxNoteClashPoint(HitboxSnapshotV1& snapshot,
                                 const HitboxRect& screen) {
    const int32_t centreX = screen.left + (screen.right - screen.left) / 2;
    const int32_t centreY = screen.top + (screen.bottom - screen.top) / 2;
    if (!snapshot.clashPointValid) {
        snapshot.clashX = centreX;
        snapshot.clashY = centreY;
        snapshot.clashPointValid = 1u;
        return;
    }
    snapshot.clashX = (snapshot.clashX + centreX) / 2;
    snapshot.clashY = (snapshot.clashY + centreY) / 2;
}

constexpr int hitboxDrawRank(const HitboxBoxKind kind) {
    return kind == HitboxBoxKind::Collision ? 0
         : kind == HitboxBoxKind::Hurt      ? 1
         : kind == HitboxBoxKind::Inert     ? 2
         : kind == HitboxBoxKind::Clash     ? 3
         : kind == HitboxBoxKind::Reflect   ? 4
         : kind == HitboxBoxKind::Capture   ? 5
                                            : 6;
}

}
