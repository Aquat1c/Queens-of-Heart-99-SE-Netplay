#pragma once

#include "qoh99/generated/BuildProfiles.hpp"
#include "qoh99/generated/FunctionCatalog.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qoh99::runtime_profile {

using RuntimeProfileId = generated_model::RuntimeProfileId;
using RuntimeQualification = generated_model::RuntimeQualification;
using FunctionId = generated_model::FunctionId;
using BuildProfile = generated_model::BuildProfile;
using BuildContentPreconditions = generated_model::BuildContentPreconditions;

inline const BuildProfile* findProfile(RuntimeProfileId profileId) {
    return generated_model::findBuildByRuntimeProfileId(profileId);
}

inline const BuildProfile* findProfileBySha256(const char* sha256) {
    return generated_model::findBuildBySha256(sha256);
}

inline const BuildProfile* findProfileByCoffTimestamp(
    std::uint32_t coffTimestamp) {
    return generated_model::findBuildByCoffTimestamp(coffTimestamp);
}

inline const BuildProfile* findProfileByExactExecutableEvidence(
    const char* sha256, std::uint64_t fileSize, std::uint16_t machine) {
    constexpr std::uint16_t kImageFileMachineI386 = 0x014Cu;
    const BuildProfile* profile = findProfileBySha256(sha256);
    if (!profile || fileSize != profile->binarySize
        || machine != kImageFileMachineI386) {
        return nullptr;
    }
    return profile;
}

inline bool profileAllowsHookInstall(const BuildProfile& profile) {
    return generated_model::runtimeProfileAllowsHookInstall(profile);
}

namespace detail {

inline std::string hex64(std::uint64_t value, int digits) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xFu];
        value >>= 4;
    }
    return "0x" + out;
}

}

inline std::string unsupportedRuntimeProfileReason(const BuildProfile& profile) {
    (void)profile;
    return "This copy of Queen's of Heart '99 is a known edition that netplay "
           "does not support yet, so it will not start here. There is nothing "
           "wrong with your copy of the game. The November and December "
           "editions are the supported ones.";
}

inline std::string unsupportedRuntimeProfileDiagnostic(
    const BuildProfile& profile) {
    const char* const qualification =
        profile.runtimeQualification == RuntimeQualification::Qualified
            ? "qualified" : "candidate";
    std::string detail = std::string("profile='") + profile.runtimeProfileKey
        + "' (" + profile.displayName + ") qualification="
        + qualification
        + " contentPolicy=" + profile.simulationContentManifestPolicy;
    const BuildContentPreconditions& contract = profile.contentPreconditions;
    detail += " expectedChrHeaderRevision="
        + std::to_string(contract.chrHeaderRevision)
        + " tableWordVa=" + detail::hex64(contract.chrHeaderRevisionVa, 8);
    if (contract.simulationProbePath && contract.simulationProbeSha256) {
        detail += std::string(" probe=") + contract.simulationProbePath
            + " expectedSha256=" + contract.simulationProbeSha256;
    }
    detail += " (addresses are modelled; executable identity alone never"
              " establishes simulation-content compatibility, so the content"
              " manifest is checked separately before admission)";
    return detail;
}

enum class RuntimeProfileSelectionState : std::uint8_t {
    Pending,
    Qualified,
    Rejected,
};

inline std::atomic<RuntimeProfileId>& selectedProfileSlot() {
    static std::atomic<RuntimeProfileId> selected{RuntimeProfileId::Unknown};
    return selected;
}

inline std::atomic<RuntimeProfileSelectionState>& selectionStateSlot() {
    static std::atomic<RuntimeProfileSelectionState> state{
        RuntimeProfileSelectionState::Pending};
    return state;
}

inline void clearSelectedProfile() {
    selectedProfileSlot().store(RuntimeProfileId::Unknown,
                                std::memory_order_release);
    selectionStateSlot().store(RuntimeProfileSelectionState::Pending,
                               std::memory_order_release);
}

inline void rejectSelectedProfile() {
    selectedProfileSlot().store(RuntimeProfileId::Unknown,
                                std::memory_order_release);
    selectionStateSlot().store(RuntimeProfileSelectionState::Rejected,
                               std::memory_order_release);
}

inline RuntimeProfileId selectedProfileId() {
    return selectedProfileSlot().load(std::memory_order_acquire);
}

