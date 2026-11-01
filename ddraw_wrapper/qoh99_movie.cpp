#include "qoh99_movie.h"

#include "ddraw_log.h"
#include "ddraw_pe.h"
#include "surface_ops.h"
#include "game/OpeningMovieCodec.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dshow.h>
#include <mmsystem.h>
#include <vfw.h>

#include <stdio.h>
#include <string.h>

#define LOG(...) \
    do { if (qoh99_wlog_should()) qoh99_wlog_write(__VA_ARGS__); } while (0)
#define LOGV(...) \
    do { if (qoh99_wlog_should_verbose()) qoh99_wlog_write_verbose(__VA_ARGS__); } while (0)

#define QOH99_GAME_MOVIE_PROBE_ADDR    0x0046F9E0u
#define QOH99_GAME_MOVIE_FILENAME_ADDR 0x004AB794u

#define WM_QOH99_MOVIE_EVENT (WM_APP + 0x53)

typedef struct MovieOpenParms {
    DWORD_PTR dwCallback;
    DWORD     wDeviceID;
    LPCSTR    lpstrDeviceType;
    LPCSTR    lpstrElementName;
    LPCSTR    lpstrAlias;
    DWORD     dwStyle;
    HWND      hWndParent;
} MovieOpenParms;

typedef struct MovieWindowParms {
    DWORD_PTR dwCallback;
    HWND      hWnd;
    UINT      nCmdShow;
    LPCSTR    lpstrText;
} MovieWindowParms;

typedef struct MovieStatusParms {
    DWORD_PTR dwCallback;
    DWORD_PTR dwReturn;
    DWORD_PTR dwItem;
    DWORD     dwTrack;
} MovieStatusParms;

typedef struct MoviePlayParms {
    DWORD_PTR dwCallback;
    DWORD_PTR dwFrom;
    DWORD_PTR dwTo;
} MoviePlayParms;

typedef struct MoviePutParms {
    DWORD_PTR dwCallback;
    LONG      x;
    LONG      y;
    LONG      cx;
    LONG      cy;
} MoviePutParms;

typedef MCIERROR (WINAPI *MciSendCommandFn)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
typedef MCIERROR (WINAPI *MciSendStringFn)(LPCSTR, LPSTR, UINT, HWND);
typedef BOOL     (WINAPI *MciGetErrorStringFn)(MCIERROR, LPSTR, UINT);

static MciSendCommandFn    s_realMciSendCommand    = NULL;
static MciSendStringFn     s_realMciSendString     = NULL;
static MciGetErrorStringFn s_realMciGetErrorString = NULL;

static Qoh99MovieDecision s_decision = { QOH99_MOVIE_LEAVE_ALONE,
                                         QOH99_MOVIE_SKIP_NONE };
static LONG s_installed = 0;

static IGraphBuilder  *s_graph   = NULL;
static IMediaControl  *s_control = NULL;
static IMediaEventEx  *s_events  = NULL;
static IVideoWindow   *s_video   = NULL;
static HWND  s_eventWindow  = NULL;
static HWND  s_videoWindow  = NULL;
static HWND  s_notifyWindow = NULL;
static HWND  s_parentWindow = NULL;
static int   s_deviceOpen   = 0;
static int   s_playing      = 0;
static int   s_comInitialized = 0;
static int   s_completionPosted = 0;

static LONG s_rectX = 0;
static LONG s_rectY = 0;
static LONG s_rectCx = 0;
static LONG s_rectCy = 0;
static int  s_haveRect = 0;
static LONG s_gameWidth = 640;
static LONG s_gameHeight = 480;

static char s_moviePath[MAX_PATH] = { 0 };
static int  s_graphHasVideo = 0;

static int fileExists(const char *path);

static int losslessSiblingPath(const char *original, char *out, size_t cap)
{
    const char *name = qmovie::kOpeningMovieLosslessName;
    size_t len;
    size_t dir;

    if (original == NULL || out == NULL || cap == 0)
        return 0;
    len = strlen(original);
    dir = len;
    while (dir > 0 && original[dir - 1] != '\\' && original[dir - 1] != '/')
        --dir;
    if (dir + strlen(name) + 1 > cap)
        return 0;
    memcpy(out, original, dir);
    strcpy(out + dir, name);
    return fileExists(out);
}

