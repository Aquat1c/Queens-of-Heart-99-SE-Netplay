#include "../../netplay/tests/TestHarness.h"

#include "../WineSocketRepair.h"
#include "../CompatEnv.h"
#include "../CompatPreamble.h"
#include "../PeImage.h"
#include "../WidePath.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace qplat;

std::wstring merged(const wchar_t* input) {
    std::wstring out = L"<untouched>";
    if (!mergeDdrawOverrideValue(input, out)) return L"<failed>";
    return out;
}

std::vector<std::wstring> blockEntries(const std::vector<wchar_t>& block) {
    std::vector<std::wstring> entries;
    size_t index = 0;
    while (index < block.size() && block[index] != L'\0') {
        const std::wstring entry(&block[index]);
        entries.push_back(entry);
        index += entry.size() + 1;
    }
    return entries;
}

bool blockIsDoubleNulTerminated(const std::vector<wchar_t>& block) {
    return block.size() >= 2
        && block[block.size() - 1] == L'\0'
        && block[block.size() - 2] == L'\0';
}

constexpr uint32_t kNtOffset = 0x40;
constexpr size_t kFixtureBytes = 0x200;

template <typename T>
void poke(std::vector<uint8_t>& image, size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof(T));
}

size_t fileHeaderOffset() { return kNtOffset + sizeof(DWORD); }

size_t machineOffset() {
    return fileHeaderOffset() + offsetof(IMAGE_FILE_HEADER, Machine);
}

size_t characteristicsOffset() {
    return fileHeaderOffset() + offsetof(IMAGE_FILE_HEADER, Characteristics);
}

size_t sizeOfOptionalHeaderOffset() {
    return fileHeaderOffset()
         + offsetof(IMAGE_FILE_HEADER, SizeOfOptionalHeader);
}

size_t optionalHeaderOffset() {
    return fileHeaderOffset() + sizeof(IMAGE_FILE_HEADER);
}

std::vector<uint8_t> minimalPe32() {
    std::vector<uint8_t> image(kFixtureBytes, 0);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = static_cast<LONG>(kNtOffset);
    std::memcpy(image.data(), &dos, sizeof(dos));

    IMAGE_NT_HEADERS32 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt.FileHeader.NumberOfSections = 1;
    nt.FileHeader.TimeDateStamp = 0x44556677u;
    nt.FileHeader.SizeOfOptionalHeader =
        static_cast<WORD>(sizeof(IMAGE_OPTIONAL_HEADER32));
    nt.FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt.OptionalHeader.AddressOfEntryPoint = 0x1234u;
    nt.OptionalHeader.SizeOfImage = 0x8000u;
    nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
    nt.OptionalHeader.CheckSum = 0xA1B2C3D4u;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    std::memcpy(image.data() + kNtOffset, &nt, sizeof(nt));
    return image;
}

bool rejectsFileImage(const std::vector<uint8_t>& image) {
    PeImageInfo info{};
    std::string error;
    const bool accepted =
        inspectFileImage(image.data(), image.size(), info, error);
    return !accepted && !error.empty() && info.sizeOfImage == 0;
}

}

QNP_TEST(PlatformDdrawOverrideCreatesRuleFromNothing) {
    QNP_CHECK(merged(L"") == L"ddraw=n,b");
    QNP_CHECK(merged(L";") == L"ddraw=n,b");
    QNP_CHECK(merged(L";;;") == L"ddraw=n,b");
}

QNP_TEST(PlatformDdrawOverridePreservesUnrelatedRulesInOrder) {
    QNP_CHECK(merged(L"comdlg32=n,b;shell32=b")
              == L"comdlg32=n,b;shell32=b;ddraw=n,b");
    QNP_CHECK(merged(L"winmm=n;dinput8=n,b;msvcrt=b;xaudio2_7=n")
              == L"winmm=n;dinput8=n,b;msvcrt=b;xaudio2_7=n;ddraw=n,b");
    QNP_CHECK(merged(L"winemenubuilder.exe=")
              == L"winemenubuilder.exe=;ddraw=n,b");
    QNP_CHECK(merged(L"comctl32=n,b;ddraw=b;shell32=n")
              == L"comctl32=n,b;shell32=n;ddraw=n,b");
}

