#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr int kProtocolVersion = 1;

#ifndef QOH99_AGENT_GIT_HASH
#define QOH99_AGENT_GIT_HASH "unknown"
#endif

const char* kBuildStamp = QOH99_AGENT_GIT_HASH " " __DATE__ " " __TIME__;

constexpr int kLegacyAssetPathMax = 127;

constexpr size_t kConfigImageSize = 0x1F6C;
constexpr size_t kConfigProfileBase = 4;
constexpr size_t kConfigProfileStride = 0x2FC;
constexpr unsigned kConfigProfileCount = 5;
constexpr size_t kConfigNoSoundOffset = 376;
constexpr unsigned kConfigNoSoundBit = 0x00000002u;

struct Json;
using JsonPtr = std::shared_ptr<Json>;

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonPtr> items;
    std::vector<std::pair<std::string, JsonPtr>> members;

    static JsonPtr makeNull() { return std::make_shared<Json>(); }
    static JsonPtr makeBool(bool value) {
        auto node = std::make_shared<Json>();
        node->kind = Kind::Bool;
        node->boolean = value;
        return node;
    }
    static JsonPtr makeNumber(double value) {
        auto node = std::make_shared<Json>();
        node->kind = Kind::Number;
        node->number = value;
        return node;
    }
    static JsonPtr makeString(const std::string& value) {
        auto node = std::make_shared<Json>();
        node->kind = Kind::String;
        node->text = value;
        return node;
    }
    static JsonPtr makeArray() {
        auto node = std::make_shared<Json>();
        node->kind = Kind::Array;
        return node;
    }
    static JsonPtr makeObject() {
        auto node = std::make_shared<Json>();
        node->kind = Kind::Object;
        return node;
    }

    void set(const std::string& key, JsonPtr value) {
        members.emplace_back(key, std::move(value));
    }
    void set(const std::string& key, const std::string& value) {
        set(key, makeString(value));
    }
    void set(const std::string& key, const char* value) {
        set(key, makeString(value ? value : ""));
    }
    void set(const std::string& key, double value) {
        set(key, makeNumber(value));
    }
    void set(const std::string& key, bool value) { set(key, makeBool(value)); }

    const Json* find(const std::string& key) const {
        for (const auto& member : members) {
            if (member.first == key) {
                return member.second.get();
            }
        }
        return nullptr;
    }
};

void appendEscaped(std::string& out, const std::string& value) {
    out.push_back('"');
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", ch);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    out.push_back('"');
}

void serialize(const Json& node, std::string& out) {
    switch (node.kind) {
        case Json::Kind::Null:
            out += "null";
            break;
        case Json::Kind::Bool:
            out += node.boolean ? "true" : "false";
            break;
        case Json::Kind::Number: {
            char buffer[64];
            const double rounded = node.number;
            if (rounded == static_cast<double>(static_cast<long long>(rounded))) {
                std::snprintf(buffer, sizeof(buffer), "%lld",
                              static_cast<long long>(rounded));
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.10g", rounded);
            }
            out += buffer;
            break;
        }
        case Json::Kind::String:
            appendEscaped(out, node.text);
            break;
        case Json::Kind::Array: {
            out.push_back('[');
            for (size_t index = 0; index < node.items.size(); ++index) {
                if (index) out.push_back(',');
                serialize(*node.items[index], out);
            }
            out.push_back(']');
            break;
        }
        case Json::Kind::Object: {
            out.push_back('{');
            for (size_t index = 0; index < node.members.size(); ++index) {
                if (index) out.push_back(',');
                appendEscaped(out, node.members[index].first);
                out.push_back(':');
                serialize(*node.members[index].second, out);
            }
            out.push_back('}');
            break;
        }
    }
}

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    JsonPtr parse() {
        skipSpace();
        JsonPtr value = parseValue();
        skipSpace();
        return value;
    }

    bool ok() const { return ok_; }

private:
    const std::string& text_;
    size_t at_ = 0;
    bool ok_ = true;

    void skipSpace() {
        while (at_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[at_]))) {
            ++at_;
        }
    }

    bool consume(char expected) {
        if (at_ < text_.size() && text_[at_] == expected) {
            ++at_;
            return true;
        }
        ok_ = false;
        return false;
    }

    JsonPtr parseValue() {
        skipSpace();
        if (at_ >= text_.size()) {
            ok_ = false;
            return Json::makeNull();
        }
        const char ch = text_[at_];
        if (ch == '{') return parseObject();
        if (ch == '[') return parseArray();
        if (ch == '"') return Json::makeString(parseString());
        if (text_.compare(at_, 4, "true") == 0) {
            at_ += 4;
            return Json::makeBool(true);
        }
        if (text_.compare(at_, 5, "false") == 0) {
            at_ += 5;
            return Json::makeBool(false);
        }
        if (text_.compare(at_, 4, "null") == 0) {
            at_ += 4;
            return Json::makeNull();
        }
        return parseNumber();
    }

    JsonPtr parseObject() {
        auto node = Json::makeObject();
        consume('{');
        skipSpace();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return node;
        }
        while (ok_) {
            skipSpace();
            std::string key = parseString();
            skipSpace();
            if (!consume(':')) break;
            node->members.emplace_back(key, parseValue());
            skipSpace();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            consume('}');
            break;
        }
        return node;
    }

    JsonPtr parseArray() {
        auto node = Json::makeArray();
        consume('[');
        skipSpace();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return node;
        }
        while (ok_) {
            node->items.push_back(parseValue());
            skipSpace();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            consume(']');
            break;
        }
        return node;
    }

    std::string parseString() {
        std::string out;
        if (!consume('"')) return out;
        while (at_ < text_.size()) {
            const char ch = text_[at_++];
            if (ch == '"') return out;
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (at_ >= text_.size()) break;
            const char escape = text_[at_++];
            switch (escape) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (at_ + 4 > text_.size()) {
                        ok_ = false;
                        return out;
                    }
                    const std::string hex = text_.substr(at_, 4);
                    at_ += 4;
                    const unsigned code =
                        static_cast<unsigned>(std::strtoul(hex.c_str(), nullptr, 16));
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(
                            static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    out.push_back(escape);
            }
        }
        ok_ = false;
        return out;
    }

    JsonPtr parseNumber() {
        const size_t start = at_;
        while (at_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[at_])) ||
                text_[at_] == '-' || text_[at_] == '+' || text_[at_] == '.' ||
                text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
        }
        if (at_ == start) {
            ok_ = false;
            return Json::makeNull();
        }
        return Json::makeNumber(std::strtod(text_.substr(start, at_ - start).c_str(),
                                            nullptr));
    }
};

std::string jsonString(const Json* node, const std::string& fallback = "") {
    if (!node || node->kind != Json::Kind::String) return fallback;
    return node->text;
}

long long jsonInt(const Json* node, long long fallback = 0) {
    if (!node) return fallback;
    if (node->kind == Json::Kind::Number) {
        return static_cast<long long>(node->number);
    }
    if (node->kind == Json::Kind::String) {
        return std::strtoll(node->text.c_str(), nullptr, 10);
    }
    if (node->kind == Json::Kind::Bool) return node->boolean ? 1 : 0;
    return fallback;
}

bool jsonBool(const Json* node, bool fallback = false) {
    if (!node) return fallback;
    if (node->kind == Json::Kind::Bool) return node->boolean;
    if (node->kind == Json::Kind::Number) return node->number != 0.0;
    return fallback;
}

