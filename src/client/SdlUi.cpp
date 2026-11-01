#include "client/SdlUi.h"

#include "i18n/Tr.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <unordered_map>

#ifdef QOH99_WITH_SDL3
#include <SDL3/SDL.h>
#endif

#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

namespace qclient {
namespace ui {

static std::string g_detectedGameEdition;

void setDetectedGameEdition(const std::string& edition) {
    g_detectedGameEdition = edition;
}

const std::string& detectedGameEdition() { return g_detectedGameEdition; }

namespace {

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), &wide[0], needed);
    return wide;
}

std::string toUtf8(const wchar_t* text, std::size_t length) {
    if (text == nullptr || length == 0) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text,
                                           static_cast<int>(length), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), &utf8[0],
                        needed, nullptr, nullptr);
    return utf8;
}

std::string win32ErrorText(const char* what, DWORD code) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s failed (error %lu)", what,
                  static_cast<unsigned long>(code));
    return std::string(buffer);
}

void copyFaceName(wchar_t (&target)[LF_FACESIZE], const std::wstring& face) {
    const std::size_t limit = LF_FACESIZE - 1;
    const std::size_t count = face.size() < limit ? face.size() : limit;
    for (std::size_t i = 0; i < count; ++i) target[i] = face[i];
    target[count] = L'\0';
}

struct EnumFaceContext {
    const wchar_t* wanted = nullptr;
    bool found = false;
};

int CALLBACK enumFaceProc(const LOGFONTW* logfont, const TEXTMETRICW*, DWORD,
                          LPARAM parameter) {
    EnumFaceContext* context = reinterpret_cast<EnumFaceContext*>(parameter);
    if (context != nullptr && logfont != nullptr
        && lstrcmpiW(logfont->lfFaceName, context->wanted) == 0) {
        context->found = true;
        return 0;
    }
    return 1;
}

bool faceInstalled(HDC dc, const std::wstring& face) {
    if (face.empty()) return false;
    LOGFONTW request{};
    request.lfCharSet = DEFAULT_CHARSET;
    copyFaceName(request.lfFaceName, face);
    EnumFaceContext context;
    context.wanted = face.c_str();
    EnumFontFamiliesExW(dc, &request, &enumFaceProc,
                        reinterpret_cast<LPARAM>(&context), 0);
    return context.found;
}

HFONT createFace(const std::wstring& face, int pixelHeight) {
    LOGFONTW logfont{};
    logfont.lfHeight = -pixelHeight;
    logfont.lfWeight = FW_NORMAL;
    logfont.lfCharSet = DEFAULT_CHARSET;
    logfont.lfOutPrecision = OUT_DEFAULT_PRECIS;
    logfont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    logfont.lfQuality = ANTIALIASED_QUALITY;
    logfont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    if (!face.empty()) copyFaceName(logfont.lfFaceName, face);
    return CreateFontIndirectW(&logfont);
}