QNP_TEST(PlatformDdrawOverrideReplacesExistingRulesRegardlessOfCase) {
    QNP_CHECK(merged(L"ddraw=b") == L"ddraw=n,b");
    QNP_CHECK(merged(L"DDRAW=N,B") == L"ddraw=n,b");
    QNP_CHECK(merged(L"DdRaW.dLl=b") == L"ddraw=n,b");
    QNP_CHECK(merged(L"ddraw=") == L"ddraw=n,b");
}

QNP_TEST(PlatformDdrawOverrideSplitsGroupedRules) {
    QNP_CHECK(merged(L"ddraw,ddrawex=b") == L"ddrawex=b;ddraw=n,b");
    QNP_CHECK(merged(L"ddrawex,ddraw=b") == L"ddrawex=b;ddraw=n,b");
    QNP_CHECK(merged(L"comdlg32,ddraw,shell32=n,b")
              == L"comdlg32,shell32=n,b;ddraw=n,b");
    QNP_CHECK(merged(L"ddraw,ddraw.dll=b") == L"ddraw=n,b");
}

QNP_TEST(PlatformDdrawOverrideCollapsesDuplicatesToExactlyOneRule) {
    const std::wstring out = merged(L"ddraw=b;winmm=n;ddraw.dll=n;DDRAW=b");
    QNP_CHECK(out == L"winmm=n;ddraw=n,b");
    QNP_REQUIRE(out.size() >= 9);
    QNP_CHECK(out.rfind(L"ddraw=n,b") == out.size() - 9);
    QNP_CHECK(out.find(L"ddraw=n,b") == out.rfind(L"ddraw=n,b"));
}

QNP_TEST(PlatformDdrawOverrideIgnoresStraySemicolons) {
    QNP_CHECK(merged(L";winmm=n") == L"winmm=n;ddraw=n,b");
    QNP_CHECK(merged(L"winmm=n;") == L"winmm=n;ddraw=n,b");
    QNP_CHECK(merged(L";;winmm=n;;dinput8=b;;")
              == L"winmm=n;dinput8=b;ddraw=n,b");
    QNP_CHECK(merged(L";ddraw=b;") == L"ddraw=n,b");
}

QNP_TEST(PlatformDdrawOverrideRejectsMalformedInputWithoutTouchingOutput) {
    using namespace qplat;
    const wchar_t* malformed[] = {
        L"winmm",
        L"winmm=n;shell32",
        L"=n,b",
        L"a,,b=n",
        L"ddraw,=b",
    };
    for (const wchar_t* input : malformed) {
        std::wstring out = L"<untouched>";
        QNP_CHECK(!mergeDdrawOverrideValue(input, out));
        QNP_CHECK(out == L"<untouched>");
    }
}

QNP_TEST(PlatformDdrawOverrideEditsOnlyTheOverridesEntry) {
    using namespace qplat;
    std::vector<std::wstring> entries = {
        L"=Z:=Z:\\game",
        L"PATH=Z:\\usr\\bin",
        L"WINEDLLOVERRIDES=winmm=n;ddraw=b",
        L"STEAM_COMPAT_DATA_PATH=Z:\\home\\user\\compatdata\\1",
    };
    QNP_REQUIRE(mergeDdrawOverride(entries));
    QNP_REQUIRE(entries.size() == 4);
    QNP_CHECK(entries[0] == L"=Z:=Z:\\game");
    QNP_CHECK(entries[1] == L"PATH=Z:\\usr\\bin");
    QNP_CHECK(entries[2] == L"WINEDLLOVERRIDES=winmm=n;ddraw=n,b");
    QNP_CHECK(entries[3]
              == L"STEAM_COMPAT_DATA_PATH=Z:\\home\\user\\compatdata\\1");

    std::vector<std::wstring> without = {L"=C:=C:\\", L"PATH=C:\\windows"};
    QNP_REQUIRE(mergeDdrawOverride(without));
    QNP_REQUIRE(without.size() == 3);
    QNP_CHECK(without[0] == L"=C:=C:\\");
    QNP_CHECK(without[1] == L"PATH=C:\\windows");
    QNP_CHECK(without[2] == L"WINEDLLOVERRIDES=ddraw=n,b");

    std::vector<std::wstring> broken = {L"WINEDLLOVERRIDES=winmm"};
    QNP_CHECK(!mergeDdrawOverride(broken));
    QNP_CHECK(broken.size() == 1 && broken[0] == L"WINEDLLOVERRIDES=winmm");
}