const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<unsigned char>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t index = 0;
    while (index + 2 < data.size()) {
        const unsigned value = (data[index] << 16) | (data[index + 1] << 8) |
                               data[index + 2];
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[value & 0x3F]);
        index += 3;
    }
    if (index + 1 == data.size()) {
        const unsigned value = data[index] << 16;
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out += "==";
    } else if (index + 2 == data.size()) {
        const unsigned value = (data[index] << 16) | (data[index + 1] << 8);
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::vector<unsigned char> base64Decode(const std::string& text) {
    std::vector<int> reverse(256, -1);
    for (int index = 0; index < 64; ++index) {
        reverse[static_cast<unsigned char>(kBase64Alphabet[index])] = index;
    }
    std::vector<unsigned char> out;
    int accumulator = 0;
    int bits = 0;
    for (unsigned char ch : text) {
        if (ch == '=' || reverse[ch] < 0) continue;
        accumulator = (accumulator << 6) | reverse[ch];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

struct Sha256 {
    uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint64_t length = 0;
    unsigned char buffer[64] = {};
    size_t pending = 0;

    static uint32_t rotr(uint32_t value, int count) {
        return (value >> count) | (value << (32 - count));
    }

    void compress(const unsigned char* block) {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
            0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
            0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
            0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
            0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
            0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
            0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
            0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
            0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        uint32_t w[64];
        for (int index = 0; index < 16; ++index) {
            w[index] = (block[index * 4] << 24) | (block[index * 4 + 1] << 16) |
                       (block[index * 4 + 2] << 8) | block[index * 4 + 3];
        }
        for (int index = 16; index < 64; ++index) {
            const uint32_t s0 = rotr(w[index - 15], 7) ^ rotr(w[index - 15], 18) ^
                                (w[index - 15] >> 3);
            const uint32_t s1 = rotr(w[index - 2], 17) ^ rotr(w[index - 2], 19) ^
                                (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int index = 0; index < 64; ++index) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + k[index] + w[index];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void update(const unsigned char* data, size_t size) {
        length += static_cast<uint64_t>(size) * 8;
        while (size) {
            const size_t take = (std::min)(sizeof(buffer) - pending, size);
            std::memcpy(buffer + pending, data, take);
            pending += take;
            data += take;
            size -= take;
            if (pending == sizeof(buffer)) {
                compress(buffer);
                pending = 0;
            }
        }
    }

    std::string finish() {
        const uint64_t total = length;
        unsigned char pad = 0x80;
        update(&pad, 1);
        length = total;
        pad = 0x00;
        while (pending != 56) {
            update(&pad, 1);
            length = total;
        }
        unsigned char tail[8];
        for (int index = 0; index < 8; ++index) {
            tail[index] = static_cast<unsigned char>((total >> (56 - index * 8)) & 0xFF);
        }
        std::memcpy(buffer + pending, tail, 8);
        compress(buffer);
        char out[65];
        for (int index = 0; index < 8; ++index) {
            std::snprintf(out + index * 8, 9, "%08x", state[index]);
        }
        return std::string(out, 64);
    }
};

std::wstring widen(const std::string& value) {
    if (value.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        &out[0], size);
    return out;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        &out[0], size, nullptr, nullptr);
    return out;
}

std::string joinPath(const std::string& parent, const std::string& child) {
    if (parent.empty()) return child;
    if (parent.back() == '\\' || parent.back() == '/') return parent + child;
    return parent + "\\" + child;
}

std::string foldCase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool pathIsDirectory(const std::string& path) {
    const DWORD attributes = GetFileAttributesW(widen(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool pathExists(const std::string& path) {
    return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

int bind_(SOCKET handle, const sockaddr_in& address) {
    return ::bind(handle, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address));
}

std::string lastErrorText(const char* label) {
    const DWORD error = GetLastError();
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s failed with GetLastError=%lu", label,
                  static_cast<unsigned long>(error));
    return buffer;
}

class AgentError : public std::exception {
public:
    explicit AgentError(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

struct SpawnedChild {
    HANDLE process = nullptr;
    HANDLE stdoutFile = nullptr;
    HANDLE stderrFile = nullptr;
    DWORD pid = 0;
    DWORD exitCode = 0;
    bool exited = false;
};

struct OpenedProcess {
    HANDLE handle = nullptr;
    DWORD pid = 0;
    bool suspended = false;
};

struct SuspendedThreadHandle {
    HANDLE handle = nullptr;
    DWORD tid = 0;
    DWORD pid = 0;
};

using NtProcessFn = LONG(NTAPI*)(HANDLE);

class Agent {
public:
    Agent(unsigned short port, long long leaseMs, std::string lockPath,
          bool useStdio)
        : port_(port), leaseMs_(leaseMs), lockPath_(std::move(lockPath)),
          useStdio_(useStdio) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        ntSuspend_ = reinterpret_cast<NtProcessFn>(
            GetProcAddress(ntdll, "NtSuspendProcess"));
        ntResume_ = reinterpret_cast<NtProcessFn>(
            GetProcAddress(ntdll, "NtResumeProcess"));
        touchLease();
    }

    int run();

private:
    unsigned short port_;
    long long leaseMs_;
    std::string lockPath_;
    bool useStdio_ = false;
    HANDLE lockFile_ = INVALID_HANDLE_VALUE;
    HANDLE singleInstance_ = nullptr;
    NtProcessFn ntSuspend_ = nullptr;
    NtProcessFn ntResume_ = nullptr;
    bool shutdownRequested_ = false;

    std::mutex leaseMutex_;
    std::chrono::steady_clock::time_point lastHeartbeat_;

    long long nextToken_ = 1;
    std::map<std::string, SpawnedChild> children_;
    std::map<std::string, OpenedProcess> processes_;
    std::map<std::string, SuspendedThreadHandle> threads_;
    std::vector<SOCKET> heldSockets_;

    bool acquireSingleInstance();
    void releaseSingleInstance();
    void touchLease();
    bool leaseExpired();
    void killAllChildren();

    std::string allocateToken(const char* prefix);
    JsonPtr dispatch(const Json& request);
    std::string handleLine(const std::string& line);
    int runStdio();
    int runTcp();

    JsonPtr verbHandshake(const Json& request);
    JsonPtr verbSpawn(const Json& request);
    JsonPtr verbProcPoll(const Json& request);
    JsonPtr verbProcStop(const Json& request);
    JsonPtr verbChildGamePid(const Json& request);
    JsonPtr verbOpenProcess(const Json& request);
    JsonPtr verbCloseProcess(const Json& request);
    JsonPtr verbProcAlive(const Json& request);
    JsonPtr verbProcExitCode(const Json& request);
    JsonPtr verbProcSuspend(const Json& request, bool suspend);
    JsonPtr verbProcTerminate(const Json& request);
    JsonPtr verbProcSetAffinity(const Json& request);
    JsonPtr verbMemRead(const Json& request);
    JsonPtr verbMemWrite(const Json& request);
    JsonPtr verbMemReadMany(const Json& request);
    JsonPtr verbSampleGameState(const Json& request);
    JsonPtr verbModuleBase(const Json& request);
    JsonPtr verbModuleMap(const Json& request);
    JsonPtr verbThreadSuspend(const Json& request);
    JsonPtr verbThreadResume(const Json& request);
    JsonPtr verbWindows(const Json& request);
    JsonPtr verbWindowInfo(const Json& request);
    JsonPtr verbPostClose(const Json& request);
    JsonPtr verbFsStat(const Json& request);
    JsonPtr verbFsGlob(const Json& request);
    JsonPtr verbFsRead(const Json& request);
    JsonPtr verbFsWrite(const Json& request);
    JsonPtr verbFsTail(const Json& request);
    JsonPtr verbFsMkdir(const Json& request);
    JsonPtr verbFsCopy(const Json& request);
    JsonPtr verbFsSha256(const Json& request);
    JsonPtr verbFsDiskUsage(const Json& request);
    JsonPtr verbCloneInstall(const Json& request);
    JsonPtr verbClonePayload(const Json& request);
    JsonPtr verbValidateAssetPaths(const Json& request);
    JsonPtr verbMuteAudio(const Json& request);
    JsonPtr verbConfigSnapshot(const Json& request);
    JsonPtr verbConfigMutate(const Json& request);
    JsonPtr verbRun(const Json& request);
    JsonPtr verbReservePort(const Json& request);
    JsonPtr verbReserveExclusiveUdp(const Json& request);

    OpenedProcess& requireProcess(const Json& request);
    SpawnedChild& requireChild(const Json& request);
};

bool Agent::acquireSingleInstance() {
    singleInstance_ = CreateMutexW(nullptr, TRUE, L"Global\\qoh99_e2e_agent");
    if (!singleInstance_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr,
                     "[agent] refusing to start: another qoh99_e2e_agent "
                     "already holds the single-instance mutex\n");
        return false;
    }
    lockFile_ = CreateFileW(widen(lockPath_).c_str(), GENERIC_WRITE,
                            0 , nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lockFile_ == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
                     "[agent] refusing to start: %s is held by another run "
                     "(GetLastError=%lu)\n",
                     lockPath_.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    return true;
}

void Agent::releaseSingleInstance() {
    if (lockFile_ != INVALID_HANDLE_VALUE) {
        CloseHandle(lockFile_);
        lockFile_ = INVALID_HANDLE_VALUE;
        DeleteFileW(widen(lockPath_).c_str());
    }
    if (singleInstance_) {
        ReleaseMutex(singleInstance_);
        CloseHandle(singleInstance_);
        singleInstance_ = nullptr;
    }
}

void Agent::touchLease() {
    std::lock_guard<std::mutex> guard(leaseMutex_);
    lastHeartbeat_ = std::chrono::steady_clock::now();
}

bool Agent::leaseExpired() {
    std::lock_guard<std::mutex> guard(leaseMutex_);
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - lastHeartbeat_)
                         .count();
    return age > leaseMs_;
}

void Agent::killAllChildren() {
    for (auto& entry : children_) {
        SpawnedChild& child = entry.second;
        if (child.process) {
            DWORD code = 0;
            if (GetExitCodeProcess(child.process, &code) && code == STILL_ACTIVE) {
                TerminateProcess(child.process, 1);
            }
            CloseHandle(child.process);
            child.process = nullptr;
        }
        if (child.stdoutFile) CloseHandle(child.stdoutFile);
        if (child.stderrFile) CloseHandle(child.stderrFile);
        child.stdoutFile = child.stderrFile = nullptr;
    }
    children_.clear();
    for (auto& entry : processes_) {
        if (entry.second.handle) CloseHandle(entry.second.handle);
    }
    processes_.clear();
    for (auto& entry : threads_) {
        if (entry.second.handle) CloseHandle(entry.second.handle);
    }
    threads_.clear();
    for (SOCKET held : heldSockets_) {
        closesocket(held);
    }
    heldSockets_.clear();
}

std::string Agent::allocateToken(const char* prefix) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s-%lld", prefix, nextToken_++);
    return buffer;
}

OpenedProcess& Agent::requireProcess(const Json& request) {
    const std::string handle = jsonString(request.find("handle"));
    auto found = processes_.find(handle);
    if (found == processes_.end()) {
        throw AgentError("unknown process handle " + handle);
    }
    return found->second;
}

SpawnedChild& Agent::requireChild(const Json& request) {
    const std::string token = jsonString(request.find("token"));
    auto found = children_.find(token);
    if (found == children_.end()) {
        throw AgentError("unknown child token " + token);
    }
    return found->second;
}

JsonPtr Agent::verbHandshake(const Json& request) {
    const long long protocol = jsonInt(request.find("protocol"), -1);
    if (protocol != kProtocolVersion) {
        throw AgentError("protocol mismatch: controller speaks " +
                         std::to_string(protocol) + ", agent speaks " +
                         std::to_string(kProtocolVersion));
    }
    const long long lease = jsonInt(request.find("lease_ms"), leaseMs_);
    if (lease > 0) leaseMs_ = lease;

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::string selfPath = narrow(self);

    Sha256 digest;
    HANDLE file = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string selfHash = "unavailable";
    if (file != INVALID_HANDLE_VALUE) {
        std::vector<unsigned char> block(1 << 20);
        DWORD got = 0;
        while (ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &got,
                        nullptr) &&
               got > 0) {
            digest.update(block.data(), got);
        }
        CloseHandle(file);
        selfHash = digest.finish();
    }

    SYSTEM_INFO info;
    GetSystemInfo(&info);

    auto result = Json::makeObject();
    result->set("protocol", static_cast<double>(kProtocolVersion));
    result->set("agent_build", kBuildStamp);
    result->set("agent_path", selfPath);
    result->set("agent_sha256", selfHash);
    result->set("lease_ms", static_cast<double>(leaseMs_));
    result->set("cpu_count", static_cast<double>(info.dwNumberOfProcessors));
    result->set("pointer_bits",
                static_cast<double>(sizeof(void*) * 8));

    auto configModel = Json::makeObject();
    configModel->set("image_size", static_cast<double>(kConfigImageSize));
    configModel->set("profile_base", static_cast<double>(kConfigProfileBase));
    configModel->set("profile_stride",
                     static_cast<double>(kConfigProfileStride));
    configModel->set("profile_count",
                     static_cast<double>(kConfigProfileCount));
    configModel->set("nosound_offset",
                     static_cast<double>(kConfigNoSoundOffset));
    configModel->set("nosound_bit", static_cast<double>(kConfigNoSoundBit));
    result->set("config_model", configModel);
    return result;
}

JsonPtr Agent::verbSpawn(const Json& request) {
    const Json* argvNode = request.find("argv");
    if (!argvNode || argvNode->kind != Json::Kind::Array || argvNode->items.empty()) {
        throw AgentError("spawn requires a non-empty argv array");
    }
    std::wstring commandLine;
    for (size_t index = 0; index < argvNode->items.size(); ++index) {
        const std::string argument = argvNode->items[index]->text;
        if (index) commandLine.push_back(L' ');
        const bool needsQuotes =
            argument.empty() ||
            argument.find_first_of(" \t\"") != std::string::npos;
        if (!needsQuotes) {
            commandLine += widen(argument);
            continue;
        }
        commandLine.push_back(L'"');
        size_t backslashes = 0;
        for (char ch : argument) {
            if (ch == '\\') {
                ++backslashes;
                continue;
            }
            if (ch == '"') {
                commandLine.append(backslashes * 2 + 1, L'\\');
            } else {
                commandLine.append(backslashes, L'\\');
            }
            backslashes = 0;
            commandLine.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(ch)));
        }
        commandLine.append(backslashes * 2, L'\\');
        commandLine.push_back(L'"');
    }

    std::wstring environmentBlock;
    const Json* envNode = request.find("env");
    if (envNode && envNode->kind == Json::Kind::Object) {
        // CreateProcessW requires the block sorted by name; build it that way
        // rather than hoping the caller's JSON order was already sorted.
        std::vector<std::wstring> entries;
        entries.reserve(envNode->members.size());
        for (const auto& member : envNode->members) {
            entries.push_back(widen(member.first) + L"=" +
                              widen(jsonString(member.second.get())));
        }
        std::sort(entries.begin(), entries.end());
        for (const auto& entry : entries) {
            environmentBlock += entry;
            environmentBlock.push_back(L'\0');
        }
        environmentBlock.push_back(L'\0');
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    auto openLog = [&](const std::string& path) -> HANDLE {
        if (path.empty()) return nullptr;
        HANDLE handle = CreateFileW(widen(path).c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &inheritable, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw AgentError("could not create " + path + ": " +
                             lastErrorText("CreateFileW"));
        }
        return handle;
    };

    SpawnedChild child;
    child.stdoutFile = openLog(jsonString(request.find("stdout")));
    child.stderrFile = openLog(jsonString(request.find("stderr")));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = child.stdoutFile;
    startup.hStdError = child.stderrFile;

    PROCESS_INFORMATION created{};
    const std::string cwd = jsonString(request.find("cwd"));
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    // CREATE_NO_WINDOW mirrors WindowsProcessControl.creation_flags.  Under
    // Wine there is no desktop to spray consoles across, but keeping the flag
    // means the child sees the identical creation environment on both sides.
    const BOOL ok = CreateProcessW(
        nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        environmentBlock.empty() ? nullptr
                                 : const_cast<wchar_t*>(environmentBlock.c_str()),
        cwd.empty() ? nullptr : widen(cwd).c_str(), &startup, &created);
    if (!ok) {
        if (child.stdoutFile) CloseHandle(child.stdoutFile);
        if (child.stderrFile) CloseHandle(child.stderrFile);
        throw AgentError(lastErrorText("CreateProcessW") + " for " +
                         narrow(commandLine));
    }
    CloseHandle(created.hThread);
    child.process = created.hProcess;
    child.pid = created.dwProcessId;

    const std::string token = allocateToken("proc");
    children_[token] = child;

    auto result = Json::makeObject();
    result->set("token", token);
    result->set("pid", static_cast<double>(child.pid));
    return result;
}

JsonPtr Agent::verbProcPoll(const Json& request) {
    SpawnedChild& child = requireChild(request);
    auto result = Json::makeObject();
    if (child.exited) {
        result->set("running", false);
        result->set("returncode", static_cast<double>(child.exitCode));
        return result;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(child.process, &code)) {
        throw AgentError(lastErrorText("GetExitCodeProcess"));
    }
    if (code == STILL_ACTIVE) {
        result->set("running", true);
        return result;
    }
    child.exited = true;
    child.exitCode = code;
    result->set("running", false);
    result->set("returncode", static_cast<double>(code));
    return result;
}

JsonPtr Agent::verbProcStop(const Json& request) {
    SpawnedChild& child = requireChild(request);
    if (child.process) {
        // Wine has no Ctrl-Break story that matches Windows' terminate(), and
        // the harness's contract is only "stop it, then force it", so both
        // steps map onto TerminateProcess with different exit codes so the
        // escalation is still visible in the report.
        TerminateProcess(child.process, jsonBool(request.find("force")) ? 1 : 0);
    }
    return Json::makeObject();
}

JsonPtr Agent::verbChildGamePid(const Json& request) {
    const DWORD parent = static_cast<DWORD>(jsonInt(request.find("parent_pid")));
    const std::string wanted = foldCase(jsonString(request.find("image")));
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw AgentError(lastErrorText("CreateToolhelp32Snapshot(process)"));
    }
    DWORD found = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parent &&
                foldCase(narrow(entry.szExeFile)) == wanted) {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    auto result = Json::makeObject();
    result->set("pid", static_cast<double>(found));
    return result;
}

JsonPtr Agent::verbOpenProcess(const Json& request) {
    const DWORD pid = static_cast<DWORD>(jsonInt(request.find("pid")));
    DWORD access = PROCESS_TERMINATE | PROCESS_VM_OPERATION | PROCESS_VM_READ |
                   PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION |
                   PROCESS_SUSPEND_RESUME;
    if (jsonBool(request.find("set_information"))) {
        access |= PROCESS_SET_INFORMATION;
    }
    HANDLE handle = OpenProcess(access, FALSE, pid);
    if (!handle) {
        throw AgentError(lastErrorText("OpenProcess") + " for pid " +
                         std::to_string(pid));
    }
    OpenedProcess opened;
    opened.handle = handle;
    opened.pid = pid;
    const std::string token = allocateToken("mem");
    processes_[token] = opened;
    auto result = Json::makeObject();
    result->set("handle", token);
    result->set("pid", static_cast<double>(pid));
    return result;
}

JsonPtr Agent::verbCloseProcess(const Json& request) {
    const std::string handle = jsonString(request.find("handle"));
    auto found = processes_.find(handle);
    if (found != processes_.end()) {
        if (found->second.handle) CloseHandle(found->second.handle);
        processes_.erase(found);
    }
    return Json::makeObject();
}

JsonPtr Agent::verbProcAlive(const Json& request) {
    OpenedProcess& process = requireProcess(request);
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process.handle, &code) != 0 &&
                       code == STILL_ACTIVE;
    auto result = Json::makeObject();
    result->set("alive", alive);
    return result;
}

JsonPtr Agent::verbProcExitCode(const Json& request) {
    OpenedProcess& process = requireProcess(request);
    DWORD code = 0;
    if (!GetExitCodeProcess(process.handle, &code)) {
        throw AgentError(lastErrorText("GetExitCodeProcess"));
    }
    auto result = Json::makeObject();
    result->set("code", static_cast<double>(code));
    return result;
}

JsonPtr Agent::verbProcSuspend(const Json& request, bool suspend) {
    OpenedProcess& process = requireProcess(request);
    NtProcessFn fn = suspend ? ntSuspend_ : ntResume_;
    if (!fn) {
        throw AgentError("ntdll does not expose NtSuspendProcess/NtResumeProcess");
    }
    const LONG status = fn(process.handle);
    if (status < 0) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer),
                      "%s(%lu) failed NTSTATUS=0x%08lX",
                      suspend ? "NtSuspendProcess" : "NtResumeProcess",
                      static_cast<unsigned long>(process.pid),
                      static_cast<unsigned long>(status));
        throw AgentError(buffer);
    }
    process.suspended = suspend;
    return Json::makeObject();
}

