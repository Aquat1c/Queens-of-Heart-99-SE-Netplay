#include "PeImage.h"
#include "WidePath.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <vector>

namespace qplat {
namespace {

constexpr uint32_t kMaxNtOffset = 0x10000000u;
constexpr uint32_t kMaxSizeOfImage = 0x7FFF0000u;
constexpr size_t kRemoteHeaderBytes = 0x1000;
constexpr uint32_t kMaxImportDescriptors = 1024;
constexpr uint32_t kMaxThunksPerDescriptor = 65536;
constexpr size_t kMaxImportNameChars = 512;

std::string winError(const char* what, DWORD error) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s (GetLastError=%lu)", what,
                  static_cast<unsigned long>(error));
    return buf;
}

bool protectionIsReadable(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0) return false;
    switch (protect & 0xFFu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

template <typename ReadableFn>
bool parseHeaders(const uint8_t* image, ReadableFn readable, PeImageInfo& out,
                  std::string& error) {
    if (!image) {
        error = "PE image pointer is null";
        return false;
    }
    if (!readable(0, sizeof(IMAGE_DOS_HEADER))) {
        error = "truncated DOS header";
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) {
        error = "missing MZ signature";
        return false;
    }
    if (dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) {
        error = "e_lfanew is negative or overlaps the DOS header";
        return false;
    }
    if ((static_cast<uint32_t>(dos.e_lfanew) & 3u) != 0) {
        error = "e_lfanew is not 4-byte aligned";
        return false;
    }
    const uint32_t ntOffset = static_cast<uint32_t>(dos.e_lfanew);
    if (ntOffset > kMaxNtOffset) {
        error = "e_lfanew is out of range";
        return false;
    }

    const uint32_t fileHeaderOffset = ntOffset + sizeof(DWORD);
    if (!readable(ntOffset, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))) {
        error = "truncated NT/COFF file header";
        return false;
    }
    DWORD signature = 0;
    std::memcpy(&signature, image + ntOffset, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE) {
        error = "missing PE\\0\\0 signature";
        return false;
    }
    IMAGE_FILE_HEADER coff{};
    std::memcpy(&coff, image + fileHeaderOffset, sizeof(coff));
    if (coff.Machine != IMAGE_FILE_MACHINE_I386) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "image is not PE32/i386 (machine=0x%04X)",
                      static_cast<unsigned>(coff.Machine));
        error = buf;
        return false;
    }
    if (coff.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)) {
        error = "SizeOfOptionalHeader is smaller than a PE32 optional header";
        return false;
    }

    const uint32_t optionalOffset = fileHeaderOffset + sizeof(IMAGE_FILE_HEADER);
    if (!readable(optionalOffset, sizeof(IMAGE_OPTIONAL_HEADER32))
        || !readable(optionalOffset, coff.SizeOfOptionalHeader)) {
        error = "truncated optional header";
        return false;
    }
    IMAGE_OPTIONAL_HEADER32 optional{};
    std::memcpy(&optional, image + optionalOffset, sizeof(optional));
    if (optional.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "optional header is not PE32 (magic=0x%04X)",
                      static_cast<unsigned>(optional.Magic));
        error = buf;
        return false;
    }
    if (optional.SizeOfImage == 0 || optional.SizeOfImage > kMaxSizeOfImage) {
        error = "SizeOfImage is zero or implausible";
        return false;
    }
    if (optional.AddressOfEntryPoint > optional.SizeOfImage) {
        error = "entry point lies outside the image";
        return false;
    }

    out = PeImageInfo{};
    out.base = 0;
    out.sizeOfImage = optional.SizeOfImage;
    out.machine = coff.Machine;
    out.timestamp = coff.TimeDateStamp;
    out.entryPointRva = optional.AddressOfEntryPoint;
    out.isDll = (coff.Characteristics & IMAGE_FILE_DLL) != 0;
    out.subsystem = optional.Subsystem;
    out.checkSum = optional.CheckSum;
    error.clear();
    return true;
}

bool importModuleMatches(const std::string& tableName, const char* wanted) {
    auto lower = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    auto stripDll = [](std::string s) {
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0)
            s.resize(s.size() - 4);
        return s;
    };
    const std::string a = lower(tableName);
    const std::string b = lower(std::string(wanted));
    if (a == b) return true;
    return stripDll(a) == stripDll(b);
}

}