QNP_TEST(PlatformEnvironmentBlockSortsDriveEntriesFirst) {
    using namespace qplat;
    const std::vector<std::wstring> entries = {
        L"PATH=C:\\windows",
        L"=Z:=Z:\\game",
        L"alpha=1",
        L"=C:=C:\\",
    };
    std::vector<wchar_t> block;
    QNP_REQUIRE(buildEnvironmentBlockW(entries, block));
    const std::vector<std::wstring> ordered = blockEntries(block);
    QNP_REQUIRE(ordered.size() == 4);
    QNP_CHECK(ordered[0] == L"=C:=C:\\");
    QNP_CHECK(ordered[1] == L"=Z:=Z:\\game");
    QNP_CHECK(ordered[2] == L"alpha=1");
    QNP_CHECK(ordered[3] == L"PATH=C:\\windows");
    QNP_CHECK(blockIsDoubleNulTerminated(block));
}

QNP_TEST(PlatformEnvironmentBlockOrdersNamesCaseInsensitively) {
    using namespace qplat;
    const std::vector<std::wstring> entries = {
        L"Zulu=1", L"beta=2", L"ALPHA=3", L"Charlie=4",
    };
    std::vector<wchar_t> block;
    QNP_REQUIRE(buildEnvironmentBlockW(entries, block));
    const std::vector<std::wstring> ordered = blockEntries(block);
    QNP_REQUIRE(ordered.size() == 4);
    QNP_CHECK(ordered[0] == L"ALPHA=3");
    QNP_CHECK(ordered[1] == L"beta=2");
    QNP_CHECK(ordered[2] == L"Charlie=4");
    QNP_CHECK(ordered[3] == L"Zulu=1");
}

QNP_TEST(PlatformEnvironmentBlockHandlesEmptyAndMalformedInput) {
    using namespace qplat;
    std::vector<wchar_t> block{L'X', L'Y'};
    QNP_REQUIRE(buildEnvironmentBlockW(std::vector<std::wstring>(), block));
    QNP_CHECK(block.size() == 2);
    QNP_CHECK(blockIsDoubleNulTerminated(block));
    QNP_CHECK(blockEntries(block).empty());

    const std::vector<std::wstring> withEmpty = {L"A=1", L"", L"B=2"};
    QNP_CHECK(!buildEnvironmentBlockW(withEmpty, block));
    const std::vector<std::wstring> withNul = {std::wstring(L"A=1\0extra", 9)};
    QNP_CHECK(!buildEnvironmentBlockW(withNul, block));
}

QNP_TEST(PlatformEnvironmentBlockRoundTripsTheRealParentEnvironment) {
    using namespace qplat;
    std::vector<std::wstring> entries;
    QNP_REQUIRE(readParentEnvironmentW(entries));
    QNP_REQUIRE(!entries.empty());
    for (const std::wstring& entry : entries) {
        QNP_CHECK(!entry.empty());
    }
    std::vector<wchar_t> block;
    QNP_REQUIRE(buildEnvironmentBlockW(entries, block));
    QNP_CHECK(blockIsDoubleNulTerminated(block));
    QNP_CHECK(blockEntries(block).size() == entries.size());

    QNP_REQUIRE(mergeDdrawOverride(entries));
    const size_t afterFirst = entries.size();
    QNP_REQUIRE(mergeDdrawOverride(entries));
    QNP_CHECK(entries.size() == afterFirst);
    size_t overrideCount = 0;
    for (const std::wstring& entry : entries) {
        if (entry.compare(0, 17, L"WINEDLLOVERRIDES=") != 0) continue;
        ++overrideCount;
        QNP_REQUIRE(entry.size() >= 9);
        QNP_CHECK(entry.rfind(L"ddraw=n,b") == entry.size() - 9);
    }
    QNP_CHECK(overrideCount == 1);
}