JsonPtr Agent::verbProcTerminate(const Json& request) {
    OpenedProcess& process = requireProcess(request);
    TerminateProcess(process.handle, 0);
    return Json::makeObject();
}

JsonPtr Agent::verbProcSetAffinity(const Json& request) {
    OpenedProcess& process = requireProcess(request);
    const DWORD_PTR mask = static_cast<DWORD_PTR>(jsonInt(request.find("mask")));
    if (mask == 0) {
        throw AgentError("invalid zero affinity mask for process " +
                         std::to_string(process.pid));
    }
    if (!SetProcessAffinityMask(process.handle, mask)) {
        throw AgentError(lastErrorText("SetProcessAffinityMask"));
    }
    return Json::makeObject();
}

// -- verbs: memory ----------------------------------------------------------

JsonPtr Agent::verbMemRead(const Json& request) {
    OpenedProcess& process = requireProcess(request);
    const uintptr_t address =
        static_cast<uintptr_t>(jsonInt(request.find("address")));
    const size_t size = static_cast<size_t>(jsonInt(request.find("size")));
    std::vector<unsigned char> buffer(size);
    SIZE_T got = 0;
    if (!ReadProcessMemory(process.handle, reinterpret_cast<LPCVOID>(address),
                           buffer.data(), size, &got) ||
        got != size) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "ReadProcessMemory(%lu,0x%llX) failed GetLastError=%lu",
                      static_cast<unsigned long>(process.pid),
                      static_cast<unsigned long long>(address),
                      static_cast<unsigned long>(GetLastError()));
        throw AgentError(message);
    }
    auto result = Json::makeObject();
    result->set("data", base64Encode(buffer));
    return result;
}