typedef BOOL (VFWAPI *IcInstallFn)(DWORD, DWORD, LPARAM, LPSTR, UINT);
typedef BOOL (VFWAPI *IcRemoveFn)(DWORD, DWORD, UINT);
typedef HIC  (VFWAPI *IcOpenFn)(DWORD, DWORD, UINT);
typedef LRESULT (VFWAPI *IcCloseFn)(HIC);
static int  s_indeoAttempted = 0;
static int  s_indeoRegistered = 0;
static char s_indeoDriverPath[MAX_PATH] = { 0 };

static int wrapperDirectory(char *out, size_t cap)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD n;
    if (VirtualQuery((const void *)&wrapperDirectory, &mbi, sizeof(mbi)) == 0)
        return 0;
    n = GetModuleFileNameA((HMODULE)mbi.AllocationBase, out, (DWORD)cap);
    if (n == 0 || n >= cap)
        return 0;
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/')
        --n;
    out[n] = '\0';
    return 1;
}

static int registerIndeoDriver(void)
{
    const DWORD fcc = mmioFOURCC('I', 'V', '5', '0');
    char candidate[3][MAX_PATH];
    char dir[MAX_PATH];
    HMODULE msvfw;
    IcInstallFn install;
    IcRemoveFn remove;
    IcOpenFn open;
    IcCloseFn close;
    int i;

    if (s_indeoAttempted)
        return s_indeoRegistered;
    s_indeoAttempted = 1;

    candidate[0][0] = '\0';
    candidate[1][0] = '\0';
    candidate[2][0] = '\0';
    if (wrapperDirectory(dir, sizeof(dir))) {
        _snprintf(candidate[0], MAX_PATH - 1, "%sir50_32.dll", dir);
        _snprintf(candidate[1], MAX_PATH - 1, "%sir50_32original.dll", dir);
    }
    if (GetSystemDirectoryA(dir, (UINT)sizeof(dir)) != 0)
        _snprintf(candidate[2], MAX_PATH - 1, "%s\\ir50_32.dll", dir);

    msvfw = LoadLibraryA("msvfw32.dll");
    install = msvfw != NULL ? (IcInstallFn)GetProcAddress(msvfw, "ICInstall") : NULL;
    remove  = msvfw != NULL ? (IcRemoveFn)GetProcAddress(msvfw, "ICRemove") : NULL;
    open    = msvfw != NULL ? (IcOpenFn)GetProcAddress(msvfw, "ICOpen") : NULL;
    close   = msvfw != NULL ? (IcCloseFn)GetProcAddress(msvfw, "ICClose") : NULL;
    if (install == NULL || remove == NULL || open == NULL || close == NULL) {
        LOG("[movie] msvfw32 ICInstall/ICOpen unavailable; no Indeo driver");
        return 0;
    }
    for (i = 0; i < 3; ++i) {
        HIC hic;
        char shortPath[MAX_PATH];
        if (candidate[i][0] == '\0' || !fileExists(candidate[i]))
            continue;
        if (GetShortPathNameA(candidate[i], shortPath, (DWORD)sizeof(shortPath)) == 0
            || shortPath[0] == '\0')
            lstrcpynA(shortPath, candidate[i], (int)sizeof(shortPath));
        if (!install(ICTYPE_VIDEO, fcc, (LPARAM)shortPath, NULL,
                     ICINSTALL_DRIVER)) {
            LOG("[movie] ICInstall refused %s", candidate[i]);
            continue;
        }
        hic = open(ICTYPE_VIDEO, fcc, ICMODE_DECOMPRESS);
        if (hic == NULL) {
            LOG("[movie] %s registered but would not open; trying the next",
                candidate[i]);
            remove(ICTYPE_VIDEO, fcc, 0);
            continue;
        }
        close(hic);
        lstrcpynA(s_indeoDriverPath, candidate[i], (int)sizeof(s_indeoDriverPath));
        s_indeoRegistered = 1;
        LOG("[movie] Indeo 5 driver registered for this process from %s",
            s_indeoDriverPath);
        return 1;
    }
    LOG("[movie] no usable Indeo 5 driver on this machine (looked beside the "
        "wrapper and in the system directory)");
    return 0;
}