QNP_TEST(PlatformWidenNarrowRoundTripsNonAscii) {
    using namespace qplat;
    QNP_CHECK(widen("plain ascii") == L"plain ascii");
    QNP_CHECK(narrow(L"plain ascii") == "plain ascii");

    const std::string jpUtf8 =
        "\xE3\x82\xAF" "\xE3\x82\xA4" "\xE3\x83\xBC" "\xE3\x83\xB3";
    const std::wstring jpUtf16 = L"\u30AF\u30A4\u30FC\u30F3";
    QNP_CHECK(widen(jpUtf8) == jpUtf16);
    QNP_CHECK(narrow(jpUtf16) == jpUtf8);
    QNP_CHECK(narrow(widen(jpUtf8)) == jpUtf8);

    const std::string accentedUtf8 = "caf" "\xC3\xA9" " na" "\xC3\xAF" "ve";
    const std::wstring accentedUtf16 = L"caf\u00E9 na\u00EFve";
    QNP_CHECK(widen(accentedUtf8) == accentedUtf16);
    QNP_CHECK(narrow(accentedUtf16) == accentedUtf8);

    const std::string emojiUtf8 = "\xF0\x9F\x8E\xAE";
    QNP_REQUIRE(widen(emojiUtf8).size() == 2);
    QNP_CHECK(narrow(widen(emojiUtf8)) == emojiUtf8);

    QNP_CHECK(widen("").empty());
    QNP_CHECK(narrow(L"").empty());
}

QNP_TEST(PlatformWidenNarrowRejectEmbeddedNulAndInvalidSequences) {
    using namespace qplat;
    const std::string narrowWithNul("a\0b", 3);
    QNP_REQUIRE(narrowWithNul.size() == 3);
    QNP_CHECK(widen(narrowWithNul).empty());

    const std::wstring wideWithNul(L"a\0b", 3);
    QNP_REQUIRE(wideWithNul.size() == 3);
    QNP_CHECK(narrow(wideWithNul).empty());

    QNP_CHECK(widen("\xFF\xFE").empty());
    QNP_CHECK(widen("\xE3\x82").empty());

    const std::wstring loneSurrogate(1, static_cast<wchar_t>(0xD800));
    QNP_CHECK(narrow(loneSurrogate).empty());
}

QNP_TEST(PlatformLongPathPrefixAtThreshold) {
    using namespace qplat;
    const std::wstring root = L"C:\\";

    const std::wstring justUnder = root + std::wstring(239 - root.size(), L'a');
    QNP_REQUIRE(justUnder.size() == 239);
    QNP_CHECK(longPathW(justUnder) == justUnder);

    const std::wstring atThreshold =
        root + std::wstring(240 - root.size(), L'a');
    QNP_REQUIRE(atThreshold.size() == 240);
    QNP_CHECK(longPathW(atThreshold) == L"\\\\?\\" + atThreshold);

    const std::wstring prefixed = L"\\\\?\\" + atThreshold;
    QNP_CHECK(longPathW(prefixed) == prefixed);

    const std::wstring relative(400, L'r');
    QNP_CHECK(longPathW(relative) == relative);

    const std::wstring unc = L"\\\\server\\share\\" + std::wstring(300, L'u');
    QNP_CHECK(longPathW(unc) == L"\\\\?\\UNC\\" + unc.substr(2));
}

QNP_TEST(PlatformPathDecompositionHasNoTrailingSeparatorSurprises) {
    using namespace qplat;
    QNP_CHECK(dirOfW(L"C:\\games\\qoh99\\GAME.EXE") == L"C:\\games\\qoh99");
    QNP_CHECK(baseNameW(L"C:\\games\\qoh99\\GAME.EXE") == L"GAME.EXE");
    QNP_CHECK(baseNameW(L"C:\\games\\qoh99\\") == L"qoh99");
    QNP_CHECK(dirOfW(L"C:\\GAME.EXE") == L"C:\\");

    const std::wstring exeDir = exeDirW();
    QNP_REQUIRE(!exeDir.empty());
    QNP_CHECK(exeDir.back() == L'\\');
    QNP_CHECK(directoryExistsW(exeDir));

    const std::wstring exePath = exePathW();
    QNP_REQUIRE(!exePath.empty());
    QNP_CHECK(fileExistsW(exePath));
    QNP_CHECK(exePath.compare(0, exeDir.size(), exeDir) == 0);
    QNP_CHECK(absolutePathW(exePath) == exePath);
    QNP_CHECK(!directoryExistsW(exePath));
    QNP_CHECK(!fileExistsW(exeDir));
}

