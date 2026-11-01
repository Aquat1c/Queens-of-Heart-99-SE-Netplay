#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qui {

constexpr int kBack    = -1;
constexpr int kTimeout = -2;
constexpr int kLeft    = -3;
constexpr int kRight   = -4;
constexpr int kClear   = -5;
constexpr int kKey     = -6;

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

struct Span {
    std::string text;
    Tone tone = Tone::Default;
};
using Row = std::vector<Span>;

struct ScreenSpec {
    std::vector<Row> above;
    std::vector<Row> items;
    std::vector<Row> below;
    bool reportClearKey = false;
    bool reportUnhandledKeys = false;
    std::vector<size_t> headers;

    bool selectable(size_t index) const {
        if (index >= items.size() || items[index].empty()) return false;
        for (size_t header : headers)
            if (header == index) return false;
        return true;
    }
};

namespace detail {

struct PollKeyOps {
    void* context = nullptr;
    DWORD (*nowMs)(void* context) = nullptr;
    DWORD (*wait)(void* context, DWORD timeoutMs) = nullptr;
    bool (*read)(void* context, INPUT_RECORD& record) = nullptr;
};

int pollKeyWithOps(const PollKeyOps& ops, int timeoutMs);

std::wstring displayTextToWide(std::string_view text);

std::size_t prefixFittingColumns(std::string_view text, int columns);

std::vector<std::string> wrapDisplayText(std::string_view text, int columns);

struct ClipboardWriteOps {
    void* context = nullptr;
    HGLOBAL (*allocate)(void* context, SIZE_T bytes) = nullptr;
    void* (*lock)(void* context, HGLOBAL memory) = nullptr;
    void (*unlock)(void* context, HGLOBAL memory) = nullptr;
    void (*release)(void* context, HGLOBAL memory) = nullptr;
    bool (*open)(void* context, HWND owner) = nullptr;
    bool (*empty)(void* context) = nullptr;
    HANDLE (*setText)(void* context, HGLOBAL memory) = nullptr;
    void (*close)(void* context) = nullptr;
};

bool copyTextWithOps(const std::string& text, HWND owner,
                     const ClipboardWriteOps& ops);

constexpr std::size_t kMaximumPromptLength = 512;

class PromptLineBuffer {
public:
    explicit PromptLineBuffer(
        UINT codePage,
        std::size_t maximumBytes = kMaximumPromptLength);

    void appendKeyUnit(wchar_t unit);

    void appendPastedText(std::wstring_view text);

    void backspace();

    const std::string& encoded() const { return _encoded; }
    bool hasPendingHighSurrogate() const { return _pendingHigh != 0; }

private:
    enum class AppendResult {
        Appended,
        Unrepresentable,
        Full,
    };

    AppendResult appendScalar(std::uint32_t scalar);

    UINT _codePage;
    std::size_t _maximumBytes;
    std::wstring _unicode;
    std::string _encoded;
    std::vector<std::size_t> _encodedScalarBytes;
    wchar_t _pendingHigh = 0;
};

}

class Console {
public:
    Console();
    ~Console();

    void setBanner(const std::string& banner) { _banner = banner; }

    int  menu(const std::string& title, const std::vector<std::string>& items,
              const std::string& backLabel = "Back", int initial = 0);

    int  menuTranslated(const std::string& title,
                        const std::vector<std::string>& items,
                        const std::string& backLabel, int initial = 0);

    int  menuTimed(const std::string& title, const std::vector<std::string>& items,
                   const std::string& backLabel, int& ioSel, int timeoutMs);

    int  menuTick(const ScreenSpec& spec, int& ioSel, int timeoutMs,
                  int* outKey = nullptr);

    int  columns() { return width(); }

    void draw(const std::vector<Row>& rows);

    bool promptLine(const std::string& title, std::string& out,
                    const std::string& initial = "",
                    UINT codePage = CP_ACP,
                    std::size_t maximumBytes = detail::kMaximumPromptLength);
    bool promptInt (const std::string& title, int& out, int initial = 0, int lo = INT_MIN, int hi = INT_MAX);

    bool copyText(const std::string& text);

    void message(const std::string& title, const std::string& body,
                 const std::string& details = "");
    void status (const std::string& line);
    void statusLine(const std::string& line);
    void resetStatusLine() { _statusRow = -1; }
    int  pollKey(int timeoutMs);

    void discardPendingInput();

    struct CapturedScan {
        unsigned short makeCode = 0;
        bool extended = false;
    };
    bool captureScanKey(int timeoutMs, CapturedScan& out);

    struct ScanCaptureState {
        bool ineligible[256] = {};
        unsigned short pendingMake = 0;
        bool pendingExtended = false;
        bool pendingDown = false;
    };
    enum class ScanStep : unsigned char { Pending, Committed, Cancelled };
    void beginScanCapture(ScanCaptureState& state);
    ScanStep stepScanCapture(ScanCaptureState& state, CapturedScan& out);
    void pause  (const std::string& prompt = "Press any key to continue...");
    void clear();

private:
    void header(std::vector<Row>& out, const std::string& title);
    void render(const std::vector<Row>& rows);
    void putRow(int y, const Row& row);
    void putRowVt(int y, const Row& row);
    void putRowLegacy(int y, const Row& row);
    WORD legacyAttr(Tone tone) const;
    void resetColor();
    void applyFontForLanguage();
    void showCursor(bool on);
    int  waitKeyCode();
    int  width();

    HANDLE _out, _in;
    wchar_t _savedFontFace[32] = {};
    bool _savedFontValid = false;
    int _fontLanguage = -1;
    WORD   _defaultAttr;
    bool   _vt = false;
    DWORD  _savedOutMode = 0;
    DWORD  _savedInMode  = 0;
    bool   _restoreModes = false;
    std::string _banner;
    int _lastLineCount = 0;
    int _statusRow = -1;
};

namespace config {
    std::string getStr(const char* key, const std::string& def = "");
    int         getInt(const char* key, int def = 0);
    void        setStr(const char* key, const std::string& val);
    void        setInt(const char* key, int val);
    void        removeKey(const char* key);
}

}