bool fontHasJapaneseGlyphs(HDC dc) {
    static const wchar_t kProbe[] = { 0x65E5, 0x672C, 0x3042, 0 };
    WORD indices[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
    const DWORD result = GetGlyphIndicesW(dc, kProbe, 3, indices,
                                          GGI_MARK_NONEXISTING_GLYPHS);
    if (result == GDI_ERROR) return false;
    for (WORD index : indices)
        if (index == 0xFFFF) return false;
    return true;
}

class GdiTextSource {
public:
    ~GdiTextSource() { destroy(); }

    bool create(int pixelHeight, const std::string& preferredFace,
                FontProbeResult& probe) {
        destroy();
        _pixelHeight = pixelHeight < 8 ? 8 : pixelHeight;

        _screenDc = CreateCompatibleDC(nullptr);
        if (_screenDc == nullptr) return false;

        probe = FontProbeResult{};
        probe.requestedFace = preferredFace;

        std::wstring chosen;
        if (!preferredFace.empty()) {
            const std::wstring wide = toWide(preferredFace);
            if (faceInstalled(_screenDc, wide)) {
                chosen = wide;
                probe.requestedFaceFound = true;
            }
        }
        if (chosen.empty()) {
            for (const std::string& candidate : japaneseFontCandidates()) {
                const std::wstring wide = toWide(candidate);
                if (faceInstalled(_screenDc, wide)) { chosen = wide; break; }
            }
        }
        if (chosen.empty()) {
            for (const std::string& candidate : latinFontCandidates()) {
                const std::wstring wide = toWide(candidate);
                if (faceInstalled(_screenDc, wide)) { chosen = wide; break; }
            }
        }

        if (qi18n::language() != qi18n::Language::English) {
            HFONT probeFont = createFace(chosen, _pixelHeight);
            bool capable = false;
            if (probeFont != nullptr) {
                HGDIOBJ previous = SelectObject(_screenDc, probeFont);
                capable = fontHasJapaneseGlyphs(_screenDc);
                SelectObject(_screenDc, previous);
                DeleteObject(probeFont);
            }
            if (!capable) {
                const std::wstring wanted = chosen;
                for (const std::string& candidate : japaneseFontCandidates()) {
                    const std::wstring wide = toWide(candidate);
                    if (!faceInstalled(_screenDc, wide)) continue;
                    HFONT trial = createFace(wide, _pixelHeight);
                    if (trial == nullptr) continue;
                    HGDIOBJ previous = SelectObject(_screenDc, trial);
                    const bool ok = fontHasJapaneseGlyphs(_screenDc);
                    SelectObject(_screenDc, previous);
                    DeleteObject(trial);
                    if (!ok) continue;
                    chosen = wide;
                    probe.japaneseSubstituted = true;
                    break;
                }
                if (probe.japaneseSubstituted) {
                    probe.substitutedFrom =
                        toUtf8(wanted.c_str(), wanted.size());
                }
            }
        }

        _font = createFace(chosen, _pixelHeight);
        if (_font == nullptr) {
            _font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            _ownsFont = false;
            probe.usedStockFont = true;
        } else {
            _ownsFont = true;
        }
        if (_font == nullptr) return false;
        SelectObject(_screenDc, _font);

        TEXTMETRICW metrics{};
        GetTextMetricsW(_screenDc, &metrics);
        _lineHeight = metrics.tmHeight + metrics.tmExternalLeading;
        if (_lineHeight <= 0) _lineHeight = _pixelHeight + 4;

        wchar_t actual[LF_FACESIZE] = {};
        GetTextFaceW(_screenDc, LF_FACESIZE, actual);
        probe.face = toUtf8(actual, std::wcslen(actual));
        probe.japaneseCapable = fontHasJapaneseGlyphs(_screenDc);

        if (!probe.requestedFace.empty() && !probe.requestedFaceFound) {
            probe.note = "The requested font \"" + probe.requestedFace
                       + "\" is not installed in this prefix; using \""
                       + probe.face + "\" instead.";
        } else {
            probe.note = "Using the installed font \"" + probe.face + "\".";
        }
        if (probe.japaneseSubstituted) {
            probe.note += "  \"" + probe.substitutedFrom
                       + "\" cannot draw Japanese, so this font was used for "
                         "the Japanese interface instead.";
        }
        if (!probe.japaneseCapable) {
            probe.note +=
                "  It has no Japanese glyphs, so Japanese names will show as "
                "boxes.  Install a CJK font in the prefix (for example "
                "fonts-noto-cjk / Noto Sans CJK JP).  The prefix locale does "
                "NOT need to be Japanese.";
        }
        _probe = probe;
        return true;
    }

    void destroy() {
        if (_memoryBitmap != nullptr) {
            if (_memoryDc != nullptr && _oldBitmap != nullptr)
                SelectObject(_memoryDc, _oldBitmap);
            DeleteObject(_memoryBitmap);
            _memoryBitmap = nullptr;
            _oldBitmap = nullptr;
            _bits = nullptr;
            _surfaceWidth = 0;
            _surfaceHeight = 0;
        }
        if (_memoryDc != nullptr) { DeleteDC(_memoryDc); _memoryDc = nullptr; }
        if (_screenDc != nullptr) { DeleteDC(_screenDc); _screenDc = nullptr; }
        if (_font != nullptr && _ownsFont) DeleteObject(_font);
        _font = nullptr;
        _ownsFont = false;
    }

    int lineHeight() const { return _lineHeight > 0 ? _lineHeight : 16; }
    const FontProbeResult& probe() const { return _probe; }
    bool ready() const { return _screenDc != nullptr && _font != nullptr; }

    int measure(const std::string& utf8) {
        if (!ready() || utf8.empty()) return 0;
        const std::wstring wide = toWide(utf8);
        if (wide.empty()) return 0;
        SIZE size{};
        if (!GetTextExtentPoint32W(_screenDc, wide.c_str(),
                                   static_cast<int>(wide.size()), &size))
            return 0;
        return size.cx;
    }

    bool rasterize(const std::string& utf8, std::vector<uint32_t>& argb,
                   int& width, int& height) {
        width = 0;
        height = 0;
        if (!ready() || utf8.empty()) return false;
        const std::wstring wide = toWide(utf8);
        if (wide.empty()) return false;

        SIZE size{};
        if (!GetTextExtentPoint32W(_screenDc, wide.c_str(),
                                   static_cast<int>(wide.size()), &size))
            return false;
        const int surfaceWidth = size.cx + 4;
        const int surfaceHeight = (size.cy > 0 ? size.cy : _lineHeight) + 2;
        if (surfaceWidth <= 0 || surfaceHeight <= 0) return false;
        if (surfaceWidth > 8192 || surfaceHeight > 512) return false;
        if (!ensureSurface(surfaceWidth, surfaceHeight)) return false;

        const std::size_t pixels =
            static_cast<std::size_t>(_surfaceWidth) * _surfaceHeight;
        std::memset(_bits, 0, pixels * 4);

        SetBkMode(_memoryDc, TRANSPARENT);
        SetTextColor(_memoryDc, RGB(255, 255, 255));
        TextOutW(_memoryDc, 0, 0, wide.c_str(), static_cast<int>(wide.size()));
        GdiFlush();

        argb.assign(static_cast<std::size_t>(surfaceWidth) * surfaceHeight, 0);
        const uint32_t* source = static_cast<const uint32_t*>(_bits);
        for (int y = 0; y < surfaceHeight; ++y) {
            const uint32_t* row = source
                                + static_cast<std::size_t>(y) * _surfaceWidth;
            uint32_t* target = argb.data()
                             + static_cast<std::size_t>(y) * surfaceWidth;
            for (int x = 0; x < surfaceWidth; ++x) {
                const uint32_t pixel = row[x];
                const uint32_t r = (pixel >> 16) & 0xFFu;
                const uint32_t g = (pixel >> 8) & 0xFFu;
                const uint32_t b = pixel & 0xFFu;
                uint32_t alpha = r > g ? r : g;
                if (b > alpha) alpha = b;
                target[x] = (alpha << 24) | 0x00FFFFFFu;
            }
        }
        width = surfaceWidth;
        height = surfaceHeight;
        return true;
    }

private:
    bool ensureSurface(int width, int height) {
        if (_memoryBitmap != nullptr && width <= _surfaceWidth
            && height <= _surfaceHeight) {
            return true;
        }
        const int newWidth = width > _surfaceWidth ? width : _surfaceWidth;
        const int newHeight = height > _surfaceHeight ? height : _surfaceHeight;

        if (_memoryDc == nullptr) {
            _memoryDc = CreateCompatibleDC(nullptr);
            if (_memoryDc == nullptr) return false;
            SelectObject(_memoryDc, _font);
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = newWidth;
        info.bmiHeader.biHeight = -newHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(_memoryDc, &info, DIB_RGB_COLORS,
                                          &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr) return false;

        HGDIOBJ previous = SelectObject(_memoryDc, bitmap);
        if (_memoryBitmap == nullptr) _oldBitmap = previous;
        if (_memoryBitmap != nullptr) DeleteObject(_memoryBitmap);
        _memoryBitmap = bitmap;
        _bits = bits;
        _surfaceWidth = newWidth;
        _surfaceHeight = newHeight;
        return true;
    }

    HDC _screenDc = nullptr;
    HDC _memoryDc = nullptr;
    HBITMAP _memoryBitmap = nullptr;
    HGDIOBJ _oldBitmap = nullptr;
    void* _bits = nullptr;
    int _surfaceWidth = 0;
    int _surfaceHeight = 0;
    HFONT _font = nullptr;
    bool _ownsFont = false;
    int _pixelHeight = 16;
    int _lineHeight = 0;
    FontProbeResult _probe;
};

#ifdef QOH99_WITH_SDL3

struct Rgba { uint8_t r, g, b, a; };

Rgba toneColor(Tone tone) {
    switch (tone) {
        case Tone::Title:    return { 0xFF, 0xFF, 0xFF, 0xFF };
        case Tone::Dim:      return { 0x8A, 0x90, 0x9B, 0xFF };
        case Tone::Accent:   return { 0x6F, 0xB2, 0xFF, 0xFF };
        case Tone::Good:     return { 0x69, 0xD0, 0x8A, 0xFF };
        case Tone::Warn:     return { 0xE8, 0xC1, 0x67, 0xFF };
        case Tone::Bad:      return { 0xF0, 0x71, 0x71, 0xFF };
        case Tone::Selected: return { 0x12, 0x16, 0x1C, 0xFF };
        case Tone::Default:  break;
    }
    return { 0xE6, 0xE8, 0xEC, 0xFF };
}

constexpr Rgba kBackground   = { 0x11, 0x13, 0x18, 0xFF };
constexpr Rgba kHeaderBand   = { 0x16, 0x19, 0x20, 0xFF };
constexpr Rgba kPanel        = { 0x1B, 0x1F, 0x27, 0xFF };
constexpr Rgba kPanelEdge    = { 0x2E, 0x34, 0x40, 0xFF };
constexpr Rgba kSelection    = { 0x6F, 0xB2, 0xFF, 0x2E };
constexpr Rgba kSelectionBar = { 0x7C, 0xB8, 0xFF, 0xFF };
constexpr Rgba kSeparator    = { 0x24, 0x29, 0x33, 0xFF };
constexpr Rgba kHeaderRule   = { 0x3A, 0x5A, 0x86, 0xFF };
constexpr Rgba kOverlay      = { 0x00, 0x00, 0x00, 0xCA };

const char* const kVirtualRows[] = {
    "1234567890-",
    "qwertyuiop",
    "asdfghjkl:.",
    "zxcvbnm_/@",
};
constexpr int kVirtualActionRow = 4;
const char* const kVirtualActions[] = { "Shift", "Space", "Back", "Paste",
                                        "OK", "Cancel" };
constexpr int kVirtualActionCount = 6;

void fillRect(SDL_Renderer* renderer, float x, float y, float w, float h,
              Rgba color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const SDL_FRect rect{ x, y, w, h };
    SDL_RenderFillRect(renderer, &rect);
}

void strokeRect(SDL_Renderer* renderer, float x, float y, float w, float h,
                Rgba color) {
    fillRect(renderer, x, y, w, 1.0f, color);
    fillRect(renderer, x, y + h - 1.0f, w, 1.0f, color);
    fillRect(renderer, x, y, 1.0f, h, color);
    fillRect(renderer, x + w - 1.0f, y, 1.0f, h, color);
}

#endif

}

bool clipboardCopyUtf8(void* ownerWindow, const std::string& utf8,
                       std::string& error) {
    error.clear();
    const HWND owner = reinterpret_cast<HWND>(ownerWindow);

    const std::wstring wide = toWide(utf8);
    if (!utf8.empty() && wide.empty()) {
        error = "the text could not be converted to Unicode";
        return false;
    }

    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        error = win32ErrorText("GlobalAlloc(clipboard)", GetLastError());
        return false;
    }
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        error = win32ErrorText("GlobalLock(clipboard)", GetLastError());
        GlobalFree(memory);
        return false;
    }
    std::memcpy(target, wide.c_str(), bytes);
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        error = win32ErrorText("OpenClipboard", GetLastError());
        GlobalFree(memory);
        return false;
    }
    if (!EmptyClipboard()) {
        error = win32ErrorText("EmptyClipboard", GetLastError());
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        error = win32ErrorText("SetClipboardData(CF_UNICODETEXT)",
                               GetLastError());
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    // Ownership has transferred to the clipboard: do NOT free on success.
    CloseClipboard();
    return true;
}

bool clipboardPasteUtf8(void* ownerWindow, std::string& utf8,
                        std::string& error) {
    utf8.clear();
    error.clear();
    const HWND owner = reinterpret_cast<HWND>(ownerWindow);

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        error = describe(DisableReason::NoClipboardText);
        return false;
    }
    if (!OpenClipboard(owner)) {
        error = win32ErrorText("OpenClipboard", GetLastError());
        return false;
    }
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) {
        error = describe(DisableReason::NoClipboardText);
        CloseClipboard();
        return false;
    }
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(handle));
    if (text == nullptr) {
        error = win32ErrorText("GlobalLock(clipboard)", GetLastError());
        CloseClipboard();
        return false;
    }
    const std::size_t maximum = GlobalSize(handle) / sizeof(wchar_t);
    std::size_t length = 0;
    while (length < maximum && text[length] != L'\0') ++length;
    utf8 = toUtf8(text, length);
    GlobalUnlock(handle);
    CloseClipboard();
    return true;
}

struct SdlUi::Impl {
    SdlUiOptions options;
    NavigationModel model;
    RendererSelection renderer;
    FontProbeResult font;
    bool started = false;
    std::string startError;

