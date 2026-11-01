#include "input/InputProfile.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#include "input/ModeControlPolicy.h"

namespace qinput {
namespace {

constexpr size_t kMaxJsonDepth = 8;
constexpr size_t kMaxJsonString = 1024;
constexpr size_t kMaxJsonMembers = 128;
constexpr size_t kMaxJsonElements = 256;
constexpr size_t kMaxJsonText = 256 * 1024;

struct JsonValue;
using JsonValuePtr = std::unique_ptr<JsonValue>;

struct JsonMember {
    std::string key;
    JsonValuePtr value;
};

struct JsonValue {
    enum class Kind { Object, Array, String, Integer, Boolean } kind;
    std::vector<JsonMember> members;
    std::vector<JsonValuePtr> elements;
    std::string text;
    int64_t integer = 0;
    bool boolean = false;
};

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& out, uint32_t scalar) {
    if (scalar <= 0x7Fu) {
        out.push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (scalar >> 6)));
        out.push_back(static_cast<char>(0x80u | (scalar & 0x3Fu)));
    } else if (scalar <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (scalar >> 12)));
        out.push_back(static_cast<char>(0x80u | ((scalar >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (scalar & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (scalar >> 18)));
        out.push_back(static_cast<char>(0x80u | ((scalar >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((scalar >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (scalar & 0x3Fu)));
    }
}

struct Parser {
    const char* p;
    const char* end;
    std::string* error;

    bool fail(const char* message) {
        if (error->empty()) {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "%s at offset %zu",
                          message,
                          static_cast<size_t>(p - begin));
            *error = buffer;
        }
        return false;
    }

    const char* begin;

    void skipSpace() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'
                           || *p == '\n')) {
            ++p;
        }
    }

    bool parseString(std::string& out) {
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (out.size() >= kMaxJsonString)
                return fail("string too long");
            const char c = *p;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in string");
            if (c == '\\') {
                ++p;
                if (p >= end) return fail("truncated escape");
                const char escape = *p++;
                switch (escape) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'u': {
                        const auto readHex4 = [&]() -> int32_t {
                            if (end - p < 4) return -1;
                            uint32_t value = 0;
                            for (int i = 0; i < 4; ++i) {
                                const int nibble = hexNibble(*p++);
                                if (nibble < 0) return -1;
                                value = (value << 4)
                                      | static_cast<uint32_t>(nibble);
                            }
                            return static_cast<int32_t>(value);
                        };
                        const int32_t first = readHex4();
                        if (first < 0) return fail("invalid unicode escape");
                        uint32_t scalar = static_cast<uint32_t>(first);
                        if (scalar >= 0xD800u && scalar <= 0xDBFFu) {
                            if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                                return fail("missing low surrogate");
                            p += 2;
                            const int32_t second = readHex4();
                            if (second < 0 || second < 0xDC00
                                || second > 0xDFFF) {
                                return fail("invalid low surrogate");
                            }
                            scalar = 0x10000u
                                + ((scalar - 0xD800u) << 10)
                                + (static_cast<uint32_t>(second) - 0xDC00u);
                        } else if (scalar >= 0xDC00u && scalar <= 0xDFFFu) {
                            return fail("unexpected low surrogate");
                        }
                        appendUtf8(out, scalar);
                        break;
                    }
                    default:
                        return fail("unsupported escape");
                }
            } else {
                out.push_back(c);
                ++p;
            }
            if (out.size() > kMaxJsonString)
                return fail("string too long");
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }

    bool parseValue(JsonValuePtr& out, size_t depth) {
        if (depth > kMaxJsonDepth) return fail("nesting too deep");
        skipSpace();
        if (p >= end) return fail("unexpected end");
        out = std::make_unique<JsonValue>();
        if (*p == '{') {
            out->kind = JsonValue::Kind::Object;
            ++p;
            skipSpace();
            if (p < end && *p == '}') { ++p; return true; }
            while (true) {
                if (out->members.size() >= kMaxJsonMembers)
                    return fail("too many members");
                JsonMember member;
                skipSpace();
                if (!parseString(member.key)) return false;
                for (const JsonMember& existing : out->members)
                    if (existing.key == member.key)
                        return fail("duplicate key");
                skipSpace();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                if (!parseValue(member.value, depth + 1)) return false;
                out->members.push_back(std::move(member));
                skipSpace();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (*p == '[') {
            out->kind = JsonValue::Kind::Array;
            ++p;
            skipSpace();
            if (p < end && *p == ']') { ++p; return true; }
            while (true) {
                if (out->elements.size() >= kMaxJsonElements)
                    return fail("too many elements");
                JsonValuePtr element;
                if (!parseValue(element, depth + 1)) return false;
                out->elements.push_back(std::move(element));
                skipSpace();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (*p == '"') {
            out->kind = JsonValue::Kind::String;
            return parseString(out->text);
        }
        if (*p == 't' || *p == 'f') {
            out->kind = JsonValue::Kind::Boolean;
            if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) {
                out->boolean = true;
                p += 4;
                return true;
            }
            if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) {
                out->boolean = false;
                p += 5;
                return true;
            }
            return fail("invalid literal");
        }
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            out->kind = JsonValue::Kind::Integer;
            const bool negative = *p == '-';
            if (negative) ++p;
            if (p >= end || *p < '0' || *p > '9')
                return fail("invalid number");
            int64_t value = 0;
            int digits = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                if (++digits > 18) return fail("number too long");
                value = value * 10 + (*p - '0');
                ++p;
            }
            if (p < end && (*p == '.' || *p == 'e' || *p == 'E'))
                return fail("non-integer number");
            out->integer = negative ? -value : value;
            return true;
        }
        return fail("unexpected character");
    }
};