static void releaseInterface(IUnknown **ptr)
{
    if (ptr != NULL && *ptr != NULL) {
        (*ptr)->Release();
        *ptr = NULL;
    }
}

static void postCompletion(void)
{
    if (s_completionPosted || s_notifyWindow == NULL)
        return;
    s_completionPosted = 1;
    LOG("[movie] completion -> MM_MCINOTIFY(SUCCESSFUL) hwnd=%p device=0x%X",
        (void *)s_notifyWindow, QOH99_MOVIE_MCI_DEVICE_ID);
    PostMessageA(s_notifyWindow, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL,
                 (LPARAM)QOH99_MOVIE_MCI_DEVICE_ID);
}

static void drainGraphEvents(void)
{
    long code = 0;
    LONG_PTR p1 = 0;
    LONG_PTR p2 = 0;

    if (s_events == NULL)
        return;
    while (SUCCEEDED(s_events->GetEvent(&code, &p1, &p2, 0))) {
        s_events->FreeEventParams(code, p1, p2);
        switch (code) {
            case EC_COMPLETE:
                LOG("[movie] EC_COMPLETE");
                postCompletion();
                break;
            case EC_ERRORABORT:
            case EC_USERABORT:
                LOG("[movie] graph aborted (code=0x%lX); reporting completion",
                    code);
                postCompletion();
                break;
            default:
                break;
        }
    }
}

static LRESULT CALLBACK MovieEventProc(HWND hWnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam)
{
    if (msg == WM_QOH99_MOVIE_EVENT) {
        drainGraphEvents();
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static HWND createEventWindow(void)
{
    static const char kClassName[] = "Qoh99MovieEvents";
    static LONG s_registered = 0;
    WNDCLASSA wc;

    if (InterlockedExchange(&s_registered, 1) == 0) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = MovieEventProc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = kClassName;
        if (RegisterClassA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            WLOG_ISSUE("[movie] RegisterClass failed err=%lu", GetLastError());
            return NULL;
        }
    }
    return CreateWindowExA(0, kClassName, "", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, NULL, GetModuleHandleA(NULL), NULL);
}

static HWND createSurfaceWindow(HWND parent)
{
    static const char kClassName[] = "Qoh99MovieSurface";
    static LONG s_registered = 0;
    WNDCLASSA wc;

    if (parent == NULL)
        return NULL;
    if (InterlockedExchange(&s_registered, 1) == 0) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = kClassName;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = NULL;
        if (RegisterClassA(&wc) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            WLOG_ISSUE("[movie] surface RegisterClass failed err=%lu", GetLastError());
            return NULL;
        }
    }
    return CreateWindowExA(0, kClassName, "",
                           WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                           0, 0, 1, 1, parent, NULL,
                           GetModuleHandleA(NULL), NULL);
}

static void ensureCom(void)
{
    HRESULT hr;
    if (s_comInitialized)
        return;
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE)
        s_comInitialized = 1;
    else if (hr == RPC_E_CHANGED_MODE)
        s_comInitialized = 0;
    else
        LOG("[movie] CoInitializeEx hr=0x%08lX", hr);
}

static void teardownGraph(void)
{
    if (s_control != NULL)
        s_control->Stop();
    if (s_events != NULL)
        s_events->SetNotifyWindow(0, 0, 0);
    if (s_video != NULL) {
        s_video->put_Visible(OAFALSE);
        /* Detach before release: a renderer window left owned by a window that
           is about to be destroyed is how these teardowns crash. */
        s_video->put_Owner(0);
        s_video->put_MessageDrain(0);
    }
    releaseInterface((IUnknown **)&s_video);
    releaseInterface((IUnknown **)&s_events);
    releaseInterface((IUnknown **)&s_control);
    releaseInterface((IUnknown **)&s_graph);
    if (s_eventWindow != NULL) {
        DestroyWindow(s_eventWindow);
        s_eventWindow = NULL;
    }
    /* After the renderer has been detached and released, never before: the
       surface is its owner window. */
    if (s_videoWindow != NULL) {
        DestroyWindow(s_videoWindow);
        s_videoWindow = NULL;
    }
    s_haveRect = 0;
    s_playing = 0;
    s_deviceOpen = 0;
}

