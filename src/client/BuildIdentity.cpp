#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include "BuildIdentity.h"
#include "../ipc/BuildContract.h"
#include "../netplay/WireProtocol.h"
#include "../replay/ReplayIntegrity.h"
#include "qoh99/generated/BuildProfiles.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace qclient {
namespace {

bool bcryptOk(NTSTATUS status) {
    return status >= 0;
}

std::string winError(const char* what, DWORD code) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s (GetLastError=%lu)", what, code);
    return buf;
}

std::wstring widenUtf8Path(const std::string& utf8) {
    if (utf8.empty() || utf8.size() > static_cast<std::size_t>(INT_MAX))
        return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           utf8.c_str(),
                                           static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                            static_cast<int>(utf8.size()), &wide[0], needed)
        != needed)
        return std::wstring();
    return wide;
}

HANDLE openForReadW(const std::string& utf8Path, const char* what,
                    std::string& error) {
    const std::wstring wide = widenUtf8Path(utf8Path);
    if (wide.empty()) {
        error = std::string(what) + ": path is not valid UTF-8";
        return INVALID_HANDLE_VALUE;
    }
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        error = winError(what, GetLastError());
    return file;
}

bool readWholeFile(const std::string& path, const char* what,
                   std::uint64_t maxBytes, std::vector<unsigned char>& bytes,
                   std::string& error) {
    bytes.clear();
    HANDLE file = openForReadW(path, what, error);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || static_cast<std::uint64_t>(size.QuadPart) > maxBytes) {
        error = std::string(what) + ": invalid file size";
        CloseHandle(file);
        return false;
    }

    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    bool readOk = true;
    while (total < bytes.size()) {
        DWORD got = 0;
        const DWORD remaining = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - total, 1024u * 1024u));
        if (!ReadFile(file, bytes.data() + total, remaining, &got, nullptr)
            || got == 0) {
            readOk = false;
            break;
        }
        total += got;
    }
    CloseHandle(file);
    if (!readOk || total != bytes.size()) {
        error = std::string(what) + ": could not read the complete file";
        bytes.clear();
        return false;
    }
    return true;
}

bool readExactAt(HANDLE file, std::uint64_t offset, void* dst, DWORD size,
                 std::string& error) {
    LARGE_INTEGER where{};
    where.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, where, nullptr, FILE_BEGIN)) {
        error = winError("SetFilePointerEx", GetLastError());
        return false;
    }
    DWORD total = 0;
    auto* bytes = static_cast<unsigned char*>(dst);
    while (total < size) {
        DWORD got = 0;
        if (!ReadFile(file, bytes + total, size - total, &got, nullptr)) {
            error = winError("ReadFile(PE header)", GetLastError());
            return false;
        }
        if (got == 0) {
            error = "Unexpected end of file while reading PE header";
            return false;
        }
        total += got;
    }
    return true;
}

bool readPeIdentity(HANDLE file, std::uint64_t fileSize,
                    ExecutableIdentity& out, std::string& error) {
    IMAGE_DOS_HEADER dos{};
    if (!readExactAt(file, 0, &dos, sizeof(dos), error)) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        error = "Not a PE executable (invalid DOS header)";
        return false;
    }
    const std::uint64_t peOffset = static_cast<std::uint32_t>(dos.e_lfanew);
    if (peOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > fileSize) {
        error = "Not a PE executable (file header outside file)";
        return false;
    }
    DWORD signature = 0;
    if (!readExactAt(file, peOffset, &signature, sizeof(signature), error))
        return false;
    if (signature != IMAGE_NT_SIGNATURE) {
        error = "Not a PE executable (invalid NT signature)";
        return false;
    }
    IMAGE_FILE_HEADER coff{};
    if (!readExactAt(file, peOffset + sizeof(signature), &coff, sizeof(coff),
                     error))
        return false;
    out.machine = coff.Machine;
    out.coffTimestamp = coff.TimeDateStamp;
    return true;
}