bool isReadableRange(const void* p, size_t len) {
    if (!p) return false;
    if (len == 0) return true;
    const uintptr_t start = reinterpret_cast<uintptr_t>(p);
    if (start > UINTPTR_MAX - len) return false;
    const uintptr_t end = start + len;
    uintptr_t cursor = start;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi))
            != sizeof(mbi)) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) return false;
        if (!protectionIsReadable(mbi.Protect)) return false;
        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor) return false;
        cursor = regionEnd;
    }
    return true;
}

bool inspectFileImage(const uint8_t* bytes, size_t len, PeImageInfo& out,
                      std::string& error) {
    out = PeImageInfo{};
    error.clear();
    if (!bytes) {
        error = "PE buffer pointer is null";
        return false;
    }
    const auto readable = [len](size_t offset, size_t size) {
        return size <= len && offset <= len - size;
    };
    return parseHeaders(bytes, readable, out, error);
}

bool inspectLoadedImage(void* moduleBase, PeImageInfo& out,
                        std::string& error) {
    out = PeImageInfo{};
    error.clear();
    const uint8_t* image = static_cast<const uint8_t*>(moduleBase);
    const auto readable = [image](size_t offset, size_t size) {
        return isReadableRange(image + offset, size);
    };
    if (!parseHeaders(image, readable, out, error)) return false;
    out.base = reinterpret_cast<uintptr_t>(moduleBase);
    return true;
}

bool validateHostImage(void* moduleBase, PeImageInfo& out,
                       std::string& error) {
    if (!inspectLoadedImage(moduleBase, out, error)) return false;

    HMODULE resolved = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(moduleBase), &resolved)
        || static_cast<void*>(resolved) != moduleBase) {
        error = "address is not the base of a loaded module";
        out = PeImageInfo{};
        return false;
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(moduleBase);
    if (begin > UINTPTR_MAX - out.sizeOfImage) {
        error = "image range overflows the address space";
        out = PeImageInfo{};
        return false;
    }
    const uintptr_t end = begin + out.sizeOfImage;
    uintptr_t cursor = begin;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi))
            != sizeof(mbi)) {
            error = winError("VirtualQuery(host image)", GetLastError());
            out = PeImageInfo{};
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            error = "host image range is not fully committed";
            out = PeImageInfo{};
            return false;
        }
        if (reinterpret_cast<uintptr_t>(mbi.AllocationBase) != begin) {
            error = "host image range escapes the module's own mapping";
            out = PeImageInfo{};
            return false;
        }
        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor) {
            error = "VirtualQuery made no forward progress";
            out = PeImageInfo{};
            return false;
        }
        cursor = regionEnd;
    }
    return true;
}