static int buildGraph(const char *path, HWND parent)
{
    wchar_t wide[MAX_PATH * 2];
    HRESULT hr;

    ensureCom();

    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) == 0) {
        WLOG_ISSUE("[movie] cannot widen path err=%lu", GetLastError());
        return 0;
    }

    hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, (void **)&s_graph);
    if (FAILED(hr) || s_graph == NULL) {
        LOG("[movie] CoCreateInstance(FilterGraph) hr=0x%08lX", hr);
        return 0;
    }

    /* A DirectShow video renderer can call DirectDrawCreate and DirectDrawEnumerate straight back into this wrapper. */
    {
        IBaseFilter *renderer = NULL;
        hr = CoCreateInstance(CLSID_VideoMixingRenderer9, NULL,
                              CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                              (void **)&renderer);
        if (SUCCEEDED(hr) && renderer != NULL) {
            hr = s_graph->AddFilter(renderer, L"QOH99 VMR9");
            if (FAILED(hr))
                LOG("[movie] AddFilter(VMR9) hr=0x%08lX", hr);
            renderer->Release();
        } else {
            LOG("[movie] VMR9 unavailable hr=0x%08lX; the graph builder may "
                "pick the DirectDraw renderer, which re-enters this wrapper",
                hr);
        }
    }

    s_graphHasVideo = 0;
    hr = s_graph->RenderFile(wide, NULL);
    if (FAILED(hr)) {
        LOG("[movie] RenderFile hr=0x%08lX for %s", hr, path);
        releaseInterface((IUnknown **)&s_graph);
        return 0;
    }
    if (hr != S_OK) {
        LOG("[movie] RenderFile partial hr=0x%08lX for %s", hr, path);
    }

    s_graph->QueryInterface(IID_IMediaControl, (void **)&s_control);
    s_graph->QueryInterface(IID_IMediaEventEx, (void **)&s_events);
    s_graph->QueryInterface(IID_IVideoWindow, (void **)&s_video);
    if (s_control == NULL) {
        LOG("[movie] graph has no IMediaControl");
        teardownGraph();
        return 0;
    }

    s_eventWindow = createEventWindow();
    if (s_events != NULL && s_eventWindow != NULL) {
        s_events->SetNotifyWindow((OAHWND)s_eventWindow, WM_QOH99_MOVIE_EVENT, 0);
    }

    if (s_video != NULL) {
        long width = 0;
        long height = 0;
        s_video->put_AutoShow(OAFALSE);
        s_video->put_Visible(OAFALSE);
        if (parent != NULL) {
            s_videoWindow = createSurfaceWindow(parent);
            if (s_videoWindow != NULL) {
                s_video->put_Owner((OAHWND)s_videoWindow);
                s_video->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS
                                         | WS_VISIBLE);
                s_video->put_MessageDrain((OAHWND)parent);
            }
        }
        {
            IBasicVideo *basic = NULL;
            if (SUCCEEDED(s_graph->QueryInterface(IID_IBasicVideo,
                                                 (void **)&basic))
                && basic != NULL) {
                basic->GetVideoSize(&width, &height);
                basic->Release();
            }
        }
        s_graphHasVideo = (width > 0 && height > 0) ? 1 : 0;
        LOG("[movie] graph built: %ldx%ld surface=%p parent=%p video=%d",
            width, height, (void *)s_videoWindow, (void *)parent,
            s_graphHasVideo);
    } else {
        LOG("[movie] graph built with no IVideoWindow (audio only?)");
    }
    return 1;
}