const JsonValue* findMember(const JsonValue& object, const char* key) {
    for (const JsonMember& member : object.members)
        if (member.key == key) return member.value.get();
    return nullptr;
}

bool onlyKnownKeys(const JsonValue& object,
                   std::initializer_list<const char*> known,
                   std::string& error) {
    for (const JsonMember& member : object.members) {
        bool found = false;
        for (const char* key : known)
            if (member.key == key) { found = true; break; }
        if (!found) {
            error = "unknown field: " + member.key;
            return false;
        }
    }
    return true;
}

bool readString(const JsonValue& object, const char* key, bool required,
                size_t maxLength, std::string& out, std::string& error) {
    const JsonValue* value = findMember(object, key);
    if (value == nullptr) {
        if (required) { error = std::string("missing field: ") + key; }
        return !required;
    }
    if (value->kind != JsonValue::Kind::String) {
        error = std::string("field is not a string: ") + key;
        return false;
    }
    if (value->text.size() > maxLength) {
        error = std::string("field too long: ") + key;
        return false;
    }
    out = value->text;
    return true;
}

bool readInt(const JsonValue& object, const char* key, bool required,
             int64_t minimum, int64_t maximum, int64_t& out,
             std::string& error) {
    const JsonValue* value = findMember(object, key);
    if (value == nullptr) {
        if (required) { error = std::string("missing field: ") + key; }
        return !required;
    }
    if (value->kind != JsonValue::Kind::Integer) {
        error = std::string("field is not an integer: ") + key;
        return false;
    }
    if (value->integer < minimum || value->integer > maximum) {
        error = std::string("field out of range: ") + key;
        return false;
    }
    out = value->integer;
    return true;
}

const char* const kActionNames[8] = {
    "Up", "Down", "Left", "Right", "A", "B", "C", "D"};
const char* const kLegacySessionControlNames[4] = {
    "Pause", "Back", "DelayDecrease", "DelayIncrease"};

const char* sourceModeName(CompiledSourceMode mode) {
    switch (mode) {
        case CompiledSourceMode::Keyboard: return "keyboard";
        case CompiledSourceMode::Controller: return "controller";
        case CompiledSourceMode::KeyboardAndController:
            return "keyboard+controller";
    }
    return "keyboard";
}

bool parseSourceMode(const std::string& text, CompiledSourceMode& out) {
    if (text == "keyboard") { out = CompiledSourceMode::Keyboard; return true; }
    if (text == "controller") {
        out = CompiledSourceMode::Controller;
        return true;
    }
    if (text == "keyboard+controller") {
        out = CompiledSourceMode::KeyboardAndController;
        return true;
    }
    return false;
}

const char* socdName(SocdPolicy policy) {
    switch (policy) {
        case SocdPolicy::Native: return "Native";
        case SocdPolicy::Neutral: return "Neutral";
        case SocdPolicy::LastInput: return "LastInput";
        case SocdPolicy::UpPriority: return "UpPriority";
    }
    return "Native";
}

bool parseSocd(const std::string& text, SocdPolicy& out) {
    if (text == "Native") { out = SocdPolicy::Native; return true; }
    if (text == "Neutral") { out = SocdPolicy::Neutral; return true; }
    if (text == "LastInput") { out = SocdPolicy::LastInput; return true; }
    if (text == "UpPriority") { out = SocdPolicy::UpPriority; return true; }
    return false;
}

const char* bindingTypeName(const ProfileBinding& binding) {
    switch (binding.type) {
        case CompiledBindingType::KeyboardScan: return "KeyboardScan";
        case CompiledBindingType::ProbeButton:
            return binding.probeKind == ProbeKind::GamepadButton
                ? "GamepadButton" : "RawJoystickButton";
        case CompiledBindingType::ProbeAxis:
            return binding.probeKind == ProbeKind::GamepadAxis
                ? "GamepadAxis" : "RawJoystickAxis";
        case CompiledBindingType::ProbeHat: return "RawJoystickHat";
        case CompiledBindingType::None: break;
    }
    return "?";
}

