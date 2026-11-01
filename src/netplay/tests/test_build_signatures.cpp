#include "TestHarness.h"
#include "RuntimeExecutableFixtures.h"

#include "qoh99/generated/BuildProfiles.hpp"
#include "qoh99/generated/FunctionCatalog.hpp"
#include "replay/ReplaySimulationContractV2.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using qoh99::generated_model::FunctionId;

struct PeFile {
    std::vector<unsigned char> bytes;
    std::uint32_t imageBase = 0;
    struct Section { std::uint32_t va, vsize, raw, rsize; };
    std::vector<Section> sections;

    bool load(const std::string& path, std::string& error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) { error = "cannot open " + path; return false; }
        bytes.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
        if (bytes.size() < 0x200) { error = "too small: " + path; return false; }
        const auto u16 = [this](std::size_t o) {
            return static_cast<std::uint16_t>(bytes[o] | (bytes[o + 1] << 8));
        };
        const auto u32 = [this](std::size_t o) {
            return static_cast<std::uint32_t>(
                bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16)
                | (static_cast<std::uint32_t>(bytes[o + 3]) << 24));
        };
        const std::size_t pe = u32(0x3C);
        if (pe + 0x100 > bytes.size()) { error = "bad e_lfanew"; return false; }
        const std::size_t sectionCount = u16(pe + 6);
        const std::size_t optionalSize = u16(pe + 20);
        imageBase = u32(pe + 24 + 28);
        const std::size_t table = pe + 24 + optionalSize;
        if (table + sectionCount * 40 > bytes.size()) {
            error = "truncated section table";
            return false;
        }
        for (std::size_t i = 0; i < sectionCount; ++i) {
            const std::size_t o = table + i * 40;
            sections.push_back({ u32(o + 12), u32(o + 8), u32(o + 20),
                                 u32(o + 16) });
        }
        return true;
    }

    const unsigned char* at(std::uint32_t va, std::size_t length) const {
        if (va < imageBase) return nullptr;
        const std::uint32_t rva = va - imageBase;
        for (const Section& s : sections) {
            const std::uint32_t span = s.vsize > s.rsize ? s.vsize : s.rsize;
            if (rva < s.va || rva >= s.va + span) continue;
            const std::size_t off = s.raw + (rva - s.va);
            if (off + length > bytes.size()) return nullptr;
            return bytes.data() + off;
        }
        return nullptr;
    }
};

std::uint32_t decemberVa(FunctionId id) {
    for (const auto& f : qoh99::generated_model::kFunctions)
        if (f.id == id) return static_cast<std::uint32_t>(f.game2000Va);
    return 0;
}
std::uint32_t novemberVa(FunctionId id) {
    for (const auto& f : qoh99::generated_model::kFunctions)
        if (f.id == id) return static_cast<std::uint32_t>(f.se2006Va);
    return 0;
}

struct Sig {
    const char* label;
    FunctionId id;
    std::uint32_t literalVa;
    std::vector<unsigned char> november;
    std::vector<unsigned char> december;
};

constexpr FunctionId kLiteral = static_cast<FunctionId>(0xFFFF);

std::vector<Sig> signatures() {
    return {
        { "match dispatcher (kMatchSig)", FunctionId::SceneMatchDispatch, 0,
          { 0x83,0xEC,0x54,0x53,0x56,0x57,0xBF,0x01 }, {} },
        { "title dispatcher (kMenuSig)", FunctionId::SceneTitleMenu, 0,
          { 0x53,0x55,0xBD,0x01,0x00,0x00,0x00,0x33 }, {} },
        { "VS dispatcher (kCharSelectSig)", FunctionId::SceneVsSetup, 0,
          { 0x53,0x56,0xBB,0x01,0x00,0x00,0x00,0x57 }, {} },
        { "config save (kConfigSaveSig)", FunctionId::ConfigSave, 0,
          { 0x81,0xEC,0x04,0x01,0x00,0x00,0x8B,0xD1,0x85,0xD2 }, {} },
        { "full input poll (kFullInputPollSig/kFullPollSig)",
          FunctionId::PollAllInputs, 0,
          { 0x56,0xE8,0x0A,0x87,0xFD,0xFF,0xA1,0x58,0xB7,0x4A,0x00 },
          { 0x56,0xE8,0xEA,0x86,0xFD,0xFF,0xA1,0x58,0xB7,0x4A,0x00 } },
        { "presence probe (kPresenceSig)", FunctionId::NativePresenceProbe, 0,
          { 0x33,0xC9,0xE8,0x79,0x84,0xFD,0xFF,0x48,
            0xB9,0x01,0x00,0x00,0x00,0xF7,0xD8,0x1B },
          { 0x33,0xC9,0xE8,0x59,0x84,0xFD,0xFF,0x48,
            0xB9,0x01,0x00,0x00,0x00,0xF7,0xD8,0x1B } },
        { "FCommand parser (kFCommandParseSig)", kLiteral, 0x46ED90u,
          { 0xB8,0x98,0x20,0x00,0x00,0xE8,0xE6,0x55,0x02,0x00 },
          { 0xB8,0x98,0x20,0x00,0x00,0xE8,0x06,0x56,0x02,0x00 } },
        { "pre-transition commit barrier (kTransitionCommitSig)", kLiteral,
          0x425B20u, { 0x83,0x3D,0x50,0x19,0x87,0x00,0x05,0x75,0x1F }, {} },
        { "managed input init (kInputInitSig)", kLiteral, 0x45CD20u,
          { 0x83,0xEC,0x14,0x56,0x33,0xF6,0x57,0x56,
            0x68,0xDC,0xA5,0x4A,0x00,0x8B,0xF9,0x68 }, {} },
        { "joystick low state (kJoyLowSig)", kLiteral, 0x45D100u,
          { 0xA1,0x14,0xA9,0x4A,0x00,0x56,0x8B,0xF1,
            0x57,0x3B,0xF0,0x8B,0xFA,0x72,0x05,0x5F }, {} },
    };
}

