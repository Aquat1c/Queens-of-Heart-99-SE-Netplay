#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace qgame {

constexpr size_t kPlayerNameStorageBytes = 32;
constexpr size_t kPlayerNameDisplayBytes = 20;

namespace detail {

struct EncodedPlayerNameCharacter {
    size_t bytes = 1;
    bool valid = false;
};

inline bool isOneUnicodeScalar(const wchar_t* converted, int count) {
    if (count == 1) {
        const uint16_t value = static_cast<uint16_t>(converted[0]);
        return value < 0xD800u || value > 0xDFFFu;
    }
    if (count != 2)
        return false;
    const uint16_t high = static_cast<uint16_t>(converted[0]);
    const uint16_t low = static_cast<uint16_t>(converted[1]);
    return high >= 0xD800u && high <= 0xDBFFu
        && low >= 0xDC00u && low <= 0xDFFFu;
}

inline EncodedPlayerNameCharacter encodedPlayerNameCharacter(
    std::string_view value, size_t offset, uint32_t codePage) {
    if (offset >= value.size())
        return {};
    const uint8_t first = static_cast<uint8_t>(value[offset]);
    if (first < 0x80u)
        return {1u, true};

    const size_t remaining = value.size() - offset;
    const size_t maximum = remaining < 4u ? remaining : 4u;
    for (size_t bytes = 1; bytes <= maximum; ++bytes) {
        wchar_t converted[2] = {};
        const int count = MultiByteToWideChar(
            static_cast<UINT>(codePage), MB_ERR_INVALID_CHARS,
            value.data() + offset, static_cast<int>(bytes),
            converted, static_cast<int>(sizeof(converted) / sizeof(converted[0])));
        if (isOneUnicodeScalar(converted, count))
            return {bytes, true};
    }
    return {1u, false};
}

}

inline bool isSupportedPlayerNameCodePage(uint32_t codePage) {
    if (codePage == 0u || !IsValidCodePage(static_cast<UINT>(codePage)))
        return false;
    CPINFO info{};
    if (!GetCPInfo(static_cast<UINT>(codePage), &info)
        || info.MaxCharSize == 0u || info.MaxCharSize > 4u) {
        return false;
    }
    wchar_t converted = 0;
    return MultiByteToWideChar(
               static_cast<UINT>(codePage), MB_ERR_INVALID_CHARS,
               "A", 1, &converted, 1) == 1
        && converted == L'A';
}

inline std::string canonicalPlayerNameForCodePage(
    std::string_view value, uint32_t codePage) {
    std::string out;
    out.reserve(kPlayerNameDisplayBytes);
    bool pendingSpace = false;
    size_t offset = 0;
    while (offset < value.size()) {
        const detail::EncodedPlayerNameCharacter character =
            detail::encodedPlayerNameCharacter(value, offset, codePage);
        bool whitespace = false;
        bool unsafe = !character.valid;
        // A valid CP932 trail byte may be 0x7C ('|') and must remain part of its multibyte character.
        if (character.valid && character.bytes == 1u) {
            const uint8_t ch =
                static_cast<uint8_t>(value[offset]);
            if (ch < 0x80u) {
                whitespace =
                    ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
                unsafe =
                    unsafe || ch < 0x20u || ch == 0x7Fu || ch == '|';
            }
        }
        if (whitespace || unsafe) {
            pendingSpace = !out.empty();
            offset += character.bytes;
            continue;
        }
        const size_t required =
            character.bytes + (pendingSpace ? 1u : 0u);
        if (out.size() + required > kPlayerNameDisplayBytes)
            break;
        if (pendingSpace)
            out.push_back(' ');
        pendingSpace = false;
        out.append(value.data() + offset, character.bytes);
        offset += character.bytes;
    }
    if (out.empty())
        out = "Player";
    return out;
}

inline std::string canonicalPlayerName(std::string_view value) {
    return canonicalPlayerNameForCodePage(value, GetACP());
}

