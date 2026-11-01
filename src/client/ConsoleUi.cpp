#include "ConsoleUi.h"

#include "i18n/Tr.h"

#include "Log.h"
#include "platform/CompatEnv.h"
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace qui {

namespace detail {

int pollKeyWithOps(const PollKeyOps& ops, int timeoutMs) {
    if (!ops.nowMs || !ops.wait || !ops.read) return 0;

    const DWORD budget = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
    const DWORD start = ops.nowMs(ops.context);
    for (;;) {
        const DWORD elapsed = ops.nowMs(ops.context) - start;
        if (budget != 0 && elapsed >= budget) return 0;

        const DWORD remaining = elapsed < budget ? budget - elapsed : 0;
        if (ops.wait(ops.context, remaining) != WAIT_OBJECT_0) return 0;

        INPUT_RECORD rec{};
        if (!ops.read(ops.context, rec)) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        return rec.Event.KeyEvent.wVirtualKeyCode;
    }
}

bool copyTextWithOps(const std::string& text, HWND owner,
                     const ClipboardWriteOps& ops) {
    if (!owner || !ops.allocate || !ops.lock || !ops.unlock
        || !ops.release || !ops.open || !ops.empty || !ops.setText
        || !ops.close) {
        return false;
    }

    HGLOBAL memory = ops.allocate(ops.context, text.size() + 1);
    if (!memory) return false;
    void* const destination = ops.lock(ops.context, memory);
    if (!destination) {
        ops.release(ops.context, memory);
        return false;
    }
    std::memcpy(destination, text.c_str(), text.size() + 1);
    ops.unlock(ops.context, memory);

    if (!ops.open(ops.context, owner)) {
        ops.release(ops.context, memory);
        return false;
    }
    bool ok = false;
    if (ops.empty(ops.context)
        && ops.setText(ops.context, memory) != nullptr) {
        ok = true;
        memory = nullptr;
    }
    ops.close(ops.context);
    if (memory) ops.release(ops.context, memory);
    return ok;
}

namespace {

bool isHighSurrogate(wchar_t unit) {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool isLowSurrogate(wchar_t unit) {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

std::uint32_t decodeSurrogatePair(wchar_t high, wchar_t low) {
    return 0x10000u
        + ((static_cast<std::uint32_t>(high) - 0xD800u) << 10)
        + (static_cast<std::uint32_t>(low) - 0xDC00u);
}

bool isPromptLineBreak(std::uint32_t scalar) {
    return scalar == L'\r' || scalar == L'\n'
        || scalar == 0x2028u || scalar == 0x2029u;
}

bool isPromptControl(std::uint32_t scalar) {
    return scalar < 0x20u
        || (scalar >= 0x7Fu && scalar <= 0x9Fu);
}

bool encodeScalarExactly(std::uint32_t scalar, UINT codePage,
                         std::wstring& utf16, std::string& encoded) {
    if (scalar > 0x10FFFFu
        || (scalar >= 0xD800u && scalar <= 0xDFFFu)) {
        return false;
    }

    wchar_t units[2]{};
    int unitCount = 1;
    if (scalar <= 0xFFFFu) {
        units[0] = static_cast<wchar_t>(scalar);
    } else {
        scalar -= 0x10000u;
        units[0] = static_cast<wchar_t>(0xD800u + (scalar >> 10));
        units[1] = static_cast<wchar_t>(0xDC00u + (scalar & 0x3FFu));
        unitCount = 2;
    }

    const int byteCount = WideCharToMultiByte(
        codePage, 0, units, unitCount, nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) return false;

    std::string candidate(static_cast<std::size_t>(byteCount), '\0');
    if (WideCharToMultiByte(
            codePage, 0, units, unitCount, candidate.data(), byteCount,
            nullptr, nullptr) != byteCount) {
        return false;
    }

    const int roundTripUnits = MultiByteToWideChar(
        codePage, 0, candidate.data(), byteCount, nullptr, 0);
    if (roundTripUnits != unitCount) return false;
    wchar_t roundTrip[2]{};
    if (MultiByteToWideChar(
            codePage, 0, candidate.data(), byteCount, roundTrip,
            roundTripUnits) != roundTripUnits
        || !std::equal(units, units + unitCount, roundTrip)) {
        return false;
    }

    utf16.assign(units, units + unitCount);
    encoded = std::move(candidate);
    return true;
}

}

PromptLineBuffer::PromptLineBuffer(UINT codePage,
                                   std::size_t maximumBytes)
    : _codePage(codePage == CP_ACP ? GetACP() : codePage),
      _maximumBytes(maximumBytes) {}

PromptLineBuffer::AppendResult
PromptLineBuffer::appendScalar(std::uint32_t scalar) {
    if (isPromptControl(scalar) || isPromptLineBreak(scalar))
        return AppendResult::Unrepresentable;

    std::wstring utf16;
    std::string encoded;
    if (!encodeScalarExactly(scalar, _codePage, utf16, encoded))
        return AppendResult::Unrepresentable;
    if (encoded.size() > _maximumBytes - (std::min)(
            _encoded.size(), _maximumBytes)) {
        return AppendResult::Full;
    }

    _unicode += utf16;
    _encoded += encoded;
    _encodedScalarBytes.push_back(encoded.size());
    return AppendResult::Appended;
}

void PromptLineBuffer::appendKeyUnit(wchar_t unit) {
    if (_pendingHigh != 0) {
        const wchar_t high = _pendingHigh;
        _pendingHigh = 0;
        if (isLowSurrogate(unit)) {
            appendScalar(decodeSurrogatePair(high, unit));
            return;
        }
    }

    if (isHighSurrogate(unit)) {
        _pendingHigh = unit;
    } else if (!isLowSurrogate(unit)) {
        appendScalar(static_cast<std::uint32_t>(unit));
    }
}

void PromptLineBuffer::appendPastedText(std::wstring_view text) {
    _pendingHigh = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t unit = text[i];
        std::uint32_t scalar = 0;
        if (isHighSurrogate(unit)) {
            if (i + 1 >= text.size() || !isLowSurrogate(text[i + 1]))
                continue;
            scalar = decodeSurrogatePair(unit, text[++i]);
        } else if (isLowSurrogate(unit)) {
            continue;
        } else {
            scalar = static_cast<std::uint32_t>(unit);
        }

        if (isPromptLineBreak(scalar)) break;
        if (isPromptControl(scalar)) continue;
        if (appendScalar(scalar) == AppendResult::Full) break;
    }
}

void PromptLineBuffer::backspace() {
    if (_pendingHigh != 0) {
        _pendingHigh = 0;
        return;
    }
    if (_encodedScalarBytes.empty()) return;

    const std::size_t byteCount = _encodedScalarBytes.back();
    _encodedScalarBytes.pop_back();
    _encoded.resize(_encoded.size() - byteCount);

    std::size_t unitCount = 1;
    if (_unicode.size() >= 2
        && isLowSurrogate(_unicode.back())
        && isHighSurrogate(_unicode[_unicode.size() - 2])) {
        unitCount = 2;
    }
    _unicode.resize(_unicode.size() - unitCount);
}

}

namespace detail {

std::wstring displayTextToWide(std::string_view text) {
    if (text.empty()) return {};
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int units = MultiByteToWideChar(
        codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (units <= 0) {
        codePage = CP_ACP;
        flags = 0;
        units = MultiByteToWideChar(
            codePage, flags, text.data(), static_cast<int>(text.size()),
            nullptr, 0);
    }
    if (units <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(units), L'\0');
    if (MultiByteToWideChar(
            codePage, flags, text.data(), static_cast<int>(text.size()),
            wide.data(), units) != units) {
        return {};
    }
    return wide;
}

}

namespace {

bool isWideScalar(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E)
        || (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF)
        || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF)
        || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60)
        || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

void appendClipped(std::wstring& out, const std::wstring& wide,
                   int& ioCols, int maxCols) {
    for (std::size_t i = 0; i < wide.size() && ioCols < maxCols; ++i) {
        const wchar_t unit = wide[i];
        std::uint32_t scalar = unit;
        std::size_t units = 1;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < wide.size()
            && wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
            scalar = 0x10000u
                + ((static_cast<std::uint32_t>(unit) - 0xD800u) << 10)
                + (static_cast<std::uint32_t>(wide[i + 1]) - 0xDC00u);
            units = 2;
        }
        const int cells = isWideScalar(scalar) ? 2 : 1;
        if (ioCols + cells > maxCols) break;
        out.append(wide, i, units);
        ioCols += cells;
        i += units - 1;
    }
}

const wchar_t* vtSgr(Tone tone) {
    switch (tone) {
        case Tone::Title:    return L"\x1b[0;38;2;198;222;255m";
        case Tone::Dim:      return L"\x1b[0;38;2;138;146;160m";
        case Tone::Accent:   return L"\x1b[0;1;38;2;124;184;255m";
        case Tone::Good:     return L"\x1b[0;38;2;105;208;138m";
        case Tone::Warn:     return L"\x1b[0;38;2;232;193;103m";
        case Tone::Bad:      return L"\x1b[0;38;2;240;113;113m";
        case Tone::Selected:
            return L"\x1b[0;38;2;18;22;28;48;2;124;184;255m";
        case Tone::Default:  break;
    }
    return L"\x1b[0;38;2;230;232;236m";
}

}