bool sha256File(HANDLE file, std::string& hex, std::string& error) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD digestLength = 0;
    DWORD cb = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    bool ok = false;

    if (!bcryptOk(BCryptOpenAlgorithmProvider(
            &alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        error = "BCryptOpenAlgorithmProvider(SHA-256) failed";
        goto done;
    }
    if (!bcryptOk(BCryptGetProperty(
            alg, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
            &cb, 0)) ||
        !bcryptOk(BCryptGetProperty(
            alg, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
            &cb, 0)) ||
        digestLength != 32) {
        error = "BCryptGetProperty(SHA-256) failed";
        goto done;
    }
    object.resize(objectLength);
    digest.resize(digestLength);
    if (!bcryptOk(BCryptCreateHash(alg, &hash, object.data(), objectLength,
                                   nullptr, 0, 0))) {
        error = "BCryptCreateHash(SHA-256) failed";
        goto done;
    }

    {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
            error = winError("SetFilePointerEx(hash)", GetLastError());
            goto done;
        }
        std::array<unsigned char, 64 * 1024> buffer{};
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                          &got, nullptr)) {
                error = winError("ReadFile(hash)", GetLastError());
                goto done;
            }
            if (got == 0) break;
            if (!bcryptOk(BCryptHashData(hash, buffer.data(), got, 0))) {
                error = "BCryptHashData(SHA-256) failed";
                goto done;
            }
        }
    }
    if (!bcryptOk(BCryptFinishHash(hash, digest.data(), digestLength, 0))) {
        error = "BCryptFinishHash(SHA-256) failed";
        goto done;
    }
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        hex.resize(digest.size() * 2);
        for (std::size_t i = 0; i < digest.size(); ++i) {
            hex[2 * i] = kHex[digest[i] >> 4];
            hex[2 * i + 1] = kHex[digest[i] & 0x0F];
        }
    }
    ok = true;

done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool sha256ArtifactFile(const std::string& path, const char* description,
                        uint8_t digest[32], std::string& error) {
    std::memset(digest, 0, 32u);
    HANDLE file = openForReadW(path, description, error);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::string hex;
    const bool hashed = sha256File(file, hex, error);
    CloseHandle(file);
    if (!hashed) return false;
    if (hex.size() != 64u) {
        error = std::string(description) + ": produced an invalid digest";
        return false;
    }
    const auto nibble = [](char ch) -> unsigned char {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned char>(ch - '0');
        if (ch >= 'A' && ch <= 'F') return static_cast<unsigned char>(ch - 'A' + 10);
        return 0xFFu;
    };
    for (size_t i = 0; i < 32u; ++i) {
        const unsigned char hi = nibble(hex[i * 2u]);
        const unsigned char lo = nibble(hex[i * 2u + 1u]);
        if (hi == 0xFFu || lo == 0xFFu) {
            error = std::string(description) + ": produced a malformed digest";
            std::memset(digest, 0, 32u);
            return false;
        }
        digest[i] = static_cast<unsigned char>((hi << 4u) | lo);
    }
    return true;
}