bool findImportSlot(void* moduleBase, const char* dllName, const char* funcName,
                    IatSlot& out, std::string& error) {
    out = IatSlot{};
    error.clear();
    if (!dllName || !*dllName || !funcName || !*funcName) {
        error = "import lookup requires a module and function name";
        return false;
    }

    PeImageInfo info{};
    if (!inspectLoadedImage(moduleBase, info, error)) return false;

    const uint8_t* image = static_cast<const uint8_t*>(moduleBase);
    const uint32_t sizeOfImage = info.sizeOfImage;

    const auto rvaReadable = [&](uint32_t rva, uint32_t size) {
        if (rva == 0 || size == 0) return false;
        if (size > sizeOfImage || rva > sizeOfImage - size) return false;
        return isReadableRange(image + rva, size);
    };
    const auto readAsciiz = [&](uint32_t rva, size_t maxChars,
                                std::string& text) {
        text.clear();
        for (size_t i = 0; i < maxChars; ++i) {
            const uint64_t at = static_cast<uint64_t>(rva) + i;
            if (at >= sizeOfImage) return false;
            const uint8_t* byte = image + static_cast<size_t>(at);
            if (!isReadableRange(byte, 1)) return false;
            if (*byte == 0) return true;
            text.push_back(static_cast<char>(*byte));
        }
        return false;
    };

    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image, sizeof(dos));
    const uint32_t ntOffset = static_cast<uint32_t>(dos.e_lfanew);
    if (!isReadableRange(image + ntOffset, sizeof(IMAGE_NT_HEADERS32))) {
        error = "NT headers with data directories are not readable";
        return false;
    }
    IMAGE_NT_HEADERS32 headers{};
    std::memcpy(&headers, image + ntOffset, sizeof(headers));
    if (headers.OptionalHeader.NumberOfRvaAndSizes
        <= static_cast<DWORD>(IMAGE_DIRECTORY_ENTRY_IMPORT)) {
        error = "image declares no import data directory";
        return false;
    }
    const IMAGE_DATA_DIRECTORY directory =
        headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0
        || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        error = "image has an empty import directory";
        return false;
    }
    if (!rvaReadable(directory.VirtualAddress, directory.Size)) {
        error = "import directory lies outside readable image memory";
        return false;
    }

    uint32_t descriptorCount =
        directory.Size / static_cast<uint32_t>(sizeof(IMAGE_IMPORT_DESCRIPTOR));
    if (descriptorCount > kMaxImportDescriptors)
        descriptorCount = kMaxImportDescriptors;

    for (uint32_t i = 0; i < descriptorCount; ++i) {
        const uint32_t descriptorRva =
            directory.VirtualAddress
            + i * static_cast<uint32_t>(sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!rvaReadable(descriptorRva, sizeof(IMAGE_IMPORT_DESCRIPTOR))) break;
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        std::memcpy(&descriptor, image + descriptorRva, sizeof(descriptor));
        if (descriptor.Name == 0 && descriptor.FirstThunk == 0) break;
        if (descriptor.Name == 0) continue;

        std::string moduleName;
        if (!readAsciiz(descriptor.Name, 260, moduleName)) continue;
        if (!importModuleMatches(moduleName, dllName)) continue;
        if (descriptor.FirstThunk == 0) {
            error = "matching import descriptor has no IAT";
            return false;
        }
        if (descriptor.OriginalFirstThunk == 0
            && descriptor.TimeDateStamp == 0xFFFFFFFFu) {
            error = "import descriptor is bound and carries no name thunks";
            return false;
        }
        const uint32_t nameThunkRva = descriptor.OriginalFirstThunk != 0
            ? descriptor.OriginalFirstThunk
            : descriptor.FirstThunk;

        for (uint32_t t = 0; t < kMaxThunksPerDescriptor; ++t) {
            const uint32_t step =
                t * static_cast<uint32_t>(sizeof(IMAGE_THUNK_DATA32));
            if (step > sizeOfImage) break;
            const uint32_t nameThunkAt = nameThunkRva + step;
            const uint32_t iatAt = descriptor.FirstThunk + step;
            if (!rvaReadable(nameThunkAt, sizeof(IMAGE_THUNK_DATA32))) break;
            if (!rvaReadable(iatAt, sizeof(IMAGE_THUNK_DATA32))) break;

            IMAGE_THUNK_DATA32 nameThunk{};
            std::memcpy(&nameThunk, image + nameThunkAt, sizeof(nameThunk));
            if (nameThunk.u1.AddressOfData == 0) break;
            if (IMAGE_SNAP_BY_ORDINAL32(nameThunk.u1.Ordinal)) {
                continue;
            }
            const uint32_t byNameRva =
                static_cast<uint32_t>(nameThunk.u1.AddressOfData);
            if (!rvaReadable(byNameRva, sizeof(WORD) + 1)) continue;
            std::string importName;
            if (!readAsciiz(byNameRva + sizeof(WORD), kMaxImportNameChars,
                            importName)) {
                continue;
            }
            if (importName != funcName) continue;

            void** slot = reinterpret_cast<void**>(
                const_cast<uint8_t*>(image) + iatAt);
            if (!isReadableRange(slot, sizeof(void*))) {
                error = "resolved IAT slot is not readable";
                return false;
            }
            void* original = *slot;
            if (original == nullptr) {
                error = "IAT slot is not bound (null target)";
                return false;
            }
            if (!isReadableRange(original, 1)) {
                error = "IAT slot points at unreadable memory";
                return false;
            }
            out.slot = slot;
            out.original = original;
            return true;
        }
    }

    char buf[320];
    std::snprintf(buf, sizeof(buf), "import %.128s!%.128s was not found",
                  dllName, funcName);
    error = buf;
    return false;
}