bool matches(const PeFile& image, std::uint32_t va,
             const std::vector<unsigned char>& want) {
    const unsigned char* got = image.at(va, want.size());
    return got != nullptr && std::memcmp(got, want.data(), want.size()) == 0;
}

void report(const char* build, const char* label, const PeFile& image,
            std::uint32_t va, const std::vector<unsigned char>& want) {
    std::fprintf(stderr, "  %s '%s' @%08X\n    want ", build, label, va);
    for (unsigned char b : want) std::fprintf(stderr, "%02X ", b);
    const unsigned char* got = image.at(va, want.size());
    std::fprintf(stderr, "\n    got  ");
    if (got == nullptr) std::fprintf(stderr, "<not mapped>");
    else for (std::size_t i = 0; i < want.size(); ++i)
        std::fprintf(stderr, "%02X ", got[i]);
    std::fprintf(stderr, "\n");
}

bool fixtures(qnetplay::test::RuntimeExecutableFixtures& out) {
    std::string error;
    if (qnetplay::test::resolveRuntimeExecutableFixtures(out, error))
        return true;
    std::fprintf(stderr, "FAIL: %s\n", error.c_str());
    return false;
}

}

QNP_TEST(EveryMandatorySignatureMatchesBothQualifiedBuilds) {
    qnetplay::test::RuntimeExecutableFixtures paths{};
    QNP_REQUIRE(fixtures(paths));
    PeFile november, december;
    std::string error;
    QNP_REQUIRE(november.load(paths.novemberExecutable.generic_u8string(),
                              error));
    QNP_REQUIRE(december.load(paths.decemberExecutable.generic_u8string(),
                              error));

    for (const Sig& sig : signatures()) {
        const bool literal = sig.id == kLiteral;
        const std::uint32_t novVa =
            literal ? sig.literalVa : novemberVa(sig.id);
        const std::uint32_t decVa =
            literal ? sig.literalVa : decemberVa(sig.id);
        QNP_CHECK(novVa != 0 && decVa != 0);

        const std::vector<unsigned char>& wantDec =
            sig.december.empty() ? sig.november : sig.december;

        const bool novOk = matches(november, novVa, sig.november);
        const bool decOk = matches(december, decVa, wantDec);
        if (!novOk) report("november", sig.label, november, novVa,
                           sig.november);
        if (!decOk) report("december", sig.label, december, decVa, wantDec);
        QNP_CHECK(novOk);
        QNP_CHECK(decOk);
    }
}

QNP_TEST(EveryPerBuildSignatureSplitIsActuallyNeeded) {
    for (const Sig& sig : signatures()) {
        if (sig.december.empty()) continue;
        QNP_CHECK(sig.november.size() == sig.december.size());
        QNP_CHECK(sig.november != sig.december);
    }
}