bool validateDllPeImage(const std::vector<unsigned char>& bytes,
                        std::string& error) {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        error = "Netplay artifact is not a PE image (missing DOS header)";
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        error = "Netplay artifact is not a PE image (invalid DOS header)";
        return false;
    }

    const std::uint64_t peOffset =
        static_cast<std::uint32_t>(dos.e_lfanew);
    const std::uint64_t fileHeaderOffset = peOffset + sizeof(DWORD);
    const std::uint64_t optionalHeaderOffset =
        fileHeaderOffset + sizeof(IMAGE_FILE_HEADER);
    if (optionalHeaderOffset > bytes.size()
        || bytes.size() - static_cast<std::size_t>(fileHeaderOffset)
            < sizeof(IMAGE_FILE_HEADER)) {
        error = "Netplay artifact is not a PE image (truncated file header)";
        return false;
    }

    DWORD signature = 0;
    std::memcpy(&signature, bytes.data() + peOffset, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE) {
        error = "Netplay artifact is not a PE image (invalid NT signature)";
        return false;
    }
    IMAGE_FILE_HEADER coff{};
    std::memcpy(&coff, bytes.data() + fileHeaderOffset, sizeof(coff));
    if (coff.Machine != IMAGE_FILE_MACHINE_I386) {
        error = "Netplay artifact is not an x86 DLL";
        return false;
    }
    if ((coff.Characteristics & IMAGE_FILE_DLL) == 0
        || (coff.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0) {
        error = "Netplay artifact is an x86 PE image, but not a DLL";
        return false;
    }
    if (coff.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)
        || optionalHeaderOffset > bytes.size()
        || bytes.size() - static_cast<std::size_t>(optionalHeaderOffset)
            < coff.SizeOfOptionalHeader) {
        error = "Netplay artifact is not a valid x86 DLL "
                "(truncated optional header)";
        return false;
    }
    WORD optionalMagic = 0;
    std::memcpy(&optionalMagic, bytes.data() + optionalHeaderOffset,
                sizeof(optionalMagic));
    if (optionalMagic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        error = "Netplay artifact is not a 32-bit PE DLL";
        return false;
    }
    return true;
}

struct PeFileView {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
    std::uint16_t machine = 0;
    std::uint16_t optionalMagic = 0;
    std::uint16_t characteristics = 0;
    std::uint32_t sizeOfImage = 0;
    std::uint32_t sizeOfHeaders = 0;
    std::uint32_t exportRva = 0;
    std::uint32_t exportSize = 0;
    std::vector<IMAGE_SECTION_HEADER> sections;
};

bool readRaw(const PeFileView& view, std::size_t offset, void* dst,
             std::size_t length) {
    if (offset > view.size || view.size - offset < length) return false;
    std::memcpy(dst, view.data + offset, length);
    return true;
}