namespace detail {

static std::size_t characterAt(std::string_view text, std::size_t offset,
                               std::uint32_t& scalar) {
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    scalar = lead;
    std::size_t length = 1;
    std::uint32_t value = lead;
    if (lead >= 0xF0u)      { length = 4; value = lead & 0x07u; }
    else if (lead >= 0xE0u) { length = 3; value = lead & 0x0Fu; }
    else if (lead >= 0xC0u) { length = 2; value = lead & 0x1Fu; }
    if (length == 1 || offset + length > text.size()) {
        scalar = lead;
        return 1;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char cont =
            static_cast<unsigned char>(text[offset + k]);
        if ((cont & 0xC0u) != 0x80u) { scalar = lead; return 1; }
        value = (value << 6) | (cont & 0x3Fu);
    }
    scalar = value;
    return length;
}

std::size_t prefixFittingColumns(std::string_view text, int columns) {
    if (columns <= 0) return 0;
    std::size_t offset = 0;
    int used = 0;
    while (offset < text.size()) {
        std::uint32_t scalar = 0;
        const std::size_t length = characterAt(text, offset, scalar);
        const int cells = isWideScalar(scalar) ? 2 : 1;
        if (used + cells > columns) break;
        used += cells;
        offset += length;
    }
    return offset;
}

std::vector<std::string> wrapDisplayText(std::string_view text, int columns) {
    std::vector<std::string> lines;
    if (columns <= 0) {
        lines.emplace_back(text);
        return lines;
    }
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() && text[cursor] == ' ') ++cursor;
        if (cursor >= text.size()) break;
        const std::string_view rest = text.substr(cursor);
        const std::size_t fits = prefixFittingColumns(rest, columns);
        if (fits >= rest.size()) {
            lines.emplace_back(rest);
            break;
        }
        std::size_t breakAt = rest.rfind(' ', fits);
        if (breakAt == std::string_view::npos || breakAt == 0) {
            breakAt = fits;
            if (breakAt == 0) {
                std::uint32_t scalar = 0;
                breakAt = characterAt(rest, 0, scalar);
            }
        }
        lines.emplace_back(rest.substr(0, breakAt));
        cursor += breakAt;
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

}