JsonPtr Agent::verbMemWrite(const Json& request) {
    OpenedProcess& process = requireProcess(request);
    const uintptr_t address =
        static_cast<uintptr_t>(jsonInt(request.find("address")));
    const std::vector<unsigned char> data =
        base64Decode(jsonString(request.find("data")));
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(process.handle, reinterpret_cast<LPVOID>(address),
                            data.data(), data.size(), &wrote) ||
        wrote != data.size()) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "WriteProcessMemory(%lu,0x%llX) failed GetLastError=%lu",
                      static_cast<unsigned long>(process.pid),
                      static_cast<unsigned long long>(address),
                      static_cast<unsigned long>(GetLastError()));
        throw AgentError(message);
    }
    return Json::makeObject();
}

// Read a {name,address,type} spec array into `out`.  Returns "" on success or
// the ReadProcessMemory diagnostic on the first failure, in which case `out`
// is not to be used -- a partial read discards the partially-filled map.
std::string readScalarSpecs(OpenedProcess& process, const Json* specs,
                            Json& out) {
    if (!specs || specs->kind != Json::Kind::Array) return std::string();
    for (const auto& spec : specs->items) {
        const std::string name = jsonString(spec->find("name"));
        const uintptr_t address =
            static_cast<uintptr_t>(jsonInt(spec->find("address")));
        const std::string kind = jsonString(spec->find("type"), "u32");
        unsigned char raw[4] = {};
        SIZE_T got = 0;
        if (!ReadProcessMemory(process.handle,
                               reinterpret_cast<LPCVOID>(address), raw, 4,
                               &got) ||
            got != 4) {
            char message[128];
            std::snprintf(message, sizeof(message),
                          "ReadProcessMemory(%lu,0x%llX) GetLastError=%lu",
                          static_cast<unsigned long>(process.pid),
                          static_cast<unsigned long long>(address),
                          static_cast<unsigned long>(GetLastError()));
            return message;
        }
        const uint32_t unsignedValue = static_cast<uint32_t>(raw[0]) |
                                       (static_cast<uint32_t>(raw[1]) << 8) |
                                       (static_cast<uint32_t>(raw[2]) << 16) |
                                       (static_cast<uint32_t>(raw[3]) << 24);
        if (kind == "i32") {
            out.set(name,
                    static_cast<double>(static_cast<int32_t>(unsignedValue)));
        } else {
            out.set(name, static_cast<double>(unsignedValue));
        }
    }
    return std::string();
}

JsonPtr Agent::verbMemReadMany(const Json& request) {
    // Batched scalar read WITHOUT the liveness probe and the EnumWindows
    // sweep that sample_game_state performs.
    //
    // It exists because counters that are COMPARED TO EACH OTHER must be
    // sampled with no game time between them.  The returned-character-select
    // dwell asserts dispatchDelta >= canonicalDelta exactly; fetching those
    // two with separate round trips let a 60 Hz tick land between them and
    // produced `dispatchDelta=181 canonicalDelta=182` on a perfectly healthy
    // remote game.  It is also called from a poll loop, where one round trip
    // instead of three is the difference between keeping and missing a
    // barrier.
    OpenedProcess& process = requireProcess(request);
    auto values = Json::makeObject();
    const std::string readError =
        readScalarSpecs(process, request.find("specs"), *values);
    if (!readError.empty()) throw AgentError(readError);
    auto result = Json::makeObject();
    result->set("values", values);
    return result;
}

JsonPtr Agent::verbSampleGameState(const Json& request) {
    // ONE round trip for what is 31 syscalls on the harness side: liveness,
    // every GAME_ADDR scalar, and the window titles.  The harness polls this
    // on a 2 ms cadence inside an 8 s same-idle-frame barrier, so a
    // per-scalar RPC would not merely be slow -- it would fail the barrier and
    // the failure would look like a game problem.
    OpenedProcess& process = requireProcess(request);
    auto result = Json::makeObject();
    result->set("pid", static_cast<double>(process.pid));

    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process.handle, &code) != 0 &&
                       code == STILL_ACTIVE;
    result->set("alive", alive);

    auto titles = Json::makeArray();
    struct Collector {
        DWORD pid;
        std::vector<std::string>* out;
    };
    std::vector<std::string> windowTitles;
    Collector collector{process.pid, &windowTitles};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<Collector*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner != state->pid) return TRUE;
            const int length = GetWindowTextLengthW(hwnd);
            if (length <= 0) return TRUE;
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(hwnd, &text[0], length + 1);
            text.resize(static_cast<size_t>(wcslen(text.c_str())));
            if (!text.empty()) {
                std::string narrowed = narrow(text);
                if (std::find(state->out->begin(), state->out->end(), narrowed) ==
                    state->out->end()) {
                    state->out->push_back(narrowed);
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));
    std::sort(windowTitles.begin(), windowTitles.end());

    if (!alive) {
        for (const auto& title : windowTitles) {
            titles->items.push_back(Json::makeString(title));
        }
        result->set("titles", titles);
        return result;
    }

    auto values = Json::makeObject();
    const std::string readError =
        readScalarSpecs(process, request.find("specs"), *values);
    // A partial read discards the partially-filled map, exactly as the
    // harness's original inline loop did when an OSError interrupted it.
    if (readError.empty()) {
        result->set("values", values);
    } else {
        result->set("read_error", readError);
    }
    for (const auto& title : windowTitles) {
        titles->items.push_back(Json::makeString(title));
    }
    result->set("titles", titles);
    return result;
}

// -- verbs: modules ---------------------------------------------------------

JsonPtr Agent::verbModuleBase(const Json& request) {
    const DWORD pid = static_cast<DWORD>(jsonInt(request.find("pid")));
    const std::string wanted = foldCase(jsonString(request.find("name")));
    auto result = Json::makeObject();

    // CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) fails with ERROR_BAD_LENGTH
    // while the target is still mapping modules and MSDN requires a retry.
    // Without this the injected DLL intermittently reports "not loaded" while
    // it is provably running -- the same transient the harness documents.
    const DWORD deadline = GetTickCount() + 2000;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (;;) {
        snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE) break;
        if (GetTickCount() >= deadline) {
            result->set("found", false);
            return result;
        }
        Sleep(20);
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (foldCase(narrow(entry.szModule)) == wanted) {
                result->set("found", true);
                result->set("base", static_cast<double>(
                                        reinterpret_cast<uintptr_t>(entry.modBaseAddr)));
                result->set("size", static_cast<double>(entry.modBaseSize));
                result->set("path", narrow(entry.szExePath));
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!found) result->set("found", false);
    return result;
}

JsonPtr Agent::verbModuleMap(const Json& request) {
    const DWORD pid = static_cast<DWORD>(jsonInt(request.find("pid")));
    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw AgentError(lastErrorText("CreateToolhelp32Snapshot(modules)"));
    }
    auto modules = Json::makeArray();
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            auto item = Json::makeObject();
            item->set("name", narrow(entry.szModule));
            item->set("path", narrow(entry.szExePath));
            item->set("base", static_cast<double>(
                                  reinterpret_cast<uintptr_t>(entry.modBaseAddr)));
            item->set("size", static_cast<double>(entry.modBaseSize));
            modules->items.push_back(item);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    std::sort(modules->items.begin(), modules->items.end(),
              [](const JsonPtr& left, const JsonPtr& right) {
                  return jsonInt(left->find("base")) < jsonInt(right->find("base"));
              });
    auto result = Json::makeObject();
    result->set("modules", modules);
    return result;
}

// -- verbs: threads ---------------------------------------------------------

JsonPtr Agent::verbThreadSuspend(const Json& request) {
    const DWORD pid = static_cast<DWORD>(jsonInt(request.find("pid")));
    const DWORD tid = static_cast<DWORD>(jsonInt(request.find("tid")));
    DWORD access = THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION;
    if (jsonBool(request.find("want_context"))) {
        access |= THREAD_GET_CONTEXT;
    }
    HANDLE handle = OpenThread(access, FALSE, tid);
    if (!handle) {
        throw AgentError(lastErrorText("OpenThread") + " for tid " +
                         std::to_string(tid));
    }
    const DWORD owner = GetProcessIdOfThread(handle);
    if (owner != pid) {
        CloseHandle(handle);
        if (owner == 0) {
            throw AgentError(lastErrorText("GetProcessIdOfThread"));
        }
        throw AgentError("frame thread " + std::to_string(tid) +
                         " belongs to pid=" + std::to_string(owner) +
                         ", expected pid=" + std::to_string(pid));
    }
    const DWORD previous = SuspendThread(handle);
    if (previous == static_cast<DWORD>(-1)) {
        CloseHandle(handle);
        throw AgentError(lastErrorText("SuspendThread"));
    }
    if (previous != 0) {
        // A thread that was ALREADY suspended means somebody else is driving
        // this process, and every frame-exact measurement taken while it is
        // stopped would be fiction.  Undo and refuse, exactly as the harness
        // does locally.
        ResumeThread(handle);
        CloseHandle(handle);
        throw AgentError("frame thread " + std::to_string(tid) +
                         " was already suspended (prior count " +
                         std::to_string(previous) + ")");
    }
    SuspendedThreadHandle record;
    record.handle = handle;
    record.tid = tid;
    record.pid = pid;
    const std::string token = allocateToken("thread");
    threads_[token] = record;
    auto result = Json::makeObject();
    result->set("token", token);
    result->set("previous", static_cast<double>(previous));
    return result;
}