bool parseBinding(const JsonValue& object, bool digitalOnly,
                  ProfileBinding& out, std::string& error) {
    std::string type;
    if (!readString(object, "type", true, 32, type, error)) return false;
    int64_t value = 0;
    if (type == "KeyboardScan") {
        if (!onlyKnownKeys(object, {"type", "dik"}, error)) return false;
        if (!readInt(object, "dik", true, 1, 255, value, error))
            return false;
        out.type = CompiledBindingType::KeyboardScan;
        out.dik = static_cast<uint8_t>(value);
        return true;
    }
    if (type == "GamepadButton" || type == "RawJoystickButton") {
        if (!onlyKnownKeys(object, {"type", "button"}, error)) return false;
        if (!readInt(object, "button", true, 0, 254, value, error))
            return false;
        out.type = CompiledBindingType::ProbeButton;
        out.probeKind = type[0] == 'G' ? ProbeKind::GamepadButton
                                       : ProbeKind::RawButton;
        out.token = static_cast<uint8_t>(value);
        return true;
    }
    if (type == "RawJoystickHat") {
        if (!onlyKnownKeys(object, {"type", "hat", "direction"}, error))
            return false;
        if (!readInt(object, "hat", true, 0, 15, value, error)) return false;
        out.type = CompiledBindingType::ProbeHat;
        out.probeKind = ProbeKind::RawHat;
        out.token = static_cast<uint8_t>(value);
        std::string direction;
        if (!readString(object, "direction", true, 8, direction, error))
            return false;
        if (direction == "Up") out.hatBit = kHatUp;
        else if (direction == "Down") out.hatBit = kHatDown;
        else if (direction == "Left") out.hatBit = kHatLeft;
        else if (direction == "Right") out.hatBit = kHatRight;
        else { error = "invalid hat direction"; return false; }
        return true;
    }
    if (type == "GamepadAxis" || type == "RawJoystickAxis") {
        if (digitalOnly) {
            error = "axis binding on a digital mode control";
            return false;
        }
        if (!onlyKnownKeys(object,
                           {"type", "axis", "polarity", "pressQ15",
                            "releaseQ15"},
                           error)) {
            return false;
        }
        if (!readInt(object, "axis", true, 0, 63, value, error))
            return false;
        out.type = CompiledBindingType::ProbeAxis;
        out.probeKind = type[0] == 'G' ? ProbeKind::GamepadAxis
                                       : ProbeKind::RawAxis;
        out.token = static_cast<uint8_t>(value);
        std::string polarity;
        if (!readString(object, "polarity", true, 16, polarity, error))
            return false;
        if (polarity == "positive") out.polarity = AxisPolarity::Positive;
        else if (polarity == "negative")
            out.polarity = AxisPolarity::Negative;
        else { error = "invalid polarity"; return false; }
        int64_t press = 16384;
        int64_t release = 11469;
        if (!readInt(object, "pressQ15", false, 1, kAxisScale, press, error))
            return false;
        if (!readInt(object, "releaseQ15", false, 0, kAxisScale, release,
                     error)) {
            return false;
        }
        if (release >= press) {
            error = "releaseQ15 must be below pressQ15";
            return false;
        }
        out.pressQ15 = static_cast<uint16_t>(press);
        out.releaseQ15 = static_cast<uint16_t>(release);
        return true;
    }
    error = "unknown binding type: " + type;
    return false;
}

void appendJsonString(std::string& out, const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    out += "\\u00";
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0x0Fu]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

void serializeBinding(std::string& out, const ProfileBinding& binding) {
    char buffer[192];
    switch (binding.type) {
        case CompiledBindingType::KeyboardScan:
            std::snprintf(buffer, sizeof(buffer),
                          "{\"type\":\"KeyboardScan\",\"dik\":%u}",
                          binding.dik);
            break;
        case CompiledBindingType::ProbeButton:
            std::snprintf(buffer, sizeof(buffer),
                          "{\"type\":\"%s\",\"button\":%u}",
                          bindingTypeName(binding), binding.token);
            break;
        case CompiledBindingType::ProbeHat: {
            const char* direction =
                binding.hatBit == kHatUp ? "Up"
                : binding.hatBit == kHatDown ? "Down"
                : binding.hatBit == kHatLeft ? "Left" : "Right";
            std::snprintf(
                buffer, sizeof(buffer),
                "{\"type\":\"RawJoystickHat\",\"hat\":%u,"
                "\"direction\":\"%s\"}",
                binding.token, direction);
            break;
        }
        case CompiledBindingType::ProbeAxis:
            std::snprintf(
                buffer, sizeof(buffer),
                "{\"type\":\"%s\",\"axis\":%u,\"polarity\":\"%s\","
                "\"pressQ15\":%u,\"releaseQ15\":%u}",
                bindingTypeName(binding), binding.token,
                binding.polarity == AxisPolarity::Positive ? "positive"
                                                           : "negative",
                binding.pressQ15, binding.releaseQ15);
            break;
        case CompiledBindingType::None:
            buffer[0] = '\0';
            break;
    }
    out += buffer;
}

