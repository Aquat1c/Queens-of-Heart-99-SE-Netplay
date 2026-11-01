#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qclient {
namespace ui {

inline constexpr const char* kCasterVersion = "1.1.1";

void setDetectedGameEdition(const std::string& edition);
const std::string& detectedGameEdition();

enum class Tone : uint8_t {
    Default,
    Title,
    Dim,
    Accent,
    Good,
    Warn,
    Bad,
    Selected,
};

enum class Page : uint8_t {
    Main = 0,
    Host,
    Join,
    Spectate,
    Replays,
    Offline,
    Controls,
    Settings,
    Diagnostics,
    Count,
};

inline const char* pageId(Page page) {
    switch (page) {
        case Page::Main:        return "main";
        case Page::Host:        return "host";
        case Page::Join:        return "join";
        case Page::Spectate:    return "spectate";
        case Page::Replays:     return "replays";
        case Page::Offline:     return "offline";
        case Page::Controls:    return "controls";
        case Page::Settings:    return "settings";
        case Page::Diagnostics: return "diagnostics";
        case Page::Count:       break;
    }
    return "unknown";
}

inline const char* pageTitle(Page page) {
    switch (page) {
        case Page::Main:        return "Main menu";
        case Page::Host:        return "Host a game";
        case Page::Join:        return "Join a game";
        case Page::Spectate:    return "Spectate a game";
        case Page::Replays:     return "Replays";
        case Page::Offline:     return "Play offline";
        case Page::Controls:    return "Controls";
        case Page::Settings:    return "Settings";
        case Page::Diagnostics: return "Diagnostics";
        case Page::Count:       break;
    }
    return "QOH99 Netplay";
}

enum class ActionId : uint16_t {
    None = 0,

    GoHost,
    GoJoin,
    GoSpectate,
    GoReplays,
    GoOffline,
    GoControls,
    GoSettings,
    GoDiagnostics,
    Back,
    Quit,

    HostStart,
    HostCancel,
    HostCopyJoinAddress,
    HostCopySpectateCode,
    HostEditPort,

    JoinEditAddress,
    JoinPasteAddress,
    JoinStart,
    JoinCancel,

    SpectateEditAddress,
    SpectatePasteAddress,
    SpectateStart,
    SpectateWaitForMatch,
    SpectateJoinAsOpponent,
    SpectateCancel,

    ReplaySelect,
    ReplayWatch,
    ReplayVerify,
    ReplayFavorite,
    ReplayRename,
    ReplayExport,
    ReplayDelete,
    ReplaySearch,
    ReplayClearSearch,
    ReplayCycleFilter,
    ReplayCycleSort,
    ReplayRefresh,
    ReplayMore,

    OfflineTraining,
    OfflineVsPlayer,
    OfflineVsCpu,
    OfflineFreePlay,

    ControlsSelectPlayer,
    ControlsSelectSource,
    ControlsSelectDevice,
    ControlsRebind,
    ControlsClearBinding,
    ControlsSave,
    ControlsRevert,

    SettingsEditText,
    SettingsEditNumber,
    SettingsToggle,
    SettingsGameConfig,
    SettingsExtraFunctions,

    DiagRunTransportTests,
    DiagSavestateSelfTest,
    DiagObserveMode,
    DiagCopyReport,
    DiagOpenStateFolder,