JsonPtr Agent::verbThreadResume(const Json& request) {
    const std::string token = jsonString(request.find("token"));
    auto found = threads_.find(token);
    if (found == threads_.end()) {
        throw AgentError("unknown thread token " + token);
    }
    SuspendedThreadHandle record = found->second;
    threads_.erase(found);
    const DWORD previous = ResumeThread(record.handle);
    CloseHandle(record.handle);
    if (previous == static_cast<DWORD>(-1)) {
        throw AgentError(lastErrorText("ResumeThread"));
    }
    if (previous != 1) {
        throw AgentError("ResumeThread(" + std::to_string(record.tid) +
                         ") observed unexpected prior suspend count " +
                         std::to_string(previous));
    }
    auto result = Json::makeObject();
    result->set("previous", static_cast<double>(previous));
    return result;
}

// -- verbs: windows ---------------------------------------------------------

JsonPtr Agent::verbWindows(const Json& request) {
    const DWORD pid = static_cast<DWORD>(jsonInt(request.find("pid")));
    struct Collector {
        DWORD pid;
        std::vector<std::pair<HWND, std::string>>* out;
    };
    std::vector<std::pair<HWND, std::string>> windows;
    Collector collector{pid, &windows};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<Collector*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner != state->pid) return TRUE;
            const int length = GetWindowTextLengthW(hwnd);
            if (length <= 0) return TRUE;
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(hwnd, &text[0], length + 1);
            text.resize(static_cast<size_t>(wcslen(text.c_str())));
            if (!text.empty()) {
                state->out->emplace_back(hwnd, narrow(text));
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));
    // Same ordering rule as process_windows: by title, then by handle, so the
    // harness's "first preferred window" is chosen identically on both sides.
    std::sort(windows.begin(), windows.end(),
              [](const std::pair<HWND, std::string>& left,
                 const std::pair<HWND, std::string>& right) {
                  if (left.second != right.second) return left.second < right.second;
                  return reinterpret_cast<uintptr_t>(left.first) <
                         reinterpret_cast<uintptr_t>(right.first);
              });
    auto list = Json::makeArray();
    for (const auto& window : windows) {
        auto item = Json::makeObject();
        item->set("hwnd", static_cast<double>(
                              reinterpret_cast<uintptr_t>(window.first)));
        item->set("title", window.second);
        list->items.push_back(item);
    }
    auto result = Json::makeObject();
    result->set("windows", list);
    return result;
}

JsonPtr Agent::verbWindowInfo(const Json& request) {
    HWND hwnd = reinterpret_cast<HWND>(
        static_cast<uintptr_t>(jsonInt(request.find("hwnd"))));
    DWORD owner = 0;
    const DWORD thread = GetWindowThreadProcessId(hwnd, &owner);
    auto result = Json::makeObject();
    result->set("thread_id", static_cast<double>(thread));
    result->set("owner_pid", static_cast<double>(owner));
    result->set("is_window", IsWindow(hwnd) != 0);
    return result;
}

JsonPtr Agent::verbPostClose(const Json& request) {
    HWND hwnd = reinterpret_cast<HWND>(
        static_cast<uintptr_t>(jsonInt(request.find("hwnd"))));
    SetLastError(0);
    const BOOL posted = PostMessageW(hwnd, WM_CLOSE, 0, 0);
    auto result = Json::makeObject();
    result->set("posted", posted != 0);
    result->set("error", static_cast<double>(posted ? 0 : GetLastError()));
    return result;
}

// -- verbs: filesystem ------------------------------------------------------

void enumerateFiles(const std::string& root, const std::string& relative,
                    const std::vector<std::string>& skipDirs,
                    std::vector<std::string>* out) {
    const std::string directory = relative.empty() ? root : joinPath(root, relative);
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(widen(joinPath(directory, "*")).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = narrow(data.cFileName);
        if (name == "." || name == "..") continue;
        const std::string childRelative =
            relative.empty() ? name : joinPath(relative, name);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const std::string folded = foldCase(name);
            if (std::find(skipDirs.begin(), skipDirs.end(), folded) !=
                skipDirs.end()) {
                continue;
            }
            enumerateFiles(root, childRelative, skipDirs, out);
        } else {
            out->push_back(childRelative);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

bool cloneSkipsName(const std::string& name) {
    // Byte-for-byte the harness's clone filter: mutable caster state is never
    // part of a game installation, and inheriting a source install's replay
    // library would make a "fresh recording" gate pass against somebody
    // else's artifact.
    static const char* kSkip[] = {"qoh99_netplay.dll", "qoh99caster.exe",
                                  "qoh99caster.ini",  "ddraw.dll",
                                  "ddraw_wrapper.log", "qoherror.txt"};
    const std::string folded = foldCase(name);
    for (const char* candidate : kSkip) {
        if (folded == candidate) return true;
    }
    const size_t dot = folded.rfind('.');
    if (dot != std::string::npos) {
        const std::string suffix = folded.substr(dot);
        if (suffix == ".log" || suffix == ".dmp") return true;
    }
    return false;
}

void makeDirectories(const std::string& path) {
    if (path.empty() || pathIsDirectory(path)) return;
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos && slash > 2) {
        makeDirectories(path.substr(0, slash));
    }
    CreateDirectoryW(widen(path).c_str(), nullptr);
}

JsonPtr Agent::verbFsStat(const Json& request) {
    const std::string path = jsonString(request.find("path"));
    WIN32_FILE_ATTRIBUTE_DATA data{};
    auto result = Json::makeObject();
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &data)) {
        result->set("exists", false);
        result->set("size", 0.0);
        result->set("mtime_ns", 0.0);
        result->set("is_dir", false);
        return result;
    }
    const unsigned long long size =
        (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
        data.nFileSizeLow;
    // FILETIME is 100 ns ticks since 1601; convert to Unix nanoseconds so the
    // number the harness compares has the same meaning it does locally.  The
    // comparison is always remote-vs-remote, never remote-vs-controller.
    unsigned long long ticks =
        (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime)
         << 32) |
        data.ftLastWriteTime.dwLowDateTime;
    const unsigned long long kEpochDelta = 116444736000000000ULL;
    const unsigned long long unixNs =
        ticks > kEpochDelta ? (ticks - kEpochDelta) * 100ULL : 0ULL;
    result->set("exists", true);
    result->set("size", static_cast<double>(size));
    result->set("mtime_ns", static_cast<double>(unixNs));
    result->set("is_dir",
                (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    return result;
}

// Expand one pattern component at a time, descending into directories.
//
// The previous version split on the FIRST '/' and handed everything after it
// to FindFirstFileW as a single file pattern.  Windows does not accept a
// wildcard in a non-final path component, so `logs/session_*/dll.log` matched
// NOTHING -- and a glob that finds nothing is indistinguishable from a role
// that produced nothing.  The cost was not an error: LogMirror mirrored the
// caster's stdout and the replay files, declared `grew=True`, and EvidenceGuard
// was satisfied, while `dll_text()` returned "" forever.  Every
// `marker in dll_text()` assertion then passed vacuously, and the run died at
// the character-select barrier because `"CHARSELECT reached" in dll_text()`
// could never become true -- on both games that had, in fact, reached it.
//
// Three of the mirror's seven patterns have that shape (dll.log, client.log
// and the *.dmp crash-dump probe), so this is component-wise and recursive:
// any number of wildcard components works, and no caller has to know which
// shapes are supported.
void globExpand(const std::string& base,
                const std::vector<std::string>& components,
                size_t index,
                std::vector<std::string>& out) {
    if (index >= components.size()) return;
    const bool last = index + 1 == components.size();
    WIN32_FIND_DATAW data{};
    HANDLE find =
        FindFirstFileW(widen(joinPath(base, components[index])).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = narrow(data.cFileName);
        if (name == "." || name == "..") continue;
        const std::string full = joinPath(base, name);
        if (last) {
            out.push_back(full);
        } else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            globExpand(full, components, index + 1, out);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

JsonPtr Agent::verbFsGlob(const Json& request) {
    const std::string root = jsonString(request.find("root"));
    const std::string pattern = jsonString(request.find("pattern"));
    std::vector<std::string> components;
    std::string current;
    for (char c : pattern) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) components.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) components.push_back(current);
    std::vector<std::string> matches;
    globExpand(root, components, 0, matches);
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    auto paths = Json::makeArray();
    for (const auto& match : matches) {
        paths->items.push_back(Json::makeString(match));
    }
    auto result = Json::makeObject();
    result->set("paths", paths);
    return result;
}

std::vector<unsigned char> readWholeFile(const std::string& path) {
    HANDLE file = CreateFileW(widen(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw AgentError("could not open " + path + ": " +
                         lastErrorText("CreateFileW"));
    }
    std::vector<unsigned char> out;
    std::vector<unsigned char> block(1 << 20);
    DWORD got = 0;
    while (ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &got,
                    nullptr) &&
           got > 0) {
        out.insert(out.end(), block.begin(), block.begin() + got);
    }
    CloseHandle(file);
    return out;
}

JsonPtr Agent::verbFsRead(const Json& request) {
    auto result = Json::makeObject();
    result->set("data", base64Encode(readWholeFile(jsonString(request.find("path")))));
    return result;
}

JsonPtr Agent::verbFsWrite(const Json& request) {
    const std::string path = jsonString(request.find("path"));
    const std::vector<unsigned char> data =
        base64Decode(jsonString(request.find("data")));
    HANDLE file = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw AgentError("could not create " + path + ": " +
                         lastErrorText("CreateFileW"));
    }
    DWORD wrote = 0;
    const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()),
                              &wrote, nullptr);
    CloseHandle(file);
    if (!ok || wrote != data.size()) {
        throw AgentError("short write to " + path);
    }
    return Json::makeObject();
}

