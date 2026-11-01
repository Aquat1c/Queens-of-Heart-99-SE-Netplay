#include "TestHarness.h"
#include "RuntimeExecutableFixtures.h"

#include "../../client/BuildIdentity.h"
#include "qoh99/generated/BuildProfiles.hpp"

#include <windows.h>

#include <cstdio>
#include <string>

#ifndef QOH99_BUILT_NETPLAY_DLL
#error QOH99_BUILT_NETPLAY_DLL must identify the matched DLL build artifact
#endif

namespace {

bool requiredFixtures(qnetplay::test::RuntimeExecutableFixtures& fixtures) {
    std::string error;
    if (qnetplay::test::resolveRuntimeExecutableFixtures(fixtures, error))
        return true;
    std::fprintf(stderr, "FAIL: %s\n", error.c_str());
    return false;
}

}

QNP_TEST(BuildIdentityAcceptsOnlySupportedSeRuntime) {
    qnetplay::test::RuntimeExecutableFixtures fixtures{};
    QNP_REQUIRE(requiredFixtures(fixtures));
    qclient::ExecutableIdentity identity;
    std::string error;
    QNP_CHECK(qclient::validateSupportedRuntimeExecutable(
        fixtures.novemberExecutable.generic_u8string(), &identity, error));
    QNP_CHECK(error.empty());
    QNP_CHECK(identity.sha256 ==
              qoh99::generated_model::supportedRuntimeBuild().binarySha256);
    QNP_CHECK(identity.fileSize == 704512u);
    QNP_CHECK(identity.coffTimestamp == 0x3A19A8A9u);
    QNP_CHECK(identity.machine == 0x014Cu);
    QNP_CHECK(identity.runtimeProfileId
              == qoh99::generated_model::RuntimeProfileId::Qoh99Se1126);
    QNP_CHECK(identity.runtimeQualification
              == qoh99::generated_model::RuntimeQualification::Qualified);
    QNP_CHECK(!identity.executableIdentityEstablishesSimulationContent);
}

QNP_TEST(BuildIdentityDetectsAndAdmitsDecember) {
    qnetplay::test::RuntimeExecutableFixtures fixtures{};
    QNP_REQUIRE(requiredFixtures(fixtures));
    qclient::ExecutableIdentity identity;
    std::string error;
    QNP_CHECK(qclient::detectKnownRuntimeExecutable(
        fixtures.decemberExecutable.generic_u8string(), &identity, error));
    QNP_CHECK(error.empty());
    QNP_CHECK(identity.sha256 ==
              qoh99::generated_model::kBuilds[0].binarySha256);
    QNP_CHECK(identity.runtimeProfileId
              == qoh99::generated_model::RuntimeProfileId::
                     Qoh99DecemberCandidate);
    QNP_CHECK(identity.runtimeQualification
              == qoh99::generated_model::RuntimeQualification::Qualified);
    QNP_CHECK(!identity.executableIdentityEstablishesSimulationContent);

    error.clear();
    QNP_CHECK(qclient::validateSupportedRuntimeExecutable(
        fixtures.decemberExecutable.generic_u8string(), &identity, error));
    QNP_CHECK(error.empty());
}

QNP_TEST(BuildIdentitySeparatesNovemberAndDecember) {
    qnetplay::test::RuntimeExecutableFixtures fixtures{};
    QNP_REQUIRE(requiredFixtures(fixtures));
    qclient::ExecutableIdentity november;
    qclient::ExecutableIdentity december;
    std::string error;
    QNP_CHECK(qclient::detectKnownRuntimeExecutable(
        fixtures.novemberExecutable.generic_u8string(), &november, error));
    QNP_CHECK(qclient::detectKnownRuntimeExecutable(
        fixtures.decemberExecutable.generic_u8string(), &december, error));
    QNP_CHECK(november.sha256 != december.sha256);
    QNP_CHECK(november.runtimeProfileId != december.runtimeProfileId);
    QNP_CHECK(november.fileSize == december.fileSize);
    QNP_CHECK(november.machine == december.machine);
    QNP_CHECK(november.coffTimestamp != december.coffTimestamp);
}

QNP_TEST(NetplayDllContractAcceptsCurrentBuiltDll) {
    std::string error;
    QNP_CHECK(qclient::validateNetplayDllContract(
        QOH99_BUILT_NETPLAY_DLL, error));
    QNP_CHECK(error.empty());
}

QNP_TEST(NetplayDllContractRejectsNonDllPeBeforeMarkerScan) {
    qnetplay::test::RuntimeExecutableFixtures fixtures{};
    QNP_REQUIRE(requiredFixtures(fixtures));
    std::string error;
    QNP_CHECK(!qclient::validateNetplayDllContract(
        fixtures.novemberExecutable.generic_u8string(), error));
    QNP_CHECK(error.find("not a DLL")
              != std::string::npos);
}

QNP_TEST(NetplayDllContractRejectsMarkerBearingCasterExecutable) {
    char executable[MAX_PATH] = {};
    const DWORD length =
        GetModuleFileNameA(nullptr, executable, sizeof(executable));
    QNP_REQUIRE(length > 0 && length < sizeof(executable));

    std::string error;
    QNP_CHECK(!qclient::validateNetplayDllContract(executable, error));
    QNP_CHECK(error.find("not a DLL") != std::string::npos);
}
