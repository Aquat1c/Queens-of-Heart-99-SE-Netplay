#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "ddraw_log.h"
#include "ddraw_pe.h"
#include "render_dx9_imgui.h"
#include "qoh99_instrument.h"
#include "qoh99_movie.h"
#include "qoh99_presentation_policy.h"
#include "surface_ops.h"
#include "surface_gdi_bridge.h"

#define DD_OK                      0x00000000L
#define DDERR_GENERIC              0x80004005L
#define DDERR_UNSUPPORTED          0x80004001L
#define DDERR_INVALIDPARAMS        0x80070057L
#define DDERR_OUTOFMEMORY          0x8007000EL
#define DDERR_SURFACELOST          0x887601C2L
#define DDERR_WASSTILLDRAWING      0x887601C4L
#define DDERR_NOTLOCKED            0x887601C8L
#define DDERR_ALREADYLOCKED        0x887601CAL
#define DDERR_SURFACEBUSY           0x887601AEL
#define DDERR_DCALREADYCREATED      0x8876026CL

#define DDSCAPS_BACKBUFFER         0x00000004
#define DDSCAPS_COMPLEX            0x00000008
#define DDSCAPS_FLIP               0x00000010
#define DDSCAPS_OFFSCREENPLAIN     0x00000040
#define DDSCAPS_PRIMARYSURFACE     0x00000200
#define DDSCAPS_SYSTEMMEMORY       0x00000800
#define DDSCAPS_VIDEOMEMORY        0x00004000

#define DDSD_CAPS                  0x00000001
#define DDSD_HEIGHT                0x00000002
#define DDSD_WIDTH                 0x00000004
#define DDSD_PITCH                 0x00000008
#define DDSD_BACKBUFFERCOUNT       0x00000020
#define DDSD_LPSURFACE             0x00000800
#define DDSD_PIXELFORMAT           0x00001000

#define DDPF_ALPHAPIXELS           0x00000001
#define DDPF_PALETTEINDEXED8       0x00000020
#define DDPF_RGB                   0x00000040

#define DDLOCK_WAIT                0x00000001
#define DDLOCK_READONLY            0x00000010
#define DDLOCK_WRITEONLY           0x00000020
#define DDLOCK_NOSYSLOCK           0x00000800

#define DDSCL_FULLSCREEN           0x00000001
#define DDSCL_ALLOWMODEX           0x00000004
#define DDSCL_NORMAL               0x00000008
#define DDSCL_EXCLUSIVE            0x00000010
#define DDSCL_ALLOWREBOOT          0x00000002

#define DDPCAPS_8BIT               0x00000004
#define DDPCAPS_ALLOW256           0x00000040

#define DDBLTFAST_NOCOLORKEY       0x00000000
#define DDBLTFAST_SRCCOLORKEY      0x00000001
#define DDBLTFAST_DESTCOLORKEY     0x00000002
#define DDBLTFAST_WAIT             0x00000010

#define DDBLT_COLORFILL            0x00000400
#define DDBLT_KEYSRC               0x00008000
#define DDBLT_KEYDEST              0x00004000
#define DDBLT_WAIT                 0x01000000

#define DDCKEY_COLORSPACE          0x00000001
#define DDCKEY_DESTBLT             0x00000002
#define DDCKEY_DESTOVERLAY         0x00000004
#define DDCKEY_SRCBLT              0x00000008
#define DDCKEY_SRCOVERLAY          0x00000010

#pragma pack(push, 4)

typedef struct { DWORD dwLow; DWORD dwHigh; } MY_DDCOLORKEY;

typedef struct {          /* 32 bytes, matches DDPIXELFORMAT */
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwRGBAlphaBitMask;
} MY_DDPIXELFORMAT;

typedef struct { DWORD dwCaps; } MY_DDSCAPS;

typedef struct {                      /* total = 108 bytes */
    DWORD           dwSize;
    DWORD           dwFlags;
    DWORD           dwHeight;
    DWORD           dwWidth;
    LONG            lPitch;
    DWORD           dwBackBufferCount;
    DWORD           dwMipMapCount;
    DWORD           dwAlphaBitDepth;
    DWORD           dwReserved;
    DWORD           lpSurface;
    MY_DDCOLORKEY   ddckCKDestOverlay;
    MY_DDCOLORKEY   ddckCKDestBlt;
    MY_DDCOLORKEY   ddckCKSrcOverlay;
    MY_DDCOLORKEY   ddckCKSrcBlt;
    MY_DDPIXELFORMAT ddpfPixelFormat;
    MY_DDSCAPS      ddsCaps;
} MY_DDSURFACEDESC;

#pragma pack(pop)

typedef char _ASSERT_DDSURFACEDESC_SIZE[(sizeof(MY_DDSURFACEDESC) == 108) ? 1 : -1];

static const GUID QOH_IID_IUnknown =
    { 0x00000000, 0x0000, 0x0000, { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const GUID QOH_IID_IDirectDraw =
    { 0x6C14DB80, 0xA733, 0x11CE, { 0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60 } };
static const GUID QOH_IID_IDirectDraw2 =
    { 0xB3A6F3E0, 0x2B43, 0x11CF, { 0xA2,0xDE,0x00,0xAA,0x00,0xB9,0x33,0x56 } };

static BOOL guid_equal(const GUID *a, const GUID *b)
    { return a && b && memcmp(a, b, sizeof(GUID)) == 0; }

typedef struct MyDDraw     MyDDraw;
typedef struct MyDDSurface MyDDSurface;
typedef struct MyDDPalette MyDDPalette;
typedef struct MyDDClipper MyDDClipper;

struct MyDDPalette {
    void   *lpVtbl;
    LONG    refCount;
    RGBQUAD entries[256];
};

struct MyDDSurface {
    void        *lpVtbl;
    LONG         refCount;
    int          width, height, pitch;
    int          bytesPP;
    DdwSurfaceFormat format;
    BYTE        *pixels;
    size_t       pixelBytes;
    BOOL         isPrimary;
    BOOL         isBackBuffer;
    MyDDSurface *attachedBackBuffer;
    MyDDPalette *palette;
    MyDDraw     *owner;
    BOOL         locked;
    BYTE        *lockedPointer;
    DdwGdiBridge gdiBridge;
    DWORD        dcPresentationDisposition;
    BOOL         gdiEffectDiagnosticPending;
    int          gdiEffectDiagnosticX;
    int          gdiEffectDiagnosticY;
    uint16_t     gdiEffectDiagnosticSourceWord;
    COLORREF     gdiEffectDiagnosticColor;
    LONG         gdiEffectDiagnosticPenWidth;
    RECT         gdiEffectDiagnosticBounds;
    BOOL         hasColorKeySrc;
    DWORD        colorKeySrcLow, colorKeySrcHigh;
};

typedef BOOL (WINAPI *Qoh99EllipseFn)(HDC, int, int, int, int);
static BOOL           g_gdiEffectDiagnosticEnabled = FALSE;
static LONG           g_gdiEffectDiagnosticBudget = 0;
static MyDDSurface   *g_gdiEffectDiagnosticSurface = NULL;
static Qoh99EllipseFn g_realEllipse = NULL;

static void gdi_effect_diagnostic_note_ellipse(
    HDC dc,
    int left,
    int top,
    int right,
    int bottom);

struct MyDDraw {
    void       *lpVtbl;
    LONG        refCount;
    HWND        hWnd;
    WNDPROC     origWndProc;
    int         gameWidth, gameHeight;
    int         modeBpp;
    QohPresentationMode presentationMode;
    QohPresentationMode lastFullscreenMode;
    RECT        savedWinRect;
    LONG        savedStyle, savedExStyle;
    BOOL        hasSavedWinRect;
    MyDDSurface *primarySurface;
    DWORD       nextSurfaceFormatGeneration;
};

/* QOH99 SE's asset loader only works at 16bpp, so everything the game sees defaults to RGB565. */
#define QOH_DEFAULT_BPP 16

static void fill_pixel_format(
    MY_DDPIXELFORMAT *pf,
    const DdwSurfaceFormat *format);

static DWORD next_surface_format_generation(MyDDraw *dd)
{
    DWORD generation;

    if (!dd)
        return 1u;
    generation = ++dd->nextSurfaceFormatGeneration;
    if (generation == 0u)
        generation = ++dd->nextSurfaceFormatGeneration;
    return generation;
}

static DdwSurfaceFormat default_surface_format(MyDDraw *dd)
{
    const DWORD generation = next_surface_format_generation(dd);
    return dd && dd->modeBpp == 8
        ? ddw_surface_format_index8(generation)
        : ddw_surface_format_rgb565(generation);
}

static int surface_format_from_desc(
    MyDDraw *dd,
    const MY_DDSURFACEDESC *desc,
    DdwSurfaceFormat *out_format)
{
    const MY_DDPIXELFORMAT *pf;
    DdwSurfaceFormat format;

    if (!out_format)
        return 0;
    if (!desc || (desc->dwFlags & DDSD_PIXELFORMAT) == 0u) {
        *out_format = default_surface_format(dd);
        return 1;
    }
    pf = &desc->ddpfPixelFormat;
    if (pf->dwSize == 0u && pf->dwFlags == 0u && pf->dwFourCC == 0u
        && pf->dwRGBBitCount == 0u && pf->dwRBitMask == 0u
        && pf->dwGBitMask == 0u && pf->dwBBitMask == 0u
        && pf->dwRGBAlphaBitMask == 0u) {
        *out_format = default_surface_format(dd);
        return 1;
    }
    if (pf->dwSize != 0u && pf->dwSize < sizeof(*pf))
        return 0;
    if ((pf->dwFlags & DDPF_PALETTEINDEXED8) != 0u) {
        if (pf->dwRGBBitCount != 8u || pf->dwRBitMask != 0u
            || pf->dwGBitMask != 0u || pf->dwBBitMask != 0u
            || pf->dwRGBAlphaBitMask != 0u) {
            return 0;
        }
        *out_format = ddw_surface_format_index8(next_surface_format_generation(dd));
        return 1;
    }
    if ((pf->dwFlags & DDPF_RGB) == 0u || pf->dwRGBBitCount != 16u)
        return 0;
    format = ddw_surface_format_from_masks(
        pf->dwRGBBitCount,
        pf->dwRBitMask,
        pf->dwGBitMask,
        pf->dwBBitMask,
        pf->dwRGBAlphaBitMask,
        next_surface_format_generation(dd));
    if (!ddw_surface_format_is_presentable(&format)
        || format.kind == DDW_PIXEL_FORMAT_INDEX8) {
        return 0;
    }
    *out_format = format;
    return 1;
}

struct MyDDClipper {
    void  *lpVtbl;
    LONG   refCount;
    HWND   hWnd;
};

typedef struct EfzDdrawOverlayFrame {
    DWORD         structSize;
    BYTE         *pixels;
    int           width;
    int           height;
    int           pitch;
    HWND          hWnd;
    const RGBQUAD *palette;
    DWORD         presentSerial;
} EfzDdrawOverlayFrame;

typedef void (__stdcall *EfzDdrawOverlayCallback)(
    const EfzDdrawOverlayFrame *frame,
    void *userData);

static MyDDraw         *g_dd        = NULL;
static HINSTANCE        g_hInst     = NULL;
static HHOOK            g_kbHook    = NULL;
static BOOL             g_appActive = TRUE;
static BOOL             g_dinputHooked = FALSE;
static BOOL             g_hasEverFlipped = FALSE;

#define LOG(...) \
    do { if (qoh99_wlog_should()) qoh99_wlog_write(__VA_ARGS__); } while (0)
#define LOGV(...) \
    do { if (qoh99_wlog_should_verbose()) qoh99_wlog_write_verbose(__VA_ARGS__); } while (0)

#define DIERR_INPUTLOST          0x8007001EL
#define DIERR_NOTACQUIRED        0x8007000CL
#define DISCL_EXCLUSIVE_DI       0x00000001
#define DISCL_NONEXCLUSIVE_DI    0x00000002
#define DISCL_FOREGROUND_DI      0x00000004
#define DISCL_BACKGROUND_DI      0x00000008
#define DISCL_NOWINKEY_DI        0x00000010

typedef HRESULT (WINAPI *PFN_DirectInputCreateEx)(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, void **ppvOut,
    void *punkOuter);
typedef HRESULT (WINAPI *PFN_DirectInputCreateA)(
    HINSTANCE hinst, DWORD dwVersion, void **ppvOut, void *punkOuter);

typedef struct DInputProxy DInputProxy;
typedef struct DInputDeviceProxy DInputDeviceProxy;

struct DInputProxy {
    void *lpVtbl;
    LONG  refCount;
    void *real;
};

struct DInputDeviceProxy {
    void *lpVtbl;
    LONG  refCount;
    void *real;
    DWORD coopFlags;
    HWND  hWnd;
    DWORD stateCalls;
    DWORD stateFailures;
    DWORD stateInactiveSuppressions;
    DWORD pollCalls;
    DWORD pollFailures;
    DWORD pollInactiveSuppressions;
};

static PFN_DirectInputCreateEx g_realDirectInputCreateEx = NULL;
static PFN_DirectInputCreateA  g_realDirectInputCreateA = NULL;
static DWORD g_focusSerial = 0;
static DWORD g_lastFocusTick = 0;
static DWORD g_presentSerial = 0;
static EfzDx9PresentationKind g_repaintPresentationKind =
    EFZ_DX9_PRESENT_OTHER_REPAINT;
typedef DWORD (__cdecl *Qoh99PresentationDispositionFn)(void);
typedef void (__cdecl *Qoh99PresentationReportFn)(
    DWORD normalVisible,
    DWORD suppressed,
    DWORD correctedFinal);
typedef int (__cdecl *Qoh99OwnsPacingFn)(void);
typedef void (__cdecl *Qoh99VoidCallbackFn)(void);
typedef int (__cdecl *Qoh99PresentationTryAcquireSlotV2Fn)(void);
typedef void (__cdecl *Qoh99PresentationFailedV3Fn)(DWORD reason);
static Qoh99PresentationDispositionFn
    g_qoh99PresentationDisposition = NULL;
static Qoh99PresentationReportFn
    g_qoh99PresentationReport = NULL;
static Qoh99OwnsPacingFn g_qoh99OwnsPacing = NULL;
static Qoh99OwnsPacingFn g_qoh99OwnsNetworkPacing = NULL;
static LONG g_qoh99OwnsNetworkPacingQueryAttempted = 0;
static Qoh99VoidCallbackFn g_qoh99BootTick = NULL;
static Qoh99PresentationTryAcquireSlotV2Fn
    g_qoh99PresentationTryAcquireSlotV2 = NULL;
static Qoh99VoidCallbackFn
    g_qoh99PresentationCommit = NULL;
static Qoh99PresentationFailedV3Fn
    g_qoh99PresentationFailedV3 = NULL;
static LONG g_qoh99NetplayBootstrapped = 0;
static LONG g_qoh99PresentationQueryAttempted = 0;
static LONG g_qoh99OwnsPacingQueryAttempted = 0;
static DWORD g_qoh99NormalVisiblePresents = 0;
static DWORD g_qoh99SuppressedPresents = 0;
static DWORD g_qoh99CorrectedFinalPresents = 0;
static DWORD g_qoh99SuppressedColorFills = 0;
static DWORD g_qoh99SuppressedBlts = 0;
static DWORD g_qoh99SuppressedBltFasts = 0;
static DWORD g_qoh99SuppressedPacingCalls = 0;
typedef struct Qoh99DdrawDcCounters {
    volatile LONG getDcCalls;
    volatile LONG getDcSuccesses;
    volatile LONG getDcFailures;
    volatile LONG releaseDcCalls;
    volatile LONG releaseDcFailures;
    volatile LONG visibleWritebacks;
    volatile LONG historicalDiscards;
    volatile LONG nestedDcRejects;
    volatile LONG lockDcConflicts;
    volatile LONG surfaceRecreations;
    volatile LONG unsupportedFormatRejects;
} Qoh99DdrawDcCounters;
static Qoh99DdrawDcCounters g_qoh99DdrawDcCounters;
enum Qoh99CanonicalPublishFailureReason {
    QOH99_PUBLISH_FAILURE_NONE = 0,
    QOH99_PUBLISH_FAILURE_NO_TARGET = 1,
    QOH99_PUBLISH_FAILURE_NO_SOURCE = 2,
    QOH99_PUBLISH_FAILURE_EMPTY_CLIENT = 3,
    QOH99_PUBLISH_FAILURE_GET_DC = 4,
    QOH99_PUBLISH_FAILURE_GDI = 5,
    QOH99_PUBLISH_FAILURE_DISPOSITION = 6
};
static DWORD g_qoh99LastPublishFailure =
    QOH99_PUBLISH_FAILURE_NONE;
static DWORD g_flipSerial = 0;
static const DWORD kInputFocusSettleMs = 100;
static BOOL  g_renderTraceAfterFocus = FALSE;
static int   g_renderTraceBudget = 0;
static int   g_wndTraceBudget = 0;
static EfzDdrawOverlayCallback g_overlayCallback = NULL;
static void *g_overlayCallbackUserData = NULL;
static void __stdcall RequestPresentationModeFromPresenter(
    QohPresentationMode mode, void *userData);
static void wrapper_lazy_init(void);

void __stdcall EFZDDRAW_RegisterOverlayCallback(
    EfzDdrawOverlayCallback callback,
    void *userData)
{
    wrapper_lazy_init();
    g_overlayCallback = callback;
    g_overlayCallbackUserData = userData;
    LOG("Overlay callback %s callback=%p user=%p",
        callback ? "registered" : "cleared",
        (void *)callback,
        userData);
}

typedef struct {
    int  fps_limit;
    QohPresentationMode presentation_mode;
    QohRendererBackend renderer_backend;
    int  window_scale;
    BOOL linear_scale;
    int  takeover_delay;
} WrapperConfig;

static WrapperConfig g_cfg = {
    60, QOH_PRESENTATION_WINDOWED, QOH_RENDERER_AUTO, 1, FALSE, 0 };

static const char *wrapper_ini_path(void)
{
    static char path[MAX_PATH];
    static BOOL resolved = FALSE;
    if (!resolved) {
        DWORD n = GetModuleFileNameA(g_hInst, path, MAX_PATH);
        char *slash = (n != 0 && n < MAX_PATH) ? strrchr(path, '\\') : NULL;
        if (slash)
            lstrcpyA(slash + 1, "ddraw_wrapper.ini");
        else
            lstrcpyA(path, "ddraw_wrapper.ini");
        resolved = TRUE;
    }
    return path;
}

static int g_restoreClientW = 0;
static int g_restoreClientH = 0;
/* SetWindowPos dispatches window messages synchronously.  A mode change must
   never let those messages save the fullscreen client area as a windowed size. */
static LONG g_presentationGeometryChange = 0;

static int win_state_get(const char *key, int def)
{
    return (int)GetPrivateProfileIntA("Window", key, def, wrapper_ini_path());
}

#define WRAPPER_INI_OLD_REFERENCE_MARKER \
    "; QOH99 wrapper setting reference (generated)"
#define WRAPPER_INI_COMMENT_MARKER "; QOH99: "

static const char *wrapper_ini_setting_comment(
    const char *section, const char *key)
{
    if (lstrcmpiA(section, "Display") == 0) {
        if (lstrcmpiA(key, "FpsLimit") == 0)
            return "Frame cap: 30-240, or 0 unlimited.";
        if (lstrcmpiA(key, "WindowScale") == 0)
            return "Window size multiplier: 1-8.";
        if (lstrcmpiA(key, "LinearScale") == 0)
            return "Smooth scaling: 0 off, 1 on.";
    }
    if (lstrcmpiA(section, "Window") == 0) {
        if (lstrcmpiA(key, "PresentationMode") == 0)
            return "Display: 0 window, 1 borderless, 2 fullscreen.";
        if (lstrcmpiA(key, "Borderless") == 0)
            return "Legacy setting. Leave at 0.";
        if (lstrcmpiA(key, "ClientWidth") == 0)
            return "Window width; set both. 0 default.";
        if (lstrcmpiA(key, "ClientHeight") == 0)
            return "Window height; set both. 0 default.";
    }
    if (lstrcmpiA(section, "Renderer") == 0
        && lstrcmpiA(key, "Backend") == 0)
        return "Graphics backend. Leave at 0.";
    if (lstrcmpiA(section, "Replay") == 0
        && lstrcmpiA(key, "TakeoverDelay") == 0)
        return "Takeover input delay: 0-9.";
    if (lstrcmpiA(section, "Overlay") == 0) {
        if (lstrcmpiA(key, "ImGuiFontScalePct") == 0)
            return "Menu size: 75-200.";
        if (lstrcmpiA(key, "PaletteAutoLoad") == 0)
            return "Custom character palettes: 1 on, 0 off. Offline only.";
        if (lstrcmpiA(key, "PaletteEditorKey") == 0)
            return "Virtual-key that opens the native palette editor "
                   "(default 0x24 = Home). Match scene only.";
        if (lstrcmpiA(key, "PaletteEditorHideKey") == 0)
            return "Override for the editor's preview-hide key. 0 = use the "
                   "key the game has bound as P1's D button.";
        if (lstrcmpiA(key, "PaletteToggleKey") == 0)
            return "Virtual-key code that swaps custom/default. 0x09 = Tab.";
    }
    if (lstrcmpiA(section, "NetplayHud") == 0) {
        if (lstrcmpiA(key, "FontScalePct") == 0)
            return "Nickname and match text size: 50-300.";
        if (lstrcmpiA(key, "P1NameX") == 0)
            return "Player 1 name left/right. -1 default.";
        if (lstrcmpiA(key, "P1NameY") == 0)
            return "Player 1 name up/down. -1 default.";
        if (lstrcmpiA(key, "P2NameX") == 0)
            return "Player 2 name left/right. -1 default.";
        if (lstrcmpiA(key, "P2NameY") == 0)
            return "Player 2 name up/down. -1 default.";
        if (lstrcmpiA(key, "SessionX") == 0)
            return "Ping line left/right. -1 default.";
        if (lstrcmpiA(key, "SessionY") == 0)
            return "Ping line up/down. -1 default.";
        if (lstrcmpiA(key, "HeaderX") == 0)
            return "Match title left/right. -1 default.";
        if (lstrcmpiA(key, "HeaderY") == 0)
            return "Match title up/down. -1 default.";
    }
    return NULL;
}

static void wrapper_ini_copy_trimmed(
    char *out, size_t outSize, const char *text)
{
    const char *begin = text;
    const char *end;
    size_t length;
    if (outSize == 0) return;
    while (*begin == ' ' || *begin == '\t') ++begin;
    end = begin + strlen(begin);
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t'
                           || end[-1] == '\r' || end[-1] == '\n')) --end;
    length = (size_t)(end - begin);
    if (length >= outSize) length = outSize - 1;
    CopyMemory(out, begin, length);
    out[length] = '\0';
}