JsonPtr Agent::verbFsTail(const Json& request) {
    // The evidence-mirror primitive.  Returns the current size as well as the
    // new bytes so the controller can detect TRUNCATION: session-log rotation
    // and the caster's exit-time archival both shrink a file, and a mirror
    // that appended blindly would splice two unrelated log bodies together.
    const std::string path = jsonString(request.find("path"));
    const long long offset = jsonInt(request.find("offset"));
    auto result = Json::makeObject();
    HANDLE file = CreateFileW(widen(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // "exists" is NOT cosmetic.  A missing file and a rotated-to-empty
        // file both report size 0, and the controller's truncation rule
        // ("size < offset means restart from zero") then overwrites its local
        // mirror with nothing.  For a file the caster has just archived and
        // DELETED, that local mirror is the only surviving copy of the remote
        // role's dll.log -- so the ambiguity silently destroyed the evidence
        // the whole distributed run exists to collect.
        result->set("offset", static_cast<double>(offset));
        result->set("size", 0.0);
        result->set("exists", false);
        result->set("data", std::string());
        return result;
    }
    result->set("exists", true);
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    if (size.QuadPart <= offset) {
        CloseHandle(file);
        result->set("offset", static_cast<double>(offset));
        result->set("size", static_cast<double>(size.QuadPart));
        result->set("data", std::string());
        return result;
    }
    LARGE_INTEGER seek{};
    seek.QuadPart = offset;
    SetFilePointerEx(file, seek, nullptr, FILE_BEGIN);
    const long long remaining = size.QuadPart - offset;
    // Bounded so one enormous log cannot make a single JSON line unbounded;
    // the controller simply tails again on the next refresh.
    const long long chunk = (std::min<long long>)(remaining, 4 * 1024 * 1024);
    std::vector<unsigned char> data(static_cast<size_t>(chunk));
    DWORD got = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(chunk), &got, nullptr);
    CloseHandle(file);
    data.resize(got);
    result->set("offset", static_cast<double>(offset + got));
    result->set("size", static_cast<double>(size.QuadPart));
    result->set("data", base64Encode(data));
    return result;
}

JsonPtr Agent::verbFsMkdir(const Json& request) {
    const std::string path = jsonString(request.find("path"));
    const bool parents = jsonBool(request.find("parents"));
    const bool existOk = jsonBool(request.find("exist_ok"));
    if (pathExists(path)) {
        if (!existOk) {
            throw AgentError("refusing to reuse existing directory " + path);
        }
        return Json::makeObject();
    }
    if (parents) {
        makeDirectories(path);
    } else if (!CreateDirectoryW(widen(path).c_str(), nullptr)) {
        throw AgentError("could not create " + path + ": " +
                         lastErrorText("CreateDirectoryW"));
    }
    if (!pathIsDirectory(path)) {
        throw AgentError("directory was not created: " + path);
    }
    return Json::makeObject();
}

JsonPtr Agent::verbFsCopy(const Json& request) {
    const std::string source = jsonString(request.find("source"));
    const std::string destination = jsonString(request.find("destination"));
    const size_t slash = destination.find_last_of("\\/");
    if (slash != std::string::npos) {
        makeDirectories(destination.substr(0, slash));
    }
    // CopyFileW preserves timestamps, which is what shutil.copy2 does and what
    // the changed-file manifest's mtime comparison depends on.
    if (!CopyFileW(widen(source).c_str(), widen(destination).c_str(), FALSE)) {
        throw AgentError("could not copy " + source + " to " + destination + ": " +
                         lastErrorText("CopyFileW"));
    }
    return Json::makeObject();
}

JsonPtr Agent::verbFsSha256(const Json& request) {
    const std::string path = jsonString(request.find("path"));
    HANDLE file = CreateFileW(widen(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw AgentError("could not open " + path + " for hashing: " +
                         lastErrorText("CreateFileW"));
    }
    Sha256 digest;
    std::vector<unsigned char> block(1 << 20);
    DWORD got = 0;
    while (ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &got,
                    nullptr) &&
           got > 0) {
        digest.update(block.data(), got);
    }
    CloseHandle(file);
    auto result = Json::makeObject();
    result->set("sha256", digest.finish());
    return result;
}

JsonPtr Agent::verbFsDiskUsage(const Json& request) {
    const std::string path = jsonString(request.find("path"));
    ULARGE_INTEGER available{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(widen(path).c_str(), &available, &total, &free)) {
        throw AgentError(lastErrorText("GetDiskFreeSpaceExW") + " for " + path);
    }
    auto result = Json::makeObject();
    result->set("total", static_cast<double>(total.QuadPart));
    result->set("used", static_cast<double>(total.QuadPart - free.QuadPart));
    result->set("free", static_cast<double>(available.QuadPart));
    return result;
}

JsonPtr Agent::verbCloneInstall(const Json& request) {
    const std::string source = jsonString(request.find("source"));
    const std::string destination = jsonString(request.find("destination"));
    if (pathExists(destination)) {
        throw AgentError("refusing to overwrite test install " + destination);
    }
    makeDirectories(destination);
    const std::vector<std::string> skipDirs = {"logs", "replays", "__pycache__"};
    std::vector<std::string> files;
    enumerateFiles(source, std::string(), skipDirs, &files);
    long long copied = 0;
    long long skipped = 0;
    for (const auto& relative : files) {
        const size_t slash = relative.find_last_of("\\/");
        const std::string name =
            slash == std::string::npos ? relative : relative.substr(slash + 1);
        if (cloneSkipsName(name)) {
            ++skipped;
            continue;
        }
        const std::string target = joinPath(destination, relative);
        const size_t targetSlash = target.find_last_of("\\/");
        if (targetSlash != std::string::npos) {
            makeDirectories(target.substr(0, targetSlash));
        }
        if (!CopyFileW(widen(joinPath(source, relative)).c_str(),
                       widen(target).c_str(), FALSE)) {
            throw AgentError("clone failed at " + relative + ": " +
                             lastErrorText("CopyFileW"));
        }
        ++copied;
    }
    CreateDirectoryW(widen(joinPath(destination, "logs")).c_str(), nullptr);
    auto result = Json::makeObject();
    result->set("copied", static_cast<double>(copied));
    result->set("skipped", static_cast<double>(skipped));
    return result;
}

JsonPtr Agent::verbClonePayload(const Json& request) {
    const std::string source = jsonString(request.find("source"));
    const std::vector<std::string> skipDirs = {"logs", "replays", "__pycache__"};
    std::vector<std::string> files;
    enumerateFiles(source, std::string(), skipDirs, &files);
    long long bytes = 0;
    long long count = 0;
    for (const auto& relative : files) {
        const size_t slash = relative.find_last_of("\\/");
        const std::string name =
            slash == std::string::npos ? relative : relative.substr(slash + 1);
        if (cloneSkipsName(name)) continue;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(widen(joinPath(source, relative)).c_str(),
                                 GetFileExInfoStandard, &data)) {
            bytes += (static_cast<long long>(data.nFileSizeHigh) << 32) |
                     data.nFileSizeLow;
            ++count;
        }
    }
    auto result = Json::makeObject();
    result->set("bytes", static_cast<double>(bytes));
    result->set("files", static_cast<double>(count));
    return result;
}

JsonPtr Agent::verbValidateAssetPaths(const Json& request) {
    const std::string root = jsonString(request.find("root"));
    std::vector<std::string> files;
    enumerateFiles(root, std::string(), {}, &files);
    long long checked = 0;
    long long longest = -1;
    std::string longestPath;
    auto tooLong = Json::makeArray();
    for (const auto& relative : files) {
        const std::string full = joinPath(root, relative);
        ++checked;
        const long long length = static_cast<long long>(full.size());
        if (length > longest) {
            longest = length;
            longestPath = full;
        }
        if (length > kLegacyAssetPathMax) {
            auto item = Json::makeObject();
            item->set("length", static_cast<double>(length));
            item->set("path", full);
            tooLong->items.push_back(item);
        }
    }
    auto result = Json::makeObject();
    result->set("checked", static_cast<double>(checked));
    result->set("limit", static_cast<double>(kLegacyAssetPathMax));
    result->set("longest_length", static_cast<double>(longest));
    result->set("longest_path", longestPath);
    result->set("too_long", tooLong);
    return result;
}

// -- verbs: QOH configuration ----------------------------------------------
// The QOHcnf.key layout constants live beside the build identity, at the top
// of this file, because the handshake publishes them.

uint32_t readU32(const std::vector<unsigned char>& image, size_t at) {
    return static_cast<uint32_t>(image[at]) |
           (static_cast<uint32_t>(image[at + 1]) << 8) |
           (static_cast<uint32_t>(image[at + 2]) << 16) |
           (static_cast<uint32_t>(image[at + 3]) << 24);
}

void writeU32(std::vector<unsigned char>& image, size_t at, uint32_t value) {
    image[at] = static_cast<unsigned char>(value & 0xFF);
    image[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    image[at + 2] = static_cast<unsigned char>((value >> 16) & 0xFF);
    image[at + 3] = static_cast<unsigned char>((value >> 24) & 0xFF);
}

void writeWholeFile(const std::string& path,
                    const std::vector<unsigned char>& data) {
    HANDLE file = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw AgentError("could not write " + path + ": " +
                         lastErrorText("CreateFileW"));
    }
    DWORD wrote = 0;
    WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr);
    CloseHandle(file);
    if (wrote != data.size()) throw AgentError("short write to " + path);
}