using LegacySessionBindings =
    std::array<std::vector<ProfileBinding>, 4>;

bool parseProfile(const JsonValue& object, InputProfile& out,
                  bool legacySchema, LegacySessionBindings* legacyControls,
                  std::string& error) {
    // Every key the writer can emit must be listed here, or the strict parser rejects the whole file.
    const bool known = legacySchema
        ? onlyKnownKeys(object,
                        {"id", "name", "source", "device", "actions",
                         "sessionControls", "axisCalibration", "socd",
                         "focus", "mapping", "probeBackend"}, error)
        : onlyKnownKeys(object,
                        {"id", "name", "source", "device", "actions",
                         "axisCalibration", "socd", "focus", "mapping",
                         "probeBackend"},
                        error);
    if (!known) {
        return false;
    }
    if (!readString(object, "id", true, kMaxProfileIdLength, out.id, error))
        return false;
    if (!readString(object, "name", true, kMaxProfileNameLength, out.name,
                    error)) {
        return false;
    }
    std::string source;
    if (!readString(object, "source", true, 32, source, error)) return false;
    if (!parseSourceMode(source, out.sourceMode)) {
        error = "invalid source mode";
        return false;
    }
    if (const JsonValue* device = findMember(object, "device")) {
        if (device->kind != JsonValue::Kind::Object) {
            error = "device is not an object";
            return false;
        }
        if (!onlyKnownKeys(*device,
                           {"guid", "vendor", "product", "version",
                            "serial", "serialHash", "pathFingerprint",
                            "name"},
                           error)) {
            return false;
        }
        if (!readString(*device, "guid", true, 63, out.device.guid, error))
            return false;
        int64_t value = 0;
        if (!readInt(*device, "vendor", true, 0, 0xFFFF, value, error))
            return false;
        out.device.vendor = static_cast<uint16_t>(value);
        if (!readInt(*device, "product", true, 0, 0xFFFF, value, error))
            return false;
        out.device.product = static_cast<uint16_t>(value);
        if (!readInt(*device, "version", true, 0, 0xFFFF, value, error))
            return false;
        out.device.version = static_cast<uint16_t>(value);
        if (!readString(*device, "serial", false, 128, out.device.serial,
                        error)) {
            return false;
        }
        if (const JsonValue* serialHashValue =
                findMember(*device, "serialHash")) {
            if (serialHashValue->kind != JsonValue::Kind::String) {
                error = "serialHash is not a hex string";
                return false;
            }
            const std::string& hex = serialHashValue->text;
            if (hex.empty() || hex.size() > 16) {
                error = "serialHash is not 1..16 hex digits";
                return false;
            }
            uint64_t parsedSerial = 0;
            for (char c : hex) {
                uint32_t digit = 0;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
                else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
                else {
                    error = "serialHash has a non-hex digit";
                    return false;
                }
                parsedSerial = (parsedSerial << 4) | digit;
            }
            out.device.serialHash = parsedSerial;
        }
        if (const JsonValue* fingerprintValue =
                findMember(*device, "pathFingerprint")) {
            if (fingerprintValue->kind == JsonValue::Kind::String) {
                const std::string& hex = fingerprintValue->text;
                if (hex.empty() || hex.size() > 16) {
                    error = "pathFingerprint is not 1..16 hex digits";
                    return false;
                }
                uint64_t parsedFingerprint = 0;
                for (char c : hex) {
                    uint32_t digit = 0;
                    if (c >= '0' && c <= '9') digit = c - '0';
                    else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
                    else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
                    else {
                        error = "pathFingerprint has a non-hex digit";
                        return false;
                    }
                    parsedFingerprint = (parsedFingerprint << 4) | digit;
                }
                out.device.pathFingerprint = parsedFingerprint;
            } else {
                int64_t fingerprint = 0;
                if (!readInt(*device, "pathFingerprint", false, 0,
                             INT64_MAX, fingerprint, error)) {
                    return false;
                }
                out.device.pathFingerprint =
                    static_cast<uint64_t>(fingerprint);
            }
        }
        if (!readString(*device, "name", false, 127,
                        out.device.normalizedName, error)) {
            return false;
        }
    }
    if (const JsonValue* actions = findMember(object, "actions")) {
        if (actions->kind != JsonValue::Kind::Object) {
            error = "actions is not an object";
            return false;
        }
        for (const JsonMember& member : actions->members) {
            int actionIndex = -1;
            for (int i = 0; i < 8; ++i)
                if (member.key == kActionNames[i]) actionIndex = i;
            if (actionIndex < 0) {
                error = "unknown action: " + member.key;
                return false;
            }
            if (member.value->kind != JsonValue::Kind::Array) {
                error = "action bindings are not an array";
                return false;
            }
            if (member.value->elements.size() > kMaxBindingsPerAction) {
                error = "too many bindings for " + member.key;
                return false;
            }
            for (const JsonValuePtr& element : member.value->elements) {
                if (element->kind != JsonValue::Kind::Object) {
                    error = "binding is not an object";
                    return false;
                }
                ProfileBinding binding{};
                if (!parseBinding(*element, false, binding, error))
                    return false;
                out.actions[actionIndex].push_back(binding);
            }
        }
    }
    if (const JsonValue* controls = findMember(object, "sessionControls")) {
        if (!legacySchema || legacyControls == nullptr) {
            error = "sessionControls is retired in schema 2";
            return false;
        }
        if (controls->kind != JsonValue::Kind::Object) {
            error = "sessionControls is not an object";
            return false;
        }
        for (const JsonMember& member : controls->members) {
            int kindIndex = -1;
            for (uint32_t i = 0; i < 4; ++i)
                if (member.key == kLegacySessionControlNames[i])
                    kindIndex = static_cast<int>(i);
            if (kindIndex < 0) {
                error = "unknown session control: " + member.key;
                return false;
            }
            if (member.value->kind != JsonValue::Kind::Array) {
                error = "session-control bindings are not an array";
                return false;
            }
            if (member.value->elements.size() > 4u) {
                error = "too many bindings for " + member.key;
                return false;
            }
            for (const JsonValuePtr& element : member.value->elements) {
                if (element->kind != JsonValue::Kind::Object) {
                    error = "binding is not an object";
                    return false;
                }
                ProfileBinding binding{};
                if (!parseBinding(*element, true, binding, error))
                    return false;
                (*legacyControls)[kindIndex].push_back(binding);
            }
        }
    }
    if (const JsonValue* calibration =
            findMember(object, "axisCalibration")) {
        if (calibration->kind != JsonValue::Kind::Array) {
            error = "axisCalibration is not an array";
            return false;
        }
        for (const JsonValuePtr& element : calibration->elements) {
            if (element->kind != JsonValue::Kind::Object) {
                error = "calibration entry is not an object";
                return false;
            }
            if (!onlyKnownKeys(*element,
                               {"kind", "axis", "minimum", "center",
                                "maximum", "invert", "unipolar",
                                "deadzoneQ15"},
                               error)) {
                return false;
            }
            ProfileAxisCalibration entry{};
            std::string kind;
            if (!readString(*element, "kind", true, 16, kind, error))
                return false;
            if (kind == "gamepad") entry.probeKind = ProbeKind::GamepadAxis;
            else if (kind == "raw") entry.probeKind = ProbeKind::RawAxis;
            else { error = "invalid calibration kind"; return false; }
            int64_t value = 0;
            if (!readInt(*element, "axis", true, 0, 63, value, error))
                return false;
            entry.axisToken = static_cast<uint8_t>(value);
            if (!readInt(*element, "minimum", true, -65536, 65535, value,
                         error)) {
                return false;
            }
            entry.values.minimum = static_cast<int32_t>(value);
            if (!readInt(*element, "center", false, -65536, 65535, value,
                         error)) {
                return false;
            }
            entry.values.center = static_cast<int32_t>(value);
            if (!readInt(*element, "maximum", true, -65536, 65535, value,
                         error)) {
                return false;
            }
            entry.values.maximum = static_cast<int32_t>(value);
            int64_t flag = 0;
            if (!readInt(*element, "invert", false, 0, 1, flag, error))
                return false;
            entry.values.invert = static_cast<uint8_t>(flag);
            flag = 0;
            if (!readInt(*element, "unipolar", false, 0, 1, flag, error))
                return false;
            entry.values.unipolar = static_cast<uint8_t>(flag);
            int64_t deadzone = 0;
            if (!readInt(*element, "deadzoneQ15", false, 0, kAxisScale,
                         deadzone, error)) {
                return false;
            }
            entry.values.deadzoneQ15 = static_cast<uint16_t>(deadzone);
            out.axisCalibration.push_back(entry);
        }
    }
    if (const JsonValue* socd = findMember(object, "socd")) {
        if (socd->kind != JsonValue::Kind::Object) {
            error = "socd is not an object";
            return false;
        }
        if (!onlyKnownKeys(*socd, {"horizontal", "vertical"}, error))
            return false;
        std::string policy;
        if (!readString(*socd, "horizontal", true, 16, policy, error))
            return false;
        if (!parseSocd(policy, out.horizontalSocd)) {
            error = "invalid horizontal SOCD";
            return false;
        }
        if (!readString(*socd, "vertical", true, 16, policy, error))
            return false;
        if (!parseSocd(policy, out.verticalSocd)) {
            error = "invalid vertical SOCD";
            return false;
        }
    }
    std::string focus;
    if (!readString(object, "focus", false, 32, focus, error)) return false;
    if (!focus.empty()) {
        if (focus == "GameWindowOnly")
            out.focusPolicy = FocusPolicy::GameWindowOnly;
        else if (focus == "AllowBackground")
            out.focusPolicy = FocusPolicy::AllowBackground;
        else { error = "invalid focus policy"; return false; }
    }
    if (!readString(object, "mapping", false, kMaxMappingStringLength - 1,
                    out.mapping, error)) {
        return false;
    }
    if (!readString(object, "probeBackend", false, 16, out.probeBackend,
                    error)) {
        return false;
    }
    if (!out.probeBackend.empty() && out.probeBackend != "sdl"
        && out.probeBackend != "dinput8") {
        error = "invalid probeBackend";
        return false;
    }
    return true;
}