    CopyFocused,
    PasteIntoFocused,
    Cancel,
    Refresh,
};

inline Page pageForAction(ActionId action) {
    switch (action) {
        case ActionId::GoHost:        return Page::Host;
        case ActionId::GoJoin:        return Page::Join;
        case ActionId::GoSpectate:    return Page::Spectate;
        case ActionId::GoReplays:     return Page::Replays;
        case ActionId::GoOffline:     return Page::Offline;
        case ActionId::GoControls:    return Page::Controls;
        case ActionId::GoSettings:    return Page::Settings;
        case ActionId::GoDiagnostics: return Page::Diagnostics;
        default:                      return Page::Count;
    }
}

enum class DisableReason : uint8_t {
    None = 0,
    Unavailable,
    Busy,
    NoSelection,
    GameNotConfigured,
    NetplayDllMissing,
    DeveloperModeRequired,
    SessionAlreadyRunning,
    NoClipboardText,
    InvalidAddress,
    NoControllerSelected,
    ReplayRecording,
    ReplayIncompatible,
    ReplayCorrupt,
    ReplayUnsupportedBuild,
    NotImplemented,
    Count,
};

inline const char* describe(DisableReason reason) {
    switch (reason) {
        case DisableReason::None:
        case DisableReason::Unavailable:
            return "not available right now";
        case DisableReason::Busy:
            return "busy: another operation is still running";
        case DisableReason::NoSelection:
            return "nothing is selected";
        case DisableReason::GameNotConfigured:
            return "the game executable is not set (Settings > Game path)";
        case DisableReason::NetplayDllMissing:
            return "qoh99_netplay.dll was not found next to the caster";
        case DisableReason::DeveloperModeRequired:
            return "developer mode only (start the caster with --dev)";
        case DisableReason::SessionAlreadyRunning:
            return "a game session is already running";
        case DisableReason::NoClipboardText:
            return "the clipboard holds no text";
        case DisableReason::InvalidAddress:
            return "enter a numeric IPv4 address, optionally with :port";
        case DisableReason::NoControllerSelected:
            return "this player has no controller selected";
        case DisableReason::ReplayRecording:
            return "this replay is still being recorded";
        case DisableReason::ReplayIncompatible:
            return "this replay is not compatible with the installed game";
        case DisableReason::ReplayCorrupt:
            return "this replay file is corrupt";
        case DisableReason::ReplayUnsupportedBuild:
            return "this replay needs a newer caster build";
        case DisableReason::NotImplemented:
            return "not implemented in this build";
        case DisableReason::Count:
            break;
    }
    return "not available right now";
}

enum class ItemKind : uint8_t {
    Action,
    Toggle,
    Value,
    Text,
    Header,
    Spacer,
};

struct Badge {
    std::string text;
    Tone tone = Tone::Default;
    bool empty() const { return text.empty(); }
};

struct Item {
    ItemKind kind = ItemKind::Action;
    ActionId action = ActionId::None;
    std::string label;
    std::string value;
    std::string hint;
    Badge badge;
    Tone labelTone = Tone::Default;
    Tone valueTone = Tone::Default;
    bool enabled = true;
    DisableReason reason = DisableReason::None;
    std::string reasonDetail;
    int index = -1;
    std::string copyText;

    bool selectable() const {
        if (kind == ItemKind::Header || kind == ItemKind::Spacer) return false;
        return !(label.empty() && value.empty() && badge.empty());
    }
};

struct DetailLine {
    std::string label;
    std::string value;
    Tone tone = Tone::Default;
};

struct DetailPane {
    std::string title;
    std::vector<DetailLine> lines;
    std::string emptyText = "Nothing selected.";
};

struct PageView {
    Page page = Page::Main;
    std::string title;
    std::vector<std::string> banner;
    std::vector<Item> items;
    std::vector<std::string> footer;
    std::string status;
    DetailPane detail;
    bool showDetail = false;
    bool allowBack = true;
    std::string backLabel = "Back";
};

inline int utf8SequenceLength(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}

inline bool utf8Decode(std::string_view text, std::size_t pos,
                       char32_t& codePoint, std::size_t& length) {
    codePoint = 0;
    length = 0;
    if (pos >= text.size()) return false;
    const unsigned char lead = static_cast<unsigned char>(text[pos]);
    const int need = utf8SequenceLength(lead);
    if (need == 0 || pos + static_cast<std::size_t>(need) > text.size())
        return false;

    char32_t value = 0;
    switch (need) {
        case 1: value = lead; break;
        case 2: value = lead & 0x1Fu; break;
        case 3: value = lead & 0x0Fu; break;
        default: value = lead & 0x07u; break;
    }
    for (int i = 1; i < need; ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[pos + i]);
        if ((byte & 0xC0) != 0x80) return false;
        value = (value << 6) | (byte & 0x3Fu);
    }
    if (need == 2 && value < 0x80) return false;
    if (need == 3 && value < 0x800) return false;
    if (need == 4 && value < 0x10000) return false;
    if (value > 0x10FFFF) return false;
    if (value >= 0xD800 && value <= 0xDFFF) return false;

    codePoint = value;
    length = static_cast<std::size_t>(need);
    return true;
}

inline bool utf8Encode(char32_t codePoint, std::string& out) {
    if (codePoint > 0x10FFFF) return false;
    if (codePoint >= 0xD800 && codePoint <= 0xDFFF) return false;
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    return true;
}

inline bool utf8Valid(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        char32_t codePoint = 0;
        std::size_t length = 0;
        if (!utf8Decode(text, pos, codePoint, length)) return false;
        pos += length;
    }
    return true;
}

inline std::size_t utf8CodePointCount(std::string_view text) {
    std::size_t pos = 0;
    std::size_t count = 0;
    while (pos < text.size()) {
        char32_t codePoint = 0;
        std::size_t length = 0;
        if (!utf8Decode(text, pos, codePoint, length)) { ++pos; ++count; continue; }
        pos += length;
        ++count;
    }
    return count;
}

