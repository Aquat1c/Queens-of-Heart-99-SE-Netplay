#include "TestHarness.h"

#include "../BuildIdentity.h"
#include "../Launcher.h"
#include "../../ipc/IpcConfig.h"
#include "qoh99/generated/BuildProfiles.hpp"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

qipc::IpcConfig configWithSessionDir(const std::string& text,
                                     uint16_t declaredBytes) {
    qipc::IpcConfig cfg{};
    const size_t copied =
        text.size() < qipc::kSessionDirBytes ? text.size()
                                             : qipc::kSessionDirBytes;
    std::memcpy(cfg.sessionDir, text.data(), copied);
    if (copied < qipc::kSessionDirBytes) cfg.sessionDir[copied] = '\0';
    cfg.sessionDirBytes = declaredBytes;
    return cfg;
}

qipc::IpcConfig configWithSessionDir(const std::string& text) {
    return configWithSessionDir(text, static_cast<uint16_t>(text.size()));
}

constexpr uint32_t kSectionRva = 0x1000;
constexpr uint32_t kSectionFileOffset = 0x200;
constexpr uint32_t kSectionSize = 0x600;
constexpr uint32_t kSizeOfImage = 0x2000;

struct PeFixtureOptions {
    bool includeExportDirectory = true;
    bool corruptNameTableRva = false;
    bool corruptFirstNameRva = false;
    bool overstateNameCount = false;
    bool truncateSectionData = false;
};

void put32(std::vector<unsigned char>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<unsigned char> buildExportFixture(
    const std::vector<std::string>& names,
    const PeFixtureOptions& options = PeFixtureOptions{}) {
    std::vector<unsigned char> bytes(kSectionFileOffset + kSectionSize, 0);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    std::memcpy(bytes.data(), &dos, sizeof(dos));

    const size_t ntOffset = static_cast<size_t>(dos.e_lfanew);
    const DWORD signature = IMAGE_NT_SIGNATURE;
    std::memcpy(bytes.data() + ntOffset, &signature, sizeof(signature));

    IMAGE_FILE_HEADER coff{};
    coff.Machine = IMAGE_FILE_MACHINE_I386;
    coff.NumberOfSections = 1;
    coff.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    coff.Characteristics = IMAGE_FILE_DLL | IMAGE_FILE_EXECUTABLE_IMAGE
                         | IMAGE_FILE_32BIT_MACHINE;
    const size_t coffOffset = ntOffset + sizeof(signature);
    std::memcpy(bytes.data() + coffOffset, &coff, sizeof(coff));

    const uint32_t exportDirRva = kSectionRva;
    const uint32_t nameTableRva =
        exportDirRva + static_cast<uint32_t>(sizeof(IMAGE_EXPORT_DIRECTORY));
    const uint32_t stringsRva =
        nameTableRva + static_cast<uint32_t>(names.size() * sizeof(DWORD)) + 16;

    const auto rvaToOffset = [](uint32_t rva) -> size_t {
        return kSectionFileOffset + (rva - kSectionRva);
    };

    IMAGE_EXPORT_DIRECTORY exports{};
    exports.Name = stringsRva;
    exports.Base = 1;
    exports.NumberOfFunctions = static_cast<DWORD>(names.size());
    exports.NumberOfNames = static_cast<DWORD>(
        options.overstateNameCount ? names.size() + 4096 : names.size());
    exports.AddressOfFunctions = nameTableRva;
    exports.AddressOfNames =
        options.corruptNameTableRva ? 0x7FFFFFFFu : nameTableRva;
    exports.AddressOfNameOrdinals = nameTableRva;
    std::memcpy(bytes.data() + rvaToOffset(exportDirRva), &exports,
                sizeof(exports));

    uint32_t cursorRva = stringsRva;
    for (size_t i = 0; i < names.size(); ++i) {
        const uint32_t pointerRva =
            (options.corruptFirstNameRva && i == 0) ? 0x7FFFFFFFu : cursorRva;
        put32(bytes, rvaToOffset(nameTableRva + i * sizeof(DWORD)),
              pointerRva);
        std::memcpy(bytes.data() + rvaToOffset(cursorRva), names[i].data(),
                    names[i].size());
        cursorRva += static_cast<uint32_t>(names[i].size()) + 1;
    }

    IMAGE_OPTIONAL_HEADER32 optional{};
    optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    optional.SectionAlignment = 0x1000;
    optional.FileAlignment = 0x200;
    optional.SizeOfImage = kSizeOfImage;
    optional.SizeOfHeaders = kSectionFileOffset;
    optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    if (options.includeExportDirectory) {
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress =
            exportDirRva;
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size =
            kSectionSize;
    }
    const size_t optionalOffset = coffOffset + sizeof(coff);
    std::memcpy(bytes.data() + optionalOffset, &optional, sizeof(optional));

    IMAGE_SECTION_HEADER section{};
    std::memcpy(section.Name, ".rdata", 6);
    section.Misc.VirtualSize = kSectionSize;
    section.VirtualAddress = kSectionRva;
    section.SizeOfRawData = kSectionSize;
    section.PointerToRawData = kSectionFileOffset;
    section.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA
                            | IMAGE_SCN_MEM_READ;
    std::memcpy(bytes.data() + optionalOffset + sizeof(optional), &section,
                sizeof(section));

    if (options.truncateSectionData) bytes.resize(kSectionFileOffset);
    return bytes;
}

const std::vector<std::string>& requiredPacingExports() {
    static const std::vector<std::string> required = {
        "EFZDDRAW_GetNetplayPacingAbi",
        "EFZDDRAW_SetFrameLimit",
    };
    return required;
}

}

