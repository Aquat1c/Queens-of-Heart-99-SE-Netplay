#include "input/InputCompiler.h"

#include <cstdio>
#include <cstring>

#include "input/DeviceIdentity.h"
#include "input/ModeControlPolicy.h"

namespace qinput {
namespace {

uint8_t internProbe(InputLaunchConfig& config, ProbeKind kind,
                    uint8_t token) {
    for (uint32_t i = 0; i < config.probeCount; ++i) {
        if (config.probes[i].kind == kind
            && config.probes[i].token == token) {
            return static_cast<uint8_t>(i);
        }
    }
    if (config.probeCount >= kMaxControllerProbes) return 0xFF;
    CompiledProbe& probe = config.probes[config.probeCount];
    probe.kind = kind;
    probe.token = token;
    return static_cast<uint8_t>(config.probeCount++);
}

const CompiledAxisCalibration* findCalibration(
    const InputProfile& profile, ProbeKind kind, uint8_t token) {
    for (const ProfileAxisCalibration& entry : profile.axisCalibration) {
        if (entry.probeKind == kind && entry.axisToken == token)
            return &entry.values;
    }
    return nullptr;
}

CompiledAxisCalibration defaultCalibration(ProbeKind kind, bool unipolar) {
    CompiledAxisCalibration calibration{};
    if (unipolar) {
        calibration.minimum = 0;
        calibration.center = 0;
        calibration.maximum = 32767;
        calibration.unipolar = 1;
    } else {
        calibration.minimum = -32768;
        calibration.center = 0;
        calibration.maximum = 32767;
        calibration.unipolar = 0;
    }
    (void)kind;
    return calibration;
}

bool gamepadAxisIsTrigger(uint8_t token) {
    return gamepadAxisTokenIsTrigger(token);
}

bool modeBindingMatchesProfileNamespace(const InputProfile& profile,
                                        const ProfileBinding& binding) {
    if (binding.type == CompiledBindingType::KeyboardScan) return true;
    const bool standardized = compiledProbeNeedsStandardizedBackend(
        binding.probeKind);
    return standardized == !profile.mapping.empty();
}

bool compileBinding(const InputProfile& profile,
                    InputLaunchConfig& config,
                    const ProfileBinding& source,
                    CompiledBinding& out, std::string& error) {
    out = CompiledBinding{};
    switch (source.type) {
        case CompiledBindingType::KeyboardScan:
            out.type = CompiledBindingType::KeyboardScan;
            out.dik = source.dik;
            return true;
        case CompiledBindingType::ProbeButton: {
            const uint8_t slot =
                internProbe(config, source.probeKind, source.token);
            if (slot == 0xFF) {
                error = "compiled probe table overflow";
                return false;
            }
            out.type = CompiledBindingType::ProbeButton;
            out.probeIndex = slot;
            return true;
        }
        case CompiledBindingType::ProbeHat: {
            const uint8_t slot =
                internProbe(config, ProbeKind::RawHat, source.token);
            if (slot == 0xFF) {
                error = "compiled probe table overflow";
                return false;
            }
            out.type = CompiledBindingType::ProbeHat;
            out.probeIndex = slot;
            out.hatBit = source.hatBit;
            return true;
        }
        case CompiledBindingType::ProbeAxis: {
            const uint8_t slot =
                internProbe(config, source.probeKind, source.token);
            if (slot == 0xFF) {
                error = "compiled probe table overflow";
                return false;
            }
            out.type = CompiledBindingType::ProbeAxis;
            out.probeIndex = slot;
            out.polarity = source.polarity;
            out.pressQ15 = source.pressQ15;
            out.releaseQ15 = source.releaseQ15;
            const bool trigger =
                source.probeKind == ProbeKind::GamepadAxis
                && gamepadAxisIsTrigger(source.token);
            const CompiledAxisCalibration* calibration =
                findCalibration(profile, source.probeKind, source.token);
            out.calibration = calibration != nullptr
                ? *calibration
                : defaultCalibration(source.probeKind, trigger);
            if (out.calibration.unipolar != 0
                && out.polarity != AxisPolarity::Positive) {
                error = "unipolar axis with Negative polarity";
                return false;
            }
            return true;
        }
        case CompiledBindingType::None:
            error = "empty binding";
            return false;
    }
    error = "unknown binding type";
    return false;
}

uint64_t predicateIdentity(const ProfileBinding& binding) {
    return (static_cast<uint64_t>(binding.type) << 56)
        | (static_cast<uint64_t>(binding.probeKind) << 48)
        | (static_cast<uint64_t>(binding.dik) << 40)
        | (static_cast<uint64_t>(binding.token) << 32)
        | (static_cast<uint64_t>(binding.hatBit) << 24)
        | (static_cast<uint64_t>(binding.polarity) << 16);
}

}

CompileResult compileInputProfile(const InputProfile& profile,
                                  bool spectator,
                                  const ControlsFile* controls) {
    CompileResult result{};
    InputLaunchConfig& config = result.config;
    config = InputLaunchConfig{};

    if (spectator) {
        const char* why = validateInputLaunchConfig(config, true);
        if (why != nullptr) {
            result.error = why;
            return result;
        }
        result.modeControls = CompiledModeControlsV1{};
        result.ok = true;
        return result;
    }

    config.sourceMode = profile.sourceMode;
    config.horizontalSocd = profile.horizontalSocd;
    config.verticalSocd = profile.verticalSocd;
    config.focusPolicy = profile.focusPolicy;

    uint64_t fighterPredicates[64];
    size_t fighterPredicateCount = 0;

    for (int action = 0; action < 8; ++action) {
        const std::vector<ProfileBinding>& bindings =
            profile.actions[action];
        if (bindings.empty()) {
            result.error = std::string("no binding for action ")
                + fightActionName(static_cast<FightAction>(action));
            return result;
        }
        if (bindings.size() > kMaxBindingsPerAction) {
            result.error = "too many bindings for one action";
            return result;
        }
        CompiledActionBindings& compiled = config.actions[action];
        for (const ProfileBinding& binding : bindings) {
            const uint64_t identity = predicateIdentity(binding);
            for (size_t i = 0; i < fighterPredicateCount; ++i) {
                if (fighterPredicates[i] == identity) {
                    result.error =
                        "one physical predicate bound to two actions";
                    return result;
                }
            }
            if (fighterPredicateCount < 64)
                fighterPredicates[fighterPredicateCount++] = identity;
            CompiledBinding compiledBinding{};
            if (!compileBinding(profile, config, binding,
                                compiledBinding, result.error)) {
                return result;
            }
            compiled.bindings[compiled.count++] = compiledBinding;
        }
    }

    ControlsFile defaults{};
    if (controls == nullptr) {
        defaults.schemaVersion = kControlsSchemaVersion;
        installDefaultModeControls(defaults);
        controls = &defaults;
    }

    for (size_t descriptorIndex = 0;
         descriptorIndex < kModeControlDescriptorCount; ++descriptorIndex) {
        const ModeControlDescriptor& descriptor =
            kModeControlDescriptors[descriptorIndex];
        const auto& bindings = controls->modeControls[
            static_cast<size_t>(descriptor.action)];
        for (const ProfileBinding& binding : bindings) {
            if (!validModeControlBinding(descriptor.action, binding,
                                         result.error)) return result;
            if (modeDomainOverlapsFighter(descriptor.domain)) {
                const uint64_t identity = predicateIdentity(binding);
                for (size_t i = 0; i < fighterPredicateCount; ++i) {
                    if (fighterPredicates[i] == identity) {
                        result.error =
                            "mode control shares an active fighter predicate";
                        return result;
                    }
                }
            }
            for (size_t otherIndex = descriptorIndex + 1;
                 otherIndex < kModeControlDescriptorCount; ++otherIndex) {
                const ModeControlDescriptor& other =
                    kModeControlDescriptors[otherIndex];
                if (!modeDomainsOverlap(descriptor.domain, other.domain))
                    continue;
                for (const ProfileBinding& candidate
                     : controls->modeControls[static_cast<size_t>(
                           other.action)]) {
                    if (samePhysicalBinding(binding, candidate)) {
                        result.error =
                            "one physical predicate bound to overlapping mode controls";
                        return result;
                    }
                }
            }
        }
    }
    result.modeControls = CompiledModeControlsV1{};
    for (const ModeControlDescriptor& descriptor : kModeControlDescriptors) {
        if (descriptor.domain == ModeControlDomain::WrapperGlobal
            || descriptor.domain == ModeControlDomain::Replay) continue;
        const auto& bindings = controls->modeControls[
            static_cast<size_t>(descriptor.action)];
        for (const ProfileBinding& binding : bindings) {
            if (binding.type != CompiledBindingType::KeyboardScan
                && (profile.device.guid.empty()
                    || !modeBindingMatchesProfileNamespace(profile,
                                                           binding))) {
                continue;
            }
            if (result.modeControls.count >= kMaxModeControlBindings) {
                result.error = "compiled mode-control table overflow";
                return result;
            }
            if (!validModeControlBinding(descriptor.action, binding,
                                         result.error)) return result;
            if (modeDomainOverlapsFighter(descriptor.domain)) {
                const uint64_t identity = predicateIdentity(binding);
                for (size_t i = 0; i < fighterPredicateCount; ++i) {
                    if (fighterPredicates[i] == identity) {
                        result.error = "mode control shares an active fighter predicate";
                        return result;
                    }
                }
            }
            CompiledModeControlBindingV1& out =
                result.modeControls.bindings[result.modeControls.count];
            out.action = descriptor.action;
            if (!compileBinding(profile, config, binding,
                                out.binding, result.error)) return result;
            ++result.modeControls.count;
        }
    }

    if (config.probeCount > 0) {
        if (profile.device.guid.empty()) {
            result.error = "controller bindings without a selected device";
            return result;
        }
        std::snprintf(config.device.guid, sizeof(config.device.guid), "%s",
                      profile.device.guid.c_str());
        config.device.vendor = profile.device.vendor;
        config.device.product = profile.device.product;
        config.device.version = profile.device.version;
        std::snprintf(config.device.normalizedName,
                      sizeof(config.device.normalizedName), "%s",
                      profile.device.normalizedName.c_str());
        if (!profile.device.serial.empty()) {
            config.device.hasSerial = 1;
            config.device.serialHash =
                identityStringHash(profile.device.serial.c_str());
        } else if (profile.device.serialHash != 0) {
            config.device.hasSerial = 1;
            config.device.serialHash = profile.device.serialHash;
        }
        if (profile.device.pathFingerprint != 0) {
            config.device.hasPathFingerprint = 1;
            config.device.pathFingerprint = profile.device.pathFingerprint;
        }
        bool hasGamepadProbe = false;
        for (uint32_t i = 0; i < config.probeCount; ++i) {
            if (config.probes[i].kind == ProbeKind::GamepadButton
                || config.probes[i].kind == ProbeKind::GamepadAxis) {
                hasGamepadProbe = true;
            }
        }
        if (hasGamepadProbe) {
            if (profile.mapping.empty()) {
                result.error =
                    "standardized bindings without a mapping identity";
                return result;
            }
            if (profile.mapping.size() >= kMaxMappingStringLength) {
                result.error = "mapping string too long";
                return result;
            }
            std::memcpy(config.mapping, profile.mapping.data(),
                        profile.mapping.size());
            config.mappingLength =
                static_cast<uint32_t>(profile.mapping.size());
        }
    }

    if (config.probeCount > 0) {
        config.probeBackend =
            profile.probeBackend == "sdl" ? CompiledProbeBackend::Sdl
            : profile.probeBackend == "dinput8"
                  ? CompiledProbeBackend::DirectInput8
                  : CompiledProbeBackend::Unknown;
    }

    config.profileHash = profileContentHash(profile);

    const char* why = validateInputLaunchConfig(config, false);
    if (why != nullptr) {
        result.error = why;
        return result;
    }
    why = validateCompiledModeControls(result.modeControls, config, false);
    if (why != nullptr) {
        result.error = why;
        return result;
    }
    result.ok = true;
    return result;
}

}