inline std::size_t utf8PrevBoundary(std::string_view text, std::size_t pos) {
    if (pos == 0) return 0;
    if (pos > text.size()) pos = text.size();
    std::size_t cursor = pos - 1;
    while (cursor > 0
           && (static_cast<unsigned char>(text[cursor]) & 0xC0) == 0x80) {
        --cursor;
    }
    return cursor;
}

inline std::size_t utf8NextBoundary(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) return text.size();
    char32_t codePoint = 0;
    std::size_t length = 0;
    if (utf8Decode(text, pos, codePoint, length)) return pos + length;
    return pos + 1;
}

inline bool isControlCodePoint(char32_t codePoint) {
    return codePoint < 0x20 || codePoint == 0x7F
        || (codePoint >= 0x80 && codePoint <= 0x9F);
}

inline bool isDisplayUnsafeCodePoint(char32_t codePoint) {
    if (isControlCodePoint(codePoint)) return true;
    switch (codePoint) {
        case 0x200E: case 0x200F:
        case 0x202A: case 0x202B: case 0x202C:
        case 0x202D: case 0x202E:
        case 0x2066: case 0x2067: case 0x2068: case 0x2069:
        case 0xFEFF:
            return true;
        default:
            return false;
    }
}

struct TextSanitizeOptions {
    bool singleLine = true;
    bool keepTabs = false;
    std::size_t maxBytes = 4096;
};

inline std::size_t skipEscapeSequence(std::string_view text, std::size_t pos) {
    ++pos;
    if (pos >= text.size()) return pos;
    const char introducer = text[pos];
    if (introducer == '[') {
        ++pos;
        while (pos < text.size()) {
            const unsigned char byte = static_cast<unsigned char>(text[pos]);
            ++pos;
            if (byte >= 0x40 && byte <= 0x7E) break;
        }
        return pos;
    }
    if (introducer == ']') {
        ++pos;
        while (pos < text.size()) {
            const unsigned char byte = static_cast<unsigned char>(text[pos]);
            if (byte == 0x07) return pos + 1;
            if (byte == 0x1B) {
                if (pos + 1 < text.size() && text[pos + 1] == '\\')
                    return pos + 2;
                return pos + 1;
            }
            ++pos;
        }
        return pos;
    }
    if (introducer == '(' || introducer == ')' || introducer == '#'
        || introducer == '%') {
        return pos + 2 <= text.size() ? pos + 2 : text.size();
    }
    return pos + 1;
}

inline std::string sanitizeText(std::string_view text,
                                const TextSanitizeOptions& options
                                    = TextSanitizeOptions{}) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (static_cast<unsigned char>(text[pos]) == 0x1B) {
            pos = skipEscapeSequence(text, pos);
            continue;
        }
        char32_t codePoint = 0;
        std::size_t length = 0;
        if (!utf8Decode(text, pos, codePoint, length)) {
            ++pos;
            continue;
        }
        pos += length;

        if (codePoint == '\r') continue;
        if (codePoint == '\n') {
            if (options.singleLine) break;
            if (out.size() + 1 > options.maxBytes) break;
            out.push_back('\n');
            continue;
        }
        if (codePoint == '\t') {
            if (out.size() + 1 > options.maxBytes) break;
            out.push_back(options.keepTabs ? '\t' : ' ');
            continue;
        }
        if (isDisplayUnsafeCodePoint(codePoint)) continue;

        std::string encoded;
        if (!utf8Encode(codePoint, encoded)) continue;
        if (out.size() + encoded.size() > options.maxBytes) break;
        out += encoded;
    }
    return out;
}

enum class TextRejection : uint8_t {
    None = 0,
    LengthLimit,
    ControlCharacter,
    InvalidEncoding,
    NotPermitted,
};

inline const char* describe(TextRejection rejection) {
    switch (rejection) {
        case TextRejection::None:             return "";
        case TextRejection::LengthLimit:      return "the field is full";
        case TextRejection::ControlCharacter: return "control characters were removed";
        case TextRejection::InvalidEncoding:  return "invalid text was removed";
        case TextRejection::NotPermitted:     return "that character is not allowed here";
    }
    return "";
}

class TextEntry {
public:
    explicit TextEntry(std::size_t maxBytes = 512) : _maxBytes(maxBytes) {}

    void setMaxBytes(std::size_t maxBytes) {
        _maxBytes = maxBytes == 0 ? 1 : maxBytes;
        if (_text.size() > _maxBytes) {
            std::size_t cut = _maxBytes;
            cut = utf8PrevBoundary(_text, cut + 1);
            _text.erase(cut);
            if (_caret > _text.size()) _caret = _text.size();
        }
    }
    std::size_t maxBytes() const { return _maxBytes; }

    void setNumericOnly(bool numeric) { _numericOnly = numeric; }
    bool numericOnly() const { return _numericOnly; }