QNP_TEST(SessionDirExactlyAtTheLimitIsAccepted) {
    const std::string longest(qipc::kSessionDirBytes - 1, 'a');
    const qipc::IpcConfig cfg = configWithSessionDir(longest);
    QNP_CHECK(cfg.sessionDirBytes == qipc::kSessionDirBytes - 1);
    QNP_CHECK(qipc::validateSessionDir(cfg) == nullptr);
}

QNP_TEST(SessionDirOneByteOverTheLimitIsRejected) {
    const qipc::IpcConfig cfg = configWithSessionDir(
        std::string(qipc::kSessionDirBytes, 'a'),
        static_cast<uint16_t>(qipc::kSessionDirBytes));
    const char* reason = qipc::validateSessionDir(cfg);
    QNP_REQUIRE(reason != nullptr);
    QNP_CHECK(std::string(reason).find("exceeds") != std::string::npos);
}

QNP_TEST(SessionDirWithEmbeddedNulIsRejected) {
    std::string path = "C:\\logs\\session_1";
    path[8] = '\0';
    qipc::IpcConfig cfg = configWithSessionDir(path);
    const char* reason = qipc::validateSessionDir(cfg);
    QNP_REQUIRE(reason != nullptr);
    QNP_CHECK(std::string(reason).find("embedded NUL") != std::string::npos);
}

QNP_TEST(SessionDirLengthMustAgreeWithTheTerminator) {
    const qipc::IpcConfig cfg = configWithSessionDir("C:\\logs\\session_1", 4);
    const char* reason = qipc::validateSessionDir(cfg);
    QNP_REQUIRE(reason != nullptr);
    QNP_CHECK(std::string(reason).find("terminated") != std::string::npos);
}

QNP_TEST(SessionDirEmptyIsStructurallyValid) {
    const qipc::IpcConfig cfg{};
    QNP_CHECK(qipc::validateSessionDir(cfg) == nullptr);
}

QNP_TEST(NativeTimeoutsAreUnchanged) {
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(qclient::kInjectTimeoutMs, false)
              == qclient::kInjectTimeoutMs);
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(qclient::kPipeConnectTimeoutMs,
                                            false)
              == qclient::kPipeConnectTimeoutMs);
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(qclient::kConfigWriteTimeoutMs,
                                            false)
              == qclient::kConfigWriteTimeoutMs);
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(qclient::kStageAReadyTimeoutMs,
                                            false)
              == qclient::kStageAReadyTimeoutMs);
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(qclient::kStageBInputTimeoutMs,
                                            false)
              == qclient::kStageBInputTimeoutMs);
}

QNP_TEST(WineTimeoutsAreLongerButBounded) {
    const DWORD phases[] = {
        qclient::kInjectTimeoutMs, qclient::kPipeConnectTimeoutMs,
        qclient::kConfigWriteTimeoutMs, qclient::kStageAReadyTimeoutMs,
        qclient::kStageBInputTimeoutMs,
    };
    for (const DWORD base : phases) {
        const DWORD scaled = qclient::scaleLaunchTimeoutMs(base, true);
        QNP_CHECK(scaled > base);
        QNP_CHECK(scaled == base * qclient::kColdPrefixTimeoutScale);
        QNP_CHECK(scaled <= qclient::kMaxPhaseTimeoutMs);
        QNP_CHECK(scaled != INFINITE);
    }
}