bool equivalentBindings(const std::vector<ProfileBinding>& a,
                        const std::vector<ProfileBinding>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!samePhysicalBinding(a[i], b[i])) return false;
    return true;
}

bool parseModeControls(const JsonValue& object, ControlsFile& file,
                       std::string& error) {
    if (object.kind != JsonValue::Kind::Object) {
        error = "modeControls is not an object";
        return false;
    }
    for (const JsonMember& member : object.members) {
        const ModeControlDescriptor* descriptor = nullptr;
        for (const ModeControlDescriptor& candidate : kModeControlDescriptors)
            if (member.key == candidate.stableName) descriptor = &candidate;
        if (descriptor == nullptr) {
            error = "unknown mode control: " + member.key;
            return false;
        }
        if (member.value->kind != JsonValue::Kind::Array) {
            error = "mode-control bindings are not an array";
            return false;
        }
        if (member.value->elements.size() > 4u) {
            error = "too many aliases for " + member.key;
            return false;
        }
        auto& destination = file.modeControls[
            static_cast<size_t>(descriptor->action)];
        for (const JsonValuePtr& element : member.value->elements) {
            if (element->kind != JsonValue::Kind::Object) {
                error = "mode-control binding is not an object";
                return false;
            }
            ProfileBinding binding{};
            if (!parseBinding(*element, false, binding, error)
                || !validModeControlBinding(descriptor->action, binding,
                                            error)) return false;
            for (const ProfileBinding& existing : destination) {
                if (samePhysicalBinding(existing, binding)) {
                    error = "duplicate alias for " + member.key;
                    return false;
                }
            }
            destination.push_back(binding);
        }
    }
    return true;
}