QNP_TEST(PlatformStateRootIsCachedConsistentAndUsable) {
    using namespace qplat;
    const std::wstring root = stateRootW();
    QNP_REQUIRE(!root.empty());
    QNP_CHECK(root.back() == L'\\');
    QNP_CHECK(stateRootW() == root);

    const std::wstring source = stateRootSourceName();
    QNP_CHECK(source == L"env" || source == L"exe"
              || source == L"localappdata");
    QNP_CHECK(stateRootUsedFallback() == (source == L"localappdata"));

    QNP_CHECK(logsRootW() == root + L"logs");
    QNP_CHECK(replayRootW() == root + L"replays");
    QNP_CHECK(logsRootW().back() != L'\\');
}

QNP_TEST(PlatformWineProtocolInfoRepairsOnlyUnsetFields) {
    using namespace qplat;

    WSAPROTOCOL_INFOW zeroed{};
    QNP_CHECK(protocolInfoNeedsUdpRepair(zeroed));
    QNP_CHECK(repairDuplicatedUdpProtocolInfo(zeroed));
    QNP_CHECK(zeroed.iAddressFamily == AF_INET);
    QNP_CHECK(zeroed.iSocketType == SOCK_DGRAM);
    QNP_CHECK(zeroed.iProtocol == IPPROTO_UDP);
    QNP_CHECK(!protocolInfoNeedsUdpRepair(zeroed));
    QNP_CHECK(describeProtocolInfo(zeroed)
              == "af=" + std::to_string(static_cast<int>(AF_INET))
               + " type=" + std::to_string(static_cast<int>(SOCK_DGRAM))
               + " proto=" + std::to_string(static_cast<int>(IPPROTO_UDP)));

    WSAPROTOCOL_INFOW correct{};
    correct.iAddressFamily = AF_INET;
    correct.iSocketType = SOCK_DGRAM;
    correct.iProtocol = IPPROTO_UDP;
    correct.dwProviderReserved = 0x1234;
    QNP_CHECK(!protocolInfoNeedsUdpRepair(correct));
    QNP_CHECK(!repairDuplicatedUdpProtocolInfo(correct));
    QNP_CHECK(correct.iAddressFamily == AF_INET);
    QNP_CHECK(correct.iSocketType == SOCK_DGRAM);
    QNP_CHECK(correct.iProtocol == IPPROTO_UDP);
    QNP_CHECK(correct.dwProviderReserved == 0x1234);

    WSAPROTOCOL_INFOW partial{};
    partial.iAddressFamily = AF_INET;
    partial.iSocketType = 0;
    partial.iProtocol = IPPROTO_UDP;
    QNP_CHECK(protocolInfoNeedsUdpRepair(partial));
    QNP_CHECK(repairDuplicatedUdpProtocolInfo(partial));
    QNP_CHECK(partial.iSocketType == SOCK_DGRAM);
    QNP_CHECK(partial.iAddressFamily == AF_INET);
    QNP_CHECK(partial.iProtocol == IPPROTO_UDP);

    WSAPROTOCOL_INFOW reported{};
    reported.iAddressFamily = AF_INET6;
    QNP_CHECK(protocolInfoNeedsUdpRepair(reported));
    QNP_CHECK(repairDuplicatedUdpProtocolInfo(reported));
    QNP_CHECK(reported.iAddressFamily == AF_INET6);
    QNP_CHECK(reported.iSocketType == SOCK_DGRAM);
    QNP_CHECK(reported.iProtocol == IPPROTO_UDP);
}