    std::string status;
    std::string toastText;
    unsigned long long toastUntilMs = 0;

    bool messageVisible = false;
    std::string messageTitle;
    std::string messageBody;
    std::string messageDetails;
    bool messageDetailsShown = false;

    bool entryActive = false;
    TextEntryRequest entryRequest;
    TextEntry entry;
    bool virtualKeyboard = false;
    bool vkShift = false;
    int vkRow = 0;
    int vkColumn = 0;

    int scroll = 0;

    std::string accessibleTitle() const {
        const PageView& view = model.view();
        std::string composed = view.title.empty() ? pageTitle(view.page)
                                                  : view.title;
        if (entryActive) {
            composed += " - " + (entryRequest.prompt.empty()
                                     ? entryRequest.title
                                     : entryRequest.prompt);
        } else if (messageVisible) {
            composed += " - " + messageTitle;
        } else {
            const Item* item = model.focusedItem();
            if (item != nullptr) {
                composed += " - " + item->label;
                if (!item->value.empty()) composed += ": " + item->value;
                if (!item->badge.empty())
                    composed += " [" + item->badge.text + "]";
                if (!item->enabled) {
                    composed += " (unavailable: ";
                    composed += describe(item->reason);
                    composed += ")";
                }
            }
        }
        return composed + " - " + options.windowTitle;
    }

#ifndef QOH99_WITH_SDL3
    void* hwnd() const { return nullptr; }
#else
    SDL_Window* window = nullptr;
    SDL_Renderer* sdlRenderer = nullptr;
    bool ownsVideo = false;
    bool ownsGamepad = false;
    bool gamepadForeignOwner = false;
    std::vector<SDL_Gamepad*> gamepads;
    float displayScale = 1.0f;
    int pixelWidth = 0;
    int pixelHeight = 0;
    GdiTextSource text;

    struct CachedText {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
        unsigned long long lastUsedFrame = 0;
    };
    std::unordered_map<std::string, CachedText> textCache;
    unsigned long long frame = 0;

    int axisDirection = 0;
    unsigned long long axisRepeatDueMs = 0;

    struct VirtualKeyboardBox {
        float x = 0.0f, y = 0.0f, keyWidth = 0.0f, rowHeight = 0.0f;
    } vkBox;

    struct Layout {
        float pad = 0;
        float lineHeight = 0;
        float rowHeight = 0;
        float listTop = 0;
        float listBottom = 0;
        float listLeft = 0;
        float listRight = 0;
        float detailLeft = 0;
        float detailRight = 0;
        int visibleRows = 1;
    };