bool parsePeFile(const std::vector<unsigned char>& bytes, PeFileView& view,
                 std::string& error) {
    view = PeFileView{};
    view.data = bytes.data();
    view.size = bytes.size();

    IMAGE_DOS_HEADER dos{};
    if (!readRaw(view, 0, &dos, sizeof(dos))
        || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        error = "not a PE image (invalid DOS header)";
        return false;
    }
    const std::size_t peOffset = static_cast<std::uint32_t>(dos.e_lfanew);
    DWORD signature = 0;
    if (!readRaw(view, peOffset, &signature, sizeof(signature))
        || signature != IMAGE_NT_SIGNATURE) {
        error = "not a PE image (invalid NT signature)";
        return false;
    }
    const std::size_t fileHeaderOffset = peOffset + sizeof(DWORD);
    IMAGE_FILE_HEADER coff{};
    if (!readRaw(view, fileHeaderOffset, &coff, sizeof(coff))) {
        error = "not a PE image (truncated file header)";
        return false;
    }
    view.machine = coff.Machine;
    view.characteristics = coff.Characteristics;

    const std::size_t optionalOffset = fileHeaderOffset + sizeof(coff);
    if (coff.SizeOfOptionalHeader < sizeof(WORD)
        || !readRaw(view, optionalOffset, &view.optionalMagic,
                    sizeof(view.optionalMagic))) {
        error = "not a PE image (truncated optional header)";
        return false;
    }

    std::uint32_t directoryCount = 0;
    std::size_t directoryOffset = 0;
    if (view.optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 opt{};
        if (coff.SizeOfOptionalHeader < sizeof(opt)
            || !readRaw(view, optionalOffset, &opt, sizeof(opt))) {
            error = "not a PE image (truncated PE32 optional header)";
            return false;
        }
        view.sizeOfImage = opt.SizeOfImage;
        view.sizeOfHeaders = opt.SizeOfHeaders;
        directoryCount = opt.NumberOfRvaAndSizes;
        directoryOffset = optionalOffset
            + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    } else if (view.optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 opt{};
        if (coff.SizeOfOptionalHeader < sizeof(opt)
            || !readRaw(view, optionalOffset, &opt, sizeof(opt))) {
            error = "not a PE image (truncated PE32+ optional header)";
            return false;
        }
        view.sizeOfImage = opt.SizeOfImage;
        view.sizeOfHeaders = opt.SizeOfHeaders;
        directoryCount = opt.NumberOfRvaAndSizes;
        directoryOffset = optionalOffset
            + offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
    } else {
        error = "not a PE image (unknown optional header magic)";
        return false;
    }

    if (directoryCount > IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
        directoryCount = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    if (directoryCount > IMAGE_DIRECTORY_ENTRY_EXPORT) {
        IMAGE_DATA_DIRECTORY exportDir{};
        if (!readRaw(view,
                     directoryOffset
                         + IMAGE_DIRECTORY_ENTRY_EXPORT * sizeof(exportDir),
                     &exportDir, sizeof(exportDir))) {
            error = "not a PE image (truncated data directory)";
            return false;
        }
        view.exportRva = exportDir.VirtualAddress;
        view.exportSize = exportDir.Size;
    }

    if (coff.NumberOfSections == 0 || coff.NumberOfSections > 96) {
        error = "not a PE image (implausible section count)";
        return false;
    }
    const std::size_t sectionOffset =
        optionalOffset + coff.SizeOfOptionalHeader;
    view.sections.resize(coff.NumberOfSections);
    if (!readRaw(view, sectionOffset, view.sections.data(),
                 view.sections.size() * sizeof(IMAGE_SECTION_HEADER))) {
        error = "not a PE image (truncated section table)";
        view.sections.clear();
        return false;
    }
    return true;
}

bool rvaToOffset(const PeFileView& view, std::uint32_t rva, std::uint32_t need,
                 std::size_t& offset) {
    if (need == 0) return false;
    if (view.sizeOfImage != 0
        && (rva >= view.sizeOfImage || view.sizeOfImage - rva < need))
        return false;
    for (const IMAGE_SECTION_HEADER& section : view.sections) {
        const std::uint32_t start = section.VirtualAddress;
        const std::uint32_t virtualSize = section.Misc.VirtualSize != 0
            ? section.Misc.VirtualSize
            : section.SizeOfRawData;
        if (virtualSize == 0 || rva < start) continue;
        const std::uint32_t delta = rva - start;
        if (delta >= virtualSize) continue;
        if (delta >= section.SizeOfRawData
            || section.SizeOfRawData - delta < need)
            return false;
        const std::uint64_t candidate =
            static_cast<std::uint64_t>(section.PointerToRawData) + delta;
        if (candidate > view.size || view.size - candidate < need)
            return false;
        offset = static_cast<std::size_t>(candidate);
        return true;
    }
    if (view.sizeOfHeaders != 0 && rva < view.sizeOfHeaders
        && view.sizeOfHeaders - rva >= need && rva + need <= view.size) {
        offset = rva;
        return true;
    }
    return false;
}

bool readRvaName(const PeFileView& view, std::uint32_t rva,
                 std::string& name) {
    std::size_t offset = 0;
    if (!rvaToOffset(view, rva, 1, offset)) return false;
    constexpr std::size_t kMaxSymbol = 1024;
    const std::size_t limit =
        std::min<std::size_t>(view.size, offset + kMaxSymbol);
    for (std::size_t i = offset; i < limit; ++i) {
        if (view.data[i] == '\0') {
            name.assign(reinterpret_cast<const char*>(view.data + offset),
                        i - offset);
            return true;
        }
    }
    return false;
}

}