static void wrapper_ini_refresh_comments(void)
{
    const char *ini = wrapper_ini_path();
    char temp[MAX_PATH + 16];
    char line[512];
    char section[64] = "";
    BOOL dropOldReference = FALSE;
    FILE *input = fopen(ini, "rb");
    FILE *output;

    if (input == NULL) return;
    wsprintfA(temp, "%s.qoh99.tmp", ini);
    output = fopen(temp, "wb");
    if (output == NULL) {
        fclose(input);
        LOG("config: could not update setting comments in %s", ini);
        return;
    }

    while (fgets(line, (int)sizeof(line), input) != NULL) {
        char trimmed[512];
        char *equals;
        const char *comment;
        wrapper_ini_copy_trimmed(trimmed, sizeof(trimmed), line);

        if (strstr(trimmed, WRAPPER_INI_OLD_REFERENCE_MARKER) != NULL) {
            dropOldReference = TRUE;
            continue;
        }
        if (dropOldReference) {
            if (trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#')
                continue;
            dropOldReference = FALSE;
        }
        if (strncmp(trimmed, WRAPPER_INI_COMMENT_MARKER,
                    strlen(WRAPPER_INI_COMMENT_MARKER)) == 0)
            continue;
        if (trimmed[0] == '[') {
            char *close = strchr(trimmed + 1, ']');
            if (close != NULL) {
                *close = '\0';
                wrapper_ini_copy_trimmed(section, sizeof(section), trimmed + 1);
            }
            fputs(line, output);
            continue;
        }
        equals = strchr(trimmed, '=');
        if (equals != NULL) {
            char key[128];
            *equals = '\0';
            wrapper_ini_copy_trimmed(key, sizeof(key), trimmed);
            comment = wrapper_ini_setting_comment(section, key);
            if (comment != NULL) {
                fputs(WRAPPER_INI_COMMENT_MARKER, output);
                fputs(comment, output);
                fputs("\r\n", output);
            }
        }
        fputs(line, output);
    }
    fclose(input);
    if (fclose(output) != 0
        || !MoveFileExA(temp, ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG("config: could not replace %s after updating setting comments", ini);
        DeleteFileA(temp);
        return;
    }
    LOG("config: refreshed editable setting comments in %s", ini);
}

void Qoh99WrapperRefreshIniComments(void)
{
    wrapper_ini_refresh_comments();
}

static BOOL ini_try_get_int(const char *section, const char *key, int *out)
{
    char value[32];
    char *end = NULL;
    long parsed;
    GetPrivateProfileStringA(section, key, "__missing__", value,
                             (DWORD)sizeof(value), wrapper_ini_path());
    if (lstrcmpA(value, "__missing__") == 0)
        return FALSE;
    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX)
        return FALSE;
    if (out) *out = (int)parsed;
    return TRUE;
}
static void win_state_set(const char *key, int value)
{
    char buf[16];
    wsprintfA(buf, "%d", value);
    WritePrivateProfileStringA("Window", key, buf, wrapper_ini_path());
}
static void win_state_save_client_size(HWND hWnd)
{
    RECT rc;
    if (InterlockedCompareExchange(&g_presentationGeometryChange, 0, 0) != 0)
        return;
    if (hWnd && GetClientRect(hWnd, &rc) && rc.right > 0 && rc.bottom > 0) {
        g_restoreClientW = (int)rc.right;
        g_restoreClientH = (int)rc.bottom;
        win_state_set("ClientWidth",  g_restoreClientW);
        win_state_set("ClientHeight", g_restoreClientH);
    }
}

static QohPresentationMode g_restorePresentationMode =
    QOH_PRESENTATION_WINDOWED;
static LONG g_deferredPresentationChange = 0;
static QohPresentationMode g_deferredPresentationMode =
    QOH_PRESENTATION_WINDOWED;
static LONG g_displayRestorePending = 0;
static LONG g_displayRestoreDone = 0;

#define WM_QOH99_APPLY_DISPLAY (WM_APP + 0x52)

#define DISPLAY_RESTORE_AFTER_PRESENTS 2u

static BOOL wrapper_ini_exists(void)
{
    const DWORD attrs = GetFileAttributesA(wrapper_ini_path());
    return attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

#define CURSOR_IDLE_HIDE_MS 2500u
static DWORD g_lastMouseMoveTick = 0;
static BOOL  g_cursorHidden      = FALSE;

static LARGE_INTEGER g_qpcFreq;
static LARGE_INTEGER g_nextFrameQpc;
static BOOL          g_qpcInit          = FALSE;
static BOOL          g_fpsPacedThisFlip = FALSE;
static volatile LONG g_runtimeFpsLimit = 60;
static volatile LONG g_fpsLimitGeneration = 1;
static LONG          g_appliedFpsLimitGeneration = 0;

static HRESULT WINAPI Hook_DirectInputCreateEx(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, void **ppvOut,
    void *punkOuter);
static HRESULT WINAPI Hook_DirectInputCreateA(
    HINSTANCE hinst, DWORD dwVersion, void **ppvOut, void *punkOuter);
static BOOL qoh99_netplay_owns_pacing(void);
static void qoh99_display_restore_tick(MyDDraw *dd, DWORD presentNo);
static void pace_release_timer_period(void);
static void release_hotkey_hook(void);
static void wrapper_release_runtime(const char *reason);

static void config_save(void)
{
    const char *ini = wrapper_ini_path();
    char buf[16];

    wsprintfA(buf, "%d", g_cfg.fps_limit);
    WritePrivateProfileStringA("Display", "FpsLimit", buf, ini);
    wsprintfA(buf, "%d", g_cfg.window_scale);
    WritePrivateProfileStringA("Display", "WindowScale", buf, ini);
    wsprintfA(buf, "%d", g_cfg.linear_scale ? 1 : 0);
    WritePrivateProfileStringA("Display", "LinearScale", buf, ini);
    win_state_set("PresentationMode", (int)g_cfg.presentation_mode);
    win_state_set("Borderless",
                  qoh99_presentation_legacy_borderless_mirror(
                      g_cfg.presentation_mode));
    wsprintfA(buf, "%d", (int)g_cfg.renderer_backend);
    WritePrivateProfileStringA("Renderer", "Backend", buf, ini);
    wsprintfA(buf, "%d", g_cfg.takeover_delay);
    WritePrivateProfileStringA("Replay", "TakeoverDelay", buf, ini);
    win_state_set("ClientWidth", g_restoreClientW);
    win_state_set("ClientHeight", g_restoreClientH);
    wrapper_ini_refresh_comments();
}

static void config_load(void)
{
    const char *ini;
    int fps;
    int scale;
    int width;
    int height;
    int takeoverDelay;

    if (!wrapper_ini_exists()) {
        LOG("config: no %s; using defaults (fps=%d scale=%d linear=%d mode=%d backend=%d)",
            wrapper_ini_path(), g_cfg.fps_limit, g_cfg.window_scale,
            (int)g_cfg.linear_scale, (int)g_cfg.presentation_mode,
            (int)g_cfg.renderer_backend);
        config_save();
        return;
    }

    ini = wrapper_ini_path();

    fps = (int)GetPrivateProfileIntA("Display", "FpsLimit", g_cfg.fps_limit, ini);
    if (fps != 0 && (fps < 30 || fps > 240)) {
        LOG("config: FpsLimit=%d out of range; using %d", fps, g_cfg.fps_limit);
        fps = g_cfg.fps_limit;
    }
    g_cfg.fps_limit = fps;

    scale = (int)GetPrivateProfileIntA("Display", "WindowScale",
                                       g_cfg.window_scale, ini);
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    g_cfg.window_scale = scale;

    g_cfg.linear_scale =
        GetPrivateProfileIntA("Display", "LinearScale", 0, ini) ? TRUE : FALSE;

    takeoverDelay = (int)GetPrivateProfileIntA(
        "Replay", "TakeoverDelay", g_cfg.takeover_delay, ini);
    if (takeoverDelay < 0) takeoverDelay = 0;
    if (takeoverDelay > 9) takeoverDelay = 9;
    g_cfg.takeover_delay = takeoverDelay;

    {
        int requested = 0;
        BOOL hasMode = ini_try_get_int(
            "Window", "PresentationMode", &requested);
        const int legacy = win_state_get("Borderless", 0) ? 1 : 0;
        g_cfg.presentation_mode = qoh99_presentation_mode_from_ini(
            hasMode ? 1 : 0, requested, legacy);
        if (hasMode && !qoh99_presentation_mode_valid(requested))
            LOG("config: invalid PresentationMode=%d; using Windowed",
                requested);
        g_restorePresentationMode = g_cfg.presentation_mode;
    }
    {
        int backend = (int)QOH_RENDERER_AUTO;
        if (!ini_try_get_int("Renderer", "Backend", &backend)
            || !qoh99_renderer_backend_valid(backend)) {
            if (backend != (int)QOH_RENDERER_AUTO)
                LOG("config: invalid Renderer Backend=%d; using Auto", backend);
            backend = (int)QOH_RENDERER_AUTO;
        }
        g_cfg.renderer_backend = qoh99_renderer_backend_from_ini(backend);
    }

    width  = win_state_get("ClientWidth", 0);
    height = win_state_get("ClientHeight", 0);
    if (width > 0 && height > 0 && width <= 16384 && height <= 16384) {
        g_restoreClientW = width;
        g_restoreClientH = height;
    } else {
        if (width != 0 || height != 0) {
            LOG("config: ignoring implausible persisted client size %dx%d",
                width, height);
        }
        g_restoreClientW = 0;
        g_restoreClientH = 0;
    }

    InterlockedExchange(&g_runtimeFpsLimit, (LONG)g_cfg.fps_limit);
    InterlockedIncrement(&g_fpsLimitGeneration);

    config_save();

    LOG("config: fps=%d scale=%d linear=%d mode=%d backend=%d restoreClient=%dx%d ini=%s",
        g_cfg.fps_limit, g_cfg.window_scale, (int)g_cfg.linear_scale,
        (int)g_cfg.presentation_mode, (int)g_cfg.renderer_backend,
        g_restoreClientW, g_restoreClientH, ini);
}

typedef enum WrapperHotkeyAction {
    WRAPPER_HOTKEY_TOGGLE_PRESENTATION = 0,
    WRAPPER_HOTKEY_TOGGLE_MENU = 1,
    WRAPPER_HOTKEY_TOGGLE_INPUT_VIEWER = 2,
    WRAPPER_HOTKEY_REPLAY_PAUSE = 3,
    WRAPPER_HOTKEY_REPLAY_STEP = 4,
    WRAPPER_HOTKEY_REPLAY_SLOWER = 5,
    WRAPPER_HOTKEY_REPLAY_FASTER = 6,
    WRAPPER_HOTKEY_REPLAY_TAKEOVER_EXIT = 7,
    WRAPPER_HOTKEY_REPLAY_TAKEOVER_P1 = 8,
    WRAPPER_HOTKEY_REPLAY_TAKEOVER_P2 = 9,
    WRAPPER_HOTKEY_REPLAY_TAKEOVER_RETRY = 10,
    WRAPPER_HOTKEY_REPLAY_TAKEOVER_DELAY_DECREASE = 11,
    WRAPPER_HOTKEY_REPLAY_TAKEOVER_DELAY_INCREASE = 12,
    WRAPPER_HOTKEY_REPLAY_TOGGLE_COLLISION_DISPLAY = 13,
    WRAPPER_HOTKEY_REPLAY_RESTART = 14,
    WRAPPER_HOTKEY_REPLAY_STEP_BACK = 15,
    WRAPPER_HOTKEY_COUNT = 16
} WrapperHotkeyAction;

typedef struct WrapperHotkeyList {
    BYTE dik[4];
    int count;
} WrapperHotkeyList;

static WrapperHotkeyList g_wrapperHotkeys[WRAPPER_HOTKEY_COUNT];
static BYTE g_wrapperHotkeyDown[256];

static const char *wrapper_hotkey_name(WrapperHotkeyAction action)
{
    static const char *names[WRAPPER_HOTKEY_COUNT] = {
        "TogglePresentation", "ToggleOverlayMenu", "ToggleInputViewer",
        "ReplayPause", "ReplayStep", "ReplaySlower", "ReplayFaster",
        "ReplayTakeoverExit", "ReplayTakeoverP1", "ReplayTakeoverP2",
        "ReplayTakeoverRetry", "ReplayTakeoverDelayDecrease",
        "ReplayTakeoverDelayIncrease", "ReplayToggleCollisionDisplay",
        "ReplayRestart", "ReplayStepBack"
    };
    return action >= 0 && action < WRAPPER_HOTKEY_COUNT
        ? names[action] : "";
}

static void wrapper_hotkey_default(
    WrapperHotkeyAction action, WrapperHotkeyList *out)
{
    static const BYTE defaults[WRAPPER_HOTKEY_COUNT][4] = {
        { 0x57, 0, 0, 0 }, { 0xD2, 0, 0, 0 }, { 0x0E, 0, 0, 0 },
        { 0x39, 0, 0, 0 }, { 0xCD, 0x34, 0, 0 },
        { 0x0C, 0x4A, 0, 0 }, { 0x0D, 0x4E, 0, 0 },
        { 0x10, 0, 0, 0 }, { 0x21, 0, 0, 0 },
        { 0x22, 0, 0, 0 }, { 0x13, 0, 0, 0 },
        { 0x0A, 0, 0, 0 }, { 0x0B, 0, 0, 0 },
        { 0x41, 0, 0, 0 }, { 0xC7, 0, 0, 0 }, { 0xCB, 0x33, 0, 0 }
    };
    static const int counts[WRAPPER_HOTKEY_COUNT] = {
        1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    ZeroMemory(out, sizeof(*out));
    out->count = counts[action];
    CopyMemory(out->dik, defaults[action], (SIZE_T)out->count);
}

static void wrapper_hotkeys_load(void)
{
    int action;
    ZeroMemory(g_wrapperHotkeyDown, sizeof(g_wrapperHotkeyDown));
    for (action = 0; action < WRAPPER_HOTKEY_COUNT; ++action) {
        char text[96];
        char *cursor;
        GetPrivateProfileStringA(
            "Hotkeys", wrapper_hotkey_name((WrapperHotkeyAction)action),
            "__missing__", text, (DWORD)sizeof(text), wrapper_ini_path());
        if (lstrcmpA(text, "__missing__") == 0) {
            wrapper_hotkey_default(
                (WrapperHotkeyAction)action, &g_wrapperHotkeys[action]);
            continue;
        }
        ZeroMemory(&g_wrapperHotkeys[action],
                   sizeof(g_wrapperHotkeys[action]));
        cursor = text;
        while (*cursor != '\0'
               && g_wrapperHotkeys[action].count < 4) {
            char *end;
            unsigned long value;
            while (*cursor == ' ' || *cursor == '\t' || *cursor == ',')
                ++cursor;
            if (*cursor == '\0') break;
            value = strtoul(cursor, &end, 0);
            if (end == cursor || value > 0xFFu) {
                LOG("hotkeys: invalid %s token near '%s'; ignoring remainder",
                    wrapper_hotkey_name((WrapperHotkeyAction)action), cursor);
                break;
            }
            {
                BOOL duplicate = FALSE;
                int i;
                for (i = 0; i < g_wrapperHotkeys[action].count; ++i)
                    if (g_wrapperHotkeys[action].dik[i] == (BYTE)value)
                        duplicate = TRUE;
                if (!duplicate)
                    g_wrapperHotkeys[action].dik[
                        g_wrapperHotkeys[action].count++] = (BYTE)value;
            }
            cursor = end;
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            if (*cursor == ',') ++cursor;
            else if (*cursor != '\0') {
                LOG("hotkeys: invalid separator in %s; ignoring remainder",
                    wrapper_hotkey_name((WrapperHotkeyAction)action));
                break;
            }
        }
        LOG("hotkeys: %s bindings=%d",
            wrapper_hotkey_name((WrapperHotkeyAction)action),
            g_wrapperHotkeys[action].count);
    }
}

__declspec(dllexport)
DWORD __stdcall EFZDDRAW_QueryWrapperHotkey(
    const char *name, BYTE *outDiks, DWORD cap)
{
    int action;
    wrapper_lazy_init();
    if (name == NULL || outDiks == NULL || cap == 0) return 0;
    for (action = 0; action < WRAPPER_HOTKEY_COUNT; ++action) {
        const WrapperHotkeyList *list;
        DWORD count;
        if (strcmp(wrapper_hotkey_name((WrapperHotkeyAction)action),
                   name) != 0) {
            continue;
        }
        list = &g_wrapperHotkeys[action];
        count = list->count < 0 ? 0u : (DWORD)list->count;
        if (count > 4u) count = 4u;
        if (count > cap) count = cap;
        CopyMemory(outDiks, list->dik, (SIZE_T)count);
        return count;
    }
    return 0;
}

static BOOL wrapper_hotkey_matches(
    WrapperHotkeyAction action, BYTE dik)
{
    int i;
    if (action < 0 || action >= WRAPPER_HOTKEY_COUNT)
        return FALSE;
    for (i = 0; i < g_wrapperHotkeys[action].count; ++i)
        if (g_wrapperHotkeys[action].dik[i] == dik)
            return TRUE;
    return FALSE;
}

static BOOL hr_failed(HRESULT hr)
{
    return hr < 0;
}

static BOOL should_log_counter(DWORD count)
{
    return count <= 8 || (count % 120) == 0;
}

static BOOL is_foreground_for_hwnd(HWND hwnd)
{
    HWND foreground;
    HWND hwndRoot;
    HWND foregroundRoot;
    DWORD foregroundPid = 0;

    if (hwnd == NULL || !IsWindow(hwnd))
        return FALSE;

    foreground = GetForegroundWindow();
    if (foreground == NULL || !IsWindow(foreground))
        return FALSE;

    GetWindowThreadProcessId(foreground, &foregroundPid);
    if (foregroundPid != GetCurrentProcessId())
        return FALSE;

    if (foreground == hwnd || IsChild(hwnd, foreground))
        return TRUE;

    hwndRoot = GetAncestor(hwnd, GA_ROOTOWNER);
    foregroundRoot = GetAncestor(foreground, GA_ROOTOWNER);
    if (hwndRoot == NULL)
        hwndRoot = hwnd;
    if (foregroundRoot == NULL)
        foregroundRoot = foreground;

    return hwndRoot == foregroundRoot;
}

static BOOL should_suppress_dinput_device(DInputDeviceProxy *dev)
{
    if (EFZDX9_Presenter_ShouldSuppressGameInput())
        return TRUE;

    if (!g_appActive)
        return TRUE;

    if (g_lastFocusTick != 0 && GetTickCount() - g_lastFocusTick < kInputFocusSettleMs)
        return TRUE;

    if (dev != NULL && dev->hWnd != NULL)
        return !is_foreground_for_hwnd(dev->hWnd);

    if (g_dd != NULL && g_dd->hWnd != NULL)
        return !is_foreground_for_hwnd(g_dd->hWnd);

    return FALSE;
}

static HRESULT call_real_release(void *real)
{
    typedef HRESULT (__stdcall *Fn)(void *);
    return ((Fn)(*(void ***)real)[2])(real);
}

static DInputDeviceProxy *wrap_dinput_device(void *real)
{
    static void *deviceVtbl[32];
    DInputDeviceProxy *dev;

    if (!real) return NULL;

    dev = (DInputDeviceProxy *)calloc(1, sizeof(*dev));
    if (!dev) return NULL;

    dev->lpVtbl = deviceVtbl;
    dev->refCount = 1;
    dev->real = real;
    dev->coopFlags = DISCL_FOREGROUND_DI;
    return dev;
}

static HRESULT __stdcall DIDev_QueryInterface(DInputDeviceProxy *dev,
    REFIID riid, void **ppv)
{
    if (!ppv) return DDERR_INVALIDPARAMS;
    *ppv = dev;
    InterlockedIncrement(&dev->refCount);
    return DD_OK;
}

static ULONG __stdcall DIDev_AddRef(DInputDeviceProxy *dev)
{
    return (ULONG)InterlockedIncrement(&dev->refCount);
}

static ULONG __stdcall DIDev_Release(DInputDeviceProxy *dev)
{
    LONG ref = InterlockedDecrement(&dev->refCount);
    if (ref == 0) {
        call_real_release(dev->real);
        free(dev);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall DIDev_GetCapabilities(DInputDeviceProxy *dev, void *caps)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[3])(dev->real, caps);
}

static HRESULT __stdcall DIDev_EnumObjects(DInputDeviceProxy *dev,
    void *callback, void *ref, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[4])(dev->real, callback, ref, flags);
}

static HRESULT __stdcall DIDev_GetProperty(DInputDeviceProxy *dev,
    REFGUID prop, void *header)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, void *);
    return ((Fn)(*(void ***)dev->real)[5])(dev->real, prop, header);
}

static HRESULT __stdcall DIDev_SetProperty(DInputDeviceProxy *dev,
    REFGUID prop, void *header)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, void *);
    return ((Fn)(*(void ***)dev->real)[6])(dev->real, prop, header);
}

static HRESULT __stdcall DIDev_Acquire(DInputDeviceProxy *dev)
{
    typedef HRESULT (__stdcall *Fn)(void *);
    HRESULT hr = ((Fn)(*(void ***)dev->real)[7])(dev->real);
    LOG("DI Acquire dev=%p real=%p hwnd=%p flags=%08X active=%d hr=%08X",
        (void *)dev, dev->real, (void *)dev->hWnd, dev->coopFlags,
        g_appActive, (DWORD)hr);
    return hr;
}

static HRESULT __stdcall DIDev_Unacquire(DInputDeviceProxy *dev)
{
    typedef HRESULT (__stdcall *Fn)(void *);
    HRESULT hr = ((Fn)(*(void ***)dev->real)[8])(dev->real);
    LOG("DI Unacquire dev=%p real=%p hwnd=%p flags=%08X active=%d hr=%08X",
        (void *)dev, dev->real, (void *)dev->hWnd, dev->coopFlags,
        g_appActive, (DWORD)hr);
    return hr;
}

static void neutralize_dinput_state(void *data, DWORD cbData)
{
    ZeroMemory(data, cbData);

    /* EFZ reads an 80-byte DIJOYSTATE for pads. POV neutral is 0xFFFFFFFF;
       zeroing it fabricates DPad Up when focus is restored or reacquire fails. */
    if (cbData == 80u) {
        BYTE *bytes = (BYTE *)data;
        DWORD neutralPov = 0xFFFFFFFFu;
        DWORD index;
        for (index = 0; index < 4u; ++index) {
            CopyMemory(bytes + 32u + (index * sizeof(DWORD)),
                &neutralPov,
                sizeof(neutralPov));
        }
    }
}

static HRESULT __stdcall DIDev_GetDeviceState(DInputDeviceProxy *dev,
    DWORD cbData, void *data)
{
    typedef HRESULT (__stdcall *GetStateFn)(void *, DWORD, void *);
    typedef HRESULT (__stdcall *AcquireFn)(void *);
    HRESULT hr;

    if (!data) {
        return ((GetStateFn)(*(void ***)dev->real)[9])(dev->real, cbData, data);
    }

    dev->stateCalls++;
    if (should_suppress_dinput_device(dev)) {
        neutralize_dinput_state(data, cbData);
        dev->stateInactiveSuppressions++;
        if (should_log_counter(dev->stateInactiveSuppressions)) {
            LOG("DI GetDeviceState inactive-neutral dev=%p call=%u inactiveNeutral=%u cb=%u hwnd=%p fg=%p focusSerial=%u",
                (void *)dev, dev->stateCalls, dev->stateInactiveSuppressions,
                cbData, (void *)dev->hWnd, (void *)GetForegroundWindow(),
                g_focusSerial);
        }
        return DD_OK;
    }

    hr = ((GetStateFn)(*(void ***)dev->real)[9])(dev->real, cbData, data);
    if (hr_failed(hr)) {
        HRESULT acquireHr;
        neutralize_dinput_state(data, cbData);
        dev->stateFailures++;
        acquireHr = ((AcquireFn)(*(void ***)dev->real)[7])(dev->real);
        if (should_log_counter(dev->stateFailures)) {
            LOG("DI GetDeviceState fail-neutral dev=%p call=%u fail=%u cb=%u hr=%08X acquireHr=%08X active=%d hwnd=%p fg=%p focusSerial=%u",
                (void *)dev, dev->stateCalls, dev->stateFailures, cbData,
                (DWORD)hr, (DWORD)acquireHr, g_appActive, (void *)dev->hWnd,
                (void *)GetForegroundWindow(), g_focusSerial);
        }
        return DD_OK;
    }

    if (dev->stateCalls <= 8 || g_renderTraceAfterFocus) {
        LOG("DI GetDeviceState ok dev=%p call=%u cb=%u active=%d hwnd=%p focusSerial=%u",
            (void *)dev, dev->stateCalls, cbData, g_appActive,
            (void *)dev->hWnd, g_focusSerial);
    }
    return hr;
}

static HRESULT __stdcall DIDev_GetDeviceData(DInputDeviceProxy *dev,
    DWORD cbObjectData, void *objectData, DWORD *inOut, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD, void *, DWORD *, DWORD);
    if (inOut != NULL && should_suppress_dinput_device(dev)) {
        *inOut = 0;
        return DD_OK;
    }
    return ((Fn)(*(void ***)dev->real)[10])(dev->real, cbObjectData, objectData,
                                          inOut, flags);
}

static HRESULT __stdcall DIDev_SetDataFormat(DInputDeviceProxy *dev, void *fmt)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[11])(dev->real, fmt);
}

static HRESULT __stdcall DIDev_SetEventNotification(DInputDeviceProxy *dev,
    HANDLE event)
{
    typedef HRESULT (__stdcall *Fn)(void *, HANDLE);
    return ((Fn)(*(void ***)dev->real)[12])(dev->real, event);
}

static HRESULT __stdcall DIDev_SetCooperativeLevel(DInputDeviceProxy *dev,
    HWND hWnd, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, HWND, DWORD);
    HRESULT hr;
    DWORD origFlags = flags;
    dev->hWnd = hWnd;

    flags = (flags & ~(DISCL_EXCLUSIVE_DI | DISCL_BACKGROUND_DI | DISCL_NOWINKEY_DI))
        | DISCL_NONEXCLUSIVE_DI
        | DISCL_FOREGROUND_DI;

    dev->coopFlags = flags;
    hr = ((Fn)(*(void ***)dev->real)[13])(dev->real, hWnd, flags);
    LOG("DI SetCooperativeLevel dev=%p real=%p hwnd=%p origFlags=%08X flags=%08X active=%d hr=%08X",
        (void *)dev, dev->real, (void *)hWnd, origFlags, flags, g_appActive, (DWORD)hr);
    return hr;
}

static HRESULT __stdcall DIDev_GetObjectInfo(DInputDeviceProxy *dev,
    void *objectInfo, DWORD object, DWORD how)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, DWORD, DWORD);
    return ((Fn)(*(void ***)dev->real)[14])(dev->real, objectInfo, object, how);
}

static HRESULT __stdcall DIDev_GetDeviceInfo(DInputDeviceProxy *dev,
    void *deviceInfo)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[15])(dev->real, deviceInfo);
}

static HRESULT __stdcall DIDev_RunControlPanel(DInputDeviceProxy *dev,
    HWND owner, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, HWND, DWORD);
    return ((Fn)(*(void ***)dev->real)[16])(dev->real, owner, flags);
}

static HRESULT __stdcall DIDev_Initialize(DInputDeviceProxy *dev,
    HINSTANCE hinst, DWORD version, REFGUID guid)
{
    typedef HRESULT (__stdcall *Fn)(void *, HINSTANCE, DWORD, REFGUID);
    return ((Fn)(*(void ***)dev->real)[17])(dev->real, hinst, version, guid);
}

static HRESULT __stdcall DIDev_CreateEffect(DInputDeviceProxy *dev,
    REFGUID guid, void *effect, void **out, void *outer)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, void *, void **, void *);
    return ((Fn)(*(void ***)dev->real)[18])(dev->real, guid, effect, out, outer);
}

static HRESULT __stdcall DIDev_EnumEffects(DInputDeviceProxy *dev,
    void *callback, void *ref, DWORD type)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[19])(dev->real, callback, ref, type);
}

static HRESULT __stdcall DIDev_GetEffectInfo(DInputDeviceProxy *dev,
    void *effectInfo, REFGUID guid)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, REFGUID);
    return ((Fn)(*(void ***)dev->real)[20])(dev->real, effectInfo, guid);
}

static HRESULT __stdcall DIDev_GetForceFeedbackState(DInputDeviceProxy *dev,
    DWORD *state)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD *);
    return ((Fn)(*(void ***)dev->real)[21])(dev->real, state);
}

static HRESULT __stdcall DIDev_SendForceFeedbackCommand(DInputDeviceProxy *dev,
    DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[22])(dev->real, flags);
}

static HRESULT __stdcall DIDev_EnumCreatedEffectObjects(DInputDeviceProxy *dev,
    void *callback, void *ref, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[23])(dev->real, callback, ref, flags);
}

static HRESULT __stdcall DIDev_Escape(DInputDeviceProxy *dev, void *escape)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[24])(dev->real, escape);
}

static HRESULT __stdcall DIDev_Poll(DInputDeviceProxy *dev)
{
    typedef HRESULT (__stdcall *Fn)(void *);
    HRESULT hr;

    dev->pollCalls++;
    if (should_suppress_dinput_device(dev)) {
        dev->pollInactiveSuppressions++;
        if (should_log_counter(dev->pollInactiveSuppressions)) {
            LOG("DI Poll inactive-ok dev=%p call=%u inactivePoll=%u hwnd=%p fg=%p focusSerial=%u",
                (void *)dev, dev->pollCalls, dev->pollInactiveSuppressions,
                (void *)dev->hWnd, (void *)GetForegroundWindow(),
                g_focusSerial);
        }
        return DD_OK;
    }

    hr = ((Fn)(*(void ***)dev->real)[25])(dev->real);
    if (hr_failed(hr)) {
        dev->pollFailures++;
        if (should_log_counter(dev->pollFailures)) {
            LOG("DI Poll fail-masked dev=%p call=%u fail=%u hr=%08X active=%d hwnd=%p fg=%p focusSerial=%u",
                (void *)dev, dev->pollCalls, dev->pollFailures, (DWORD)hr,
                g_appActive, (void *)dev->hWnd, (void *)GetForegroundWindow(),
                g_focusSerial);
        }
        return DD_OK;
    }
    if (dev->pollCalls <= 8 || g_renderTraceAfterFocus) {
        LOG("DI Poll ok dev=%p call=%u active=%d hwnd=%p focusSerial=%u",
            (void *)dev, dev->pollCalls, g_appActive, (void *)dev->hWnd,
            g_focusSerial);
    }
    return hr;
}

static HRESULT __stdcall DIDev_SendDeviceData(DInputDeviceProxy *dev,
    DWORD cbObjectData, void *objectData, DWORD *inOut, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD, void *, DWORD *, DWORD);
    return ((Fn)(*(void ***)dev->real)[26])(dev->real, cbObjectData, objectData,
                                          inOut, flags);
}

static HRESULT __stdcall DIDev_EnumEffectsInFile(DInputDeviceProxy *dev,
    const char *fileName, void *callback, void *ref, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, const char *, void *, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[27])(dev->real, fileName, callback, ref,
                                          flags);
}

static HRESULT __stdcall DIDev_WriteEffectToFile(DInputDeviceProxy *dev,
    const char *fileName, DWORD entries, void *fileEffects, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, const char *, DWORD, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[28])(dev->real, fileName, entries,
                                          fileEffects, flags);
}

static HRESULT __stdcall DIDev_BuildActionMap(DInputDeviceProxy *dev,
    void *actionFormat, const char *userName, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, const char *, DWORD);
    return ((Fn)(*(void ***)dev->real)[29])(dev->real, actionFormat, userName,
                                          flags);
}

static HRESULT __stdcall DIDev_SetActionMap(DInputDeviceProxy *dev,
    void *actionFormat, const char *userName, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, const char *, DWORD);
    return ((Fn)(*(void ***)dev->real)[30])(dev->real, actionFormat, userName,
                                          flags);
}

static HRESULT __stdcall DIDev_GetImageInfo(DInputDeviceProxy *dev,
    void *imageInfo)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[31])(dev->real, imageInfo);
}

static void init_dinput_device_vtbl(void)
{
    DInputDeviceProxy *dummy = NULL;
    void **vtbl;

    dummy = wrap_dinput_device((void *)1);
    if (!dummy) return;
    vtbl = (void **)dummy->lpVtbl;
    vtbl[0]  = DIDev_QueryInterface;
    vtbl[1]  = DIDev_AddRef;
    vtbl[2]  = DIDev_Release;
    vtbl[3]  = DIDev_GetCapabilities;
    vtbl[4]  = DIDev_EnumObjects;
    vtbl[5]  = DIDev_GetProperty;
    vtbl[6]  = DIDev_SetProperty;
    vtbl[7]  = DIDev_Acquire;
    vtbl[8]  = DIDev_Unacquire;
    vtbl[9]  = DIDev_GetDeviceState;
    vtbl[10] = DIDev_GetDeviceData;
    vtbl[11] = DIDev_SetDataFormat;
    vtbl[12] = DIDev_SetEventNotification;
    vtbl[13] = DIDev_SetCooperativeLevel;
    vtbl[14] = DIDev_GetObjectInfo;
    vtbl[15] = DIDev_GetDeviceInfo;
    vtbl[16] = DIDev_RunControlPanel;
    vtbl[17] = DIDev_Initialize;
    vtbl[18] = DIDev_CreateEffect;
    vtbl[19] = DIDev_EnumEffects;
    vtbl[20] = DIDev_GetEffectInfo;
    vtbl[21] = DIDev_GetForceFeedbackState;
    vtbl[22] = DIDev_SendForceFeedbackCommand;
    vtbl[23] = DIDev_EnumCreatedEffectObjects;
    vtbl[24] = DIDev_Escape;
    vtbl[25] = DIDev_Poll;
    vtbl[26] = DIDev_SendDeviceData;
    vtbl[27] = DIDev_EnumEffectsInFile;
    vtbl[28] = DIDev_WriteEffectToFile;
    vtbl[29] = DIDev_BuildActionMap;
    vtbl[30] = DIDev_SetActionMap;
    vtbl[31] = DIDev_GetImageInfo;
    free(dummy);
}

static void *wrap_dinput_device_out(void *real)
{
    static BOOL initialized = FALSE;
    DInputDeviceProxy *dev;

    if (!initialized) {
        init_dinput_device_vtbl();
        initialized = TRUE;
    }

    dev = wrap_dinput_device(real);
    if (!dev) return real;
    LOG("DirectInput device wrapped real=%p proxy=%p", real, (void *)dev);
    return dev;
}

static HRESULT __stdcall DI_QueryInterface(DInputProxy *di,
    REFIID riid, void **ppv)
{
    if (!ppv) return DDERR_INVALIDPARAMS;
    *ppv = di;
    InterlockedIncrement(&di->refCount);
    return DD_OK;
}

static ULONG __stdcall DI_AddRef(DInputProxy *di)
{
    return (ULONG)InterlockedIncrement(&di->refCount);
}