    void* hwnd() const {
        if (window == nullptr) return nullptr;
        const SDL_PropertiesID props = SDL_GetWindowProperties(window);
        if (props == 0) return nullptr;
        return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                      nullptr);
    }

    void toast(const std::string& utf8, int milliseconds = 2500) {
        toastText = sanitizeText(utf8);
        toastUntilMs = SDL_GetTicks()
                     + static_cast<unsigned long long>(
                           milliseconds < 0 ? 0 : milliseconds);
    }
    void refreshWindowTitle() {
        if (window != nullptr)
            SDL_SetWindowTitle(window, accessibleTitle().c_str());
    }

    Layout layout() const {
        Layout out;
        out.lineHeight = static_cast<float>(text.lineHeight());
        out.pad = std::max(12.0f, out.lineHeight * 1.1f);
        out.rowHeight = out.lineHeight * 1.45f;

        const PageView& view = model.view();
        const float bannerRows = static_cast<float>(view.banner.size());
        const float footerRows = static_cast<float>(view.footer.size());

        out.listTop = out.pad + out.lineHeight * 1.9f
                    + bannerRows * out.lineHeight + out.lineHeight * 0.9f;
        out.listBottom = static_cast<float>(pixelHeight) - out.pad
                       - footerRows * out.lineHeight - out.lineHeight * 1.5f;
        out.listLeft = out.pad;
        out.listRight = static_cast<float>(pixelWidth) - out.pad;
        if (view.showDetail) {
            const float split = out.listLeft
                              + (out.listRight - out.listLeft) * 0.56f;
            out.listRight = split - out.pad * 0.5f;
            out.detailLeft = split + out.pad * 0.5f;
            out.detailRight = static_cast<float>(pixelWidth) - out.pad;
        }
        const float usable = out.listBottom - out.listTop;
        out.visibleRows = usable > 0.0f
                              ? static_cast<int>(usable / out.rowHeight)
                              : 1;
        if (out.visibleRows < 1) out.visibleRows = 1;
        return out;
    }

    CachedText* acquireText(const std::string& utf8) {
        auto found = textCache.find(utf8);
        if (found != textCache.end()) {
            found->second.lastUsedFrame = frame;
            return &found->second;
        }
        std::vector<uint32_t> pixels;
        int width = 0;
        int height = 0;
        if (!text.rasterize(utf8, pixels, width, height)) return nullptr;

        SDL_Texture* texture = SDL_CreateTexture(
            sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
            width, height);
        if (texture == nullptr) return nullptr;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(texture, nullptr, pixels.data(),
                          width * static_cast<int>(sizeof(uint32_t)));

        CachedText created;
        created.texture = texture;
        created.width = width;
        created.height = height;
        created.lastUsedFrame = frame;
        return &textCache.emplace(utf8, created).first->second;
    }

    void clearTextCache() {
        for (auto& cached : textCache)
            if (cached.second.texture != nullptr)
                SDL_DestroyTexture(cached.second.texture);
        textCache.clear();
    }

    void evictTextCache() {
        for (auto it = textCache.begin(); it != textCache.end();) {
            if (frame - it->second.lastUsedFrame > 900) {
                if (it->second.texture != nullptr)
                    SDL_DestroyTexture(it->second.texture);
                it = textCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    int measure(const std::string& utf8) { return text.measure(utf8); }

    int draw(float x, float y, const std::string& utf8, Tone tone,
             float maxWidth = 0.0f) {
        if (utf8.empty()) return 0;
        CachedText* cached = acquireText(utf8);
        if (cached == nullptr || cached->texture == nullptr) return 0;
        const Rgba color = toneColor(tone);
        SDL_SetTextureColorMod(cached->texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(cached->texture, color.a);

        float width = static_cast<float>(cached->width);
        SDL_FRect source{ 0.0f, 0.0f, width,
                          static_cast<float>(cached->height) };
        if (maxWidth > 0.0f && width > maxWidth) {
            source.w = maxWidth;
            width = maxWidth;
        }
        SDL_FRect target{ x, y, width, static_cast<float>(cached->height) };
        SDL_RenderTexture(sdlRenderer, cached->texture, &source, &target);
        return static_cast<int>(width);
    }

    qi18n::Language fontLanguage = qi18n::Language::English;

    void rebuildFont() {
        fontLanguage = qi18n::language();
        clearTextCache();
        const int pixels = static_cast<int>(
            std::lround(static_cast<double>(options.baseFontHeight)
                        * static_cast<double>(displayScale)));
        text.create(pixels, options.preferredFontFace, font);
    }

    void ensureFocusVisible() {
        const Layout box = layout();
        const int focus = model.focus();
        if (focus < 0) return;
        if (focus < scroll) scroll = focus;
        if (focus >= scroll + box.visibleRows)
            scroll = focus - box.visibleRows + 1;
        if (scroll < 0) scroll = 0;
    }

    void announceFocus(std::vector<UiEvent>& events) {
        UiEvent event;
        event.kind = UiEventKind::FocusChanged;
        event.page = model.page();
        const Item* item = model.focusedItem();
        if (item != nullptr) {
            event.action = item->action;
            event.index = item->index;
            event.detail = item->hint;
        }
        events.push_back(std::move(event));
        refreshWindowTitle();
        ensureFocusVisible();
    }

    void navigateBy(int delta, std::vector<UiEvent>& events) {
        if (model.moveFocus(delta)) announceFocus(events);
    }

    void reject(std::vector<UiEvent>& events) {
        const ActivateResult result = model.activate();
        UiEvent event;
        event.kind = UiEventKind::Rejected;
        event.page = result.page;
        event.action = result.action;
        event.index = result.index;
        event.reason = result.reason;
        event.detail = result.reasonText();
        toast(event.detail, 3500);
        events.push_back(std::move(event));
    }

    void activateFocused(std::vector<UiEvent>& events) {
        const ActivateResult result = model.activate();
        if (!result.accepted) { reject(events); return; }
        UiEvent event;
        event.kind = UiEventKind::Activated;
        event.page = result.page;
        event.action = result.action;
        event.index = result.index;
        events.push_back(std::move(event));
    }

    bool requestBack(std::vector<UiEvent>& events) {
        UiEvent event;
        event.page = model.page();
        if (model.depth() > 1) {
            event.kind = UiEventKind::Back;
            events.push_back(std::move(event));
            return true;
        }
        event.kind = UiEventKind::Quit;
        events.push_back(std::move(event));
        return false;
    }

    void copyFocused(std::vector<UiEvent>& events) {
        const Item* item = model.focusedItem();
        std::string payload;
        if (item != nullptr) {
            payload = !item->copyText.empty()
                          ? item->copyText
                          : (item->value.empty() ? item->label : item->value);
        }
        if (payload.empty()) { toast("Nothing to copy here."); return; }
        std::string error;
        if (clipboardCopyUtf8(hwnd(), payload, error)) toast("Copied.");
        else toast(error, 4000);

        UiEvent event;
        event.kind = UiEventKind::CopyRequested;
        event.page = model.page();
        if (item != nullptr) {
            event.action = item->action;
            event.index = item->index;
        }
        event.text = payload;
        events.push_back(std::move(event));
    }

    void pasteToCaller(std::vector<UiEvent>& events) {
        std::string raw;
        std::string error;
        if (!clipboardPasteUtf8(hwnd(), raw, error)) { toast(error, 4000); return; }
        UiEvent event;
        event.kind = UiEventKind::PasteRequested;
        event.page = model.page();
        const Item* item = model.focusedItem();
        if (item != nullptr) {
            event.action = item->action;
            event.index = item->index;
        }
        event.text = sanitizeText(raw);
        events.push_back(std::move(event));
    }

    void pasteIntoEntry() {
        std::string raw;
        std::string error;
        if (!clipboardPasteUtf8(hwnd(), raw, error)) { toast(error, 4000); return; }
        entry.insert(raw);
        const TextRejection rejection = entry.lastRejection();
        if (rejection != TextRejection::None) toast(describe(rejection), 3000);
    }

    void endTextEntry() {
        if (!entryActive) return;
        entryActive = false;
        virtualKeyboard = false;
        vkShift = false;
        if (window != nullptr) SDL_StopTextInput(window);
        refreshWindowTitle();
    }

    void commitTextEntry(std::vector<UiEvent>& events) {
        UiEvent event;
        event.kind = UiEventKind::TextCommitted;
        event.page = model.page();
        event.action = entryRequest.commitAction;
        event.index = entryRequest.index;
        event.text = entry.text();
        events.push_back(std::move(event));
        endTextEntry();
    }

    void cancelTextEntry(std::vector<UiEvent>& events) {
        UiEvent event;
        event.kind = UiEventKind::TextCancelled;
        event.page = model.page();
        event.action = entryRequest.commitAction;
        event.index = entryRequest.index;
        events.push_back(std::move(event));
        endTextEntry();
    }

    static int virtualRowWidth(int row) {
        if (row < 0 || row > kVirtualActionRow) return 0;
        if (row == kVirtualActionRow) return kVirtualActionCount;
        return static_cast<int>(std::strlen(kVirtualRows[row]));
    }

    void moveVirtualKeyboard(int dx, int dy) {
        vkRow += dy;
        if (vkRow < 0) vkRow = kVirtualActionRow;
        if (vkRow > kVirtualActionRow) vkRow = 0;
        const int width = virtualRowWidth(vkRow);
        if (width <= 0) { vkColumn = 0; return; }
        vkColumn += dx;
        if (vkColumn < 0) vkColumn = width - 1;
        if (vkColumn >= width) vkColumn = 0;
    }

    void pressVirtualKey(std::vector<UiEvent>& events) {
        if (vkRow < kVirtualActionRow) {
            const char* row = kVirtualRows[vkRow];
            const int width = static_cast<int>(std::strlen(row));
            if (vkColumn < 0 || vkColumn >= width) return;
            char ch = row[vkColumn];
            if (vkShift && ch >= 'a' && ch <= 'z')
                ch = static_cast<char>(ch - 'a' + 'A');
            entry.insert(std::string(1, ch));
            return;
        }
        switch (vkColumn) {
            case 0: vkShift = !vkShift; return;
            case 1: entry.insert(" "); return;
            case 2: entry.backspace(); return;
            case 3: pasteIntoEntry(); return;
            case 4: commitTextEntry(events); return;
            default: cancelTextEntry(events); return;
        }
    }

    void clickVirtualKeyboard(float mouseX, float mouseY,
                              std::vector<UiEvent>& events) {
        if (vkBox.rowHeight <= 0.0f || vkBox.keyWidth <= 0.0f) return;
        const int row = static_cast<int>((mouseY - vkBox.y) / vkBox.rowHeight);
        if (row < 0 || row > kVirtualActionRow) return;
        vkRow = row;
        if (row < kVirtualActionRow) {
            const int column =
                static_cast<int>((mouseX - vkBox.x) / vkBox.keyWidth);
            if (column < 0 || column >= virtualRowWidth(row)) return;
            vkColumn = column;
        }
        pressVirtualKey(events);
    }

    int rowAtPoint(float mouseY) {
        const Layout box = layout();
        if (mouseY < box.listTop || mouseY > box.listBottom) return -1;
        const int row = static_cast<int>((mouseY - box.listTop) / box.rowHeight);
        const int index = scroll + row;
        const PageView& view = model.view();
        if (index < 0 || static_cast<std::size_t>(index) >= view.items.size())
            return -1;
        if (!view.items[static_cast<std::size_t>(index)].selectable()) return -1;
        return index;
    }

    bool handleEvent(const SDL_Event& event, std::vector<UiEvent>& events);
    bool handleKey(const SDL_KeyboardEvent& key, std::vector<UiEvent>& events);
    bool handleTextEntryKey(const SDL_KeyboardEvent& key,
                            std::vector<UiEvent>& events);
    bool handleMouseButton(const SDL_MouseButtonEvent& button,
                           std::vector<UiEvent>& events);
    bool handleGamepadButton(const SDL_GamepadButtonEvent& button,
                             std::vector<UiEvent>& events);

    void render();
    void renderList(const Layout& box);
    void renderDetail(const Layout& box);
    void renderChrome(const Layout& box);
    void renderTextEntry(const Layout& box);
    void renderMessage(const Layout& box);
    void wrap(const std::string& body, float maxWidth,
              std::vector<std::string>& out);
#endif
};

SdlUi::SdlUi() : _impl(new Impl()) {}
SdlUi::~SdlUi() { stop(); }

bool SdlUi::builtWithSdl() {
#ifdef QOH99_WITH_SDL3
    return true;
#else
    return false;
#endif
}

bool SdlUi::runtimeAvailable(std::string& why) {
#ifdef QOH99_WITH_SDL3
    // Plain LoadLibrary only: SDL3 is delay-loaded, so touching a symbol before this
    // succeeds kills the process with STATUS_ENTRYPOINT_NOT_FOUND.
    static int cached = -1;
    if (cached < 0) cached = LoadLibraryW(L"SDL3.dll") != nullptr ? 1 : 0;
    if (cached == 1) { why.clear(); return true; }
    why = "SDL3.dll could not be loaded from the caster directory";
    return false;
#else
    why = "this caster was built without the pinned SDL3 runtime";
    return false;
#endif
}

NavigationModel& SdlUi::model() { return _impl->model; }
const NavigationModel& SdlUi::model() const { return _impl->model; }
Page SdlUi::page() const { return _impl->model.page(); }
const RendererSelection& SdlUi::renderer() const { return _impl->renderer; }
const FontProbeResult& SdlUi::font() const { return _impl->font; }
bool SdlUi::running() const { return _impl->started; }
const char* SdlUi::backendName() const {
    return _impl->started ? "sdl3" : "unavailable";
}
bool SdlUi::textEntryActive() const { return _impl->entryActive; }
const TextEntry& SdlUi::textEntry() const { return _impl->entry; }
bool SdlUi::virtualKeyboardVisible() const { return _impl->virtualKeyboard; }
void SdlUi::setVirtualKeyboardVisible(bool visible) {
    _impl->virtualKeyboard = visible;
}
bool SdlUi::messageVisible() const { return _impl->messageVisible; }
void SdlUi::dismissMessage() {
    _impl->messageVisible = false;
    _impl->messageDetailsShown = false;
}
void SdlUi::setStatus(const std::string& utf8) {
    _impl->status = sanitizeText(utf8);
}
void SdlUi::showMessage(const std::string& title, const std::string& body,
                        const std::string& details) {
    TextSanitizeOptions multiline;
    multiline.singleLine = false;
    _impl->messageTitle = sanitizeText(title);
    _impl->messageBody = sanitizeText(body, multiline);
    _impl->messageDetails = sanitizeText(details, multiline);
    _impl->messageDetailsShown = false;
    _impl->messageVisible = true;
}
void* SdlUi::nativeWindow() const { return _impl->hwnd(); }

bool SdlUi::copyToClipboard(const std::string& utf8, std::string& error) {
    return clipboardCopyUtf8(nativeWindow(), utf8, error);
}
bool SdlUi::pasteFromClipboard(std::string& utf8, std::string& error) {
    return clipboardPasteUtf8(nativeWindow(), utf8, error);
}

std::string SdlUi::diagnosticsText() const {
    const Impl& impl = *_impl;
    std::string out;
    out += "frontend: ";
    out += backendName();
    out += "\n";
    if (!impl.startError.empty()) out += "start error: " + impl.startError + "\n";

#ifdef QOH99_WITH_SDL3
    if (impl.started) {
        char buffer[192];
        const int version = SDL_GetVersion();
        std::snprintf(buffer, sizeof(buffer),
                      "SDL runtime: %d.%d.%d (headers %d.%d.%d)\n",
                      version / 1000000, (version / 1000) % 1000,
                      version % 1000, SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                      SDL_MICRO_VERSION);
        out += buffer;
        std::snprintf(buffer, sizeof(buffer),
                      "window: %dx%d px, display scale %.2f\n",
                      impl.pixelWidth, impl.pixelHeight,
                      static_cast<double>(impl.displayScale));
        out += buffer;
    }
#else
    out += "SDL runtime: not built in\n";
#endif

    out += "renderer: ";
    out += rendererKindName(impl.renderer.kind);
    if (!impl.renderer.driver.empty()) out += " (" + impl.renderer.driver + ")";
    out += "\n";
    if (!impl.renderer.fallbackReason.empty())
        out += "renderer fallback reason: " + impl.renderer.fallbackReason + "\n";
    if (!impl.renderer.failureReason.empty())
        out += "renderer failure: " + impl.renderer.failureReason + "\n";
    for (const RendererAttempt& attempt : impl.renderer.attempts) {
        out += "  attempt ";
        out += rendererKindName(attempt.kind);
        if (attempt.succeeded) {
            out += " = ok";
            if (!attempt.driver.empty()) out += " (" + attempt.driver + ")";
        } else {
            out += " = failed: " + attempt.failure;
        }
        out += "\n";
    }

    if (!impl.font.note.empty()) out += "font: " + impl.font.note + "\n";
    out += std::string("japanese glyphs: ")
         + (impl.font.japaneseCapable ? "available" : "MISSING") + "\n";

#ifdef QOH99_WITH_SDL3
    out += std::string("gamepad navigation: ")
         + (impl.ownsGamepad
                ? "active"
                : impl.gamepadForeignOwner
                      ? "off (another owner holds SDL's gamepad subsystem)"
                      : "off")
         + "\n";
#endif
    return out;
}

#ifndef QOH99_WITH_SDL3

bool SdlUi::start(const SdlUiOptions& options, std::string& error) {
    _impl->options = options;
    error = "this caster was built without the pinned SDL3 runtime; "
            "using the console frontend";
    _impl->startError = error;
    _impl->renderer = RendererSelection{};
    _impl->renderer.failureReason = error;
    return false;
}
void SdlUi::stop() { _impl->started = false; }
void SdlUi::setPage(PageView view) { _impl->model.setView(std::move(view)); }
bool SdlUi::pump(int, std::vector<UiEvent>&) { return false; }
void SdlUi::setToast(const std::string&, int) {}
void SdlUi::beginTextEntry(const TextEntryRequest&) {}
void SdlUi::endTextEntry() {}
bool SdlUi::gamepadNavigationActive() const { return false; }
bool SdlUi::acquireGamepadNavigation() { return false; }
void SdlUi::releaseGamepadNavigation() {}

#else

bool SdlUi::start(const SdlUiOptions& options, std::string& error) {
    Impl& impl = *_impl;
    impl.options = options;
    error.clear();
    impl.startError.clear();

    std::string why;
    if (!runtimeAvailable(why)) {
        error = why;
        impl.startError = why;
        impl.renderer = RendererSelection{};
        impl.renderer.failureReason = why;
        return false;
    }

    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MENU_MNEMONICS, "0");
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");

    if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            error = std::string("SDL video could not start: ") + SDL_GetError();
            impl.startError = error;
            impl.renderer = RendererSelection{};
            impl.renderer.failureReason = error;
            return false;
        }
        impl.ownsVideo = true;
    }

    SDL_WindowFlags flags = 0;
    if (options.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (options.highDpi) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    impl.window = SDL_CreateWindow(options.windowTitle.c_str(), options.width,
                                   options.height, flags);
    if (impl.window == nullptr) {
        error = std::string("the caster window could not be created: ")
              + SDL_GetError();
        impl.startError = error;
        impl.renderer = RendererSelection{};
        impl.renderer.failureReason = error;
        stop();
        return false;
    }
    SDL_SetWindowMinimumSize(impl.window, 720, 460);

    SDL_Window* window = impl.window;
    SDL_Renderer** slot = &impl.sdlRenderer;
    impl.renderer = selectRenderer(
        [window, slot](RendererKind kind, std::string& driverOut,
                       std::string& failureOut) -> bool {
            if (kind == RendererKind::Software) {
                SDL_Renderer* created = SDL_CreateRenderer(window, "software");
                if (created == nullptr) {
                    failureOut = SDL_GetError();
                    return false;
                }
                *slot = created;
                driverOut = "software";
                return true;
            }
            const int drivers = SDL_GetNumRenderDrivers();
            if (drivers <= 0) {
                failureOut = "SDL reports no render drivers at all";
                return false;
            }
            std::string collected;
            for (int i = 0; i < drivers; ++i) {
                const char* name = SDL_GetRenderDriver(i);
                if (name == nullptr) continue;
                if (SDL_strcasecmp(name, "software") == 0) continue;
                SDL_Renderer* created = SDL_CreateRenderer(window, name);
                if (created != nullptr) {
                    *slot = created;
                    driverOut = name;
                    return true;
                }
                if (!collected.empty()) collected += "; ";
                collected += std::string(name) + ": " + SDL_GetError();
            }
            failureOut = collected.empty()
                             ? std::string("no accelerated render driver could "
                                           "be created")
                             : collected;
            return false;
        },
        options.allowSoftwareFallback);

    if (!impl.renderer.ok() || impl.sdlRenderer == nullptr) {
        error = "no SDL renderer could be created: "
              + (impl.renderer.failureReason.empty()
                     ? std::string("unknown reason")
                     : impl.renderer.failureReason);
        impl.startError = error;
        stop();
        return false;
    }
    SDL_SetRenderDrawBlendMode(impl.sdlRenderer, SDL_BLENDMODE_BLEND);

    impl.displayScale = SDL_GetWindowDisplayScale(impl.window);
    if (!(impl.displayScale > 0.1f) || impl.displayScale > 8.0f)
        impl.displayScale = 1.0f;
    SDL_GetWindowSizeInPixels(impl.window, &impl.pixelWidth, &impl.pixelHeight);

    const int fontPixels = static_cast<int>(
        std::lround(static_cast<double>(options.baseFontHeight)
                    * static_cast<double>(impl.displayScale)));
    if (!impl.text.create(fontPixels, options.preferredFontFace, impl.font)) {
        error = "the caster could not create a GDI font for the window";
        impl.startError = error;
        stop();
        return false;
    }

    if (options.gamepadNavigation) acquireGamepadNavigation();

    SDL_ShowWindow(impl.window);
    SDL_RaiseWindow(impl.window);
    impl.started = true;
    impl.refreshWindowTitle();
    return true;
}

void SdlUi::stop() {
    Impl& impl = *_impl;
    releaseGamepadNavigation();
    if (impl.sdlRenderer != nullptr) impl.clearTextCache();
    impl.text.destroy();
    if (impl.sdlRenderer != nullptr) {
        SDL_DestroyRenderer(impl.sdlRenderer);
        impl.sdlRenderer = nullptr;
    }
    if (impl.window != nullptr) {
        SDL_DestroyWindow(impl.window);
        impl.window = nullptr;
    }
    if (impl.ownsVideo) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        impl.ownsVideo = false;
    }
    impl.started = false;
}