QNP_TEST(PlatformPeAcceptsMinimalWellFormedPe32) {
    using namespace qplat;
    const std::vector<uint8_t> image = minimalPe32();
    PeImageInfo info{};
    std::string error;
    QNP_REQUIRE(inspectFileImage(image.data(), image.size(), info, error));
    QNP_CHECK(error.empty());
    QNP_CHECK(info.base == 0);
    QNP_CHECK(info.sizeOfImage == 0x8000u);
    QNP_CHECK(info.machine == IMAGE_FILE_MACHINE_I386);
    QNP_CHECK(info.timestamp == 0x44556677u);
    QNP_CHECK(info.entryPointRva == 0x1234u);
    QNP_CHECK(!info.isDll);
    QNP_CHECK(info.subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI);
    QNP_CHECK(info.checkSum == 0xA1B2C3D4u);

    std::vector<uint8_t> asDll = image;
    poke<WORD>(asDll, characteristicsOffset(),
               static_cast<WORD>(IMAGE_FILE_EXECUTABLE_IMAGE
                                 | IMAGE_FILE_DLL));
    PeImageInfo dllInfo{};
    QNP_REQUIRE(inspectFileImage(asDll.data(), asDll.size(), dllInfo, error));
    QNP_CHECK(dllInfo.isDll);
}

QNP_TEST(PlatformPeRejectsTruncatedAndMalformedHeaders) {
    using namespace qplat;
    const std::vector<uint8_t> good = minimalPe32();

    PeImageInfo info{};
    std::string error;
    QNP_CHECK(!inspectFileImage(nullptr, 64, info, error));
    QNP_CHECK(!inspectFileImage(good.data(), 0, info, error));

    QNP_CHECK(rejectsFileImage(
        std::vector<uint8_t>(good.begin(), good.begin() + 16)));
    QNP_CHECK(rejectsFileImage(
        std::vector<uint8_t>(good.begin(), good.begin() + kNtOffset)));

    std::vector<uint8_t> badMz = good;
    poke<WORD>(badMz, 0, static_cast<WORD>(0x4142));
    QNP_CHECK(rejectsFileImage(badMz));

    const LONG badLfanew[] = {-8L, -1L, 0x40000000L, 0x7FFFFFFFL, 0x41L, 0x10L};
    for (const LONG value : badLfanew) {
        std::vector<uint8_t> bad = good;
        poke<LONG>(bad, offsetof(IMAGE_DOS_HEADER, e_lfanew), value);
        QNP_CHECK(rejectsFileImage(bad));
    }

    std::vector<uint8_t> badNt = good;
    poke<DWORD>(badNt, kNtOffset, 0x12345678u);
    QNP_CHECK(rejectsFileImage(badNt));

    const WORD badMachines[] = {
        static_cast<WORD>(IMAGE_FILE_MACHINE_AMD64),
        static_cast<WORD>(IMAGE_FILE_MACHINE_IA64),
        static_cast<WORD>(IMAGE_FILE_MACHINE_UNKNOWN),
    };
    for (const WORD machine : badMachines) {
        std::vector<uint8_t> bad = good;
        poke<WORD>(bad, machineOffset(), machine);
        QNP_CHECK(rejectsFileImage(bad));
    }

    const WORD badOptionalSizes[] = {
        0, 0x20, static_cast<WORD>(sizeof(IMAGE_OPTIONAL_HEADER32) - 1),
    };
    for (const WORD size : badOptionalSizes) {
        std::vector<uint8_t> bad = good;
        poke<WORD>(bad, sizeOfOptionalHeaderOffset(), size);
        QNP_CHECK(rejectsFileImage(bad));
    }

    std::vector<uint8_t> hybrid = good;
    poke<WORD>(hybrid, optionalHeaderOffset(),
               static_cast<WORD>(IMAGE_NT_OPTIONAL_HDR64_MAGIC));
    QNP_CHECK(rejectsFileImage(hybrid));

    const DWORD badImageSizes[] = {0u, 0x80000000u, 0xFFFFFFFFu};
    for (const DWORD sizeOfImage : badImageSizes) {
        std::vector<uint8_t> bad = good;
        poke<DWORD>(bad,
                    optionalHeaderOffset()
                        + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfImage),
                    sizeOfImage);
        QNP_CHECK(rejectsFileImage(bad));
    }

    QNP_CHECK(rejectsFileImage(
        std::vector<uint8_t>(good.begin(), good.begin() + kNtOffset + 32)));
}