static ULONG __stdcall DI_Release(DInputProxy *di)
{
    LONG ref = InterlockedDecrement(&di->refCount);
    if (ref == 0) {
        call_real_release(di->real);
        free(di);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall DI_CreateDevice(DInputProxy *di,
    REFGUID guid, void **deviceOut, void *outer)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, void **, void *);
    HRESULT hr = ((Fn)(*(void ***)di->real)[3])(di->real, guid, deviceOut, outer);
    if (!hr_failed(hr) && deviceOut && *deviceOut)
        *deviceOut = wrap_dinput_device_out(*deviceOut);
    return hr;
}

static HRESULT __stdcall DI_EnumDevices(DInputProxy *di,
    DWORD devType, void *callback, void *ref, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD, void *, void *, DWORD);
    return ((Fn)(*(void ***)di->real)[4])(di->real, devType, callback, ref, flags);
}

static HRESULT __stdcall DI_GetDeviceStatus(DInputProxy *di, REFGUID guid)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID);
    return ((Fn)(*(void ***)di->real)[5])(di->real, guid);
}

static HRESULT __stdcall DI_RunControlPanel(DInputProxy *di,
    HWND owner, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, HWND, DWORD);
    return ((Fn)(*(void ***)di->real)[6])(di->real, owner, flags);
}

static HRESULT __stdcall DI_Initialize(DInputProxy *di,
    HINSTANCE hinst, DWORD version)
{
    typedef HRESULT (__stdcall *Fn)(void *, HINSTANCE, DWORD);
    return ((Fn)(*(void ***)di->real)[7])(di->real, hinst, version);
}

static HRESULT __stdcall DI_FindDevice(DInputProxy *di,
    REFGUID guid, const char *name, GUID *outGuid)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, const char *, GUID *);
    return ((Fn)(*(void ***)di->real)[8])(di->real, guid, name, outGuid);
}

static HRESULT __stdcall DI_CreateDeviceEx(DInputProxy *di,
    REFGUID guid, REFIID riid, void **deviceOut, void *outer)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, REFIID, void **, void *);
    HRESULT hr = ((Fn)(*(void ***)di->real)[9])(di->real, guid, riid,
                                              deviceOut, outer);
    if (!hr_failed(hr) && deviceOut && *deviceOut)
        *deviceOut = wrap_dinput_device_out(*deviceOut);
    return hr;
}

static void *wrap_dinput(void *real)
{
    static void *diVtbl[10] = {
        DI_QueryInterface,
        DI_AddRef,
        DI_Release,
        DI_CreateDevice,
        DI_EnumDevices,
        DI_GetDeviceStatus,
        DI_RunControlPanel,
        DI_Initialize,
        DI_FindDevice,
        DI_CreateDeviceEx
    };
    DInputProxy *di;

    if (!real) return NULL;
    di = (DInputProxy *)calloc(1, sizeof(*di));
    if (!di) return real;

    di->lpVtbl = diVtbl;
    di->refCount = 1;
    di->real = real;
    LOG("DirectInput wrapped real=%p proxy=%p", real, (void *)di);
    return di;
}

static HRESULT WINAPI Hook_DirectInputCreateEx(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, void **ppvOut,
    void *punkOuter)
{
    HRESULT hr;

    if (!g_realDirectInputCreateEx)
        return DDERR_GENERIC;

    hr = g_realDirectInputCreateEx(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    if (!hr_failed(hr) && ppvOut && *ppvOut)
        *ppvOut = wrap_dinput(*ppvOut);
    return hr;
}

static HRESULT WINAPI Hook_DirectInputCreateA(
    HINSTANCE hinst, DWORD dwVersion, void **ppvOut, void *punkOuter)
{
    HRESULT hr;

    if (!g_realDirectInputCreateA)
        return DDERR_GENERIC;

    hr = g_realDirectInputCreateA(hinst, dwVersion, ppvOut, punkOuter);
    LOG("DirectInputCreateA hook version=%08X hr=%08X out=%p",
        dwVersion, (DWORD)hr, ppvOut ? *ppvOut : NULL);
    if (!hr_failed(hr) && ppvOut && *ppvOut)
        *ppvOut = wrap_dinput(*ppvOut);
    return hr;
}

static Qoh99IatTxn g_wrapperIatTxn;
static BOOL        g_wrapperIatTxnLive = FALSE;

static void wrapper_iat_txn_ensure(void)
{
    if (!g_wrapperIatTxnLive) {
        qoh99_iat_txn_begin(&g_wrapperIatTxn);
        g_wrapperIatTxnLive = TRUE;
    }
}

static int wrapper_iat_patch(const char *dll, const char *fn,
                             void *replacement, void **original)
{
    char err[256];
    int result;

    wrapper_iat_txn_ensure();
    err[0] = '\0';
    result = qoh99_iat_patch_module(&g_wrapperIatTxn, GetModuleHandleA(NULL),
                                    dll, fn, replacement, original,
                                    err, sizeof(err));
    if (result > 0)
        LOG("IAT patch %s!%s -> %p (%d slot(s))", dll, fn, replacement, result);
    else if (result == 0)
        LOGV("IAT patch %s!%s skipped: %s", dll, fn, err);
    else
        WLOG_ISSUE("IAT patch %s!%s FAILED code=%d: %s", dll, fn, result, err);
    return result;
}

static BOOL WINAPI Hook_Ellipse(HDC dc, int left, int top, int right, int bottom)
{
    gdi_effect_diagnostic_note_ellipse(dc, left, top, right, bottom);
    return g_realEllipse
        ? g_realEllipse(dc, left, top, right, bottom)
        : Ellipse(dc, left, top, right, bottom);
}

static void hook_gdi_effect_diagnostic_iat(void)
{
    static LONG s_done = 0;
    HMODULE gdi32;
    FARPROC real;
    void *original = NULL;
    char value[8];
    DWORD length;
    int result;

    if (InterlockedExchange(&s_done, 1))
        return;
    length = GetEnvironmentVariableA(
        "QOH99_DDRAW_EFFECT_DIAGNOSTIC", value, (DWORD)sizeof(value));
    if (length != 1u || value[0] != '1')
        return;

    gdi32 = GetModuleHandleA("GDI32.dll");
    if (!gdi32)
        gdi32 = LoadLibraryA("GDI32.dll");
    real = gdi32 ? GetProcAddress(gdi32, "Ellipse") : NULL;
    if (!real) {
        LOG("GDI effect diagnostic disabled: GDI32!Ellipse unavailable");
        return;
    }
    g_realEllipse = (Qoh99EllipseFn)real;
    result = wrapper_iat_patch(
        "GDI32.dll", "Ellipse", (void *)Hook_Ellipse, &original);
    if (result > 0) {
        if (original && original != (void *)Hook_Ellipse)
            g_realEllipse = (Qoh99EllipseFn)original;
        g_gdiEffectDiagnosticEnabled = TRUE;
        InterlockedExchange(&g_gdiEffectDiagnosticBudget, 3);
        qoh99_iat_txn_commit(&g_wrapperIatTxn);
        LOG("GDI effect diagnostic enabled: bounded Ellipse capture budget=3");
    } else {
        LOG("GDI effect diagnostic disabled: host does not expose a patchable GDI32!Ellipse import");
    }
}

static void hook_dinput_iat(void)
{
    static const char *const kDInputDlls[2] = { "dinput.dll", "dinput8.dll" };
    HMODULE dinput;
    FARPROC dinputCreateEx;
    FARPROC dinputCreateA;
    int hookCount = 0;
    int hardFailure = 0;
    size_t i;

    if (g_dinputHooked)
        return;

    if (!GetModuleHandleA(NULL)) {
        WLOG_ISSUE("DirectInput hook: GetModuleHandle(NULL) failed");
        return;
    }

    dinput = LoadLibraryA("DINPUT.dll");
    dinputCreateEx = dinput ? GetProcAddress(dinput, "DirectInputCreateEx") : NULL;
    dinputCreateA  = dinput ? GetProcAddress(dinput, "DirectInputCreateA") : NULL;

    if (!g_realDirectInputCreateEx && dinputCreateEx)
        g_realDirectInputCreateEx = (PFN_DirectInputCreateEx)dinputCreateEx;
    if (!g_realDirectInputCreateA && dinputCreateA)
        g_realDirectInputCreateA = (PFN_DirectInputCreateA)dinputCreateA;

    LOG("DirectInput hook: dinput=%p procEx=%p procA=%p",
        (void *)dinput, (void *)dinputCreateEx, (void *)dinputCreateA);

    for (i = 0; i < 2 && !hardFailure; ++i) {
        const char *dll = kDInputDlls[i];
        void *originalEx = NULL;
        void *originalA = NULL;
        int result;

        result = wrapper_iat_patch(dll, "DirectInputCreateEx",
                                   (void *)Hook_DirectInputCreateEx,
                                   &originalEx);
        if (result < 0) {
            hardFailure = 1;
        } else if (result > 0) {
            hookCount += result;
            if (originalEx != NULL
                && originalEx != (void *)Hook_DirectInputCreateEx) {
                g_realDirectInputCreateEx =
                    (PFN_DirectInputCreateEx)originalEx;
            }
        }

        if (hardFailure)
            break;

        result = wrapper_iat_patch(dll, "DirectInputCreateA",
                                   (void *)Hook_DirectInputCreateA,
                                   &originalA);
        if (result < 0) {
            hardFailure = 1;
        } else if (result > 0) {
            hookCount += result;
            if (originalA != NULL
                && originalA != (void *)Hook_DirectInputCreateA) {
                g_realDirectInputCreateA = (PFN_DirectInputCreateA)originalA;
            }
        }
    }

    if (hardFailure) {
        const int restored = qoh99_iat_txn_rollback(&g_wrapperIatTxn);
        LOG("DirectInput hook: aborted, rolled back %d slot(s)", restored);
        return;
    }

    if (hookCount > 0) {
        qoh99_iat_txn_commit(&g_wrapperIatTxn);
        g_dinputHooked = TRUE;
        LOG("DirectInput hook installed (%d slot(s))", hookCount);
        return;
    }

    LOG("DirectInput hook: not installed (no dinput import) realProcEx=%p realProcA=%p",
        (void *)dinputCreateEx, (void *)dinputCreateA);
}

#define PACE_MAX_WAIT_ITERATIONS 20000u
#define PACE_MAX_WAIT_MS         100u
#define PACE_SLEEP_MARGIN_MS     2

static double  g_frameCarryTicks = 0.0;
static BOOL    g_qpcChecked = FALSE;
static BOOL    g_qpcUsable = FALSE;
static LONG    g_timerPeriodHeld = 0;
static DWORD   g_paceLateFrames = 0;
static DWORD   g_paceDeadlineResets = 0;
static DWORD   g_paceSleeps = 0;
static DWORD   g_paceYields = 0;
static DWORD   g_paceBailouts = 0;
static DWORD   g_paceCalls = 0;

typedef UINT (WINAPI *PFN_timePeriod)(UINT);

static PFN_timePeriod pace_time_period(const char *name)
{
    static HMODULE s_winmm = NULL;
    static LONG s_tried = 0;

    if (!InterlockedCompareExchange(&s_tried, 1, 0))
        s_winmm = LoadLibraryA("winmm.dll");
    if (!s_winmm)
        return NULL;
    return (PFN_timePeriod)GetProcAddress(s_winmm, name);
}

static void pace_acquire_timer_period(void)
{
    PFN_timePeriod begin;

    if (InterlockedCompareExchange(&g_timerPeriodHeld, 0, 0) != 0)
        return;
    begin = pace_time_period("timeBeginPeriod");
    if (!begin) {
        LOG("pacing: timeBeginPeriod unavailable; using default timer resolution");
        return;
    }
    if (begin(1) == 0 ) {
        InterlockedExchange(&g_timerPeriodHeld, 1);
        LOG("pacing: timer resolution raised to 1 ms");
    } else {
        LOG("pacing: timeBeginPeriod(1) refused; using default timer resolution");
    }
}

static void pace_release_timer_period(void)
{
    PFN_timePeriod end;
    const LONG held = InterlockedExchange(&g_timerPeriodHeld, 0);

    if (held == 0)
        return;
    end = pace_time_period("timeEndPeriod");
    if (end) {
        end((UINT)held);
        LOG("pacing: timer resolution released (%ld ms)", held);
    }
}

static void pace_log_summary(void)
{
    LOG("[pacing] calls=%lu late=%lu resets=%lu sleeps=%lu yields=%lu bailouts=%lu",
        (unsigned long)g_paceCalls,
        (unsigned long)g_paceLateFrames,
        (unsigned long)g_paceDeadlineResets,
        (unsigned long)g_paceSleeps,
        (unsigned long)g_paceYields,
        (unsigned long)g_paceBailouts);
}

static BOOL pace_qpc_usable(void)
{
    if (g_qpcChecked)
        return g_qpcUsable;
    g_qpcChecked = TRUE;

    ZeroMemory(&g_qpcFreq, sizeof(g_qpcFreq));
    if (!QueryPerformanceFrequency(&g_qpcFreq)) {
        WLOG_ISSUE("pacing: QueryPerformanceFrequency failed (error %lu); frame limiting DISABLED",
            GetLastError());
        return FALSE;
    }
    if (g_qpcFreq.QuadPart < 1000LL || g_qpcFreq.QuadPart > 100000000000LL) {
        LOG("pacing: QueryPerformanceFrequency implausible (%lld Hz); frame limiting DISABLED",
            (long long)g_qpcFreq.QuadPart);
        return FALSE;
    }
    LOG("pacing: QPC frequency %lld Hz", (long long)g_qpcFreq.QuadPart);
    g_qpcUsable = TRUE;
    return TRUE;
}

static void fps_throttle(void)
{
    if (qoh99_netplay_owns_pacing())
        return;

    const LONG fpsLimit =
        InterlockedCompareExchange(&g_runtimeFpsLimit, 0, 0);
    const LONG generation =
        InterlockedCompareExchange(&g_fpsLimitGeneration, 0, 0);
    LARGE_INTEGER now;
    double frameTicksExact;
    double advance;
    LONGLONG frameTicksWhole;
    unsigned int iterations = 0;
    unsigned int yields = 0;
    unsigned int sleeps = 0;
    DWORD waitStartTick;

    if (!pace_qpc_usable())
        return;

    if (!g_qpcInit) {
        QueryPerformanceCounter(&g_nextFrameQpc);
        g_frameCarryTicks = 0.0;
        g_qpcInit = TRUE;
    }

    if (generation != g_appliedFpsLimitGeneration) {
        QueryPerformanceCounter(&g_nextFrameQpc);
        g_frameCarryTicks = 0.0;
        g_appliedFpsLimitGeneration = generation;
        ++g_paceDeadlineResets;
    }

    if (fpsLimit <= 0)
        return;

    pace_acquire_timer_period();
    ++g_paceCalls;

    frameTicksExact = (double)g_qpcFreq.QuadPart / (double)fpsLimit;
    if (frameTicksExact < 1.0)
        frameTicksExact = 1.0;

    QueryPerformanceCounter(&now);
    waitStartTick = GetTickCount();
    if (now.QuadPart < g_nextFrameQpc.QuadPart) {
        for (;;) {
            LONGLONG remaining = g_nextFrameQpc.QuadPart - now.QuadPart;
            LONGLONG remainingMs;

            if (remaining <= 0)
                break;
            remainingMs = remaining * 1000LL / g_qpcFreq.QuadPart;
            if (remainingMs > PACE_SLEEP_MARGIN_MS) {
                Sleep((DWORD)(remainingMs - PACE_SLEEP_MARGIN_MS));
                ++sleeps;
            } else {
                if (!SwitchToThread())
                    Sleep(0);
                ++yields;
            }
            if (++iterations >= PACE_MAX_WAIT_ITERATIONS
                || (GetTickCount() - waitStartTick) > PACE_MAX_WAIT_MS) {
                ++g_paceBailouts;
                LOG("pacing: wait bail-out after %u iterations / %lu ms (limit=%ld)",
                    iterations, (unsigned long)(GetTickCount() - waitStartTick),
                    fpsLimit);
                break;
            }
            QueryPerformanceCounter(&now);
        }
    } else {
        ++g_paceLateFrames;
    }
    g_paceSleeps += sleeps;
    g_paceYields += yields;

    advance = frameTicksExact + g_frameCarryTicks;
    frameTicksWhole = (LONGLONG)advance;
    g_frameCarryTicks = advance - (double)frameTicksWhole;
    g_nextFrameQpc.QuadPart += frameTicksWhole;

    QueryPerformanceCounter(&now);
    if (g_nextFrameQpc.QuadPart < now.QuadPart - frameTicksWhole) {
        g_nextFrameQpc = now;
        g_frameCarryTicks = 0.0;
        ++g_paceDeadlineResets;
    }

    if ((g_paceCalls % 3600u) == 1u)
        pace_log_summary();
}

static void reset_fps_deadline(void)
{
    if (!g_qpcInit)
        return;

    QueryPerformanceCounter(&g_nextFrameQpc);
    g_frameCarryTicks = 0.0;
    g_fpsPacedThisFlip = FALSE;
}

static BYTE *alloc_pixels(
    int w,
    int h,
    int bytesPP,
    int *pitchOut,
    size_t *pixelBytesOut)
{
    DdwSurfaceLayout layout;
    BYTE *pixels;

    if (pitchOut)
        *pitchOut = 0;
    if (pixelBytesOut)
        *pixelBytesOut = 0;
    if (!pitchOut || !pixelBytesOut
        || !ddw_compute_surface_layout(w, h, bytesPP, &layout))
        return NULL;

    pixels = (BYTE *)calloc(1, layout.buffer_bytes);
    if (!pixels)
        return NULL;

    *pitchOut = layout.pitch;
    *pixelBytesOut = layout.buffer_bytes;
    return pixels;
}

static DdwSurfaceView surface_view(MyDDSurface *surface)
{
    DdwSurfaceView view;

    ZeroMemory(&view, sizeof(view));
    if (!surface)
        return view;
    view.pixels = surface->pixels;
    view.width = surface->width;
    view.height = surface->height;
    view.pitch = surface->pitch;
    view.bytes_per_pixel = surface->bytesPP;
    view.buffer_bytes = surface->pixelBytes;
    view.format = surface->format;
    return view;
}

static DdwRect surface_rect(const RECT *rect, int width, int height)
{
    DdwRect result;

    if (rect) {
        result.left = (int32_t)rect->left;
        result.top = (int32_t)rect->top;
        result.right = (int32_t)rect->right;
        result.bottom = (int32_t)rect->bottom;
    } else {
        result.left = 0;
        result.top = 0;
        result.right = width;
        result.bottom = height;
    }
    return result;
}

static int32_t signed_bltfast_coordinate(DWORD coordinate)
{
    if (coordinate <= (DWORD)INT32_MAX)
        return (int32_t)coordinate;
    return (int32_t)((int64_t)coordinate - INT64_C(4294967296));
}

static void compute_letterbox(int screenW, int screenH, int gameW, int gameH,
                               int *outX, int *outY, int *outW, int *outH)
{
    ddw_letterbox(screenW, screenH, gameW, gameH, outX, outY, outW, outH);
}

static void get_work_area_for_window(HWND hWnd, RECT *wa)
{
    if (wa == NULL)
        return;
    if (hWnd) {
        HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (mon && GetMonitorInfoA(mon, &mi)) {
            *wa = mi.rcWork;
            return;
        }
    }
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, wa, 0)) {
        wa->left = 0;
        wa->top = 0;
        wa->right = GetSystemMetrics(SM_CXSCREEN);
        wa->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

static int max_integer_scale_for_workarea(HWND hWnd, int gameW, int gameH)
{
    RECT wa;
    int sx, sy, s;
    if (gameW <= 0 || gameH <= 0) return 1;
    get_work_area_for_window(hWnd, &wa);
    sx = (wa.right - wa.left) / gameW;
    sy = (wa.bottom - wa.top) / gameH;
    s = (sx < sy) ? sx : sy;
    if (s < 1) s = 1;
    return s;
}

static void window_border_size(HWND hWnd, LONG style, LONG exStyle,
                               int *borderW, int *borderH)
{
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    typedef BOOL (WINAPI *AdjustForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static GetDpiForWindowFn s_getDpi = NULL;
    static AdjustForDpiFn s_adjustForDpi = NULL;
    static LONG s_resolved = 0;
    RECT br = { 0, 0, 0, 0 };
    BOOL adjusted = FALSE;

    if (!InterlockedCompareExchange(&s_resolved, 0, 0)) {
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32) {
            s_getDpi = (GetDpiForWindowFn)GetProcAddress(
                user32, "GetDpiForWindow");
            s_adjustForDpi = (AdjustForDpiFn)GetProcAddress(
                user32, "AdjustWindowRectExForDpi");
        }
        InterlockedExchange(&s_resolved, 1);
    }
    if (hWnd && s_getDpi && s_adjustForDpi) {
        UINT dpi = s_getDpi(hWnd);
        if (dpi != 0)
            adjusted = s_adjustForDpi(&br, (DWORD)style, FALSE,
                                      (DWORD)exStyle, dpi);
    }
    if (!adjusted)
        AdjustWindowRectEx(&br, style, FALSE, exStyle);
    if (borderW) *borderW = br.right - br.left;
    if (borderH) *borderH = br.bottom - br.top;
}

static int rounded_mul_div_int(int value, int mul, int div)
{
    if (div == 0)
        return value;
    return (int)(((long long)value * (long long)mul + (long long)div / 2) / (long long)div);
}

static void clamp_aspect_client_to_workarea(HWND hWnd,
                                            int *clientW, int *clientH,
                                            int gameW, int gameH,
                                            int borderW, int borderH)
{
    RECT wa;
    int maxClientW;
    int maxClientH;

    if (clientW == NULL || clientH == NULL || gameW <= 0 || gameH <= 0)
        return;

    if (*clientW < gameW)
        *clientW = gameW;
    if (*clientH < gameH)
        *clientH = gameH;

    get_work_area_for_window(hWnd, &wa);
    maxClientW = (wa.right - wa.left) - borderW;
    maxClientH = (wa.bottom - wa.top) - borderH;
    if (maxClientW < gameW)
        maxClientW = gameW;
    if (maxClientH < gameH)
        maxClientH = gameH;

    if (*clientW > maxClientW) {
        *clientW = maxClientW;
        *clientH = rounded_mul_div_int(*clientW, gameH, gameW);
    }
    if (*clientH > maxClientH) {
        *clientH = maxClientH;
        *clientW = rounded_mul_div_int(*clientH, gameW, gameH);
    }

    if (*clientW < gameW) {
        *clientW = gameW;
        *clientH = rounded_mul_div_int(*clientW, gameH, gameW);
    }
    if (*clientH < gameH) {
        *clientH = gameH;
        *clientW = rounded_mul_div_int(*clientH, gameW, gameH);
    }
}

static void aspect_client_from_drag(HWND hWnd,
                                    WPARAM sizingEdge,
                                    int proposedClientW,
                                    int proposedClientH,
                                    int gameW,
                                    int gameH,
                                    int borderW,
                                    int borderH,
                                    int snapToIntegerScale,
                                    int *outClientW,
                                    int *outClientH)
{
    int clientW = proposedClientW;
    int clientH = proposedClientH;

    if (clientW < gameW)
        clientW = gameW;
    if (clientH < gameH)
        clientH = gameH;

    switch (sizingEdge) {
        case WMSZ_LEFT:
        case WMSZ_RIGHT:
            clientH = rounded_mul_div_int(clientW, gameH, gameW);
            break;
        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            clientW = rounded_mul_div_int(clientH, gameW, gameH);
            break;
        default:
            if ((long long)clientW * (long long)gameH > (long long)clientH * (long long)gameW)
                clientW = rounded_mul_div_int(clientH, gameW, gameH);
            else
                clientH = rounded_mul_div_int(clientW, gameH, gameW);
            break;
    }

    if (snapToIntegerScale && gameW > 0) {
        int k = (clientW + gameW / 2) / gameW;
        if (k < 1) k = 1;
        if (clientW - k * gameW <= gameW / 10
            && k * gameW - clientW <= gameW / 10) {
            clientW = k * gameW;
            clientH = k * gameH;
        }
    }

    clamp_aspect_client_to_workarea(hWnd, &clientW, &clientH, gameW, gameH,
                                    borderW, borderH);
    *outClientW = clientW;
    *outClientH = clientH;
}

#define CURSOR_ADJUST_MAX_STEPS 32

static void apply_cursor_visibility(BOOL show)
{
    static int s_state = -1;
    static LONG s_loggedGiveUp = 0;
    int want = show ? 1 : 0;
    int steps = 0;

    if (s_state == want)
        return;
    s_state = want;
    if (show) {
        while (ShowCursor(TRUE) < 0 && ++steps < CURSOR_ADJUST_MAX_STEPS) {}
    } else {
        while (ShowCursor(FALSE) >= 0 && ++steps < CURSOR_ADJUST_MAX_STEPS) {}
    }
    if (steps >= CURSOR_ADJUST_MAX_STEPS
        && InterlockedExchange(&s_loggedGiveUp, 1) == 0) {
        LOG("cursor: ShowCursor counter did not converge after %d steps (show=%d); giving up",
            CURSOR_ADJUST_MAX_STEPS, (int)show);
    }
    g_cursorHidden = !show;
}

static void update_cursor_idle(void)
{
    if (EFZDX9_Presenter_ShouldSuppressGameInput()) {
        apply_cursor_visibility(TRUE);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return;
    }
    if (!g_cursorHidden && (GetTickCount() - g_lastMouseMoveTick) >= CURSOR_IDLE_HIDE_MS)
        apply_cursor_visibility(FALSE);
}

static DWORD sample_surface_hash(MyDDSurface *src)
{
    DWORD hash = 2166136261u;
    size_t total;
    size_t step;
    size_t i;

    if (!src || !src->pixels || src->width <= 0 || src->height <= 0)
        return 0;

    total = src->pixelBytes;
    step = total / 1024u;
    if (step == 0)
        step = 1;

    for (i = 0; i < total; i += step) {
        hash ^= src->pixels[i];
        hash *= 16777619u;
    }
    return hash;
}

enum FrameSourceKind {
    FRAMESRC_PRIMARY = 0,
    FRAMESRC_QOH99_SCALE_320x240 = 1
};

static MyDDSurface     *g_frameSource     = NULL;
static enum FrameSourceKind g_frameSourceKind = FRAMESRC_PRIMARY;

static BYTE   *g_snapPixels   = NULL;
static size_t  g_snapCapacity = 0;
static int     g_snapW = 0, g_snapH = 0, g_snapPitch = 0;
static DdwSurfaceFormat g_snapFormat;
static RGBQUAD g_snapPalette[256];
static BOOL    g_snapHasPalette = FALSE;
static BOOL    g_snapValid = FALSE;

typedef struct DdwPresenterBitmapInfo {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[256];
} DdwPresenterBitmapInfo;

static int configure_present_bitmap_info(
    DdwPresenterBitmapInfo *bmi,
    int width,
    int height,
    int pitch,
    const DdwSurfaceFormat *format,
    const RGBQUAD *palette)
{
    if (!bmi || !format || width <= 0 || height <= 0 || pitch <= 0)
        return 0;
    ZeroMemory(bmi, sizeof(*bmi));
    bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi->bmiHeader.biHeight = -height;
    bmi->bmiHeader.biPlanes = 1;
    if (format->kind == DDW_PIXEL_FORMAT_INDEX8) {
        if (format->bits_per_pixel != 8u || pitch < width)
            return 0;
        bmi->bmiHeader.biWidth = pitch;
        bmi->bmiHeader.biBitCount = 8;
        bmi->bmiHeader.biCompression = BI_RGB;
        bmi->bmiHeader.biClrUsed = 256;
        if (palette)
            memcpy(bmi->bmiColors, palette, 256u * sizeof(RGBQUAD));
        return 1;
    }
    if (!ddw_surface_format_is_presentable(format)
        || format->bits_per_pixel != 16u || pitch < width * 2)
        return 0;
    bmi->bmiHeader.biWidth = pitch / 2;
    bmi->bmiHeader.biBitCount = 16;
    bmi->bmiHeader.biCompression = BI_BITFIELDS;
    ((DWORD *)bmi->bmiColors)[0] = format->red_mask;
    ((DWORD *)bmi->bmiColors)[1] = format->green_mask;
    ((DWORD *)bmi->bmiColors)[2] = format->blue_mask;
    return 1;
}

static void qoh99_snapshot_store(const MyDDSurface *src, const MyDDPalette *pal)
{
    if (!src || !src->pixels || src->width <= 0 || src->height <= 0
        || src->pixelBytes == 0)
        return;
    if (src->pixelBytes > g_snapCapacity) {
        BYTE *grown = (BYTE *)realloc(g_snapPixels, src->pixelBytes);
        if (!grown)
            return;
        g_snapPixels = grown;
        g_snapCapacity = src->pixelBytes;
    }
    memcpy(g_snapPixels, src->pixels, src->pixelBytes);
    g_snapW = src->width;
    g_snapH = src->height;
    g_snapPitch = src->pitch;
    g_snapFormat = src->format;
    g_snapHasPalette = FALSE;
    if (src->bytesPP == 1 && pal) {
        memcpy(g_snapPalette, pal->entries, sizeof(g_snapPalette));
        g_snapHasPalette = TRUE;
    }
    g_snapValid = TRUE;
}

typedef struct {
    int matched;
    int src_w, src_h, dst_w, dst_h;
    int dst_origin_x, dst_origin_y;
} Qoh99FrameBltInfo;

static Qoh99FrameBltInfo detect_qoh99_frame_scale_blt(
    MyDDSurface *dst, MyDDSurface *src,
    const RECT *srcRect, const RECT *dstRect)
{
    Qoh99FrameBltInfo info;
    DdwRect source;
    DdwRect destination;

    ZeroMemory(&info, sizeof(info));
    if (!dst || !src)
        return info;

    source = surface_rect(srcRect, src->width, src->height);
    destination = surface_rect(dstRect, dst->width, dst->height);
    info.matched = ddw_is_qoh99_frame_commit(
        dst->isPrimary,
        src->isPrimary,
        dst->width,
        dst->height,
        src->width,
        src->height,
        destination,
        source);
    if (info.matched) {
        info.src_w = 320;
        info.src_h = 240;
        info.dst_w = (int)(destination.right - destination.left);
        info.dst_h = (int)(destination.bottom - destination.top);
        info.dst_origin_x = destination.left;
        info.dst_origin_y = destination.top;
    }
    return info;
}

/* Sole game-frame boundary: rollback/instrument state advances only for the canonical completed-frame scale Blt. */
static BOOL qoh99_frame_commit(void)
{
    qoh99_instr_on_frame();

    if (InterlockedExchange(&g_qoh99NetplayBootstrapped, 1) == 0) {
        HMODULE h = GetModuleHandleA("qoh99_netplay.dll");
        if (!h && GetFileAttributesA("qoh99_netplay.dll") != INVALID_FILE_ATTRIBUTES)
            h = LoadLibraryA("qoh99_netplay.dll");
        if (h)
            g_qoh99BootTick =
                (Qoh99VoidCallbackFn)GetProcAddress(
                    h, "qoh99np_boot_tick");
        if (h)
            g_qoh99PresentationTryAcquireSlotV2 =
                (Qoh99PresentationTryAcquireSlotV2Fn)GetProcAddress(
                    h, "qoh99np_presentation_try_acquire_slot_v2");
        if (h)
            g_qoh99PresentationCommit =
                (Qoh99VoidCallbackFn)GetProcAddress(
                    h, "qoh99np_presentation_commit");
        if (h)
            g_qoh99PresentationFailedV3 =
                (Qoh99PresentationFailedV3Fn)GetProcAddress(
                    h, "qoh99np_presentation_failed_v3");
        LOG("netplay bootstrap: dll=%p bootTick=%p "
            "presentationTryAcquireSlotV2=%p presentationCommit=%p "
            "presentationFailedV3=%p",
            (void *)h, (void *)g_qoh99BootTick,
            (void *)g_qoh99PresentationTryAcquireSlotV2,
            (void *)g_qoh99PresentationCommit,
            (void *)g_qoh99PresentationFailedV3);
    }
    if (g_qoh99BootTick)
        g_qoh99BootTick();
    if (g_qoh99PresentationTryAcquireSlotV2)
        return g_qoh99PresentationTryAcquireSlotV2() ? TRUE : FALSE;
    return TRUE;
}

static void qoh99_frame_published(void)
{
    if (g_qoh99PresentationCommit)
        g_qoh99PresentationCommit();
}

static void qoh99_frame_publish_failed(DWORD reason)
{
    if (g_qoh99PresentationFailedV3)
        g_qoh99PresentationFailedV3(reason);
}

static DWORD qoh99_presentation_disposition(void)
{
    if (!g_qoh99PresentationDisposition
        && InterlockedCompareExchange(
               &g_qoh99PresentationQueryAttempted, 0, 0) != 0)
        return QOH99_PRESENT_NORMAL;

    if (!g_qoh99PresentationDisposition) {
        HMODULE netplay = GetModuleHandleA("qoh99_netplay.dll");
        if (netplay) {
            g_qoh99PresentationDisposition =
                (Qoh99PresentationDispositionFn)GetProcAddress(
                    netplay, "qoh99np_presentation_disposition");
            g_qoh99PresentationReport =
                (Qoh99PresentationReportFn)GetProcAddress(
                    netplay, "qoh99np_presentation_report");
            if (InterlockedExchange(
                    &g_qoh99PresentationQueryAttempted, 1) == 0) {
                LOG("netplay presentation query: dll=%p fn=%p report=%p",
                    (void *)netplay,
                    (void *)g_qoh99PresentationDisposition,
                    (void *)g_qoh99PresentationReport);
            }
        }
    }
    if (!g_qoh99PresentationDisposition)
        return QOH99_PRESENT_NORMAL;

    __try {
        DWORD disposition = g_qoh99PresentationDisposition();
        if (disposition == QOH99_PRESENT_SUPPRESS_HISTORICAL
            || disposition == QOH99_PRESENT_CORRECTED_FINAL)
            return disposition;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        WLOG_ISSUE("netplay presentation query faulted; failing visible");
        g_qoh99PresentationDisposition = NULL;
        InterlockedExchange(&g_qoh99PresentationQueryAttempted, 2);
    }
    return QOH99_PRESENT_NORMAL;
}

static BOOL qoh99_netplay_owns_pacing(void)
{
    if (!g_qoh99OwnsPacing
        && InterlockedCompareExchange(
               &g_qoh99OwnsPacingQueryAttempted, 0, 0) != 0)
        return FALSE;

    if (!g_qoh99OwnsPacing) {
        HMODULE netplay = GetModuleHandleA("qoh99_netplay.dll");
        if (netplay) {
            g_qoh99OwnsPacing =
                (Qoh99OwnsPacingFn)GetProcAddress(
                    netplay, "qoh99np_owns_pacing");
            if (InterlockedExchange(
                    &g_qoh99OwnsPacingQueryAttempted, 1) == 0) {
                LOG("netplay pacing owner query: dll=%p fn=%p",
                    (void *)netplay, (void *)g_qoh99OwnsPacing);
            }
        }
    }
    if (!g_qoh99OwnsPacing)
        return FALSE;

    __try {
        return g_qoh99OwnsPacing() != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("netplay pacing owner query faulted; disabling query");
        g_qoh99OwnsPacing = NULL;
        InterlockedExchange(&g_qoh99OwnsPacingQueryAttempted, 2);
    }
    return FALSE;
}

static BOOL qoh99_netplay_owns_network_pacing(void)
{
    if (!g_qoh99OwnsNetworkPacing
        && InterlockedCompareExchange(
               &g_qoh99OwnsNetworkPacingQueryAttempted, 0, 0) == 0) {
        HMODULE netplay = GetModuleHandleA("qoh99_netplay.dll");
        if (netplay) {
            g_qoh99OwnsNetworkPacing =
                (Qoh99OwnsPacingFn)GetProcAddress(
                    netplay, "qoh99np_owns_network_pacing");
            if (InterlockedExchange(
                    &g_qoh99OwnsNetworkPacingQueryAttempted, 1) == 0) {
                LOG("netplay network-pacing owner query: dll=%p fn=%p",
                    (void *)netplay, (void *)g_qoh99OwnsNetworkPacing);
            }
        }
    }
    if (!g_qoh99OwnsNetworkPacing)
        return qoh99_netplay_owns_pacing();

    __try {
        return g_qoh99OwnsNetworkPacing() != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("netplay network-pacing owner query faulted; disabling query");
        g_qoh99OwnsNetworkPacing = NULL;
        InterlockedExchange(&g_qoh99OwnsNetworkPacingQueryAttempted, 2);
    }
    return qoh99_netplay_owns_pacing();
}

static LONG qoh99_dc_counter_value(const volatile LONG *counter)
{
    return counter ? InterlockedCompareExchange((volatile LONG *)counter, 0, 0)
                   : 0;
}

static void qoh99_log_dc_summary(const char *reason)
{
    LOG("[ddraw-gdi] reason=%s getDcCalls=%ld getDcSuccesses=%ld "
        "getDcFailures=%ld unsupportedFormatRejects=%ld releaseDcCalls=%ld "
        "releaseDcFailures=%ld visibleWritebacks=%ld historicalDiscards=%ld "
        "nestedDcRejects=%ld lockDcConflicts=%ld surfaceRecreations=%ld",
        reason ? reason : "summary",
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.getDcCalls),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.getDcSuccesses),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.getDcFailures),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.unsupportedFormatRejects),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.releaseDcCalls),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.releaseDcFailures),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.visibleWritebacks),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.historicalDiscards),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.nestedDcRejects),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.lockDcConflicts),
        qoh99_dc_counter_value(&g_qoh99DdrawDcCounters.surfaceRecreations));
}