bool SdlUi::acquireGamepadNavigation() {
    Impl& impl = *_impl;
    if (impl.ownsGamepad) return true;
    if ((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0) {
        impl.gamepadForeignOwner = true;
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
    impl.ownsGamepad = true;
    impl.gamepadForeignOwner = false;
    SDL_SetGamepadEventsEnabled(true);

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        for (int i = 0; i < count; ++i) {
            SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]);
            if (pad != nullptr) impl.gamepads.push_back(pad);
        }
        SDL_free(ids);
    }
    return true;
}

void SdlUi::releaseGamepadNavigation() {
    Impl& impl = *_impl;
    if (!impl.ownsGamepad) return;
    for (SDL_Gamepad* pad : impl.gamepads)
        if (pad != nullptr) SDL_CloseGamepad(pad);
    impl.gamepads.clear();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    impl.ownsGamepad = false;
    impl.axisDirection = 0;
}

bool SdlUi::gamepadNavigationActive() const { return _impl->ownsGamepad; }

void SdlUi::setPage(PageView view) {
    Impl& impl = *_impl;
    const Page previous = impl.model.view().page;
    impl.model.setView(std::move(view));
    if (impl.model.view().page != previous) impl.scroll = 0;
    impl.ensureFocusVisible();
    impl.refreshWindowTitle();
}