JsonPtr configSnapshotFor(const std::string& path,
                          const std::vector<long long>& offsets) {
    std::vector<unsigned char> image = readWholeFile(path);
    if (image.size() != kConfigImageSize) {
        throw AgentError(path + " has size " + std::to_string(image.size()) +
                         ", expected " + std::to_string(kConfigImageSize));
    }
    const uint32_t profile = readU32(image, 0);
    if (profile >= kConfigProfileCount) {
        throw AgentError(path + " selects invalid QOH config profile " +
                         std::to_string(profile));
    }
    const size_t record = kConfigProfileBase + profile * kConfigProfileStride;
    auto values = Json::makeObject();
    for (long long offset : offsets) {
        values->set(std::to_string(offset),
                    static_cast<double>(
                        readU32(image, record + static_cast<size_t>(offset))));
    }
    Sha256 digest;
    digest.update(image.data(), image.size());

    WIN32_FILE_ATTRIBUTE_DATA data{};
    unsigned long long unixNs = 0;
    if (GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &data)) {
        unsigned long long ticks =
            (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime)
             << 32) |
            data.ftLastWriteTime.dwLowDateTime;
        const unsigned long long kEpochDelta = 116444736000000000ULL;
        unixNs = ticks > kEpochDelta ? (ticks - kEpochDelta) * 100ULL : 0ULL;
    }

    auto result = Json::makeObject();
    result->set("profile", static_cast<double>(profile));
    result->set("record_offset", static_cast<double>(record));
    result->set("values", values);
    result->set("file_sha256", digest.finish());
    result->set("mtime_ns", static_cast<double>(unixNs));
    return result;
}

JsonPtr Agent::verbConfigSnapshot(const Json& request) {
    std::vector<long long> offsets;
    const Json* node = request.find("offsets");
    if (node && node->kind == Json::Kind::Array) {
        for (const auto& item : node->items) {
            offsets.push_back(jsonInt(item.get()));
        }
    }
    return configSnapshotFor(jsonString(request.find("path")), offsets);
}

JsonPtr Agent::verbConfigMutate(const Json& request) {
    const std::string path = jsonString(request.find("path"));
    const long long profile = jsonInt(request.find("profile"));
    if (profile < 0 || profile >= static_cast<long long>(kConfigProfileCount)) {
        throw AgentError("invalid QOH config profile " + std::to_string(profile));
    }
    std::vector<unsigned char> image = readWholeFile(path);
    if (image.size() != kConfigImageSize) {
        throw AgentError(path + " has size " + std::to_string(image.size()) +
                         ", expected " + std::to_string(kConfigImageSize));
    }
    std::vector<std::pair<long long, uint32_t>> writes;
    const Json* values = request.find("values");
    if (values && values->kind == Json::Kind::Object) {
        for (const auto& member : values->members) {
            const long long offset = std::strtoll(member.first.c_str(), nullptr, 10);
            if (offset < 0 || offset % 4 != 0 ||
                offset + 4 > static_cast<long long>(kConfigProfileStride)) {
                throw AgentError("invalid QOH profile DWORD offset " +
                                 std::to_string(offset));
            }
            writes.emplace_back(
                offset, static_cast<uint32_t>(jsonInt(member.second.get())));
        }
    }
    writeU32(image, 0, static_cast<uint32_t>(profile));
    const size_t record =
        kConfigProfileBase + static_cast<size_t>(profile) * kConfigProfileStride;
    for (const auto& write : writes) {
        writeU32(image, record + static_cast<size_t>(write.first), write.second);
    }
    writeWholeFile(path, image);

    std::vector<long long> offsets;
    for (const auto& write : writes) offsets.push_back(write.first);
    std::sort(offsets.begin(), offsets.end());
    JsonPtr readback = configSnapshotFor(path, offsets);
    if (jsonInt(readback->find("profile")) != profile) {
        throw AgentError(path + " did not retain the requested config mutation");
    }
    return readback;
}

JsonPtr Agent::verbMuteAudio(const Json& request) {
    // QOH SE parses LocalConfig/FCommand.txt AFTER loading QOHcnf.key, and its
    // shipped `NoSound FALSE` would clear the bit again, so both have to move.
    // These are cloned artifacts, never a source install.
    const std::string install = jsonString(request.find("install"));
    const std::string key = joinPath(joinPath(install, "System"), "QOHcnf.key");
    std::vector<unsigned char> image = readWholeFile(key);
    if (image.size() != kConfigImageSize) {
        throw AgentError(key + " has size " + std::to_string(image.size()) +
                         ", expected " + std::to_string(kConfigImageSize));
    }
    const uint32_t profile = readU32(image, 0);
    if (profile >= kConfigProfileCount) {
        throw AgentError(key + " selects invalid QOH config profile " +
                         std::to_string(profile));
    }
    const size_t record = kConfigProfileBase + profile * kConfigProfileStride;
    const size_t flagAt = record + kConfigNoSoundOffset;
    const uint32_t before = readU32(image, flagAt);
    const uint32_t after = before | kConfigNoSoundBit;
    writeU32(image, flagAt, after);
    writeWholeFile(key, image);

    auto changed = Json::makeArray();
    const std::string commandPaths[] = {
        joinPath(joinPath(install, "LocalConfig"), "FCommand.txt"),
        joinPath(install, "FCommand.txt")};
    bool sawCanonical = false;
    for (const std::string& path : commandPaths) {
        if (!pathExists(path)) continue;
        std::vector<unsigned char> body = readWholeFile(path);
        std::string text(body.begin(), body.end());
        const std::string folded = foldCase(text);
        size_t replacements = 0;
        size_t at = 0;
        while ((at = folded.find("nosound", at)) != std::string::npos) {
            size_t cursor = at + 7;
            while (cursor < text.size() &&
                   (text[cursor] == ' ' || text[cursor] == '\t')) {
                ++cursor;
            }
            if (folded.compare(cursor, 5, "false") == 0) {
                text.replace(cursor, 5, "TRUE ");
                ++replacements;
            } else if (folded.compare(cursor, 4, "true") == 0) {
                sawCanonical = true;
            }
            at = cursor;
        }
        if (replacements > 1) {
            throw AgentError(path + " does not contain one canonical NoSound "
                                    "setting");
        }
        if (replacements == 1) {
            writeWholeFile(path, std::vector<unsigned char>(text.begin(), text.end()));
            changed->items.push_back(Json::makeString(path));
            sawCanonical = true;
        }
    }
    if (!sawCanonical && changed->items.empty()) {
        throw AgentError("test audio mute found no canonical NoSound setting "
                         "under " + install);
    }
    JsonPtr verify = configSnapshotFor(key, {static_cast<long long>(
                                                kConfigNoSoundOffset)});
    const Json* verifyValues = verify->find("values");
    const Json* flag =
        verifyValues ? verifyValues->find(std::to_string(kConfigNoSoundOffset))
                     : nullptr;
    if (!flag ||
        (static_cast<uint32_t>(jsonInt(flag)) & kConfigNoSoundBit) == 0) {
        throw AgentError(key + " did not retain the native NoSound bit");
    }
    auto result = Json::makeObject();
    result->set("scope", "temporary cloned test install only");
    result->set("profile", static_cast<double>(profile));
    result->set("flags_before", static_cast<double>(before));
    result->set("flags_after", static_cast<double>(after));
    result->set("no_sound_bit", static_cast<double>(kConfigNoSoundBit));
    result->set("fcommand_files", changed);
    result->set("verified", true);
    return result;
}

// -- verbs: subprocess ------------------------------------------------------

JsonPtr Agent::verbRun(const Json& request) {
    // Used for `qoh99caster replay inspect`, which must run where the binary
    // and the .q99r are co-located.  Wine's fixme: noise is returned verbatim:
    // if it breaks the shipped parser that is a finding about the environment,
    // not something to hide behind a grep.
    const Json* argvNode = request.find("argv");
    if (!argvNode || argvNode->kind != Json::Kind::Array || argvNode->items.empty()) {
        throw AgentError("run requires a non-empty argv array");
    }
    std::string commandLine;
    for (size_t index = 0; index < argvNode->items.size(); ++index) {
        if (index) commandLine.push_back(' ');
        const std::string argument = argvNode->items[index]->text;
        if (argument.find_first_of(" \t\"") == std::string::npos) {
            commandLine += argument;
        } else {
            commandLine.push_back('"');
            commandLine += argument;
            commandLine.push_back('"');
        }
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inheritable, 0)) {
        throw AgentError(lastErrorText("CreatePipe"));
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION created{};
    std::wstring wide = widen(commandLine);
    std::vector<wchar_t> mutableCommandLine(wide.begin(), wide.end());
    mutableCommandLine.push_back(L'\0');
    const std::string cwd = jsonString(request.find("cwd"));
    if (!CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : widen(cwd).c_str(), &startup,
                        &created)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        throw AgentError(lastErrorText("CreateProcessW") + " for " + commandLine);
    }
    CloseHandle(writePipe);
    CloseHandle(created.hThread);

    std::string output;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        output.append(buffer, got);
    }
    CloseHandle(readPipe);

    const double timeoutSeconds = jsonInt(request.find("run_timeout"), 20);
    const DWORD waited = WaitForSingleObject(
        created.hProcess, static_cast<DWORD>(timeoutSeconds * 1000.0));
    DWORD code = 0;
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(created.hProcess, 1);
        code = 1;
    } else {
        GetExitCodeProcess(created.hProcess, &code);
    }
    CloseHandle(created.hProcess);

    auto result = Json::makeObject();
    result->set("returncode", static_cast<double>(code));
    result->set("output", output);
    return result;
}

JsonPtr Agent::verbReservePort(const Json& request) {
    const std::string kind = jsonString(request.find("kind"), "game");
    const std::string bind = jsonString(request.find("bind"), "127.0.0.1");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, bind.c_str(), &address.sin_addr);

    if (kind == "relay") {
        for (int attempt = 0; attempt < 100; ++attempt) {
            SOCKET tcp = socket(AF_INET, SOCK_STREAM, 0);
            address.sin_port = 0;
            if (bind_(tcp, address) != 0) {
                closesocket(tcp);
                continue;
            }
            sockaddr_in bound{};
            int boundSize = sizeof(bound);
            getsockname(tcp, reinterpret_cast<sockaddr*>(&bound), &boundSize);
            SOCKET udp = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in same = address;
            same.sin_port = bound.sin_port;
            if (bind_(udp, same) != 0) {
                closesocket(tcp);
                closesocket(udp);
                continue;
            }
            const unsigned short port = ntohs(bound.sin_port);
            closesocket(tcp);
            closesocket(udp);
            auto result = Json::makeObject();
            result->set("port", static_cast<double>(port));
            return result;
        }
        throw AgentError("could not reserve a TCP+UDP relay port");
    }
    for (unsigned short port = 46000; port < 46998; ++port) {
        SOCKET udp = socket(AF_INET, SOCK_DGRAM, 0);
        address.sin_port = htons(port);
        if (bind_(udp, address) != 0) {
            closesocket(udp);
            continue;
        }
        closesocket(udp);
        auto result = Json::makeObject();
        result->set("port", static_cast<double>(port));
        return result;
    }
    throw AgentError("could not reserve a UDP game/control port");
}