    void setText(std::string_view utf8) {
        TextSanitizeOptions options;
        options.singleLine = true;
        options.maxBytes = _maxBytes;
        _text = sanitizeText(utf8, options);
        if (_numericOnly) stripNonNumeric(_text);
        _caret = _text.size();
        _rejection = TextRejection::None;
    }

    void clear() {
        _text.clear();
        _caret = 0;
        _rejection = TextRejection::None;
    }

    const std::string& text() const { return _text; }
    std::size_t caretBytes() const { return _caret; }
    std::size_t caretCodePoints() const {
        return utf8CodePointCount(std::string_view(_text).substr(0, _caret));
    }
    std::size_t lengthCodePoints() const { return utf8CodePointCount(_text); }
    bool empty() const { return _text.empty(); }
    bool full() const { return _text.size() >= _maxBytes; }
    TextRejection lastRejection() const { return _rejection; }

    bool insert(std::string_view utf8) {
        _rejection = TextRejection::None;
        const bool hadInvalid = !utf8Valid(utf8);
        const bool hadControl = containsControl(utf8);

        TextSanitizeOptions options;
        options.singleLine = true;
        options.maxBytes = 1u << 20;
        std::string clean = sanitizeText(utf8, options);
        if (_numericOnly) {
            const std::size_t before = clean.size();
            stripNonNumeric(clean);
            if (clean.size() != before) _rejection = TextRejection::NotPermitted;
        }

        bool truncated = false;
        std::string accepted;
        accepted.reserve(clean.size());
        std::size_t pos = 0;
        while (pos < clean.size()) {
            const std::size_t next = utf8NextBoundary(clean, pos);
            const std::size_t width = next - pos;
            if (_text.size() + accepted.size() + width > _maxBytes) {
                truncated = true;
                break;
            }
            accepted.append(clean, pos, width);
            pos = next;
        }

        if (truncated) _rejection = TextRejection::LengthLimit;
        else if (hadControl) _rejection = TextRejection::ControlCharacter;
        else if (hadInvalid) _rejection = TextRejection::InvalidEncoding;
        else if (_rejection == TextRejection::None
                 && accepted.empty() && !utf8.empty()) {
            _rejection = TextRejection::ControlCharacter;
        }

        if (accepted.empty()) return false;
        _text.insert(_caret, accepted);
        _caret += accepted.size();
        return true;
    }

    bool insertCodePoint(char32_t codePoint) {
        std::string encoded;
        if (!utf8Encode(codePoint, encoded)) {
            _rejection = TextRejection::InvalidEncoding;
            return false;
        }
        return insert(encoded);
    }

    bool backspace() {
        _rejection = TextRejection::None;
        if (_caret == 0) return false;
        const std::size_t start = utf8PrevBoundary(_text, _caret);
        _text.erase(start, _caret - start);
        _caret = start;
        return true;
    }

    bool deleteForward() {
        _rejection = TextRejection::None;
        if (_caret >= _text.size()) return false;
        const std::size_t end = utf8NextBoundary(_text, _caret);
        _text.erase(_caret, end - _caret);
        return true;
    }

    bool moveLeft() {
        if (_caret == 0) return false;
        _caret = utf8PrevBoundary(_text, _caret);
        return true;
    }
    bool moveRight() {
        if (_caret >= _text.size()) return false;
        _caret = utf8NextBoundary(_text, _caret);
        return true;
    }
    void moveHome() { _caret = 0; }
    void moveEnd() { _caret = _text.size(); }

private:
    static bool containsControl(std::string_view text) {
        std::size_t pos = 0;
        while (pos < text.size()) {
            char32_t codePoint = 0;
            std::size_t length = 0;
            if (!utf8Decode(text, pos, codePoint, length)) { ++pos; continue; }
            pos += length;
            if (isControlCodePoint(codePoint)) return true;
        }
        return false;
    }

    static void stripNonNumeric(std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char ch : text) {
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == ':' || ch == '-')
                out.push_back(ch);
        }
        text.swap(out);
    }

    std::string _text;
    std::size_t _caret = 0;
    std::size_t _maxBytes = 512;
    bool _numericOnly = false;
    TextRejection _rejection = TextRejection::None;
};

struct ActivateResult {
    bool accepted = false;
    Page page = Page::Main;
    ActionId action = ActionId::None;
    int index = -1;
    DisableReason reason = DisableReason::None;
    std::string detail;

    std::string reasonText() const {
        if (accepted) return std::string();
        std::string text = describe(reason);
        if (!detail.empty()) {
            text += " - ";
            text += detail;
        }
        return text;
    }
};

class NavigationModel {
public:
    NavigationModel() { reset(Page::Main); }