void installMissingExtendedModeControls(
        const JsonValue& object, ControlsFile& file) {
    ControlsFile defaults{};
    installDefaultModeControls(defaults);
    constexpr ModeControlAction actions[] = {
        ModeControlAction::ReplayRestart,
        ModeControlAction::ReplayStepBack,
        ModeControlAction::ReplayTakeoverExit,
        ModeControlAction::ReplayTakeoverP1,
        ModeControlAction::ReplayTakeoverP2,
        ModeControlAction::ReplayTakeoverRetry,
        ModeControlAction::ReplayTakeoverDelayDecrease,
        ModeControlAction::ReplayTakeoverDelayIncrease,
        ModeControlAction::ReplayToggleCollisionDisplay,
        ModeControlAction::OnlineReturnCharacterSelect,
    };
    for (ModeControlAction action : actions) {
        const ModeControlDescriptor* descriptor = modeControlDescriptor(action);
        if (descriptor != nullptr
            && findMember(object, descriptor->stableName) == nullptr) {
            file.modeControls[static_cast<size_t>(action)] =
                defaults.modeControls[static_cast<size_t>(action)];
        }
    }
}

void migrateLegacyModeControls(
        ControlsFile& file,
        const std::vector<LegacySessionBindings>& legacy) {
    installDefaultModeControls(file);
    struct MigrationMap { uint32_t oldIndex; ModeControlAction action; };
    constexpr MigrationMap maps[] = {
        {1u, ModeControlAction::LeaveMatch},
        {2u, ModeControlAction::InputDelayDecrease},
        {3u, ModeControlAction::InputDelayIncrease},
    };
    for (const MigrationMap& map : maps) {
        std::vector<std::vector<ProfileBinding>> distinct;
        for (const LegacySessionBindings& profile : legacy) {
            const auto& candidate = profile[map.oldIndex];
            if (candidate.empty()) continue;
            bool seen = false;
            for (const auto& prior : distinct)
                seen |= equivalentBindings(prior, candidate);
            if (!seen) distinct.push_back(candidate);
        }
        if (distinct.empty()) continue;
        auto& destination =
            file.modeControls[static_cast<size_t>(map.action)];
        if (distinct.size() == 1u) {
            destination = distinct[0];
            continue;
        }
        destination.clear();
        ModeControlMigrationChoice choice{};
        choice.action = map.action;
        for (size_t p = 0; p < legacy.size() && p < 4u; ++p)
            choice.playerBindings[p] = legacy[p][map.oldIndex];
        file.pendingModeMigrations.push_back(std::move(choice));
    }
}

}