Console::Console() {
    _out = GetStdHandle(STD_OUTPUT_HANDLE);
    _in  = GetStdHandle(STD_INPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(_out, &info);
    _defaultAttr = info.wAttributes;
    SetConsoleTitleW(L"qoh99caster");

    CONSOLE_FONT_INFOEX saved{};
    saved.cbSize = sizeof(saved);
    if (GetCurrentConsoleFontEx(_out, FALSE, &saved)) {
        std::wmemcpy(_savedFontFace, saved.FaceName,
                     sizeof(_savedFontFace) / sizeof(wchar_t));
        _savedFontFace[(sizeof(_savedFontFace) / sizeof(wchar_t)) - 1] = L'\0';
        _savedFontValid = true;
    }

    DWORD outMode = 0;
    if (GetConsoleMode(_out, &outMode)) {
        _savedOutMode = outMode;
        _vt = SetConsoleMode(
                  _out, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }

    if (!_vt && qplat::detectCompat().wine && GetConsoleWindow() == nullptr)
        _vt = true;

    if (const char* forced = std::getenv("QOH99_CONSOLE_VT")) {
        if (forced[0] == '0') _vt = false;
        else if (forced[0] == '1') _vt = true;
    }

    QLOG("[console] backend=%s wine=%d vtNegotiated=%d",
         _vt ? "vt" : "legacy-cells",
         qplat::detectCompat().wine ? 1 : 0,
         (_savedOutMode != 0) ? 1 : 0);
    DWORD inMode = 0;
    if (GetConsoleMode(_in, &inMode)) {
        _savedInMode = inMode;
        _restoreModes = true;
    }
    SetConsoleMode(_in, ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT
                        | ENABLE_WINDOW_INPUT);
}

Console::~Console() {
    resetColor();
    showCursor(true);
    if (_restoreModes) SetConsoleMode(_in, _savedInMode);
    if (_savedOutMode) SetConsoleMode(_out, _savedOutMode);
}

void Console::resetColor() { SetConsoleTextAttribute(_out, _defaultAttr); }

void Console::showCursor(bool on) {
    CONSOLE_CURSOR_INFO ci{};
    GetConsoleCursorInfo(_out, &ci);
    ci.bVisible = on ? TRUE : FALSE;
    SetConsoleCursorInfo(_out, &ci);
}

int Console::width() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(_out, &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
    return 80;
}

WORD Console::legacyAttr(Tone tone) const {
    switch (tone) {
        case Tone::Title:
            return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        case Tone::Dim:
            return FOREGROUND_INTENSITY;
        case Tone::Accent:
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
                 | FOREGROUND_INTENSITY;
        case Tone::Good:
            return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Tone::Warn:
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Tone::Bad:
            return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case Tone::Selected:
            return BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE
                 | FOREGROUND_INTENSITY;
        case Tone::Default:
            break;
    }
    return _defaultAttr;
}

void Console::putRowVt(int y, const Row& row) {
    const int w = width();
    std::wstring out = L"\x1b[" + std::to_wstring(y + 1) + L";1H";
    int cols = 0;
    for (const Span& span : row) {
        if (cols >= w) break;
        out += vtSgr(span.tone);
        appendClipped(out, detail::displayTextToWide(span.text), cols, w);
    }
    out += L"\x1b[0m\x1b[K";
    DWORD written = 0;
    WriteConsoleW(_out, out.data(), static_cast<DWORD>(out.size()),
                  &written, nullptr);
}

void Console::putRowLegacy(int y, const Row& row) {
    const int w = width();
    int x = 0;
    DWORD wrote = 0;
    for (const Span& span : row) {
        if (x >= w) break;
        std::wstring text;
        int cells = 0;
        appendClipped(text, detail::displayTextToWide(span.text),
                      cells, w - x);
        if (text.empty()) continue;
        const COORD at{ static_cast<SHORT>(x), static_cast<SHORT>(y) };
        FillConsoleOutputAttribute(_out, legacyAttr(span.tone),
                                   static_cast<DWORD>(cells), at, &wrote);
        WriteConsoleOutputCharacterW(_out, text.c_str(),
                                     static_cast<DWORD>(text.size()), at,
                                     &wrote);
        x += cells;
    }
    if (x < w) {
        const COORD at{ static_cast<SHORT>(x), static_cast<SHORT>(y) };
        FillConsoleOutputAttribute(_out, _defaultAttr,
                                   static_cast<DWORD>(w - x), at, &wrote);
        FillConsoleOutputCharacterW(_out, L' ', static_cast<DWORD>(w - x), at,
                                    &wrote);
    }
}

void Console::putRow(int y, const Row& row) {
    if (_vt) putRowVt(y, row);
    else     putRowLegacy(y, row);
}

void Console::applyFontForLanguage() {
    const int wanted = static_cast<int>(qi18n::language());
    if (wanted == _fontLanguage) return;
    _fontLanguage = wanted;

    CONSOLE_FONT_INFOEX request{};
    request.cbSize = sizeof(request);
    if (!GetCurrentConsoleFontEx(_out, FALSE, &request)) {
        QLOG("[console] font query refused err=%lu; the host keeps its own "
             "face, so Japanese will draw as boxes unless it is a CJK font",
             GetLastError());
        return;
    }

    if (qi18n::language() == qi18n::Language::English) {
        if (!_savedFontValid) return;
        std::wmemcpy(request.FaceName, _savedFontFace,
                     sizeof(request.FaceName) / sizeof(wchar_t));
        SetCurrentConsoleFontEx(_out, FALSE, &request);
        return;
    }

    static const wchar_t* const kJapaneseConsoleFaces[] = {
        L"MS Gothic", L"NSimSun", L"SimSun-ExtB", L"Yu Gothic UI",
        L"Meiryo UI", L"Noto Sans Mono CJK JP",
    };
    const COORD size = request.dwFontSize;
    for (const wchar_t* face : kJapaneseConsoleFaces) {
        CONSOLE_FONT_INFOEX attempt = request;
        attempt.dwFontSize = size;
        attempt.FontFamily = 54;
        attempt.FontWeight = 400;
        std::wcsncpy(attempt.FaceName, face,
                     sizeof(attempt.FaceName) / sizeof(wchar_t) - 1);
        attempt.FaceName[sizeof(attempt.FaceName) / sizeof(wchar_t) - 1]
            = L'\0';
        if (SetCurrentConsoleFontEx(_out, FALSE, &attempt)) {
            QLOG("[console] font for Japanese: '%ls' (was '%ls')",
                 face, _savedFontValid ? _savedFontFace : L"?");
            return;
        }
        QLOG("[console] font '%ls' refused err=%lu", face, GetLastError());
    }
    QLOG("[console] no console font accepted; Japanese will draw as boxes "
         "unless this window is already using a CJK font");
}

void Console::render(const std::vector<Row>& rows) {
    applyFontForLanguage();
    showCursor(false);
    int y = 0;
    if (qi18n::language() == qi18n::Language::English) {
        for (const Row& row : rows) putRow(y++, row);
    } else {
        Row localized;
        for (const Row& row : rows) {
            localized.clear();
            localized.reserve(row.size());
            for (const Span& span : row)
                localized.push_back({ qi18n::trText(span.text), span.tone });
            putRow(y++, localized);
        }
    }
    for (; y < _lastLineCount; ++y) putRow(y, Row{});
    _lastLineCount = static_cast<int>(rows.size());
    _statusRow = -1;
    SetConsoleCursorPosition(_out, COORD{ 0, 0 });
}

void Console::draw(const std::vector<Row>& rows) { render(rows); }

void Console::clear() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(_out, &info)) return;
    DWORD cells = info.dwSize.X * info.dwSize.Y, written = 0;
    COORD home{ 0, 0 };
    FillConsoleOutputCharacterW(_out, L' ', cells, home, &written);
    FillConsoleOutputAttribute(_out, _defaultAttr, cells, home, &written);
    SetConsoleCursorPosition(_out, home);
    _lastLineCount = 0;
    _statusRow = -1;
    showCursor(true);
}