inline RuntimeProfileSelectionState runtimeProfileSelectionState() {
    return selectionStateSlot().load(std::memory_order_acquire);
}

inline bool runtimeProfileSelectionFinalized() {
    return runtimeProfileSelectionState()
        != RuntimeProfileSelectionState::Pending;
}

inline bool runtimeProfileSelectionRejected() {
    return runtimeProfileSelectionState()
        == RuntimeProfileSelectionState::Rejected;
}

inline const BuildProfile* selectedProfile() {
    if (runtimeProfileSelectionState()
        != RuntimeProfileSelectionState::Qualified) {
        return nullptr;
    }
    const BuildProfile* profile = findProfile(selectedProfileId());
    if (!profile || !profileAllowsHookInstall(*profile)) return nullptr;
    return profile;
}

inline const char* selectedProfileKey() {
    const BuildProfile* profile = selectedProfile();
    return profile ? profile->runtimeProfileKey : "<none>";
}

inline bool selectProfileForRuntime(RuntimeProfileId profileId,
                                    std::string& error) {
    clearSelectedProfile();
    const BuildProfile* profile = findProfile(profileId);
    if (!profile) {
        error = "unknown runtime profile; no fixed-address hooks are allowed";
        rejectSelectedProfile();
        return false;
    }
    if (!profileAllowsHookInstall(*profile)) {
        error = unsupportedRuntimeProfileReason(*profile);
        rejectSelectedProfile();
        return false;
    }
    selectedProfileSlot().store(profileId, std::memory_order_release);
    selectionStateSlot().store(RuntimeProfileSelectionState::Qualified,
                               std::memory_order_release);
    error.clear();
    return true;
}

inline bool selectProfileForExactExecutableEvidence(
    const char* sha256, std::uint64_t fileSize, std::uint16_t machine,
    std::string& error) {
    clearSelectedProfile();
    const BuildProfile* profile = findProfileByExactExecutableEvidence(
        sha256, fileSize, machine);
    if (!profile) {
        error = "executable does not match an exact generated runtime profile; "
                "fixed-address hooks are not allowed";
        rejectSelectedProfile();
        return false;
    }
    return selectProfileForRuntime(profile->runtimeProfileId, error);
}

inline bool resolveFunctionForProfile(RuntimeProfileId profileId,
                                      FunctionId functionId,
                                      std::uintptr_t& out,
                                      std::string& error) {
    out = 0;
    const BuildProfile* profile = findProfile(profileId);
    if (!profile) {
        error = "unknown runtime profile; function address not resolved";
        return false;
    }
    if (functionId == FunctionId::Unknown) {
        error = std::string("runtime profile '") + profile->runtimeProfileKey
            + "' was asked to resolve an unknown function";
        return false;
    }
    const GameAddr va = generated_model::resolveFunctionVa(
        profile->id, functionId);
    if (va == 0) {
        error = std::string("runtime profile '") + profile->runtimeProfileKey
            + "' has no explicit address for the requested function";
        return false;
    }
    out = static_cast<std::uintptr_t>(va);
    error.clear();
    return true;
}

// Address hazard: every entry above 0x470510 differs by exactly 0x20 between builds, so a mis-selected profile lands mid-instruction.
inline bool resolveSelectedFunction(FunctionId functionId,
                                    std::uintptr_t& out,
                                    std::string& error) {
    if (runtimeProfileSelectionState()
        != RuntimeProfileSelectionState::Qualified) {
        out = 0;
        error = runtimeProfileSelectionRejected()
            ? "runtime profile selection was rejected"
            : "no qualified runtime profile is selected";
        return false;
    }
    const RuntimeProfileId profileId = selectedProfileId();
    const BuildProfile* profile = findProfile(profileId);
    if (profileId == RuntimeProfileId::Unknown || !profile) {
        out = 0;
        error = "no qualified runtime profile is selected";
        return false;
    }
    if (!profileAllowsHookInstall(*profile)) {
        out = 0;
        error = unsupportedRuntimeProfileReason(*profile);
        return false;
    }
    return resolveFunctionForProfile(profileId, functionId, out, error);
}

inline std::uintptr_t selectedFunctionOrZero(FunctionId functionId) {
    std::uintptr_t out = 0;
    std::string ignored;
    return resolveSelectedFunction(functionId, out, ignored) ? out : 0;
}

}