QNP_TEST(PlatformPeInspectsAndValidatesTheRunningImage) {
    using namespace qplat;
    void* self = GetModuleHandleW(nullptr);
    QNP_REQUIRE(self != nullptr);

    PeImageInfo info{};
    std::string error;
    QNP_REQUIRE(inspectLoadedImage(self, info, error));
    QNP_CHECK(error.empty());
    QNP_CHECK(info.base == reinterpret_cast<uintptr_t>(self));
    QNP_CHECK(info.machine == IMAGE_FILE_MACHINE_I386);
    QNP_CHECK(info.sizeOfImage != 0);
    QNP_CHECK(!info.isDll);

    PeImageInfo hostInfo{};
    QNP_CHECK(validateHostImage(self, hostInfo, error));
    QNP_CHECK(hostInfo.sizeOfImage == info.sizeOfImage);

    PeImageInfo rejected{};
    void* inside = static_cast<uint8_t*>(self) + 0x1000;
    QNP_CHECK(!validateHostImage(inside, rejected, error));
    QNP_CHECK(!error.empty());
    QNP_CHECK(rejected.sizeOfImage == 0);

    QNP_CHECK(!inspectLoadedImage(nullptr, rejected, error));
    QNP_CHECK(!inspectLoadedImage(reinterpret_cast<void*>(0x10), rejected,
                                  error));
    QNP_CHECK(!isReadableRange(nullptr, 4));
    QNP_CHECK(!isReadableRange(reinterpret_cast<void*>(0x10), 4));
    QNP_CHECK(isReadableRange(self, 64));
}

QNP_TEST(PlatformPeFindsAndRestoresARealImportSlot) {
    using namespace qplat;
    void* self = GetModuleHandleW(nullptr);
    QNP_REQUIRE(self != nullptr);

    IatSlot slot;
    std::string error;
    if (!findImportSlot(self, "kernel32.dll", "GetTickCount", slot, error)) {
        QNP_REQUIRE(findImportSlot(self, "kernel32", "GetProcAddress", slot,
                                   error));
    }
    QNP_REQUIRE(slot.slot != nullptr);
    QNP_REQUIRE(slot.original != nullptr);
    QNP_CHECK(*slot.slot == slot.original);

    PeImageInfo info{};
    QNP_REQUIRE(inspectLoadedImage(self, info, error));
    const uintptr_t slotAddress = reinterpret_cast<uintptr_t>(slot.slot);
    QNP_CHECK(slotAddress >= info.base);
    QNP_CHECK(slotAddress < info.base + info.sizeOfImage);

    static int sentinel = 0;
    void* const replacement = &sentinel;
    QNP_REQUIRE(patchImportSlot(slot, replacement, error));
    QNP_CHECK(*slot.slot == replacement);
    QNP_REQUIRE(restoreImportSlot(slot, error));
    QNP_CHECK(*slot.slot == slot.original);

    IatSlot missing;
    QNP_CHECK(!findImportSlot(self, "kernel32.dll", "ThisExportDoesNotExist",
                              missing, error));
    QNP_CHECK(!error.empty());
    QNP_CHECK(missing.slot == nullptr);
    QNP_CHECK(!findImportSlot(self, "no_such_module.dll", "GetProcAddress",
                              missing, error));
    QNP_CHECK(!findImportSlot(self, "", "GetProcAddress", missing, error));
    QNP_CHECK(!findImportSlot(nullptr, "kernel32.dll", "GetProcAddress",
                              missing, error));

    IatSlot empty;
    QNP_CHECK(!patchImportSlot(empty, replacement, error));
    QNP_CHECK(!restoreImportSlot(empty, error));
}