static MCIERROR handleOpen(DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    MovieOpenParms *parms = (MovieOpenParms *)dwParam2;
    HWND parent;

    if (parms == NULL)
        return MCIERR_MISSING_PARAMETER;

    parent = ((dwParam1 & 0x00020000u) != 0u) ? parms->hWndParent : NULL;
    s_parentWindow = parent;
    lstrcpynA(s_moviePath, parms->lpstrElementName, (int)sizeof(s_moviePath));

    if (s_deviceOpen)
        teardownGraph();
    s_completionPosted = 0;

    {
        char lossless[MAX_PATH];
        int built = 0;
        if (registerIndeoDriver()) {
            if (buildGraph(s_moviePath, parent) && s_graphHasVideo) {
                LOG("[movie] playing %s through the Indeo 5 driver %s",
                    s_moviePath, s_indeoDriverPath);
                built = 1;
            } else {
                LOG("[movie] Indeo 5 driver registered but %s did not render "
                    "video", s_moviePath);
                teardownGraph();
                s_completionPosted = 0;
            }
        }
        if (!built && losslessSiblingPath(s_moviePath, lossless, sizeof(lossless))) {
            if (!qmovie::registerOpeningMovieCodec()) {
                LOG("[movie] %s found but the Q99V decoder could not be "
                    "registered (msvfw32 ICInstall); using the game's file",
                    lossless);
            } else if (buildGraph(lossless, parent) && s_graphHasVideo) {
                LOG("[movie] playing the lossless copy %s through the "
                    "in-process Q99V decoder", lossless);
                lstrcpynA(s_moviePath, lossless, (int)sizeof(s_moviePath));
                built = 1;
            } else {
                LOG("[movie] %s did not render video; using the game's file",
                    lossless);
                teardownGraph();
                s_completionPosted = 0;
            }
        }
        if (built) {
            s_deviceOpen = 1;
            parms->wDeviceID = QOH99_MOVIE_MCI_DEVICE_ID;
            LOG("[movie] MCI_OPEN served: %s (device=0x%X parent=%p)",
                s_moviePath, QOH99_MOVIE_MCI_DEVICE_ID, (void *)parent);
            return 0;
        }
    }

    if (!buildGraph(s_moviePath, parent)) {
        LOG("[movie] open accepted with no graph; will report immediate "
            "completion");
    }
    s_deviceOpen = 1;
    parms->wDeviceID = QOH99_MOVIE_MCI_DEVICE_ID;
    LOG("[movie] MCI_OPEN served: %s (device=0x%X parent=%p)",
        s_moviePath, QOH99_MOVIE_MCI_DEVICE_ID, (void *)parent);
    return 0;
}

static MCIERROR handleWindow(DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    MovieWindowParms *parms = (MovieWindowParms *)dwParam2;
    if ((dwParam1 & 0x00040000u) != 0u && parms != NULL) {
        const int show = parms->nCmdShow != SW_HIDE;
        if (s_videoWindow != NULL)
            ShowWindow(s_videoWindow, show ? SW_SHOWNA : SW_HIDE);
        if (s_video != NULL)
            s_video->put_Visible(show ? OATRUE : OAFALSE);
        LOGV("[movie] MCI_WINDOW nCmdShow=%u -> visible=%d", parms->nCmdShow,
             show);
    }
    return 0;
}

static MCIERROR handleStatus(DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    MovieStatusParms *parms = (MovieStatusParms *)dwParam2;

    if (parms == NULL)
        return MCIERR_MISSING_PARAMETER;
    if ((dwParam1 & QOH99_MCI_FLAG_STATUS_ITEM) == 0u)
        return 0;

    switch ((unsigned int)parms->dwItem) {
        case QOH99_MCI_DGV_STATUS_HWND:
            parms->dwReturn = (DWORD_PTR)s_videoWindow;
            break;
        case MCI_STATUS_MODE:
            parms->dwReturn = s_playing ? MCI_MODE_PLAY : MCI_MODE_STOP;
            break;
        default:
            parms->dwReturn = 0;
            break;
    }
    return 0;
}