void Console::header(std::vector<Row>& out, const std::string& title) {
    if (!_banner.empty())
        out.push_back({ { " " + _banner, Tone::Dim } });
    const int titleColumns = (std::max)(20, width() - 2);
    for (const std::string& line
         : detail::wrapDisplayText(title, titleColumns)) {
        out.push_back({ { " " + line, Tone::Title } });
    }
    const int rule = (std::max)(20, width() - 2);
    const std::string glyph = "\xE2\x94\x80";
    std::string line;
    line.reserve(static_cast<std::size_t>(rule) * glyph.size());
    for (int i = 0; i < rule; ++i) line += glyph;
    out.push_back({ { " " + line, Tone::Dim } });
    out.push_back({});
}

namespace {

std::string trs(const std::string& text) {
    return qi18n::trText(text);
}

std::vector<std::string> trAll(const std::vector<std::string>& items) {
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const std::string& item : items) out.push_back(trs(item));
    return out;
}

}

int Console::menu(const std::string& titleIn,
                  const std::vector<std::string>& itemsIn,
                  const std::string& backLabelIn,
                  int initial) {
    const std::string title = trs(titleIn);
    const std::vector<std::string> items = trAll(itemsIn);
    const std::string backLabel = trs(backLabelIn);
    return menuTranslated(title, items, backLabel, initial);
}