QNP_TEST(WineTimeoutScalingSaturatesAtTheCeiling) {
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(qclient::kMaxPhaseTimeoutMs, true)
              == qclient::kMaxPhaseTimeoutMs);
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(0xFFFFFFFEu, true)
              == qclient::kMaxPhaseTimeoutMs);
}

QNP_TEST(WineTimeoutScalingPassesThroughSentinels) {
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(0, true) == 0);
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(INFINITE, true) == INFINITE);
    QNP_CHECK(qclient::scaleLaunchTimeoutMs(INFINITE, false) == INFINITE);
}

QNP_TEST(HeartbeatIsShorterThanEveryPhase) {
    QNP_CHECK(qclient::kPhaseHeartbeatMs < qclient::kConfigWriteTimeoutMs);
    QNP_CHECK(qclient::kPhaseHeartbeatMs > 0);
}

QNP_TEST(DdrawExportCheckAcceptsBothPacingSymbols) {
    const std::vector<unsigned char> image = buildExportFixture(
        {"EFZDDRAW_GetNetplayPacingAbi", "EFZDDRAW_SetFrameLimit",
         "DirectDrawCreate"});
    std::string error;
    QNP_CHECK(qclient::peFileExportsSymbols(image, requiredPacingExports(),
                                            error));
    QNP_CHECK(error.empty());
}

QNP_TEST(DdrawExportCheckNamesTheMissingSymbol) {
    const std::vector<unsigned char> image =
        buildExportFixture({"EFZDDRAW_GetNetplayPacingAbi",
                            "DirectDrawCreate"});
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(image, requiredPacingExports(),
                                             error));
    QNP_CHECK(error.find("EFZDDRAW_SetFrameLimit") != std::string::npos);
}

QNP_TEST(DdrawExportCheckRejectsABuiltinLookalike) {
    const std::vector<unsigned char> image = buildExportFixture(
        {"DirectDrawCreate", "DirectDrawCreateEx", "DirectDrawEnumerateA"});
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(image, requiredPacingExports(),
                                             error));
    QNP_CHECK(error.find("missing export") != std::string::npos);
}

QNP_TEST(DdrawExportCheckRejectsAnImageWithoutAnExportDirectory) {
    PeFixtureOptions options;
    options.includeExportDirectory = false;
    const std::vector<unsigned char> image =
        buildExportFixture({"EFZDDRAW_SetFrameLimit"}, options);
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(image, requiredPacingExports(),
                                             error));
    QNP_CHECK(error.find("export directory") != std::string::npos);
}

QNP_TEST(DdrawExportCheckRejectsAnOutOfImageNameTable) {
    PeFixtureOptions options;
    options.corruptNameTableRva = true;
    const std::vector<unsigned char> image = buildExportFixture(
        {"EFZDDRAW_GetNetplayPacingAbi", "EFZDDRAW_SetFrameLimit"}, options);
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(image, requiredPacingExports(),
                                             error));
    QNP_CHECK(error.find("outside the image") != std::string::npos);
}

QNP_TEST(DdrawExportCheckRejectsAnOutOfImageNamePointer) {
    PeFixtureOptions options;
    options.corruptFirstNameRva = true;
    const std::vector<unsigned char> image = buildExportFixture(
        {"EFZDDRAW_GetNetplayPacingAbi", "EFZDDRAW_SetFrameLimit"}, options);
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(image, requiredPacingExports(),
                                             error));
    QNP_CHECK(error.find("outside the image") != std::string::npos);
}

QNP_TEST(DdrawExportCheckRejectsAnOverstatedNameCount) {
    PeFixtureOptions options;
    options.overstateNameCount = true;
    const std::vector<unsigned char> image = buildExportFixture(
        {"EFZDDRAW_GetNetplayPacingAbi", "EFZDDRAW_SetFrameLimit"}, options);
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(image, requiredPacingExports(),
                                             error));
    QNP_CHECK(error.find("outside the image") != std::string::npos);
}

QNP_TEST(DdrawExportCheckRejectsATruncatedImage) {
    PeFixtureOptions options;
    options.truncateSectionData = true;
    const std::vector<unsigned char> image = buildExportFixture(
        {"EFZDDRAW_GetNetplayPacingAbi", "EFZDDRAW_SetFrameLimit"}, options);
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(image, requiredPacingExports(),
                                             error));
    QNP_CHECK(!error.empty());
}

QNP_TEST(DdrawExportCheckRejectsNonPeInput) {
    const std::vector<unsigned char> notAPe(4096, 0x41);
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(notAPe, requiredPacingExports(),
                                             error));
    QNP_CHECK(error.find("PE") != std::string::npos);
}