bool peFileExportsSymbols(const std::vector<unsigned char>& bytes,
                          const std::vector<std::string>& required,
                          std::string& error) {
    error.clear();
    if (required.empty()) return true;

    PeFileView view;
    if (!parsePeFile(bytes, view, error)) return false;
    if (view.exportRva == 0 || view.exportSize < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        error = "the image has no export directory";
        return false;
    }

    std::size_t exportOffset = 0;
    if (!rvaToOffset(view, view.exportRva,
                     static_cast<std::uint32_t>(sizeof(IMAGE_EXPORT_DIRECTORY)),
                     exportOffset)) {
        error = "the export directory is outside the image";
        return false;
    }
    IMAGE_EXPORT_DIRECTORY exports{};
    std::memcpy(&exports, view.data + exportOffset, sizeof(exports));

    constexpr std::uint32_t kMaxNames = 65535;
    if (exports.NumberOfNames == 0) {
        error = "the image exports nothing by name";
        return false;
    }
    if (exports.NumberOfNames > kMaxNames) {
        error = "the export directory declares an implausible name count";
        return false;
    }
    const std::uint64_t nameTableBytes =
        static_cast<std::uint64_t>(exports.NumberOfNames) * sizeof(DWORD);
    if (nameTableBytes > 0xFFFFFFFFull) {
        error = "the export name table is out of range";
        return false;
    }
    std::size_t nameTableOffset = 0;
    if (!rvaToOffset(view, exports.AddressOfNames,
                     static_cast<std::uint32_t>(nameTableBytes),
                     nameTableOffset)) {
        error = "the export name table is outside the image";
        return false;
    }

    std::set<std::string> missing(required.begin(), required.end());
    for (std::uint32_t i = 0; i < exports.NumberOfNames && !missing.empty();
         ++i) {
        DWORD nameRva = 0;
        std::memcpy(&nameRva, view.data + nameTableOffset + i * sizeof(DWORD),
                    sizeof(nameRva));
        std::string name;
        if (!readRvaName(view, nameRva, name)) {
            error = "an export name pointer is outside the image";
            return false;
        }
        missing.erase(name);
    }
    if (!missing.empty()) {
        error = "missing export '" + *missing.begin() + "'";
        return false;
    }
    return true;
}

bool validatePackagedDdrawWrapper(const std::string& path,
                                  std::string& error) {
    error.clear();
    std::vector<unsigned char> bytes;
    if (!readWholeFile(path, "packaged ddraw.dll", 64ull * 1024ull * 1024ull,
                       bytes, error))
        return false;

    PeFileView view;
    std::string detail;
    if (!parsePeFile(bytes, view, detail)) {
        error = "packaged ddraw.dll is " + detail;
        return false;
    }
    if (view.machine != IMAGE_FILE_MACHINE_I386
        || view.optionalMagic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        error = "packaged ddraw.dll is not a PE32/i386 image";
        return false;
    }
    if ((view.characteristics & IMAGE_FILE_DLL) == 0) {
        error = "packaged ddraw.dll is not a DLL";
        return false;
    }
    const std::vector<std::string> required = {
        "EFZDDRAW_GetNetplayPacingAbi",
        "EFZDDRAW_SetFrameLimit",
    };
    if (!peFileExportsSymbols(bytes, required, detail)) {
        error = "packaged ddraw.dll is missing the required pacing ABI ("
              + detail + ")";
        return false;
    }
    return true;
}

bool inspectExecutable(const std::string& path, ExecutableIdentity& out,
                       std::string& error) {
    out = {};
    error.clear();
    HANDLE file = openForReadW(path, "CreateFileW(game executable)", error);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE;
    if (!ok || size.QuadPart < 0) {
        error = winError("GetFileSizeEx(game executable)", GetLastError());
    } else {
        out.fileSize = static_cast<std::uint64_t>(size.QuadPart);
        ok = readPeIdentity(file, out.fileSize, out, error) &&
             sha256File(file, out.sha256, error);
    }
    CloseHandle(file);
    return ok;
}