void SdlUi::setToast(const std::string& utf8, int milliseconds) {
    _impl->toast(utf8, milliseconds);
}

void SdlUi::beginTextEntry(const TextEntryRequest& request) {
    Impl& impl = *_impl;
    impl.entryRequest = request;
    impl.entry.setMaxBytes(request.maxBytes);
    impl.entry.setNumericOnly(request.numericOnly);
    impl.entry.setText(request.initial);
    impl.entryActive = true;
    impl.virtualKeyboard = request.showVirtualKeyboard;
    impl.vkShift = false;
    impl.vkRow = 0;
    impl.vkColumn = 0;
    if (impl.window != nullptr) SDL_StartTextInput(impl.window);
    impl.refreshWindowTitle();
}

void SdlUi::endTextEntry() { _impl->endTextEntry(); }

bool SdlUi::pump(int timeoutMs, std::vector<UiEvent>& events) {
    Impl& impl = *_impl;
    if (!impl.started || impl.sdlRenderer == nullptr) return false;

    bool alive = true;
    SDL_Event event;
    bool waited = false;
    for (;;) {
        bool got = false;
        if (!waited && timeoutMs > 0) {
            got = SDL_WaitEventTimeout(&event, timeoutMs);
            waited = true;
        } else {
            got = SDL_PollEvent(&event);
            waited = true;
        }
        if (!got) break;
        if (!impl.handleEvent(event, events)) alive = false;
    }

    if (impl.ownsGamepad && impl.axisDirection != 0) {
        const unsigned long long now = SDL_GetTicks();
        if (now >= impl.axisRepeatDueMs) {
            impl.axisRepeatDueMs = now + 110;
            impl.navigateBy(impl.axisDirection, events);
        }
    }

    if (!impl.toastText.empty() && SDL_GetTicks() > impl.toastUntilMs)
        impl.toastText.clear();

    impl.render();
    return alive;
}

bool SdlUi::Impl::handleEvent(const SDL_Event& event,
                              std::vector<UiEvent>& events) {
    switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
            UiEvent quit;
            quit.kind = UiEventKind::Quit;
            quit.page = model.page();
            events.push_back(std::move(quit));
            return false;
        }

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
            ensureFocusVisible();
            UiEvent resized;
            resized.kind = UiEventKind::Resized;
            resized.page = model.page();
            events.push_back(std::move(resized));
            return true;
        }

        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
            const float scale = SDL_GetWindowDisplayScale(window);
            if (scale > 0.1f && scale < 8.0f
                && std::fabs(scale - displayScale) > 0.01f) {
                displayScale = scale;
                rebuildFont();
            }
            return true;
        }

        case SDL_EVENT_TEXT_INPUT:
            if (entryActive && event.text.text != nullptr)
                entry.insert(event.text.text);
            return true;

        case SDL_EVENT_KEY_DOWN:
            return handleKey(event.key, events);

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            return handleMouseButton(event.button, events);

        case SDL_EVENT_MOUSE_MOTION: {
            if (messageVisible || entryActive) return true;
            const int row = rowAtPoint(event.motion.y);
            if (row >= 0 && row != model.focus() && model.focusIndexRaw(row))
                announceFocus(events);
            return true;
        }

        case SDL_EVENT_MOUSE_WHEEL:
            if (event.wheel.y != 0.0f) {
                scroll -= static_cast<int>(event.wheel.y);
                if (scroll < 0) scroll = 0;
            }
            return true;

        case SDL_EVENT_GAMEPAD_ADDED: {
            if (!ownsGamepad) return true;
            SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which);
            if (pad != nullptr) gamepads.push_back(pad);
            return true;
        }
        case SDL_EVENT_GAMEPAD_REMOVED:
            return true;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            return handleGamepadButton(event.gbutton, events);

        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            if (event.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) return true;
            const int direction = event.gaxis.value > 18000    ? 1
                                : event.gaxis.value < -18000   ? -1
                                                               : 0;
            if (direction != axisDirection) {
                axisDirection = direction;
                if (direction != 0) {
                    navigateBy(direction, events);
                    axisRepeatDueMs = SDL_GetTicks() + 380;
                }
            }
            return true;
        }

        default:
            return true;
    }
}

bool SdlUi::Impl::handleKey(const SDL_KeyboardEvent& key,
                            std::vector<UiEvent>& events) {
    const bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;
    const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;

    if (messageVisible) {
        if (key.key == SDLK_D && !messageDetails.empty()) {
            messageDetailsShown = !messageDetailsShown;
            return true;
        }
        if (ctrl && key.key == SDLK_C) {
            std::string body = messageTitle + "\n" + messageBody;
            if (!messageDetails.empty()) body += "\n" + messageDetails;
            std::string error;
            if (clipboardCopyUtf8(hwnd(), body, error)) toast("Copied.");
            else toast(error, 4000);
            return true;
        }
        messageVisible = false;
        messageDetailsShown = false;
        refreshWindowTitle();
        return true;
    }

    if (entryActive) return handleTextEntryKey(key, events);

    switch (key.key) {
        case SDLK_UP:       navigateBy(-1, events); return true;
        case SDLK_DOWN:     navigateBy(1, events); return true;
        case SDLK_TAB:      navigateBy(shift ? -1 : 1, events); return true;
        case SDLK_PAGEUP:   navigateBy(-5, events); return true;
        case SDLK_PAGEDOWN: navigateBy(5, events); return true;
        case SDLK_HOME:
            if (model.focusFirst()) announceFocus(events);
            return true;
        case SDLK_END:
            if (model.focusLast()) announceFocus(events);
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            activateFocused(events);
            return true;
        case SDLK_ESCAPE:
            return requestBack(events);
        case SDLK_F5: {
            UiEvent refresh;
            refresh.kind = UiEventKind::Refresh;
            refresh.page = model.page();
            events.push_back(std::move(refresh));
            return true;
        }
        case SDLK_LEFT:
        case SDLK_RIGHT: {
            const Item* item = model.focusedItem();
            if (item == nullptr) return true;
            if (item->kind != ItemKind::Toggle && item->kind != ItemKind::Value)
                return true;
            if (!item->enabled) { reject(events); return true; }
            UiEvent nudge;
            nudge.kind = UiEventKind::Activated;
            nudge.page = model.page();
            nudge.action = item->action;
            nudge.index = item->index;
            nudge.detail = key.key == SDLK_LEFT ? "left" : "right";
            events.push_back(std::move(nudge));
            return true;
        }
        case SDLK_C:
            if (ctrl) copyFocused(events);
            return true;
        case SDLK_V:
            if (ctrl) pasteToCaller(events);
            return true;
        default:
            return true;
    }
}

bool SdlUi::Impl::handleTextEntryKey(const SDL_KeyboardEvent& key,
                                     std::vector<UiEvent>& events) {
    const bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;

    if (virtualKeyboard) {
        switch (key.key) {
            case SDLK_UP:    moveVirtualKeyboard(0, -1); return true;
            case SDLK_DOWN:  moveVirtualKeyboard(0, 1); return true;
            default: break;
        }
    }

    switch (key.key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (virtualKeyboard) pressVirtualKey(events);
            else commitTextEntry(events);
            return true;
        case SDLK_ESCAPE:    cancelTextEntry(events); return true;
        case SDLK_BACKSPACE: entry.backspace(); return true;
        case SDLK_DELETE:    entry.deleteForward(); return true;
        case SDLK_LEFT:
            if (virtualKeyboard) moveVirtualKeyboard(-1, 0);
            else entry.moveLeft();
            return true;
        case SDLK_RIGHT:
            if (virtualKeyboard) moveVirtualKeyboard(1, 0);
            else entry.moveRight();
            return true;
        case SDLK_HOME: entry.moveHome(); return true;
        case SDLK_END:  entry.moveEnd(); return true;
        case SDLK_F2:
            virtualKeyboard = !virtualKeyboard;
            return true;
        case SDLK_V:
            if (ctrl) pasteIntoEntry();
            return true;
        case SDLK_C:
            if (ctrl) {
                std::string error;
                if (clipboardCopyUtf8(hwnd(), entry.text(), error))
                    toast("Copied.");
                else
                    toast(error, 4000);
            }
            return true;
        default:
            return true;
    }
}