QNP_TEST(DdrawExportCheckRejectsAnEmptyBuffer) {
    const std::vector<unsigned char> empty;
    std::string error;
    QNP_CHECK(!qclient::peFileExportsSymbols(empty, requiredPacingExports(),
                                             error));
    QNP_CHECK(!error.empty());
}

namespace {

std::vector<unsigned char> minimalPeFile(uint16_t machine,
                                        uint32_t timestamp,
                                        size_t totalBytes,
                                        unsigned char filler) {
    std::vector<unsigned char> image(totalBytes, filler);
    const uint32_t ntOffset = 0x80;
    image[0] = 'M';
    image[1] = 'Z';
    std::memcpy(&image[0x3C], &ntOffset, sizeof(ntOffset));
    image[ntOffset + 0] = 'P';
    image[ntOffset + 1] = 'E';
    image[ntOffset + 2] = 0;
    image[ntOffset + 3] = 0;
    std::memcpy(&image[ntOffset + 4], &machine, sizeof(machine));
    std::memcpy(&image[ntOffset + 8], &timestamp, sizeof(timestamp));
    return image;
}

class TempPeFile {
public:
    TempPeFile(const char* stem, const std::vector<unsigned char>& bytes) {
        char dir[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, dir);
        _path = std::string(dir) + "qoh99_gamecheck_" + stem + ".exe";
        HANDLE h = CreateFileA(_path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                      &written, nullptr);
            CloseHandle(h);
            _ok = written == bytes.size();
        }
    }
    ~TempPeFile() { if (!_path.empty()) DeleteFileA(_path.c_str()); }
    const std::string& path() const { return _path; }
    bool ok() const { return _ok; }

private:
    std::string _path;
    bool _ok = false;
};

}

QNP_TEST(GameCheckRejectsA64BitExecutableAsUnattachable) {
    const TempPeFile fixture(
        "x64", minimalPeFile(0x8664u, 0x69F7FFB2u, 4096, 0x11));
    QNP_CHECK(fixture.ok());
    std::string error;
    QNP_CHECK(!qclient::validateSupportedRuntimeExecutable(
        fixture.path(), nullptr, error));
    QNP_CHECK(error.find("64-bit") != std::string::npos);
    QNP_CHECK(error.find("cannot attach") != std::string::npos);
    QNP_CHECK(error.find("SHA-256") == std::string::npos);
}

QNP_TEST(GameCheckRejectsAnUnknown32BitExecutableWithBothHashes) {
    const TempPeFile fixture(
        "unknown", minimalPeFile(IMAGE_FILE_MACHINE_I386, 0x3A19A8A9u, 4096,
                                 0x22));
    QNP_CHECK(fixture.ok());
    std::string error;
    QNP_CHECK(!qclient::validateSupportedRuntimeExecutable(
        fixture.path(), nullptr, error));
    QNP_CHECK(error.find("does not match either supported edition")
              != std::string::npos);
    QNP_CHECK(error.find("Supported editions") != std::string::npos);
    QNP_CHECK(error.find("Found") != std::string::npos);
    QNP_CHECK(error.find(qoh99::generated_model::supportedRuntimeBuild()
                             .binarySha256)
              != std::string::npos);
    const auto* december = qoh99::generated_model::findBuildByRuntimeProfileId(
        qoh99::generated_model::RuntimeProfileId::Qoh99DecemberCandidate);
    QNP_REQUIRE(december != nullptr);
    QNP_CHECK(error.find(december->binarySha256) != std::string::npos);
}

QNP_TEST(GameCheckReportsTheSupportedBuildIdentityItRequires) {
    const auto& supported = qoh99::generated_model::supportedRuntimeBuild();
    QNP_CHECK(supported.binarySize == 704512ull);
    QNP_CHECK(std::string(supported.key) == "se_2006");
    QNP_CHECK(std::string(supported.binarySha256).size() == 64u);
    const auto* other = qoh99::generated_model::findBuildBySha256(
        "C41E755E6609B39749716FEE02D3A393EF8A11F9D8BF84419AA9131B5E91EB63");
    QNP_CHECK(other != nullptr);
    if (other) {
        QNP_CHECK(other->binarySize == supported.binarySize);
        QNP_CHECK(std::string(other->key) != std::string(supported.key));
        QNP_CHECK(qoh99::generated_model::runtimeProfileAllowsHookInstall(
            *other));
    }
}