InputProfile makeDefaultKeyboardProfile() {
    InputProfile profile{};
    profile.id = "default-keyboard";
    profile.name = "QOH keyboard defaults";
    profile.sourceMode = CompiledSourceMode::Keyboard;
    const uint8_t defaults[8] = {
        0xC8,
        0xD0,
        0xCB,
        0xCD,
        0x2C,
        0x2D,
        0x2E,
        0x2F,
    };
    for (int action = 0; action < 8; ++action) {
        ProfileBinding binding{};
        binding.type = CompiledBindingType::KeyboardScan;
        binding.dik = defaults[action];
        profile.actions[action].push_back(binding);
    }
    return profile;
}

ControlsFile makeDefaultControlsFile() {
    ControlsFile file{};
    file.schemaVersion = kControlsSchemaVersion;
    InputProfile profile = makeDefaultKeyboardProfile();
    file.activeProfileId = profile.id;
    file.activeByPlayer[0] = profile.id;
    file.profiles.push_back(std::move(profile));
    installDefaultModeControls(file);
    return file;
}

bool parseControlsFile(const std::string& text, ControlsFile& out,
                       std::string& error) {
    error.clear();
    if (text.size() > kMaxJsonText) {
        error = "controls file too large";
        return false;
    }
    Parser parser{text.data(), text.data() + text.size(), &error,
                  text.data()};
    JsonValuePtr root;
    if (!parser.parseValue(root, 0)) return false;
    parser.skipSpace();
    if (parser.p != parser.end) {
        error = "trailing content";
        return false;
    }
    if (root->kind != JsonValue::Kind::Object) {
        error = "root is not an object";
        return false;
    }
    if (!onlyKnownKeys(*root,
                       {"schema", "activeProfile", "activeByPlayer",
                        "profiles", "modeControls"},
                       error)) {
        return false;
    }
    int64_t schema = 0;
    if (!readInt(*root, "schema", true, 1, INT64_MAX, schema, error))
        return false;
    if (schema != 1 && schema != kControlsSchemaVersion) {
        error = "unsupported schema version";
        return false;
    }
    ControlsFile file{};
    const bool legacySchema = schema == 1;
    file.schemaVersion = kControlsSchemaVersion;
    if (!readString(*root, "activeProfile", false, kMaxProfileIdLength,
                    file.activeProfileId, error)) {
        return false;
    }
    if (const JsonValue* actives = findMember(*root, "activeByPlayer")) {
        if (actives->kind != JsonValue::Kind::Array
            || actives->elements.size() > 4) {
            error = "activeByPlayer is not an array of up to 4 ids";
            return false;
        }
        for (size_t i = 0; i < actives->elements.size(); ++i) {
            if (actives->elements[i]->kind != JsonValue::Kind::String
                || actives->elements[i]->text.size() > 63) {
                error = "activeByPlayer entry is not a short string";
                return false;
            }
            file.activeByPlayer[i] = actives->elements[i]->text;
        }
    }
    if (file.activeByPlayer[0].empty())
        file.activeByPlayer[0] = file.activeProfileId;
    else if (file.activeProfileId.empty())
        file.activeProfileId = file.activeByPlayer[0];

    const JsonValue* profiles = findMember(*root, "profiles");
    if (profiles == nullptr || profiles->kind != JsonValue::Kind::Array) {
        error = "profiles missing or not an array";
        return false;
    }
    if (profiles->elements.size() > kMaxProfiles) {
        error = "too many profiles";
        return false;
    }
    std::vector<LegacySessionBindings> legacyControls;
    for (const JsonValuePtr& element : profiles->elements) {
        if (element->kind != JsonValue::Kind::Object) {
            error = "profile is not an object";
            return false;
        }
        InputProfile profile{};
        LegacySessionBindings legacy{};
        if (!parseProfile(*element, profile, legacySchema,
                          legacySchema ? &legacy : nullptr, error)) {
            return false;
        }
        for (const InputProfile& existing : file.profiles) {
            if (existing.id == profile.id) {
                error = "duplicate profile id: " + profile.id;
                return false;
            }
        }
        file.profiles.push_back(std::move(profile));
        legacyControls.push_back(std::move(legacy));
    }
    if (legacySchema) {
        migrateLegacyModeControls(file, legacyControls);
    } else if (const JsonValue* modeControls =
                   findMember(*root, "modeControls")) {
        if (!parseModeControls(*modeControls, file, error)) return false;
        installMissingExtendedModeControls(*modeControls, file);
    } else {
        installDefaultModeControls(file);
    }
    out = std::move(file);
    return true;
}