int Console::menuTranslated(const std::string& title,
                  const std::vector<std::string>& items,
                  const std::string& backLabel,
                  int initial) {
    int sel = initial;
    const int n = static_cast<int>(items.size());
    const bool hasBack = !backLabel.empty();
    const int total = n + (hasBack ? 1 : 0);
    if (sel < 0) sel = 0;
    if (sel >= total) sel = total - 1;

    for (;;) {
        std::vector<Row> rows;
        header(rows, title);
        for (int i = 0; i < n; ++i) {
            const std::string num = "[" + std::to_string(i + 1) + "] ";
            if (i == sel) {
                rows.push_back({ { " > " + num + items[i], Tone::Selected } });
            } else {
                rows.push_back({ { "   ", Tone::Default },
                                 { num, Tone::Dim },
                                 { items[i], Tone::Default } });
            }
        }
        if (hasBack) {
            if (sel == n) {
                rows.push_back({ { " > [0] " + backLabel, Tone::Selected } });
            } else {
                rows.push_back({ { "   ", Tone::Default },
                                 { "[0] ", Tone::Dim },
                                 { backLabel, Tone::Dim } });
            }
        }
        rows.push_back({});
        rows.push_back({ { "  Up/Down + Enter | number keys | Esc = back",
                           Tone::Dim } });
        render(rows);

        INPUT_RECORD rec; DWORD read = 0;
        if (!ReadConsoleInput(_in, &rec, 1, &read) || read == 0) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        char ch = rec.Event.KeyEvent.uChar.AsciiChar;

        if (vk == VK_UP)          sel = (sel - 1 + total) % total;
        else if (vk == VK_DOWN)   sel = (sel + 1) % total;
        else if (vk == VK_RETURN) return (hasBack && sel == n) ? kBack : sel;
        else if (vk == VK_ESCAPE) return kBack;
        else if (ch == '0' && hasBack) return kBack;
        else if (ch >= '1' && ch <= '9') { int idx = ch - '1'; if (idx < n) return idx; }
    }
}

int Console::menuTimed(const std::string& title,
                       const std::vector<std::string>& items,
                       const std::string& backLabel,
                       int& ioSel, int timeoutMs) {
    ScreenSpec spec;
    header(spec.above, title);
    for (const std::string& item : items)
        spec.items.push_back({ { item, Tone::Default } });
    if (!backLabel.empty())
        spec.items.push_back({ { backLabel, Tone::Dim } });
    spec.below.push_back({});
    spec.below.push_back({ { "  Up/Down | Left/Right = adjust | Enter = select"
                             " | Esc = back", Tone::Dim } });
    const int n = static_cast<int>(items.size());
    const bool hasBack = !backLabel.empty();
    const int r = menuTick(spec, ioSel, timeoutMs);
    if (hasBack && r == n) return kBack;
    return r;
}