static void qoh99_log_presentation_counters(void)
{
    const DWORD total = g_qoh99NormalVisiblePresents
        + g_qoh99SuppressedPresents
        + g_qoh99CorrectedFinalPresents;
    if (g_qoh99PresentationReport) {
        __try {
            g_qoh99PresentationReport(
                g_qoh99NormalVisiblePresents,
                g_qoh99SuppressedPresents,
                g_qoh99CorrectedFinalPresents);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("netplay presentation report faulted; disabling telemetry");
            g_qoh99PresentationReport = NULL;
        }
    }
    if (total <= 8 || (total % 300) == 0) {
        LOG("[netplay-present] normalVisiblePresents=%lu "
            "suppressedPresents=%lu correctedFinalPresents=%lu "
            "hiddenColorFills=%lu hiddenBlts=%lu hiddenBltFasts=%lu "
            "hiddenPacingCalls=%lu",
            (unsigned long)g_qoh99NormalVisiblePresents,
            (unsigned long)g_qoh99SuppressedPresents,
            (unsigned long)g_qoh99CorrectedFinalPresents,
            (unsigned long)g_qoh99SuppressedColorFills,
             (unsigned long)g_qoh99SuppressedBlts,
             (unsigned long)g_qoh99SuppressedBltFasts,
             (unsigned long)g_qoh99SuppressedPacingCalls);
        qoh99_log_dc_summary("presentation-sample");
    }
}

static void qoh99_record_visible_present(DWORD disposition)
{
    if (disposition == QOH99_PRESENT_CORRECTED_FINAL)
        ++g_qoh99CorrectedFinalPresents;
    else
        ++g_qoh99NormalVisiblePresents;
    qoh99_log_presentation_counters();
}

static void qoh99_record_suppressed_present(void)
{
    ++g_qoh99SuppressedPresents;
    qoh99_log_presentation_counters();
}

static BOOL present_frame(MyDDraw *dd, BOOL canonicalFrame)
{
    g_qoh99LastPublishFailure = QOH99_PUBLISH_FAILURE_NONE;
    if (qoh99_movie_is_playing())
        return FALSE;
    if (qoh99_netplay_owns_pacing() && !canonicalFrame) {
        ++g_qoh99SuppressedPacingCalls;
        return FALSE;
    }
    const DWORD disposition = qoh99_presentation_disposition();
    if (disposition == QOH99_PRESENT_SUPPRESS_HISTORICAL) {
        qoh99_record_suppressed_present();
        g_qoh99LastPublishFailure =
            QOH99_PUBLISH_FAILURE_DISPOSITION;
        return FALSE;
    }

    DWORD presentNo = ++g_presentSerial;
    BOOL traceThisPresent = FALSE;

    if (g_renderTraceAfterFocus && g_renderTraceBudget > 0) {
        traceThisPresent = TRUE;
        g_renderTraceBudget--;
        if (g_renderTraceBudget == 0)
            g_renderTraceAfterFocus = FALSE;
    } else if (presentNo <= 8 || (presentNo % 300) == 0) {
        traceThisPresent = TRUE;
    }

    if (!dd || !dd->hWnd) {
        g_qoh99LastPublishFailure =
            QOH99_PUBLISH_FAILURE_NO_TARGET;
        LOG("Render present skipped frame=%u reason=no-dd-or-hwnd dd=%p",
            presentNo, (void *)dd);
        return FALSE;
    }

    enum FrameSourceKind srcKind = FRAMESRC_PRIMARY;
    MyDDSurface *src;
    if (g_frameSource && g_frameSource->pixels) {
        src     = g_frameSource;
        srcKind = g_frameSourceKind;
    } else {
        src = (dd->primarySurface && dd->primarySurface->attachedBackBuffer)
            ? dd->primarySurface->attachedBackBuffer
            : dd->primarySurface;
    }
    if (!src || !src->pixels) {
        g_qoh99LastPublishFailure =
            QOH99_PUBLISH_FAILURE_NO_SOURCE;
        LOG("Render present skipped frame=%u reason=no-source primary=%p back=%p active=%d focusSerial=%u",
            presentNo,
            dd->primarySurface ? (void *)dd->primarySurface : NULL,
            (dd->primarySurface && dd->primarySurface->attachedBackBuffer)
                ? (void *)dd->primarySurface->attachedBackBuffer : NULL,
            g_appActive, g_focusSerial);
        return FALSE;
    }

    MyDDPalette *pal = NULL;
    if (src->format.kind == DDW_PIXEL_FORMAT_INDEX8) {
        pal = src->palette
            ? src->palette
            : (dd->primarySurface ? dd->primarySurface->palette : NULL);
    }
    DdwPresenterBitmapInfo bmi;
    if (!configure_present_bitmap_info(
            &bmi,
            src->width,
            src->height,
            src->pitch,
            &src->format,
            pal ? pal->entries : NULL)) {
        g_qoh99LastPublishFailure = QOH99_PUBLISH_FAILURE_NO_SOURCE;
        LOG("Render present skipped frame=%u reason=unsupported-format surf=%p bpp=%lu masks=%08lX/%08lX/%08lX/%08lX",
            presentNo,
            (void *)src,
            (unsigned long)src->format.bits_per_pixel,
            (unsigned long)src->format.red_mask,
            (unsigned long)src->format.green_mask,
            (unsigned long)src->format.blue_mask,
            (unsigned long)src->format.alpha_mask);
        return FALSE;
    }

    if (g_overlayCallback && src->format.kind == DDW_PIXEL_FORMAT_INDEX8) {
        EfzDdrawOverlayFrame frame;
        frame.structSize = sizeof(frame);
        frame.pixels = src->pixels;
        frame.width = src->width;
        frame.height = src->height;
        frame.pitch = src->pitch;
        frame.hWnd = dd->hWnd;
        frame.palette = pal ? pal->entries : NULL;
        frame.presentSerial = presentNo;

        __try {
            g_overlayCallback(&frame, g_overlayCallbackUserData);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("Overlay callback faulted during present frame=%u; disabling callback",
                presentNo);
            g_overlayCallback = NULL;
            g_overlayCallbackUserData = NULL;
        }
    }

    RECT cr;
    GetClientRect(dd->hWnd, &cr);
    int fullW = cr.right  - cr.left;
    int fullH = cr.bottom - cr.top;
    if (fullW <= 0 || fullH <= 0) {
        g_qoh99LastPublishFailure =
            QOH99_PUBLISH_FAILURE_EMPTY_CLIENT;
        LOG("Render present skipped frame=%u reason=empty-client client=%dx%d hwnd=%p active=%d focusSerial=%u",
            presentNo, fullW, fullH, (void *)dd->hWnd, g_appActive,
            g_focusSerial);
        return FALSE;
    }

    update_cursor_idle();

    int dstX, dstY, dstW, dstH;
    if (src->width > 0 && src->height > 0) {
        compute_letterbox(fullW, fullH, src->width, src->height,
                          &dstX, &dstY, &dstW, &dstH);
    } else {
        dstX = 0; dstY = 0; dstW = fullW; dstH = fullH;
    }

    if (!EFZDX9_Presenter_HasDevice()) {
        EFZDX9_Presenter_RequestDeviceIfNeeded(
            dd->hWnd, fullW, fullH, dd->presentationMode,
            g_cfg.renderer_backend);
    }

    EfzDx9PresentParams dx9Params;
    {
        ZeroMemory(&dx9Params, sizeof(dx9Params));
        dx9Params.window = dd->hWnd;
        dx9Params.pixels = src->pixels;
        dx9Params.width = src->width;
        dx9Params.height = src->height;
        dx9Params.pitch = src->pitch;
        dx9Params.palette = pal ? pal->entries : NULL;
        dx9Params.clientWidth = fullW;
        dx9Params.clientHeight = fullH;
        dx9Params.dstX = dstX;
        dx9Params.dstY = dstY;
        dx9Params.dstW = dstW;
        dx9Params.dstH = dstH;
        dx9Params.requestedMode = g_cfg.presentation_mode;
        dx9Params.activeMode = dd->presentationMode;
        dx9Params.appActive = g_appActive;
        dx9Params.linearScale = &g_cfg.linear_scale;
        dx9Params.presentSerial = presentNo;
        dx9Params.focusSerial = g_focusSerial;
        dx9Params.presentationKind = canonicalFrame
            ? EFZ_DX9_PRESENT_CANONICAL : g_repaintPresentationKind;
        dx9Params.requestPresentationMode =
            RequestPresentationModeFromPresenter;
        dx9Params.requestPresentationModeUserData = dd;
        dx9Params.bytesPerPixel = src->bytesPP;
        dx9Params.format = src->format;

        if (EFZDX9_Presenter_Present(&dx9Params)) {
            if (canonicalFrame)
                qoh99_snapshot_store(src, pal);
            qoh99_record_visible_present(disposition);
            qoh99_display_restore_tick(dd, presentNo);
            return TRUE;
        }
        LOGV("present %u: DX9 declined (device=%d netplayPaced=%d client=%dx%d bpp=%d)",
             presentNo, (int)EFZDX9_Presenter_HasDevice(),
             (int)qoh99_netplay_owns_pacing(), fullW, fullH, src->bytesPP);
    }

    {
        EfzDx9PresentationStatus status;
        ZeroMemory(&status, sizeof(status));
        status.structSize = sizeof(status);
        if (EFZDX9_Presenter_GetPresentationStatus(
                &status, sizeof(status))
            && !qoh99_presentation_backend_allows_gdi(
                status.effectiveMode, status.effectiveBackend)) {
            g_qoh99LastPublishFailure = QOH99_PUBLISH_FAILURE_GDI;
            LOGV("present %u: GDI suppressed during transient Exclusive loss "
                 "readiness=%u hr=%08lX",
                 presentNo, (unsigned)status.readiness,
                 (unsigned long)status.lastError);
            return FALSE;
        }
    }

    HDC hDC = GetDC(dd->hWnd);
    if (!hDC) {
        g_qoh99LastPublishFailure =
            QOH99_PUBLISH_FAILURE_GET_DC;
        LOG("Render present skipped frame=%u reason=GetDC-failed hwnd=%p active=%d focusSerial=%u lastError=%lu",
            presentNo, (void *)dd->hWnd, g_appActive, g_focusSerial,
            GetLastError());
        return FALSE;
    }

    if (src->width > 0 && src->height > 0
        && (dstX > 0 || dstY > 0 || dstX + dstW < fullW || dstY + dstH < fullH)) {
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bar;
        if (dstY > 0) {
            bar.left = 0; bar.top = 0; bar.right = fullW; bar.bottom = dstY;
            FillRect(hDC, &bar, black);
        }
        if (dstY + dstH < fullH) {
            bar.left = 0; bar.top = dstY + dstH; bar.right = fullW; bar.bottom = fullH;
            FillRect(hDC, &bar, black);
        }
        if (dstX > 0) {
            bar.left = 0; bar.top = dstY; bar.right = dstX; bar.bottom = dstY + dstH;
            FillRect(hDC, &bar, black);
        }
        if (dstX + dstW < fullW) {
            bar.left = dstX + dstW; bar.top = dstY; bar.right = fullW; bar.bottom = dstY + dstH;
            FillRect(hDC, &bar, black);
        }
    }

    if (g_cfg.linear_scale) {
        SetStretchBltMode(hDC, HALFTONE);
        SetBrushOrgEx(hDC, 0, 0, NULL);
    } else {
        SetStretchBltMode(hDC, COLORONCOLOR);
    }
    LARGE_INTEGER gdiEntryQpc;
    LARGE_INTEGER gdiReturnQpc;
    ZeroMemory(&gdiEntryQpc, sizeof(gdiEntryQpc));
    ZeroMemory(&gdiReturnQpc, sizeof(gdiReturnQpc));
    QueryPerformanceCounter(&gdiEntryQpc);
    int stretchResult = StretchDIBits(hDC,
                                      dstX, dstY, dstW, dstH,
                                      0, 0, src->width, src->height,
                                      src->pixels,
                                      (BITMAPINFO *)&bmi,
                                      DIB_RGB_COLORS,
                                      SRCCOPY);
    QueryPerformanceCounter(&gdiReturnQpc);
    EFZDX9_Presenter_RecordGdiPresentation(
        &dx9Params, gdiEntryQpc.QuadPart, gdiReturnQpc.QuadPart,
        (stretchResult == GDI_ERROR || stretchResult == 0) ? E_FAIL : S_OK);
    if (traceThisPresent || stretchResult == GDI_ERROR || stretchResult == 0) {
        LOG("Render present frame=%u flip=%u frameSrc=%d active=%d focusSerial=%u focusAgeMs=%lu hwnd=%p client=%dx%d src=%dx%d pitch=%d dst=%d,%d %dx%d mode=%d pal=%d hash=%08X StretchDIBits=%d lastError=%lu",
            presentNo, g_flipSerial, (int)srcKind, g_appActive, g_focusSerial,
            GetTickCount() - g_lastFocusTick, (void *)dd->hWnd,
            fullW, fullH, src->width, src->height, src->pitch,
            dstX, dstY, dstW, dstH, (int)dd->presentationMode, pal != NULL,
            sample_surface_hash(src), stretchResult, GetLastError());
    }
    ReleaseDC(dd->hWnd, hDC);
    if (stretchResult == GDI_ERROR || stretchResult == 0)
    {
        g_qoh99LastPublishFailure =
            QOH99_PUBLISH_FAILURE_GDI;
        return FALSE;
    }
    if (canonicalFrame)
        qoh99_snapshot_store(src, pal);
    qoh99_record_visible_present(disposition);
    qoh99_display_restore_tick(dd, presentNo);
    return TRUE;
}

