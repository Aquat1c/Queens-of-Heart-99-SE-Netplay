#include "audio/WaveMetadata.h"

#include <cstring>
#include <new>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace qaudio {
namespace {

uint32_t le32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint16_t le16(const uint8_t* bytes) {
    return static_cast<uint16_t>(
        bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

uint32_t durationFrames(uint32_t dataBytes, uint32_t byteRate) {
    if (dataBytes == 0u || byteRate == 0u) return 0u;
    const uint64_t numerator = static_cast<uint64_t>(dataBytes) * 6060ull;
    const uint64_t denominator = static_cast<uint64_t>(byteRate) * 100ull;
    return static_cast<uint32_t>((numerator + denominator - 1ull)
                                 / denominator);
}

}

WaveFacts parseWaveFacts(const uint8_t* bytes, size_t fileBytes) {
    WaveFacts facts{};
    if (bytes == nullptr || fileBytes < 12u) return facts;
    if (std::memcmp(bytes, "RIFF", 4) != 0
        || std::memcmp(bytes + 8, "WAVE", 4) != 0) {
        return facts;
    }

    bool haveFormat = false;
    bool haveData = false;
    uint64_t offset = 12u;
    while (offset + 8u <= fileBytes && (!haveFormat || !haveData)) {
        const uint8_t* chunk = bytes + offset;
        const uint32_t chunkBytes = le32(chunk + 4);
        const uint64_t payload = offset + 8u;
        if (payload > fileBytes) return WaveFacts{};
        const uint64_t declaredEnd = payload + chunkBytes;
        if (declaredEnd > fileBytes) return WaveFacts{};

        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkBytes >= 16u) {
            const uint8_t* format = bytes + payload;
            facts.byteRate = le32(format + 8);
            facts.blockAlign = le16(format + 12);
            facts.bitsPerSample = le16(format + 14);
            haveFormat = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            facts.dataBytes = chunkBytes;
            haveData = true;
        }

        // Stock character WAVs omit the final pad byte, so accept EOF at the unpadded boundary of an odd chunk.
        uint64_t next = declaredEnd;
        if ((chunkBytes & 1u) != 0u) {
            if (declaredEnd == fileBytes) break;
            next += 1u;
        }
        if (next <= offset) return WaveFacts{};
        offset = next;
    }

    if (!haveFormat || !haveData
        || facts.byteRate == 0u || facts.dataBytes == 0u) {
        return WaveFacts{};
    }
    facts.durationFrames = durationFrames(facts.dataBytes, facts.byteRate);
    facts.valid = true;
    return facts;
}

WaveFacts loadWaveFactsFromFile(const char* path) {
#if defined(_WIN32)
    WaveFacts facts{};
    if (path == nullptr || path[0] == '\0') return facts;
    const DWORD savedLastError = GetLastError();
    HANDLE file = CreateFileA(
        path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0
            && size.QuadPart <= 64ll * 1024 * 1024) {
            const size_t total = static_cast<size_t>(size.QuadPart);
            uint8_t* buffer = new (std::nothrow) uint8_t[total];
            DWORD read = 0;
            if (buffer != nullptr
                && ReadFile(file, buffer, static_cast<DWORD>(total), &read,
                            nullptr)
                && read == total) {
                facts = parseWaveFacts(buffer, total);
            }
            delete[] buffer;
        }
        CloseHandle(file);
    }
    SetLastError(savedLastError);
    return facts;
#else
    (void)path;
    return WaveFacts{};
#endif
}

}