bool SdlUi::Impl::handleMouseButton(const SDL_MouseButtonEvent& button,
                                    std::vector<UiEvent>& events) {
    if (!button.down) return true;

    if (button.button == SDL_BUTTON_RIGHT) {
        if (entryActive) pasteIntoEntry();
        else pasteToCaller(events);
        return true;
    }
    if (button.button != SDL_BUTTON_LEFT) return true;

    if (messageVisible) {
        messageVisible = false;
        messageDetailsShown = false;
        refreshWindowTitle();
        return true;
    }
    if (entryActive) {
        if (virtualKeyboard) clickVirtualKeyboard(button.x, button.y, events);
        return true;
    }

    const int row = rowAtPoint(button.y);
    if (row < 0) return true;
    if (!model.focusIndexRaw(row)) return true;
    announceFocus(events);
    activateFocused(events);
    return true;
}

bool SdlUi::Impl::handleGamepadButton(const SDL_GamepadButtonEvent& button,
                                      std::vector<UiEvent>& events) {
    if (!button.down) return true;
    switch (button.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            if (entryActive && virtualKeyboard) moveVirtualKeyboard(0, -1);
            else navigateBy(-1, events);
            return true;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            if (entryActive && virtualKeyboard) moveVirtualKeyboard(0, 1);
            else navigateBy(1, events);
            return true;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            if (entryActive && virtualKeyboard) moveVirtualKeyboard(-1, 0);
            else if (entryActive) entry.moveLeft();
            return true;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            if (entryActive && virtualKeyboard) moveVirtualKeyboard(1, 0);
            else if (entryActive) entry.moveRight();
            return true;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            if (messageVisible) {
                messageVisible = false;
                messageDetailsShown = false;
                refreshWindowTitle();
                return true;
            }
            if (entryActive) {
                if (virtualKeyboard) pressVirtualKey(events);
                else commitTextEntry(events);
                return true;
            }
            activateFocused(events);
            return true;
        case SDL_GAMEPAD_BUTTON_EAST:
            if (messageVisible) {
                messageVisible = false;
                messageDetailsShown = false;
                refreshWindowTitle();
                return true;
            }
            if (entryActive) { cancelTextEntry(events); return true; }
            return requestBack(events);
        case SDL_GAMEPAD_BUTTON_WEST:
            if (entryActive) pasteIntoEntry();
            else pasteToCaller(events);
            return true;
        case SDL_GAMEPAD_BUTTON_NORTH:
            if (entryActive) virtualKeyboard = !virtualKeyboard;
            else copyFocused(events);
            return true;
        case SDL_GAMEPAD_BUTTON_START:
            if (entryActive) { commitTextEntry(events); return true; }
            activateFocused(events);
            return true;
        default:
            return true;
    }
}

void SdlUi::Impl::render() {
    if (fontLanguage != qi18n::language()) rebuildFont();
    if (sdlRenderer == nullptr) return;
    ++frame;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
    const Layout box = layout();

    SDL_SetRenderDrawColor(sdlRenderer, kBackground.r, kBackground.g,
                           kBackground.b, 255);
    SDL_RenderClear(sdlRenderer);

    const PageView& view = model.view();
    float y = box.pad;
    const float bandHeight = box.pad * 2.0f + box.lineHeight
                           + static_cast<float>(view.banner.size())
                                 * box.lineHeight;
    fillRect(sdlRenderer, 0.0f, 0.0f, static_cast<float>(pixelWidth),
             bandHeight, kHeaderBand);
    draw(box.pad, y, view.title.empty() ? pageTitle(view.page) : view.title,
         Tone::Title);
    {
        std::string identity = std::string("QOH99 Caster ") + kCasterVersion;
        const std::string& edition = detectedGameEdition();
        if (!edition.empty()) identity += "  -  " + edition;
        const float right = static_cast<float>(pixelWidth) - box.pad
                          - static_cast<float>(measure(identity));
        draw(right, y, identity, Tone::Dim);
    }
    y += box.lineHeight * 1.5f;
    for (const std::string& line : view.banner) {
        draw(box.pad, y, line, Tone::Dim);
        y += box.lineHeight;
    }
    fillRect(sdlRenderer, 0.0f, bandHeight - 1.0f,
             static_cast<float>(pixelWidth), 1.0f, kHeaderRule);

    renderList(box);
    if (view.showDetail) renderDetail(box);
    renderChrome(box);
    if (entryActive) renderTextEntry(box);
    if (messageVisible) renderMessage(box);

    SDL_RenderPresent(sdlRenderer);
    if ((frame & 0xFF) == 0) evictTextCache();
}

void SdlUi::Impl::renderList(const Layout& box) {
    const PageView& view = model.view();
    const float listWidth = box.listRight - box.listLeft;
    const float labelWidth = listWidth * (view.showDetail ? 0.60f : 0.52f);
    const int total = static_cast<int>(view.items.size());

    if (scroll > total - box.visibleRows) scroll = total - box.visibleRows;
    if (scroll < 0) scroll = 0;

    for (int row = 0; row < box.visibleRows; ++row) {
        const int index = scroll + row;
        if (index >= total) break;
        const Item& item = view.items[static_cast<std::size_t>(index)];
        const float rowY = box.listTop + static_cast<float>(row) * box.rowHeight;
        const bool focused = index == model.focus();

        if (focused) {
            fillRect(sdlRenderer, box.listLeft - 6.0f, rowY - 3.0f,
                     listWidth + 12.0f, box.rowHeight - 2.0f, kSelection);
            fillRect(sdlRenderer, box.listLeft - 6.0f, rowY - 2.0f, 4.0f,
                     box.rowHeight - 4.0f, kSelectionBar);
        }
        if (item.kind == ItemKind::Spacer) continue;

        Tone labelTone = item.labelTone;
        if (item.kind == ItemKind::Header) labelTone = Tone::Accent;
        else if (!item.enabled) labelTone = Tone::Dim;
        else if (focused && labelTone == Tone::Default) labelTone = Tone::Title;

        const float indent = item.kind == ItemKind::Header ? 0.0f
                                                           : box.lineHeight;
        draw(box.listLeft + indent, rowY, item.label, labelTone,
             labelWidth - indent);

        float x = box.listLeft + labelWidth + box.lineHeight * 0.5f;
        if (!item.value.empty()) {
            x += static_cast<float>(
                     draw(x, rowY, item.value,
                          item.enabled ? item.valueTone : Tone::Dim,
                          box.listRight - x))
               + box.lineHeight * 0.5f;
        }
        if (!item.badge.empty()) {
            draw(x, rowY, "[" + item.badge.text + "]",
                 item.enabled ? item.badge.tone : Tone::Dim, box.listRight - x);
        }
        if (!item.enabled && focused) {
            std::string why = std::string("- ") + describe(item.reason);
            if (!item.reasonDetail.empty()) why += " - " + item.reasonDetail;
            draw(box.listLeft + indent, rowY + box.lineHeight * 0.95f, why,
                 Tone::Warn, listWidth - indent);
        }
    }

    if (total > box.visibleRows) {
        char counter[64];
        std::snprintf(counter, sizeof(counter), "%d-%d of %d", scroll + 1,
                      std::min(scroll + box.visibleRows, total), total);
        draw(box.listRight - static_cast<float>(measure(counter)),
             box.listTop - box.lineHeight * 1.15f, counter, Tone::Dim);
    }
}

void SdlUi::Impl::renderDetail(const Layout& box) {
    const PageView& view = model.view();
    const float panelX = box.detailLeft - box.pad * 0.4f;
    const float panelY = box.listTop - box.lineHeight * 0.6f;
    const float panelW = box.detailRight - box.detailLeft + box.pad * 0.8f;
    const float panelH = box.listBottom - box.listTop + box.lineHeight;
    fillRect(sdlRenderer, panelX, panelY, panelW, panelH, kPanel);
    strokeRect(sdlRenderer, panelX, panelY, panelW, panelH, kPanelEdge);

    float y = box.listTop;
    draw(box.detailLeft, y,
         view.detail.title.empty() ? std::string("Details") : view.detail.title,
         Tone::Accent, box.detailRight - box.detailLeft);
    y += box.lineHeight * 1.4f;

    if (view.detail.lines.empty()) {
        draw(box.detailLeft, y, view.detail.emptyText, Tone::Dim,
             box.detailRight - box.detailLeft);
        return;
    }
    for (const DetailLine& line : view.detail.lines) {
        if (y > box.listBottom - box.lineHeight) break;
        float x = box.detailLeft;
        if (!line.label.empty()) {
            x += static_cast<float>(draw(x, y, line.label, Tone::Dim,
                                         box.detailRight - x))
               + box.lineHeight * 0.4f;
        }
        draw(x, y, line.value, line.tone, box.detailRight - x);
        y += box.lineHeight;
    }
}