int Console::menuTick(const ScreenSpec& spec, int& ioSel, int timeoutMs,
                      int* outKey) {
    const int n = static_cast<int>(spec.items.size());
    if (ioSel < 0) ioSel = 0;
    if (n > 0 && ioSel >= n) ioSel = n - 1;
    const auto selectable = [&spec](int index) {
        return spec.selectable(static_cast<size_t>(index));
    };
    const auto move = [&](int from, int direction) {
        if (n == 0) return from;
        int index = from;
        for (int steps = 0; steps < n; ++steps) {
            index = (index + direction + n) % n;
            if (selectable(index)) return index;
        }
        return from;
    };
    if (n > 0 && !selectable(ioSel)) ioSel = move(ioSel, +1);

    std::vector<Row> rows = spec.above;
    for (int i = 0; i < n; ++i) {
        if (i == ioSel) {
            std::string merged = " > ";
            for (const Span& span : spec.items[i]) merged += span.text;
            rows.push_back({ { merged, Tone::Selected } });
        } else {
            Row row{ { "   ", Tone::Default } };
            row.insert(row.end(), spec.items[i].begin(), spec.items[i].end());
            rows.push_back(std::move(row));
        }
    }
    rows.insert(rows.end(), spec.below.begin(), spec.below.end());
    render(rows);

    const DWORD start = GetTickCount();
    for (;;) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= (DWORD)timeoutMs) return kTimeout;
        DWORD w = WaitForSingleObject(_in, (DWORD)timeoutMs - elapsed);
        if (w != WAIT_OBJECT_0) return kTimeout;
        INPUT_RECORD rec; DWORD read = 0;
        if (!ReadConsoleInput(_in, &rec, 1, &read) || read == 0) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        char ch = rec.Event.KeyEvent.uChar.AsciiChar;
        if (vk == VK_UP)     { if (n) ioSel = move(ioSel, -1); return kTimeout; }
        if (vk == VK_DOWN)   { if (n) ioSel = move(ioSel, +1); return kTimeout; }
        if (vk == VK_LEFT)   return kLeft;
        if (vk == VK_RIGHT)  return kRight;
        if (vk == VK_RETURN) return n && selectable(ioSel) ? ioSel : kTimeout;
        if (vk == VK_ESCAPE) return kBack;
        if (spec.reportClearKey
            && (vk == VK_DELETE || vk == VK_BACK)) {
            return kClear;
        }
        if (ch == '0')       return kBack;
        if (ch >= '1' && ch <= '9') {
            int idx = ch - '1';
            if (idx < n && selectable(idx)) return idx;
        }
        if (spec.reportUnhandledKeys && outKey != nullptr) {
            const unsigned char printable = static_cast<unsigned char>(ch);
            *outKey = (printable >= 0x20u && printable <= 0x7Eu)
                ? std::toupper(printable)
                : static_cast<int>(vk);
            return kKey;
        }
    }
}

void Console::statusLine(const std::string& lineIn) {
    const std::string line = trs(lineIn);
    if (_statusRow < 0) _statusRow = _lastLineCount++;
    putRow(_statusRow, { { "  " + line, Tone::Dim } });
    SetConsoleCursorPosition(_out, COORD{ 0, (SHORT)_lastLineCount });
}

int Console::pollKey(int timeoutMs) {
    struct Win32PollContext { HANDLE input; } context{ _in };
    detail::PollKeyOps ops{};
    ops.context = &context;
    ops.nowMs = [](void*) -> DWORD { return GetTickCount(); };
    ops.wait = [](void* opaque, DWORD waitMs) -> DWORD {
        const auto* ctx = static_cast<const Win32PollContext*>(opaque);
        return WaitForSingleObject(ctx->input, waitMs);
    };
    ops.read = [](void* opaque, INPUT_RECORD& rec) -> bool {
        const auto* ctx = static_cast<const Win32PollContext*>(opaque);
        DWORD read = 0;
        return ReadConsoleInput(ctx->input, &rec, 1, &read) != FALSE && read == 1;
    };
    return detail::pollKeyWithOps(ops, timeoutMs);
}

void Console::discardPendingInput() {
    DWORD pending = 0;
    const bool counted =
        GetNumberOfConsoleInputEvents(_in, &pending) != FALSE;
    if (!FlushConsoleInputBuffer(_in)) {
        QLOG("[console] pending-input discard failed error=%lu",
             static_cast<unsigned long>(GetLastError()));
        return;
    }
    QLOG("[console] pending-input discarded records=%lu",
         static_cast<unsigned long>(counted ? pending : 0u));
}