namespace {

// The two attested 1126 executables differ only at file offset 0x71784 (VA 0x471784, sub_471680): 26 bytes, NOPped in the patched variant.
struct ExecutableVariant {
    const char* physical;
    const char* canonical;
    const char* key;
};
constexpr ExecutableVariant kAttestedVariants[] = {
    { "0562A1C33D54D8ECC5DA843A01352E317473506A63ECBDC6BC5BC796DFA72E5A",
      "A79002592953E8D9ACE3E363B3115CE4452C1A0789EB5B17C9BCB032AF27F1F6",
      "qoh99-se1126-original" },
};

bool canonicalizeKnownExecutableVariant(ExecutableIdentity& found) {
    for (const ExecutableVariant& variant : kAttestedVariants) {
        if (found.sha256 != variant.physical) continue;
        found.variantKey = variant.key;
        found.sha256 = variant.canonical;
        return true;
    }
    return false;
}

}

bool detectKnownRuntimeExecutable(const std::string& path,
                                  ExecutableIdentity* identity,
                                  std::string& error) {
    if (identity) *identity = {};
    ExecutableIdentity found;
    if (!inspectExecutable(path, found, error)) return false;
    found.physicalSha256 = found.sha256;
    canonicalizeKnownExecutableVariant(found);
    if (identity) *identity = found;

    const auto& supported = qoh99::generated_model::supportedRuntimeBuild();

    if (found.machine != IMAGE_FILE_MACHINE_I386) {
        char detail[448];
        std::snprintf(
            detail, sizeof(detail),
            "This is a %s executable.\n\n"
            "The netplay runtime is 32-bit and attaches to the original "
            "32-bit game, so it cannot attach to this program at all - this "
            "is not a matter of finding a different copy.\n\n"
            "Needed:  %s (32-bit, %llu bytes)",
            found.machine == 0x8664u ? "64-bit (x64)"
                                     : "non-x86",
            supported.displayName,
            static_cast<unsigned long long>(supported.binarySize));
        error = detail;
        return false;
    }

    const auto* hashProfile =
        qoh99::runtime_profile::findProfileBySha256(found.sha256.c_str());
    if (!hashProfile) {
        const auto* december =
            qoh99::runtime_profile::findProfile(
                qoh99::generated_model::RuntimeProfileId::
                    Qoh99DecemberCandidate);
        char detail[1024];
        std::snprintf(
            detail, sizeof(detail),
            "This copy of the game does not match either supported edition.\n\n"
            "Supported editions:\n"
            "  November: %s (%s)\n"
            "  SHA-256 %s\n"
            "  December: %s (%s)\n"
            "  SHA-256 %s\n"
            "Found:   SHA-256 %s\n"
            "         %llu bytes\n\n"
            "Netplay writes to fixed addresses that differ between editions, "
            "so it only runs on one of these exact builds; another release or "
            "a modified copy cannot be used.",
            supported.displayName, supported.runtimeProfileKey,
            supported.binarySha256,
            december ? december->displayName : "December edition",
            december ? december->runtimeProfileKey
                     : "qoh99-december-candidate",
            december ? december->binarySha256 : "<unavailable>",
            found.physicalSha256.c_str(),
            static_cast<unsigned long long>(found.fileSize));
        error = detail;
        return false;
    }
    const auto* profile =
        qoh99::runtime_profile::findProfileByExactExecutableEvidence(
            found.sha256.c_str(), found.fileSize, found.machine);
    if (!profile) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "Known profile hash has inconsistent executable evidence "
                      "(size=%llu machine=%04X; timestamp=%08X forensic)",
                      static_cast<unsigned long long>(found.fileSize),
                      found.machine, found.coffTimestamp);
        error = detail;
        return false;
    }
    found.runtimeProfileId = profile->runtimeProfileId;
    found.runtimeQualification = profile->runtimeQualification;
    found.executableIdentityEstablishesSimulationContent =
        profile->executableIdentityEstablishesSimulationContent;
    if (identity) *identity = found;
    error.clear();
    return true;
}