static MCIERROR handlePlay(DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    MoviePlayParms *parms = (MoviePlayParms *)dwParam2;
    HRESULT hr;

    if (parms != NULL && (dwParam1 & QOH99_MCI_FLAG_NOTIFY) != 0u)
        s_notifyWindow = (HWND)parms->dwCallback;
    if (s_notifyWindow == NULL)
        s_notifyWindow = s_parentWindow;

    if (s_control == NULL) {
        LOG("[movie] MCI_PLAY with no graph; reporting completion now");
        postCompletion();
        return 0;
    }

    if (s_videoWindow != NULL)
        ShowWindow(s_videoWindow, SW_SHOWNA);
    if (s_video != NULL)
        s_video->put_Visible(OATRUE);
    hr = s_control->Run();
    if (FAILED(hr)) {
        LOG("[movie] Run hr=0x%08lX; reporting completion", hr);
        postCompletion();
        return 0;
    }
    s_playing = 1;
    LOG("[movie] MCI_PLAY running (notify=%p)", (void *)s_notifyWindow);
    return 0;
}

static void applyMovieGeometry(void)
{
    RECT client;
    LONG gameW;
    LONG gameH;
    LONG x;
    LONG y;
    LONG cx;
    LONG cy;

    if (s_videoWindow == NULL || !s_haveRect)
        return;
    if (s_parentWindow == NULL || !GetClientRect(s_parentWindow, &client))
        return;
    if (client.right <= 0 || client.bottom <= 0)
        return;

    gameW = s_gameWidth > 0 ? s_gameWidth : 640;
    gameH = s_gameHeight > 0 ? s_gameHeight : 480;

    {
        int boxX = 0;
        int boxY = 0;
        int boxW = 0;
        int boxH = 0;
        ddw_letterbox((int)client.right, (int)client.bottom, (int)gameW,
                      (int)gameH, &boxX, &boxY, &boxW, &boxH);
        if (boxW <= 0 || boxH <= 0)
            return;
        x  = boxX + MulDiv(s_rectX,  boxW, gameW);
        y  = boxY + MulDiv(s_rectY,  boxH, gameH);
        cx = MulDiv(s_rectCx, boxW, gameW);
        cy = MulDiv(s_rectCy, boxH, gameH);
        if (cx <= 0 || cy <= 0)
            return;

        SetWindowPos(s_videoWindow, NULL, x, y, cx, cy,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        if (s_video != NULL)
            s_video->SetWindowPosition(0, 0, cx, cy);
        LOGV("[movie] geometry: game %ld,%ld %ldx%ld (of %ldx%ld) -> box "
             "%d,%d %dx%d of client %ldx%ld -> %ld,%ld %ldx%ld",
             s_rectX, s_rectY, s_rectCx, s_rectCy, gameW, gameH,
             boxX, boxY, boxW, boxH, client.right, client.bottom,
             x, y, cx, cy);
    }
}

static MCIERROR handlePut(DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    MoviePutParms *parms = (MoviePutParms *)dwParam2;

    (void)dwParam1;
    if (parms == NULL)
        return 0;
    if (parms->cx <= 0 || parms->cy <= 0)
        return 0;
    s_rectX = parms->x;
    s_rectY = parms->y;
    s_rectCx = parms->cx;
    s_rectCy = parms->cy;
    s_haveRect = 1;
    applyMovieGeometry();
    return 0;
}

static MCIERROR handleStop(void)
{
    if (s_control != NULL)
        s_control->Stop();
    if (s_video != NULL)
        s_video->put_Visible(OAFALSE);
    if (s_videoWindow != NULL)
        ShowWindow(s_videoWindow, SW_HIDE);
    s_playing = 0;
    LOG("[movie] MCI_STOP");
    return 0;
}

static MCIERROR handleClose(void)
{
    const qmovie::OpeningMovieCodecStats stats = qmovie::openingMovieCodecStats();
    LOG("[movie] MCI_CLOSE");
    if (stats.instancesOpened != 0u) {
        LOG("[movie] Q99V decoder: instances=%u frames=%u rejected=%u "
            "outOfSequence=%u",
            stats.instancesOpened, stats.framesDecoded, stats.framesRejected,
            stats.framesOutOfSequence);
    }
    teardownGraph();
    return 0;
}

static void *resolveWinmm(const char *name)
{
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    if (winmm == NULL)
        winmm = LoadLibraryA("winmm.dll");
    if (winmm == NULL) {
        LOG("[movie] winmm is not loadable; cannot forward %s", name);
        return NULL;
    }
    return (void *)GetProcAddress(winmm, name);
}

static MCIERROR forwardCommand(MCIDEVICEID deviceId, UINT msg,
                               DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    if (s_realMciSendCommand == NULL) {
        s_realMciSendCommand =
            (MciSendCommandFn)resolveWinmm("mciSendCommandA");
    }
    if (s_realMciSendCommand == NULL)
        return MCIERR_INVALID_DEVICE_ID;
    return s_realMciSendCommand(deviceId, msg, dwParam1, dwParam2);
}

static MCIERROR WINAPI Hook_mciSendCommandA(MCIDEVICEID deviceId, UINT msg,
                                            DWORD_PTR dwParam1,
                                            DWORD_PTR dwParam2)
{
    if (msg == QOH99_MCI_OPEN) {
        const MovieOpenParms *parms = (const MovieOpenParms *)dwParam2;
        const char *element = parms != NULL ? parms->lpstrElementName : NULL;
        if (qoh99_movie_mci_open_is_ours(msg, (unsigned int)dwParam1, element))
            return handleOpen(dwParam1, dwParam2);
        return forwardCommand(deviceId, msg, dwParam1, dwParam2);
    }

    if (!qoh99_movie_mci_is_our_device((unsigned int)deviceId))
        return forwardCommand(deviceId, msg, dwParam1, dwParam2);

    switch (msg) {
        case QOH99_MCI_WINDOW: return handleWindow(dwParam1, dwParam2);
        case QOH99_MCI_STATUS: return handleStatus(dwParam1, dwParam2);
        case QOH99_MCI_PLAY:   return handlePlay(dwParam1, dwParam2);
        case QOH99_MCI_PUT:    return handlePut(dwParam1, dwParam2);
        case QOH99_MCI_STOP:   return handleStop();
        case QOH99_MCI_CLOSE:  return handleClose();
        default:
            LOGV("[movie] MCI msg 0x%X accepted without action", msg);
            return 0;
    }
}

static MCIERROR WINAPI Hook_mciSendStringA(LPCSTR command, LPSTR ret,
                                           UINT retLen, HWND callback)
{
    if (s_realMciSendString == NULL)
        s_realMciSendString = (MciSendStringFn)resolveWinmm("mciSendStringA");
    if (s_realMciSendString == NULL)
        return MCIERR_INVALID_DEVICE_ID;
    return s_realMciSendString(command, ret, retLen, callback);
}

static BOOL WINAPI Hook_mciGetErrorStringA(MCIERROR err, LPSTR text, UINT len)
{
    if (err == 0 && text != NULL && len > 0) {
        text[0] = '\0';
        return FALSE;
    }
    return s_realMciGetErrorString(err, text, len);
}

static int fileExists(const char *path)
{
    const DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int findMovie(char *out, size_t cap)
{
    static const char kRelative[] = ".\\System\\Opening.avi";
    if (cap == 0)
        return 0;
    lstrcpynA(out, kRelative, (int)cap);
    return fileExists(out);
}

static int casterRequestedSkip(void)
{
    char value[16];
    const DWORD n = GetEnvironmentVariableA("QOH99_SKIP_INTRO", value,
                                            (DWORD)sizeof(value));
    if (n == 0 || n >= sizeof(value))
        return 0;
    return (value[0] != '0' && value[0] != '\0') ? 1 : 0;
}

static int applySkipPatch(void)
{
    unsigned char patch[QOH99_MOVIE_SKIP_PATCH_BYTES];
    unsigned char actual[QOH99_MOVIE_PROBE_PROLOGUE_BYTES];
    void *site = (void *)QOH99_GAME_MOVIE_PROBE_ADDR;
    DWORD oldProtect = 0;
    SIZE_T got = 0;

    if (qoh99_movie_skip_patch_encode(patch, sizeof(patch),
                                      QOH99_GAME_MOVIE_FILENAME_ADDR) == 0) {
        LOG("[movie] skip patch could not be encoded");
        return 0;
    }
    if (!ReadProcessMemory(GetCurrentProcess(), site, actual, sizeof(actual),
                           &got)
        || got != sizeof(actual)) {
        LOG("[movie] skip patch site 0x%08X unreadable err=%lu",
            QOH99_GAME_MOVIE_PROBE_ADDR, GetLastError());
        return 0;
    }
    if (!qoh99_movie_skip_patch_site_matches(actual, sizeof(actual))) {
        LOG("[movie] skip patch REFUSED: 0x%08X starts with "
            "%02X %02X %02X %02X %02X %02X, not the expected probe prologue. "
            "Different build or another tool is already there; leaving the "
            "intro alone.",
            QOH99_GAME_MOVIE_PROBE_ADDR, actual[0], actual[1], actual[2],
            actual[3], actual[4], actual[5]);
        return 0;
    }
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE,
                        &oldProtect)) {
        WLOG_ISSUE("[movie] VirtualProtect on 0x%08X failed err=%lu",
            QOH99_GAME_MOVIE_PROBE_ADDR, GetLastError());
        return 0;
    }
    memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(patch), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    LOG("[movie] intro skipped: probe at 0x%08X now clears 0x%08X and reports "
        "no movie",
        QOH99_GAME_MOVIE_PROBE_ADDR, QOH99_GAME_MOVIE_FILENAME_ADDR);
    return 1;
}