std::string serializeControlsFile(const ControlsFile& file) {
    std::string out;
    out.reserve(4096);
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "{\"schema\":%u",
                  file.schemaVersion);
    out += buffer;
    if (!file.activeProfileId.empty()) {
        out += ",\"activeProfile\":";
        appendJsonString(out, file.activeProfileId);
    }
    if (!file.activeByPlayer[1].empty() || !file.activeByPlayer[2].empty()
        || !file.activeByPlayer[3].empty()) {
        out += ",\"activeByPlayer\":[";
        for (int i = 0; i < 4; ++i) {
            if (i > 0) out += ',';
            appendJsonString(
                out, i == 0 && file.activeByPlayer[0].empty()
                    ? file.activeProfileId : file.activeByPlayer[i]);
        }
        out += "]";
    }

    out += ",\"profiles\":[";
    for (size_t p = 0; p < file.profiles.size(); ++p) {
        const InputProfile& profile = file.profiles[p];
        if (p != 0) out += ",";
        out += "{\"id\":";
        appendJsonString(out, profile.id);
        out += ",\"name\":";
        appendJsonString(out, profile.name);
        out += ",\"source\":";
        appendJsonString(out, sourceModeName(profile.sourceMode));
        if (!profile.device.guid.empty()) {
            out += ",\"device\":{\"guid\":";
            appendJsonString(out, profile.device.guid);
            std::snprintf(buffer, sizeof(buffer),
                          ",\"vendor\":%u,"
                          "\"product\":%u,\"version\":%u",
                          profile.device.vendor, profile.device.product,
                          profile.device.version);
            out += buffer;
            if (!profile.device.serial.empty()) {
                out += ",\"serial\":";
                appendJsonString(out, profile.device.serial);
            }
            if (profile.device.serialHash != 0) {
                std::snprintf(buffer, sizeof(buffer),
                              ",\"serialHash\":\"%016llX\"",
                              static_cast<unsigned long long>(
                                  profile.device.serialHash));
                out += buffer;
            }
            if (profile.device.pathFingerprint != 0) {
                std::snprintf(buffer, sizeof(buffer),
                              ",\"pathFingerprint\":\"%016llX\"",
                              static_cast<unsigned long long>(
                                  profile.device.pathFingerprint));
                out += buffer;
            }
            if (!profile.device.normalizedName.empty()) {
                out += ",\"name\":";
                appendJsonString(out, profile.device.normalizedName);
            }
            out += "}";
        }
        out += ",\"actions\":{";
        bool firstAction = true;
        for (int action = 0; action < 8; ++action) {
            if (profile.actions[action].empty()) continue;
            if (!firstAction) out += ",";
            firstAction = false;
            out += "\"";
            out += kActionNames[action];
            out += "\":[";
            for (size_t i = 0; i < profile.actions[action].size(); ++i) {
                if (i != 0) out += ",";
                serializeBinding(out, profile.actions[action][i]);
            }
            out += "]";
        }
        out += "}";
        if (!profile.axisCalibration.empty()) {
            out += ",\"axisCalibration\":[";
            for (size_t i = 0; i < profile.axisCalibration.size(); ++i) {
                const ProfileAxisCalibration& entry =
                    profile.axisCalibration[i];
                if (i != 0) out += ",";
                std::snprintf(
                    buffer, sizeof(buffer),
                    "{\"kind\":\"%s\",\"axis\":%u,\"minimum\":%d,"
                    "\"center\":%d,\"maximum\":%d,\"invert\":%u,"
                    "\"unipolar\":%u,\"deadzoneQ15\":%u}",
                    entry.probeKind == ProbeKind::GamepadAxis ? "gamepad"
                                                              : "raw",
                    entry.axisToken, entry.values.minimum,
                    entry.values.center, entry.values.maximum,
                    entry.values.invert, entry.values.unipolar,
                    entry.values.deadzoneQ15);
                out += buffer;
            }
            out += "]";
        }
        out += ",\"socd\":{\"horizontal\":\"";
        out += socdName(profile.horizontalSocd);
        out += "\",\"vertical\":\"";
        out += socdName(profile.verticalSocd);
        out += "\"},\"focus\":\"";
        out += profile.focusPolicy == FocusPolicy::GameWindowOnly
                   ? "GameWindowOnly" : "AllowBackground";
        out += "\"";
        if (!profile.mapping.empty()) {
            out += ",\"mapping\":";
            appendJsonString(out, profile.mapping);
        }
        if (!profile.probeBackend.empty()) {
            out += ",\"probeBackend\":";
            appendJsonString(out, profile.probeBackend);
        }
        out += "}";
    }
    out += "],\"modeControls\":{";
    bool firstMode = true;
    for (const ModeControlDescriptor& descriptor : kModeControlDescriptors) {
        if (!firstMode) out += ',';
        firstMode = false;
        out += '"';
        out += descriptor.stableName;
        out += "\":[";
        const auto& bindings = file.modeControls[
            static_cast<size_t>(descriptor.action)];
        for (size_t i = 0; i < bindings.size(); ++i) {
            if (i != 0) out += ',';
            serializeBinding(out, bindings[i]);
        }
        out += ']';
    }
    out += "}}";
    return out;
}

uint64_t profileContentHash(const InputProfile& profile) {
    ControlsFile file{};
    file.profiles.push_back(profile);
    const std::string serialized = serializeControlsFile(file);
    return hashInputConfigBytes(serialized.data(), serialized.size());
}

}
