#pragma once

#include "../RuntimeProfileRegistry.h"
#include "../compat/CompatibilityIdentity.h"
#include "../content/SimulationContentIdentity.h"
#include "../net/Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace qclient {

struct ExecutableIdentity {
    std::string sha256;
    std::string physicalSha256;
    std::string variantKey;
    std::uint64_t fileSize = 0;
    std::uint32_t coffTimestamp = 0;
    std::uint16_t machine = 0;
    qoh99::runtime_profile::RuntimeProfileId runtimeProfileId =
        qoh99::runtime_profile::RuntimeProfileId::Unknown;
    qoh99::runtime_profile::RuntimeQualification runtimeQualification =
        qoh99::runtime_profile::RuntimeQualification::Unknown;
    bool executableIdentityEstablishesSimulationContent = false;
};

struct RuntimeInstallationIdentity {
    ExecutableIdentity executable;
    qoh99::content::ManifestResult content;
    qnetplay::compat::CompatibilityIdentity compatibility{};
};

bool inspectExecutable(const std::string& path, ExecutableIdentity& out,
                       std::string& error);

bool detectKnownRuntimeExecutable(const std::string& path,
                                  ExecutableIdentity* identity,
                                  std::string& error);

bool validateSupportedRuntimeExecutable(const std::string& path,
                                        ExecutableIdentity* identity,
                                        std::string& error);

bool inspectRuntimeInstallation(const std::string& executablePath,
                                RuntimeInstallationIdentity& out,
                                std::string& error);

bool validateRuntimeInstallationForAdmission(
    const std::string& executablePath, RuntimeInstallationIdentity* identity,
    std::string& error);

bool validateNetplayDllContract(const std::string& path, std::string& error);

bool peFileExportsSymbols(const std::vector<unsigned char>& bytes,
                          const std::vector<std::string>& required,
                          std::string& error);

bool validatePackagedDdrawWrapper(const std::string& path, std::string& error);

bool makeRuntimeArtifactIdentity(
    const std::string& casterPath, const std::string& netplayDllPath,
    const std::string& ddrawPath, qnet::RuntimeArtifactIdentityWire& out,
    std::string& error);

}