static void installMciHooks(void)
{
    char err[256];
    HMODULE game = GetModuleHandleA(NULL);

    err[0] = '\0';
    if (qoh99_iat_patch(game, "WINMM.dll", "mciSendCommandA",
                        (void *)Hook_mciSendCommandA,
                        (void **)&s_realMciSendCommand, err, sizeof(err)) <= 0
        || s_realMciSendCommand == NULL) {
        LOG("[movie] could not patch mciSendCommandA (%s); the intro is left to "
            "the game", err);
        s_realMciSendCommand = NULL;
        return;
    }

    err[0] = '\0';
    if (qoh99_iat_patch(game, "WINMM.dll", "mciSendStringA",
                        (void *)Hook_mciSendStringA,
                        (void **)&s_realMciSendString, err, sizeof(err)) <= 0
        || s_realMciSendString == NULL) {
        LOGV("[movie] mciSendStringA not hooked (%s)", err);
        s_realMciSendString = NULL;
    }
    err[0] = '\0';
    if (qoh99_iat_patch(game, "WINMM.dll", "mciGetErrorStringA",
                        (void *)Hook_mciGetErrorStringA,
                        (void **)&s_realMciGetErrorString, err, sizeof(err)) <= 0
        || s_realMciGetErrorString == NULL) {
        LOGV("[movie] mciGetErrorStringA not hooked (%s)", err);
        s_realMciGetErrorString = NULL;
    }
    LOG("[movie] serving the opening movie through DirectShow");
}