bool validateSupportedRuntimeExecutable(const std::string& path,
                                        ExecutableIdentity* identity,
                                        std::string& error) {
    ExecutableIdentity found;
    if (!detectKnownRuntimeExecutable(path, &found, error)) {
        if (identity) *identity = found;
        return false;
    }
    if (identity) *identity = found;

    const auto* profile =
        qoh99::runtime_profile::findProfile(found.runtimeProfileId);
    if (!profile) {
        error = "Recognized executable digest has no registered runtime "
                "profile; fixed-address injection is refused.";
        return false;
    }
    if (!qoh99::runtime_profile::profileAllowsHookInstall(*profile)) {
        error = qoh99::runtime_profile::unsupportedRuntimeProfileReason(
            *profile);
        return false;
    }
    error.clear();
    return true;
}

namespace {

qoh99::content::RuntimeProfile contentProfileFor(
    const qoh99::runtime_profile::RuntimeProfileId profile) {
    using GeneratedProfile = qoh99::runtime_profile::RuntimeProfileId;
    switch (profile) {
        case GeneratedProfile::Qoh99Se1126:
            return qoh99::content::RuntimeProfile::November1126;
        case GeneratedProfile::Qoh99DecemberCandidate:
            return qoh99::content::RuntimeProfile::DecemberCandidate;
        default:
            return qoh99::content::RuntimeProfile::Unknown;
    }
}

constexpr std::uint64_t compatibilityDomainId(const char* text) {
    std::uint64_t value = 1469598103934665603ull;
    while (text != nullptr && *text != '\0') {
        value ^= static_cast<std::uint8_t>(*text++);
        value *= 1099511628211ull;
    }
    return value;
}

constexpr std::uint64_t kContentIdentityStateSchema =
    qnetplay::compat::kStateSchemaCurrent;
constexpr std::uint64_t kContentIdentityModSimulation =
    compatibilityDomainId("qoh99-netplay-mod-simulation-content-identity-v1");
constexpr std::uint64_t kContentIdentityTimingProfile =
    compatibilityDomainId("qoh99-netplay-timing-profile-content-identity-v1");

bool makeCompatibilityIdentity(const ExecutableIdentity& executable,
                               const qoh99::content::ManifestResult& content,
                               qnetplay::compat::CompatibilityIdentity& out,
                               std::string& error) {
    out = {};
    qoh99::content::Digest executableDigest{};
    if (!qreplay::fromHex(executable.sha256, executableDigest)) {
        error = "exact executable identity has an invalid SHA-256 encoding";
        return false;
    }
    const qoh99::content::RuntimeProfile profile =
        contentProfileFor(executable.runtimeProfileId);
    if (!qnetplay::compat::setProfileId(out,
                                        qoh99::content::profileKey(profile))) {
        error = "runtime profile has no canonical compatibility profile ID";
        return false;
    }
    out.schemaVersion = qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    out.protocolVersion = qnetplay::wire::kVersion;
    std::copy(executableDigest.begin(), executableDigest.end(),
              out.runtimeBuild.executableSha256);
    out.runtimeBuild.executableSize = executable.fileSize;
    out.runtimeBuild.peTimestamp = executable.coffTimestamp;
    out.simulationContent.manifestSchemaVersion = content.identity.schemaVersion;
    out.simulationContent.manifestFlags = content.identity.manifestFlags;
    std::copy(content.identity.simulationRootSha256.begin(),
              content.identity.simulationRootSha256.end(),
              out.simulationContent.simulationRootSha256);
    std::copy(content.identity.validationRootSha256.begin(),
              content.identity.validationRootSha256.end(),
              out.simulationContent.validationRootSha256);
    out.simulationContent.entryCount = content.identity.entryCount;
    out.simulationContent.requiredEntryCount = content.identity.requiredEntryCount;
    out.stateSchemaId = kContentIdentityStateSchema;
    out.modSimulationId = kContentIdentityModSimulation;
    out.timingProfileId = kContentIdentityTimingProfile;
    if (!qnetplay::compat::isValidCompatibilityIdentity(out)) {
        error = "generated compatibility identity is incomplete";
        out = {};
        return false;
    }
    error.clear();
    return true;
}

bool inspectRuntimeInstallationWithVerification(
    const std::string& executablePath, const bool verifyActualBytes,
    RuntimeInstallationIdentity& out, std::string& error) {
    out = {};
    if (!detectKnownRuntimeExecutable(executablePath, &out.executable, error))
        return false;
    const qoh99::content::RuntimeProfile contentProfile =
        contentProfileFor(out.executable.runtimeProfileId);
    qoh99::content::ManifestBuildOptions options{};
    options.verifyActualBytes = verifyActualBytes;
    out.content = qoh99::content::SimulationContentManifestBuilder::build(
        std::filesystem::u8path(executablePath).parent_path(), contentProfile,
        options);
    if (!makeCompatibilityIdentity(out.executable, out.content,
                                   out.compatibility, error)) {
        return false;
    }
    return true;
}

}