inline bool isCanonicalPlayerNameForCodePage(
    std::string_view value, uint32_t codePage) {
    return !value.empty()
        && value.size() <= kPlayerNameDisplayBytes
        && isSupportedPlayerNameCodePage(codePage)
        && canonicalPlayerNameForCodePage(value, codePage) == value;
}

inline std::string playerNameForLocalDisplay(
    std::string_view value, uint32_t sourceCodePage) {
    const uint32_t localCodePage = GetACP();
    if (!isCanonicalPlayerNameForCodePage(value, sourceCodePage))
        return "Player";
    if (sourceCodePage == localCodePage)
        return std::string(value);

    const int wideBytes = MultiByteToWideChar(
        static_cast<UINT>(sourceCodePage), MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wideBytes <= 0)
        return "Player";
    std::vector<wchar_t> wide(static_cast<size_t>(wideBytes));
    if (MultiByteToWideChar(
            static_cast<UINT>(sourceCodePage), MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            wide.data(), wideBytes) != wideBytes) {
        return "Player";
    }
    const int localBytes = WideCharToMultiByte(
        static_cast<UINT>(localCodePage), 0,
        wide.data(), wideBytes, nullptr, 0, nullptr, nullptr);
    if (localBytes <= 0)
        return "Player";
    std::string local(static_cast<size_t>(localBytes), '\0');
    if (WideCharToMultiByte(
            static_cast<UINT>(localCodePage), 0,
            wide.data(), wideBytes, local.data(), localBytes,
            nullptr, nullptr) != localBytes) {
        return "Player";
    }
    return canonicalPlayerNameForCodePage(local, localCodePage);
}

inline std::string playerNameForUtf8Display(
    std::string_view value, uint32_t sourceCodePage) {
    if (!isCanonicalPlayerNameForCodePage(value, sourceCodePage))
        return "Player";
    if (sourceCodePage == CP_UTF8)
        return std::string(value);

    const int wideBytes = MultiByteToWideChar(
        static_cast<UINT>(sourceCodePage), MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wideBytes <= 0)
        return "Player";
    std::vector<wchar_t> wide(static_cast<size_t>(wideBytes));
    if (MultiByteToWideChar(
            static_cast<UINT>(sourceCodePage), MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            wide.data(), wideBytes) != wideBytes) {
        return "Player";
    }
    const int utf8Bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideBytes,
        nullptr, 0, nullptr, nullptr);
    if (utf8Bytes <= 0)
        return "Player";
    std::string utf8(static_cast<size_t>(utf8Bytes), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideBytes,
            utf8.data(), utf8Bytes, nullptr, nullptr) != utf8Bytes) {
        return "Player";
    }
    return utf8;
}

inline bool isNicknameOptionValue(std::string_view value) {
    return !value.empty() && value.front() != '-';
}

inline uint32_t directPlayLocalPlayerId(uint32_t localPlayer) {
    return localPlayer == 2u ? 2u : 1u;
}

inline uint32_t directPlayRemotePlayerId(uint32_t localPlayer) {
    return directPlayLocalPlayerId(localPlayer) == 1u ? 2u : 1u;
}

template <size_t N>
inline void storeCanonicalPlayerNameForCodePage(
    char (&destination)[N], std::string_view value, uint32_t codePage) {
    static_assert(N >= kPlayerNameDisplayBytes + 1,
                  "player-name storage is too small");
    const std::string canonical =
        canonicalPlayerNameForCodePage(value, codePage);
    std::memset(destination, 0, N);
    std::memcpy(destination, canonical.data(), canonical.size());
}

template <size_t N>
inline void storeCanonicalPlayerName(char (&destination)[N],
                                     std::string_view value) {
    storeCanonicalPlayerNameForCodePage(destination, value, GetACP());
}

inline const char* playerNameForId(uint32_t requestedPlayerId,
                                   uint32_t localPlayerId,
                                   uint32_t remotePlayerId,
                                   const char* localName,
                                   const char* remoteName) {
    if (requestedPlayerId == localPlayerId)
        return localName && localName[0] ? localName : "Player";
    if (requestedPlayerId == remotePlayerId)
        return remoteName && remoteName[0] ? remoteName : "Player";
    return "Player";
}

}
