#pragma once

#include <cstddef>
#include <cstdint>

#include "input/InputLaunchConfig.h"

namespace qinput {

struct DeviceRequirements {
    bool needsStandardized = false;
    uint16_t axes = 0;
    uint16_t buttons = 0;
    uint16_t hats = 0;
};

struct BoundDeviceKey {
    bool hasSerialHash = false;
    uint64_t serialHash = 0;
    bool hasPathFingerprint = false;
    uint64_t pathFingerprint = 0;
    const char* guid = nullptr;
    uint16_t vendor = 0;
    uint16_t product = 0;
};

struct MigrationCandidate {
    bool hasSerialHash = false;
    uint64_t serialHash = 0;
    bool hasPathFingerprint = false;
    uint64_t pathFingerprint = 0;
    const char* guid = nullptr;
    uint16_t vendor = 0;
    uint16_t product = 0;
    bool isGamepad = false;
    bool selectable = false;
    uint16_t axes = 0;
    uint16_t buttons = 0;
    uint16_t hats = 0;
};

enum class MigrationVerdict : uint8_t {
    BoundDevicePresent = 0,
    NoCandidate = 1,
    Migrate = 2,
    Ambiguous = 3,
    NoCompatibleCandidate = 4,
};

struct MigrationPlan {
    MigrationVerdict verdict = MigrationVerdict::BoundDevicePresent;
    std::size_t candidateIndex = 0;
    std::size_t compatibleCount = 0;
};

inline bool migrationTextEqual(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
    if (*a == '\0' || *b == '\0') return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

inline bool migrationSameDevice(const BoundDeviceKey& bound,
                                const MigrationCandidate& candidate) {
    if (bound.hasSerialHash && candidate.hasSerialHash)
        return bound.serialHash == candidate.serialHash;
    if (bound.hasPathFingerprint && candidate.hasPathFingerprint
        && bound.pathFingerprint == candidate.pathFingerprint) {
        return migrationTextEqual(bound.guid, candidate.guid);
    }
    return false;
}

inline bool migrationCandidateSatisfies(const DeviceRequirements& need,
                                        const MigrationCandidate& candidate) {
    if (!candidate.selectable) return false;
    if (need.needsStandardized && !candidate.isGamepad) return false;
    return candidate.axes >= need.axes
        && candidate.buttons >= need.buttons
        && candidate.hats >= need.hats;
}

inline bool migrationSameDeviceClass(const BoundDeviceKey& bound,
                                     const MigrationCandidate& candidate) {
    if (bound.vendor != 0 && bound.product != 0
        && candidate.vendor != 0 && candidate.product != 0) {
        return bound.vendor == candidate.vendor
            && bound.product == candidate.product;
    }
    return migrationTextEqual(bound.guid, candidate.guid);
}

inline DeviceRequirements deviceRequirementsFromProbes(
        const CompiledProbe* probes, uint32_t count) {
    DeviceRequirements need{};
    if (probes == nullptr) return need;
    for (uint32_t i = 0; i < count; ++i) {
        const CompiledProbe& probe = probes[i];
        const uint16_t needed = static_cast<uint16_t>(probe.token) + 1u;
        switch (probe.kind) {
            case ProbeKind::GamepadButton:
            case ProbeKind::GamepadAxis:
                need.needsStandardized = true;
                break;
            case ProbeKind::RawButton:
                if (needed > need.buttons) need.buttons = needed;
                break;
            case ProbeKind::RawAxis:
                if (needed > need.axes) need.axes = needed;
                break;
            case ProbeKind::RawHat:
                if (needed > need.hats) need.hats = needed;
                break;
            case ProbeKind::None:
            default:
                break;
        }
    }
    return need;
}

inline MigrationPlan planDeviceMigration(
        const BoundDeviceKey& bound, const DeviceRequirements& need,
        const MigrationCandidate* candidates, std::size_t count) {
    MigrationPlan plan{};
    const bool boundKnown =
        bound.hasSerialHash || bound.hasPathFingerprint
        || (bound.guid != nullptr && bound.guid[0] != '\0');
    if (!boundKnown) {
        plan.verdict = MigrationVerdict::BoundDevicePresent;
        return plan;
    }
    if (candidates == nullptr || count == 0) {
        plan.verdict = MigrationVerdict::NoCandidate;
        return plan;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (migrationSameDevice(bound, candidates[i])) {
            plan.verdict = MigrationVerdict::BoundDevicePresent;
            plan.candidateIndex = i;
            return plan;
        }
    }
    std::size_t compatible = 0;
    std::size_t firstCompatible = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!need.needsStandardized
            && !migrationSameDeviceClass(bound, candidates[i])) continue;
        if (!migrationCandidateSatisfies(need, candidates[i])) continue;
        if (compatible == 0) firstCompatible = i;
        ++compatible;
    }
    plan.compatibleCount = compatible;
    if (compatible == 0) {
        bool anySelectable = false;
        for (std::size_t i = 0; i < count; ++i)
            if (candidates[i].selectable) anySelectable = true;
        plan.verdict = anySelectable ? MigrationVerdict::NoCompatibleCandidate
                                     : MigrationVerdict::NoCandidate;
        return plan;
    }
    if (compatible > 1) {
        plan.verdict = MigrationVerdict::Ambiguous;
        return plan;
    }
    plan.verdict = MigrationVerdict::Migrate;
    plan.candidateIndex = firstCompatible;
    return plan;
}

}