bool inspectRuntimeInstallation(const std::string& executablePath,
                                RuntimeInstallationIdentity& out,
                                std::string& error) {
    return inspectRuntimeInstallationWithVerification(executablePath, false,
                                                      out, error);
}

bool validateRuntimeInstallationForAdmission(
    const std::string& executablePath, RuntimeInstallationIdentity* identity,
    std::string& error) {
    RuntimeInstallationIdentity observed{};
    if (!inspectRuntimeInstallationWithVerification(executablePath, true,
                                                    observed, error)) {
        if (identity) *identity = observed;
        return false;
    }
    if (identity) *identity = observed;
    if (!observed.content.admissionAllowed()) {
        error = std::string("runtime installation is not admitted: ")
            + qoh99::content::formatManifestDiagnosticHuman(observed.content);
        return false;
    }
    error.clear();
    return true;
}

bool validateNetplayDllContract(const std::string& path, std::string& error) {
    error.clear();
    std::vector<unsigned char> bytes;
    if (!readWholeFile(path, "netplay DLL", 64ull * 1024ull * 1024ull, bytes,
                       error))
        return false;
    if (!validateDllPeImage(bytes, error)) return false;

    const auto* marker = reinterpret_cast<const unsigned char*>(
        qipc::kBuildContractMarker);
    const std::size_t markerLength = std::strlen(qipc::kBuildContractMarker);
    if (std::search(bytes.begin(), bytes.end(), marker, marker + markerLength)
        == bytes.end()) {
        error =
            "qoh99caster.exe and qoh99_netplay.dll are from different builds "
            "(launcher/DLL ABI fingerprint mismatch). Copy both Release "
            "artifacts together.";
        return false;
    }
    return true;
}

bool makeRuntimeArtifactIdentity(
    const std::string& casterPath, const std::string& netplayDllPath,
    const std::string& ddrawPath, qnet::RuntimeArtifactIdentityWire& out,
    std::string& error) {
    out = {};
    if (!sha256ArtifactFile(casterPath, "caster executable",
                            out.casterSha256, error)
        || !sha256ArtifactFile(netplayDllPath, "netplay DLL",
                               out.netplayDllSha256, error)
        || !sha256ArtifactFile(ddrawPath, "ddraw wrapper", out.ddrawSha256,
                               error)) {
        out = {};
        return false;
    }
    out.schemaVersion = qnet::kRuntimeArtifactIdentitySchemaVersion;
    if (!qnet::isValidRuntimeArtifactIdentity(out)) {
        out = {};
        error = "runtime artifact identity is incomplete";
        return false;
    }
    error.clear();
    return true;
}

}