void Console::beginScanCapture(ScanCaptureState& state) {
    state = ScanCaptureState{};
    BYTE snapshot[256] = {};
    if (GetKeyboardState(snapshot)) {
        for (int vk = 1; vk < 256; ++vk)
            if (snapshot[vk] & 0x80u) state.ineligible[vk] = true;
    }
    state.ineligible[VK_RETURN] = true;
    for (int drain = 0; drain < 64; ++drain) {
        DWORD pending = 0;
        if (!GetNumberOfConsoleInputEvents(_in, &pending) || pending == 0)
            break;
        INPUT_RECORD record{};
        DWORD read = 0;
        if (!ReadConsoleInput(_in, &record, 1, &read) || read != 1) break;
    }
}

Console::ScanStep Console::stepScanCapture(ScanCaptureState& state,
                                           CapturedScan& out) {
    for (;;) {
        DWORD pending = 0;
        if (!GetNumberOfConsoleInputEvents(_in, &pending) || pending == 0)
            return ScanStep::Pending;
        INPUT_RECORD record{};
        DWORD read = 0;
        if (!ReadConsoleInput(_in, &record, 1, &read) || read != 1)
            return ScanStep::Pending;
        if (record.EventType != KEY_EVENT) continue;
        const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
        const WORD vk = key.wVirtualKeyCode;
        const unsigned short make = key.wVirtualScanCode;
        const bool extended =
            (key.dwControlKeyState & ENHANCED_KEY) != 0;
        if (!key.bKeyDown) {
            if (vk < 256) state.ineligible[vk] = false;
            if (state.pendingDown && make == state.pendingMake
                && extended == state.pendingExtended) {
                out.makeCode = state.pendingMake;
                out.extended = state.pendingExtended;
                return ScanStep::Committed;
            }
            continue;
        }
        if (vk == VK_ESCAPE && !(vk < 256 && state.ineligible[vk]))
            return ScanStep::Cancelled;
        if (vk < 256 && state.ineligible[vk]) continue;
        if (!state.pendingDown) {
            state.pendingMake = make;
            state.pendingExtended = extended;
            state.pendingDown = true;
            if (vk < 256) state.ineligible[vk] = true;
        }
    }
}

bool Console::captureScanKey(int timeoutMs, CapturedScan& out) {
    ScanCaptureState state;
    beginScanCapture(state);
    const DWORD startedMs = GetTickCount();
    for (;;) {
        switch (stepScanCapture(state, out)) {
            case ScanStep::Committed: return true;
            case ScanStep::Cancelled: return false;
            case ScanStep::Pending: break;
        }
        const DWORD elapsed = GetTickCount() - startedMs;
        if (elapsed >= static_cast<DWORD>(timeoutMs)) return false;
        if (WaitForSingleObject(_in, static_cast<DWORD>(timeoutMs) - elapsed)
                != WAIT_OBJECT_0) {
            return false;
        }
    }
}