    void reset(Page root = Page::Main) {
        _stack.assign(1, root);
        for (int& remembered : _focusByPage) remembered = -1;
        _view = PageView{};
        _view.page = root;
        _view.title = pageTitle(root);
        _focus = -1;
    }
    Page page() const { return _stack.empty() ? Page::Main : _stack.back(); }
    std::size_t depth() const { return _stack.size(); }

    bool navigate(Page target) {
        if (target == Page::Count) return false;
        if (!_stack.empty() && _stack.back() == target) return false;
        rememberFocus();
        _stack.push_back(target);
        _focus = -1;
        return true;
    }

    bool back() {
        if (_stack.size() <= 1) return false;
        rememberFocus();
        _stack.pop_back();
        _focus = -1;
        return true;
    }

    void setView(PageView view) {
        const ActionId previousAction = focusedAction();
        const int previousIndex = focusedIndex();
        const Page previousPage = _view.page;

        _view = std::move(view);
        if (_view.title.empty()) _view.title = pageTitle(_view.page);

        if (_view.page == previousPage && previousAction != ActionId::None) {
            if (focusMatching(previousAction, previousIndex)) return;
        }
        const int remembered = rememberedFocus(_view.page);
        if (remembered >= 0 && focusIndexRaw(remembered)) return;
        focusFirst();
    }

    const PageView& view() const { return _view; }

    int focus() const { return _focus; }
    std::size_t itemCount() const { return _view.items.size(); }

    const Item* focusedItem() const {
        if (_focus < 0 || static_cast<std::size_t>(_focus) >= _view.items.size())
            return nullptr;
        return &_view.items[static_cast<std::size_t>(_focus)];
    }
    ActionId focusedAction() const {
        const Item* item = focusedItem();
        return item != nullptr ? item->action : ActionId::None;
    }
    int focusedIndex() const {
        const Item* item = focusedItem();
        return item != nullptr ? item->index : -1;
    }

    std::size_t selectableCount() const {
        std::size_t count = 0;
        for (const Item& item : _view.items)
            if (item.selectable()) ++count;
        return count;
    }

    bool moveFocus(int delta) {
        const int count = static_cast<int>(_view.items.size());
        if (count == 0 || selectableCount() == 0 || delta == 0) return false;
        const int step = delta > 0 ? 1 : -1;
        int remaining = delta > 0 ? delta : -delta;
        int cursor = _focus;
        if (cursor < 0) cursor = step > 0 ? -1 : count;

        while (remaining > 0) {
            int guard = 0;
            do {
                cursor += step;
                if (cursor >= count) cursor = 0;
                if (cursor < 0) cursor = count - 1;
                ++guard;
            } while (!_view.items[static_cast<std::size_t>(cursor)].selectable()
                     && guard <= count);
            if (!_view.items[static_cast<std::size_t>(cursor)].selectable())
                return false;
            --remaining;
        }
        _focus = cursor;
        rememberFocus();
        return true;
    }

    bool focusFirst() {
        for (std::size_t i = 0; i < _view.items.size(); ++i) {
            if (_view.items[i].selectable()) {
                _focus = static_cast<int>(i);
                rememberFocus();
                return true;
            }
        }
        _focus = -1;
        return false;
    }

    bool focusLast() {
        for (std::size_t i = _view.items.size(); i > 0; --i) {
            if (_view.items[i - 1].selectable()) {
                _focus = static_cast<int>(i - 1);
                rememberFocus();
                return true;
            }
        }
        _focus = -1;
        return false;
    }

    bool focusIndexRaw(int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= _view.items.size())
            return false;
        if (!_view.items[static_cast<std::size_t>(index)].selectable())
            return false;
        _focus = index;
        rememberFocus();
        return true;
    }

    bool focusAction(ActionId action, int payload = -1) {
        return focusMatching(action, payload);
    }

    ActivateResult activate() const {
        ActivateResult result;
        result.page = page();
        const Item* item = focusedItem();
        if (item == nullptr) {
            result.reason = DisableReason::NoSelection;
            return result;
        }
        result.action = item->action;
        result.index = item->index;
        result.detail = item->reasonDetail;
        if (!item->enabled) {
            result.reason = item->reason == DisableReason::None
                                ? DisableReason::Unavailable
                                : item->reason;
            return result;
        }
        if (item->action == ActionId::None) {
            result.reason = DisableReason::Unavailable;
            result.detail.clear();
            return result;
        }
        result.accepted = true;
        result.detail.clear();
        return result;
    }