bool patchImportSlot(IatSlot& slot, void* replacement, std::string& error) {
    error.clear();
    if (!slot.slot) {
        error = "import slot is null";
        return false;
    }
    if (!isReadableRange(slot.slot, sizeof(void*))) {
        error = "import slot is not readable";
        return false;
    }
    DWORD previous = 0;
    if (!VirtualProtect(slot.slot, sizeof(void*), PAGE_READWRITE, &previous)) {
        error = winError("VirtualProtect(import slot, PAGE_READWRITE)",
                         GetLastError());
        return false;
    }
    if (!slot.original) slot.original = *slot.slot;
    *slot.slot = replacement;
    DWORD restored = 0;
    const bool protectionRestored =
        VirtualProtect(slot.slot, sizeof(void*), previous, &restored) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), slot.slot, sizeof(void*));
    if (!protectionRestored) {
        error = winError("VirtualProtect(import slot, restore)",
                         GetLastError());
    }
    return true;
}

bool restoreImportSlot(const IatSlot& slot, std::string& error) {
    error.clear();
    if (!slot.slot) {
        error = "import slot is null";
        return false;
    }
    if (!slot.original) {
        error = "import slot has no recorded original target";
        return false;
    }
    if (!isReadableRange(slot.slot, sizeof(void*))) {
        error = "import slot is not readable";
        return false;
    }
    DWORD previous = 0;
    if (!VirtualProtect(slot.slot, sizeof(void*), PAGE_READWRITE, &previous)) {
        error = winError("VirtualProtect(import slot, PAGE_READWRITE)",
                         GetLastError());
        return false;
    }
    *slot.slot = slot.original;
    DWORD restored = 0;
    const bool protectionRestored =
        VirtualProtect(slot.slot, sizeof(void*), previous, &restored) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), slot.slot, sizeof(void*));
    if (!protectionRestored) {
        error = winError("VirtualProtect(import slot, restore)",
                         GetLastError());
    }
    return true;
}

bool remoteModuleBase(void* process, const wchar_t* moduleBaseName,
                      uintptr_t& base, std::string& error) {
    base = 0;
    error.clear();
    if (!process) {
        error = "remote module lookup requires a process handle";
        return false;
    }
    if (!moduleBaseName || !*moduleBaseName) {
        error = "remote module lookup requires a module base name";
        return false;
    }
    // INVALID_HANDLE_VALUE is bit-identical to the GetCurrentProcess() pseudo-handle and must not be rejected here.
    const DWORD pid = GetProcessId(static_cast<HANDLE>(process));
    if (pid == 0) {
        error = winError("GetProcessId", GetLastError());
        return false;
    }

    constexpr int kAttempts = 40;
    constexpr DWORD kRetryDelayMs = 25;
    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE) {
            lastError = GetLastError();
            if (lastError == ERROR_BAD_LENGTH
                || lastError == ERROR_PARTIAL_COPY) {
                Sleep(kRetryDelayMs);
                continue;
            }
            error = winError("CreateToolhelp32Snapshot", lastError);
            return false;
        }
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        if (Module32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szModule, moduleBaseName) == 0) {
                    base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                    found = true;
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        } else {
            lastError = GetLastError();
        }
        CloseHandle(snapshot);
        if (found) return base != 0;
        Sleep(kRetryDelayMs);
    }

    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "module '%.128s' was not present in pid %lu after %d "
                  "bounded retries (last GetLastError=%lu)",
                  narrow(moduleBaseName).c_str(),
                  static_cast<unsigned long>(pid), kAttempts,
                  static_cast<unsigned long>(lastError));
    error = buf;
    return false;
}

bool remoteValidateModule(void* process, uintptr_t base, PeImageInfo& out,
                          std::string& error) {
    out = PeImageInfo{};
    error.clear();
    if (!process) {
        error = "remote module validation requires a process handle";
        return false;
    }
    if (base == 0) {
        error = "remote module validation requires a module base";
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(static_cast<HANDLE>(process),
                       reinterpret_cast<LPCVOID>(base), &mbi, sizeof(mbi))
        != sizeof(mbi)) {
        error = winError("VirtualQueryEx(remote module)", GetLastError());
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        error = "remote module base is not committed memory";
        return false;
    }
    if (reinterpret_cast<uintptr_t>(mbi.AllocationBase) != base) {
        error = "remote address is not a module allocation base";
        return false;
    }
    if (mbi.Type != MEM_IMAGE) {
        error = "remote allocation is not a mapped image";
        return false;
    }

    std::vector<uint8_t> headers(kRemoteHeaderBytes, 0);
    SIZE_T read = 0;
    if (!ReadProcessMemory(static_cast<HANDLE>(process),
                           reinterpret_cast<LPCVOID>(base), headers.data(),
                           headers.size(), &read)) {
        const DWORD err = GetLastError();
        if (read < sizeof(IMAGE_DOS_HEADER)) {
            error = winError("ReadProcessMemory(remote PE headers)", err);
            return false;
        }
    }
    if (read < headers.size()) headers.resize(read);
    if (!inspectFileImage(headers.data(), headers.size(), out, error))
        return false;
    out.base = base;
    return true;
}