bool Console::promptLine(const std::string& title, std::string& out,
                         const std::string& initial, UINT codePage,
                         std::size_t maximumBytes) {
    detail::PromptLineBuffer buf(codePage, maximumBytes);
    bool rightWasDown = false;
    auto appendClipboard = [&]() {
        if (!OpenClipboard(nullptr)) return;
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        const wchar_t* raw =
            data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
        if (raw) {
            const std::size_t capacity =
                static_cast<std::size_t>(GlobalSize(data) / sizeof(wchar_t));
            std::size_t length = 0;
            while (length < capacity && raw[length] != L'\0') ++length;
            if (length < capacity)
                buf.appendPastedText(std::wstring_view(raw, length));
            GlobalUnlock(data);
        }
        CloseClipboard();
    };
    for (;;) {
        std::vector<Row> rows;
        header(rows, title);
        rows.push_back({ { "  > ", Tone::Dim },
                         { buf.encoded(), Tone::Accent },
                         { "_", Tone::Dim } });
        rows.push_back({});
        rows.push_back({ { initial.empty()
                              ? "  Enter = accept | Ctrl+V/right click = paste | Esc = cancel"
                              : "  Enter = accept (blank keeps \"" + initial
                                    + "\") | Ctrl+V/right click = paste | Esc = cancel",
                           Tone::Dim } });
        render(rows);

        INPUT_RECORD rec; DWORD read = 0;
        if (!ReadConsoleInputW(_in, &rec, 1, &read) || read == 0) continue;
        if (rec.EventType == MOUSE_EVENT) {
            const bool rightDown =
                (rec.Event.MouseEvent.dwButtonState
                 & RIGHTMOST_BUTTON_PRESSED) != 0;
            if (rightDown && !rightWasDown) appendClipboard();
            rightWasDown = rightDown;
            continue;
        }
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch = rec.Event.KeyEvent.uChar.UnicodeChar;
        const DWORD control = rec.Event.KeyEvent.dwControlKeyState;
        if (vk == 'V'
            && (control & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0) {
            appendClipboard();
            continue;
        }
        if (vk == VK_ESCAPE) return false;
        if (vk == VK_RETURN) {
            out = buf.encoded().empty() ? initial : buf.encoded();
            return true;
        }
        if (vk == VK_BACK) {
            buf.backspace();
            continue;
        }
        if (ch != L'\0') buf.appendKeyUnit(ch);
    }
}

bool Console::copyText(const std::string& text) {
    detail::ClipboardWriteOps ops{};
    ops.allocate = [](void*, SIZE_T bytes) -> HGLOBAL {
        return GlobalAlloc(GMEM_MOVEABLE, bytes);
    };
    ops.lock = [](void*, HGLOBAL memory) -> void* {
        return GlobalLock(memory);
    };
    ops.unlock = [](void*, HGLOBAL memory) {
        GlobalUnlock(memory);
    };
    ops.release = [](void*, HGLOBAL memory) {
        GlobalFree(memory);
    };
    ops.open = [](void*, HWND owner) {
        return OpenClipboard(owner) != FALSE;
    };
    ops.empty = [](void*) {
        return EmptyClipboard() != FALSE;
    };
    ops.setText = [](void*, HGLOBAL memory) -> HANDLE {
        return SetClipboardData(CF_TEXT, memory);
    };
    ops.close = [](void*) {
        CloseClipboard();
    };
    return detail::copyTextWithOps(text, GetConsoleWindow(), ops);
}

bool Console::promptInt(const std::string& title, int& out, int initial, int lo, int hi) {
    for (;;) {
        std::string s;
        if (!promptLine(title, s, std::to_string(initial))) return false;
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        if (end && *end == '\0' && v >= lo && v <= hi) { out = (int)v; return true; }
        message(title, "Invalid - enter a number between " + std::to_string(lo)
                + " and " + std::to_string(hi) + ".");
    }
}

void Console::message(const std::string& titleIn, const std::string& bodyIn,
                      const std::string& detailsIn) {
    const std::string title = trs(titleIn);
    const std::string body = trs(bodyIn);
    const std::string details = trs(detailsIn);
    auto wrapInto = [this](std::vector<Row>& rows, const std::string& text,
                           Tone tone) {
        const int w = width() > 4 ? width() - 4 : 76;
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t nl = text.find('\n', pos);
            const std::string seg = text.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            for (const std::string& line : detail::wrapDisplayText(seg, w))
                rows.push_back({ { "  " + line, tone } });
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    };

    bool showDetails = false;
    for (;;) {
        std::vector<Row> rows;
        header(rows, title);
        wrapInto(rows, body, Tone::Default);
        if (showDetails && !details.empty()) {
            rows.push_back({});
            wrapInto(rows, details, Tone::Dim);
        }
        rows.push_back({});
        rows.push_back({ { (!details.empty() && !showDetails)
                               ? "  [D] technical details | any other key continues"
                               : "  Press any key to continue...",
                           Tone::Dim } });
        render(rows);
        const int vk = waitKeyCode();
        if (!details.empty() && !showDetails && vk == 'D') {
            showDetails = true;
            continue;
        }
        break;
    }
    _statusRow = -1;
}

void Console::status(const std::string& line) {
    putRow(_lastLineCount++, { { "  " + line, Tone::Dim } });
    SetConsoleCursorPosition(_out, COORD{ 0, (SHORT)_lastLineCount });
}

void Console::pause(const std::string& prompt) {
    putRow(_lastLineCount++, { { "  " + prompt, Tone::Dim } });
    waitKeyCode();
}

int Console::waitKeyCode() {
    for (;;) {
        INPUT_RECORD rec; DWORD read = 0;
        if (!ReadConsoleInput(_in, &rec, 1, &read) || read == 0) continue;
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
            return rec.Event.KeyEvent.wVirtualKeyCode;
    }
}

namespace config {

static const char* kSection = "qoh99caster";

static std::string iniPath() {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string p = exe;
    size_t s = p.find_last_of("\\/");
    return (s == std::string::npos ? std::string() : p.substr(0, s + 1)) + "qoh99caster.ini";
}

std::string getStr(const char* key, const std::string& def) {
    char buf[512] = {0};
    GetPrivateProfileStringA(kSection, key, def.c_str(), buf, sizeof(buf), iniPath().c_str());
    return buf;
}
int getInt(const char* key, int def) {
    return (int)GetPrivateProfileIntA(kSection, key, def, iniPath().c_str());
}
void setStr(const char* key, const std::string& val) {
    WritePrivateProfileStringA(kSection, key, val.c_str(), iniPath().c_str());
}
void setInt(const char* key, int val) {
    setStr(key, std::to_string(val));
}
void removeKey(const char* key) {
    WritePrivateProfileStringA(kSection, key, nullptr, iniPath().c_str());
}

}

}