QNP_TEST(PerBuildSignaturesDifferOnlyInACallDisplacement) {
    for (const Sig& sig : signatures()) {
        if (sig.december.empty()) continue;
        std::size_t firstDiff = sig.november.size();
        std::size_t lastDiff = 0;
        for (std::size_t i = 0; i < sig.november.size(); ++i) {
            if (sig.november[i] == sig.december[i]) continue;
            if (firstDiff == sig.november.size()) firstDiff = i;
            lastDiff = i;
        }
        QNP_REQUIRE(firstDiff < sig.november.size());
        QNP_CHECK(lastDiff - firstDiff <= 3u);
        bool foundOpcode = false;
        for (std::size_t start = 0; start + 4 < sig.november.size(); ++start) {
            if (sig.november[start] != 0xE8) continue;
            if (firstDiff < start + 1 || lastDiff > start + 4) continue;
            const auto rel = [&](const std::vector<unsigned char>& v) {
                return static_cast<std::int32_t>(
                    v[start + 1] | (v[start + 2] << 8) | (v[start + 3] << 16)
                    | (static_cast<std::uint32_t>(v[start + 4]) << 24));
            };
            const std::int32_t delta = rel(sig.november) - rel(sig.december);
            QNP_CHECK(delta == 0x20 || delta == -0x20);
            foundOpcode = true;
            break;
        }
        QNP_CHECK(foundOpcode);
    }
}

QNP_TEST(TheOriginal1126IsTheSupportedBuildPlusTwentySixNops) {
    qnetplay::test::RuntimeExecutableFixtures paths{};
    std::string ignored;
    if (!qnetplay::test::resolveRuntimeExecutableFixtures(paths, ignored))
        return;
    const std::filesystem::path original =
        paths.root / "november" / "Qoh99_1126OG.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(original, ec)) {
        std::fprintf(stderr,
                     "  (skipped: %s absent - the original 1126 variant is "
                     "optional evidence)\n",
                     original.generic_u8string().c_str());
        return;
    }

    const auto readAll = [](const std::filesystem::path& p) {
        std::ifstream in(p.generic_u8string(), std::ios::binary);
        return std::vector<unsigned char>(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>());
    };
    const std::vector<unsigned char> og = readAll(original);
    const std::vector<unsigned char> supported =
        readAll(paths.novemberExecutable);
    QNP_REQUIRE(!og.empty() && !supported.empty());
    QNP_CHECK(og.size() == supported.size());
    if (og.size() != supported.size()) return;

    std::size_t first = og.size();
    std::size_t last = 0;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < og.size(); ++i) {
        if (og[i] == supported[i]) continue;
        if (first == og.size()) first = i;
        last = i;
        ++differing;
    }
    QNP_CHECK(differing == 26u);
    QNP_CHECK(first == 0x71784u);
    QNP_CHECK(last == 0x7179Du);
    QNP_CHECK(last - first + 1u == differing);

    static const unsigned char kGate[26] = {
        0x81,0xBC,0x24,0x1C,0x01,0x00,0x00,0x00,0x1B,0x3E,0xDA,0x75,0x1B,
        0x81,0xBC,0x24,0x20,0x01,0x00,0x00,0xC3,0xA6,0xBF,0x01,0x75,0x0E
    };
    for (std::size_t i = 0; i < 26u; ++i) {
        QNP_CHECK(supported[0x71784u + i] == 0x90u);
        QNP_CHECK(og[0x71784u + i] == kGate[i]);
    }
}

QNP_TEST(ReplayContractDigestsMatchQualifiedProfiles) {
    const auto hexOf = [](const std::array<std::uint8_t, 32>& digest) {
        static const char kHex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(64);
        for (std::uint8_t byte : digest) {
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
        return out;
    };

    std::vector<std::string> qualifiedProfiles;
    for (const auto& build : qoh99::generated_model::kBuilds) {
        if (build.runtimeQualification
            != qoh99::generated_model::RuntimeQualification::Qualified) {
            continue;
        }
        qualifiedProfiles.emplace_back(build.binarySha256);
        bool found = false;
        for (std::size_t i = 0;
             i < qreplay::qualifiedExecutableCountV2(); ++i) {
            if (hexOf(qreplay::qualifiedExecutableSha256V2(i))
                == build.binarySha256) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr,
                         "  qualified build '%s' (%s) has NO digest in "
                         "ReplaySimulationContractV2 - replays would be "
                         "unusable on it\n",
                         build.runtimeProfileKey, build.binarySha256);
        }
        QNP_CHECK(found);
    }
    QNP_CHECK(qualifiedProfiles.size()
              == qreplay::qualifiedExecutableCountV2());

    for (std::size_t i = 0; i < qreplay::qualifiedExecutableCountV2(); ++i) {
        const std::string digest =
            hexOf(qreplay::qualifiedExecutableSha256V2(i));
        bool qualified = false;
        for (const std::string& known : qualifiedProfiles) {
            if (known == digest) { qualified = true; break; }
        }
        if (!qualified) {
            std::fprintf(stderr,
                         "  ReplaySimulationContractV2 digest %s matches no "
                         "QUALIFIED build profile\n", digest.c_str());
        }
        QNP_CHECK(qualified);
    }
}