private:
    bool focusMatching(ActionId action, int payload) {
        if (action == ActionId::None) return false;
        for (std::size_t i = 0; i < _view.items.size(); ++i) {
            const Item& item = _view.items[i];
            if (!item.selectable()) continue;
            if (item.action != action) continue;
            if (payload >= 0 && item.index != payload) continue;
            _focus = static_cast<int>(i);
            rememberFocus();
            return true;
        }
        return false;
    }

    void rememberFocus() {
        const std::size_t slot = static_cast<std::size_t>(_view.page);
        if (slot < static_cast<std::size_t>(Page::Count))
            _focusByPage[slot] = _focus;
    }
    int rememberedFocus(Page page) const {
        const std::size_t slot = static_cast<std::size_t>(page);
        if (slot >= static_cast<std::size_t>(Page::Count)) return -1;
        return _focusByPage[slot];
    }

    std::vector<Page> _stack;
    PageView _view;
    int _focus = -1;
    int _focusByPage[static_cast<std::size_t>(Page::Count)] = {};
};

struct MainMenuOptions {
    std::string nickname = "Player";
    unsigned hostPort = 7500;
    bool developerMode = false;
    bool gameConfigured = true;
    std::string gameProblem;
    bool sessionRunning = false;
};

inline Item makeAction(ActionId action, std::string label,
                       std::string hint = std::string()) {
    Item item;
    item.kind = ItemKind::Action;
    item.action = action;
    item.label = std::move(label);
    item.hint = std::move(hint);
    return item;
}

inline Item makeHeader(std::string label) {
    Item item;
    item.kind = ItemKind::Header;
    item.label = std::move(label);
    item.labelTone = Tone::Accent;
    return item;
}

inline Item makeSpacer() {
    Item item;
    item.kind = ItemKind::Spacer;
    return item;
}

inline Item& disable(Item& item, DisableReason reason, std::string detail = {}) {
    item.enabled = false;
    item.reason = reason;
    item.reasonDetail = std::move(detail);
    return item;
}

inline PageView buildMainMenu(const MainMenuOptions& options) {
    PageView view;
    view.page = Page::Main;
    view.title = pageTitle(Page::Main);
    view.banner.push_back("QOH99 Netplay - " + options.nickname);
    view.allowBack = true;
    view.backLabel = "Quit";

    Item host = makeAction(ActionId::GoHost,
                           "Host a game  (port "
                               + std::to_string(options.hostPort) + ")",
                           "Wait for an opponent on this port.");
    Item join = makeAction(ActionId::GoJoin, "Join a game",
                           "Connect to someone who is hosting.");
    Item spectate = makeAction(ActionId::GoSpectate, "Spectate a game",
                               "Watch a match that is already running.");
    if (!options.gameConfigured) {
        disable(host, DisableReason::GameNotConfigured, options.gameProblem);
        disable(join, DisableReason::GameNotConfigured, options.gameProblem);
        disable(spectate, DisableReason::GameNotConfigured, options.gameProblem);
    } else if (options.sessionRunning) {
        disable(host, DisableReason::SessionAlreadyRunning);
        disable(join, DisableReason::SessionAlreadyRunning);
        disable(spectate, DisableReason::SessionAlreadyRunning);
    }
    view.items.push_back(std::move(host));
    view.items.push_back(std::move(join));
    view.items.push_back(std::move(spectate));

    view.items.push_back(makeAction(ActionId::GoReplays, "Replays",
                                    "Watch and manage recorded matches."));

    Item offline = makeAction(ActionId::GoOffline, "Play offline",
                              "Training, VS Player, VS CPU, Free Play.");
    if (!options.gameConfigured)
        disable(offline, DisableReason::GameNotConfigured, options.gameProblem);
    else if (options.sessionRunning)
        disable(offline, DisableReason::SessionAlreadyRunning);
    view.items.push_back(std::move(offline));

    view.items.push_back(makeAction(ActionId::GoControls, "Controls",
                                    "Keyboard and controller bindings."));
    view.items.push_back(makeAction(ActionId::GoSettings, "Settings",
                                    "Nickname, paths, ports, delay, volume."));

    Item diagnostics = makeAction(ActionId::GoDiagnostics, "Diagnostics",
                                  "Developer self-tests and observe mode.");
    if (!options.developerMode)
        disable(diagnostics, DisableReason::DeveloperModeRequired);
    view.items.push_back(std::move(diagnostics));

    view.footer.push_back("Up/Down move   Enter choose   Esc quit");
    view.footer.push_back("Ctrl+C copy    Ctrl+V paste");
    return view;
}

struct OfflineMenuOptions {
    bool gameConfigured = true;
    std::string gameProblem;
};