void qoh99_movie_install(void)
{
    char path[MAX_PATH];
    int present;
    int requested;

    if (InterlockedExchange(&s_installed, 1) != 0)
        return;

    requested = casterRequestedSkip();
    present = findMovie(path, sizeof(path));

    s_decision = qoh99_movie_policy_decide(requested, present);
    LOG("[movie] policy: %s%s%s (file=%s)",
        qoh99_movie_policy_text(s_decision.policy),
        s_decision.skipReason != QOH99_MOVIE_SKIP_NONE ? " - " : "",
        qoh99_movie_skip_reason_text(s_decision.skipReason),
        present ? path : "absent");

    switch (s_decision.policy) {
        case QOH99_MOVIE_SKIP:
            if (!applySkipPatch())
                installMciHooks();
            break;
        case QOH99_MOVIE_PLAY:
            installMciHooks();
            break;
        case QOH99_MOVIE_LEAVE_ALONE:
        default:
            break;
    }
}

void qoh99_movie_set_game_resolution(int width, int height)
{
    if (width > 0 && height > 0) {
        s_gameWidth = (LONG)width;
        s_gameHeight = (LONG)height;
    }
}

void qoh99_movie_window_resized(void)
{
    if (s_videoWindow != NULL && s_haveRect)
        applyMovieGeometry();
}

void qoh99_movie_shutdown(void)
{
    if (s_deviceOpen || s_graph != NULL)
        teardownGraph();
}

Qoh99MovieDecision qoh99_movie_decision(void)
{
    return s_decision;
}

int qoh99_movie_is_playing(void)
{
    return s_playing;
}