static void set_windowed_mode(MyDDraw *dd)
{
    HWND hWnd = dd->hWnd;
    if (!hWnd || dd->gameWidth <= 0) return;

    InterlockedIncrement(&g_presentationGeometryChange);

    LONG style   = WS_OVERLAPPEDWINDOW;
    LONG exStyle = WS_EX_APPWINDOW;

    SetWindowLongA(hWnd, GWL_STYLE,   style);
    SetWindowLongA(hWnd, GWL_EXSTYLE, exStyle);

    if (dd->hasSavedWinRect
        && MonitorFromRect(&dd->savedWinRect, MONITOR_DEFAULTTONULL) != NULL) {
        SetWindowPos(hWnd, HWND_TOP,
                     dd->savedWinRect.left, dd->savedWinRect.top,
                     dd->savedWinRect.right - dd->savedWinRect.left,
                     dd->savedWinRect.bottom - dd->savedWinRect.top,
                     SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        dd->presentationMode = QOH_PRESENTATION_WINDOWED;
        LOG("Window mode: windowed restored %ldx%ld at (%ld,%ld)",
            dd->savedWinRect.right - dd->savedWinRect.left,
            dd->savedWinRect.bottom - dd->savedWinRect.top,
            dd->savedWinRect.left, dd->savedWinRect.top);
        InterlockedDecrement(&g_presentationGeometryChange);
        return;
    }

    int maxScale = max_integer_scale_for_workarea(hWnd, dd->gameWidth, dd->gameHeight);
    int scale = 1;
    if (InterlockedCompareExchange(&g_displayRestoreDone, 0, 0) != 0
        && g_cfg.window_scale > 0)
        scale = g_cfg.window_scale;
    if (scale > maxScale) scale = maxScale;
    if (scale < 1) scale = 1;
    int borderW, borderH;
    window_border_size(hWnd, style, exStyle, &borderW, &borderH);
    int winW = dd->gameWidth * scale + borderW;
    int winH = dd->gameHeight * scale + borderH;

    RECT wa;
    get_work_area_for_window(hWnd, &wa);
    int posX = wa.left + ((wa.right - wa.left) - winW) / 2;
    int posY = wa.top + ((wa.bottom - wa.top) - winH) / 2;
    if (posX < wa.left) posX = wa.left;
    if (posY < wa.top) posY = wa.top;

    SetWindowPos(hWnd, HWND_TOP,
                 posX, posY, winW, winH,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    dd->presentationMode = QOH_PRESENTATION_WINDOWED;
    LOG("Window mode: windowed %dx%d (game %dx%d) at (%d,%d)",
        winW, winH, dd->gameWidth, dd->gameHeight, posX, posY);
    InterlockedDecrement(&g_presentationGeometryChange);
}

static void set_fullscreen_window_mode(
    MyDDraw *dd, QohPresentationMode mode)
{
    HWND hWnd = dd->hWnd;
    if (!hWnd || (mode != QOH_PRESENTATION_BORDERLESS
                  && mode != QOH_PRESENTATION_EXCLUSIVE)) return;

    InterlockedIncrement(&g_presentationGeometryChange);

    if (dd->presentationMode == QOH_PRESENTATION_WINDOWED) {
        GetWindowRect(hWnd, &dd->savedWinRect);
        dd->savedStyle   = GetWindowLongA(hWnd, GWL_STYLE);
        dd->savedExStyle = GetWindowLongA(hWnd, GWL_EXSTYLE);
        dd->hasSavedWinRect = TRUE;
    }

    SetWindowLongA(hWnd, GWL_STYLE,   WS_POPUP | WS_VISIBLE);
    SetWindowLongA(hWnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

    HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    RECT r;
    if (GetMonitorInfoA(mon, &mi)) {
        r = mi.rcMonitor;
    } else {
        r.left = 0; r.top = 0;
        r.right = GetSystemMetrics(SM_CXSCREEN);
        r.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    SetWindowPos(hWnd, HWND_TOP,
                 r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    dd->presentationMode = mode;
    dd->lastFullscreenMode = mode;
    LOG("Window mode: %s %dx%d at (%d,%d)",
        mode == QOH_PRESENTATION_EXCLUSIVE ? "exclusive-requested"
                                            : "borderless",
        r.right - r.left, r.bottom - r.top, r.left, r.top);
    InterlockedDecrement(&g_presentationGeometryChange);
}

static BOOL present_snapshot(MyDDraw *dd)
{
    if (!dd || !dd->hWnd || !g_snapValid || !g_snapPixels)
        return FALSE;

    DdwPresenterBitmapInfo bmi;
    if (!configure_present_bitmap_info(
            &bmi,
            g_snapW,
            g_snapH,
            g_snapPitch,
            &g_snapFormat,
            g_snapHasPalette ? g_snapPalette : NULL)) {
        LOG("Snapshot present refused unsupported source format bpp=%lu masks=%08lX/%08lX/%08lX/%08lX",
            (unsigned long)g_snapFormat.bits_per_pixel,
            (unsigned long)g_snapFormat.red_mask,
            (unsigned long)g_snapFormat.green_mask,
            (unsigned long)g_snapFormat.blue_mask,
            (unsigned long)g_snapFormat.alpha_mask);
        return FALSE;
    }

    RECT cr;
    GetClientRect(dd->hWnd, &cr);
    int fullW = cr.right - cr.left;
    int fullH = cr.bottom - cr.top;
    if (fullW <= 0 || fullH <= 0)
        return FALSE;

    int dstX, dstY, dstW, dstH;
    compute_letterbox(fullW, fullH, g_snapW, g_snapH,
                      &dstX, &dstY, &dstW, &dstH);

    EfzDx9PresentParams params;
    {
        ZeroMemory(&params, sizeof(params));
        params.window = dd->hWnd;
        params.pixels = g_snapPixels;
        params.width = g_snapW;
        params.height = g_snapH;
        params.pitch = g_snapPitch;
        params.palette = g_snapHasPalette ? g_snapPalette : NULL;
        params.clientWidth = fullW;
        params.clientHeight = fullH;
        params.dstX = dstX;
        params.dstY = dstY;
        params.dstW = dstW;
        params.dstH = dstH;
        params.requestedMode = g_cfg.presentation_mode;
        params.activeMode = dd->presentationMode;
        params.appActive = g_appActive;
        params.linearScale = &g_cfg.linear_scale;
        params.presentSerial = g_flipSerial;
        params.focusSerial = g_focusSerial;
        params.presentationKind = EFZ_DX9_PRESENT_OTHER_REPAINT;
        params.requestPresentationMode = RequestPresentationModeFromPresenter;
        params.requestPresentationModeUserData = dd;
        params.bytesPerPixel = g_snapFormat.kind == DDW_PIXEL_FORMAT_INDEX8
            ? 1 : 2;
        params.format = g_snapFormat;
        if (EFZDX9_Presenter_Present(&params))
            return TRUE;
        {
            EfzDx9PresentationStatus status;
            ZeroMemory(&status, sizeof(status));
            if (EFZDX9_Presenter_GetPresentationStatus(
                    &status, sizeof(status))
                && !qoh99_presentation_backend_allows_gdi(
                    status.effectiveMode, status.effectiveBackend))
                return FALSE;
        }
    }

    HDC hDC = GetDC(dd->hWnd);
    if (!hDC)
        return FALSE;
    {
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bar;
        if (dstY > 0) {
            bar.left = 0; bar.top = 0; bar.right = fullW; bar.bottom = dstY;
            FillRect(hDC, &bar, black);
        }
        if (dstY + dstH < fullH) {
            bar.left = 0; bar.top = dstY + dstH; bar.right = fullW; bar.bottom = fullH;
            FillRect(hDC, &bar, black);
        }
        if (dstX > 0) {
            bar.left = 0; bar.top = dstY; bar.right = dstX; bar.bottom = dstY + dstH;
            FillRect(hDC, &bar, black);
        }
        if (dstX + dstW < fullW) {
            bar.left = dstX + dstW; bar.top = dstY; bar.right = fullW; bar.bottom = dstY + dstH;
            FillRect(hDC, &bar, black);
        }
    }
    if (g_cfg.linear_scale) {
        SetStretchBltMode(hDC, HALFTONE);
        SetBrushOrgEx(hDC, 0, 0, NULL);
    } else {
        SetStretchBltMode(hDC, COLORONCOLOR);
    }
    LARGE_INTEGER gdiEntryQpc;
    LARGE_INTEGER gdiReturnQpc;
    ZeroMemory(&gdiEntryQpc, sizeof(gdiEntryQpc));
    ZeroMemory(&gdiReturnQpc, sizeof(gdiReturnQpc));
    QueryPerformanceCounter(&gdiEntryQpc);
    int stretchResult = StretchDIBits(hDC,
                                      dstX, dstY, dstW, dstH,
                                      0, 0, g_snapW, g_snapH,
                                      g_snapPixels,
                                      (BITMAPINFO *)&bmi,
                                      DIB_RGB_COLORS,
                                      SRCCOPY);
    QueryPerformanceCounter(&gdiReturnQpc);
    EFZDX9_Presenter_RecordGdiPresentation(
        &params, gdiEntryQpc.QuadPart, gdiReturnQpc.QuadPart,
        (stretchResult == GDI_ERROR || stretchResult == 0) ? E_FAIL : S_OK);
    ReleaseDC(dd->hWnd, hDC);
    return stretchResult != GDI_ERROR && stretchResult != 0;
}

static void present_current(MyDDraw *dd)
{
    if (!dd || !dd->hWnd)
        return;
    if (qoh99_netplay_owns_pacing()) {
        present_snapshot(dd);
        return;
    }
    if (dd->primarySurface)
        present_frame(dd, FALSE);
}

static void request_presentation_mode(
    MyDDraw *dd, QohPresentationMode mode)
{
    RECT cr;
    if (!dd || !qoh99_presentation_mode_valid((int)mode)) return;
    g_cfg.presentation_mode = mode;
    g_restorePresentationMode = mode;
    if (mode != QOH_PRESENTATION_WINDOWED)
        dd->lastFullscreenMode = mode;
    win_state_set("PresentationMode", (int)mode);
    win_state_set("Borderless",
                  qoh99_presentation_legacy_borderless_mirror(mode));
    if (qoh99_netplay_owns_network_pacing()
        && (mode == QOH_PRESENTATION_EXCLUSIVE
            || dd->presentationMode == QOH_PRESENTATION_EXCLUSIVE)) {
        g_deferredPresentationMode = mode;
        InterlockedExchange(&g_deferredPresentationChange, 1);
        LOG("presentation mode %d queued until the active peer session ends",
            (int)mode);
        return;
    }
    InterlockedExchange(&g_deferredPresentationChange, 0);
    if (mode == QOH_PRESENTATION_WINDOWED) {
        set_windowed_mode(dd);
    } else {
        if (dd->presentationMode == QOH_PRESENTATION_WINDOWED)
            win_state_save_client_size(dd->hWnd);
        set_fullscreen_window_mode(dd, mode);
    }
    if (GetClientRect(dd->hWnd, &cr)
        && cr.right > cr.left && cr.bottom > cr.top) {
        EFZDX9_Presenter_RequestDeviceIfNeeded(
            dd->hWnd, cr.right - cr.left, cr.bottom - cr.top,
            mode, g_cfg.renderer_backend);
    }
    present_current(dd);
}

static void toggle_window_mode(MyDDraw *dd)
{
    if (!dd) return;
    request_presentation_mode(
        dd, qoh99_preferred_fullscreen_toggle(
                g_cfg.presentation_mode, dd->lastFullscreenMode));
}

static void __stdcall RequestPresentationModeFromPresenter(
    QohPresentationMode mode, void *userData)
{
    request_presentation_mode((MyDDraw *)userData, mode);
}

static BOOL build_windowed_restore_rect(
    MyDDraw *dd, RECT *outRect, int *outClientW, int *outClientH)
{
    int gameW;
    int gameH;
    int clientW;
    int clientH;
    int borderW = 0;
    int borderH = 0;
    RECT current;
    RECT workArea;
    int winW;
    int winH;
    int posX;
    int posY;

    if (!dd || !dd->hWnd || !IsWindow(dd->hWnd) || !outRect)
        return FALSE;

    gameW = dd->gameWidth > 0 ? dd->gameWidth : 640;
    gameH = dd->gameHeight > 0 ? dd->gameHeight : 480;
    clientW = g_restoreClientW;
    clientH = g_restoreClientH;
    if (clientW <= 0 || clientH <= 0) {
        if (g_cfg.window_scale <= 1)
            return FALSE;
        clientW = gameW * g_cfg.window_scale;
        clientH = gameH * g_cfg.window_scale;
    }

    window_border_size(
        dd->hWnd, WS_OVERLAPPEDWINDOW, WS_EX_APPWINDOW,
        &borderW, &borderH);
    clamp_aspect_client_to_workarea(
        dd->hWnd, &clientW, &clientH,
        gameW, gameH, borderW, borderH);
    if (clientW <= 0 || clientH <= 0 || !GetWindowRect(dd->hWnd, &current))
        return FALSE;

    winW = clientW + borderW;
    winH = clientH + borderH;
    posX = current.left;
    posY = current.top;
    get_work_area_for_window(dd->hWnd, &workArea);
    if (posX + winW > workArea.right)
        posX = workArea.right - winW;
    if (posY + winH > workArea.bottom)
        posY = workArea.bottom - winH;
    if (posX < workArea.left)
        posX = workArea.left;
    if (posY < workArea.top)
        posY = workArea.top;

    outRect->left = posX;
    outRect->top = posY;
    outRect->right = posX + winW;
    outRect->bottom = posY + winH;
    if (outClientW) *outClientW = clientW;
    if (outClientH) *outClientH = clientH;
    return TRUE;
}

static void apply_display_restore(MyDDraw *dd)
{
    RECT windowedRect;
    int clientW = 0;
    int clientH = 0;

    if (!dd || !dd->hWnd || !IsWindow(dd->hWnd))
        return;
    if (IsZoomed(dd->hWnd))
        return;

    if (g_restorePresentationMode != QOH_PRESENTATION_WINDOWED) {
        if (dd->presentationMode == QOH_PRESENTATION_WINDOWED) {
            const BOOL haveWindowedRestore = build_windowed_restore_rect(
                dd, &windowedRect, &clientW, &clientH);
            set_fullscreen_window_mode(dd, g_restorePresentationMode);
            if (haveWindowedRestore) {
                dd->savedWinRect = windowedRect;
                dd->hasSavedWinRect = TRUE;
                LOG("display restore: preserved windowed client %dx%d for F11",
                    clientW, clientH);
            }
            present_current(dd);
        }
        LOG("display restore: mode=%d applied from %s",
            (int)g_restorePresentationMode, wrapper_ini_path());
        return;
    }

    if (!build_windowed_restore_rect(
            dd, &windowedRect, &clientW, &clientH))
        return;

    SetWindowPos(
        dd->hWnd, HWND_TOP,
        windowedRect.left, windowedRect.top,
        windowedRect.right - windowedRect.left,
        windowedRect.bottom - windowedRect.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    LOG("display restore: client %dx%d (window %dx%d at %d,%d) from %s",
        clientW, clientH,
        windowedRect.right - windowedRect.left,
        windowedRect.bottom - windowedRect.top,
        windowedRect.left, windowedRect.top, wrapper_ini_path());
    present_current(dd);
}

static void qoh99_display_restore_tick(MyDDraw *dd, DWORD presentNo)
{
    if (InterlockedCompareExchange(&g_displayRestorePending, 0, 0) == 0)
        return;
    if (presentNo < DISPLAY_RESTORE_AFTER_PRESENTS)
        return;
    if (!dd || !dd->hWnd || !IsWindow(dd->hWnd))
        return;
    if (InterlockedExchange(&g_displayRestorePending, 0) == 0)
        return;
    InterlockedExchange(&g_displayRestoreDone, 1);
    PostMessageA(dd->hWnd, WM_QOH99_APPLY_DISPLAY, 0, 0);
}

static BYTE wrapper_dik_from_window_lparam(LPARAM lParam)
{
    BYTE dik = (BYTE)((((DWORD_PTR)lParam) >> 16u) & 0xFFu);
    if ((((DWORD_PTR)lParam) & (1u << 24u)) != 0)
        dik = (BYTE)(dik | 0x80u);
    return dik;
}

static BOOL wrapper_dispatch_hotkey(
    BYTE dik, BOOL firstPress, BOOL repeat)
{
    if (wrapper_hotkey_matches(
            WRAPPER_HOTKEY_TOGGLE_PRESENTATION, dik)) {
        if (firstPress && g_dd) toggle_window_mode(g_dd);
        return TRUE;
    }
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_TOGGLE_MENU, dik)) {
        if (firstPress && !qoh99_netplay_owns_network_pacing()) {
            EFZDX9_Presenter_ToggleMenu();
            apply_cursor_visibility(TRUE);
            SetCursor(LoadCursor(NULL, IDC_ARROW));
        }
        return TRUE;
    }
    if (wrapper_hotkey_matches(
            WRAPPER_HOTKEY_TOGGLE_INPUT_VIEWER, dik)) {
        if (firstPress) EFZDX9_Presenter_ToggleInputDisplay();
        return TRUE;
    }
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_PAUSE, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TRANSPORT_TOGGLE_PAUSE, FALSE);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_STEP, dik))
        return EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TRANSPORT_STEP, repeat);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_STEP_BACK, dik))
        return EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TRANSPORT_STEP_BACK, repeat);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_RESTART, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TRANSPORT_RESTART, FALSE);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_SLOWER, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TRANSPORT_SPEED_SLOWER, FALSE);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_FASTER, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TRANSPORT_SPEED_FASTER, FALSE);
    if (wrapper_hotkey_matches(
            WRAPPER_HOTKEY_REPLAY_TOGGLE_COLLISION_DISPLAY, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TOGGLE_COLLISION_DISPLAY, FALSE);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_TAKEOVER_EXIT, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TAKEOVER_EXIT, FALSE);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_TAKEOVER_P1, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TAKEOVER_P1, FALSE);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_TAKEOVER_P2, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TAKEOVER_P2, FALSE);
    if (wrapper_hotkey_matches(WRAPPER_HOTKEY_REPLAY_TAKEOVER_RETRY, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TAKEOVER_RETRY, FALSE);
    if (wrapper_hotkey_matches(
            WRAPPER_HOTKEY_REPLAY_TAKEOVER_DELAY_DECREASE, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TAKEOVER_DELAY_DECREASE, FALSE);
    if (wrapper_hotkey_matches(
            WRAPPER_HOTKEY_REPLAY_TAKEOVER_DELAY_INCREASE, dik))
        return firstPress && EFZDX9_Presenter_SubmitReplayHotkey(
            QOH99_REPLAY_TAKEOVER_DELAY_INCREASE, FALSE);
    return FALSE;
}

#define WM_QOH99_HOTKEY (WM_APP + 0x51)

static LRESULT CALLBACK LLKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
        BYTE dik = (BYTE)(kb->scanCode & 0x7Fu);
        BOOL keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        BOOL keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        BOOL globalHotkey;
        if ((kb->flags & LLKHF_EXTENDED) != 0)
            dik = (BYTE)(dik | 0x80u);
        globalHotkey =
            wrapper_hotkey_matches(
                WRAPPER_HOTKEY_TOGGLE_PRESENTATION, dik)
            || wrapper_hotkey_matches(WRAPPER_HOTKEY_TOGGLE_MENU, dik)
            || wrapper_hotkey_matches(
                WRAPPER_HOTKEY_TOGGLE_INPUT_VIEWER, dik);
        if (g_dd && g_dd->hWnd && g_appActive
            && is_foreground_for_hwnd(g_dd->hWnd)) {
            if (globalHotkey && (keyDown || keyUp)) {
                if (keyDown) {
                    const BOOL firstPress = !g_wrapperHotkeyDown[dik];
                    g_wrapperHotkeyDown[dik] = 1u;
                    PostMessageA(g_dd->hWnd, WM_QOH99_HOTKEY,
                                 (WPARAM)dik,
                                 firstPress ? 1 : 2);
                } else {
                    g_wrapperHotkeyDown[dik] = 0u;
                }
                return 1;
            }
            if (keyDown && (kb->vkCode == VK_F2 || kb->vkCode == VK_F3))
                PostMessageA(g_dd->hWnd, WM_QOH99_HOTKEY, kb->vkCode, 0);
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

#define HOTKEY_HOOK_MAX_ATTEMPTS 3

static LONG g_kbHookAttempts = 0;
static LONG g_kbHookFailureLogged = 0;

static void install_hotkey_hook(void)
{
    if (g_kbHook != NULL)
        return;
    if (InterlockedIncrement(&g_kbHookAttempts) > HOTKEY_HOOK_MAX_ATTEMPTS)
        return;

    g_kbHook = SetWindowsHookExA(WH_KEYBOARD_LL, LLKeyboardProc, g_hInst, 0);
    if (g_kbHook != NULL) {
        LOG("LL keyboard hook installed (%p, attempt %ld)",
            (void *)g_kbHook, InterlockedCompareExchange(&g_kbHookAttempts, 0, 0));
        return;
    }
    if (InterlockedExchange(&g_kbHookFailureLogged, 1) == 0) {
        LOG("LL keyboard hook unavailable (error %lu); F11/F2/F3 continue on the window message path",
            GetLastError());
    }
}

static void release_hotkey_hook(void)
{
    HHOOK hook = g_kbHook;
    if (hook == NULL)
        return;
    g_kbHook = NULL;
    if (!UnhookWindowsHookEx(hook))
        LOG("LL keyboard hook release failed (error %lu)", GetLastError());
}

static LRESULT CALLBACK WrapperWndProc(HWND hWnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_QOH99_D3D_DEVICE_WORK) {
        EFZDX9_Presenter_ProcessDeviceWorkOnWindowThread(hWnd);
        return 0;
    }
    if (msg == WM_QOH99_D3D_SHUTDOWN) {
        EFZDX9_Presenter_Shutdown();
        return 0;
    }
    if (g_dd && !qoh99_netplay_owns_network_pacing()
        && InterlockedExchange(&g_deferredPresentationChange, 0) != 0) {
        request_presentation_mode(g_dd, g_deferredPresentationMode);
    }
    if (msg == WM_DISPLAYCHANGE && g_dd && hWnd == g_dd->hWnd) {
        RECT cr;
        if (GetClientRect(hWnd, &cr)
            && cr.right > cr.left && cr.bottom > cr.top) {
            EFZDX9_Presenter_RequestDeviceIfNeeded(
                hWnd, cr.right - cr.left, cr.bottom - cr.top,
                g_dd->presentationMode, g_cfg.renderer_backend);
        }
    }
    if (msg == WM_ACTIVATE) {
        BOOL newActive = (LOWORD(wParam) != WA_INACTIVE);
        if (newActive != g_appActive || LOWORD(wParam) == WA_INACTIVE) {
            g_focusSerial++;
            g_lastFocusTick = GetTickCount();
            g_renderTraceAfterFocus = TRUE;
            g_renderTraceBudget = 20;
            g_wndTraceBudget = 80;
            LOG("Focus WM_ACTIVATE serial=%u active %d->%d state=%u minimized=%u hwnd=%p other=%p fg=%p",
                g_focusSerial, g_appActive, newActive, LOWORD(wParam),
                HIWORD(wParam), (void *)hWnd, (void *)lParam,
                (void *)GetForegroundWindow());
            reset_fps_deadline();
        }
        g_appActive = newActive;
        if (!newActive)
            ZeroMemory(g_wrapperHotkeyDown, sizeof(g_wrapperHotkeyDown));
        EFZDX9_Presenter_SetActive(newActive);
        if (g_dd && g_dd->origWndProc)
            return CallWindowProcA(g_dd->origWndProc, hWnd, msg, wParam, lParam);
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    } else if (msg == WM_ACTIVATEAPP) {
        BOOL newActive = (wParam != FALSE);
        if (newActive != g_appActive || !newActive) {
            g_focusSerial++;
            g_lastFocusTick = GetTickCount();
            g_renderTraceAfterFocus = TRUE;
            g_renderTraceBudget = 20;
            g_wndTraceBudget = 80;
            LOG("Focus WM_ACTIVATEAPP serial=%u active %d->%d thread=%lu hwnd=%p fg=%p",
                g_focusSerial, g_appActive, newActive, (DWORD)lParam,
                (void *)hWnd, (void *)GetForegroundWindow());
            reset_fps_deadline();
        }
        g_appActive = newActive;
        if (!newActive)
            ZeroMemory(g_wrapperHotkeyDown, sizeof(g_wrapperHotkeyDown));
        EFZDX9_Presenter_SetActive(newActive);
        if (g_dd && g_dd->origWndProc)
            return CallWindowProcA(g_dd->origWndProc, hWnd, msg, wParam, lParam);
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        LOG("Focus %s active=%d hwnd=%p other=%p fg=%p focusSerial=%u",
            msg == WM_SETFOCUS ? "WM_SETFOCUS" : "WM_KILLFOCUS",
            g_appActive, (void *)hWnd, (void *)wParam,
            (void *)GetForegroundWindow(), g_focusSerial);
        if (g_dd && g_dd->origWndProc)
            return CallWindowProcA(g_dd->origWndProc, hWnd, msg, wParam, lParam);
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    } else if (msg == WM_PAINT && g_renderTraceAfterFocus) {
        LOG("Render WM_PAINT after-focus active=%d hwnd=%p primary=%p focusSerial=%u",
            g_appActive, (void *)hWnd,
            g_dd ? (void *)g_dd->primarySurface : NULL, g_focusSerial);
    } else if ((msg == WM_SIZE || msg == WM_WINDOWPOSCHANGED) && g_renderTraceAfterFocus) {
        LOG("Window msg=%s active=%d hwnd=%p wParam=%08X lParam=%08X focusSerial=%u",
            msg == WM_SIZE ? "WM_SIZE" : "WM_WINDOWPOSCHANGED",
            g_appActive, (void *)hWnd, (DWORD)wParam, (DWORD)lParam,
            g_focusSerial);
    } else if (g_wndTraceBudget > 0) {
        LOG("Window msg after-focus msg=%04X active=%d hwnd=%p wParam=%08X lParam=%08X focusSerial=%u",
            msg, g_appActive, (void *)hWnd, (DWORD)wParam, (DWORD)lParam,
            g_focusSerial);
        g_wndTraceBudget--;
    }

    if (msg == WM_MOUSEMOVE) {
        g_lastMouseMoveTick = GetTickCount();
        apply_cursor_visibility(TRUE);
    }

    if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && g_dd
        && EFZDX9_Presenter_ShouldSuppressGameInput()) {
        apply_cursor_visibility(TRUE);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return TRUE;
    }

    if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && g_dd
        && !EFZDX9_Presenter_ShouldSuppressGameInput()) {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        if ((GetTickCount() - g_lastMouseMoveTick) < CURSOR_IDLE_HIDE_MS)
            apply_cursor_visibility(TRUE);
        return TRUE;
    }

    if (msg == WM_MOVING || msg == WM_ENTERSIZEMOVE)
        return DefWindowProcA(hWnd, msg, wParam, lParam);

    if (msg == 0x02E0 && g_dd
        && g_dd->presentationMode == QOH_PRESENTATION_WINDOWED) {
        const RECT *suggested = (const RECT *)lParam;
        if (suggested) {
            SetWindowPos(hWnd, NULL, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            present_current(g_dd);
        }
        return 0;
    }

    if (msg == WM_SIZING && g_dd
        && g_dd->presentationMode == QOH_PRESENTATION_WINDOWED) {
        RECT *wr = (RECT *)lParam;
        int gameW = g_dd->gameWidth  > 0 ? g_dd->gameWidth  : 640;
        int gameH = g_dd->gameHeight > 0 ? g_dd->gameHeight : 480;
        LONG style   = GetWindowLongA(hWnd, GWL_STYLE);
        LONG exStyle = GetWindowLongA(hWnd, GWL_EXSTYLE);
        int borderW, borderH;
        window_border_size(hWnd, style, exStyle, &borderW, &borderH);
        int propW = (wr->right - wr->left) - borderW;
        int propH = (wr->bottom - wr->top) - borderH;
        int clientW = gameW;
        int clientH = gameH;
        int snap = (GetKeyState(VK_CONTROL) & 0x8000) == 0;
        aspect_client_from_drag(hWnd, wParam, propW, propH, gameW, gameH,
                                borderW, borderH, snap, &clientW, &clientH);
        {
            int newW = clientW + borderW;
            int newH = clientH + borderH;
            switch (wParam) {
                case WMSZ_LEFT: case WMSZ_TOPLEFT: case WMSZ_BOTTOMLEFT:
                    wr->left = wr->right - newW; break;
                default:
                    wr->right = wr->left + newW; break;
            }
            switch (wParam) {
                case WMSZ_TOP: case WMSZ_TOPLEFT: case WMSZ_TOPRIGHT:
                    wr->top = wr->bottom - newH; break;
                default:
                    wr->bottom = wr->top + newH; break;
            }
        }
        return TRUE;
    }

    if (msg == WM_GETMINMAXINFO && g_dd
        && g_dd->presentationMode == QOH_PRESENTATION_WINDOWED) {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        int gameW = g_dd->gameWidth  > 0 ? g_dd->gameWidth  : 640;
        int gameH = g_dd->gameHeight > 0 ? g_dd->gameHeight : 480;
        int borderW, borderH;
        window_border_size(hWnd, GetWindowLongA(hWnd, GWL_STYLE),
                           GetWindowLongA(hWnd, GWL_EXSTYLE),
                           &borderW, &borderH);
        mmi->ptMinTrackSize.x = gameW + borderW;
        mmi->ptMinTrackSize.y = gameH + borderH;
        return 0;
    }

    if (msg == WM_SIZE && wParam != SIZE_MINIMIZED) {
        qoh99_movie_window_resized();
    }
    if (msg == WM_SIZE && wParam != SIZE_MINIMIZED && g_dd) {
        present_current(g_dd);
    } else if (msg == WM_EXITSIZEMOVE && g_dd) {
        present_current(g_dd);
        if (g_dd->presentationMode == QOH_PRESENTATION_WINDOWED)
            win_state_save_client_size(hWnd);
    }

    if (msg == WM_QOH99_HOTKEY && g_dd) {
        if (lParam == 1 || lParam == 2)
            (void)wrapper_dispatch_hotkey(
                (BYTE)wParam, lParam == 1, lParam == 2);
        else if (wParam == VK_F2)
            qoh99_instr_reset();
        else if (wParam == VK_F3)
            qoh99_instr_dump("F3");
        return 0;
    }

    if (msg == WM_QOH99_APPLY_DISPLAY && g_dd) {
        apply_display_restore(g_dd);
        return 0;
    }

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
        && g_dd && g_appActive
        && is_foreground_for_hwnd(g_dd->hWnd)) {
        const BOOL firstPress = (lParam & (1UL << 30)) == 0;
        if (wrapper_dispatch_hotkey(
                wrapper_dik_from_window_lparam(lParam), firstPress,
                firstPress ? FALSE : TRUE))
            return 0;
    }

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
        && g_kbHook == NULL && g_dd && g_appActive
        && (lParam & (1UL << 30)) == 0
        && is_foreground_for_hwnd(g_dd->hWnd)) {
        if (wParam == VK_F2)
            qoh99_instr_reset();
        else if (wParam == VK_F3)
            qoh99_instr_dump("F3");
    }

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN
         || msg == WM_KEYUP || msg == WM_SYSKEYUP)
        && wParam == VK_F1) {
        return 0;
    }

    {
        LRESULT imguiResult = 0;
        if (EFZDX9_Presenter_HandleWindowMessage(hWnd, msg, wParam, lParam, &imguiResult))
            return imguiResult;
    }

    if (msg == WM_SYSCOMMAND) {
        WPARAM cmd = wParam & 0xFFF0;
        if (cmd == SC_SCREENSAVE || cmd == SC_MONITORPOWER)
            return 0;
        if (cmd == SC_KEYMENU)
            return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    if (msg == WM_DESTROY && g_dd && hWnd == g_dd->hWnd)
        wrapper_release_runtime("WM_DESTROY");

    if (msg == WM_ERASEBKGND) return 1;

    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        if (g_dd)
            present_current(g_dd);
        (void)hdc;
        EndPaint(hWnd, &ps);
        return 0;
    }
    if (g_dd && g_dd->origWndProc)
        return CallWindowProcA(g_dd->origWndProc, hWnd, msg, wParam, lParam);
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static ULONG   __stdcall DDPal_AddRef(MyDDPalette *p)
    { return (ULONG)InterlockedIncrement(&p->refCount); }

static ULONG   __stdcall DDPal_Release(MyDDPalette *p)
{
    LONG ref = InterlockedDecrement(&p->refCount);
    if (ref == 0) { LOG("IDirectDrawPalette freed"); free(p); }
    return (ULONG)ref;
}

static HRESULT __stdcall DDPal_QueryInterface(MyDDPalette *p, REFIID r, void **ppv)
    { *ppv = p; DDPal_AddRef(p); return DD_OK; }

static HRESULT __stdcall DDPal_GetCaps(MyDDPalette *p, DWORD *pdwCaps)
    { if (pdwCaps) *pdwCaps = DDPCAPS_8BIT | DDPCAPS_ALLOW256; return DD_OK; }

static HRESULT __stdcall DDPal_GetEntries(MyDDPalette *p,
    DWORD dwFlags, DWORD dwBase, DWORD dwNumEntries, PALETTEENTRY *pEntries)
{
    for (DWORD i = 0; i < dwNumEntries && (dwBase + i) < 256; i++) {
        pEntries[i].peRed   = p->entries[dwBase + i].rgbRed;
        pEntries[i].peGreen = p->entries[dwBase + i].rgbGreen;
        pEntries[i].peBlue  = p->entries[dwBase + i].rgbBlue;
        pEntries[i].peFlags = 0;
    }
    return DD_OK;
}