inline PageView buildOfflineMenu(const OfflineMenuOptions& options) {
    PageView view;
    view.page = Page::Offline;
    view.title = pageTitle(Page::Offline);
    const ActionId actions[] = {
        ActionId::OfflineTraining, ActionId::OfflineVsPlayer,
        ActionId::OfflineVsCpu, ActionId::OfflineFreePlay,
    };
    const char* labels[] = { "Training", "VS Player", "VS CPU", "Free Play" };
    const char* hints[] = {
        "Boots straight into Training with the training overlay.",
        "Local 1v1 on this PC.",
        "1v1 against the CPU.",
        "Start the game with no netplay mode selected.",
    };
    for (int i = 0; i < 4; ++i) {
        Item item = makeAction(actions[i], labels[i], hints[i]);
        if (!options.gameConfigured)
            disable(item, DisableReason::GameNotConfigured, options.gameProblem);
        view.items.push_back(std::move(item));
    }
    view.footer.push_back("Up/Down move   Enter start   Esc back");
    return view;
}

inline PageView buildDiagnosticsMenu(bool developerMode) {
    PageView view;
    view.page = Page::Diagnostics;
    view.title = pageTitle(Page::Diagnostics);
    view.banner.push_back("Developer diagnostics");

    Item tests = makeAction(ActionId::DiagRunTransportTests,
                            "Run transport tests",
                            "Runs the in-process caster self-tests.");
    Item selftest = makeAction(ActionId::DiagSavestateSelfTest,
                               "Savestate self-test  (validate rollback regions)",
                               "Launches the game in savestate self-test mode.");
    Item observe = makeAction(ActionId::DiagObserveMode,
                              "Observe mode  (log game state, no changes)",
                              "Launches the game with logging hooks only.");
    if (!developerMode) {
        disable(tests, DisableReason::DeveloperModeRequired);
        disable(selftest, DisableReason::DeveloperModeRequired);
        disable(observe, DisableReason::DeveloperModeRequired);
    }
    view.items.push_back(std::move(tests));
    view.items.push_back(std::move(selftest));
    view.items.push_back(std::move(observe));
    view.items.push_back(makeSpacer());
    view.items.push_back(makeAction(ActionId::DiagCopyReport,
                                    "Copy compatibility report",
                                    "Copies renderer, font, SDL and input "
                                    "backend details to the clipboard."));
    view.items.push_back(makeAction(ActionId::DiagOpenStateFolder,
                                    "Show the state folder path",
                                    "Where logs, replays and config are "
                                    "written."));
    view.footer.push_back("Up/Down move   Enter run   Esc back");
    return view;
}

enum class RendererKind : uint8_t { None = 0, Accelerated, Software };

inline const char* rendererKindName(RendererKind kind) {
    switch (kind) {
        case RendererKind::Accelerated: return "accelerated";
        case RendererKind::Software:    return "software";
        case RendererKind::None:        break;
    }
    return "none";
}

struct RendererAttempt {
    RendererKind kind = RendererKind::Accelerated;
    std::string driver;
    bool succeeded = false;
    std::string failure;
};

struct RendererSelection {
    RendererKind kind = RendererKind::None;
    std::string driver;
    std::string fallbackReason;
    std::string failureReason;
    std::vector<RendererAttempt> attempts;

    bool usedSoftwareFallback() const { return kind == RendererKind::Software; }
    bool ok() const { return kind != RendererKind::None; }
};

using RendererFactory =
    std::function<bool(RendererKind kind, std::string& driverOut,
                       std::string& failureOut)>;

inline RendererSelection selectRenderer(const RendererFactory& factory,
                                        bool allowSoftwareFallback = true) {
    RendererSelection selection;
    if (!factory) {
        selection.failureReason = "no renderer factory was supplied";
        return selection;
    }

    const RendererKind order[] = { RendererKind::Accelerated,
                                   RendererKind::Software };
    for (RendererKind kind : order) {
        if (kind == RendererKind::Software && !allowSoftwareFallback) {
            selection.failureReason =
                selection.attempts.empty()
                    ? std::string("software fallback is disabled")
                    : selection.attempts.front().failure
                          + " (software fallback is disabled)";
            return selection;
        }

        RendererAttempt attempt;
        attempt.kind = kind;
        std::string driver;
        std::string failure;
        attempt.succeeded = factory(kind, driver, failure);
        attempt.driver = driver;
        if (attempt.succeeded) {
            attempt.failure.clear();
            selection.attempts.push_back(attempt);
            selection.kind = kind;
            selection.driver = driver.empty() ? std::string(rendererKindName(kind))
                                              : driver;
            if (kind == RendererKind::Software && !selection.attempts.empty()) {
                for (const RendererAttempt& earlier : selection.attempts) {
                    if (earlier.kind == RendererKind::Accelerated
                        && !earlier.succeeded) {
                        selection.fallbackReason = earlier.failure;
                        break;
                    }
                }
                if (selection.fallbackReason.empty()) {
                    selection.fallbackReason =
                        "the accelerated renderer was not available";
                }
            }
            return selection;
        }
        attempt.failure = failure.empty()
                              ? std::string("the renderer backend reported no "
                                            "error text")
                              : failure;
        selection.attempts.push_back(attempt);
    }

    selection.failureReason.clear();
    for (const RendererAttempt& attempt : selection.attempts) {
        if (!selection.failureReason.empty()) selection.failureReason += "; ";
        selection.failureReason += rendererKindName(attempt.kind);
        selection.failureReason += ": ";
        selection.failureReason += attempt.failure;
    }
    if (selection.failureReason.empty())
        selection.failureReason = "no renderer could be created";
    return selection;
}