JsonPtr Agent::verbReserveExclusiveUdp(const Json& request) {
    const std::string bind = jsonString(request.find("bind"), "0.0.0.0");
    SOCKET held = socket(AF_INET, SOCK_DGRAM, 0);
    BOOL exclusive = TRUE;
    setsockopt(held, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    inet_pton(AF_INET, bind.c_str(), &address.sin_addr);
    if (bind_(held, address) != 0) {
        closesocket(held);
        throw AgentError("could not bind the exclusive unavailable-service "
                         "fixture socket");
    }
    sockaddr_in bound{};
    int boundSize = sizeof(bound);
    getsockname(held, reinterpret_cast<sockaddr*>(&bound), &boundSize);
    const unsigned short port = ntohs(bound.sin_port);
    if (port == 0) {
        closesocket(held);
        throw AgentError("exclusive unavailable-service fixture received port "
                         "zero");
    }
    heldSockets_.push_back(held);
    auto result = Json::makeObject();
    result->set("port", static_cast<double>(port));
    return result;
}

JsonPtr Agent::dispatch(const Json& request) {
    const std::string verb = jsonString(request.find("verb"));
    touchLease();

    if (verb == "handshake") return verbHandshake(request);
    if (verb == "heartbeat") return Json::makeObject();
    if (verb == "shutdown") {
        shutdownRequested_ = true;
        return Json::makeObject();
    }
    if (verb == "spawn") return verbSpawn(request);
    if (verb == "proc_poll") return verbProcPoll(request);
    if (verb == "proc_stop") return verbProcStop(request);
    if (verb == "child_game_pid") return verbChildGamePid(request);
    if (verb == "open_process") return verbOpenProcess(request);
    if (verb == "close_process") return verbCloseProcess(request);
    if (verb == "proc_alive") return verbProcAlive(request);
    if (verb == "proc_exit_code") return verbProcExitCode(request);
    if (verb == "proc_suspend") return verbProcSuspend(request, true);
    if (verb == "proc_resume") return verbProcSuspend(request, false);
    if (verb == "proc_terminate") return verbProcTerminate(request);
    if (verb == "proc_set_affinity") return verbProcSetAffinity(request);
    if (verb == "mem_read") return verbMemRead(request);
    if (verb == "mem_write") return verbMemWrite(request);
    if (verb == "mem_read_many") return verbMemReadMany(request);
    if (verb == "sample_game_state") return verbSampleGameState(request);
    if (verb == "module_base") return verbModuleBase(request);
    if (verb == "module_map") return verbModuleMap(request);
    if (verb == "thread_suspend") return verbThreadSuspend(request);
    if (verb == "thread_resume") return verbThreadResume(request);
    if (verb == "windows") return verbWindows(request);
    if (verb == "window_info") return verbWindowInfo(request);
    if (verb == "post_close") return verbPostClose(request);
    if (verb == "fs_stat") return verbFsStat(request);
    if (verb == "fs_glob") return verbFsGlob(request);
    if (verb == "fs_read") return verbFsRead(request);
    if (verb == "fs_write") return verbFsWrite(request);
    if (verb == "fs_tail") return verbFsTail(request);
    if (verb == "fs_mkdir") return verbFsMkdir(request);
    if (verb == "fs_copy") return verbFsCopy(request);
    if (verb == "fs_sha256") return verbFsSha256(request);
    if (verb == "fs_disk_usage") return verbFsDiskUsage(request);
    if (verb == "clone_install") return verbCloneInstall(request);
    if (verb == "clone_payload") return verbClonePayload(request);
    if (verb == "validate_asset_paths") return verbValidateAssetPaths(request);
    if (verb == "mute_audio") return verbMuteAudio(request);
    if (verb == "config_snapshot") return verbConfigSnapshot(request);
    if (verb == "config_mutate") return verbConfigMutate(request);
    if (verb == "run") return verbRun(request);
    if (verb == "reserve_port") return verbReservePort(request);
    if (verb == "reserve_exclusive_udp") return verbReserveExclusiveUdp(request);
    throw AgentError("unknown verb " + verb);
}

std::string Agent::handleLine(const std::string& line) {
    JsonParser parser(line);
    JsonPtr request = parser.parse();
    auto reply = Json::makeObject();
    long long id = 0;
    if (parser.ok() && request && request->kind == Json::Kind::Object) {
        id = jsonInt(request->find("id"));
    }
    reply->set("id", static_cast<double>(id));
    if (!parser.ok() || !request || request->kind != Json::Kind::Object) {
        reply->set("error", std::string("unparseable request line"));
    } else {
        try {
            reply->set("result", dispatch(*request));
        } catch (const AgentError& error) {
            reply->set("error", std::string(error.what()));
        } catch (const std::exception& error) {
            reply->set("error",
                       std::string("unexpected agent failure: ") + error.what());
        }
    }
    std::string encoded;
    serialize(*reply, encoded);
    encoded.push_back('\n');
    return encoded;
}

int Agent::runStdio() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    std::fprintf(stderr, "[agent] stdio transport lease=%lldms build=%s\n",
                 leaseMs_, kBuildStamp);
    std::fflush(stderr);

    std::thread watchdog([this]() {
        while (!shutdownRequested_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (shutdownRequested_) return;
            if (leaseExpired()) {
                std::fprintf(stderr,
                             "[agent] lease expired after %lldms with no "
                             "heartbeat; killing spawned children and exiting\n",
                             leaseMs_);
                std::fflush(stderr);
                killAllChildren();
                releaseSingleInstance();
                std::exit(4);
            }
        }
    });
    watchdog.detach();

    std::string pending;
    char buffer[8192];
    const int stdinFd = _fileno(stdin);
    while (!shutdownRequested_) {
        const int got = _read(stdinFd, buffer, sizeof(buffer));
        if (got <= 0) break;
        pending.append(buffer, static_cast<size_t>(got));
        size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const std::string encoded = handleLine(line);
            std::fwrite(encoded.data(), 1, encoded.size(), stdout);
            std::fflush(stdout);
        }
    }

    killAllChildren();
    releaseSingleInstance();
    std::fprintf(stderr, "[agent] clean shutdown (stdio)\n");
    return 0;
}

int Agent::run() {
    if (!acquireSingleInstance()) {
        return 2;
    }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[agent] WSAStartup failed\n");
        releaseSingleInstance();
        return 3;
    }
    if (useStdio_) {
        const int code = runStdio();
        WSACleanup();
        return code;
    }
    return runTcp();
}

int Agent::runTcp() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        std::fprintf(stderr, "[agent] could not create the listening socket\n");
        releaseSingleInstance();
        return 3;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
            0 ||
        listen(listener, 1) != 0) {
        std::fprintf(stderr, "[agent] could not bind 127.0.0.1:%u\n",
                     static_cast<unsigned>(port_));
        closesocket(listener);
        releaseSingleInstance();
        return 3;
    }
    std::fprintf(stderr, "[agent] listening on 127.0.0.1:%u lease=%lldms build=%s\n",
                 static_cast<unsigned>(port_), leaseMs_, kBuildStamp);
    std::fflush(stderr);

    std::thread watchdog([this, listener]() {
        while (!shutdownRequested_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (shutdownRequested_) return;
            if (leaseExpired()) {
                std::fprintf(stderr,
                             "[agent] lease expired after %lldms with no "
                             "heartbeat; killing spawned children and exiting\n",
                             leaseMs_);
                std::fflush(stderr);
                killAllChildren();
                closesocket(listener);
                releaseSingleInstance();
                std::exit(4);
            }
        }
    });
    watchdog.detach();

    SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
        closesocket(listener);
        releaseSingleInstance();
        return 3;
    }

    std::string pending;
    char buffer[8192];
    while (!shutdownRequested_) {
        const int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        pending.append(buffer, static_cast<size_t>(got));
        size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (line.empty()) continue;

            const std::string encoded = handleLine(line);
            size_t sent = 0;
            while (sent < encoded.size()) {
                const int wrote =
                    send(client, encoded.data() + sent,
                         static_cast<int>(encoded.size() - sent), 0);
                if (wrote <= 0) {
                    shutdownRequested_ = true;
                    break;
                }
                sent += static_cast<size_t>(wrote);
            }
        }
    }

    killAllChildren();
    closesocket(client);
    closesocket(listener);
    WSACleanup();
    releaseSingleInstance();
    std::fprintf(stderr, "[agent] clean shutdown\n");
    return 0;
}

}

int wmain(int argc, wchar_t** argv) {
    unsigned short port = 47311;
    long long leaseMs = 30000;
    std::string lockPath = "C:\\qoh99-e2e-agent.lock";
    bool useStdio = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring flag = argv[index];
        const bool hasValue = index + 1 < argc;
        if (flag == L"--stdio") {
            useStdio = true;
        } else if (flag == L"--port" && hasValue) {
            port = static_cast<unsigned short>(_wtoi(argv[++index]));
        } else if (flag == L"--lease-ms" && hasValue) {
            leaseMs = _wtoi64(argv[++index]);
        } else if (flag == L"--lock" && hasValue) {
            lockPath = narrow(argv[++index]);
        } else if (flag == L"--version") {
            std::printf("qoh99_e2e_agent protocol=%d build=%s\n",
                        kProtocolVersion, kBuildStamp);
            return 0;
        }
    }

    Agent agent(port, leaseMs, lockPath, useStdio);
    return agent.run();
}