static HRESULT __stdcall DDPal_Initialize(MyDDPalette *p, void *pDD,
    DWORD dwFlags, PALETTEENTRY *pEntries) { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDPal_SetEntries(MyDDPalette *p,
    DWORD dwFlags, DWORD dwBase, DWORD dwNumEntries, PALETTEENTRY *pEntries)
{
    for (DWORD i = 0; i < dwNumEntries && (dwBase + i) < 256; i++) {
        p->entries[dwBase + i].rgbRed      = pEntries[i].peRed;
        p->entries[dwBase + i].rgbGreen    = pEntries[i].peGreen;
        p->entries[dwBase + i].rgbBlue     = pEntries[i].peBlue;
        p->entries[dwBase + i].rgbReserved = 0;
    }
    return DD_OK;
}

static void *g_DDPalVtbl[] = {
    (void *)DDPal_QueryInterface,
    (void *)DDPal_AddRef,
    (void *)DDPal_Release,
    (void *)DDPal_GetCaps,
    (void *)DDPal_GetEntries,
    (void *)DDPal_Initialize,
    (void *)DDPal_SetEntries,
};

static HRESULT __stdcall DDSurf_QueryInterface(MyDDSurface *, REFIID, void **);
static ULONG   __stdcall DDSurf_AddRef(MyDDSurface *);
static ULONG   __stdcall DDSurf_Release(MyDDSurface *);
static HRESULT __stdcall DDSurf_AddAttachedSurface(MyDDSurface *, MyDDSurface *);
static HRESULT __stdcall DDSurf_AddOverlayDirtyRect(MyDDSurface *, RECT *);
static HRESULT __stdcall DDSurf_Blt(MyDDSurface *, RECT *, MyDDSurface *, RECT *, DWORD, void *);
static HRESULT __stdcall DDSurf_BltBatch(MyDDSurface *, void *, DWORD, DWORD);
static HRESULT __stdcall DDSurf_BltFast(MyDDSurface *, DWORD, DWORD, MyDDSurface *, RECT *, DWORD);
static HRESULT __stdcall DDSurf_DeleteAttachedSurface(MyDDSurface *, DWORD, MyDDSurface *);
static HRESULT __stdcall DDSurf_EnumAttachedSurfaces(MyDDSurface *, void *, void *);
static HRESULT __stdcall DDSurf_EnumOverlayZOrders(MyDDSurface *, DWORD, void *, void *);
static HRESULT __stdcall DDSurf_Flip(MyDDSurface *, MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf_GetAttachedSurface(MyDDSurface *, MY_DDSCAPS *, MyDDSurface **);
static HRESULT __stdcall DDSurf_GetBltStatus(MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf_GetCaps(MyDDSurface *, MY_DDSCAPS *);
static HRESULT __stdcall DDSurf_GetClipper(MyDDSurface *, void **);
static HRESULT __stdcall DDSurf_GetColorKey(MyDDSurface *, DWORD, MY_DDCOLORKEY *);
static HRESULT __stdcall DDSurf_GetDC(MyDDSurface *, HDC *);
static HRESULT __stdcall DDSurf_GetFlipStatus(MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf_GetOverlayPosition(MyDDSurface *, LONG *, LONG *);
static HRESULT __stdcall DDSurf_GetPalette(MyDDSurface *, MyDDPalette **);
static HRESULT __stdcall DDSurf_GetPixelFormat(MyDDSurface *, MY_DDPIXELFORMAT *);
static HRESULT __stdcall DDSurf_GetSurfaceDesc(MyDDSurface *, MY_DDSURFACEDESC *);
static HRESULT __stdcall DDSurf_Initialize(MyDDSurface *, void *, MY_DDSURFACEDESC *);
static HRESULT __stdcall DDSurf_IsLost(MyDDSurface *);
static HRESULT __stdcall DDSurf_Lock(MyDDSurface *, RECT *, MY_DDSURFACEDESC *, DWORD, HANDLE);
static HRESULT __stdcall DDSurf_ReleaseDC(MyDDSurface *, HDC);
static HRESULT __stdcall DDSurf_Restore(MyDDSurface *);
static HRESULT __stdcall DDSurf_SetClipper(MyDDSurface *, void *);
static HRESULT __stdcall DDSurf_SetColorKey(MyDDSurface *, DWORD, MY_DDCOLORKEY *);
static HRESULT __stdcall DDSurf_SetOverlayPosition(MyDDSurface *, LONG, LONG);
static HRESULT __stdcall DDSurf_SetPalette(MyDDSurface *, MyDDPalette *);
static HRESULT __stdcall DDSurf_Unlock(MyDDSurface *, void *);
static HRESULT __stdcall DDSurf_UpdateOverlay(MyDDSurface *, RECT *, MyDDSurface *, RECT *, DWORD, void *);
static HRESULT __stdcall DDSurf_UpdateOverlayDisplay(MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf_UpdateOverlayZOrder(MyDDSurface *, DWORD, MyDDSurface *);
static HRESULT __stdcall DDSurf3_GetDDInterface(MyDDSurface *, void **);
static HRESULT __stdcall DDSurf3_PageLock(MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf3_PageUnlock(MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf3_SetSurfaceDesc(MyDDSurface *, MY_DDSURFACEDESC *, DWORD);

static void *g_DDSurfVtbl[] = {
    (void *)DDSurf_QueryInterface,
    (void *)DDSurf_AddRef,
    (void *)DDSurf_Release,
    (void *)DDSurf_AddAttachedSurface,
    (void *)DDSurf_AddOverlayDirtyRect,
    (void *)DDSurf_Blt,
    (void *)DDSurf_BltBatch,
    (void *)DDSurf_BltFast,
    (void *)DDSurf_DeleteAttachedSurface,
    (void *)DDSurf_EnumAttachedSurfaces,
    (void *)DDSurf_EnumOverlayZOrders,
    (void *)DDSurf_Flip,
    (void *)DDSurf_GetAttachedSurface,
    (void *)DDSurf_GetBltStatus,
    (void *)DDSurf_GetCaps,
    (void *)DDSurf_GetClipper,
    (void *)DDSurf_GetColorKey,
    (void *)DDSurf_GetDC,
    (void *)DDSurf_GetFlipStatus,
    (void *)DDSurf_GetOverlayPosition,
    (void *)DDSurf_GetPalette,
    (void *)DDSurf_GetPixelFormat,
    (void *)DDSurf_GetSurfaceDesc,
    (void *)DDSurf_Initialize,
    (void *)DDSurf_IsLost,
    (void *)DDSurf_Lock,
    (void *)DDSurf_ReleaseDC,
    (void *)DDSurf_Restore,
    (void *)DDSurf_SetClipper,
    (void *)DDSurf_SetColorKey,
    (void *)DDSurf_SetOverlayPosition,
    (void *)DDSurf_SetPalette,
    (void *)DDSurf_Unlock,
    (void *)DDSurf_UpdateOverlay,
    (void *)DDSurf_UpdateOverlayDisplay,
    (void *)DDSurf_UpdateOverlayZOrder,
    /* The game QIs surfaces to IDirectDrawSurface3 and receives this same object, so the vtable is padded through the Surface2/3 slots. */
    (void *)DDSurf3_GetDDInterface,
    (void *)DDSurf3_PageLock,
    (void *)DDSurf3_PageUnlock,
    (void *)DDSurf3_SetSurfaceDesc,
};

static MyDDSurface *surf_create(MyDDraw *dd, int w, int h,
                                 BOOL isPrimary, BOOL isBackBuffer,
                                 const DdwSurfaceFormat *format)
{
    MyDDSurface *s = (MyDDSurface *)calloc(1, sizeof(MyDDSurface));
    if (!s) return NULL;
    s->lpVtbl      = g_DDSurfVtbl;
    s->refCount    = 1;
    s->width       = w;
    s->height      = h;
    s->format      = format ? *format : default_surface_format(dd);
    s->bytesPP     = (int)(s->format.bits_per_pixel / 8u);
    if (s->bytesPP != 1 && s->bytesPP != 2) {
        free(s);
        return NULL;
    }
    s->pixels      = alloc_pixels(
        w,
        h,
        s->bytesPP,
        &s->pitch,
        &s->pixelBytes);
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    s->isPrimary   = isPrimary;
    s->isBackBuffer = isBackBuffer;
    s->owner       = dd;
    return s;
}

static ULONG __stdcall DDSurf_AddRef(MyDDSurface *s)
    { return (ULONG)InterlockedIncrement(&s->refCount); }

static ULONG __stdcall DDSurf_Release(MyDDSurface *s)
{
    LONG ref = InterlockedDecrement(&s->refCount);
    if (ref == 0) {
        LOG("IDirectDrawSurface freed (%s %dx%d)",
            s->isPrimary ? "primary" : s->isBackBuffer ? "backbuf" : "offscreen",
            s->width, s->height);
        if (s->attachedBackBuffer) {
            DDSurf_Release(s->attachedBackBuffer);
            s->attachedBackBuffer = NULL;
        }
        if (s->palette) {
            DDPal_Release(s->palette);
            s->palette = NULL;
        }
        if (s->isPrimary && s->owner && s->owner->primarySurface == s)
            s->owner->primarySurface = NULL;
        if (g_frameSource == s) {
            g_frameSource = NULL;
            g_frameSourceKind = FRAMESRC_PRIMARY;
        }
        if (g_gdiEffectDiagnosticSurface == s)
            g_gdiEffectDiagnosticSurface = NULL;
        ddw_gdi_bridge_destroy(&s->gdiBridge);
        free(s->pixels);
        free(s);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall DDSurf_QueryInterface(MyDDSurface *s, REFIID r, void **ppv)
    { *ppv = s; DDSurf_AddRef(s); return DD_OK; }

static HRESULT __stdcall DDSurf_AddAttachedSurface(MyDDSurface *s, MyDDSurface *surf)
    { return DD_OK; }
static HRESULT __stdcall DDSurf_AddOverlayDirtyRect(MyDDSurface *s, RECT *r)
    { return DD_OK; }

static HRESULT __stdcall DDSurf_Blt(MyDDSurface *dst, RECT *destRect,
                                     MyDDSurface *src, RECT *srcRect,
                                     DWORD flags, void *pFX)
{
    const DWORD disposition = qoh99_presentation_disposition();
    DdwSurfaceView dstView;
    DdwSurfaceView srcView;
    DdwRect sr;
    DdwRect dr;

    if (!dst)
        return DDERR_INVALIDPARAMS;

    /* Color fill (32-bit DDBLTFX::dwFillColor is the DWORD at byte offset 80). */
    if (flags & DDBLT_COLORFILL) {
        uint32_t fill;
        DdwOpResult fillResult;

        if (!ddw_read_fill_color(pFX, &fill))
            return DDERR_INVALIDPARAMS;
        dstView = surface_view(dst);
        dr = surface_rect(destRect, dst->width, dst->height);
        if ((dst->isPrimary || dst->isBackBuffer || dst == g_frameSource)
            && !qoh99_presentation_executes_surface_work(
                disposition, FALSE)) {
            if (!dstView.pixels || dstView.width <= 0
                || dstView.height <= 0)
                return DDERR_INVALIDPARAMS;
            ++g_qoh99SuppressedColorFills;
            return DD_OK;
        }
        fillResult = ddw_fill_rect(&dstView, dr, fill);
        if (fillResult == DDW_OP_INVALID)
            return DDERR_INVALIDPARAMS;

        if (dst->isPrimary && dst->owner) {
            g_hasEverFlipped = TRUE;
            present_frame(dst->owner, FALSE);
        }
        return DD_OK;
    }
    if (!src)
        return DDERR_INVALIDPARAMS;

    sr = surface_rect(srcRect, src->width, src->height);
    dr = surface_rect(destRect, dst->width, dst->height);

    {
        int64_t sw = (int64_t)sr.right - (int64_t)sr.left;
        int64_t sh = (int64_t)sr.bottom - (int64_t)sr.top;
        int64_t dw = (int64_t)dr.right - (int64_t)dr.left;
        int64_t dh = (int64_t)dr.bottom - (int64_t)dr.top;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
            return DD_OK;
    }

    if ((flags & (DDBLT_KEYSRC | DDBLT_KEYDEST)) == 0) {
        Qoh99FrameBltInfo fb = detect_qoh99_frame_scale_blt(dst, src, srcRect, destRect);
        if (fb.matched) {
            g_frameSource     = src;
            g_frameSourceKind = FRAMESRC_QOH99_SCALE_320x240;
            LOG("[qoh99-frameblt] detected src=%dx%d dst=%dx%d dst_origin=(%d,%d) action=present_source",
                fb.src_w, fb.src_h, fb.dst_w, fb.dst_h,
                fb.dst_origin_x, fb.dst_origin_y);
            if (dst->owner) {
                g_hasEverFlipped = TRUE;
            }
            if (dst->owner) {
                if (qoh99_frame_commit()) {
                    if (present_frame(dst->owner, TRUE))
                        qoh99_frame_published();
                    else
                        qoh99_frame_publish_failed(
                            g_qoh99LastPublishFailure);
                } else if (
                    disposition == QOH99_PRESENT_SUPPRESS_HISTORICAL) {
                    qoh99_record_suppressed_present();
                }
            }
            return DD_OK;
        }
    }

    if (!qoh99_presentation_executes_surface_work(disposition, FALSE)) {
        if (!dst->pixels || !src->pixels
            || dst->width <= 0 || dst->height <= 0
            || src->width <= 0 || src->height <= 0
            || dst->bytesPP != src->bytesPP)
            return DDERR_INVALIDPARAMS;
        ++g_qoh99SuppressedBlts;
        return DD_OK;
    }

    {
        int64_t sw = (int64_t)sr.right - (int64_t)sr.left;
        int64_t sh = (int64_t)sr.bottom - (int64_t)sr.top;
        int64_t dw = (int64_t)dr.right - (int64_t)dr.left;
        int64_t dh = (int64_t)dr.bottom - (int64_t)dr.top;
        if ((dst->isPrimary || src->isPrimary) && (sw != dw || sh != dh))
            LOG("DIAG SCALEBLT %s->%s src=%dx%d@(%d,%d,%d,%d) dst=%dx%d@(%d,%d,%d,%d) flags=%08X",
                src->isPrimary ? "PRIM" : "off", dst->isPrimary ? "PRIM" : "off",
                src->width, src->height, sr.left, sr.top, sr.right, sr.bottom,
                dst->width, dst->height, dr.left, dr.top, dr.right, dr.bottom, flags);
    }

    {
        BOOL ck = (flags & DDBLT_KEYSRC) && src->hasColorKeySrc;
        void *scratch = NULL;
        size_t scratchBytes = 0;
        DdwOpResult bltResult;

        dstView = surface_view(dst);
        srcView = surface_view(src);
        bltResult = ddw_blt_nearest(
            &dstView,
            dr,
            &srcView,
            sr,
            ck,
            src->colorKeySrcLow,
            src->colorKeySrcHigh,
            NULL,
            0);
        if (bltResult == DDW_OP_NEEDS_SCRATCH) {
            scratchBytes = ddw_blt_scratch_bytes(&dstView, dr, &srcView, sr);
            if (scratchBytes == 0)
                return DDERR_INVALIDPARAMS;
            scratch = malloc(scratchBytes);
            if (!scratch)
                return DDERR_OUTOFMEMORY;
            bltResult = ddw_blt_nearest(
                &dstView,
                dr,
                &srcView,
                sr,
                ck,
                src->colorKeySrcLow,
                src->colorKeySrcHigh,
                scratch,
                scratchBytes);
            free(scratch);
        }
        if (bltResult == DDW_OP_INVALID || bltResult == DDW_OP_NEEDS_SCRATCH)
            return DDERR_INVALIDPARAMS;
    }

    if (dst->isPrimary && dst->owner) {
        g_hasEverFlipped = TRUE;
        present_frame(dst->owner, FALSE);
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_BltBatch(MyDDSurface *s, void *pBatch,
                                          DWORD dwCount, DWORD dwFlags)
    { return DD_OK; }

static HRESULT __stdcall DDSurf_BltFast(MyDDSurface *dst, DWORD destX, DWORD destY,
                                         MyDDSurface *src, RECT *srcRect, DWORD flags)
{
    const DWORD disposition = qoh99_presentation_disposition();
    DdwSurfaceView dstView;
    DdwSurfaceView srcView;
    DdwRect sr;
    DdwOpResult bltResult;
    BOOL ck;

    if (!dst || !src)
        return DDERR_INVALIDPARAMS;

    dstView = surface_view(dst);
    srcView = surface_view(src);
    sr = surface_rect(srcRect, src->width, src->height);
    ck = (flags & DDBLTFAST_SRCCOLORKEY) && src->hasColorKeySrc;

    if (!ck) {
        RECT commitDst;
        Qoh99FrameBltInfo fb;
        commitDst.left   = (LONG)signed_bltfast_coordinate(destX);
        commitDst.top    = (LONG)signed_bltfast_coordinate(destY);
        commitDst.right  = commitDst.left + (LONG)(sr.right - sr.left);
        commitDst.bottom = commitDst.top + (LONG)(sr.bottom - sr.top);
        fb = detect_qoh99_frame_scale_blt(dst, src, srcRect, &commitDst);
        if (fb.matched) {
            g_frameSource     = src;
            g_frameSourceKind = FRAMESRC_QOH99_SCALE_320x240;
            LOG("[qoh99-frameblt] detected src=%dx%d dst=%dx%d dst_origin=(%d,%d) action=present_source path=bltfast",
                fb.src_w, fb.src_h, fb.dst_w, fb.dst_h,
                fb.dst_origin_x, fb.dst_origin_y);
            if (dst->owner) {
                g_hasEverFlipped = TRUE;
                if (qoh99_frame_commit()) {
                    if (present_frame(dst->owner, TRUE))
                        qoh99_frame_published();
                    else
                        qoh99_frame_publish_failed(
                            g_qoh99LastPublishFailure);
                } else if (
                    disposition == QOH99_PRESENT_SUPPRESS_HISTORICAL) {
                    qoh99_record_suppressed_present();
                }
            }
            return DD_OK;
        }
    }

    if (!qoh99_presentation_executes_surface_work(disposition, FALSE)) {
        if (!dstView.pixels || !srcView.pixels
            || dstView.width <= 0 || dstView.height <= 0
            || srcView.width <= 0 || srcView.height <= 0
            || dstView.bytes_per_pixel != srcView.bytes_per_pixel)
            return DDERR_INVALIDPARAMS;
        ++g_qoh99SuppressedBltFasts;
        return DD_OK;
    }
    bltResult = ddw_blt_fast(
        &dstView,
        signed_bltfast_coordinate(destX),
        signed_bltfast_coordinate(destY),
        &srcView,
        sr,
        ck,
        src->colorKeySrcLow,
        src->colorKeySrcHigh);
    if (bltResult == DDW_OP_INVALID)
        return DDERR_INVALIDPARAMS;

    if (dst->isPrimary && dst->owner) {
        g_hasEverFlipped = TRUE;
        present_frame(dst->owner, FALSE);
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_DeleteAttachedSurface(MyDDSurface *s,
    DWORD dwFlags, MyDDSurface *surf) { return DD_OK; }
static HRESULT __stdcall DDSurf_EnumAttachedSurfaces(MyDDSurface *s,
    void *pCtx, void *pfn) { return DD_OK; }
static HRESULT __stdcall DDSurf_EnumOverlayZOrders(MyDDSurface *s,
    DWORD dwFlags, void *pCtx, void *pfn) { return DD_OK; }

static HRESULT __stdcall DDSurf_Flip(MyDDSurface *s, MyDDSurface *target, DWORD flags)
{
    const DWORD disposition = qoh99_presentation_disposition();
    DWORD flipNo = ++g_flipSerial;

    if (flipNo <= 8 || g_renderTraceAfterFocus || (flipNo % 300) == 0) {
        LOG("DDSurf_Flip flip=%u surf=%p primary=%d owner=%p target=%p flags=%08X active=%d focusSerial=%u",
            flipNo, (void *)s, s ? s->isPrimary : 0,
            s ? (void *)s->owner : NULL, (void *)target, flags,
            g_appActive, g_focusSerial);
    }

    if (s && s->isPrimary && s->owner) {
        if (qoh99_presentation_executes_pacing(disposition)) {
            if (!g_fpsPacedThisFlip)
                fps_throttle();
        } else {
            ++g_qoh99SuppressedPacingCalls;
        }
        g_fpsPacedThisFlip = FALSE;

        g_hasEverFlipped = TRUE;
        present_frame(s->owner, FALSE);
    }
    else if (flipNo <= 8 || g_renderTraceAfterFocus) {
        LOG("DDSurf_Flip skipped-present flip=%u reason=%s",
            flipNo, !s ? "null-surface" :
            !s->isPrimary ? "not-primary" : "no-owner");
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetAttachedSurface(MyDDSurface *s,
    MY_DDSCAPS *pCaps, MyDDSurface **ppSurf)
{
    if (!ppSurf) return DDERR_INVALIDPARAMS;
    if (s->attachedBackBuffer) {
        DDSurf_AddRef(s->attachedBackBuffer);
        *ppSurf = s->attachedBackBuffer;
        LOG("GetAttachedSurface → back buffer %dx%d",
            s->attachedBackBuffer->width, s->attachedBackBuffer->height);
        return DD_OK;
    }
    *ppSurf = NULL;
    return DDERR_GENERIC;
}

static HRESULT __stdcall DDSurf_GetBltStatus(MyDDSurface *s, DWORD f)
    { return DD_OK; }

static HRESULT __stdcall DDSurf_GetCaps(MyDDSurface *s, MY_DDSCAPS *pCaps)
{
    if (pCaps)
        pCaps->dwCaps = s->isPrimary
                      ? (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                         DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY)
                      : s->isBackBuffer
                      ? (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_SYSTEMMEMORY)
                      : (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetClipper(MyDDSurface *s, void **pp)
    { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_GetColorKey(MyDDSurface *s,
    DWORD dwFlags, MY_DDCOLORKEY *pCK)
{
    if (pCK && (dwFlags & DDCKEY_SRCBLT) && s->hasColorKeySrc) {
        pCK->dwLow  = s->colorKeySrcLow;
        pCK->dwHigh = s->colorKeySrcHigh;
    }
    return DD_OK;
}

static HRESULT ddw_gdi_bridge_hresult(DdwGdiBridgeResult result)
{
    switch (result) {
        case DDW_GDI_BRIDGE_SURFACE_LOCKED:
            return DDERR_SURFACEBUSY;
        case DDW_GDI_BRIDGE_DC_ALREADY_ACTIVE:
            return DDERR_DCALREADYCREATED;
        case DDW_GDI_BRIDGE_UNSUPPORTED_FORMAT:
            return DDERR_UNSUPPORTED;
        case DDW_GDI_BRIDGE_OUT_OF_MEMORY:
            return DDERR_OUTOFMEMORY;
        case DDW_GDI_BRIDGE_INVALID_ARGUMENT:
        case DDW_GDI_BRIDGE_INVALID_SURFACE:
        case DDW_GDI_BRIDGE_INVALID_DC:
            return DDERR_INVALIDPARAMS;
        default:
            return DDERR_GENERIC;
    }
}

static int gdi_effect_diagnostic_clamp(int value, int limit)
{
    if (value < 0)
        return 0;
    if (value >= limit)
        return limit - 1;
    return value;
}

static void gdi_effect_diagnostic_note_ellipse(
    HDC dc,
    int left,
    int top,
    int right,
    int bottom)
{
    MyDDSurface *surface;
    LONG expected;
    HGDIOBJ pen;
    LOGPEN logpen;
    int x;
    int y;

    if (!g_gdiEffectDiagnosticEnabled)
        return;
    surface = g_gdiEffectDiagnosticSurface;
    if (!surface || !surface->gdiBridge.active
        || surface->gdiBridge.dc != dc || surface->bytesPP != 2)
        return;
    do {
        expected = InterlockedCompareExchange(&g_gdiEffectDiagnosticBudget, 0, 0);
        if (expected <= 0)
            return;
    } while (InterlockedCompareExchange(
                 &g_gdiEffectDiagnosticBudget, expected - 1, expected)
             != expected);

    x = gdi_effect_diagnostic_clamp(
        (int)((int64_t)left + ((int64_t)right - (int64_t)left) / 2),
        surface->width);
    y = gdi_effect_diagnostic_clamp(top, surface->height);
    ZeroMemory(&logpen, sizeof(logpen));
    pen = GetCurrentObject(dc, OBJ_PEN);
    if (pen)
        (void)GetObject(pen, sizeof(logpen), &logpen);
    surface->gdiEffectDiagnosticPending = TRUE;
    surface->gdiEffectDiagnosticX = x;
    surface->gdiEffectDiagnosticY = y;
    surface->gdiEffectDiagnosticColor = logpen.lopnColor;
    surface->gdiEffectDiagnosticPenWidth = logpen.lopnWidth.x;
    surface->gdiEffectDiagnosticBounds.left = left;
    surface->gdiEffectDiagnosticBounds.top = top;
    surface->gdiEffectDiagnosticBounds.right = right;
    surface->gdiEffectDiagnosticBounds.bottom = bottom;
    memcpy(
        &surface->gdiEffectDiagnosticSourceWord,
        surface->pixels + (size_t)y * surface->pitch + (size_t)x * 2u,
        sizeof(surface->gdiEffectDiagnosticSourceWord));

    LOG("DDRAW_EFFECT_DIAG begin nativeColorref=%08lX penWidth=%ld ellipse=%d,%d,%d,%d sample=%d,%d surf=%p dims=%dx%d pitch=%d bpp=%lu masks=%08lX/%08lX/%08lX/%08lX dibBpp=%d dibCompression=%lu dibMasks=00000000/00000000/00000000 dibPitch=%d disposition=%lu saveDc=%d sourceWord=%04X",
        (unsigned long)surface->gdiEffectDiagnosticColor,
        surface->gdiEffectDiagnosticPenWidth,
        left, top, right, bottom, x, y, (void *)surface,
        surface->width, surface->height, surface->pitch,
        (unsigned long)surface->format.bits_per_pixel,
        (unsigned long)surface->format.red_mask,
        (unsigned long)surface->format.green_mask,
        (unsigned long)surface->format.blue_mask,
        (unsigned long)surface->format.alpha_mask,
        ddw_gdi_bridge_scratch_bits_per_pixel(&surface->gdiBridge),
        (unsigned long)ddw_gdi_bridge_scratch_compression(&surface->gdiBridge),
        ddw_gdi_bridge_pitch(&surface->gdiBridge),
        (unsigned long)surface->dcPresentationDisposition,
        surface->gdiBridge.save_level != 0,
        (unsigned)surface->gdiEffectDiagnosticSourceWord);
}

static void gdi_effect_diagnostic_finish(
    MyDDSurface *surface,
    int write_back,
    DdwGdiBridgeResult result)
{
    const int x = surface->gdiEffectDiagnosticX;
    const int y = surface->gdiEffectDiagnosticY;
    uint8_t blue = 0u;
    uint8_t green = 0u;
    uint8_t red = 0u;
    uint16_t destination_word = 0u;
    DdwRgb8 final_rgb;
    int final_decoded = 0;

    if (!surface->gdiEffectDiagnosticPending)
        return;
    ZeroMemory(&final_rgb, sizeof(final_rgb));
    if (surface->gdiBridge.bits && x >= 0 && y >= 0
        && x < surface->width && y < surface->height) {
        const uint8_t *scratch = (const uint8_t *)surface->gdiBridge.bits
            + (size_t)y * ddw_gdi_bridge_pitch(&surface->gdiBridge)
            + (size_t)x * 4u;
        blue = scratch[0];
        green = scratch[1];
        red = scratch[2];
    }
    if (write_back && result == DDW_GDI_BRIDGE_OK) {
        memcpy(
            &destination_word,
            surface->pixels + (size_t)y * surface->pitch + (size_t)x * 2u,
            sizeof(destination_word));
        final_decoded = ddw_surface_format_decode_rgb(
            &surface->format, destination_word, &final_rgb);
    }
    LOG("DDRAW_EFFECT_DIAG finish restoreDc=%d gdiFlush=%d writeback=%d result=%d scratchBgrx=%u,%u,%u,0 destinationWord=%04X finalPresenterRgb=%u,%u,%u decoded=%d",
        surface->gdiBridge.last_restore_success ? 1 : 0,
        surface->gdiBridge.last_flush_success ? 1 : 0,
        write_back,
        (int)result,
        (unsigned)blue, (unsigned)green, (unsigned)red,
        (unsigned)destination_word,
        (unsigned)final_rgb.red, (unsigned)final_rgb.green,
        (unsigned)final_rgb.blue, final_decoded);
    surface->gdiEffectDiagnosticPending = FALSE;
}

static HRESULT __stdcall DDSurf_GetDC(MyDDSurface *s, HDC *phDC)
{
    DdwSurfaceView view;
    DdwGdiBridgeResult bridge_result;
    int recreated = 0;
    const DWORD disposition = qoh99_presentation_disposition();

    InterlockedIncrement(&g_qoh99DdrawDcCounters.getDcCalls);
    if (phDC) *phDC = NULL;
    if (!s || !phDC) {
        InterlockedIncrement(&g_qoh99DdrawDcCounters.getDcFailures);
        return DDERR_INVALIDPARAMS;
    }

    view = surface_view(s);
    bridge_result = ddw_gdi_bridge_acquire(
        &s->gdiBridge, &view, s->locked ? 1 : 0, phDC, &recreated);
    if (bridge_result != DDW_GDI_BRIDGE_OK) {
        InterlockedIncrement(&g_qoh99DdrawDcCounters.getDcFailures);
        if (bridge_result == DDW_GDI_BRIDGE_DC_ALREADY_ACTIVE)
            InterlockedIncrement(&g_qoh99DdrawDcCounters.nestedDcRejects);
        if (bridge_result == DDW_GDI_BRIDGE_SURFACE_LOCKED)
            InterlockedIncrement(&g_qoh99DdrawDcCounters.lockDcConflicts);
        if (bridge_result == DDW_GDI_BRIDGE_UNSUPPORTED_FORMAT) {
            LONG shown = InterlockedIncrement(
                &g_qoh99DdrawDcCounters.unsupportedFormatRejects);
            if (shown <= 3) {
                LOG("[ddraw-gdi] EFFECT SURFACE REJECTED (#%ld): GetDC on a "
                    "non-RGB565 surface - the exact-RGB565 GDI bridge gate "
                    "refuses it, so a native effect drawn through GetDC/"
                    "Ellipse/ReleaseDC onto THIS surface will not appear. "
                    "surf=%p %dx%d bpp=%lu masks=R%08lX/G%08lX/B%08lX/A%08lX "
                    "isPrimary=%d isBackBuffer=%d",
                    shown, (void *)s, s->width, s->height,
                    (unsigned long)s->format.bits_per_pixel,
                    (unsigned long)s->format.red_mask,
                    (unsigned long)s->format.green_mask,
                    (unsigned long)s->format.blue_mask,
                    (unsigned long)s->format.alpha_mask,
                    s->isPrimary ? 1 : 0, s->isBackBuffer ? 1 : 0);
            }
        }
        LOGV("DDSurf_GetDC rejected surf=%p result=%d locked=%d active=%d bpp=%lu masks=%08lX/%08lX/%08lX/%08lX (only exact RGB565 is bridge-qualified)",
             (void *)s, (int)bridge_result, s->locked ? 1 : 0,
             ddw_gdi_bridge_is_active(&s->gdiBridge),
             (unsigned long)s->format.bits_per_pixel,
             (unsigned long)s->format.red_mask,
             (unsigned long)s->format.green_mask,
             (unsigned long)s->format.blue_mask,
             (unsigned long)s->format.alpha_mask);
        return ddw_gdi_bridge_hresult(bridge_result);
    }

    /* Snapshot the disposition exactly once: a rollback scope can legally end between GetDC and ReleaseDC. */
    s->dcPresentationDisposition = disposition;
    s->gdiEffectDiagnosticPending = FALSE;
    if (g_gdiEffectDiagnosticEnabled)
        g_gdiEffectDiagnosticSurface = s;
    if (recreated)
        InterlockedIncrement(&g_qoh99DdrawDcCounters.surfaceRecreations);
    InterlockedIncrement(&g_qoh99DdrawDcCounters.getDcSuccesses);
    LOGV("DDSurf_GetDC acquired surf=%p dc=%p %dx%d pitch=%d bpp=%d "
         "masks=%08lX/%08lX/%08lX/%08lX scratch=32bpp-BI_RGB pitch=%d disposition=%lu recreated=%d",
         (void *)s, (void *)*phDC, s->width, s->height, s->pitch,
         s->bytesPP,
         (unsigned long)s->format.red_mask,
         (unsigned long)s->format.green_mask,
         (unsigned long)s->format.blue_mask,
         (unsigned long)s->format.alpha_mask,
         ddw_gdi_bridge_pitch(&s->gdiBridge),
         (unsigned long)disposition, recreated);
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetFlipStatus(MyDDSurface *s, DWORD f)
    { return DD_OK; }
static HRESULT __stdcall DDSurf_GetOverlayPosition(MyDDSurface *s, LONG *px, LONG *py)
    { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_GetPalette(MyDDSurface *s, MyDDPalette **pp)
{
    if (!pp) return DDERR_INVALIDPARAMS;
    *pp = s->palette;
    if (s->palette) DDPal_AddRef(s->palette);
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetPixelFormat(MyDDSurface *s, MY_DDPIXELFORMAT *pFmt)
{
    if (!pFmt) return DDERR_INVALIDPARAMS;
    ZeroMemory(pFmt, sizeof(*pFmt));
    fill_pixel_format(pFmt, &s->format);
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetSurfaceDesc(MyDDSurface *s, MY_DDSURFACEDESC *pDesc)
{
    if (!pDesc || pDesc->dwSize < sizeof(MY_DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    pDesc->dwFlags  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_CAPS
                    | DDSD_PIXELFORMAT;
    pDesc->dwWidth  = (DWORD)s->width;
    pDesc->dwHeight = (DWORD)s->height;
    pDesc->lPitch   = s->pitch;
    fill_pixel_format(&pDesc->ddpfPixelFormat, &s->format);
    pDesc->ddsCaps.dwCaps = s->isPrimary ? DDSCAPS_PRIMARYSURFACE
                          : s->isBackBuffer ? DDSCAPS_BACKBUFFER
                          : DDSCAPS_OFFSCREENPLAIN;
    if (s->locked) {
        pDesc->dwFlags   |= DDSD_LPSURFACE;
        pDesc->lpSurface  = (DWORD)(ULONG_PTR)s->lockedPointer;
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_Initialize(MyDDSurface *s, void *pDD,
    MY_DDSURFACEDESC *pDesc) { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_IsLost(MyDDSurface *s) { return DD_OK; }

static HRESULT __stdcall DDSurf_Lock(MyDDSurface *s, RECT *pRect,
                                      MY_DDSURFACEDESC *pDesc,
                                      DWORD dwFlags, HANDLE hEvent)
{
    DdwSurfaceView view;
    DdwRect requestedRect;
    const DdwRect *requestedRectPointer = NULL;
    DdwLockRegion region;
    BYTE *lockPointer;

    (void)dwFlags;
    (void)hEvent;
    if (!s || !pDesc || pDesc->dwSize < sizeof(MY_DDSURFACEDESC))
        return DDERR_INVALIDPARAMS;
    if (!ddw_gdi_bridge_lock_allowed(&s->gdiBridge)) {
        InterlockedIncrement(&g_qoh99DdrawDcCounters.lockDcConflicts);
        LOGV("DDSurf_Lock rejected while GetDC is active surf=%p", (void *)s);
        return DDERR_SURFACEBUSY;
    }
    if (s->locked)
        return DDERR_ALREADYLOCKED;

    view = surface_view(s);
    if (pRect) {
        requestedRect = surface_rect(pRect, s->width, s->height);
        requestedRectPointer = &requestedRect;
    }
    if (!ddw_validate_lock_region(&view, requestedRectPointer, &region))
        return DDERR_INVALIDPARAMS;
    lockPointer = s->pixels + region.byte_offset;

    pDesc->dwFlags  |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_LPSURFACE
                     | DDSD_PIXELFORMAT;
    pDesc->dwWidth   = (DWORD)region.width;
    pDesc->dwHeight  = (DWORD)region.height;
    pDesc->lPitch    = s->pitch;
    pDesc->lpSurface = (DWORD)(ULONG_PTR)lockPointer;
    fill_pixel_format(&pDesc->ddpfPixelFormat, &s->format);
    s->lockedPointer = lockPointer;
    s->locked = TRUE;

    if (pRect) {
        if (s->isPrimary)
            LOG("DIAG primary SUBRECT lock (%ld,%ld,%ld,%ld) of %dx%d",
                pRect->left, pRect->top, pRect->right, pRect->bottom,
                s->width, s->height);
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_ReleaseDC(MyDDSurface *s, HDC hDC)
{
    DdwSurfaceView view;
    DdwGdiBridgeResult bridge_result;
    const int write_back = s != NULL
        && !qoh99_presentation_suppresses_surface_work(
            s->dcPresentationDisposition);

    InterlockedIncrement(&g_qoh99DdrawDcCounters.releaseDcCalls);
    if (!s) {
        InterlockedIncrement(&g_qoh99DdrawDcCounters.releaseDcFailures);
        return DDERR_INVALIDPARAMS;
    }
    view = surface_view(s);
    bridge_result = ddw_gdi_bridge_release(
        &s->gdiBridge, &view, hDC, write_back);
    if (!ddw_gdi_bridge_is_active(&s->gdiBridge)) {
        gdi_effect_diagnostic_finish(s, write_back, bridge_result);
    }
    if (!ddw_gdi_bridge_is_active(&s->gdiBridge)
        && g_gdiEffectDiagnosticSurface == s) {
        g_gdiEffectDiagnosticSurface = NULL;
    }
    if (bridge_result != DDW_GDI_BRIDGE_OK) {
        InterlockedIncrement(&g_qoh99DdrawDcCounters.releaseDcFailures);
        if (!ddw_gdi_bridge_is_active(&s->gdiBridge))
            s->dcPresentationDisposition = QOH99_PRESENT_NORMAL;
        LOGV("DDSurf_ReleaseDC rejected surf=%p dc=%p result=%d active=%d",
             (void *)s, (void *)hDC, (int)bridge_result,
             ddw_gdi_bridge_is_active(&s->gdiBridge));
        return ddw_gdi_bridge_hresult(bridge_result);
    }

    if (write_back)
        InterlockedIncrement(&g_qoh99DdrawDcCounters.visibleWritebacks);
    else
        InterlockedIncrement(&g_qoh99DdrawDcCounters.historicalDiscards);
    s->dcPresentationDisposition = QOH99_PRESENT_NORMAL;
    LOGV("DDSurf_ReleaseDC completed surf=%p dc=%p writeback=%d",
         (void *)s, (void *)hDC, write_back);
    return DD_OK;
}
static HRESULT __stdcall DDSurf_Restore(MyDDSurface *s)
    { return DD_OK; }
static HRESULT __stdcall DDSurf_SetClipper(MyDDSurface *s, void *pClipper)
    { return DD_OK; }

static HRESULT __stdcall DDSurf_SetColorKey(MyDDSurface *s,
    DWORD dwFlags, MY_DDCOLORKEY *pCK)
{
    if (!pCK) return DDERR_INVALIDPARAMS;
    if (dwFlags & DDCKEY_SRCBLT) {
        s->hasColorKeySrc  = TRUE;
        s->colorKeySrcLow  = pCK->dwLow;
        s->colorKeySrcHigh = pCK->dwHigh;
        LOG("SetColorKey SRCBLT [%u, %u] on %s surface",
            pCK->dwLow, pCK->dwHigh,
            s->isPrimary ? "primary" : s->isBackBuffer ? "backbuf" : "offscreen");
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_SetOverlayPosition(MyDDSurface *s, LONG x, LONG y)
    { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_SetPalette(MyDDSurface *s, MyDDPalette *pPal)
{
    if (s->palette)  DDPal_Release(s->palette);
    s->palette = pPal;
    if (pPal) DDPal_AddRef(pPal);
    LOG("SetPalette on %s surface",
        s->isPrimary ? "primary" : s->isBackBuffer ? "backbuf" : "offscreen");
    return DD_OK;
}

static HRESULT __stdcall DDSurf_Unlock(MyDDSurface *s, void *pSurfData)
{
    if (!s)
        return DDERR_INVALIDPARAMS;
    if (!s->locked)
        return DDERR_NOTLOCKED;
    if (pSurfData && pSurfData != s->lockedPointer)
        return DDERR_INVALIDPARAMS;

    s->locked = FALSE;
    s->lockedPointer = NULL;
    if (s->isPrimary && s->owner) {
        g_hasEverFlipped = TRUE;
        present_frame(s->owner, FALSE);
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_UpdateOverlay(MyDDSurface *s, RECT *sr,
    MyDDSurface *dst, RECT *dr, DWORD f, void *pFX) { return DDERR_UNSUPPORTED; }
static HRESULT __stdcall DDSurf_UpdateOverlayDisplay(MyDDSurface *s, DWORD f)
    { return DDERR_UNSUPPORTED; }
static HRESULT __stdcall DDSurf_UpdateOverlayZOrder(MyDDSurface *s,
    DWORD f, MyDDSurface *ref) { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf3_GetDDInterface(MyDDSurface *s, void **ppDD)
    { if (ppDD) *ppDD = s->owner; return DD_OK; }
static HRESULT __stdcall DDSurf3_PageLock(MyDDSurface *s, DWORD f)
    { (void)s; (void)f; return DD_OK; }
static HRESULT __stdcall DDSurf3_PageUnlock(MyDDSurface *s, DWORD f)
    { (void)s; (void)f; return DD_OK; }
static HRESULT __stdcall DDSurf3_SetSurfaceDesc(MyDDSurface *s,
    MY_DDSURFACEDESC *pDesc, DWORD f)
    { (void)s; (void)pDesc; (void)f; return DDERR_UNSUPPORTED; }

static ULONG __stdcall DD_AddRef(MyDDraw *dd)
    { return (ULONG)InterlockedIncrement(&dd->refCount); }

static ULONG __stdcall DD_Release(MyDDraw *dd)
{
    LONG ref = InterlockedDecrement(&dd->refCount);
    if (ref == 0) {
        const BOOL wasPresenting = (dd == g_dd);
        HWND hWnd = dd->hWnd;

        LOG("IDirectDraw freed (presenting=%d hwnd=%p)",
            (int)wasPresenting, (void *)hWnd);
        if (dd->primarySurface) {
            DDSurf_Release(dd->primarySurface);
            dd->primarySurface = NULL;
        }
        if (wasPresenting) g_dd = NULL;
        free(dd);

        if (wasPresenting && (hWnd == NULL || !IsWindow(hWnd)))
            wrapper_release_runtime("IDirectDraw::Release");
    }
    return (ULONG)ref;
}

/* The game drives everything through IDirectDraw2; the single vtable is IDirectDraw2-shaped, so unknown IIDs must be refused rather than handed a mis-shaped vtable. */
static HRESULT __stdcall DD_QueryInterface(MyDDraw *dd, REFIID r, void **ppv)
{
    const GUID *iid = (const GUID *)r;
    if (!ppv) return DDERR_INVALIDPARAMS;
    if (guid_equal(iid, &QOH_IID_IUnknown) ||
        guid_equal(iid, &QOH_IID_IDirectDraw) ||
        guid_equal(iid, &QOH_IID_IDirectDraw2)) {
        *ppv = dd;
        DD_AddRef(dd);
        return DD_OK;
    }
    if (iid)
        LOG("DD_QueryInterface: unsupported IID {%08lX-%04X-%04X-...}",
            iid->Data1, iid->Data2, iid->Data3);
    *ppv = NULL;
    return 0x80004002L;
}

static HRESULT __stdcall DD_Compact(MyDDraw *dd) { return DD_OK; }

static HRESULT __stdcall DDClip_QueryInterface(MyDDClipper *c, REFIID r, void **ppv)
    { (void)r; if (!ppv) return DDERR_INVALIDPARAMS; *ppv = c; c->refCount++; return DD_OK; }
static ULONG __stdcall DDClip_AddRef(MyDDClipper *c)
    { return (ULONG)InterlockedIncrement(&c->refCount); }
static ULONG __stdcall DDClip_Release(MyDDClipper *c)
{
    LONG n = InterlockedDecrement(&c->refCount);
    if (n <= 0) { free(c); return 0; }
    return (ULONG)n;
}
static HRESULT __stdcall DDClip_GetClipList(MyDDClipper *c, RECT *pRect,
    void *pClipList, DWORD *pdwSize)
    { (void)c; (void)pRect; (void)pClipList; if (pdwSize) *pdwSize = 0; return DD_OK; }
static HRESULT __stdcall DDClip_GetHWnd(MyDDClipper *c, HWND *phWnd)
    { if (phWnd) *phWnd = c->hWnd; return DD_OK; }
static HRESULT __stdcall DDClip_Initialize(MyDDClipper *c, void *pDD, DWORD dwFlags)
    { (void)c; (void)pDD; (void)dwFlags; return DD_OK; }
static HRESULT __stdcall DDClip_IsClipListChanged(MyDDClipper *c, BOOL *pChanged)
    { (void)c; if (pChanged) *pChanged = FALSE; return DD_OK; }
static HRESULT __stdcall DDClip_SetClipList(MyDDClipper *c, void *pClipList, DWORD dwFlags)
    { (void)c; (void)pClipList; (void)dwFlags; return DD_OK; }
static HRESULT __stdcall DDClip_SetHWnd(MyDDClipper *c, DWORD dwFlags, HWND hWnd)
{
    (void)dwFlags;
    c->hWnd = hWnd;
    LOG("DDClip_SetHWnd hwnd=%p", (void *)hWnd);
    return DD_OK;
}

static void *g_DDClipVtbl[] = {
    (void *)DDClip_QueryInterface,
    (void *)DDClip_AddRef,
    (void *)DDClip_Release,
    (void *)DDClip_GetClipList,
    (void *)DDClip_GetHWnd,
    (void *)DDClip_Initialize,
    (void *)DDClip_IsClipListChanged,
    (void *)DDClip_SetClipList,
    (void *)DDClip_SetHWnd,
};

static HRESULT __stdcall DD_CreateClipper(MyDDraw *dd, DWORD dwFlags,
    void **ppClipper, void *pUnk)
{
    MyDDClipper *clip;
    (void)dd; (void)dwFlags; (void)pUnk;
    if (!ppClipper) return DDERR_INVALIDPARAMS;
    clip = (MyDDClipper *)calloc(1, sizeof(MyDDClipper));
    if (!clip) return DDERR_OUTOFMEMORY;
    clip->lpVtbl   = g_DDClipVtbl;
    clip->refCount = 1;
    *ppClipper = clip;
    LOG("DD_CreateClipper -> %p", (void *)clip);
    return DD_OK;
}

static HRESULT __stdcall DD_CreatePalette(MyDDraw *dd, DWORD dwFlags,
    PALETTEENTRY *pEntries, MyDDPalette **ppPal, void *pUnk)
{
    if (!ppPal) return DDERR_INVALIDPARAMS;
    MyDDPalette *pal = (MyDDPalette *)calloc(1, sizeof(MyDDPalette));
    if (!pal) return DDERR_OUTOFMEMORY;
    pal->lpVtbl   = g_DDPalVtbl;
    pal->refCount = 1;
    if (pEntries) {
        for (int i = 0; i < 256; i++) {
            pal->entries[i].rgbRed      = pEntries[i].peRed;
            pal->entries[i].rgbGreen    = pEntries[i].peGreen;
            pal->entries[i].rgbBlue     = pEntries[i].peBlue;
            pal->entries[i].rgbReserved = 0;
        }
    }
    *ppPal = pal;
    LOG("CreatePalette flags=%08X", dwFlags);
    return DD_OK;
}

static HRESULT __stdcall DD_CreateSurface(MyDDraw *dd, MY_DDSURFACEDESC *pDesc,
    MyDDSurface **ppSurf, void *pUnk)
{
    DWORD caps;
    int w;
    int h;
    DdwSurfaceFormat requested_format;

    if (!dd || !pDesc || !ppSurf)
        return DDERR_INVALIDPARAMS;
    *ppSurf = NULL;
    if (pDesc->dwWidth > (DWORD)INT_MAX
        || pDesc->dwHeight > (DWORD)INT_MAX)
        return DDERR_INVALIDPARAMS;
    if (!surface_format_from_desc(dd, pDesc, &requested_format)) {
        LOG("CreateSurface rejected unsupported pixel format flags=%08lX bpp=%lu masks=%08lX/%08lX/%08lX/%08lX",
            (unsigned long)pDesc->ddpfPixelFormat.dwFlags,
            (unsigned long)pDesc->ddpfPixelFormat.dwRGBBitCount,
            (unsigned long)pDesc->ddpfPixelFormat.dwRBitMask,
            (unsigned long)pDesc->ddpfPixelFormat.dwGBitMask,
            (unsigned long)pDesc->ddpfPixelFormat.dwBBitMask,
            (unsigned long)pDesc->ddpfPixelFormat.dwRGBAlphaBitMask);
        return DDERR_UNSUPPORTED;
    }

    caps = pDesc->ddsCaps.dwCaps;
    w = (int)pDesc->dwWidth;
    h = (int)pDesc->dwHeight;

    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (dd->gameWidth  > 0) w = dd->gameWidth;
        if (dd->gameHeight > 0) h = dd->gameHeight;
        if (w <= 0) w = 640;
        if (h <= 0) h = 480;

        MyDDSurface *primary = surf_create(
            dd, w, h, TRUE, FALSE, &requested_format);
        if (!primary) return DDERR_OUTOFMEMORY;

        DWORD backCount = pDesc->dwBackBufferCount ? pDesc->dwBackBufferCount : 1;
        if ((caps & DDSCAPS_FLIP) && backCount > 0) {
            MyDDSurface *back = surf_create(
                dd, w, h, FALSE, TRUE, &requested_format);
            if (!back) {
                DDSurf_Release(primary);
                return DDERR_OUTOFMEMORY;
            }
            primary->attachedBackBuffer = back;
        }
        dd->primarySurface = primary;
        *ppSurf = primary;
        LOG("CreateSurface primary %dx%d caps=%08X backCount=%u bpp=%lu masks=%08lX/%08lX/%08lX/%08lX formatGen=%lu",
            w, h, caps, backCount,
            (unsigned long)requested_format.bits_per_pixel,
            (unsigned long)requested_format.red_mask,
            (unsigned long)requested_format.green_mask,
            (unsigned long)requested_format.blue_mask,
            (unsigned long)requested_format.alpha_mask,
            (unsigned long)requested_format.generation);
        return DD_OK;
    }

    if (w <= 0 || h <= 0) { w = 1; h = 1; }
    MyDDSurface *surf = surf_create(
        dd, w, h, FALSE, FALSE, &requested_format);
    if (!surf) return DDERR_OUTOFMEMORY;
    *ppSurf = surf;
    LOG("CreateSurface offscreen %dx%d caps=%08X bpp=%lu masks=%08lX/%08lX/%08lX/%08lX formatGen=%lu",
        w, h, caps,
        (unsigned long)requested_format.bits_per_pixel,
        (unsigned long)requested_format.red_mask,
        (unsigned long)requested_format.green_mask,
        (unsigned long)requested_format.blue_mask,
        (unsigned long)requested_format.alpha_mask,
        (unsigned long)requested_format.generation);
    return DD_OK;
}

static HRESULT __stdcall DD_DuplicateSurface(MyDDraw *dd,
    MyDDSurface *pSrc, MyDDSurface **ppDup)
{
    if (!pSrc || !ppDup) return DDERR_INVALIDPARAMS;
    DDSurf_AddRef(pSrc);
    *ppDup = pSrc;
    return DD_OK;
}

static HRESULT __stdcall DD_EnumDisplayModes(MyDDraw *dd, DWORD dwFlags,
    MY_DDSURFACEDESC *pDesc, void *pCtx, void *pfn) { return DD_OK; }
static HRESULT __stdcall DD_EnumSurfaces(MyDDraw *dd, DWORD dwFlags,
    MY_DDSURFACEDESC *pDesc, void *pCtx, void *pfn) { return DD_OK; }
static HRESULT __stdcall DD_FlipToGDISurface(MyDDraw *dd) { return DD_OK; }
static HRESULT __stdcall DD_GetCaps(MyDDraw *dd, void *pDDCaps, void *pDDCaps2)
    { return DD_OK; }

static void fill_pixel_format(
    MY_DDPIXELFORMAT *pf,
    const DdwSurfaceFormat *format)
{
    if (!pf)
        return;
    ZeroMemory(pf, sizeof(*pf));
    pf->dwSize = sizeof(MY_DDPIXELFORMAT);
    if (!format)
        return;
    if (format->kind == DDW_PIXEL_FORMAT_INDEX8) {
        pf->dwFlags       = DDPF_PALETTEINDEXED8;
        pf->dwRGBBitCount = 8;
    } else {
        pf->dwFlags       = DDPF_RGB;
        if (format->alpha_mask != 0u)
            pf->dwFlags |= DDPF_ALPHAPIXELS;
        pf->dwRGBBitCount = format->bits_per_pixel;
        pf->dwRBitMask    = format->red_mask;
        pf->dwGBitMask    = format->green_mask;
        pf->dwBBitMask    = format->blue_mask;
        pf->dwRGBAlphaBitMask = format->alpha_mask;
    }
}

static HRESULT __stdcall DD_GetDisplayMode(MyDDraw *dd, MY_DDSURFACEDESC *pDesc)
{
    int w;
    int h;
    int bytesPP;
    DdwSurfaceLayout layout;

    if (!dd || !pDesc)
        return DDERR_INVALIDPARAMS;
    w = dd->gameWidth  > 0 ? dd->gameWidth  : 640;
    h = dd->gameHeight > 0 ? dd->gameHeight : 480;
    bytesPP = (dd->modeBpp == 8) ? 1 : 2;
    if (!ddw_compute_surface_layout(w, h, bytesPP, &layout))
        return DDERR_INVALIDPARAMS;
    pDesc->dwFlags  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    pDesc->dwWidth  = (DWORD)w;
    pDesc->dwHeight = (DWORD)h;
    pDesc->lPitch   = layout.pitch;
    {
        DdwSurfaceFormat format = dd->modeBpp == 8
            ? ddw_surface_format_index8(0u)
            : ddw_surface_format_rgb565(0u);
        fill_pixel_format(&pDesc->ddpfPixelFormat, &format);
    }
    return DD_OK;
}

static HRESULT __stdcall DD_GetFourCCCodes(MyDDraw *dd, DWORD *pNum, DWORD *pCodes)
    { return DD_OK; }
static HRESULT __stdcall DD_GetGDISurface(MyDDraw *dd, MyDDSurface **ppSurf)
    { return DDERR_UNSUPPORTED; }
static HRESULT __stdcall DD_GetMonitorFrequency(MyDDraw *dd, DWORD *pFreq)
    { if (pFreq) *pFreq = 60; return DD_OK; }
static HRESULT __stdcall DD_GetScanLine(MyDDraw *dd, DWORD *pSL)
    { if (pSL) *pSL = 0; return DD_OK; }
static HRESULT __stdcall DD_GetVerticalBlankStatus(MyDDraw *dd, BOOL *pbInVB)
    { if (pbInVB) *pbInVB = FALSE; return DD_OK; }
static HRESULT __stdcall DD_Initialize(MyDDraw *dd, GUID *pGUID)
    { return DDERR_UNSUPPORTED; }
static HRESULT __stdcall DD_RestoreDisplayMode(MyDDraw *dd)
    { LOG("RestoreDisplayMode"); return DD_OK; }

static HRESULT __stdcall DD_SetCooperativeLevel(MyDDraw *dd, HWND hWnd, DWORD dwFlags)
{
    LOG("SetCooperativeLevel hWnd=%p flags=%08X", (void *)hWnd, dwFlags);
    dd->hWnd = hWnd;
    g_dd     = dd;

    if (hWnd && !dd->origWndProc) {
        dd->origWndProc = (WNDPROC)SetWindowLongPtrA(
            hWnd, GWLP_WNDPROC, (LONG_PTR)WrapperWndProc);
        LOG("WndProc hooked (orig=%p)", (void *)dd->origWndProc);
    }

    install_hotkey_hook();

    return DD_OK;
}

/* IDirectDraw2 form, 5 args, __stdcall: a 3-arg implementation here would corrupt the caller's stack. */
static HRESULT __stdcall DD_SetDisplayMode(MyDDraw *dd,
    DWORD dwWidth, DWORD dwHeight, DWORD dwBPP,
    DWORD dwRefreshRate, DWORD dwFlags)
{
    int width;
    int height;
    int modeBpp;
    int bytesPP;
    DdwSurfaceLayout layout;

    (void)dwRefreshRate; (void)dwFlags;
    if (!dd || dwWidth == 0 || dwHeight == 0
        || dwWidth > (DWORD)INT_MAX || dwHeight > (DWORD)INT_MAX)
        return DDERR_INVALIDPARAMS;

    width = (int)dwWidth;
    height = (int)dwHeight;
    modeBpp = (dwBPP == 8) ? 8 : 16;
    bytesPP = modeBpp == 8 ? 1 : 2;
    if (!ddw_compute_surface_layout(width, height, bytesPP, &layout))
        return DDERR_INVALIDPARAMS;

    dd->gameWidth  = width;
    dd->gameHeight = height;
    dd->modeBpp    = modeBpp;
    qoh99_movie_set_game_resolution(width, height);
    LOG("SetDisplayMode %ux%ux%u (emulating %dbpp)", dwWidth, dwHeight, dwBPP, dd->modeBpp);

    if (dd->hWnd) {
        set_windowed_mode(dd);
        if (InterlockedCompareExchange(&g_displayRestoreDone, 0, 0) == 0
            && (g_restorePresentationMode != QOH_PRESENTATION_WINDOWED
                || g_restoreClientW > 0
                || g_cfg.window_scale > 1)) {
            InterlockedExchange(&g_displayRestorePending, 1);
            LOG("display restore armed: mode=%d client=%dx%d scale=%d",
                (int)g_restorePresentationMode, g_restoreClientW, g_restoreClientH,
                g_cfg.window_scale);
        }
    }

    return DD_OK;
}

static HRESULT __stdcall DD_WaitForVerticalBlank(MyDDraw *dd,
    DWORD dwFlags, HANDLE hEvent)
{
    if (dwFlags == 1) {
        const DWORD disposition = qoh99_presentation_disposition();
        if (!qoh99_presentation_executes_pacing(disposition)) {
            ++g_qoh99SuppressedPacingCalls;
            g_fpsPacedThisFlip = FALSE;
            return DD_OK;
        }
        fps_throttle();
        g_fpsPacedThisFlip = TRUE;
    }
    return DD_OK;
}

static HRESULT __stdcall DD2_GetAvailableVidMem(MyDDraw *dd,
    MY_DDSCAPS *pCaps, DWORD *pdwTotal, DWORD *pdwFree)
{
    (void)dd; (void)pCaps;
    if (pdwTotal) *pdwTotal = 64u * 1024u * 1024u;
    if (pdwFree)  *pdwFree  = 64u * 1024u * 1024u;
    return DD_OK;
}

static void *g_DDVtbl[] = {
    (void *)DD_QueryInterface,
    (void *)DD_AddRef,
    (void *)DD_Release,
    (void *)DD_Compact,
    (void *)DD_CreateClipper,
    (void *)DD_CreatePalette,
    (void *)DD_CreateSurface,
    (void *)DD_DuplicateSurface,
    (void *)DD_EnumDisplayModes,
    (void *)DD_EnumSurfaces,
    (void *)DD_FlipToGDISurface,
    (void *)DD_GetCaps,
    (void *)DD_GetDisplayMode,
    (void *)DD_GetFourCCCodes,
    (void *)DD_GetGDISurface,
    (void *)DD_GetMonitorFrequency,
    (void *)DD_GetScanLine,
    (void *)DD_GetVerticalBlankStatus,
    (void *)DD_Initialize,
    (void *)DD_RestoreDisplayMode,
    (void *)DD_SetCooperativeLevel,
    (void *)DD_SetDisplayMode,
    (void *)DD_WaitForVerticalBlank,
    (void *)DD2_GetAvailableVidMem,
};

static void sjis_to_wide(const char *src, WCHAR *dst, int dstChars)
{
    if (!MultiByteToWideChar(932, 0, src, -1, dst, dstChars))
        MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dstChars);
    dst[dstChars - 1] = 0;
}

static int WINAPI Hook_MessageBoxA(HWND hWnd, LPCSTR text, LPCSTR caption, UINT type)
{
    WCHAR wText[2048], wCap[256];
    LPCWSTR pText = NULL, pCap = NULL;
    if (text)    { sjis_to_wide(text, wText, 2048);  pText = wText; }
    if (caption) { sjis_to_wide(caption, wCap, 256); pCap  = wCap; }
    LOG("MessageBoxA intercepted (CP932->W), type=%08X", type);
    return MessageBoxW(hWnd, pText, pCap, type);
}

static HFONT WINAPI Hook_CreateFontIndirectA(const LOGFONTA *lfA)
{
    LOGFONTW lfW;
    if (!lfA) return NULL;
    memcpy(&lfW, lfA, FIELD_OFFSET(LOGFONTA, lfFaceName));
    sjis_to_wide(lfA->lfFaceName, lfW.lfFaceName, LF_FACESIZE);
    LOG("CreateFontIndirectA (CP932->W) charset=%u height=%ld",
        lfA->lfCharSet, lfA->lfHeight);
    return CreateFontIndirectW(&lfW);
}

static void hook_locale_iat(void)
{
    static LONG s_done = 0;
    if (InterlockedExchange(&s_done, 1))
        return;
    if (wrapper_iat_patch("USER32.dll", "MessageBoxA",
                          (void *)Hook_MessageBoxA, NULL) > 0) {
        LOG("Locale hook: MessageBoxA -> CP932-aware MessageBoxW");
    }
    if (wrapper_iat_patch("GDI32.dll", "CreateFontIndirectA",
                          (void *)Hook_CreateFontIndirectA, NULL) > 0) {
        LOG("Locale hook: CreateFontIndirectA -> CP932-aware CreateFontIndirectW");
    }
    qoh99_iat_txn_commit(&g_wrapperIatTxn);
}

static void enable_dpi_awareness(void)
{
    static LONG s_done = 0;
    HMODULE user32;
    if (InterlockedExchange(&s_done, 1))
        return;
    user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        SetCtxFn setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) {
            if (setCtx((HANDLE)-4) || setCtx((HANDLE)-3)) {
                LOG("DPI: per-monitor awareness enabled (SetProcessDpiAwarenessContext)");
                return;
            }
        }
        {
            typedef BOOL (WINAPI *SetDpiFn)(void);
            SetDpiFn setDpi = (SetDpiFn)GetProcAddress(user32, "SetProcessDPIAware");
            if (setDpi && setDpi()) {
                LOG("DPI: system awareness enabled (SetProcessDPIAware)");
                return;
            }
        }
    }
    LOG("DPI: awareness APIs unavailable; running DPI-unaware");
}

__declspec(dllexport)
BOOL __stdcall EFZDDRAW_SetFrameLimit(UINT fps)
{
    wrapper_lazy_init();
    if (fps != 0u && (fps < 30u || fps > 240u)) {
        LOG("SetFrameLimit rejected fps=%u", fps);
        return FALSE;
    }

    InterlockedExchange(&g_runtimeFpsLimit, (LONG)fps);
    InterlockedIncrement(&g_fpsLimitGeneration);
    if (fps == 0u) {
        pace_release_timer_period();
    }
    LOG("SetFrameLimit fps=%u generation=%ld", fps,
        InterlockedCompareExchange(&g_fpsLimitGeneration, 0, 0));
    return TRUE;
}

__declspec(dllexport)
BOOL __stdcall EFZDDRAW_PrepareOverlayDevice(void)
{
    RECT cr;
    BOOL ok;
    wrapper_lazy_init();
    if (!g_dd || !g_dd->hWnd || !IsWindow(g_dd->hWnd)) {
        LOG("PrepareOverlayDevice: no game window yet");
        return FALSE;
    }
    if (EFZDX9_Presenter_HasDevice())
        return TRUE;
    if (!GetClientRect(g_dd->hWnd, &cr)
        || cr.right - cr.left <= 0 || cr.bottom - cr.top <= 0) {
        LOG("PrepareOverlayDevice: client rect unavailable");
        return FALSE;
    }
    ok = EFZDX9_Presenter_PrepareDevice(
        g_dd->hWnd, cr.right - cr.left, cr.bottom - cr.top,
        g_dd->presentationMode, g_cfg.renderer_backend);
    LOG("PrepareOverlayDevice: hwnd=%p client=%ldx%ld mode=%d backend=%d ready=%d",
        (void *)g_dd->hWnd, cr.right - cr.left, cr.bottom - cr.top,
        (int)g_dd->presentationMode, (int)g_cfg.renderer_backend, (int)ok);
    return ok;
}

static LARGE_INTEGER g_heldFramePresentQpc;
static DWORD g_heldFramePresents = 0;
static DWORD g_heldFramePresentsCoalesced = 0;
static DWORD g_heldFramePresentFailures = 0;

__declspec(dllexport)
BOOL __stdcall EFZDDRAW_PresentHeldFrame(void)
{
    LARGE_INTEGER now;
    LONG limit;
    LONGLONG minTicks;
    BOOL published;

    wrapper_lazy_init();
    if (!g_dd || !g_dd->hWnd || !IsWindow(g_dd->hWnd))
        return FALSE;
    if (!g_dd->primarySurface)
        return FALSE;

    if (pace_qpc_usable() && QueryPerformanceCounter(&now)) {
        limit = InterlockedCompareExchange(&g_runtimeFpsLimit, 0, 0);
        if (limit <= 0)
            limit = 60;
        minTicks = g_qpcFreq.QuadPart / ((LONGLONG)limit * 2);
        if (g_heldFramePresentQpc.QuadPart != 0
            && now.QuadPart >= g_heldFramePresentQpc.QuadPart
            && now.QuadPart - g_heldFramePresentQpc.QuadPart < minTicks) {
            ++g_heldFramePresentsCoalesced;
            return FALSE;
        }
        g_heldFramePresentQpc = now;
    }

    ++g_heldFramePresents;
    {
        EfzDx9PresentationKind priorKind = g_repaintPresentationKind;
        g_repaintPresentationKind = EFZ_DX9_PRESENT_HELD_REPAINT;
        published = qoh99_netplay_owns_pacing()
            ? present_snapshot(g_dd) : present_frame(g_dd, FALSE);
        g_repaintPresentationKind = priorKind;
    }
    if (!published)
        ++g_heldFramePresentFailures;
    if (g_heldFramePresents <= 4u || (g_heldFramePresents % 600u) == 0u) {
        LOG("[held-frame] repaint=%lu published=%d coalesced=%lu failed=%lu",
            (unsigned long)g_heldFramePresents, (int)published,
            (unsigned long)g_heldFramePresentsCoalesced,
            (unsigned long)g_heldFramePresentFailures);
    }
    return published;
}

__declspec(dllexport)
UINT __stdcall EFZDDRAW_GetNetplayPacingAbi(void)
{
    wrapper_lazy_init();
    return 3u;
}

static volatile LONG g_processExiting = 0;
static volatile LONG g_lazyInitState = 0;
static volatile LONG g_runtimeReleased = 0;

static void qoh99_report_december_startup_gate(void);

#define QOH99_CONFIG_KEY_BYTES      8044u
#define QOH99_CONFIG_PROFILE_BASE      4u
#define QOH99_CONFIG_PROFILE_STRIDE  764u
#define QOH99_CONFIG_FLAGS_OFFSET    376u
#define QOH99_CONFIG_FULLCOLOR_BIT     1u

#define QOH99_LIVE_PROFILE_POINTER 0x613DE0u

static BOOL qoh99_address_is_readable(const void *address, SIZE_T bytes,
                                      BOOL needWrite)
{
    MEMORY_BASIC_INFORMATION info;
    DWORD protect;
    if (address == NULL)
        return FALSE;
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info))
        return FALSE;
    if (info.State != MEM_COMMIT)
        return FALSE;
    if ((BYTE *)address + bytes
        > (BYTE *)info.BaseAddress + info.RegionSize)
        return FALSE;
    protect = info.Protect & 0xFFu;
    if (protect == 0 || (info.Protect & PAGE_GUARD) != 0)
        return FALSE;
    if (needWrite) {
        return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY
            || protect == PAGE_EXECUTE_READWRITE
            || protect == PAGE_EXECUTE_WRITECOPY;
    }
    return protect != PAGE_NOACCESS;
}

static void qoh99_force_full_color_live_profile(void)
{
    const DWORD *pointerSlot = (const DWORD *)QOH99_LIVE_PROFILE_POINTER;
    DWORD profile;
    DWORD *flags;
    DWORD before;

    if (!qoh99_address_is_readable(pointerSlot, sizeof(DWORD), FALSE)) {
        LOG("[fullcolor] live profile pointer %08X is not readable yet; "
            "the QOHcnf.key edit applies from the next launch",
            QOH99_LIVE_PROFILE_POINTER);
        return;
    }
    profile = *pointerSlot;
    if (profile == 0) {
        LOG("[fullcolor] the game has not loaded a profile yet "
            "(dword_613DE0=0); the QOHcnf.key edit applies from the next "
            "launch");
        return;
    }
    flags = (DWORD *)(ULONG_PTR)(profile + QOH99_CONFIG_FLAGS_OFFSET);
    if (!qoh99_address_is_readable(flags, sizeof(DWORD), TRUE)) {
        LOG("[fullcolor] live profile %08X+%u is not writable; leaving it "
            "alone", profile, QOH99_CONFIG_FLAGS_OFFSET);
        return;
    }
    before = *flags;
    if ((before & QOH99_CONFIG_FULLCOLOR_BIT) != 0u) {
        LOG("[fullcolor] live profile %08X already full colour "
            "(flags=0x%08lX)", profile, (unsigned long)before);
        return;
    }
    /* Change only this bit: NoSound (1) and Vsync (2) share this dword and belong to the player. */
    *flags = before | QOH99_CONFIG_FULLCOLOR_BIT;
    LOG("[fullcolor] live profile %08X flags 0x%08lX -> 0x%08lX - effects "
        "enabled for THIS run", profile, (unsigned long)before,
        (unsigned long)*flags);
}

static void qoh99_force_full_color_config(void)
{
    static const char *const kPath = ".\\System\\QOHcnf.key";
    HANDLE file;
    DWORD moved;
    DWORD got = 0;
    DWORD wrote = 0;
    unsigned char header[4];
    unsigned char flags[4];
    unsigned int index;
    unsigned int flagsValue;
    LARGE_INTEGER size;
    LONG offset;

    file = CreateFileA(kPath, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LOG("[fullcolor] no %s (err=%lu); leaving colour depth to the game",
            kPath, GetLastError());
        return;
    }
    if (!GetFileSizeEx(file, &size)
        || size.QuadPart != (LONGLONG)QOH99_CONFIG_KEY_BYTES) {
        LOG("[fullcolor] %s is %lld bytes, not %u - unrecognised layout, "
            "left untouched", kPath, (long long)size.QuadPart,
            QOH99_CONFIG_KEY_BYTES);
        CloseHandle(file);
        return;
    }
    if (!ReadFile(file, header, sizeof(header), &got, NULL)
        || got != sizeof(header)) {
        LOG("[fullcolor] could not read the profile index from %s (err=%lu)",
            kPath, GetLastError());
        CloseHandle(file);
        return;
    }
    index = (unsigned int)header[0] | ((unsigned int)header[1] << 8)
          | ((unsigned int)header[2] << 16) | ((unsigned int)header[3] << 24);
    if (index > (QOH99_CONFIG_KEY_BYTES - QOH99_CONFIG_PROFILE_BASE)
                    / QOH99_CONFIG_PROFILE_STRIDE
        || QOH99_CONFIG_PROFILE_BASE
               + index * QOH99_CONFIG_PROFILE_STRIDE
               + QOH99_CONFIG_FLAGS_OFFSET + 4u > QOH99_CONFIG_KEY_BYTES) {
        LOG("[fullcolor] %s names profile %u, which does not fit the file - "
            "left untouched", kPath, index);
        CloseHandle(file);
        return;
    }
    offset = (LONG)(QOH99_CONFIG_PROFILE_BASE
                    + index * QOH99_CONFIG_PROFILE_STRIDE
                    + QOH99_CONFIG_FLAGS_OFFSET);
    moved = SetFilePointer(file, offset, NULL, FILE_BEGIN);
    if (moved == INVALID_SET_FILE_POINTER
        || !ReadFile(file, flags, sizeof(flags), &got, NULL)
        || got != sizeof(flags)) {
        LOG("[fullcolor] could not read the flags at offset %ld (err=%lu)",
            offset, GetLastError());
        CloseHandle(file);
        return;
    }
    flagsValue = (unsigned int)flags[0] | ((unsigned int)flags[1] << 8)
               | ((unsigned int)flags[2] << 16) | ((unsigned int)flags[3] << 24);
    if ((flagsValue & QOH99_CONFIG_FULLCOLOR_BIT) != 0u) {
        LOG("[fullcolor] already on: profile %u flags=0x%08X "
            "(16bit=1 nosound=%u vsync=%u)",
            index, flagsValue, (flagsValue >> 1) & 1u,
            (flagsValue >> 2) & 1u);
        CloseHandle(file);
        return;
    }
    flagsValue |= QOH99_CONFIG_FULLCOLOR_BIT;
    flags[0] = (unsigned char)(flagsValue & 0xFFu);
    flags[1] = (unsigned char)((flagsValue >> 8) & 0xFFu);
    flags[2] = (unsigned char)((flagsValue >> 16) & 0xFFu);
    flags[3] = (unsigned char)((flagsValue >> 24) & 0xFFu);
    moved = SetFilePointer(file, offset, NULL, FILE_BEGIN);
    if (moved == INVALID_SET_FILE_POINTER
        || !WriteFile(file, flags, sizeof(flags), &wrote, NULL)
        || wrote != sizeof(flags)) {
        WLOG_ISSUE("[fullcolor] FAILED to write the flags at offset %ld (err=%lu); "
            "effects will not render this run", offset, GetLastError());
        CloseHandle(file);
        return;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    LOG("[fullcolor] ENABLED for profile %u: flags 0x%08X -> 0x%08X "
        "(effects are gated on bit 0; CONFIG.EXE checkbox 1043 owns it)",
        index, flagsValue & ~QOH99_CONFIG_FULLCOLOR_BIT, flagsValue);
}

static void wrapper_lazy_init(void)
{
    if (InterlockedCompareExchange(&g_lazyInitState, 0, 0) == 2)
        return;
    if (InterlockedCompareExchange(&g_processExiting, 0, 0) != 0)
        return;

    if (InterlockedCompareExchange(&g_lazyInitState, 1, 0) != 0) {
        int spins = 0;
        while (InterlockedCompareExchange(&g_lazyInitState, 0, 0) != 2
               && ++spins < 4000) {
            if (!SwitchToThread())
                Sleep(1);
        }
        return;
    }

    /* Must not reorder: logging, then config (publishes the frame limit), then DPI awareness (must precede the game's window creation), then import patches. */
    qoh99_wlog_init();
    LOG("wrapper init: pid=%lu module=%p built " __DATE__ " " __TIME__,
        (unsigned long)GetCurrentProcessId(), (void *)g_hInst);
    qoh99_report_december_startup_gate();
    config_load();
    wrapper_hotkeys_load();
    qoh99_force_full_color_config();
    enable_dpi_awareness();
    hook_dinput_iat();
    hook_locale_iat();
    hook_gdi_effect_diagnostic_iat();

    qoh99_movie_install();

    InterlockedExchange(&g_lazyInitState, 2);
}

static void wrapper_release_runtime(const char *reason)
{
    if (InterlockedExchange(&g_runtimeReleased, 1) != 0)
        return;

    qoh99_movie_shutdown();

    LOG("[netplay-present-final] reason=%s normalVisiblePresents=%lu "
        "suppressedPresents=%lu correctedFinalPresents=%lu "
        "hiddenColorFills=%lu hiddenBlts=%lu hiddenBltFasts=%lu "
        "hiddenPacingCalls=%lu",
        reason ? reason : "unknown",
        (unsigned long)g_qoh99NormalVisiblePresents,
        (unsigned long)g_qoh99SuppressedPresents,
        (unsigned long)g_qoh99CorrectedFinalPresents,
        (unsigned long)g_qoh99SuppressedColorFills,
        (unsigned long)g_qoh99SuppressedBlts,
         (unsigned long)g_qoh99SuppressedBltFasts,
         (unsigned long)g_qoh99SuppressedPacingCalls);
    qoh99_log_dc_summary("wrapper-release");
    pace_log_summary();

    release_hotkey_hook();
    pace_release_timer_period();
    EFZDX9_Presenter_Shutdown();
    qoh99_instr_shutdown();
    LOG("wrapper runtime released (%s)", reason ? reason : "unknown");
}

static const GUID QOH_IID_IDirectDraw7 =
    { 0x15E65EC0, 0x3B9C, 0x11D2, { 0xB9,0x2F,0x00,0x60,0x97,0x97,0xEA,0x5B } };
static const GUID QOH_IID_IDirectDraw4 =
    { 0x9C59509A, 0x39BD, 0x11D1, { 0x8C,0x4A,0x00,0xC0,0x4F,0xD9,0x30,0xC5 } };

typedef BOOL (WINAPI *QohDDEnumCallbackA)(GUID *, LPSTR, LPSTR, LPVOID);
typedef BOOL (WINAPI *QohDDEnumCallbackW)(GUID *, LPWSTR, LPWSTR, LPVOID);
typedef BOOL (WINAPI *QohDDEnumCallbackExA)(GUID *, LPSTR, LPSTR, LPVOID, HMONITOR);
typedef BOOL (WINAPI *QohDDEnumCallbackExW)(GUID *, LPWSTR, LPWSTR, LPVOID, HMONITOR);

static char  g_ddEnumDescA[] = "QOH99 DirectDraw wrapper (primary display)";
static char  g_ddEnumNameA[] = "display";
static WCHAR g_ddEnumDescW[] = L"QOH99 DirectDraw wrapper (primary display)";
static WCHAR g_ddEnumNameW[] = L"display";

__declspec(dllexport)
HRESULT __stdcall DirectDrawCreate(GUID *lpGUID, void **lplpDD, void *pUnkOuter)
{
    MyDDraw *dd;

    wrapper_lazy_init();
    LOG("DirectDrawCreate called (pid=%lu guid=%p outer=%p)",
        (unsigned long)GetCurrentProcessId(), (void *)lpGUID, pUnkOuter);
    if (!lplpDD)
        return DDERR_INVALIDPARAMS;
    if (pUnkOuter != NULL) {
        LOG("DirectDrawCreate refused: aggregation is not supported (outer=%p)",
            pUnkOuter);
        *lplpDD = NULL;
        return 0x80040110L;
    }

    dd = (MyDDraw *)calloc(1, sizeof(MyDDraw));
    if (!dd)
        return DDERR_OUTOFMEMORY;
    dd->lpVtbl   = g_DDVtbl;
    dd->refCount = 1;
    dd->modeBpp  = QOH_DEFAULT_BPP;
    dd->presentationMode = QOH_PRESENTATION_WINDOWED;
    dd->lastFullscreenMode =
        g_cfg.presentation_mode == QOH_PRESENTATION_EXCLUSIVE
            ? QOH_PRESENTATION_EXCLUSIVE
            : QOH_PRESENTATION_BORDERLESS;
    g_dd  = dd;
    *lplpDD = dd;

    LOG("DirectDrawCreate OK -> %p", (void *)dd);
    return DD_OK;
}

__declspec(dllexport)
HRESULT __stdcall DirectDrawCreateEx(GUID *lpGUID, void **lplpDD,
    REFIID iid, void *pUnkOuter)
{
    const GUID *riid = (const GUID *)iid;

    wrapper_lazy_init();
    if (!lplpDD)
        return DDERR_INVALIDPARAMS;
    if (riid == NULL
        || guid_equal(riid, &QOH_IID_IUnknown)
        || guid_equal(riid, &QOH_IID_IDirectDraw)
        || guid_equal(riid, &QOH_IID_IDirectDraw2)) {
        return DirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
    }
    LOG("DirectDrawCreateEx refused: unsupported interface {%08lX-%04X-%04X} "
        "(idd7=%d idd4=%d); this wrapper only implements IDirectDraw/IDirectDraw2",
        (unsigned long)riid->Data1, (unsigned)riid->Data2, (unsigned)riid->Data3,
        (int)guid_equal(riid, &QOH_IID_IDirectDraw7),
        (int)guid_equal(riid, &QOH_IID_IDirectDraw4));
    *lplpDD = NULL;
    return DDERR_INVALIDPARAMS;
}

__declspec(dllexport)
HRESULT __stdcall DirectDrawCreateClipper(DWORD dwFlags,
    void **ppClipper, void *pUnkOuter)
{
    HRESULT hr;

    wrapper_lazy_init();
    if (!ppClipper)
        return DDERR_INVALIDPARAMS;
    if (pUnkOuter != NULL) {
        LOG("DirectDrawCreateClipper refused: aggregation is not supported");
        *ppClipper = NULL;
        return 0x80040110L;
    }
    hr = DD_CreateClipper(NULL, dwFlags, ppClipper, pUnkOuter);
    LOG("DirectDrawCreateClipper flags=%08lX hr=%08lX clipper=%p",
        (unsigned long)dwFlags, (unsigned long)hr, *ppClipper);
    return hr;
}

__declspec(dllexport)
HRESULT __stdcall DirectDrawEnumerateA(void *pfn, void *pCtx)
{
    wrapper_lazy_init();
    if (!pfn) {
        LOG("DirectDrawEnumerateA: null callback");
        return DDERR_INVALIDPARAMS;
    }
    ((QohDDEnumCallbackA)pfn)(NULL, g_ddEnumDescA, g_ddEnumNameA, pCtx);
    LOG("DirectDrawEnumerateA: reported the primary display");
    return DD_OK;
}

__declspec(dllexport)
HRESULT __stdcall DirectDrawEnumerateW(void *pfn, void *pCtx)
{
    wrapper_lazy_init();
    if (!pfn) {
        LOG("DirectDrawEnumerateW: null callback");
        return DDERR_INVALIDPARAMS;
    }
    ((QohDDEnumCallbackW)pfn)(NULL, g_ddEnumDescW, g_ddEnumNameW, pCtx);
    LOG("DirectDrawEnumerateW: reported the primary display");
    return DD_OK;
}

__declspec(dllexport)
HRESULT __stdcall DirectDrawEnumerateExA(void *pfn, void *pCtx, DWORD f)
{
    HMONITOR primary;
    POINT origin;

    wrapper_lazy_init();
    if (!pfn) {
        LOG("DirectDrawEnumerateExA: null callback (flags=%08lX)",
            (unsigned long)f);
        return DDERR_INVALIDPARAMS;
    }
    origin.x = 0;
    origin.y = 0;
    primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    ((QohDDEnumCallbackExA)pfn)(NULL, g_ddEnumDescA, g_ddEnumNameA, pCtx,
                                primary);
    LOG("DirectDrawEnumerateExA: reported the primary display only (flags=%08lX)",
        (unsigned long)f);
    return DD_OK;
}

__declspec(dllexport)
HRESULT __stdcall DirectDrawEnumerateExW(void *pfn, void *pCtx, DWORD f)
{
    HMONITOR primary;
    POINT origin;

    wrapper_lazy_init();
    if (!pfn) {
        LOG("DirectDrawEnumerateExW: null callback (flags=%08lX)",
            (unsigned long)f);
        return DDERR_INVALIDPARAMS;
    }
    origin.x = 0;
    origin.y = 0;
    primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    ((QohDDEnumCallbackExW)pfn)(NULL, g_ddEnumDescW, g_ddEnumNameW, pCtx,
                                primary);
    LOG("DirectDrawEnumerateExW: reported the primary display only (flags=%08lX)",
        (unsigned long)f);
    return DD_OK;
}

/* S_FALSE, always: this DLL owns process-wide state (window subclass, IAT
   patches, a D3D9 device and its worker) and must never be unloaded. */
__declspec(dllexport) HRESULT __stdcall DllCanUnloadNow(void) { return 1 ; }

__declspec(dllexport)
HRESULT __stdcall DllGetClassObject(REFIID rclsid, REFIID riid, void **ppv)
{
    const GUID *clsid = (const GUID *)rclsid;

    wrapper_lazy_init();
    if (ppv)
        *ppv = NULL;
    LOG("DllGetClassObject refused: COM activation is not implemented "
        "(clsid={%08lX-%04X-%04X}); use DirectDrawCreate",
        clsid ? (unsigned long)clsid->Data1 : 0ul,
        clsid ? (unsigned)clsid->Data2 : 0u,
        clsid ? (unsigned)clsid->Data3 : 0u);
    return 0x80040111L;
}

#define QOH99_DEC_GATE_VA   0x00471784u
#define QOH99_DEC_GATE_LEN  26u

enum {
    QOH99_GATE_UNTOUCHED   = 0,
    QOH99_GATE_PATCHED     = 1,
    QOH99_GATE_NOT_PRESENT = 2,
    QOH99_GATE_UNKNOWN     = 3,
    QOH99_GATE_FAILED      = 4
};
static LONG g_decemberGateState = QOH99_GATE_UNTOUCHED;

static void qoh99_neutralize_december_startup_gate(void)
{
    static const unsigned char kGate[QOH99_DEC_GATE_LEN] = {
        0x81, 0xBC, 0x24, 0x1C, 0x01, 0x00, 0x00, 0x00, 0x1B, 0x3E, 0xDA,
        0x75, 0x1B,
        0x81, 0xBC, 0x24, 0x20, 0x01, 0x00, 0x00, 0xC3, 0xA6, 0xBF, 0x01,
        0x75, 0x0E
    };
    unsigned char *site = (unsigned char *)QOH99_DEC_GATE_VA;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD previous = 0;
    unsigned int i;
    int alreadyNops = 1;

    if ((void *)GetModuleHandleA(NULL) != (void *)0x00400000) {
        InterlockedExchange(&g_decemberGateState, QOH99_GATE_UNKNOWN);
        return;
    }
    if (VirtualQuery((LPCVOID)site, &mbi, sizeof(mbi)) != sizeof(mbi)
        || mbi.State != MEM_COMMIT) {
        InterlockedExchange(&g_decemberGateState, QOH99_GATE_UNKNOWN);
        return;
    }
    for (i = 0; i < QOH99_DEC_GATE_LEN; ++i) {
        if (site[i] != 0x90) { alreadyNops = 0; break; }
    }
    if (alreadyNops) {
        InterlockedExchange(&g_decemberGateState, QOH99_GATE_NOT_PRESENT);
        return;
    }
    if (memcmp(site, kGate, QOH99_DEC_GATE_LEN) != 0) {
        InterlockedExchange(&g_decemberGateState, QOH99_GATE_UNKNOWN);
        return;
    }
    if (!VirtualProtect((LPVOID)site, QOH99_DEC_GATE_LEN,
                        PAGE_EXECUTE_READWRITE, &previous)) {
        InterlockedExchange(&g_decemberGateState, QOH99_GATE_FAILED);
        return;
    }
    memset(site, 0x90, QOH99_DEC_GATE_LEN);
    VirtualProtect((LPVOID)site, QOH99_DEC_GATE_LEN, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site,
                          QOH99_DEC_GATE_LEN);
    InterlockedExchange(&g_decemberGateState, QOH99_GATE_PATCHED);
}

static void qoh99_report_december_startup_gate(void)
{
    switch (InterlockedCompareExchange(&g_decemberGateState, 0, 0)) {
        case QOH99_GATE_PATCHED:
            LOG("december startup gate: NEUTRALIZED at %08X (26 bytes -> NOP). "
                "That build refuses to start when System\\WinMes\\Tir_Mes1.Img "
                "has a different modification time, which any copy or archive "
                "extraction changes; the November build already ships this site "
                "NOPped and we reproduce it.", QOH99_DEC_GATE_VA);
            break;
        case QOH99_GATE_NOT_PRESENT:
            LOG("december startup gate: not present at %08X (already NOPs) - "
                "November, or neutralized earlier", QOH99_DEC_GATE_VA);
            break;
        case QOH99_GATE_UNKNOWN:
            LOG("december startup gate: unrecognized bytes at %08X - left "
                "untouched", QOH99_DEC_GATE_VA);
            break;
        case QOH99_GATE_FAILED:
            LOG("december startup gate: VirtualProtect refused at %08X - the "
                "game may close with a file-date error before its menu",
                QOH99_DEC_GATE_VA);
            break;
        default:
            break;
    }
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            g_hInst = hInstDLL;
            DisableThreadLibraryCalls(hInstDLL);
            /* Must run here: WinMain runs the file-date gate before touching DirectDraw, so the lazy init is too late. */
            qoh99_neutralize_december_startup_gate();
            break;
        case DLL_PROCESS_DETACH:
            InterlockedExchange(&g_processExiting, 1);
            qoh99_wlog_mark_process_exit();
            break;
        default:
            break;
    }
    return TRUE;
}