bool remoteModuleBaseOrValidatedCandidate(void* process,
                                          const wchar_t* moduleBaseName,
                                          const uintptr_t candidateBase,
                                          const uint32_t expectedSizeOfImage,
                                          const char* requiredExport,
                                          uintptr_t& base,
                                          bool& usedCandidate,
                                          std::string& error) {
    base = 0;
    usedCandidate = false;
    error.clear();

    std::string enumerateError;
    if (remoteModuleBase(process, moduleBaseName, base, enumerateError))
        return true;

    if (candidateBase == 0) {
        error = enumerateError.empty()
            ? std::string("module lookup failed and no candidate base was "
                          "supplied")
            : enumerateError;
        return false;
    }
    if (!requiredExport || !*requiredExport) {
        error = "a candidate base requires an export to validate against";
        return false;
    }

    const HANDLE target = static_cast<HANDLE>(process);
    const auto readRange = [&](uint32_t rva, void* dst, size_t bytes) -> bool {
        if (bytes == 0) return false;
        if (expectedSizeOfImage != 0
            && (static_cast<uint64_t>(rva) + bytes) > expectedSizeOfImage) {
            return false;
        }
        SIZE_T got = 0;
        return ReadProcessMemory(
                   target,
                   reinterpret_cast<LPCVOID>(candidateBase + rva),
                   dst, bytes, &got)
            && got == bytes;
    };

    std::vector<uint8_t> headers(kRemoteHeaderBytes, 0);
    if (!readRange(0, headers.data(), headers.size())) {
        error = "candidate module is not mapped yet (target loader has not "
                "run); base is unverified until after loader init";
        base = candidateBase;
        usedCandidate = true;
        return false;
    }

    PeImageInfo info{};
    std::string inspectError;
    if (!inspectFileImage(headers.data(), headers.size(), info,
                          inspectError)) {
        error = "candidate base does not hold a valid PE image: "
            + inspectError;
        return false;
    }
    if (info.machine != IMAGE_FILE_MACHINE_I386) {
        error = "candidate module is not PE32/i386";
        return false;
    }
    if (expectedSizeOfImage != 0 && info.sizeOfImage != expectedSizeOfImage) {
        error = "candidate module image size differs from the local module";
        return false;
    }

    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(headers.data());
    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            headers.data() + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0
        || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        error = "candidate module has no export directory";
        return false;
    }

    IMAGE_EXPORT_DIRECTORY exports{};
    if (!readRange(dir.VirtualAddress, &exports, sizeof(exports))) {
        error = "candidate module export directory is unreadable";
        return false;
    }
    const uint32_t nameCount = exports.NumberOfNames;
    const uint32_t namesRva = exports.AddressOfNames;
    constexpr uint32_t kMaxExportNames = 65536u;
    if (nameCount == 0 || nameCount > kMaxExportNames || namesRva == 0) {
        error = "candidate module export name table is out of range";
        return false;
    }
    std::vector<uint32_t> nameRvas(nameCount, 0);
    if (!readRange(namesRva, nameRvas.data(),
                   static_cast<size_t>(nameCount) * sizeof(uint32_t))) {
        error = "candidate module export name table is unreadable";
        return false;
    }

    bool matched = false;
    const size_t wanted = std::strlen(requiredExport);
    std::vector<char> nameBuffer(wanted + 2, 0);
    for (uint32_t i = 0; i < nameCount && !matched; ++i) {
        if (nameRvas[i] == 0) continue;
        std::fill(nameBuffer.begin(), nameBuffer.end(), '\0');
        if (!readRange(nameRvas[i], nameBuffer.data(), nameBuffer.size()))
            continue;
        if (nameBuffer[wanted] == '\0'
            && std::memcmp(nameBuffer.data(), requiredExport, wanted) == 0) {
            matched = true;
        }
    }
    if (!matched) {
        error = std::string("candidate module does not export '")
            + requiredExport + "'";
        return false;
    }

    base = candidateBase;
    usedCandidate = true;
    return true;
}

}