QNP_TEST(PlatformPeResolvesModulesInTheCurrentProcessRemotely) {
    using namespace qplat;
    HANDLE self = GetCurrentProcess();
    uintptr_t base = 0;
    std::string error;
    QNP_REQUIRE(remoteModuleBase(self, L"kernel32.dll", base, error));
    QNP_CHECK(error.empty());
    QNP_CHECK(base
              == reinterpret_cast<uintptr_t>(GetModuleHandleW(L"kernel32.dll")));

    PeImageInfo info{};
    QNP_REQUIRE(remoteValidateModule(self, base, info, error));
    QNP_CHECK(info.base == base);
    QNP_CHECK(info.machine == IMAGE_FILE_MACHINE_I386);
    QNP_CHECK(info.isDll);
    QNP_CHECK(info.sizeOfImage != 0);

    uintptr_t upper = 0;
    QNP_CHECK(remoteModuleBase(self, L"KERNEL32.DLL", upper, error));
    QNP_CHECK(upper == base);

    uintptr_t unused = 0;
    QNP_CHECK(!remoteModuleBase(nullptr, L"kernel32.dll", unused, error));
    QNP_CHECK(!remoteModuleBase(self, L"", unused, error));
    QNP_CHECK(unused == 0);
    PeImageInfo rejected{};
    QNP_CHECK(!remoteValidateModule(self, 0, rejected, error));
    QNP_CHECK(!remoteValidateModule(self, base + 0x1000, rejected, error));
    QNP_CHECK(rejected.sizeOfImage == 0);
}

QNP_TEST(PlatformCompatDetectionAndPreambleAreSelfConsistent) {
    using namespace qplat;
    const CompatInfo& compat = detectCompat();
    QNP_CHECK(&compat == &detectCompat());
    if (!compat.wine) {
        QNP_CHECK(compat.wineVersion.empty());
        QNP_CHECK(compat.wineBuildId.empty());
        QNP_CHECK(compat.wineHostOs.empty());
    }

    PreambleContext ctx;
    ctx.component = "caster";
    ctx.buildContract = "qoh99-netplay-test";
    ctx.stateRoot = stateRootW();
    ctx.stateRootFallback = stateRootUsedFallback();
    ctx.modules.push_back(
        PreambleModule{L"ddraw.dll", L"C:\\game\\ddraw.dll", "ABCDEF"});
    ctx.extra = "ddraw-override=\"ddraw=n,b\"";

    std::vector<std::string> records;
    writeSessionPreamble(ctx, [&](const std::string& line) {
        records.push_back(line);
    });
    QNP_REQUIRE(records.size() >= 6);

    std::string joined;
    for (const std::string& record : records) {
        QNP_CHECK(record.rfind("[compat] ", 0) == 0);
        QNP_CHECK(record.find('\n') == std::string::npos);
        QNP_CHECK(record.find('\r') == std::string::npos);
        joined += record;
        joined += '\n';
    }
    QNP_CHECK(joined.find("component=caster") != std::string::npos);
    QNP_CHECK(joined.find("qoh99-netplay-test") != std::string::npos);
    QNP_CHECK(joined.find("wine=") != std::string::npos);
    QNP_CHECK(joined.find("proton=") != std::string::npos);
    QNP_CHECK(joined.find("qpc-frequency=") != std::string::npos);
    QNP_CHECK(joined.find("fallback=") != std::string::npos);
    QNP_CHECK(joined.find("C:\\\\game\\\\ddraw.dll") != std::string::npos);
    QNP_CHECK(joined.find("ddraw-override=") != std::string::npos);

    writeSessionPreamble(ctx, nullptr);
}

QNP_TEST(PlatformOptionalModulesLoadDynamically) {
    using namespace qplat;
    void* module = nullptr;
    QNP_CHECK(loadOptionalModule(L"kernel32.dll", module));
    QNP_CHECK(module == GetModuleHandleW(L"kernel32.dll"));

    void* missing = reinterpret_cast<void*>(0x1);
    QNP_CHECK(!loadOptionalModule(L"qoh99_no_such_provider.dll", missing));
    QNP_CHECK(missing == nullptr);
    missing = reinterpret_cast<void*>(0x1);
    QNP_CHECK(!loadOptionalModule(nullptr, missing));
    QNP_CHECK(missing == nullptr);
    missing = reinterpret_cast<void*>(0x1);
    QNP_CHECK(!loadOptionalModule(L"", missing));
    QNP_CHECK(missing == nullptr);
}