struct FontProbeResult {
    std::string requestedFace;
    bool requestedFaceFound = false;
    std::string face;
    bool japaneseCapable = false;
    bool usedStockFont = false;
    bool japaneseSubstituted = false;
    std::string substitutedFrom;
    std::string note;
};

inline const std::vector<std::string>& japaneseFontCandidates() {
    static const std::vector<std::string> candidates = {
        "Meiryo", "Meiryo UI", "Yu Gothic UI", "Yu Gothic", "MS UI Gothic",
        "MS Gothic", "MS PGothic",
        "Noto Sans CJK JP", "Noto Sans JP", "Source Han Sans JP",
        "IPAexGothic", "IPAGothic", "IPAPGothic",
        "VL Gothic", "TakaoGothic", "Sazanami Gothic",
        "Droid Sans Fallback", "WenQuanYi Zen Hei",
    };
    return candidates;
}

inline const std::vector<std::string>& latinFontCandidates() {
    static const std::vector<std::string> candidates = {
        "Segoe UI", "Tahoma", "DejaVu Sans", "Liberation Sans",
        "Noto Sans", "Verdana", "Arial",
    };
    return candidates;
}

bool clipboardCopyUtf8(void* ownerWindow, const std::string& utf8,
                       std::string& error);
bool clipboardPasteUtf8(void* ownerWindow, std::string& utf8,
                        std::string& error);

enum class UiEventKind : uint8_t {
    None = 0,
    Activated,
    Rejected,
    Back,
    Quit,
    FocusChanged,
    TextCommitted,
    TextCancelled,
    CopyRequested,
    PasteRequested,
    Refresh,
    Resized,
};

struct UiEvent {
    UiEventKind kind = UiEventKind::None;
    Page page = Page::Main;
    ActionId action = ActionId::None;
    int index = -1;
    DisableReason reason = DisableReason::None;
    std::string text;
    std::string detail;
};

struct TextEntryRequest {
    std::string title;
    std::string prompt;
    std::string initial;
    std::string hint;
    std::size_t maxBytes = 512;
    bool numericOnly = false;
    bool showVirtualKeyboard = false;
    ActionId commitAction = ActionId::None;
    int index = -1;
};

struct SdlUiOptions {
    std::string windowTitle = "QOH99 Netplay";
    int width = 1040;
    int height = 660;
    bool resizable = true;
    bool highDpi = true;
    bool allowSoftwareFallback = true;
    bool gamepadNavigation = true;
    std::string preferredFontFace;
    float baseFontHeight = 17.0f;
};

class SdlUi {
public:
    SdlUi();
    ~SdlUi();
    SdlUi(const SdlUi&) = delete;
    SdlUi& operator=(const SdlUi&) = delete;

    static bool builtWithSdl();
    static bool runtimeAvailable(std::string& why);

    bool start(const SdlUiOptions& options, std::string& error);
    void stop();
    bool running() const;

    NavigationModel& model();
    const NavigationModel& model() const;
    void setPage(PageView view);
    Page page() const;

    bool pump(int timeoutMs, std::vector<UiEvent>& events);

    void setStatus(const std::string& utf8);
    void setToast(const std::string& utf8, int milliseconds = 2500);
    void showMessage(const std::string& title, const std::string& body,
                     const std::string& details = std::string());
    void dismissMessage();
    bool messageVisible() const;

    void beginTextEntry(const TextEntryRequest& request);
    void endTextEntry();
    bool textEntryActive() const;
    const TextEntry& textEntry() const;
    void setVirtualKeyboardVisible(bool visible);
    bool virtualKeyboardVisible() const;

    bool copyToClipboard(const std::string& utf8, std::string& error);
    bool pasteFromClipboard(std::string& utf8, std::string& error);

    void* nativeWindow() const;
    const RendererSelection& renderer() const;
    const FontProbeResult& font() const;
    const char* backendName() const;
    bool gamepadNavigationActive() const;
    bool acquireGamepadNavigation();
    void releaseGamepadNavigation();
    std::string diagnosticsText() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
}