void SdlUi::Impl::renderChrome(const Layout& box) {
    const PageView& view = model.view();
    float footerY = static_cast<float>(pixelHeight) - box.pad
                  - static_cast<float>(view.footer.size()) * box.lineHeight;

    const std::string& line = view.status.empty() ? status : view.status;
    if (!line.empty())
        draw(box.pad, footerY - box.lineHeight * 1.3f, line, Tone::Accent);
    for (const std::string& hint : view.footer) {
        draw(box.pad, footerY, hint, Tone::Dim);
        footerY += box.lineHeight;
    }

    if (toastText.empty()) return;
    const int toastWidth = measure(toastText);
    const float x = static_cast<float>(pixelWidth) - box.pad
                  - static_cast<float>(toastWidth) - box.lineHeight;
    const float y = box.pad * 0.35f;
    fillRect(sdlRenderer, x - box.lineHeight * 0.5f, y - 4.0f,
             static_cast<float>(toastWidth) + box.lineHeight,
             box.lineHeight + 8.0f, kPanel);
    draw(x, y, toastText, Tone::Warn);
}

void SdlUi::Impl::renderTextEntry(const Layout& box) {
    fillRect(sdlRenderer, 0.0f, 0.0f, static_cast<float>(pixelWidth),
             static_cast<float>(pixelHeight), kOverlay);

    const float boxWidth =
        std::min(static_cast<float>(pixelWidth) - box.pad * 2.0f,
                 box.lineHeight * 34.0f);
    const float rows = virtualKeyboard ? 14.0f : 5.5f;
    const float boxHeight = box.lineHeight * rows;
    const float x = (static_cast<float>(pixelWidth) - boxWidth) * 0.5f;
    const float y = (static_cast<float>(pixelHeight) - boxHeight) * 0.35f;

    fillRect(sdlRenderer, x, y, boxWidth, boxHeight, kPanel);
    strokeRect(sdlRenderer, x, y, boxWidth, boxHeight, kPanelEdge);
    fillRect(sdlRenderer, x, y, boxWidth, 2.0f, kSelectionBar);

    const float inner = boxWidth - box.pad * 2.0f;
    float cursorY = y + box.lineHeight * 0.7f;
    draw(x + box.pad, cursorY,
         entryRequest.title.empty() ? entryRequest.prompt : entryRequest.title,
         Tone::Title, inner);
    cursorY += box.lineHeight * 1.3f;
    if (!entryRequest.prompt.empty() && !entryRequest.title.empty()) {
        draw(x + box.pad, cursorY, entryRequest.prompt, Tone::Dim, inner);
        cursorY += box.lineHeight;
    }

    const float fieldY = cursorY + box.lineHeight * 0.3f;
    fillRect(sdlRenderer, x + box.pad, fieldY - 3.0f, inner,
             box.lineHeight + 6.0f, kBackground);
    draw(x + box.pad + 4.0f, fieldY, entry.text(), Tone::Default, inner - 8.0f);

    const std::string before = entry.text().substr(0, entry.caretBytes());
    const float caretX = x + box.pad + 4.0f
                       + static_cast<float>(measure(before));
    fillRect(sdlRenderer, caretX, fieldY - 1.0f, 2.0f, box.lineHeight + 2.0f,
             kSelectionBar);

    const SDL_Rect area{ static_cast<int>(x + box.pad),
                         static_cast<int>(fieldY), static_cast<int>(inner),
                         static_cast<int>(box.lineHeight) };
    SDL_SetTextInputArea(window, &area,
                         static_cast<int>(caretX - (x + box.pad)));

    float hintY = fieldY + box.lineHeight * 1.5f;
    const TextRejection rejection = entry.lastRejection();
    if (rejection != TextRejection::None) {
        draw(x + box.pad, hintY, describe(rejection), Tone::Warn, inner);
        hintY += box.lineHeight;
    }
    draw(x + box.pad, hintY,
         entryRequest.hint.empty()
             ? std::string("Enter accepts   Esc cancels   Ctrl+V pastes   "
                           "F2 on-screen keyboard")
             : entryRequest.hint,
         Tone::Dim, inner);

    if (!virtualKeyboard) {
        vkBox = VirtualKeyboardBox{};
        return;
    }

    const float keyWidth = box.lineHeight * 1.8f;
    const float rowHeight = box.lineHeight * 1.5f;
    float keyboardY = hintY + box.lineHeight * 1.4f;
    vkBox.x = x + box.pad;
    vkBox.y = keyboardY;
    vkBox.keyWidth = keyWidth;
    vkBox.rowHeight = rowHeight;

    for (int row = 0; row <= kVirtualActionRow; ++row) {
        float keyX = x + box.pad;
        if (row < kVirtualActionRow) {
            const char* keys = kVirtualRows[row];
            for (int column = 0; keys[column] != '\0'; ++column) {
                if (row == vkRow && column == vkColumn) {
                    fillRect(sdlRenderer, keyX - 3.0f, keyboardY - 3.0f,
                             keyWidth, box.lineHeight + 6.0f, kSelection);
                }
                char label[2] = { keys[column], '\0' };
                if (vkShift && label[0] >= 'a' && label[0] <= 'z')
                    label[0] = static_cast<char>(label[0] - 'a' + 'A');
                draw(keyX, keyboardY, label, Tone::Default);
                keyX += keyWidth;
            }
        } else {
            for (int column = 0; column < kVirtualActionCount; ++column) {
                const std::string label = kVirtualActions[column];
                const float width = static_cast<float>(measure(label))
                                  + box.lineHeight;
                if (row == vkRow && column == vkColumn) {
                    fillRect(sdlRenderer, keyX - 3.0f, keyboardY - 3.0f, width,
                             box.lineHeight + 6.0f, kSelection);
                }
                draw(keyX, keyboardY, label,
                     column == 0 && vkShift ? Tone::Good : Tone::Default);
                keyX += width + box.lineHeight * 0.4f;
            }
        }
        keyboardY += rowHeight;
    }
}

void SdlUi::Impl::renderMessage(const Layout& box) {
    fillRect(sdlRenderer, 0.0f, 0.0f, static_cast<float>(pixelWidth),
             static_cast<float>(pixelHeight), kOverlay);

    const float boxWidth =
        std::min(static_cast<float>(pixelWidth) - box.pad * 2.0f,
                 box.lineHeight * 40.0f);
    const float inner = boxWidth - box.pad * 2.0f;

    std::vector<std::string> lines;
    wrap(messageBody, inner, lines);
    if (messageDetailsShown && !messageDetails.empty()) {
        lines.push_back(std::string());
        wrap(messageDetails, inner, lines);
    }
    const float boxHeight = box.lineHeight
                          * (static_cast<float>(lines.size()) + 4.5f);
    const float x = (static_cast<float>(pixelWidth) - boxWidth) * 0.5f;
    const float y = (static_cast<float>(pixelHeight) - boxHeight) * 0.35f;

    fillRect(sdlRenderer, x, y, boxWidth, boxHeight, kPanel);
    strokeRect(sdlRenderer, x, y, boxWidth, boxHeight, kPanelEdge);
    fillRect(sdlRenderer, x, y, boxWidth, 2.0f, kSelectionBar);

    float cursorY = y + box.lineHeight * 0.7f;
    draw(x + box.pad, cursorY, messageTitle, Tone::Title, inner);
    fillRect(sdlRenderer, x + box.pad, cursorY + box.lineHeight * 1.05f,
             inner, 1.0f, kSeparator);
    cursorY += box.lineHeight * 1.4f;
    for (const std::string& line : lines) {
        draw(x + box.pad, cursorY, line, Tone::Default, inner);
        cursorY += box.lineHeight;
    }
    cursorY += box.lineHeight * 0.5f;
    draw(x + box.pad, cursorY,
         messageDetails.empty()
             ? std::string("Any key closes   Ctrl+C copies")
             : std::string("Any key closes   D shows details   Ctrl+C copies"),
         Tone::Dim, inner);
}

void SdlUi::Impl::wrap(const std::string& body, float maxWidth,
                       std::vector<std::string>& out) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t newline = body.find('\n', start);
        const std::string paragraph =
            body.substr(start, newline == std::string::npos
                                   ? std::string::npos
                                   : newline - start);
        std::string line;
        std::size_t word = 0;
        while (word < paragraph.size()) {
            std::size_t space = paragraph.find(' ', word);
            if (space == std::string::npos) space = paragraph.size();
            const std::string token = paragraph.substr(word, space - word);
            const std::string candidate = line.empty() ? token
                                                       : line + " " + token;
            if (!line.empty()
                && static_cast<float>(measure(candidate)) > maxWidth) {
                out.push_back(line);
                line = token;
            } else {
                line = candidate;
            }
            word = space + 1;
        }
        out.push_back(line);
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
}

#endif

}
}
