#include "render_dx9_imgui.h"
#include "i18n/Tr.h"

#include "ddraw_log.h"
#include "input_display_layout.h"
#include "replay_hud.h"
#include "training/TrainingActionCatalog.h"

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include <d3d9.h>

#define DXLOG(...) \
    do { if (qoh99_wlog_should()) qoh99_wlog_write(__VA_ARGS__); } while (0)
#define DXLOGV(...) \
    do { if (qoh99_wlog_should_verbose()) qoh99_wlog_write_verbose(__VA_ARGS__); } while (0)

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

extern "C" void Qoh99WrapperRefreshIniComments(void);

namespace
{
constexpr DWORD kFrameQuadFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

struct FrameQuadVertex
{
    float x;
    float y;
    float z;
    float rhw;
    float u;
    float v;
};

struct PresenterState
{
    IDirect3D9 *d3d = nullptr;
    IDirect3D9Ex *d3dEx = nullptr;
    IDirect3DDevice9 *device = nullptr;
    IDirect3DDevice9Ex *deviceEx = nullptr;
    IDirect3DTexture9 *frameTexture = nullptr;
    IDirect3DTexture9 *scaleTexture = nullptr;
    int scaleTextureW = 0;
    int scaleTextureH = 0;
    D3DPRESENT_PARAMETERS presentParams = {};
    HWND window = nullptr;
    QohPresentationMode requestedMode = QOH_PRESENTATION_WINDOWED;
    QohPresentationMode effectiveMode = QOH_PRESENTATION_WINDOWED;
    QohRendererBackend requestedBackend = QOH_RENDERER_AUTO;
    QohRendererBackend effectiveBackend = QOH_RENDERER_CLASSIC_D3D9;
    QohRendererReadiness readiness = QOH_RENDERER_NOT_REQUESTED;
    UINT adapterOrdinal = D3DADAPTER_DEFAULT;
    HMONITOR monitor = nullptr;
    D3DDISPLAYMODE desktopMode = {};
    DWORD resetGeneration = 0;
    char adapterDescription[128] = {};
    char fallbackReason[160] = {};
    bool imguiInitialized = false;
    bool menuVisible = false;
    bool showDemoWindow = false;
    bool deviceLost = false;
    bool exDisabledAfterHardFailure = false;
    bool appActive = true;
    int textureWidth = 0;
    int textureHeight = 0;
    EfzDx9NetplayHudProvider hudProvider = nullptr;
    void *hudProviderUserData = nullptr;
    EfzDx9OverlayProvider overlayProvider = nullptr;
    void *overlayProviderUserData = nullptr;
    DWORD trainingUiApiVersion = 0;
    DWORD trainingUiRejectedApiVersion = 0;
    EfzDx9TrainingStatusProviderV1 trainingStatusProvider = nullptr;
    EfzDx9TrainingCommandEnqueueV1 trainingCommandEnqueue = nullptr;
    EfzDx9TrainingMenuCaptureV1 trainingMenuCapture = nullptr;
    void *trainingUiUserData = nullptr;
    EfzDx9TrainingMenuRendererFn trainingMenuRenderer = nullptr;
    EfzDx9TrainingMenuVisibleFn trainingMenuVisible = nullptr;
    EfzDx9TrainingMenuHandleMessageFn trainingMenuHandleMessage = nullptr;
    EfzDx9TrainingMenuWantsDrawFn trainingMenuWantsDraw = nullptr;
    EfzDx9TrainingMenuDeviceNotifyFn trainingMenuDeviceNotify = nullptr;
    void *trainingMenuUserData = nullptr;
    DWORD inputDisplayApiVersion = 0;
    DWORD inputDisplayRejectedApiVersion = 0;
    EfzDx9InputDisplayProviderV1 inputDisplayProvider = nullptr;
    void *inputDisplayUserData = nullptr;
    DWORD replayTransportApiVersion = 0;
    DWORD replayTransportRejectedApiVersion = 0;
    EfzDx9ReplayTransportStatusV2Fn replayTransportStatus = nullptr;
    EfzDx9ReplayTransportSubmitV2Fn replayTransportSubmit = nullptr;
    void *replayTransportUserData = nullptr;
    EfzDx9SpectatorHotkeySinkFn spectatorHotkeySink = nullptr;
    void *spectatorHotkeyUserData = nullptr;
    ImGuiContext *imguiContext = nullptr;
};

PresenterState g_presenter = {};

struct TrainingUiLocalState
{
    DWORD nextCommandId = 1;
    DWORD generation = 0;
    DWORD lifecycleState = QOH99_TRAINING_LIFECYCLE_OFF;
    DWORD scene = 0;
    DWORD matchSubstate = 0;
    bool statusObserved = false;
    bool refreshDraftsOnNextDraw = true;
    DWORD capturedGeneration = 0;
    int capturedState = -1;
    DWORD lastRejectedCommandId = 0;
    DWORD lastRejectedTick = 0;
    int refillTarget = QOH99_TRAINING_REFILL_BOTH;
    int resourceTarget = QOH99_TRAINING_REFILL_BOTH;
    int lifeSetValue = 0;
    int exStocksValue = static_cast<int>(QOH99_TRAINING_EX_STOCKS_FULL);
    int exFractionValue = 0;
    int guardGaugeValue = static_cast<int>(QOH99_TRAINING_GUARD_GAUGE_FULL);
    bool resourceDraftsSeeded = false;
    int recoveryDelaySteps = 0;
    int recoveryRandomMin = 0;
    int recoveryRandomMax = 0;
    bool recoveryDraftsSeeded = false;
    DWORD requestedRecoveryMode[QOH99_TRAINING_RECOVERY_SLOT_COUNT] = {};
    DWORD recoveryRequestTick = 0;
    bool recoveryRequestPending = false;
    bool throwTechEnabled = false;
    int triggerMacroSlot[QOH99_TRAINING_TRIGGER_EVENT_COUNT] = {};
    int triggerMoveIndex[QOH99_TRAINING_TRIGGER_EVENT_COUNT] = {};
    int triggerDelaySteps[QOH99_TRAINING_TRIGGER_EVENT_COUNT] = {};
    int triggerChancePercent[QOH99_TRAINING_TRIGGER_EVENT_COUNT] = {
        100, 100, 100, 100, 100, 100, 100, 100 };
    DWORD combatToolsDraftCharacter = 0xFFFFFFFFu;
    DWORD combatToolsDraftGeneration = 0xFFFFFFFFu;
    bool combatToolsDraftsSeeded = false;
};
static_assert(QOH99_TRAINING_TRIGGER_EVENT_COUNT == 8u,
              "the chance mirror's initializer lists one entry per event");
static_assert(static_cast<uint32_t>(QOH99_TRAINING_PLAY_RANDOM)
                  == static_cast<uint32_t>(qtraining::PlaybackMode::Random)
              && static_cast<uint32_t>(QOH99_TRAINING_PLAY_RANDOM_LOOP)
                  == static_cast<uint32_t>(
                      qtraining::PlaybackMode::RandomLoop),
              "playback modes must mirror qtraining::PlaybackMode");
static_assert(QOH99_TRAINING_TRIGGER_RANDOM_RECORDING
                  == qtraining::kTrainingTriggerRandomMacroSlot,
              "random recording must mirror kTrainingTriggerRandomMacroSlot");

TrainingUiLocalState g_trainingUi = {};

bool CreateDevice(HWND window, int clientWidth, int clientHeight,
                  QohPresentationMode mode, QohRendererBackend backend);
bool ResetDevice(int clientWidth, int clientHeight,
                 QohPresentationMode mode, QohRendererBackend backend);
void NotifyDeviceObjectsInvalid();
void NotifyDeviceObjectsValid();

enum : LONG {
    kDeviceIdle = 0,
    kDevicePending = 1,
    kDeviceReady = 2,
    kDeviceFailed = 3,
    kDeviceInFrame = 4,  // game thread owns g_presenter - worker must keep out
    kDeviceWorking = 5,
    kDeviceFallbackReady = 6,
};

volatile LONG g_deviceState = kDeviceIdle;
HWND g_deviceRequestWindow = nullptr;
int g_deviceRequestWidth = 0;
int g_deviceRequestHeight = 0;
QohPresentationMode g_deviceRequestMode = QOH_PRESENTATION_WINDOWED;
QohRendererBackend g_deviceRequestBackend = QOH_RENDERER_AUTO;
volatile LONG g_deviceRequestGeneration = 0;
DWORD g_deviceLastFailureTick = 0;

constexpr DWORD kDeviceRetryBackoffMs = 5000;
constexpr DWORD kDeviceLostRetryMs = 250;
DWORD g_deviceRetryDelayMs = kDeviceRetryBackoffMs;

DWORD g_frameThreadId = 0;
int g_presentDepth = 0;

DWORD g_lastResizeRequestTick = 0;
constexpr DWORD kResizeCoalesceMs = 120;

bool g_deferredRequestValid = false;
HWND g_deferredRequestWindow = nullptr;
int g_deferredRequestWidth = 0;
int g_deferredRequestHeight = 0;
QohPresentationMode g_deferredRequestMode = QOH_PRESENTATION_WINDOWED;
QohRendererBackend g_deferredRequestBackend = QOH_RENDERER_AUTO;

bool g_menuCaptureSyncPending = false;

bool g_inputResyncPending = false;

bool g_inputDisplayVisible = true;
bool g_replayTransportVisible = true;

/* Box visibility mask: one bit per HitboxBoxKind (0..6), bit 7 for the axis line. */
unsigned g_hitboxKindMask[2] = {0xFFu, 0xFFu};
unsigned g_frameAdvSideMask = 3u;
unsigned g_inputViewerSideMask = 3u;
bool g_overlayFilterLoaded = false;
void OverlayFilterSettingsEnsure();

struct PresenterDiag
{
    DWORD creates = 0;
    DWORD resets = 0;
    DWORD failures = 0;
    DWORD losses = 0;
    DWORD reentrantPresents = 0;
    HRESULT lastError = 0;
};
PresenterDiag g_diag = {};

constexpr size_t kPresentationTraceCapacity = 256u;
struct PresentationTraceEntry
{
    DWORD serial = 0;
    DWORD kind = 0;
    QohPresentationMode requestedMode = QOH_PRESENTATION_WINDOWED;
    QohPresentationMode effectiveMode = QOH_PRESENTATION_WINDOWED;
    QohRendererBackend backend = QOH_RENDERER_AUTO;
    BOOL d3dWindowed = TRUE;
    DWORD swapEffect = 0;
    DWORD presentationInterval = 0;
    UINT adapter = 0;
    HMONITOR monitor = nullptr;
    DWORD sourceFormat = 0;
    DWORD sourceBitsPerPixel = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    DWORD backBufferFormat = 0;
    UINT backBufferWidth = 0;
    UINT backBufferHeight = 0;
    UINT refreshHz = 0;
    LONGLONG uploadStartQpc = 0;
    LONGLONG uploadEndQpc = 0;
    LONGLONG beginSceneQpc = 0;
    LONGLONG presentEntryQpc = 0;
    LONGLONG presentReturnQpc = 0;
    LONGLONG gdiEntryQpc = 0;
    LONGLONG gdiReturnQpc = 0;
    HRESULT result = S_OK;
    DWORD resetGeneration = 0;
    char fallbackReason[96] = {};
};

PresentationTraceEntry g_presentationTrace[kPresentationTraceCapacity] = {};
size_t g_presentationTraceWrite = 0u;
size_t g_presentationTraceSize = 0u;
unsigned long long g_presentationTraceTotal = 0u;
LARGE_INTEGER g_presentationTraceFrequency = {};

LONGLONG presentationQpcNow()
{
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

void initializePresentationTraceEntry(PresentationTraceEntry &entry,
                                      const EfzDx9PresentParams &params)
{
    entry = PresentationTraceEntry{};
    entry.serial = params.presentSerial;
    entry.kind = static_cast<DWORD>(params.presentationKind);
    entry.requestedMode = params.requestedMode;
    entry.effectiveMode = g_presenter.effectiveMode;
    entry.backend = g_presenter.effectiveBackend;
    entry.d3dWindowed = g_presenter.presentParams.Windowed;
    entry.swapEffect = static_cast<DWORD>(g_presenter.presentParams.SwapEffect);
    entry.presentationInterval = g_presenter.presentParams.PresentationInterval;
    entry.adapter = g_presenter.adapterOrdinal;
    entry.monitor = g_presenter.monitor;
    entry.sourceFormat = static_cast<DWORD>(params.format.kind);
    entry.sourceBitsPerPixel = params.format.bits_per_pixel;
    entry.sourceWidth = params.width;
    entry.sourceHeight = params.height;
    entry.backBufferFormat =
        static_cast<DWORD>(g_presenter.presentParams.BackBufferFormat);
    entry.backBufferWidth = g_presenter.presentParams.BackBufferWidth;
    entry.backBufferHeight = g_presenter.presentParams.BackBufferHeight;
    entry.refreshHz = g_presenter.presentParams.FullScreen_RefreshRateInHz;
    if (entry.refreshHz == 0u) entry.refreshHz = g_presenter.desktopMode.RefreshRate;
    entry.resetGeneration = g_presenter.resetGeneration;
}

unsigned long long traceTicksToMicroseconds(LONGLONG ticks)
{
    if (ticks <= 0) return 0u;
    if (g_presentationTraceFrequency.QuadPart <= 0)
        QueryPerformanceFrequency(&g_presentationTraceFrequency);
    if (g_presentationTraceFrequency.QuadPart <= 0) return 0u;
    return static_cast<unsigned long long>(ticks) * 1000000ull
        / static_cast<unsigned long long>(g_presentationTraceFrequency.QuadPart);
}

void logPresentationTraceSummary()
{
    if (g_presentationTraceSize == 0u) return;
    unsigned long long durations[kPresentationTraceCapacity] = {};
    size_t count = 0u;
    size_t gdiCount = 0u;
    for (size_t i = 0; i < g_presentationTraceSize; ++i)
    {
        const PresentationTraceEntry &entry = g_presentationTrace[i];
        LONGLONG elapsed = 0;
        if (entry.gdiEntryQpc != 0 && entry.gdiReturnQpc >= entry.gdiEntryQpc)
        {
            elapsed = entry.gdiReturnQpc - entry.gdiEntryQpc;
            ++gdiCount;
        }
        else if (entry.presentEntryQpc != 0
                 && entry.presentReturnQpc >= entry.presentEntryQpc)
        {
            elapsed = entry.presentReturnQpc - entry.presentEntryQpc;
        }
        if (elapsed > 0) durations[count++] = traceTicksToMicroseconds(elapsed);
    }
    if (count == 0u) return;
    std::sort(durations, durations + count);
    const auto percentile = [&](size_t numerator) {
        size_t index = ((count - 1u) * numerator + 99u) / 100u;
        if (index >= count) index = count - 1u;
        return durations[index];
    };
    DXLOG("[present-summary] count=%llu window=%u median=%lluus p90=%lluus "
          "p95=%lluus p99=%lluus max=%lluus gdi=%u lost=%lu resets=%lu",
          g_presentationTraceTotal, static_cast<unsigned>(g_presentationTraceSize),
          durations[(count - 1u) / 2u], percentile(90u), percentile(95u),
          percentile(99u), durations[count - 1u], static_cast<unsigned>(gdiCount),
          static_cast<unsigned long>(g_diag.losses),
          static_cast<unsigned long>(g_presenter.resetGeneration));
}

PresentationTraceEntry &appendPresentationTrace(
    const EfzDx9PresentParams &params)
{
    PresentationTraceEntry &entry =
        g_presentationTrace[g_presentationTraceWrite];
    initializePresentationTraceEntry(entry, params);
    g_presentationTraceWrite =
        (g_presentationTraceWrite + 1u) % kPresentationTraceCapacity;
    if (g_presentationTraceSize < kPresentationTraceCapacity)
        ++g_presentationTraceSize;
    ++g_presentationTraceTotal;
    return entry;
}

void recordD3dPresentation(const EfzDx9PresentParams &params,
                           LONGLONG uploadStartQpc,
                           LONGLONG uploadEndQpc,
                           LONGLONG beginSceneQpc,
                           LONGLONG presentEntryQpc,
                           LONGLONG presentReturnQpc,
                           HRESULT result,
                           const char *fallbackReason)
{
    PresentationTraceEntry &entry = appendPresentationTrace(params);
    entry.uploadStartQpc = uploadStartQpc;
    entry.uploadEndQpc = uploadEndQpc;
    entry.beginSceneQpc = beginSceneQpc;
    entry.presentEntryQpc = presentEntryQpc;
    entry.presentReturnQpc = presentReturnQpc;
    entry.result = result;
    if (fallbackReason != nullptr)
        std::snprintf(entry.fallbackReason, sizeof(entry.fallbackReason), "%s",
                      fallbackReason);
    if (SUCCEEDED(result)
        && (g_presentationTraceTotal % kPresentationTraceCapacity) == 0u)
        logPresentationTraceSummary();
}

LONG deviceState()
{
    return InterlockedCompareExchange(&g_deviceState, 0, 0);
}

bool deviceReady()
{
    const LONG state = deviceState();
    return (state == kDeviceReady || state == kDeviceInFrame)
        && g_presenter.device != nullptr;
}

void LogDeviceWindowGeometry(const char* stage, HWND window,
                             QohPresentationMode mode, int width, int height)
{
    RECT outer{}, client{};
    GetWindowRect(window, &outer);
    GetClientRect(window, &client);
    DXLOG("[d3d9-window] %s mode=%u requestedClient=%dx%d "
          "outer=(%ld,%ld)-(%ld,%ld) client=%ldx%ld style=%08lX zoomed=%d",
          stage, static_cast<unsigned>(mode), width, height,
          outer.left, outer.top, outer.right, outer.bottom,
          client.right - client.left, client.bottom - client.top,
          static_cast<unsigned long>(GetWindowLongA(window, GWL_STYLE)),
          static_cast<int>(IsZoomed(window)));
}

struct ExclusiveExitWindowFrame {
    HWND window = nullptr;
    QohPresentationMode mode = QOH_PRESENTATION_WINDOWED;
    RECT rect{};
    LONG style = 0;
    LONG exStyle = 0;
    uint64_t captureId = 0;
};
ExclusiveExitWindowFrame g_exclusiveExitWindowFrame;
std::mutex g_exclusiveExitWindowFrameMutex;
uint64_t g_exclusiveExitCaptureId = 0;

void DiscardExclusiveExitWindowFrame()
{
    std::lock_guard<std::mutex> guard(g_exclusiveExitWindowFrameMutex);
    g_exclusiveExitWindowFrame = {};
}

class RestoreWindowAfterDeviceWork {
public:
    RestoreWindowAfterDeviceWork(HWND window, QohPresentationMode mode) {
        std::lock_guard<std::mutex> guard(g_exclusiveExitWindowFrameMutex);
        if (g_exclusiveExitWindowFrame.window == window
            && g_exclusiveExitWindowFrame.mode == mode)
            frame_ = g_exclusiveExitWindowFrame;
    }
    ~RestoreWindowAfterDeviceWork() {
        if (frame_.window == nullptr) return;
        {
            std::lock_guard<std::mutex> guard(g_exclusiveExitWindowFrameMutex);
            if (g_exclusiveExitWindowFrame.captureId == frame_.captureId)
                g_exclusiveExitWindowFrame = {};
        }
        SetWindowLongA(frame_.window, GWL_STYLE, frame_.style);
        SetWindowLongA(frame_.window, GWL_EXSTYLE, frame_.exStyle & ~WS_EX_TOPMOST);
        SetWindowPos(frame_.window, HWND_NOTOPMOST,
            frame_.rect.left, frame_.rect.top,
            frame_.rect.right - frame_.rect.left,
            frame_.rect.bottom - frame_.rect.top,
            SWP_NOACTIVATE | SWP_FRAMECHANGED);
        LogDeviceWindowGeometry("exit-frame-restored", frame_.window, frame_.mode, 0, 0);
    }
private:
    ExclusiveExitWindowFrame frame_{};
};

bool ProvisionDevice(HWND window, int clientWidth, int clientHeight,
                     QohPresentationMode mode, QohRendererBackend backend)
{
    if (window == nullptr || clientWidth <= 0 || clientHeight <= 0)
    {
        return false;
    }

    if (g_presenter.device == nullptr || g_presenter.window != window
        || g_presenter.effectiveMode != mode
        || g_presenter.requestedBackend != backend)
    {
        return CreateDevice(window, clientWidth, clientHeight, mode, backend);
    }

    if (g_presenter.deviceEx != nullptr
        && g_presenter.exDisabledAfterHardFailure)
    {
        return CreateDevice(window, clientWidth, clientHeight, mode, backend);
    }

    if (g_presenter.deviceLost)
    {
        const HRESULT status = g_presenter.deviceEx != nullptr
            ? g_presenter.deviceEx->CheckDeviceState(g_presenter.window)
            : g_presenter.device->TestCooperativeLevel();
        if (status == S_PRESENT_OCCLUDED)
        {
            g_diag.lastError = status;
            g_deviceRetryDelayMs = kDeviceLostRetryMs;
            return false;
        }
        if (status == D3DERR_DEVICELOST)
        {
            g_diag.lastError = status;
            g_deviceRetryDelayMs = kDeviceLostRetryMs;
            return false;
        }
        if (status != D3DERR_DEVICENOTRESET && SUCCEEDED(status))
        {
            g_presenter.deviceLost = false;
        }
    }
    else if (!g_presenter.presentParams.Windowed
             || (static_cast<UINT>(clientWidth)
                     == g_presenter.presentParams.BackBufferWidth
                 && static_cast<UINT>(clientHeight)
                     == g_presenter.presentParams.BackBufferHeight))
    {
        NotifyDeviceObjectsValid();
        return true;
    }

    if (ResetDevice(clientWidth, clientHeight, mode, backend))
    {
        return true;
    }

    return CreateDevice(window, clientWidth, clientHeight, mode, backend);
}

bool RequestDeviceWork(HWND window, int clientWidth, int clientHeight,
                       QohPresentationMode mode, QohRendererBackend backend)
{
    if (window == nullptr || !IsWindow(window)
        || clientWidth <= 0 || clientHeight <= 0
        || !qoh99_presentation_mode_valid(static_cast<int>(mode)))
    {
        return false;
    }

    const LONG current = deviceState();
    if (current == kDeviceInFrame && GetCurrentThreadId() != g_frameThreadId)
        return false;
    {
        std::lock_guard<std::mutex> guard(g_exclusiveExitWindowFrameMutex);
        if (mode == QOH_PRESENTATION_EXCLUSIVE) {
            g_exclusiveExitWindowFrame = {};
        } else if (g_presenter.device != nullptr && !g_presenter.presentParams.Windowed
            && (g_exclusiveExitWindowFrame.window != window
                || g_exclusiveExitWindowFrame.mode != mode)) {
            RECT rect{};
            if (GetWindowRect(window, &rect)) {
                g_exclusiveExitWindowFrame = {window, mode, rect,
                    GetWindowLongA(window, GWL_STYLE), GetWindowLongA(window, GWL_EXSTYLE),
                    ++g_exclusiveExitCaptureId};
            }
        }
    }

    if (current == kDeviceInFrame)
    {
        if (GetCurrentThreadId() == g_frameThreadId)
        {
            g_deferredRequestWindow = window;
            g_deferredRequestWidth = clientWidth;
            g_deferredRequestHeight = clientHeight;
            g_deferredRequestMode = mode;
            g_deferredRequestBackend = backend;
            g_deferredRequestValid = true;
        }
        return false;
    }
    if (current == kDeviceFailed
        && mode == g_presenter.requestedMode
        && backend == g_presenter.requestedBackend
        && (GetTickCount() - g_deviceLastFailureTick) < g_deviceRetryDelayMs)
    {
        return false;
    }

    if (mode != g_presenter.effectiveMode)
        LogDeviceWindowGeometry("request", window, mode, clientWidth, clientHeight);
    g_deviceRequestWindow = window;
    g_deviceRequestWidth = clientWidth;
    g_deviceRequestHeight = clientHeight;
    g_deviceRequestMode = mode;
    g_deviceRequestBackend = backend;
    if (backend != QOH_RENDERER_D3D9EX_FLIPEX)
        g_presenter.exDisabledAfterHardFailure = false;
    g_presenter.requestedMode = mode;
    g_presenter.requestedBackend = backend;
    g_presenter.readiness = QOH_RENDERER_PENDING;
    InterlockedIncrement(&g_deviceRequestGeneration);
    if (current == kDeviceWorking)
    {
        PostMessageA(window, WM_QOH99_D3D_DEVICE_WORK, 0, 0);
        return false;
    }
    InterlockedExchange(&g_deviceState, kDevicePending);
    if (!PostMessageA(window, WM_QOH99_D3D_DEVICE_WORK, 0, 0))
    {
        g_diag.lastError = HRESULT_FROM_WIN32(GetLastError());
        g_presenter.readiness = QOH_RENDERER_FAILED;
        InterlockedExchange(&g_deviceState, kDeviceFailed);
        return false;
    }
    return true;
}

bool TrainingMenuShouldDraw()
{
    return g_presenter.trainingMenuRenderer != nullptr
        && (g_presenter.trainingMenuVisible == nullptr
            || g_presenter.trainingMenuVisible(g_presenter.trainingMenuUserData));
}

bool TrainingMenuWantsDraw()
{
    if (g_presenter.trainingMenuRenderer == nullptr)
        return false;
    if (g_presenter.trainingMenuWantsDraw != nullptr)
        return g_presenter.trainingMenuWantsDraw(g_presenter.trainingMenuUserData) != FALSE;
    return TrainingMenuShouldDraw();
}

bool PresenterNeedsImGuiPath()
{
    return g_presenter.menuVisible
        || g_presenter.hudProvider != nullptr
        || g_presenter.overlayProvider != nullptr
        || g_presenter.trainingStatusProvider != nullptr
        || g_presenter.inputDisplayProvider != nullptr
        || g_presenter.replayTransportStatus != nullptr;
}

bool PresenterNeedsAnyOverlay()
{
    return PresenterNeedsImGuiPath() || TrainingMenuWantsDraw();
}

ImDrawList* WrapperOverlayDrawList()
{
    return g_presenter.menuVisible ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
}

bool ReadTrainingStatus(Qoh99TrainingStatusV1 &outStatus)
{
    std::memset(&outStatus, 0, sizeof(outStatus));
    outStatus.apiVersion = QOH99_TRAINING_UI_API_VERSION;
    outStatus.structSize = sizeof(outStatus);
    if (g_presenter.trainingUiApiVersion != QOH99_TRAINING_UI_API_VERSION
        || g_presenter.trainingStatusProvider == nullptr)
    {
        return false;
    }
    if (!g_presenter.trainingStatusProvider(
            &outStatus, sizeof(outStatus), g_presenter.trainingUiUserData))
    {
        return false;
    }
    if (outStatus.apiVersion != QOH99_TRAINING_UI_API_VERSION
        || outStatus.structSize < QOH99_TRAINING_STATUS_V1_BASE_SIZE
        || outStatus.structSize > sizeof(Qoh99TrainingStatusV1))
    {
        return false;
    }
    outStatus.disableText[sizeof(outStatus.disableText) - 1] = '\0';
    outStatus.statusText[sizeof(outStatus.statusText) - 1] = '\0';
    outStatus.resultText[sizeof(outStatus.resultText) - 1] = '\0';
    return true;
}

void InvalidateTrainingUiDrafts()
{
    g_trainingUi.resourceDraftsSeeded = false;
    g_trainingUi.recoveryDraftsSeeded = false;
    g_trainingUi.combatToolsDraftsSeeded = false;
    g_trainingUi.combatToolsDraftCharacter = 0xFFFFFFFFu;
    g_trainingUi.combatToolsDraftGeneration = 0xFFFFFFFFu;
    g_trainingUi.recoveryRequestPending = false;
    g_trainingUi.recoveryRequestTick = 0;
}

void RefreshTrainingUiForStatus(const Qoh99TrainingStatusV1 &status)
{
    const bool generationChanged =
        !g_trainingUi.statusObserved
        || g_trainingUi.generation != status.trainingGeneration;
    const bool lifecycleChanged =
        !g_trainingUi.statusObserved
        || g_trainingUi.lifecycleState != status.lifecycleState
        || g_trainingUi.scene != status.scene
        || g_trainingUi.matchSubstate != status.matchSubstate;

    if (generationChanged)
    {
        g_trainingUi.lastRejectedCommandId = 0;
        g_trainingUi.lastRejectedTick = 0;
        g_trainingUi.refillTarget = QOH99_TRAINING_REFILL_BOTH;
        g_trainingUi.resourceTarget = QOH99_TRAINING_REFILL_BOTH;
    }

    if (generationChanged || lifecycleChanged
        || g_trainingUi.refreshDraftsOnNextDraw)
    {
        InvalidateTrainingUiDrafts();
    }

    for (int i = 0; i < QOH99_TRAINING_RECOVERY_SLOT_COUNT; ++i)
    {
        if (generationChanged)
        {
            g_trainingUi.requestedRecoveryMode[i] =
                QOH99_TRAINING_RECOVERY_OFF;
        }
    }

    g_trainingUi.generation = status.trainingGeneration;
    g_trainingUi.lifecycleState = status.lifecycleState;
    g_trainingUi.scene = status.scene;
    g_trainingUi.matchSubstate = status.matchSubstate;
    g_trainingUi.statusObserved = true;
    g_trainingUi.refreshDraftsOnNextDraw = false;
}

bool TrainingHasExtendedStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_COMBAT_TIER;
}

bool TrainingHasFighterStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_FIGHTER_TIER;
}

bool TrainingHasRecoveryStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_RECOVERY_TIER;
}

bool TrainingHasWallStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_WALL_TIER;
}

bool TrainingHasRecoveryDiagnosticStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize
        >= QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER;
}

void DrawTrainingStaleDllReason(
    const Qoh99TrainingStatusV1 &status, DWORD requiredTier)
{
    ImGui::TextDisabled(
        "Needs a newer injected QOH99 DLL - it is reporting a %lu-byte\n"
        "status block, and this group needs at least %lu. Everything else\n"
        "on these tabs still works exactly as it did.",
        static_cast<unsigned long>(status.structSize),
        static_cast<unsigned long>(requiredTier));
}

bool EnqueueTrainingCommand(
    const Qoh99TrainingStatusV1 &status,
    DWORD kind,
    DWORD slotOrSetting,
    DWORD value,
    DWORD flags = 0)
{
    if (g_presenter.trainingUiApiVersion != QOH99_TRAINING_UI_API_VERSION
        || g_presenter.trainingCommandEnqueue == nullptr)
    {
        return false;
    }

    if (g_trainingUi.nextCommandId == 0)
    {
        return false;
    }

    Qoh99TrainingCommandV1 command = {};
    command.apiVersion = QOH99_TRAINING_UI_API_VERSION;
    command.structSize = sizeof(command);
    command.trainingGeneration = status.trainingGeneration;
    command.commandId = g_trainingUi.nextCommandId++;
    command.commandKind = kind;
    command.slotOrSetting = slotOrSetting;
    command.value = value;
    command.flags = flags;

    if (!g_presenter.trainingCommandEnqueue(
            &command, sizeof(command), g_presenter.trainingUiUserData))
    {
        g_trainingUi.lastRejectedCommandId = command.commandId;
        g_trainingUi.lastRejectedTick = GetTickCount();
        return false;
    }
    g_trainingUi.lastRejectedCommandId = 0;
    g_trainingUi.lastRejectedTick = 0;
    return true;
}

void SyncTrainingMenuCapture()
{
    if (g_presenter.trainingMenuCapture == nullptr)
    {
        g_trainingUi.capturedState = -1;
        g_trainingUi.capturedGeneration = 0;
        return;
    }

    Qoh99TrainingStatusV1 status = {};
    if (!ReadTrainingStatus(status) || status.explicitTrainingPresent == 0)
    {
        if (g_trainingUi.capturedState == 1)
        {
            g_presenter.trainingMenuCapture(
                g_trainingUi.capturedGeneration, FALSE,
                g_presenter.trainingUiUserData);
        }
        g_trainingUi.capturedState = -1;
        g_trainingUi.capturedGeneration = 0;
        return;
    }

    const int desired = g_presenter.menuVisible ? 1 : 0;
    if (g_trainingUi.capturedGeneration == status.trainingGeneration
        && g_trainingUi.capturedState == desired)
    {
        return;
    }
    if (g_presenter.trainingMenuCapture(
            status.trainingGeneration, desired ? TRUE : FALSE,
            g_presenter.trainingUiUserData))
    {
        g_trainingUi.capturedGeneration = status.trainingGeneration;
        g_trainingUi.capturedState = desired;
    }
}

void RenderTrainingMenu(const EfzDx9PresentParams *params)
{
    if (params == nullptr || !TrainingMenuWantsDraw())
    {
        return;
    }

    ImGuiContext *savedContext = ImGui::GetCurrentContext();
    g_presenter.trainingMenuRenderer(
        g_presenter.device,
        g_presenter.window,
        params,
        g_presenter.trainingMenuUserData);
    if (savedContext != nullptr)
    {
        ImGui::SetCurrentContext(savedContext);
    }
}

void ReleaseFrameTexture()
{
    if (g_presenter.frameTexture != nullptr)
    {
        g_presenter.frameTexture->Release();
        g_presenter.frameTexture = nullptr;
    }
    g_presenter.textureWidth = 0;
    g_presenter.textureHeight = 0;
}

// The prescale target is a D3DPOOL_DEFAULT texture, so it must be released before any device->Reset().
void ReleaseScaleTexture()
{
    if (g_presenter.scaleTexture != nullptr)
    {
        g_presenter.scaleTexture->Release();
        g_presenter.scaleTexture = nullptr;
    }
    g_presenter.scaleTextureW = 0;
    g_presenter.scaleTextureH = 0;
}

ImFont *g_trainingPixelFont = nullptr;

volatile LONG g_menuNavigationPending = 0;

constexpr LONG kMenuNavigationMaxPending = 4;
volatile LONG g_menuNavigationCount[QOH99_MENU_NAV_COUNT] = {};

char g_menuConfirmLabel[24] = "A";
char g_menuBackLabel[24] = "B";

void ShutdownImGui()
{
    if (!g_presenter.imguiInitialized)
    {
        return;
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_trainingPixelFont = nullptr;
    g_presenter.imguiContext = nullptr;
    g_presenter.imguiInitialized = false;
}

bool g_deviceObjectsLive = false;

void NotifyDeviceObjectsInvalid()
{
    if (!g_deviceObjectsLive)
    {
        return;
    }
    g_deviceObjectsLive = false;

    // Each ImGui_ImplDX9 call must run with its OWNING context current.
    if (g_presenter.imguiInitialized && g_presenter.imguiContext != nullptr)
    {
        ImGuiContext *saved = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_presenter.imguiContext);
        ImGui_ImplDX9_InvalidateDeviceObjects();
        ImGui::SetCurrentContext(saved);
    }
    if (g_presenter.trainingMenuDeviceNotify != nullptr)
    {
        g_presenter.trainingMenuDeviceNotify(0, g_presenter.trainingMenuUserData);
    }
    ReleaseFrameTexture();
    ReleaseScaleTexture();
}

void NotifyDeviceObjectsValid()
{
    if (g_deviceObjectsLive || g_presenter.device == nullptr)
    {
        return;
    }
    g_deviceObjectsLive = true;

    if (g_presenter.imguiInitialized && g_presenter.imguiContext != nullptr)
    {
        ImGuiContext *saved = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_presenter.imguiContext);
        ImGui_ImplDX9_CreateDeviceObjects();
        ImGui::SetCurrentContext(saved);
    }
    if (g_presenter.trainingMenuDeviceNotify != nullptr)
    {
        g_presenter.trainingMenuDeviceNotify(1, g_presenter.trainingMenuUserData);
    }
}

void DestroyDevice()
{
    // Phase 0 must run while the device is alive, because consumers cannot release objects against a lost device.
    NotifyDeviceObjectsInvalid();
    ReleaseFrameTexture();
    ReleaseScaleTexture();
    ShutdownImGui();

    if (g_presenter.deviceEx != nullptr)
    {
        g_presenter.deviceEx->Release();
        g_presenter.deviceEx = nullptr;
        g_presenter.device = nullptr;
    }
    else if (g_presenter.device != nullptr)
    {
        g_presenter.device->Release();
        g_presenter.device = nullptr;
    }
    if (g_presenter.d3dEx != nullptr)
    {
        g_presenter.d3dEx->Release();
        g_presenter.d3dEx = nullptr;
        g_presenter.d3d = nullptr;
    }
    else if (g_presenter.d3d != nullptr)
    {
        g_presenter.d3d->Release();
        g_presenter.d3d = nullptr;
    }

    g_presenter.window = nullptr;
    g_presenter.deviceLost = false;
}

struct OverlayFontStatus
{
    bool cyrillic = false;
    bool japanese = false;
    std::string basePath;
    std::string japanesePath;
};

OverlayFontStatus g_overlayFontStatus;

bool ExistingFontFile(const std::string &path)
{
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string WrapperModuleDirectory()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&WrapperModuleDirectory), &self);
    char modulePath[MAX_PATH] = {};
    const DWORD count = GetModuleFileNameA(self, modulePath, MAX_PATH);
    if (count == 0 || count >= MAX_PATH)
    {
        return {};
    }
    char *slash = std::strrchr(modulePath, '\\');
    if (slash == nullptr)
    {
        return {};
    }
    *slash = '\0';
    return modulePath;
}

template <size_t N>
bool FindInstalledFontFile(const char *const (&fileNames)[N],
                           std::string &outPath)
{
    std::string directories[3];
    directories[0] = WrapperModuleDirectory();

    char windowsPath[MAX_PATH] = {};
    const UINT windowsLength = GetWindowsDirectoryA(windowsPath, MAX_PATH);
    if (windowsLength != 0 && windowsLength < MAX_PATH)
    {
        directories[1] = std::string(windowsPath) + "\\Fonts";
    }

    char localAppData[MAX_PATH] = {};
    const DWORD localLength = GetEnvironmentVariableA(
        "LOCALAPPDATA", localAppData, MAX_PATH);
    if (localLength != 0 && localLength < MAX_PATH)
    {
        directories[2] = std::string(localAppData)
            + "\\Microsoft\\Windows\\Fonts";
    }

    for (const std::string &directory : directories)
    {
        if (directory.empty())
        {
            continue;
        }
        for (const char *fileName : fileNames)
        {
            const std::string candidate = directory + "\\" + fileName;
            if (ExistingFontFile(candidate))
            {
                outPath = candidate;
                return true;
            }
        }
    }
    return false;
}

template <size_t N>
ImFont *AddInstalledGdiFont(
    ImFontAtlas *atlas, const wchar_t *const (&faces)[N],
    const wchar_t *probe, int probeCount, float pixels,
    const ImFontConfig *config, const ImWchar *ranges,
    std::string &outDescription)
{
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr)
    {
        return nullptr;
    }

    ImFont *added = nullptr;
    for (const wchar_t *face : faces)
    {
        LOGFONTW request = {};
        request.lfHeight = -static_cast<LONG>(pixels);
        request.lfWeight = FW_NORMAL;
        request.lfCharSet = DEFAULT_CHARSET;
        request.lfQuality = ANTIALIASED_QUALITY;
        lstrcpynW(request.lfFaceName, face, LF_FACESIZE);
        HFONT font = CreateFontIndirectW(&request);
        if (font == nullptr)
        {
            continue;
        }
        HGDIOBJ oldFont = SelectObject(dc, font);
        if (oldFont == nullptr || oldFont == HGDI_ERROR)
        {
            DeleteObject(font);
            continue;
        }

        WORD indices[4] = { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu };
        bool hasGlyphs = probeCount > 0 && probeCount <= 4
            && GetGlyphIndicesW(dc, probe, probeCount, indices,
                                GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR;
        for (int i = 0; hasGlyphs && i < probeCount; ++i)
        {
            hasGlyphs = indices[i] != 0xFFFFu;
        }

        const DWORD fontBytes = hasGlyphs
            ? GetFontData(dc, 0u, 0u, nullptr, 0u) : GDI_ERROR;
        constexpr DWORD kMaximumFontBytes = 64u * 1024u * 1024u;
        if (fontBytes != GDI_ERROR && fontBytes > 0u
            && fontBytes <= kMaximumFontBytes)
        {
            void *data = ImGui::MemAlloc(fontBytes);
            if (data != nullptr
                && GetFontData(dc, 0u, 0u, data, fontBytes) == fontBytes)
            {
                added = atlas->AddFontFromMemoryTTF(
                    data, static_cast<int>(fontBytes), pixels, config, ranges);
                if (added != nullptr)
                {
                    char faceName[LF_FACESIZE] = {};
                    WideCharToMultiByte(CP_UTF8, 0, face, -1,
                                        faceName, LF_FACESIZE,
                                        nullptr, nullptr);
                    outDescription = std::string("GDI: ") + faceName;
                }
                else
                {
                    ImGui::MemFree(data);
                }
            }
            else if (data != nullptr)
            {
                ImGui::MemFree(data);
            }
        }

        SelectObject(dc, oldFont);
        DeleteObject(font);
        if (added != nullptr)
        {
            break;
        }
    }
    DeleteDC(dc);
    return added;
}

void ConfigureOverlayFonts(ImGuiIO &io)
{
    g_overlayFontStatus = {};
    static const char *const kCyrillicFonts[] = {
        "segoeui.ttf", "arial.ttf", "tahoma.ttf",
        "DejaVuSans.ttf", "LiberationSans-Regular.ttf",
        "NotoSans-Regular.ttf", "NotoSansCJKjp-Regular.ttf",
    };
    static const char *const kJapaneseFonts[] = {
        "NotoSansCJKjp-Regular.ttf", "NotoSansCJKjp-Regular.otf",
        "NotoSansCJK-Regular.ttc", "NotoSansJP-Regular.ttf",
        "YuGothM.ttc", "YuGothR.ttc", "meiryo.ttc", "msgothic.ttc",
        "ipaexg.ttf", "ipag.ttf", "VL-Gothic-Regular.ttf",
        "DroidSansFallbackFull.ttf", "wqy-zenhei.ttc",
    };
    static const wchar_t *const kCyrillicFaces[] = {
        L"Segoe UI", L"Arial", L"Tahoma", L"DejaVu Sans",
        L"Liberation Sans", L"Noto Sans",
    };
    static const wchar_t *const kJapaneseFaces[] = {
        L"Meiryo", L"Meiryo UI", L"Yu Gothic UI", L"Yu Gothic",
        L"MS UI Gothic", L"MS Gothic", L"MS PGothic",
        L"Noto Sans CJK JP", L"Noto Sans JP", L"Source Han Sans JP",
        L"IPAexGothic", L"IPAGothic", L"IPAPGothic", L"VL Gothic",
        L"Droid Sans Fallback", L"WenQuanYi Zen Hei",
    };
    static const wchar_t kCyrillicProbe[] = { 0x0416 };
    static const wchar_t kJapaneseProbe[] = { 0x3042, 0x30A2, 0x65E5 };

    constexpr float kOverlayFontPixels = 14.0f;
    ImFont *font = nullptr;
    if (FindInstalledFontFile(kCyrillicFonts, g_overlayFontStatus.basePath))
    {
        font = io.Fonts->AddFontFromFileTTF(
            g_overlayFontStatus.basePath.c_str(), kOverlayFontPixels, nullptr,
            io.Fonts->GetGlyphRangesCyrillic());
    }
    if (font == nullptr)
    {
        font = AddInstalledGdiFont(
            io.Fonts, kCyrillicFaces, kCyrillicProbe,
            static_cast<int>(sizeof(kCyrillicProbe) / sizeof(kCyrillicProbe[0])),
            kOverlayFontPixels, nullptr, io.Fonts->GetGlyphRangesCyrillic(),
            g_overlayFontStatus.basePath);
    }
    if (font == nullptr)
    {
        g_overlayFontStatus.basePath.clear();
        font = io.Fonts->AddFontDefault();
    }
    io.FontDefault = font;

    ImFontConfig merge;
    merge.MergeMode = true;
    merge.PixelSnapH = true;
    ImFont *japanese = nullptr;
    if (FindInstalledFontFile(kJapaneseFonts,
                              g_overlayFontStatus.japanesePath))
    {
        japanese = io.Fonts->AddFontFromFileTTF(
            g_overlayFontStatus.japanesePath.c_str(),
            kOverlayFontPixels, &merge,
            io.Fonts->GetGlyphRangesJapanese());
    }
    if (japanese == nullptr)
    {
        g_overlayFontStatus.japanesePath.clear();
        japanese = AddInstalledGdiFont(
            io.Fonts, kJapaneseFaces, kJapaneseProbe,
            static_cast<int>(sizeof(kJapaneseProbe) / sizeof(kJapaneseProbe[0])),
            kOverlayFontPixels, &merge, io.Fonts->GetGlyphRangesJapanese(),
            g_overlayFontStatus.japanesePath);
    }

    {
        ImFontConfig pixelConfig;
        pixelConfig.MergeMode = false;
        pixelConfig.PixelSnapH = true;
        pixelConfig.OversampleH = 1;
        pixelConfig.OversampleV = 1;
        g_trainingPixelFont = io.Fonts->AddFontDefault(&pixelConfig);
    }

    if (font != nullptr && io.Fonts->Build())
    {
        g_overlayFontStatus.cyrillic =
            font->FindGlyphNoFallback(0x0416) != nullptr;
        g_overlayFontStatus.japanese =
            font->FindGlyphNoFallback(0x3042) != nullptr
            && font->FindGlyphNoFallback(0x30A2) != nullptr
            && font->FindGlyphNoFallback(0x65E5) != nullptr;
    }

    DXLOG("[imgui] unicode font cyrillic=%u japanese=%u base=\"%s\" jp=\"%s\"",
          g_overlayFontStatus.cyrillic ? 1u : 0u,
          g_overlayFontStatus.japanese ? 1u : 0u,
          g_overlayFontStatus.basePath.empty()
              ? "embedded default" : g_overlayFontStatus.basePath.c_str(),
          g_overlayFontStatus.japanesePath.empty()
              ? "not found" : g_overlayFontStatus.japanesePath.c_str());
}

bool InitializeImGui(HWND window)
{
    if (g_presenter.imguiInitialized)
    {
        return true;
    }
    if (g_presenter.device == nullptr || window == nullptr)
    {
        return false;
    }

    IMGUI_CHECKVERSION();
    g_presenter.imguiContext = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ConfigureOverlayFonts(io);

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(window))
    {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX9_Init(g_presenter.device))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    g_presenter.imguiInitialized = true;
    return true;
}

void LogAdapter(IDirect3D9 *d3d, UINT adapter, HMONITOR monitor,
                const D3DDISPLAYMODE &desktop)
{
    if (d3d == nullptr || !qoh99_wlog_should())
    {
        return;
    }

    D3DADAPTER_IDENTIFIER9 ident = {};
    const HRESULT hr = d3d->GetAdapterIdentifier(adapter, 0, &ident);
    if (FAILED(hr))
    {
        DXLOG("[d3d9] GetAdapterIdentifier failed hr=0x%08lX",
              static_cast<unsigned long>(hr));
        return;
    }
    D3DCAPS9 caps = {};
    const HRESULT capsHr =
        d3d->GetDeviceCaps(adapter, D3DDEVTYPE_HAL, &caps);
    DXLOG("[d3d9] monitor=%p adapter=%u adapter=\"%.128s\" driver=\"%.64s\" "
          "desktop=%ux%u format=%u refresh=%u version=%u.%u.%u.%u "
          "vendor=%04X device=%04X adapters=%u capsHr=0x%08lX vsVersion=%08lX",
          static_cast<void *>(monitor), static_cast<unsigned>(adapter),
          ident.Description, ident.Driver,
          static_cast<unsigned>(desktop.Width),
          static_cast<unsigned>(desktop.Height),
          static_cast<unsigned>(desktop.Format),
          static_cast<unsigned>(desktop.RefreshRate),
          static_cast<unsigned>(HIWORD(ident.DriverVersion.HighPart)),
          static_cast<unsigned>(LOWORD(ident.DriverVersion.HighPart)),
          static_cast<unsigned>(HIWORD(ident.DriverVersion.LowPart)),
          static_cast<unsigned>(LOWORD(ident.DriverVersion.LowPart)),
          static_cast<unsigned>(ident.VendorId),
          static_cast<unsigned>(ident.DeviceId),
          static_cast<unsigned>(d3d->GetAdapterCount()),
          static_cast<unsigned long>(capsHr),
          static_cast<unsigned long>(caps.VertexShaderVersion));
    std::snprintf(g_presenter.adapterDescription,
                  sizeof(g_presenter.adapterDescription), "%.127s",
                  ident.Description);
}

bool TrainingHasMeterChargeStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_METER_CHARGE_TIER;
}

bool TrainingHasCombatToolsStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER;
}

bool TrainingHasContactPolicyStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_CONTACT_POLICY_TIER;
}

bool TrainingHasMacroAfterResetStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize
        >= QOH99_TRAINING_STATUS_V1_MACRO_AFTER_RESET_TIER;
}

bool TrainingHasReducePolicyStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_REDUCE_POLICY_TIER;
}

bool TrainingHasGuardTimingStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_GUARD_TIMING_TIER;
}

bool TrainingHasPlayerSwapStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_PLAYER_SWAP_TIER;
}

bool TrainingHasExRestoreTargetStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize
        >= QOH99_TRAINING_STATUS_V1_EX_RESTORE_TARGET_TIER;
}

bool TrainingHasActionTriggerStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER;
}

bool TrainingHasDebugOverlayStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER;
}

bool TrainingHasStateLogStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER;
}

bool TrainingHasActorPoolStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_ACTOR_POOL_TIER;
}

bool TrainingHasNativeHudStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize
        >= QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER;
}

bool TrainingHasFrameTraceStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_FRAME_TRACE_TIER;
}

bool TrainingHasFrameAdvantageGapStatus(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize
        >= QOH99_TRAINING_STATUS_V1_FRAME_ADVANTAGE_GAP_TIER;
}

char TrainingActorPoolLetter(DWORD encodedSlot)
{
    switch ((encodedSlot >> 24) & 0xFFu)
    {
    case QOH99_TRAINING_ACTOR_POOL_EFFECT: return 'E';
    case QOH99_TRAINING_ACTOR_POOL_CLONE:  return 'C';
    case QOH99_TRAINING_ACTOR_POOL_HELPER: return 'H';
    default: return '?';
    }
}

const char *TrainingSuppressionReasonText(DWORD reason)
{
    switch (reason)
    {
    case QOH99_TRAINING_SUPPRESSION_NONE:
        return "Responding now.";
    case QOH99_TRAINING_SUPPRESSION_NO_EVENT_ARMED:
        return "Nothing to react to yet.";
    case QOH99_TRAINING_SUPPRESSION_EVENT_NOT_CONFIGURED:
        return "The situation happened, but no response is set for it.";
    case QOH99_TRAINING_SUPPRESSION_WAITING_RESPONSE_READY:
        return "Waiting: the dummy cannot act yet.";
    case QOH99_TRAINING_SUPPRESSION_WAITING_DELAY:
        return "Waiting out the delay you set.";
    case QOH99_TRAINING_SUPPRESSION_WAITING_GUARD_CANCEL_WINDOW:
        return "Waiting for the guard-cancel window to open.";
    case QOH99_TRAINING_SUPPRESSION_WAITING_WAKEUP_RELEASE:
        return "Reversal is buffered, holding its button for wake-up.";
    case QOH99_TRAINING_SUPPRESSION_MACRO_BUSY:
        return "Busy: another recording or move is still playing.";
    case QOH99_TRAINING_SUPPRESSION_MOVE_UNAVAILABLE:
        return "The chosen move is not available on this character.";
    case QOH99_TRAINING_SUPPRESSION_EMPTY_SLOT:
        return "The chosen recording slot is empty.";
    case QOH99_TRAINING_SUPPRESSION_CHANCE_SKIPPED:
        return "Chance roll declined this one.";
    case QOH99_TRAINING_SUPPRESSION_NOT_MEASURABLE:
        return "Not measurable right now (round or state unreadable).";
    default:
        return "Unknown.";
    }
}

bool TrainingActionTriggerCatalogMatches(const Qoh99TrainingStatusV1 &status)
{
    return TrainingHasActionTriggerStatus(status)
        && status.triggerMoveCatalogId != 0u
        && status.triggerMoveCatalogId == qtraining::trainingMoveCatalogId();
}

void SeedTrainingCombatToolDrafts(const Qoh99TrainingStatusV1 &status)
{
    const bool triggerBlockLive = TrainingHasActionTriggerStatus(status)
        && status.triggerMoveCatalogId != 0u;
    if (!TrainingHasCombatToolsStatus(status) || !triggerBlockLive)
    {
        if (g_trainingUi.combatToolsDraftsSeeded)
        {
            for (int event = 0;
                 event < QOH99_TRAINING_TRIGGER_EVENT_COUNT; ++event)
            {
                g_trainingUi.triggerMacroSlot[event] = 0;
                g_trainingUi.triggerMoveIndex[event] = 0;
            }
            g_trainingUi.combatToolsDraftsSeeded = false;
            g_trainingUi.combatToolsDraftCharacter = 0xFFFFFFFFu;
            g_trainingUi.combatToolsDraftGeneration = 0xFFFFFFFFu;
        }
        return;
    }
    if (g_trainingUi.combatToolsDraftsSeeded
        && g_trainingUi.combatToolsDraftCharacter == status.p2Character
        && g_trainingUi.combatToolsDraftGeneration
               == status.trainingGeneration)
    {
        return;
    }

    g_trainingUi.throwTechEnabled = status.throwTechChancePercent != 0u;
    for (int event = 0; event < QOH99_TRAINING_TRIGGER_EVENT_COUNT; ++event)
    {
        g_trainingUi.triggerMacroSlot[event] = static_cast<int>(
            (std::min)(status.triggerMacroSlot[event],
                       static_cast<DWORD>(
                           QOH99_TRAINING_TRIGGER_RANDOM_RECORDING)));
        g_trainingUi.triggerMoveIndex[event] =
            TrainingActionTriggerCatalogMatches(status)
                ? static_cast<int>((std::min)(
                      status.triggerMoveIndex[event],
                      static_cast<DWORD>(
                          QOH99_TRAINING_TRIGGER_MAX_MOVE_INDEX)))
                : 0;
        g_trainingUi.triggerDelaySteps[event] = static_cast<int>(
            (std::min)(status.triggerDelaySteps[event],
                       static_cast<DWORD>(
                           QOH99_TRAINING_TRIGGER_MAX_DELAY_STEPS)));
        g_trainingUi.triggerChancePercent[event] = static_cast<int>(
            (std::min)(status.triggerChancePercent[event],
                       static_cast<DWORD>(
                           QOH99_TRAINING_TRIGGER_MAX_CHANCE_PERCENT)));
    }
    g_trainingUi.combatToolsDraftCharacter = status.p2Character;
    g_trainingUi.combatToolsDraftGeneration = status.trainingGeneration;
    g_trainingUi.combatToolsDraftsSeeded = true;
}

struct PresenterModeConfig
{
    QohPresentationMode mode = QOH_PRESENTATION_WINDOWED;
    UINT adapter = D3DADAPTER_DEFAULT;
    HMONITOR monitor = nullptr;
    D3DDISPLAYMODE desktop = {};
    D3DPRESENT_PARAMETERS params = {};
    bool exactAdapter = false;
};

bool BuildModeConfig(IDirect3D9 *d3d, HWND window, int clientWidth,
                     int clientHeight, QohPresentationMode mode,
                     PresenterModeConfig &out)
{
    if (d3d == nullptr || window == nullptr || clientWidth <= 0
        || clientHeight <= 0 || !qoh99_presentation_mode_valid(mode))
        return false;
    out = {};
    out.mode = mode;
    out.monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    for (UINT adapter = 0; adapter < d3d->GetAdapterCount(); ++adapter)
    {
        if (d3d->GetAdapterMonitor(adapter) == out.monitor)
        {
            out.adapter = adapter;
            out.exactAdapter = true;
            break;
        }
    }
    if (!out.exactAdapter)
    {
        out.adapter = D3DADAPTER_DEFAULT;
        if (mode == QOH_PRESENTATION_EXCLUSIVE)
            return false;
    }
    if (FAILED(d3d->GetAdapterDisplayMode(out.adapter, &out.desktop)))
        return false;

    out.params.Windowed = mode == QOH_PRESENTATION_EXCLUSIVE ? FALSE : TRUE;
    out.params.BackBufferWidth = out.params.Windowed
        ? static_cast<UINT>(clientWidth) : out.desktop.Width;
    out.params.BackBufferHeight = out.params.Windowed
        ? static_cast<UINT>(clientHeight) : out.desktop.Height;
    out.params.BackBufferFormat = out.params.Windowed
        ? D3DFMT_UNKNOWN : out.desktop.Format;
    out.params.FullScreen_RefreshRateInHz = out.params.Windowed
        ? 0u : out.desktop.RefreshRate;
    out.params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    out.params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    out.params.hDeviceWindow = window;
    out.params.EnableAutoDepthStencil = FALSE;
    if (!out.params.Windowed
        && (out.params.BackBufferFormat == D3DFMT_UNKNOWN
            || out.params.BackBufferWidth == 0u
            || out.params.BackBufferHeight == 0u))
        return false;
    return true;
}

using Direct3DCreate9ExFn = HRESULT (WINAPI *)(UINT, IDirect3D9Ex **);

bool TryCreateD3D9ExDevice(HWND window, int clientWidth, int clientHeight,
                           QohPresentationMode mode, DWORD behaviorFlags,
                           PresenterModeConfig &config,
                           HRESULT &createHr, HRESULT &latencyHr)
{
    createHr = D3DERR_NOTAVAILABLE;
    latencyHr = D3DERR_NOTAVAILABLE;
    if (mode == QOH_PRESENTATION_EXCLUSIVE)
        return false;

    HMODULE d3d9Module = GetModuleHandleW(L"d3d9.dll");
    const auto create9Ex = d3d9Module != nullptr
        ? reinterpret_cast<Direct3DCreate9ExFn>(
            GetProcAddress(d3d9Module, "Direct3DCreate9Ex"))
        : nullptr;
    if (create9Ex == nullptr)
    {
        DXLOG("[d3d9ex] Direct3DCreate9Ex export unavailable");
        return false;
    }

    createHr = create9Ex(D3D_SDK_VERSION, &g_presenter.d3dEx);
    if (FAILED(createHr) || g_presenter.d3dEx == nullptr)
    {
        DXLOG("[d3d9ex] Direct3DCreate9Ex FAILED hr=0x%08lX",
              static_cast<unsigned long>(createHr));
        return false;
    }
    g_presenter.d3d = static_cast<IDirect3D9 *>(g_presenter.d3dEx);
    if (!BuildModeConfig(g_presenter.d3d, window, clientWidth, clientHeight,
                         mode, config))
    {
        createHr = D3DERR_NOTAVAILABLE;
        return false;
    }

    config.params.Windowed = TRUE;
    config.params.BackBufferWidth = static_cast<UINT>(clientWidth);
    config.params.BackBufferHeight = static_cast<UINT>(clientHeight);
    config.params.BackBufferFormat = D3DFMT_UNKNOWN;
    config.params.BackBufferCount = 2u;
    config.params.SwapEffect = D3DSWAPEFFECT_FLIPEX;
    config.params.FullScreen_RefreshRateInHz = 0u;
    g_presenter.presentParams = config.params;
    createHr = g_presenter.d3dEx->CreateDeviceEx(
        config.adapter, D3DDEVTYPE_HAL, window, behaviorFlags,
        &g_presenter.presentParams, nullptr, &g_presenter.deviceEx);
    if (FAILED(createHr) || g_presenter.deviceEx == nullptr)
    {
        DXLOG("[d3d9ex] CreateDeviceEx FlipEx FAILED adapter=%u %dx%d "
              "hr=0x%08lX",
              static_cast<unsigned>(config.adapter), clientWidth, clientHeight,
              static_cast<unsigned long>(createHr));
        return false;
    }
    g_presenter.device = static_cast<IDirect3DDevice9 *>(g_presenter.deviceEx);
    g_presenter.presentParams = config.params;
    latencyHr = g_presenter.deviceEx->SetMaximumFrameLatency(1u);
    DXLOG("[d3d9ex] CreateDeviceEx FlipEx READY adapter=%u %dx%d "
          "buffers=%u queueCap=1 hr=0x%08lX",
          static_cast<unsigned>(config.adapter), clientWidth, clientHeight,
          static_cast<unsigned>(config.params.BackBufferCount),
          static_cast<unsigned long>(latencyHr));
    return true;
}

bool CreateDevice(HWND window, int clientWidth, int clientHeight,
                   QohPresentationMode mode, QohRendererBackend backend)
{
    LogDeviceWindowGeometry("before-destroy", window, mode, clientWidth, clientHeight);
    RestoreWindowAfterDeviceWork restoreWindow(window, mode);
    DestroyDevice();
    LogDeviceWindowGeometry("after-destroy", window, mode, clientWidth, clientHeight);

    g_presenter.requestedMode = mode;
    g_presenter.requestedBackend = backend;
    g_presenter.fallbackReason[0] = '\0';
    if (backend == QOH_RENDERER_GDI_DIAGNOSTIC)
    {
        g_presenter.effectiveMode = mode == QOH_PRESENTATION_EXCLUSIVE
            ? QOH_PRESENTATION_BORDERLESS : mode;
        g_presenter.effectiveBackend = QOH_RENDERER_GDI_DIAGNOSTIC;
        g_presenter.readiness = QOH_RENDERER_FALLBACK_READY;
        std::snprintf(g_presenter.fallbackReason,
                      sizeof(g_presenter.fallbackReason),
                      "GDI diagnostic backend selected");
        return false;
    }

    const DWORD behaviorFlags = D3DCREATE_FPU_PRESERVE
        | D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
    const bool exRequested = backend == QOH_RENDERER_D3D9EX_FLIPEX;
    if (exRequested && !g_presenter.exDisabledAfterHardFailure
        && mode != QOH_PRESENTATION_EXCLUSIVE)
    {
        PresenterModeConfig exConfig{};
        HRESULT exCreateHr = D3DERR_NOTAVAILABLE;
        HRESULT latencyHr = D3DERR_NOTAVAILABLE;
        if (TryCreateD3D9ExDevice(
                window, clientWidth, clientHeight, mode, behaviorFlags,
                exConfig, exCreateHr, latencyHr))
        {
            LogDeviceWindowGeometry("after-create-ex", window, mode, clientWidth, clientHeight);
            g_presenter.window = window;
            g_presenter.effectiveMode = mode;
            g_presenter.effectiveBackend = QOH_RENDERER_D3D9EX_FLIPEX;
            g_presenter.adapterOrdinal = exConfig.adapter;
            g_presenter.monitor = exConfig.monitor;
            g_presenter.desktopMode = exConfig.desktop;
            LogAdapter(g_presenter.d3d, exConfig.adapter, exConfig.monitor,
                       exConfig.desktop);
            if (!InitializeImGui(window))
            {
                DXLOG("[d3d9ex] ImGui backend init failed; falling back as "
                      "one complete backend");
                exCreateHr = E_FAIL;
            }
            else
            {
                ++g_diag.creates;
                ++g_presenter.resetGeneration;
                g_presenter.readiness = QOH_RENDERER_READY;
                NotifyDeviceObjectsValid();
                return true;
            }
        }
        g_diag.lastError = exCreateHr;
        DestroyDevice();
        g_presenter.requestedMode = mode;
        g_presenter.requestedBackend = backend;
        std::snprintf(g_presenter.fallbackReason,
                      sizeof(g_presenter.fallbackReason),
                      "D3D9Ex FlipEx unavailable/failed (0x%08lX); using Classic D3D9",
                      static_cast<unsigned long>(exCreateHr));
    }
    else if (exRequested)
    {
        std::snprintf(g_presenter.fallbackReason,
                      sizeof(g_presenter.fallbackReason),
                      "%s",
                      g_presenter.exDisabledAfterHardFailure
                          ? "D3D9Ex FlipEx hard-failed; using Classic D3D9 for this selection"
                          : "D3D9Ex FlipEx is windowed-only; using Classic D3D9 for Exclusive");
    }

    g_presenter.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (g_presenter.d3d == nullptr)
    {
        DXLOG("[d3d9] Direct3DCreate9 returned null (no D3D9 runtime); "
              "presentation stays on the GDI path");
        g_presenter.effectiveMode = mode == QOH_PRESENTATION_EXCLUSIVE
            ? QOH_PRESENTATION_BORDERLESS : mode;
        g_presenter.effectiveBackend = QOH_RENDERER_GDI_DIAGNOSTIC;
        g_presenter.readiness = QOH_RENDERER_FALLBACK_READY;
        std::snprintf(g_presenter.fallbackReason,
                      sizeof(g_presenter.fallbackReason),
                      "Direct3DCreate9 unavailable; using GDI");
        return false;
    }

    QohPresentationMode effectiveMode = mode;
    PresenterModeConfig config{};
    if (!BuildModeConfig(g_presenter.d3d, window, clientWidth, clientHeight,
                         effectiveMode, config))
    {
        if (mode != QOH_PRESENTATION_EXCLUSIVE)
        {
            DestroyDevice();
            g_presenter.effectiveMode = mode;
            g_presenter.effectiveBackend = QOH_RENDERER_GDI_DIAGNOSTIC;
            g_presenter.readiness = QOH_RENDERER_FALLBACK_READY;
            std::snprintf(g_presenter.fallbackReason,
                          sizeof(g_presenter.fallbackReason),
                          "target monitor adapter/display mode unavailable; using GDI");
            return false;
        }
        effectiveMode = QOH_PRESENTATION_BORDERLESS;
        std::snprintf(g_presenter.fallbackReason,
                      sizeof(g_presenter.fallbackReason),
                      "Exclusive adapter did not match the target monitor; using Borderless");
        if (!BuildModeConfig(g_presenter.d3d, window, clientWidth,
                             clientHeight, effectiveMode, config))
        {
            DestroyDevice();
            g_presenter.effectiveMode = QOH_PRESENTATION_BORDERLESS;
            g_presenter.effectiveBackend = QOH_RENDERER_GDI_DIAGNOSTIC;
            g_presenter.readiness = QOH_RENDERER_FALLBACK_READY;
            return false;
        }
    }
    g_presenter.presentParams = config.params;

    HRESULT createHr = g_presenter.d3d->CreateDevice(
        config.adapter,
        D3DDEVTYPE_HAL,
        window,
        behaviorFlags,
        &g_presenter.presentParams,
        &g_presenter.device);
    LogDeviceWindowGeometry("after-create", window, mode, clientWidth, clientHeight);
    DXLOG("[d3d9] CreateDevice hwnd=%p requested=%u effective=%u adapter=%u "
          "%ux%u windowed=%d refresh=%u flags=%08lX hr=0x%08lX",
          static_cast<void *>(window),
          static_cast<unsigned>(mode), static_cast<unsigned>(effectiveMode),
          static_cast<unsigned>(config.adapter),
          static_cast<unsigned>(config.params.BackBufferWidth),
          static_cast<unsigned>(config.params.BackBufferHeight),
          static_cast<int>(config.params.Windowed),
          static_cast<unsigned>(config.params.FullScreen_RefreshRateInHz),
          static_cast<unsigned long>(behaviorFlags),
          static_cast<unsigned long>(createHr));
    if (FAILED(createHr) && effectiveMode == QOH_PRESENTATION_EXCLUSIVE)
    {
        effectiveMode = QOH_PRESENTATION_BORDERLESS;
        std::snprintf(g_presenter.fallbackReason,
                      sizeof(g_presenter.fallbackReason),
                      "Exclusive CreateDevice failed (0x%08lX); using Borderless",
                      static_cast<unsigned long>(createHr));
        if (BuildModeConfig(g_presenter.d3d, window, clientWidth, clientHeight,
                            effectiveMode, config))
        {
            g_presenter.presentParams = config.params;
            createHr = g_presenter.d3d->CreateDevice(
                config.adapter, D3DDEVTYPE_HAL, window, behaviorFlags,
                &g_presenter.presentParams, &g_presenter.device);
        }
    }
    if (FAILED(createHr))
    {
        WLOG_ISSUE("[d3d9] device creation exhausted fallbacks hr=0x%08lX",
                   static_cast<unsigned long>(createHr));
        g_diag.lastError = createHr;
        DestroyDevice();
        g_presenter.effectiveMode = mode == QOH_PRESENTATION_EXCLUSIVE
            ? QOH_PRESENTATION_BORDERLESS : mode;
        g_presenter.effectiveBackend = QOH_RENDERER_GDI_DIAGNOSTIC;
        g_presenter.readiness = QOH_RENDERER_FALLBACK_READY;
        if (g_presenter.fallbackReason[0] == '\0')
            std::snprintf(g_presenter.fallbackReason,
                          sizeof(g_presenter.fallbackReason),
                          "Classic D3D9 CreateDevice failed (0x%08lX); using GDI",
                          static_cast<unsigned long>(createHr));
        return false;
    }

    g_presenter.window = window;
    g_presenter.effectiveMode = effectiveMode;
    g_presenter.effectiveBackend = QOH_RENDERER_CLASSIC_D3D9;
    g_presenter.adapterOrdinal = config.adapter;
    g_presenter.monitor = config.monitor;
    g_presenter.desktopMode = config.desktop;
    LogAdapter(g_presenter.d3d, config.adapter, config.monitor, config.desktop);
    if (!InitializeImGui(window))
    {
        WLOG_ISSUE("[d3d9] ImGui backend init failed on a good device; "
              "destroying it and staying on GDI");
        g_diag.lastError = E_FAIL;
        DestroyDevice();
        g_presenter.effectiveMode = mode == QOH_PRESENTATION_EXCLUSIVE
            ? QOH_PRESENTATION_BORDERLESS : mode;
        g_presenter.effectiveBackend = QOH_RENDERER_GDI_DIAGNOSTIC;
        g_presenter.readiness = QOH_RENDERER_FALLBACK_READY;
        std::snprintf(g_presenter.fallbackReason,
                      sizeof(g_presenter.fallbackReason),
                      "Classic D3D9 ImGui initialization failed; using GDI");
        return false;
    }

    ++g_diag.creates;
    ++g_presenter.resetGeneration;
    g_presenter.readiness = (effectiveMode == mode && !exRequested)
        ? QOH_RENDERER_READY : QOH_RENDERER_FALLBACK_READY;
    NotifyDeviceObjectsValid();
    LogDeviceWindowGeometry("create-return", window, mode, clientWidth, clientHeight);
    return true;
}

bool ResetDevice(int clientWidth, int clientHeight,
                 QohPresentationMode mode, QohRendererBackend backend)
{
    if (g_presenter.device == nullptr)
    {
        return false;
    }

    // Reset fails with D3DERR_INVALIDCALL unless all D3DPOOL_DEFAULT objects are released first, including the training DLL's ImGui backend on this device.
    NotifyDeviceObjectsInvalid();

    PresenterModeConfig config{};
    if (!BuildModeConfig(g_presenter.d3d, g_presenter.window, clientWidth,
                         clientHeight, mode, config)
        || config.adapter != g_presenter.adapterOrdinal)
        return false;
    const bool exActive = g_presenter.deviceEx != nullptr
        && g_presenter.effectiveBackend == QOH_RENDERER_D3D9EX_FLIPEX;
    if (exActive)
    {
        if (mode == QOH_PRESENTATION_EXCLUSIVE)
            return false;
        config.params.Windowed = TRUE;
        config.params.BackBufferWidth = static_cast<UINT>(clientWidth);
        config.params.BackBufferHeight = static_cast<UINT>(clientHeight);
        config.params.BackBufferFormat = D3DFMT_UNKNOWN;
        config.params.BackBufferCount = 2u;
        config.params.SwapEffect = D3DSWAPEFFECT_FLIPEX;
        config.params.FullScreen_RefreshRateInHz = 0u;
    }
    const D3DPRESENT_PARAMETERS previous = g_presenter.presentParams;
    g_presenter.presentParams = config.params;
    const HRESULT resetHr = exActive
        ? g_presenter.deviceEx->ResetEx(&g_presenter.presentParams, nullptr)
        : g_presenter.device->Reset(&g_presenter.presentParams);
    DXLOG("[%s] %s %ux%u mode=%u -> %ux%u windowed=%d refresh=%u "
          "adapter=%u hr=0x%08lX",
          exActive ? "d3d9ex" : "d3d9",
          exActive ? "ResetEx" : "Reset",
          static_cast<unsigned>(previous.BackBufferWidth),
          static_cast<unsigned>(previous.BackBufferHeight),
          static_cast<unsigned>(mode),
          static_cast<unsigned>(config.params.BackBufferWidth),
          static_cast<unsigned>(config.params.BackBufferHeight),
          static_cast<int>(config.params.Windowed),
          static_cast<unsigned>(config.params.FullScreen_RefreshRateInHz),
          static_cast<unsigned>(config.adapter),
          static_cast<unsigned long>(resetHr));
    if (FAILED(resetHr))
    {
        g_presenter.presentParams = previous;
        g_presenter.deviceLost = true;
        if (exActive)
            g_presenter.exDisabledAfterHardFailure = true;
        g_diag.lastError = resetHr;
        return false;
    }

    g_presenter.presentParams = config.params;
    ++g_diag.resets;
    ++g_presenter.resetGeneration;
    g_presenter.requestedMode = mode;
    g_presenter.effectiveMode = mode;
    g_presenter.requestedBackend = backend;
    g_presenter.effectiveBackend = exActive
        ? QOH_RENDERER_D3D9EX_FLIPEX : QOH_RENDERER_CLASSIC_D3D9;
    g_presenter.adapterOrdinal = config.adapter;
    g_presenter.monitor = config.monitor;
    g_presenter.desktopMode = config.desktop;
    g_presenter.readiness = backend == g_presenter.effectiveBackend
        ? QOH_RENDERER_READY : QOH_RENDERER_FALLBACK_READY;
    NotifyDeviceObjectsValid();
    g_presenter.deviceLost = false;
    return true;
}

bool EnsureFrameTexture(int width, int height)
{
    if (g_presenter.device == nullptr || width <= 0 || height <= 0)
    {
        g_diag.lastError = g_presenter.device == nullptr ? E_POINTER : E_INVALIDARG;
        return false;
    }

    if (g_presenter.frameTexture != nullptr && g_presenter.textureWidth == width && g_presenter.textureHeight == height)
    {
        return true;
    }

    ReleaseFrameTexture();
    const bool exActive = g_presenter.deviceEx != nullptr
        && g_presenter.effectiveBackend == QOH_RENDERER_D3D9EX_FLIPEX;
    const HRESULT createHr = g_presenter.device->CreateTexture(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        1,
        exActive ? D3DUSAGE_DYNAMIC : 0u,
        D3DFMT_A8R8G8B8,
        exActive ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
        &g_presenter.frameTexture,
        nullptr);
    if (FAILED(createHr))
    {
        DXLOG("EnsureFrameTexture: CreateTexture %dx%d A8R8G8B8/%s "
              "FAILED hr=0x%08lX", width, height,
              exActive ? "DEFAULT+DYNAMIC" : "MANAGED",
              static_cast<unsigned long>(createHr));
        g_diag.lastError = createHr;
        return false;
    }

    g_presenter.textureWidth = width;
    g_presenter.textureHeight = height;
    return true;
}

bool EnsureScaleTexture(int width, int height)
{
    if (g_presenter.device == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }
    if (g_presenter.scaleTexture != nullptr && g_presenter.scaleTextureW == width && g_presenter.scaleTextureH == height)
    {
        return true;
    }
    ReleaseScaleTexture();
    const HRESULT createHr = g_presenter.device->CreateTexture(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        1,
        D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &g_presenter.scaleTexture,
        nullptr);
    if (FAILED(createHr))
    {
        DXLOG("EnsureScaleTexture: CreateTexture %dx%d A8R8G8B8/"
              "RENDERTARGET/DEFAULT FAILED hr=0x%08lX - falling back to "
              "unscaled 1x presentation", width, height,
              static_cast<unsigned long>(createHr));
        g_presenter.scaleTexture = nullptr;
        return false;
    }
    g_presenter.scaleTextureW = width;
    g_presenter.scaleTextureH = height;
    return true;
}

#define PALLOG(...) DXLOG(__VA_ARGS__)

struct PalettePresentDiag
{
    volatile LONG frames = 0;
    volatile LONG indexedFrames = 0;
    volatile LONG paletteFrames = 0;
    volatile LONG overrideFrames = 0;
    volatile LONG lastBpp = 0;
    volatile LONG lastFormatKind = -1;
};
PalettePresentDiag g_palettePresentDiag;

bool PaletteApplyLiveOverride(RGBQUAD *palette);

const std::uint16_t *PaletteFrameRemap();

void PaletteNoteCanonicalFrame();

void PaletteAutoTick(bool appActive);

bool UploadFrame(const EfzDx9PresentParams &params)
{
    if (g_presenter.frameTexture == nullptr || params.pixels == nullptr)
    {
        g_diag.lastError = E_POINTER;
        return false;
    }

    D3DLOCKED_RECT lockedRect = {};
    const DWORD lockFlags = g_presenter.deviceEx != nullptr
        && g_presenter.effectiveBackend == QOH_RENDERER_D3D9EX_FLIPEX
            ? D3DLOCK_DISCARD : 0u;
    const HRESULT lockHr =
        g_presenter.frameTexture->LockRect(
            0, &lockedRect, nullptr, lockFlags);
    if (FAILED(lockHr))
    {
        DXLOG("UploadFrame: LockRect FAILED hr=0x%08lX (%dx%d bpp=%d)",
              static_cast<unsigned long>(lockHr), params.width, params.height,
              params.bytesPerPixel);
        g_diag.lastError = lockHr;
        return false;
    }

    const bool sourceIsPacked16 = params.bytesPerPixel == 2;
    if ((sourceIsPacked16 && !ddw_surface_format_is_presentable(&params.format))
        || (!sourceIsPacked16
            && params.format.kind != DDW_PIXEL_FORMAT_INDEX8))
    {
        DXLOG("UploadFrame: source format not presentable - bpp=%d kind=%d "
              "masks=R%08lX/G%08lX/B%08lX/A%08lX (%dx%d)",
              params.bytesPerPixel, static_cast<int>(params.format.kind),
              static_cast<unsigned long>(params.format.red_mask),
              static_cast<unsigned long>(params.format.green_mask),
              static_cast<unsigned long>(params.format.blue_mask),
              static_cast<unsigned long>(params.format.alpha_mask),
              params.width, params.height);
        g_diag.lastError = E_INVALIDARG;
        g_presenter.frameTexture->UnlockRect(0);
        return false;
    }
    if (params.presentationKind == EFZ_DX9_PRESENT_CANONICAL)
    {
        PaletteNoteCanonicalFrame();
    }
    PaletteAutoTick(params.appActive != FALSE);
    const std::uint16_t *const frameRemap = PaletteFrameRemap();
    InterlockedIncrement(&g_palettePresentDiag.frames);
    InterlockedExchange(&g_palettePresentDiag.lastBpp, params.bytesPerPixel);
    InterlockedExchange(&g_palettePresentDiag.lastFormatKind,
                        static_cast<LONG>(params.format.kind));
    const RGBQUAD *activePalette = params.palette;
    RGBQUAD patchedPalette[256];
    if (!sourceIsPacked16)
    {
        InterlockedIncrement(&g_palettePresentDiag.indexedFrames);
    }
    if (activePalette != nullptr)
    {
        InterlockedIncrement(&g_palettePresentDiag.paletteFrames);
        std::memcpy(patchedPalette, activePalette, sizeof(patchedPalette));
        if (PaletteApplyLiveOverride(patchedPalette))
        {
            InterlockedIncrement(&g_palettePresentDiag.overrideFrames);
            activePalette = patchedPalette;
        }
    }
    for (int y = 0; y < params.height; ++y)
    {
        const BYTE *srcRow = params.pixels + static_cast<size_t>(y) * static_cast<size_t>(params.pitch);
        auto *dstRow = reinterpret_cast<std::uint32_t *>(static_cast<BYTE *>(lockedRect.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lockedRect.Pitch));
        if (sourceIsPacked16)
        {
            for (int x = 0; x < params.width; ++x)
            {
                std::uint16_t v = 0;
                DdwRgb8 decoded = {};
                std::memcpy(&v, srcRow + static_cast<size_t>(x) * 2u, sizeof(v));
                if (frameRemap != nullptr) v = frameRemap[v];
                if (!ddw_surface_format_decode_rgb(&params.format, v, &decoded))
                {
                    WLOG_ISSUE("UploadFrame: pixel decode FAILED at (%d,%d) raw=0x%04X "
                          "kind=%d masks=R%08lX/G%08lX/B%08lX/A%08lX (%dx%d)",
                          x, y, static_cast<unsigned>(v),
                          static_cast<int>(params.format.kind),
                          static_cast<unsigned long>(params.format.red_mask),
                          static_cast<unsigned long>(params.format.green_mask),
                          static_cast<unsigned long>(params.format.blue_mask),
                          static_cast<unsigned long>(params.format.alpha_mask),
                          params.width, params.height);
                    g_diag.lastError = E_FAIL;
                    g_presenter.frameTexture->UnlockRect(0);
                    return false;
                }
                dstRow[x] = 0xFF000000u
                    | (static_cast<std::uint32_t>(decoded.red) << 16u)
                    | (static_cast<std::uint32_t>(decoded.green) << 8u)
                    | static_cast<std::uint32_t>(decoded.blue);
            }
            continue;
        }
        for (int x = 0; x < params.width; ++x)
        {
            const BYTE index = srcRow[x];
            BYTE red = index;
            BYTE green = index;
            BYTE blue = index;
            if (activePalette != nullptr)
            {
                const RGBQUAD &color = activePalette[index];
                red = color.rgbRed;
                green = color.rgbGreen;
                blue = color.rgbBlue;
            }
            dstRow[x] = 0xFF000000u
                | (static_cast<std::uint32_t>(red) << 16u)
                | (static_cast<std::uint32_t>(green) << 8u)
                | static_cast<std::uint32_t>(blue);
        }
    }

    const HRESULT unlockHr = g_presenter.frameTexture->UnlockRect(0);
    if (FAILED(unlockHr))
    {
        DXLOG("UploadFrame: UnlockRect FAILED hr=0x%08lX (%dx%d bpp=%d)",
              static_cast<unsigned long>(unlockHr), params.width, params.height,
              params.bytesPerPixel);
        g_diag.lastError = unlockHr;
        return false;
    }
    return true;
}

float ClampFloat(float value, float lo, float hi)
{
    return (std::max)(lo, (std::min)(hi, value));
}

float GameToScreenX(const EfzDx9PresentParams &params, float virtualX)
{
    return static_cast<float>(params.dstX)
        + virtualX * static_cast<float>(params.dstW) / 640.0f;
}

float GameToScreenY(const EfzDx9PresentParams &params, float virtualY)
{
    return static_cast<float>(params.dstY)
        + virtualY * static_cast<float>(params.dstH) / 480.0f;
}

float GameScale(const EfzDx9PresentParams &params)
{
    if (params.dstW <= 0 || params.dstH <= 0)
    {
        return 1.0f;
    }
    return ClampFloat(
        (std::min)(static_cast<float>(params.dstW) / 640.0f, static_cast<float>(params.dstH) / 480.0f),
        0.75f,
        3.0f);
}

ImVec2 MeasureText(ImFont *font, float fontSize, const char *text)
{
    if (font == nullptr || text == nullptr || text[0] == '\0')
    {
        return ImVec2(0.0f, 0.0f);
    }
    return font->CalcTextSizeA(fontSize, 1000000.0f, 0.0f, text);
}

std::string ClipTextToWidth(ImFont *font, float fontSize, const char *text, float maxWidth)
{
    if (font == nullptr || text == nullptr || text[0] == '\0' || maxWidth <= 0.0f)
    {
        return {};
    }

    std::string clipped(text);
    while (!clipped.empty() && MeasureText(font, fontSize, clipped.c_str()).x > maxWidth)
    {
        clipped.pop_back();
    }

    if (clipped.size() < std::strlen(text) && maxWidth > fontSize * 2.5f)
    {
        while (!clipped.empty() && MeasureText(font, fontSize, (clipped + "...").c_str()).x > maxWidth)
        {
            clipped.pop_back();
        }
        clipped += "...";
    }
    return clipped;
}

void DrawShadowedText(ImDrawList *drawList, ImFont *font, float fontSize, ImVec2 pos, ImU32 color, const char *text)
{
    if (drawList == nullptr || font == nullptr || text == nullptr || text[0] == '\0')
    {
        return;
    }

    drawList->AddText(font, fontSize, ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 180), text);
    drawList->AddText(font, fontSize, pos, color, text);
}

float DrawCompactHudLine(
    ImDrawList *drawList,
    ImFont *font,
    float fontSize,
    float scale,
    float anchorX,
    float topY,
    float maxWidth,
    bool rightAlign,
    const char *text,
    ImU32 textColor)
{
    if (drawList == nullptr || font == nullptr || text == nullptr || text[0] == '\0')
    {
        return topY;
    }

    const float padX = 5.0f * scale;
    const float padY = 3.0f * scale;
    const float rounding = 3.0f * scale;
    const std::string clipped = ClipTextToWidth(font, fontSize, text, (std::max)(1.0f, maxWidth - padX * 2.0f));
    if (clipped.empty())
    {
        return topY;
    }

    const ImVec2 textSize = MeasureText(font, fontSize, clipped.c_str());
    const float boxWidth = (std::min)(maxWidth, textSize.x + padX * 2.0f);
    const float boxHeight = fontSize + padY * 2.0f;
    const float left = rightAlign ? (anchorX - boxWidth) : anchorX;
    const float right = left + boxWidth;
    const float bottom = topY + boxHeight;

    drawList->AddRectFilled(ImVec2(left, topY), ImVec2(right, bottom), IM_COL32(0, 0, 0, 150), rounding);
    drawList->AddRect(ImVec2(left, topY), ImVec2(right, bottom), IM_COL32(72, 72, 72, 180), rounding);

    float textX = left + padX;
    if (rightAlign)
    {
        textX = right - padX - textSize.x;
    }
    DrawShadowedText(drawList, font, fontSize, ImVec2(textX, topY + padY), textColor, clipped.c_str());
    return bottom;
}

float DrawCenteredHudLine(
    ImDrawList *drawList,
    ImFont *font,
    float fontSize,
    float scale,
    float centerX,
    float topY,
    float maxWidth,
    const char *text,
    ImU32 textColor)
{
    if (drawList == nullptr || font == nullptr || text == nullptr || text[0] == '\0')
    {
        return topY;
    }
    const float padX = 5.0f * scale;
    const std::string clipped = ClipTextToWidth(
        font, fontSize, text, (std::max)(1.0f, maxWidth - padX * 2.0f));
    if (clipped.empty())
    {
        return topY;
    }
    const ImVec2 textSize = MeasureText(font, fontSize, clipped.c_str());
    const float boxWidth = (std::min)(maxWidth, textSize.x + padX * 2.0f);
    return DrawCompactHudLine(
        drawList, font, fontSize, scale, centerX + boxWidth * 0.5f, topY,
        maxWidth, true, text, textColor);
}

constexpr int kHudDefFontScalePct = 107;
constexpr int kMenuDefFontScalePct = 135;
constexpr int kMenuMinFontScalePct = 75;
constexpr int kMenuMaxFontScalePct = 200;
constexpr int kHudDefP1X = 0,    kHudDefP1Y = 13;
constexpr int kHudDefP2X = 640,  kHudDefP2Y = 13;
constexpr int kHudDefSessionX = 341, kHudDefSessionY = 462;
constexpr int kHudDefHeaderX = 313,  kHudDefHeaderY = 112;

struct HudLayout
{
    int fontScalePct = kHudDefFontScalePct;
    int p1x = -1, p1y = -1;
    int p2x = -1, p2y = -1;
    int sx  = -1, sy  = -1;
    int hx  = -1, hy  = -1;
};

struct OverlayUiSettings
{
    int menuFontScalePct = kMenuDefFontScalePct;
};

HudLayout g_hudLayout = {};
OverlayUiSettings g_overlayUiSettings = {};
bool g_hudLayoutLoaded = false;
bool g_hudPreview = false;
char g_hudPreviewPlayerOne[64] =
    "\xE3\x83\x97\xE3\x83\xAC\xE3\x82\xA4\xE3\x83\xA4\xE3\x83\xBC";
char g_hudPreviewPlayerTwo[64] =
    "\xD0\x98\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA";

const char *HudIniPath()
{
    static char path[MAX_PATH] = {};
    if (path[0] != '\0')
    {
        return path;
    }
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&HudIniPath), &self);
    char modulePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(self, modulePath, MAX_PATH);
    const char *const kIniName = "ddraw_wrapper.ini";
    const char *slash = (n != 0 && n < MAX_PATH) ? std::strrchr(modulePath, '\\') : nullptr;
    if (slash != nullptr)
    {
        const int dirLen = static_cast<int>(slash - modulePath) + 1;
        std::snprintf(path, sizeof(path), "%.*s%s", dirLen, modulePath, kIniName);
    }
    else
    {
        std::snprintf(path, sizeof(path), "%s", kIniName);
    }
    return path;
}

int HudIniGet(const char *key, int def)
{
    return static_cast<int>(GetPrivateProfileIntA("NetplayHud", key, def, HudIniPath()));
}

int OverlayIniGet(const char *key, int def)
{
    return static_cast<int>(GetPrivateProfileIntA("Overlay", key, def, HudIniPath()));
}

void HudIniSet(const char *key, int value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA("NetplayHud", key, buf, HudIniPath());
}

void OverlayIniSet(const char *key, int value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA("Overlay", key, buf, HudIniPath());
}

void HudLayoutLoad()
{
    qi18n::setLanguageFromIniValue(OverlayIniGet("Language", 0));
    g_overlayUiSettings.menuFontScalePct = OverlayIniGet(
        "ImGuiFontScalePct", kMenuDefFontScalePct);
    g_overlayUiSettings.menuFontScalePct = (std::max)(
        kMenuMinFontScalePct,
        (std::min)(kMenuMaxFontScalePct,
                   g_overlayUiSettings.menuFontScalePct));
    g_hudLayout.fontScalePct = HudIniGet("FontScalePct", kHudDefFontScalePct);
    g_hudLayout.p1x = HudIniGet("P1NameX", -1);
    g_hudLayout.p1y = HudIniGet("P1NameY", -1);
    g_hudLayout.p2x = HudIniGet("P2NameX", -1);
    g_hudLayout.p2y = HudIniGet("P2NameY", -1);
    g_hudLayout.sx = HudIniGet("SessionX", -1);
    g_hudLayout.sy = HudIniGet("SessionY", -1);
    g_hudLayout.hx = HudIniGet("HeaderX", -1);
    g_hudLayout.hy = HudIniGet("HeaderY", -1);
    g_hudLayout.fontScalePct = (std::max)(50, (std::min)(300, g_hudLayout.fontScalePct));
    OverlayIniSet("ImGuiFontScalePct", g_overlayUiSettings.menuFontScalePct);
    OverlayIniSet("PaletteAutoLoad", OverlayIniGet("PaletteAutoLoad", 1));
    OverlayIniSet("PaletteToggleKey",
                  OverlayIniGet("PaletteToggleKey", VK_TAB));
    HudIniSet("FontScalePct", g_hudLayout.fontScalePct);
    HudIniSet("P1NameX", g_hudLayout.p1x);
    HudIniSet("P1NameY", g_hudLayout.p1y);
    HudIniSet("P2NameX", g_hudLayout.p2x);
    HudIniSet("P2NameY", g_hudLayout.p2y);
    HudIniSet("SessionX", g_hudLayout.sx);
    HudIniSet("SessionY", g_hudLayout.sy);
    HudIniSet("HeaderX", g_hudLayout.hx);
    HudIniSet("HeaderY", g_hudLayout.hy);
    if (g_presenter.imguiContext != nullptr)
    {
        ImGui::GetIO().FontGlobalScale =
            static_cast<float>(g_overlayUiSettings.menuFontScalePct) / 100.0f;
    }
    Qoh99WrapperRefreshIniComments();
    g_hudLayoutLoaded = true;
}

void HudLayoutEnsureLoaded()
{
    if (!g_hudLayoutLoaded)
    {
        HudLayoutLoad();
    }
}

float HudResolveX(const EfzDx9PresentParams &params, int stored, float def)
{
    return GameToScreenX(params, stored < 0 ? def : static_cast<float>(stored));
}

float HudResolveY(const EfzDx9PresentParams &params, int stored, float def)
{
    return GameToScreenY(params, stored < 0 ? def : static_cast<float>(stored));
}

void DrawNetplayHud(const EfzDx9PresentParams &params)
{
    EfzDx9NetplayHudState hud = {};
    hud.structSize = sizeof(hud);
    const bool haveLive =
        g_presenter.hudProvider != nullptr
        && g_presenter.hudProvider(&hud, g_presenter.hudProviderUserData) != FALSE
        && hud.visible != FALSE;
    if (!haveLive)
    {
        if (!(g_presenter.menuVisible && g_hudPreview))
        {
            return;
        }
        hud = {};
        hud.structSize = sizeof(hud);
        hud.visible = TRUE;
        std::snprintf(hud.playerOneName, sizeof(hud.playerOneName), "%s (0)",
                      g_hudPreviewPlayerOne);
        std::snprintf(hud.playerTwoName, sizeof(hud.playerTwoName), "%s (0)",
                      g_hudPreviewPlayerTwo);
        std::snprintf(hud.sessionDetail, sizeof(hud.sessionDetail),
                      "ping 42 ms   delay 3   rollback 2");
    }

    HudLayoutEnsureLoaded();

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr)
    {
        return;
    }

    const float scale = GameScale(params);
    const float userScale = static_cast<float>(
        g_hudLayout.fontScalePct > 0 ? g_hudLayout.fontScalePct : 100) / 100.0f;
    const float fontSize = font->FontSize * scale * userScale;
    const float lineGap = 2.0f * scale;
    const float groupGap = 6.0f * scale;
    const float statusMaxWidth = 186.0f * scale;
    const float toastMaxWidth = 220.0f * scale;
    const float leftAnchor = GameToScreenX(params, 16.0f);
    const float rightAnchor = GameToScreenX(params, 624.0f);
    const float topY = GameToScreenY(params, 136.0f);

    const float nameMaxWidth = 150.0f * scale;
    if (hud.playerOneName[0] != '\0')
    {
        DrawCompactHudLine(
            drawList, font, fontSize, scale,
            HudResolveX(params, g_hudLayout.p1x, static_cast<float>(kHudDefP1X)),
            HudResolveY(params, g_hudLayout.p1y, static_cast<float>(kHudDefP1Y)),
            nameMaxWidth, false, hud.playerOneName,
            IM_COL32(232, 244, 255, 255));
    }
    if (hud.playerTwoName[0] != '\0')
    {
        DrawCompactHudLine(
            drawList, font, fontSize, scale,
            HudResolveX(params, g_hudLayout.p2x, static_cast<float>(kHudDefP2X)),
            HudResolveY(params, g_hudLayout.p2y, static_cast<float>(kHudDefP2Y)),
            nameMaxWidth, true, hud.playerTwoName,
            IM_COL32(232, 244, 255, 255));
    }
    if (hud.matchHeader[0] != '\0')
    {
        DrawCenteredHudLine(
            drawList, font, fontSize, scale,
            HudResolveX(params, g_hudLayout.hx, static_cast<float>(kHudDefHeaderX)),
            HudResolveY(params, g_hudLayout.hy, static_cast<float>(kHudDefHeaderY)), 360.0f * scale,
            hud.matchHeader, IM_COL32(232, 244, 255, 255));
    }
    if (hud.sessionDetail[0] != '\0')
    {
        DrawCenteredHudLine(
            drawList, font, fontSize, scale,
            HudResolveX(params, g_hudLayout.sx, static_cast<float>(kHudDefSessionX)),
            HudResolveY(params, g_hudLayout.sy, static_cast<float>(kHudDefSessionY)), 300.0f * scale,
            hud.sessionDetail, IM_COL32(232, 244, 255, 255));
    }

    float rightY = topY;
    const int debugCount = (std::max)(0, (std::min)(hud.debugLineCount, EFZ_DX9_NETPLAY_HUD_MAX_LINES));
    for (int i = 0; i < debugCount; ++i)
    {
        rightY = DrawCompactHudLine(
            drawList, font, fontSize, scale, rightAnchor, rightY, statusMaxWidth,
            true, hud.debugLines[i], IM_COL32(244, 244, 244, 245)) + lineGap;
    }

    float leftY = topY;
    const int practiceCount = (std::max)(0, (std::min)(hud.practiceLineCount, EFZ_DX9_NETPLAY_HUD_MAX_LINES));
    for (int i = 0; i < practiceCount; ++i)
    {
        leftY = DrawCompactHudLine(
            drawList, font, fontSize, scale, leftAnchor, leftY, statusMaxWidth,
            false, hud.practiceLines[i], IM_COL32(230, 230, 230, 245)) + lineGap;
    }

    const int toastCount = (std::max)(0, (std::min)(hud.toastLineCount, EFZ_DX9_NETPLAY_HUD_MAX_LINES));
    if (toastCount > 0)
    {
        const float lineHeight = fontSize + 6.0f * scale;
        const float toastBlockHeight = static_cast<float>(toastCount) * lineHeight
            + static_cast<float>((std::max)(0, toastCount - 1)) * lineGap;
        float toastY = (std::max)(
            leftY + groupGap,
            GameToScreenY(params, 480.0f - 54.0f) - toastBlockHeight);
        for (int i = 0; i < toastCount; ++i)
        {
            toastY = DrawCompactHudLine(
                drawList, font, fontSize, scale, leftAnchor, toastY, toastMaxWidth,
                false, hud.toastLines[i], IM_COL32(236, 236, 236, 245)) + lineGap;
        }
    }

    if (hud.rematchPromptActive != FALSE)
    {
        ImDrawList *drawList = ImGui::GetForegroundDrawList();
        ImFont *font = ImGui::GetFont();
        const float scale = params.dstH > 0 ? params.dstH / 480.0f : 1.0f;
        const float fontSize = (std::max)(12.0f, 16.0f * scale);
        const float yesCx = GameToScreenX(params, 124.0f);
        const float noCx  = GameToScreenX(params, 204.0f);
        const float caretY = GameToScreenY(params, 224.0f);
        const float tagY   = GameToScreenY(params, 224.0f + 18.0f);
        const bool p1No = hud.rematchP1No != FALSE;
        const bool p2No = hud.rematchP2No != FALSE;
        const ImU32 dim    = IM_COL32(220, 220, 220, 140);
        const ImU32 locked = IM_COL32(255, 235, 120, 255);
        const ImU32 caretC = IM_COL32(236, 236, 236, 220);
        auto textW = [&](const char *t) {
            return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, t).x;
        };
        auto centred = [&](float cx, float y, const char *t, ImU32 c) {
            drawList->AddText(font, fontSize,
                              ImVec2(cx - textW(t) * 0.5f, y), c, t);
        };
        if (!p1No || !p2No) centred(yesCx, caretY, "^", caretC);
        if (p1No || p2No)   centred(noCx,  caretY, "^", caretC);
        const char *p1t = hud.rematchP1Ready != FALSE ? "P1*" : "P1";
        const char *p2t = hud.rematchP2Ready != FALSE ? "P2*" : "P2";
        const ImU32 p1c = hud.rematchP1Ready != FALSE ? locked : dim;
        const ImU32 p2c = hud.rematchP2Ready != FALSE ? locked : dim;
        const float pad = 6.0f * scale;
        if (p1No == p2No)
        {
            const float cx = p1No ? noCx : yesCx;
            const float w1 = textW(p1t), w2 = textW(p2t);
            const float total = w1 + pad + w2;
            drawList->AddText(font, fontSize,
                              ImVec2(cx - total * 0.5f, tagY), p1c, p1t);
            drawList->AddText(font, fontSize,
                              ImVec2(cx - total * 0.5f + w1 + pad, tagY),
                              p2c, p2t);
        }
        else
        {
            centred(p1No ? noCx : yesCx, tagY, p1t, p1c);
            centred(p2No ? noCx : yesCx, tagY, p2t, p2c);
        }
    }
}

ImU32 ArgbToImColor(DWORD color)
{
    const int a = static_cast<int>((color >> 24u) & 0xFFu);
    const int r = static_cast<int>((color >> 16u) & 0xFFu);
    const int g = static_cast<int>((color >> 8u) & 0xFFu);
    const int b = static_cast<int>(color & 0xFFu);
    return IM_COL32(r, g, b, a);
}

float OverlayScale(const EfzDx9PresentParams &params, const EfzDx9OverlayFrame &frame)
{
    const float virtualWidth = frame.virtualWidth > 0.0f ? frame.virtualWidth : 640.0f;
    const float virtualHeight = frame.virtualHeight > 0.0f ? frame.virtualHeight : 480.0f;
    if (params.dstW <= 0 || params.dstH <= 0)
    {
        return 1.0f;
    }
    return ClampFloat(
        (std::min)(static_cast<float>(params.dstW) / virtualWidth, static_cast<float>(params.dstH) / virtualHeight),
        0.5f,
        6.0f);
}

ImVec2 OverlayToScreen(const EfzDx9PresentParams &params, const EfzDx9OverlayFrame &frame, float x, float y)
{
    const float virtualWidth = frame.virtualWidth > 0.0f ? frame.virtualWidth : 640.0f;
    const float virtualHeight = frame.virtualHeight > 0.0f ? frame.virtualHeight : 480.0f;
    return ImVec2(
        static_cast<float>(params.dstX) + x * static_cast<float>(params.dstW) / virtualWidth,
        static_cast<float>(params.dstY) + y * static_cast<float>(params.dstH) / virtualHeight);
}

void DrawOverlayCommandText(
    ImDrawList *drawList,
    ImFont *font,
    const EfzDx9PresentParams &params,
    const EfzDx9OverlayFrame &frame,
    const EfzDx9OverlayCommand &command,
    float scale)
{
    if (command.text[0] == '\0')
    {
        return;
    }

    const float fontSize =
        (command.fontSize > 0.0f ? command.fontSize : font->FontSize) * scale;
    const float maxWidth = command.w > 0.0f ? command.w * scale : 0.0f;
    const std::string clipped = maxWidth > 0.0f
        ? ClipTextToWidth(font, fontSize, command.text, maxWidth)
        : std::string(command.text);
    if (clipped.empty())
    {
        return;
    }

    ImVec2 pos = OverlayToScreen(params, frame, command.x, command.y);
    const ImVec2 textSize = MeasureText(font, fontSize, clipped.c_str());
    if ((command.flags & EFZ_DX9_OVERLAY_TEXT_ALIGN_CENTER) != 0u)
    {
        pos.x -= textSize.x * 0.5f;
    }
    else if ((command.flags & EFZ_DX9_OVERLAY_TEXT_ALIGN_RIGHT) != 0u)
    {
        pos.x -= textSize.x;
    }

    if ((command.flags & EFZ_DX9_OVERLAY_TEXT_SHADOW) != 0u)
    {
        DrawShadowedText(drawList, font, fontSize, pos, ArgbToImColor(command.color), clipped.c_str());
        return;
    }

    drawList->AddText(font, fontSize, pos, ArgbToImColor(command.color), clipped.c_str());
}

void DrawGenericOverlay(const EfzDx9PresentParams &params)
{
    if (g_presenter.overlayProvider == nullptr)
    {
        return;
    }

    EfzDx9OverlayFrame frame = {};
    frame.structSize = sizeof(frame);
    if (!g_presenter.overlayProvider(&frame, g_presenter.overlayProviderUserData) || !frame.visible)
    {
        return;
    }

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr)
    {
        return;
    }

    OverlayFilterSettingsEnsure();
    const int commandCount = (std::max)(0, (std::min)(frame.commandCount, EFZ_DX9_OVERLAY_MAX_COMMANDS));
    const float scale = OverlayScale(params, frame);
    const ImVec2 clipMin(static_cast<float>(params.dstX), static_cast<float>(params.dstY));
    const ImVec2 clipMax(static_cast<float>(params.dstX + params.dstW), static_cast<float>(params.dstY + params.dstH));
    drawList->PushClipRect(clipMin, clipMax, true);

    for (int i = 0; i < commandCount; ++i)
    {
        const EfzDx9OverlayCommand &command = frame.commands[i];
        if (command.structSize < sizeof(EfzDx9OverlayCommand))
        {
            continue;
        }

        /* Box meta: owner in bits 8..9 (1 = P1, 2 = P2), kind in bits 12..15 (HitboxBoxKind + 1; 8 = axis line). Commands without these bits are never filtered. */
        const unsigned metaOwner = (command.flags >> 8) & 0x3u;
        const unsigned metaKind = (command.flags >> 12) & 0xFu;
        if ((metaOwner == 1u || metaOwner == 2u) && metaKind != 0u)
        {
            const unsigned bit = metaKind == 8u
                ? 7u : (metaKind <= 7u ? metaKind - 1u : 8u);
            if (bit < 8u
                && (g_hitboxKindMask[metaOwner - 1u] & (1u << bit)) == 0u)
            {
                continue;
            }
        }

        const ImVec2 p0 = OverlayToScreen(params, frame, command.x, command.y);
        const ImVec2 p1 = OverlayToScreen(params, frame, command.x + command.w, command.y + command.h);
        switch (command.type)
        {
        case EFZ_DX9_OVERLAY_COMMAND_TEXT:
            DrawOverlayCommandText(drawList, font, params, frame, command, scale);
            break;
        case EFZ_DX9_OVERLAY_COMMAND_RECT_FILLED:
            drawList->AddRectFilled(p0, p1, ArgbToImColor(command.color));
            break;
        case EFZ_DX9_OVERLAY_COMMAND_RECT:
            drawList->AddRect(p0, p1, ArgbToImColor(command.color), 0.0f, 0, (std::max)(1.0f, command.thickness * scale));
            break;
        default:
            break;
        }
    }

    drawList->PopClipRect();
}

struct InputDisplayCompass
{
    float c;
    float s;
};

const InputDisplayCompass kInputDisplayCompass[8] = {
    {  1.0000f,  0.0000f },
    {  0.7071f,  0.7071f },
    {  0.0000f,  1.0000f },
    { -0.7071f,  0.7071f },
    { -1.0000f,  0.0000f },
    { -0.7071f, -0.7071f },
    {  0.0000f, -1.0000f },
    {  0.7071f, -0.7071f },
};

const ImVec2 kInputDisplayArrowShaft[4] = {
    { -0.40f, -0.135f }, {  0.06f, -0.135f },
    {  0.06f,  0.135f }, { -0.40f,  0.135f },
};
const ImVec2 kInputDisplayArrowHead[3] = {
    {  0.48f,  0.000f },
    {  0.04f, -0.320f },
    {  0.04f,  0.320f },
};

ImVec2 InputDisplayGlyphPoint(
    ImVec2 centre,
    const InputDisplayCompass &dir,
    ImVec2 unit,
    float size,
    float inflate)
{
    const float px = unit.x * inflate;
    const float py = unit.y * inflate;
    return ImVec2(centre.x + size * (px * dir.c - py * dir.s),
                  centre.y + size * (px * dir.s + py * dir.c));
}

void DrawInputDisplayArrow(
    ImDrawList *drawList, ImVec2 centre, DWORD compassIndex, float cell)
{
    if (compassIndex >= 8u)
    {
        return;
    }
    const InputDisplayCompass &dir = kInputDisplayCompass[compassIndex];
    const float size = cell * 0.92f;
    const float outline = (std::max)(1.0f, std::floor(0.07f * cell + 0.5f));
    const float inflate = (size > 0.0f) ? (1.0f + (2.0f * outline) / size) : 1.0f;

    for (int pass = 0; pass < 2; ++pass)
    {
        const float k = (pass == 0) ? inflate : 1.0f;
        const ImU32 color =
            (pass == 0) ? IM_COL32(0, 0, 0, 215) : IM_COL32(252, 252, 252, 245);
        drawList->AddQuadFilled(
            InputDisplayGlyphPoint(centre, dir, kInputDisplayArrowShaft[0], size, k),
            InputDisplayGlyphPoint(centre, dir, kInputDisplayArrowShaft[1], size, k),
            InputDisplayGlyphPoint(centre, dir, kInputDisplayArrowShaft[2], size, k),
            InputDisplayGlyphPoint(centre, dir, kInputDisplayArrowShaft[3], size, k),
            color);
        drawList->AddTriangleFilled(
            InputDisplayGlyphPoint(centre, dir, kInputDisplayArrowHead[0], size, k),
            InputDisplayGlyphPoint(centre, dir, kInputDisplayArrowHead[1], size, k),
            InputDisplayGlyphPoint(centre, dir, kInputDisplayArrowHead[2], size, k),
            color);
    }
}

const ImU32 kInputDisplayButtonFill[4] = {
    IM_COL32(84, 156, 232, 240),
    IM_COL32(232, 186, 72, 240),
    IM_COL32(226, 96, 96, 240),
    IM_COL32(120, 206, 138, 240),
};
const char *const kInputDisplayButtonLabel[4] = { "A", "B", "C", "D" };
const DWORD kInputDisplayButtonBit[4] = {
    QOH99_INPUT_LOGICAL_A, QOH99_INPUT_LOGICAL_B,
    QOH99_INPUT_LOGICAL_C, QOH99_INPUT_LOGICAL_D,
};

void DrawInputDisplayButton(
    ImDrawList *drawList,
    ImFont *font,
    ImVec2 centre,
    float cell,
    int buttonIndex,
    bool pressed)
{
    if (buttonIndex < 0 || buttonIndex >= 4)
    {
        return;
    }
    const float pip = cell * 0.82f;
    const float half = pip * 0.5f;
    const ImVec2 p0(centre.x - half, centre.y - half);
    const ImVec2 p1(centre.x + half, centre.y + half);
    const float rounding = pip * 0.30f;

    if (pressed)
    {
        drawList->AddRectFilled(
            p0, p1, kInputDisplayButtonFill[buttonIndex], rounding);
        drawList->AddRect(p0, p1, IM_COL32(0, 0, 0, 225), rounding, 0,
                          (std::max)(1.0f, 0.08f * cell));
    }
    else
    {
        drawList->AddRect(p0, p1, IM_COL32(210, 210, 215, 90), rounding, 0,
                          (std::max)(1.0f, 0.06f * cell));
    }

    if (font == nullptr)
    {
        return;
    }
    const float fontSize = ClampFloat(pip * 0.86f, 8.0f, 48.0f);
    const ImVec2 textSize =
        MeasureText(font, fontSize, kInputDisplayButtonLabel[buttonIndex]);
    const ImVec2 textPos(centre.x - textSize.x * 0.5f,
                         centre.y - textSize.y * 0.5f);
    drawList->AddText(
        font, fontSize, textPos,
        pressed ? IM_COL32(16, 16, 18, 255) : IM_COL32(210, 210, 215, 90),
        kInputDisplayButtonLabel[buttonIndex]);
}

DWORD InputDisplayCompassIndex(DWORD logicalMask)
{
    const bool up = (logicalMask & QOH99_INPUT_LOGICAL_UP) != 0u;
    const bool down = (logicalMask & QOH99_INPUT_LOGICAL_DOWN) != 0u;
    const bool left = (logicalMask & QOH99_INPUT_LOGICAL_LEFT) != 0u;
    const bool right = (logicalMask & QOH99_INPUT_LOGICAL_RIGHT) != 0u;
    const bool holdUp = up && !down;
    const bool holdDown = down && !up;
    const bool holdLeft = left && !right;
    const bool holdRight = right && !left;
    if (holdRight)
    {
        if (holdUp) return 7u;
        if (holdDown) return 1u;
        return 0u;
    }
    if (holdLeft)
    {
        if (holdUp) return 5u;
        if (holdDown) return 3u;
        return 4u;
    }
    if (holdUp) return 6u;
    if (holdDown) return 2u;
    return 0xFFFFFFFFu;
}

bool InputDisplaySnapshotRenderable(const Qoh99InputDisplaySnapshotV1 &snapshot)
{
    if (snapshot.apiVersion != QOH99_INPUT_DISPLAY_API_VERSION)
        return false;
    if (snapshot.structSize < QOH99_INPUT_DISPLAY_SNAPSHOT_V1_BASE_SIZE)
        return false;
    if (snapshot.structSize > sizeof(Qoh99InputDisplaySnapshotV1))
        return false;
    if (snapshot.mode != QOH99_INPUT_DISPLAY_MODE_TRAINING
        && snapshot.mode != QOH99_INPUT_DISPLAY_MODE_REPLAY)
    {
        return false;
    }
    if (snapshot.laneCount == 0u
        || snapshot.laneCount > QOH99_INPUT_DISPLAY_LANES)
    {
        return false;
    }
    if (snapshot.historyCapacity != QOH99_INPUT_DISPLAY_HISTORY)
        return false;
    if (snapshot.holdCap != QOH99_INPUT_DISPLAY_HOLD_CAP)
        return false;
    if (snapshot.mode == QOH99_INPUT_DISPLAY_MODE_TRAINING)
    {
        if (snapshot.drivenLane >= snapshot.laneCount)
            return false;
    }
    else if (snapshot.drivenLane != QOH99_INPUT_DISPLAY_NO_LANE)
    {
        return false;
    }

    bool anyActive = false;
    for (DWORD lane = 0; lane < snapshot.laneCount; ++lane)
    {
        const Qoh99InputDisplayLaneV1 &view = snapshot.lanes[lane];
        if (view.active > 1u)
            return false;
        if (view.entryCount > QOH99_INPUT_DISPLAY_HISTORY)
            return false;
        if (view.head >= QOH99_INPUT_DISPLAY_HISTORY)
            return false;
        if ((view.currentMask & ~QOH99_INPUT_LOGICAL_FIGHT_MASK) != 0u)
            return false;
        for (DWORD i = 0; i < QOH99_INPUT_DISPLAY_HISTORY; ++i)
        {
            if ((view.entries[i].logicalMask
                 & ~QOH99_INPUT_LOGICAL_FIGHT_MASK) != 0u)
            {
                return false;
            }
            if (view.entries[i].heldSteps > QOH99_INPUT_DISPLAY_HOLD_CAP)
                return false;
        }
        if (view.active != 0u)
            anyActive = true;
    }
    return anyActive;
}

DWORD InputDisplayLaneSlotAt(const Qoh99InputDisplayLaneV1 &lane, DWORD ordinal)
{
    if (lane.entryCount == 0u || lane.entryCount > QOH99_INPUT_DISPLAY_HISTORY)
        return QOH99_INPUT_DISPLAY_HISTORY;
    if (lane.head >= QOH99_INPUT_DISPLAY_HISTORY)
        return QOH99_INPUT_DISPLAY_HISTORY;
    if (ordinal >= lane.entryCount)
        return QOH99_INPUT_DISPLAY_HISTORY;
    return (lane.head + QOH99_INPUT_DISPLAY_HISTORY - ordinal)
        % QOH99_INPUT_DISPLAY_HISTORY;
}

struct InputDisplayPanel
{
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    float liveBottom = 0.0f;
    float historyTop = 0.0f;
    float cell = 0.0f;
    float inset = 0.0f;
    float durationColumn = 0.0f;
    int visibleRows = 0;
    bool rightAligned = false;
};

void DrawInputDisplayRow(
    ImDrawList *drawList,
    ImFont *font,
    const InputDisplayPanel &panel,
    float rowTop,
    DWORD logicalMask,
    DWORD heldSteps,
    bool ghosted)
{
    const float cell = panel.cell;
    const float centreY = rowTop + cell * 0.5f;
    const float contentL = panel.left + panel.inset;
    const float contentR = panel.right - panel.inset;

    const float glyphStart = panel.rightAligned
        ? (contentR - cell * 0.5f)
        : (contentL + cell * 0.5f);
    const float step = panel.rightAligned ? -cell : cell;
    const float glyphLimit = panel.rightAligned
        ? (contentL + panel.durationColumn)
        : (contentR - panel.durationColumn);

    float cursor = glyphStart;
    int placed = 0;
    const int kMaxGlyphs = 5;

    const float snappedY = std::floor(centreY) + 0.5f;

    const DWORD compass = InputDisplayCompassIndex(logicalMask);
    {
        const ImVec2 centre(std::floor(cursor) + 0.5f, snappedY);
        if (compass != 0xFFFFFFFFu)
        {
            DrawInputDisplayArrow(drawList, centre, compass, cell);
        }
        cursor += step;
        ++placed;
    }

    for (int b = 0; b < 4; ++b)
    {
        const bool pressed = (logicalMask & kInputDisplayButtonBit[b]) != 0u;
        if (!pressed && !ghosted)
        {
            continue;
        }
        if (placed >= kMaxGlyphs)
        {
            break;
        }
        const bool overrunsDuration = panel.rightAligned
            ? (cursor - cell * 0.5f < glyphLimit)
            : (cursor + cell * 0.5f > glyphLimit);
        if (overrunsDuration)
        {
            break;
        }
        DrawInputDisplayButton(
            drawList, font, ImVec2(std::floor(cursor) + 0.5f, snappedY), cell,
            b, pressed);
        cursor += step;
        ++placed;
    }

    if (heldSteps == 0u || font == nullptr)
    {
        return;
    }
    char text[16] = {};
    if (heldSteps >= QOH99_INPUT_DISPLAY_HOLD_CAP)
    {
        std::snprintf(text, sizeof(text), "%lu+",
                      static_cast<unsigned long>(QOH99_INPUT_DISPLAY_HOLD_CAP));
    }
    else
    {
        std::snprintf(text, sizeof(text), "%lu",
                      static_cast<unsigned long>(heldSteps));
    }
    const float digitPx = ClampFloat(cell * 0.80f, 9.0f, 40.0f);
    const ImVec2 textSize = MeasureText(font, digitPx, text);
    const float textX = panel.rightAligned
        ? contentL
        : (contentR - textSize.x);
    DrawShadowedText(
        drawList, font, digitPx,
        ImVec2(textX, snappedY - textSize.y * 0.5f),
        IM_COL32(232, 232, 236, 235), text);
}

void DrawInputDisplayLane(
    ImDrawList *drawList,
    ImFont *font,
    const InputDisplayPanel &panel,
    const Qoh99InputDisplayLaneV1 &lane,
    float scale)
{
    const ImVec2 bandP0(panel.left, panel.top);
    const ImVec2 bandP1(panel.right, panel.bottom);
    drawList->PushClipRect(bandP0, bandP1, true);

    const float rounding = 3.0f * scale;
    const float border = (std::max)(1.0f, scale);

    const ImVec2 histP0(panel.left, panel.historyTop);
    const ImVec2 histP1(panel.right, panel.bottom);
    drawList->AddRectFilled(histP0, histP1, IM_COL32(0, 0, 0, 110), rounding);
    drawList->AddRect(histP0, histP1, IM_COL32(255, 255, 255, 42), rounding, 0,
                      border);

    const ImVec2 liveP0(panel.left, panel.top);
    const ImVec2 liveP1(panel.right, panel.liveBottom);
    drawList->AddRectFilled(liveP0, liveP1, IM_COL32(0, 0, 0, 165), rounding);
    drawList->AddRect(liveP0, liveP1, IM_COL32(255, 255, 255, 130), rounding, 0,
                      border);

    DrawInputDisplayRow(
        drawList, font, panel, panel.top, lane.currentMask, 0u, true);

    if (font != nullptr)
    {
        const char *const kLiveLabel = "NOW";
        const float digitPx = ClampFloat(panel.cell * 0.80f, 9.0f, 40.0f);
        const ImVec2 textSize = MeasureText(font, digitPx, kLiveLabel);
        const float contentL = panel.left + panel.inset;
        const float contentR = panel.right - panel.inset;
        const float textX = panel.rightAligned
            ? contentL
            : (contentR - textSize.x);
        const float centreY = panel.top + panel.cell * 0.5f;
        DrawShadowedText(
            drawList, font, digitPx,
            ImVec2(textX, std::floor(centreY) + 0.5f - textSize.y * 0.5f),
            IM_COL32(196, 208, 226, 190), kLiveLabel);
    }

    for (int row = 0; row < panel.visibleRows; ++row)
    {
        const DWORD slot =
            InputDisplayLaneSlotAt(lane, static_cast<DWORD>(row));
        if (slot >= QOH99_INPUT_DISPLAY_HISTORY)
        {
            break;
        }
        const Qoh99InputDisplayEntryV1 &entry = lane.entries[slot];
        const float rowTop =
            panel.historyTop + panel.cell * static_cast<float>(row);
        if (rowTop + panel.cell > panel.bottom)
        {
            break;
        }
        DrawInputDisplayRow(
            drawList, font, panel, rowTop, entry.logicalMask, entry.heldSteps,
            false);
    }

    drawList->PopClipRect();
}

void DrawInputDisplay(const EfzDx9PresentParams &params)
{
    if (!g_inputDisplayVisible)
    {
        return;
    }
    if (g_presenter.inputDisplayApiVersion != QOH99_INPUT_DISPLAY_API_VERSION
        || g_presenter.inputDisplayProvider == nullptr)
    {
        return;
    }

    Qoh99InputDisplaySnapshotV1 snapshot;
    std::memset(&snapshot, 0, sizeof(snapshot));
    if (!g_presenter.inputDisplayProvider(
            &snapshot, sizeof(snapshot), g_presenter.inputDisplayUserData))
    {
        return;
    }
    if (!InputDisplaySnapshotRenderable(snapshot))
    {
        return;
    }

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr)
    {
        return;
    }

    const qoh99_input_display_layout::Metrics metrics =
        qoh99_input_display_layout::compute(
            params.dstX, params.dstY, params.dstW, params.dstH,
            static_cast<int>(QOH99_INPUT_DISPLAY_HISTORY));
    if (!metrics.renderable)
    {
        return;
    }
    const float scale = metrics.scale;

    OverlayFilterSettingsEnsure();
    for (DWORD lane = 0; lane < snapshot.laneCount; ++lane)
    {
        const Qoh99InputDisplayLaneV1 &view = snapshot.lanes[lane];
        if (view.active == 0u)
        {
            continue;
        }
        if (lane < 2u && (g_inputViewerSideMask & (1u << lane)) == 0u)
        {
            continue;
        }
        if (snapshot.mode == QOH99_INPUT_DISPLAY_MODE_TRAINING
            && lane != snapshot.drivenLane)
        {
            continue;
        }

        const qoh99_input_display_layout::LanePanelX laneX =
            qoh99_input_display_layout::lanePanelX(metrics, lane);
        InputDisplayPanel panel;
        panel.rightAligned = laneX.rightAligned;
        panel.left = laneX.left;
        panel.right = laneX.right;
        panel.top = metrics.bandTop;
        panel.bottom = metrics.bandBottom;
        panel.liveBottom = metrics.liveBottom;
        panel.historyTop = metrics.historyTop;
        panel.cell = metrics.cell;
        panel.inset = metrics.inset;
        panel.durationColumn = metrics.durationColumn;
        panel.visibleRows = metrics.visibleRows;

        DrawInputDisplayLane(drawList, font, panel, view, scale);
    }
}

DWORD g_replayTakeoverDelayGeneration = 0u;

bool ReplayTransportPoll(Qoh99ReplayTransportStatusV2 &out)
{
    if (g_presenter.replayTransportApiVersion
            != QOH99_REPLAY_TRANSPORT_API_VERSION
        || g_presenter.replayTransportStatus == nullptr)
    {
        return false;
    }
    std::memset(&out, 0, sizeof(out));
    if (!g_presenter.replayTransportStatus(
            &out, sizeof(out), g_presenter.replayTransportUserData))
    {
        return false;
    }
    if (out.apiVersion != QOH99_REPLAY_TRANSPORT_API_VERSION
        || out.structBytes < sizeof(Qoh99ReplayTransportStatusV2))
    {
        return false;
    }
    if (out.active != 0u && out.takeoverAvailable != 0u
        && g_replayTakeoverDelayGeneration != out.generation
        && g_presenter.replayTransportSubmit != nullptr)
    {
        int delay = static_cast<int>(GetPrivateProfileIntA(
            "Replay", "TakeoverDelay", 0, HudIniPath()));
        delay = (std::max)(0, (std::min)(9, delay));
        if (g_presenter.replayTransportSubmit(
                out.generation, QOH99_REPLAY_TAKEOVER_SET_DELAY,
                static_cast<ULONGLONG>(delay),
                g_presenter.replayTransportUserData))
        {
            g_replayTakeoverDelayGeneration = out.generation;
        }
    }
    return out.active != 0u;
}

void ReplayTransportSubmit(const Qoh99ReplayTransportStatusV2 &status,
                           DWORD command, ULONGLONG argument = 0ull)
{
    if (g_presenter.replayTransportSubmit == nullptr)
    {
        return;
    }
    (void)g_presenter.replayTransportSubmit(
        status.generation, command, argument,
        g_presenter.replayTransportUserData);
}

void FormatReplaySpeed(char *out, size_t cap, DWORD permille)
{
    if (permille == 0u)
    {
        std::snprintf(out, cap, "x?.??");
        return;
    }
    std::snprintf(out, cap, "x%u.%02u",
                  static_cast<unsigned>(permille / 1000u),
                  static_cast<unsigned>((permille % 1000u) / 10u));
}

void DrawReplayTakeoverHud(
    const EfzDx9PresentParams &params,
    const Qoh99ReplayTransportStatusV2 &status,
    ImDrawList *drawList, ImFont *font, float fontSize)
{
    if (status.takeoverAvailable == 0u)
        return;

    char number[24];
    std::snprintf(number, sizeof(number), "%lu",
                  static_cast<unsigned long>(status.takeoverDelay));
    drawList->AddText(
        font, fontSize,
        ImVec2(GameToScreenX(params, 10.0f),
               GameToScreenY(params, 466.0f)),
        IM_COL32(255, 255, 255, 255), number);

    if (status.takeoverPhase != 1u && status.takeoverPhase != 2u)
        return;
    const float centerX = status.takeoverLane == 2u ? 416.0f : 224.0f;
    constexpr float centerY = 428.0f;
    const auto rect = [&](float x1, float y1, float x2, float y2,
                          ImU32 color) {
        drawList->AddRectFilled(
            ImVec2(GameToScreenX(params, x1), GameToScreenY(params, y1)),
            ImVec2(GameToScreenX(params, x2), GameToScreenY(params, y2)),
            color);
    };

    const DWORD shown = status.takeoverPhase == 2u
        ? status.takeoverElapsed : status.takeoverCountdown;
    std::snprintf(number, sizeof(number), "%lu",
                  static_cast<unsigned long>(shown));
    const ImVec2 numberSize = font->CalcTextSizeA(
        fontSize, FLT_MAX, 0.0f, number);

    if (status.takeoverPhase == 2u)
    {
        const float halfWidth = numberSize.x * 0.5f
            / (std::max)(0.001f, GameScale(params));
        rect(centerX - halfWidth, centerY - 9.0f,
             centerX + halfWidth, centerY + 9.0f,
             IM_COL32(0, 255, 0, 255));
    }
    else
    {
        rect(centerX - 60.0f, centerY - 9.0f,
             centerX + 60.0f, centerY + 9.0f,
             IM_COL32(255, 0, 0, 255));
        const DWORD remaining = (std::min)(
            static_cast<DWORD>(40u), status.takeoverCountdown);
        const float progress = 116.0f
            * static_cast<float>(40u - remaining) / 40.0f;
        rect(centerX - 58.0f, centerY - 7.0f,
             centerX - 58.0f + progress, centerY + 7.0f,
             IM_COL32(255, 255, 0, 255));
        rect(centerX - 58.0f + progress, centerY - 7.0f,
             centerX + 58.0f, centerY + 7.0f,
             IM_COL32(0, 0, 255, 255));
    }
    drawList->AddText(
        font, fontSize,
        ImVec2(GameToScreenX(params, centerX) - numberSize.x * 0.5f,
               GameToScreenY(params, centerY - 7.0f)),
        IM_COL32(255, 255, 255, 255), number);
}

void DrawReplayTransport(const EfzDx9PresentParams &params)
{
    Qoh99ReplayTransportStatusV2 status;
    if (!ReplayTransportPoll(status))
    {
        return;
    }

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr)
    {
        return;
    }

    HudLayoutEnsureLoaded();

    char speed[16];
    FormatReplaySpeed(speed, sizeof(speed), status.speedPermille);

    char line[96];
    std::snprintf(line, sizeof(line), "%s  %lu / %lu  %s",
                  status.paused != 0u ? "PAUSED" : "REPLAY",
                  static_cast<unsigned long>(status.nextRecord),
                  static_cast<unsigned long>(status.endpointRecord),
                  speed);

    const float scale = GameScale(params);
    const float userScale = static_cast<float>(
        g_hudLayout.fontScalePct > 0 ? g_hudLayout.fontScalePct : 100) / 100.0f;
    const float fontSize = font->FontSize * scale * userScale;
    DrawReplayTakeoverHud(
        params, status, drawList, font, font->FontSize * scale);
    if (status.paused != 0u && status.mayHold != 0u)
    {
        BYTE pauseKey = 0u, menuKey = 0u;
        EFZDDRAW_QueryWrapperHotkey("ReplayPause", &pauseKey, 1u);
        EFZDDRAW_QueryWrapperHotkey("ToggleOverlayMenu", &menuKey, 1u);
        char hint[96];
        FormatReplayResumeHint(hint, sizeof(hint), pauseKey, menuKey);
        DrawCenteredHudLine(
            drawList, font, fontSize, scale,
            HudResolveX(params, g_hudLayout.sx, static_cast<float>(kHudDefSessionX)),
            HudResolveY(params, g_hudLayout.sy, static_cast<float>(kHudDefSessionY))
                - fontSize - 8.0f * scale,
            300.0f * scale, hint, IM_COL32(255, 226, 120, 255));
    }
    if (!g_replayTransportVisible)
        return;
    DrawCenteredHudLine(
        drawList, font, fontSize, scale,
        HudResolveX(params, g_hudLayout.sx, static_cast<float>(kHudDefSessionX)),
        HudResolveY(params, g_hudLayout.sy, static_cast<float>(kHudDefSessionY)),
        300.0f * scale, line,
        status.paused != 0u ? IM_COL32(255, 226, 120, 255)
                            : IM_COL32(232, 244, 255, 255));
}

void DrawTrainingFrameAdvantageOverlay(const EfzDx9PresentParams &params)
{
    Qoh99TrainingStatusV1 status = {};
    if (!ReadTrainingStatus(status)
        || status.explicitTrainingPresent == 0
        || (g_presenter.menuVisible && g_hudPreview)
        || status.inMatchReady == 0
        || status.lifecycleState != QOH99_TRAINING_LIFECYCLE_ACTIVE
        || !TrainingHasCombatToolsStatus(status)
        || (status.capabilityFlags & QOH99_TRAINING_CAP_FRAME_ADVANTAGE) == 0
        || status.frameAdvantageEnabled == 0
        || status.combatToolsReadoutValid == 0)
    {
        return;
    }

    char line[64] = {};
    ImU32 color = IM_COL32(232, 244, 255, 255);

    const bool liveGap =
        TrainingHasFrameAdvantageGapStatus(status)
        && status.frameAdvantageGapValid != 0u;
    if (liveGap)
    {
        std::snprintf(line, sizeof(line), "Gap %lu",
                      static_cast<unsigned long>(
                          status.frameAdvantageGapFrames));
        color = IM_COL32(255, 220, 90, 255);
    }
    else
    switch (status.frameAdvantageState)
    {
    case QOH99_TRAINING_FRAME_ADVANTAGE_COMPLETE:
        if (status.frameAdvantageValid == 0)
        {
            return;
        }
        {
            const int value = static_cast<int>(
                static_cast<INT32>(status.frameAdvantageValue));
            const unsigned player =
                static_cast<unsigned>(status.frameAdvantageAttackerOwner + 1u);
            const char *contact = "hit";
            if (status.frameAdvantageContactKind
                    == QOH99_TRAINING_CONTACT_BLOCK)
            {
                contact = "block";
            }
            else if (status.frameAdvantageContactKind
                     == QOH99_TRAINING_CONTACT_THROW)
            {
                contact = "throw";
            }
            OverlayFilterSettingsEnsure();
            if ((g_frameAdvSideMask
                 & (status.frameAdvantageAttackerOwner == 0u ? 1u : 2u))
                == 0u)
            {
                return;
            }
            std::snprintf(
                line, sizeof(line), "P%u  %+d  %s", player, value, contact);
            color = value > 0
                ? IM_COL32(130, 244, 150, 255)
                : value < 0 ? IM_COL32(255, 150, 125, 255)
                            : IM_COL32(238, 238, 238, 255);
        }
        break;
    case QOH99_TRAINING_FRAME_ADVANTAGE_TRADE:
        std::snprintf(line, sizeof(line), "Trade");
        color = IM_COL32(255, 220, 120, 255);
        break;
    default:
        return;
    }

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr)
    {
        return;
    }

    HudLayoutEnsureLoaded();
    const float scale = GameScale(params);
    const float userScale = static_cast<float>(
        g_hudLayout.fontScalePct > 0 ? g_hudLayout.fontScalePct : 100) / 100.0f;
    DrawCenteredHudLine(
        drawList, font, font->FontSize * scale * userScale, scale,
        HudResolveX(params, g_hudLayout.sx, static_cast<float>(kHudDefSessionX)),
        HudResolveY(params, g_hudLayout.sy, static_cast<float>(kHudDefSessionY)),
        300.0f * scale, line, color);
}

bool TrainingHasStatusMessages(const Qoh99TrainingStatusV1 &status)
{
    return status.structSize >= QOH99_TRAINING_STATUS_V1_STATUS_MESSAGE_TIER;
}

ImU32 TrainingStatusModeColor(DWORD mode)
{
    switch (mode)
    {
    case QOH99_TRAINING_STATUS_MODE_PRE_RECORD:
        return IM_COL32(255, 200, 90, 255);
    case QOH99_TRAINING_STATUS_MODE_RECORDING:
        return IM_COL32(255, 110, 110, 255);
    case QOH99_TRAINING_STATUS_MODE_PLAYING:
        return IM_COL32(130, 200, 255, 255);
    default:
        return IM_COL32(238, 238, 238, 255);
    }
}

ImU32 TrainingStatusEventColor(DWORD event)
{
    switch (event)
    {
    case QOH99_TRAINING_STATUS_EVENT_STATE_SAVED:
    case QOH99_TRAINING_STATUS_EVENT_MACRO_RECORDED:
        return IM_COL32(130, 244, 150, 255);
    default:
        return IM_COL32(238, 238, 238, 255);
    }
}

const char *TrainingPositionPresetLabel(DWORD preset)
{
    switch (preset)
    {
    case QOH99_TRAINING_POSITION_ROUND_START:  return "Round Start";
    case QOH99_TRAINING_POSITION_LEFT_CORNER:  return "Left Corner";
    case QOH99_TRAINING_POSITION_RIGHT_CORNER: return "Right Corner";
    case QOH99_TRAINING_POSITION_CENTER:       return "Center";
    case QOH99_TRAINING_POSITION_VERY_CLOSE:   return "Touching";
    default:                                   return "";
    }
}

void TrainingStatusModeText(const Qoh99TrainingStatusV1 &status, char *out,
                            size_t outBytes)
{
    out[0] = '\0';
    const unsigned long slot =
        static_cast<unsigned long>(status.statusModeDetail);
    switch (status.statusModeKind)
    {
    case QOH99_TRAINING_STATUS_MODE_PRE_RECORD:
        std::snprintf(
            out, outBytes,
            qi18n::tr("PRE-RECORD slot %lu  -  move the dummy, Record to start"),
            slot);
        break;
    case QOH99_TRAINING_STATUS_MODE_RECORDING:
        std::snprintf(out, outBytes, qi18n::tr("RECORDING slot %lu"), slot);
        break;
    case QOH99_TRAINING_STATUS_MODE_PLAYING:
        std::snprintf(out, outBytes, qi18n::tr("PLAYING slot %lu"), slot);
        break;
    default:
        break;
    }
}

void TrainingStatusEventText(const Qoh99TrainingStatusV1 &status, char *out,
                             size_t outBytes)
{
    out[0] = '\0';
    const unsigned long detail =
        static_cast<unsigned long>(status.statusEventDetail);
    switch (status.statusEventKind)
    {
    case QOH99_TRAINING_STATUS_EVENT_POSITION_RESET:
    {
        const char *preset =
            TrainingPositionPresetLabel(status.statusEventDetail);
        if (preset[0] != '\0')
        {
            std::snprintf(out, outBytes, qi18n::tr("Position reset: %s"),
                          qi18n::tr(preset));
        }
        else
        {
            std::snprintf(out, outBytes, "%s", qi18n::tr("Position reset"));
        }
        break;
    }
    case QOH99_TRAINING_STATUS_EVENT_ROUND_RESET:
        std::snprintf(out, outBytes, "%s", qi18n::tr("Reset to Neutral"));
        break;
    case QOH99_TRAINING_STATUS_EVENT_STATE_SAVED:
        std::snprintf(out, outBytes, qi18n::tr("Saved state slot %lu"), detail);
        break;
    case QOH99_TRAINING_STATUS_EVENT_STATE_LOADED:
        std::snprintf(out, outBytes, qi18n::tr("Loaded state slot %lu"),
                      detail);
        break;
    case QOH99_TRAINING_STATUS_EVENT_STATE_CLEARED:
        std::snprintf(out, outBytes, qi18n::tr("Cleared state slot %lu"),
                      detail);
        break;
    case QOH99_TRAINING_STATUS_EVENT_STATE_SLOT:
        std::snprintf(out, outBytes, qi18n::tr("State slot %lu"), detail);
        break;
    case QOH99_TRAINING_STATUS_EVENT_MACRO_SLOT:
        std::snprintf(out, outBytes, qi18n::tr("Recording slot %lu"), detail);
        break;
    case QOH99_TRAINING_STATUS_EVENT_MACRO_CLEARED:
        std::snprintf(out, outBytes, qi18n::tr("Cleared recording slot %lu"),
                      detail);
        break;
    case QOH99_TRAINING_STATUS_EVENT_MACRO_RECORDED:
        std::snprintf(out, outBytes, qi18n::tr("Recorded %lu frames"), detail);
        break;
    case QOH99_TRAINING_STATUS_EVENT_MACRO_STOPPED:
        std::snprintf(out, outBytes, "%s", qi18n::tr("Recording stopped"));
        break;
    default:
        break;
    }
}

void DrawTrainingStatusMessages(const EfzDx9PresentParams &params)
{
    Qoh99TrainingStatusV1 status = {};
    if (!ReadTrainingStatus(status)
        || status.explicitTrainingPresent == 0
        || !TrainingHasStatusMessages(status)
        || (g_presenter.menuVisible && g_hudPreview)
        || status.inMatchReady == 0
        || status.lifecycleState != QOH99_TRAINING_LIFECYCLE_ACTIVE)
    {
        return;
    }

    static DWORD lastSequence = 0u;
    static DWORD lastGeneration = 0u;
    static double eventShownAt = 0.0;
    const double now = ImGui::GetTime();
    if (status.statusEventSequence != lastSequence
        || status.trainingGeneration != lastGeneration)
    {
        lastSequence = status.statusEventSequence;
        lastGeneration = status.trainingGeneration;
        eventShownAt = now;
    }

    char modeLine[128] = {};
    char eventLine[128] = {};
    TrainingStatusModeText(status, modeLine, sizeof(modeLine));
    TrainingStatusEventText(status, eventLine, sizeof(eventLine));

    const double age = now - eventShownAt;
    const double kHoldSeconds = 2.0;
    const double kFadeSeconds = 0.6;
    float eventAlpha = 1.0f;
    if (status.statusEventSequence == 0u || age >= kHoldSeconds + kFadeSeconds)
    {
        eventAlpha = 0.0f;
    }
    else if (age > kHoldSeconds)
    {
        eventAlpha =
            1.0f - static_cast<float>((age - kHoldSeconds) / kFadeSeconds);
    }
    if (eventAlpha <= 0.0f) eventLine[0] = '\0';

    if (modeLine[0] == '\0' && eventLine[0] == '\0') return;

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr) return;

    HudLayoutEnsureLoaded();
    const float scale = GameScale(params);
    const float userScale = static_cast<float>(
        g_hudLayout.fontScalePct > 0 ? g_hudLayout.fontScalePct : 100) / 100.0f;
    const float fontSize = font->FontSize * scale * userScale;
    const float lineStep = fontSize + 4.0f * scale;
    const float x =
        HudResolveX(params, g_hudLayout.sx, static_cast<float>(kHudDefSessionX));
    const float y =
        HudResolveY(params, g_hudLayout.sy, static_cast<float>(kHudDefSessionY));

    if (modeLine[0] != '\0')
    {
        DrawCenteredHudLine(
            drawList, font, fontSize, scale, x, y - 2.0f * lineStep,
            360.0f * scale, modeLine,
            TrainingStatusModeColor(status.statusModeKind));
    }
    if (eventLine[0] != '\0')
    {
        const ImU32 base = TrainingStatusEventColor(status.statusEventKind);
        const ImU32 faded =
            (base & 0x00FFFFFFu)
            | (static_cast<ImU32>(255.0f * eventAlpha) << IM_COL32_A_SHIFT);
        DrawCenteredHudLine(
            drawList, font, fontSize, scale, x, y - lineStep,
            360.0f * scale, eventLine, faded);
    }
}

const char *TrainingWakeupPhaseLabel(DWORD phase)
{
    switch (phase)
    {
    case QOH99_TRAINING_WAKEUP_WATCHING: return "watching";
    case QOH99_TRAINING_WAKEUP_WAITING_SUFFIX: return "waiting";
    case QOH99_TRAINING_WAKEUP_PRIMING: return "priming";
    case QOH99_TRAINING_WAKEUP_FIRST_RESPONSE: return "first-ready";
    case QOH99_TRAINING_WAKEUP_RELEASED: return "released";
    case QOH99_TRAINING_WAKEUP_COMPLETED: return "complete";
    case QOH99_TRAINING_WAKEUP_CANCELLED: return "cancelled";
    default: return "idle";
    }
}

void DrawTrainingDebugOverlay(const EfzDx9PresentParams &params)
{
    Qoh99TrainingStatusV1 status = {};
    if (!ReadTrainingStatus(status)
        || !TrainingHasDebugOverlayStatus(status)
        || status.explicitTrainingPresent == 0u
        || status.debugOverlayEnabled == 0u
        || status.inMatchReady == 0u
        || status.lifecycleState != QOH99_TRAINING_LIFECYCLE_ACTIVE
        || status.fighterReadoutValid == 0u)
    {
        return;
    }

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr)
    {
        return;
    }

    const float scale = GameScale(params);
    const float fontSize = ImGui::GetFontSize() * scale * 0.92f;
    const float lineHeight = fontSize + 2.0f * scale;
    const float left = GameToScreenX(params, 14.0f);
    const float top = GameToScreenY(params, 102.0f);

    static const int kFixedLines = 7;
    static const int kActorLines = 3;
    char lines[kFixedLines + kActorLines][192] = {};
    int lineCount = kFixedLines;
    char p1Dispatch[16] = "--";
    char p2Dispatch[16] = "--";
    char p1Flags[16] = "--";
    char p2Flags[16] = "--";
    if (status.resourcePolicyValid != 0u)
    {
        std::snprintf(p1Dispatch, sizeof(p1Dispatch), "%lu",
            static_cast<unsigned long>(status.p1DispatchAction));
        std::snprintf(p2Dispatch, sizeof(p2Dispatch), "%lu",
            static_cast<unsigned long>(status.p2DispatchAction));
        std::snprintf(p1Flags, sizeof(p1Flags), "%02lX",
            static_cast<unsigned long>(status.p1AuthoredFrameFlags));
        std::snprintf(p2Flags, sizeof(p2Flags), "%02lX",
            static_cast<unsigned long>(status.p2AuthoredFrameFlags));
    }
    const unsigned long long step =
        (static_cast<unsigned long long>(status.debugStepSerialHigh) << 32u)
        | status.debugStepSerialLow;
    std::snprintf(lines[0], sizeof(lines[0]),
                  "TRAINING DEBUG  step %llu", step);
    std::snprintf(lines[1], sizeof(lines[1]),
                  "P1  action %lu  anim %lu  next frame %lu  dispatch %s  request %lu",
                  static_cast<unsigned long>(status.p1ActionState),
                  static_cast<unsigned long>(status.p1AnimFrame),
                  static_cast<unsigned long>(status.p1ActionFrame),
                  p1Dispatch,
                  static_cast<unsigned long>(status.p1NextAction));
    std::snprintf(lines[2], sizeof(lines[2]),
                  "P2  action %lu  anim %lu  next frame %lu  dispatch %s  request %lu",
                  static_cast<unsigned long>(status.p2ActionState),
                  static_cast<unsigned long>(status.p2AnimFrame),
                  static_cast<unsigned long>(status.p2ActionFrame),
                  p2Dispatch,
                  static_cast<unsigned long>(status.p2NextAction));
    std::snprintf(lines[3], sizeof(lines[3]),
                  "Ready  P1 %s / freeze %lu    P2 %s / freeze %lu    GC %s",
                  status.p1ResponseReady ? "yes" : "no",
                  static_cast<unsigned long>(status.p1FreezeFrames),
                  status.p2ResponseReady ? "yes" : "no",
                  static_cast<unsigned long>(status.p2FreezeFrames),
                  status.p2GuardCancelReady ? "open" : "closed");
    std::snprintf(lines[4], sizeof(lines[4]),
                  "Input  P1 %05lX  P2 %05lX    flags %s / %s",
                  static_cast<unsigned long>(status.debugP1LogicalMask),
                  static_cast<unsigned long>(status.debugP2LogicalMask),
                  p1Flags, p2Flags);
    std::snprintf(lines[5], sizeof(lines[5]),
                  "Wakeup  %s  move %lu  locked %lu  prefix %lu/%lu  result %lu",
                  TrainingWakeupPhaseLabel(status.wakeupReversalPhase),
                  static_cast<unsigned long>(status.wakeupReversalMoveIndex),
                  static_cast<unsigned long>(
                      status.wakeupLockedStepsRemaining),
                  static_cast<unsigned long>(status.wakeupPrefixCursor),
                   static_cast<unsigned long>(status.wakeupCommitIndex),
                   static_cast<unsigned long>(status.wakeupLastOutcome));
    std::snprintf(lines[6], sizeof(lines[6]),
                  "Throw tech  %s  tries %lu  native %lu  auto %lu  result %lu",
                  status.throwTechChancePercent != 0u ? "on" : "off",
                  static_cast<unsigned long>(status.throwTechOpportunities),
                  static_cast<unsigned long>(status.throwTechNativeAccepted),
                  static_cast<unsigned long>(status.throwTechAutomaticAccepted),
                  static_cast<unsigned long>(status.throwTechResultObserved));

    if (TrainingHasActorPoolStatus(status) && status.actorPoolValid != 0u
        && status.actorPoolReportedCount != 0u)
    {
        const DWORD reported = (std::min)(
            status.actorPoolReportedCount,
            static_cast<DWORD>(QOH99_TRAINING_ACTOR_REPORT_CAPACITY));
        std::snprintf(lines[lineCount], sizeof(lines[0]),
                      "Actors  %lu live%s",
                      static_cast<unsigned long>(status.actorPoolLiveCount),
                      status.actorPoolLiveCount > reported
                          ? "  (list truncated)" : "");
        ++lineCount;

        DWORD index = 0;
        while (index < reported && lineCount < kFixedLines + kActorLines)
        {
            char row[192] = {};
            std::size_t used = 0;
            for (int column = 0; column < 4 && index < reported;
                 ++column, ++index)
            {
                char owner[8];
                if (status.actorPoolOwner[index]
                    == QOH99_TRAINING_ACTOR_OWNER_UNKNOWN)
                {
                    std::snprintf(owner, sizeof(owner), "?");
                }
                else
                {
                    std::snprintf(owner, sizeof(owner), "P%lu",
                        static_cast<unsigned long>(
                            status.actorPoolOwner[index] + 1u));
                }
                const int written = std::snprintf(
                    row + used, used < sizeof(row) ? sizeof(row) - used : 0,
                    "%s%c%02lu:%lu/%s",
                    used == 0 ? "  " : "   ",
                    TrainingActorPoolLetter(status.actorPoolSlot[index]),
                    static_cast<unsigned long>(
                        status.actorPoolSlot[index] & 0x00FFFFFFu),
                    static_cast<unsigned long>(status.actorPoolAction[index]),
                    owner);
                if (written <= 0) break;
                used += static_cast<std::size_t>(written);
                if (used >= sizeof(row)) break;
            }
            std::snprintf(lines[lineCount], sizeof(lines[0]), "%s", row);
            ++lineCount;
        }
    }

    float widest = 0.0f;
    for (int index = 0; index < lineCount; ++index)
    {
        widest = (std::max)(widest,
            font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lines[index]).x);
    }
    const float right = (std::min)(
        GameToScreenX(params, 626.0f), left + widest + 14.0f * scale);
    const float bottom =
        top + lineHeight * static_cast<float>(lineCount) + 8.0f * scale;
    drawList->AddRectFilled(
        ImVec2(left, top), ImVec2(right, bottom),
        IM_COL32(7, 10, 16, 190), 4.0f * scale);
    drawList->AddRect(
        ImVec2(left, top), ImVec2(right, bottom),
        IM_COL32(105, 154, 214, 210), 4.0f * scale,
        0, 1.0f * scale);

    for (int index = 0; index < lineCount; ++index)
    {
        const ImU32 color = index == 0
            ? IM_COL32(155, 205, 255, 255)
            : index == 5 ? IM_COL32(255, 225, 145, 255)
            : index >= kFixedLines ? IM_COL32(170, 240, 190, 255)
                         : IM_COL32(235, 241, 248, 255);
        drawList->AddText(
            font, fontSize,
            ImVec2(left + 7.0f * scale,
                   top + 4.0f * scale + lineHeight * index),
            color, lines[index]);
    }
}

void DrawTexturedQuad(IDirect3DTexture9 *tex, float dx, float dy, float dw, float dh, DWORD filter)
{
    const float left = dx - 0.5f;
    const float top = dy - 0.5f;
    const float right = dx + dw - 0.5f;
    const float bottom = dy + dh - 0.5f;

    const FrameQuadVertex vertices[4] = {
        {left, top, 0.0f, 1.0f, 0.0f, 0.0f},
        {right, top, 0.0f, 1.0f, 1.0f, 0.0f},
        {left, bottom, 0.0f, 1.0f, 0.0f, 1.0f},
        {right, bottom, 0.0f, 1.0f, 1.0f, 1.0f},
    };

    g_presenter.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_presenter.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_presenter.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_presenter.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_presenter.device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_presenter.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_presenter.device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_presenter.device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
    g_presenter.device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
    g_presenter.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_presenter.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_presenter.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_presenter.device->SetFVF(kFrameQuadFvf);
    g_presenter.device->SetTexture(0, tex);
    g_presenter.device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(FrameQuadVertex));
    g_presenter.device->SetTexture(0, nullptr);
}

int ComputePrescaleFactor(int srcW, int srcH, int dstW, int dstH)
{
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return 1;
    int pw = (dstW + srcW - 1) / srcW;
    int ph = (dstH + srcH - 1) / srcH;
    int p = (pw > ph) ? pw : ph;
    if (p < 1) p = 1;
    if (p > 6) p = 6;
    return p;
}

void RenderFrameQuad(const EfzDx9PresentParams &params)
{
    const float dx = static_cast<float>(params.dstX);
    const float dy = static_cast<float>(params.dstY);
    const float dw = static_cast<float>(params.dstW);
    const float dh = static_cast<float>(params.dstH);

    const int prescale = ComputePrescaleFactor(params.width, params.height, params.dstW, params.dstH);
    if (prescale > 1 && g_presenter.frameTexture != nullptr)
    {
        const int sw = params.width * prescale;
        const int sh = params.height * prescale;
        if (EnsureScaleTexture(sw, sh))
        {
            IDirect3DSurface9 *backRT = nullptr;
            IDirect3DSurface9 *scaleRT = nullptr;
            if (SUCCEEDED(g_presenter.device->GetRenderTarget(0, &backRT))
                && SUCCEEDED(g_presenter.scaleTexture->GetSurfaceLevel(0, &scaleRT)))
            {
                g_presenter.device->SetRenderTarget(0, scaleRT);
                DrawTexturedQuad(g_presenter.frameTexture, 0.0f, 0.0f,
                                 static_cast<float>(sw), static_cast<float>(sh), D3DTEXF_POINT);
                g_presenter.device->SetRenderTarget(0, backRT);
                DrawTexturedQuad(g_presenter.scaleTexture, dx, dy, dw, dh, D3DTEXF_LINEAR);
                if (scaleRT) scaleRT->Release();
                if (backRT) backRT->Release();
                return;
            }
            if (scaleRT) scaleRT->Release();
            if (backRT) backRT->Release();
        }
    }

    const DWORD filter = (params.linearScale != nullptr && *params.linearScale != FALSE)
        ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    DrawTexturedQuad(g_presenter.frameTexture, dx, dy, dw, dh, filter);
}

enum SettingBoolId
{
    SettingBoolApplyRemotePalettes = 0,
    SettingBoolNonArcadeRematch = 1,
    SettingBoolOfflineVsHuman = 2,
    SettingBoolDebugMenu = 3,
    SettingBoolEnableHolePunch = 4,
    SettingBoolEnableUpnp = 5,
    SettingBoolFileLogging = 6,
    SettingBoolConsole = 7,
    SettingBoolPreserveModLog = 8,
    SettingBoolPreserveRevivalLogs = 9,
    SettingBoolTlsVerify = 10,
    SettingBoolForceWinInet = 11,
    SettingBoolForceEmbeddedTls = 12,
    SettingBoolHideEmptySets = 13,
    SettingBoolHarnessShm = 14,
    SettingBoolStressTest = 15,
    SettingBoolHudPreview = 16,
    SettingBoolAllowPracticeKeys = 17,
    SettingBoolHudRollbackMetrics = 18,
    SettingBoolDeepTelemetry = 19,
};

enum SettingIntId
{
    SettingIntDefaultPort = 0,
    SettingIntInputDelayP1 = 1,
    SettingIntInputDelayP2 = 2,
    SettingIntBgmVolume = 3,
    SettingIntSeVolume = 4,
    SettingIntSavestateSaveKey = 5,
    SettingIntSavestateLoadKey = 6,
    SettingIntPlayerSwapKey = 7,
    SettingIntFramestepPauseKey = 8,
    SettingIntFramestepStepKey = 9,
    SettingIntDelayDecreaseKey = 10,
    SettingIntDelayIncreaseKey = 11,
    SettingIntDelayPlayerToggleKey = 12,
    SettingIntCollisionHitKey = 13,
    SettingIntCollisionHurtKey = 14,
    SettingIntCollisionBoxKey = 15,
    SettingIntHudFontScale = 18,
    SettingIntHudPosBase = 19,
};

enum SettingStringId
{
    SettingStringNickname = 0,
    SettingStringLastJoinAddress = 1,
    SettingStringHolePunchServer = 2,
    SettingStringBaseUrl = 3,
    SettingStringProxyBaseUrl = 4,
};

#pragma pack(push, 1)
struct BmeSessionStatus
{
    int32_t  phase;
    int32_t  role;
    int32_t  pingMs;
    int32_t  delayFrames;
    int32_t  activePlayer;
    int32_t  p1Wins;
    int32_t  p2Wins;
    uint16_t port;
    uint16_t localPort;
    uint32_t phaseTickMs;
    char     address[64];
    char     nickname[64];
    char     p1Name[64];
    char     p2Name[64];
    char     phaseName[32];
    char     errorMsg[128];
};

struct BmeDelayMetrics
{
    int32_t serial;
    int32_t averagePingMs;
    int32_t minPingMs;
    int32_t maxPingMs;
    int32_t recommendedDelay;
    int32_t minDelay;
    int32_t maxDelay;
};

struct BmeMatchStats
{
    uint64_t localFrame;
    uint64_t peerFrame;
    int64_t  inputLead;
    uint64_t mispredictCount;
    uint64_t rollbackRequests;
    uint64_t rollbackExecuted;
    uint64_t rollbackAborted;
    uint64_t framesReplayed;
    uint32_t lastReplayLength;
    uint32_t _pad0;
};
#pragma pack(pop)

struct ModApi
{
    HMODULE handle = nullptr;
    bool probed = false;

    using IsAvailableFn = int (*)(void);
    using SetBotActiveFn = void (*)(int, int, int);
    using IsBotActiveFn = int (*)(int);
    using SetLoggingFn = void (*)(int);
    using GetLastFrameFn = int (*)(uint64_t*, uint32_t*, uint8_t*, uint8_t*);
    using GetInputStatusFn = void (*)(int, uint8_t*, uint8_t*);

    using SessionStartFn = int (*)(int, uint16_t, const char*, const char*);
    using SessionCancelFn = void (*)(const char*);
    using SessionApplyDelayFn = int (*)(int);
    using SessionGetStatusFn = int (*)(BmeSessionStatus*);
    using SessionGetDelayMetricsFn = int (*)(BmeDelayMetrics*);
    using SessionGetRecentEventsFn = int (*)(char*, int);
    using SessionGetMatchStatsFn = int (*)(BmeMatchStats*);
    using SessionGetEndReasonNameFn = const char* (*)(void);

    using SettingsVoidFn = void (*)(void);
    using SettingsGetBoolFn = int (*)(int);
    using SettingsSetBoolFn = int (*)(int, int);
    using SettingsGetIntFn = int (*)(int);
    using SettingsSetIntFn = int (*)(int, int);
    using SettingsGetStringFn = int (*)(int, char*, int);
    using SettingsSetStringFn = int (*)(int, const char*);

    IsAvailableFn botAvailable = nullptr;
    SetBotActiveFn setBotActive = nullptr;
    IsBotActiveFn isBotActive = nullptr;
    SetLoggingFn setBotLogging = nullptr;
    IsAvailableFn isBotLogging = nullptr;
    GetLastFrameFn getLastFrame = nullptr;
    GetInputStatusFn getInputStatus = nullptr;

    IsAvailableFn sessionAvailable = nullptr;
    SessionStartFn sessionStart = nullptr;
    SessionCancelFn sessionCancel = nullptr;
    SessionApplyDelayFn sessionApplyDelay = nullptr;
    SessionGetStatusFn sessionGetStatus = nullptr;
    SessionGetDelayMetricsFn sessionGetDelayMetrics = nullptr;
    SessionGetRecentEventsFn sessionGetRecentEvents = nullptr;
    SessionGetMatchStatsFn sessionGetMatchStats = nullptr;
    SessionGetEndReasonNameFn sessionGetEndReasonName = nullptr;

    IsAvailableFn settingsAvailable = nullptr;
    SettingsVoidFn settingsReload = nullptr;
    SettingsVoidFn settingsSave = nullptr;
    SettingsGetBoolFn settingsGetBool = nullptr;
    SettingsSetBoolFn settingsSetBool = nullptr;
    SettingsGetIntFn settingsGetInt = nullptr;
    SettingsSetIntFn settingsSetInt = nullptr;
    SettingsGetStringFn settingsGetString = nullptr;
    SettingsSetStringFn settingsSetString = nullptr;
};

struct DebugMenuEditState
{
    bool loadedFromSettings = false;
    char nickname[64] = "Player";
    char joinAddress[128] = "127.0.0.1";
    char holePunchServer[128] = "";
    char baseUrl[256] = "";
    char proxyBaseUrl[256] = "";
    int defaultPort = 10800;
    int sessionDelay = 2;
};

template <typename T>
T ResolveProc(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

ModApi& GetModApi()
{
    static ModApi api = {};
    HMODULE module = GetModuleHandleA("efz_bme_netplay.dll");
    if (api.handle != module)
    {
        api = {};
        api.handle = module;
    }

    if (module == nullptr || api.probed)
    {
        return api;
    }

    api.probed = true;
    api.botAvailable = ResolveProc<ModApi::IsAvailableFn>(module, "EFZBME_BotMatch_IsAvailable");
    api.setBotActive = ResolveProc<ModApi::SetBotActiveFn>(module, "EFZBME_BotMatch_SetBotActive");
    api.isBotActive = ResolveProc<ModApi::IsBotActiveFn>(module, "EFZBME_BotMatch_IsBotActive");
    api.setBotLogging = ResolveProc<ModApi::SetLoggingFn>(module, "EFZBME_BotMatch_SetLoggingEnabled");
    api.isBotLogging = ResolveProc<ModApi::IsAvailableFn>(module, "EFZBME_BotMatch_IsLoggingEnabled");
    api.getLastFrame = ResolveProc<ModApi::GetLastFrameFn>(module, "EFZBME_BotMatch_GetLastFrame");
    api.getInputStatus = ResolveProc<ModApi::GetInputStatusFn>(module, "EFZBME_BotMatch_GetInputStatus");

    api.sessionAvailable = ResolveProc<ModApi::IsAvailableFn>(module, "EFZBME_Session_IsAvailable");
    api.sessionStart = ResolveProc<ModApi::SessionStartFn>(module, "EFZBME_Session_Start");
    api.sessionCancel = ResolveProc<ModApi::SessionCancelFn>(module, "EFZBME_Session_Cancel");
    api.sessionApplyDelay = ResolveProc<ModApi::SessionApplyDelayFn>(module, "EFZBME_Session_ApplyDelay");
    api.sessionGetStatus = ResolveProc<ModApi::SessionGetStatusFn>(module, "EFZBME_Session_GetStatus");
    api.sessionGetDelayMetrics = ResolveProc<ModApi::SessionGetDelayMetricsFn>(module, "EFZBME_Session_GetDelayMetrics");
    api.sessionGetRecentEvents = ResolveProc<ModApi::SessionGetRecentEventsFn>(module, "EFZBME_Session_GetRecentEvents");
    api.sessionGetMatchStats = ResolveProc<ModApi::SessionGetMatchStatsFn>(module, "EFZBME_Session_GetMatchStats");
    api.sessionGetEndReasonName = ResolveProc<ModApi::SessionGetEndReasonNameFn>(module, "EFZBME_Session_GetEndReasonName");

    api.settingsAvailable = ResolveProc<ModApi::IsAvailableFn>(module, "EFZBME_Settings_IsAvailable");
    api.settingsReload = ResolveProc<ModApi::SettingsVoidFn>(module, "EFZBME_Settings_Reload");
    api.settingsSave = ResolveProc<ModApi::SettingsVoidFn>(module, "EFZBME_Settings_Save");
    api.settingsGetBool = ResolveProc<ModApi::SettingsGetBoolFn>(module, "EFZBME_Settings_GetBool");
    api.settingsSetBool = ResolveProc<ModApi::SettingsSetBoolFn>(module, "EFZBME_Settings_SetBool");
    api.settingsGetInt = ResolveProc<ModApi::SettingsGetIntFn>(module, "EFZBME_Settings_GetInt");
    api.settingsSetInt = ResolveProc<ModApi::SettingsSetIntFn>(module, "EFZBME_Settings_SetInt");
    api.settingsGetString = ResolveProc<ModApi::SettingsGetStringFn>(module, "EFZBME_Settings_GetString");
    api.settingsSetString = ResolveProc<ModApi::SettingsSetStringFn>(module, "EFZBME_Settings_SetString");
    return api;
}

bool SettingsReady(const ModApi& api)
{
    return api.settingsAvailable != nullptr
        && api.settingsAvailable() != 0
        && api.settingsGetBool != nullptr
        && api.settingsSetBool != nullptr
        && api.settingsGetInt != nullptr
        && api.settingsSetInt != nullptr;
}

bool SessionReady(const ModApi& api)
{
    return api.sessionAvailable != nullptr
        && api.sessionAvailable() != 0
        && api.sessionGetStatus != nullptr;
}

bool BotReady(const ModApi& api)
{
    return api.botAvailable != nullptr
        && api.botAvailable() != 0
        && api.isBotActive != nullptr
        && api.setBotActive != nullptr;
}

void LoadStringSetting(const ModApi& api, int settingId, char* dst, int cap)
{
    if (api.settingsGetString != nullptr && dst != nullptr && cap > 0)
    {
        api.settingsGetString(settingId, dst, cap);
        dst[cap - 1] = '\0';
    }
}

void LoadEditStateFromSettings(const ModApi& api, DebugMenuEditState& edit)
{
    if (!SettingsReady(api))
    {
        return;
    }

    LoadStringSetting(api, SettingStringNickname, edit.nickname, sizeof(edit.nickname));
    LoadStringSetting(api, SettingStringLastJoinAddress, edit.joinAddress, sizeof(edit.joinAddress));
    LoadStringSetting(api, SettingStringHolePunchServer, edit.holePunchServer, sizeof(edit.holePunchServer));
    LoadStringSetting(api, SettingStringBaseUrl, edit.baseUrl, sizeof(edit.baseUrl));
    LoadStringSetting(api, SettingStringProxyBaseUrl, edit.proxyBaseUrl, sizeof(edit.proxyBaseUrl));
    edit.defaultPort = api.settingsGetInt(SettingIntDefaultPort);
    if (edit.defaultPort <= 0)
    {
        edit.defaultPort = 10800;
    }
    edit.loadedFromSettings = true;
}

bool DrawSettingCheckbox(const ModApi& api, int settingId, const char* label)
{
    const bool ready = SettingsReady(api);
    bool value = ready ? (api.settingsGetBool(settingId) != 0) : false;
    ImGui::BeginDisabled(!ready);
    const bool changed = ImGui::Checkbox(label, &value);
    if (changed)
    {
        api.settingsSetBool(settingId, value ? 1 : 0);
    }
    ImGui::EndDisabled();
    return changed;
}

void DrawSettingSlider(const ModApi& api, int settingId, const char* label, int minValue, int maxValue, const char* format)
{
    const bool ready = SettingsReady(api);
    int value = ready ? api.settingsGetInt(settingId) : 0;
    ImGui::BeginDisabled(!ready);
    if (ImGui::SliderInt(label, &value, minValue, maxValue, format))
    {
        api.settingsSetInt(settingId, value);
    }
    ImGui::EndDisabled();
}

void DrawHotkeyInt(const ModApi& api, int settingId, const char* label)
{
    const bool ready = SettingsReady(api);
    int value = ready ? api.settingsGetInt(settingId) : 0;
    ImGui::BeginDisabled(!ready);
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(90.0f);
    int edited = value;
    if (ImGui::InputInt("VK", &edited, 1, 16, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (edited < 0) edited = 0;
        if (edited > 0xFE) edited = 0xFE;
        api.settingsSetInt(settingId, edited);
        value = edited;
    }
    ImGui::SameLine();
    ImGui::Text("0x%02X", static_cast<unsigned>(value & 0xFF));
    ImGui::PopID();
    ImGui::EndDisabled();
}

void DrawHudPosRow(const ModApi& api, int elementIndex, const char* label)
{
    const bool ready = SettingsReady(api);
    const int idX = SettingIntHudPosBase + elementIndex * 2;
    const int idY = idX + 1;
    int x = ready ? api.settingsGetInt(idX) : -1;
    int y = ready ? api.settingsGetInt(idY) : -1;

    ImGui::BeginDisabled(!ready);
    ImGui::PushID(elementIndex);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::InputInt("X", &x, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (x < -1) x = -1;
        api.settingsSetInt(idX, x);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::InputInt("Y", &y, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (y < -1) y = -1;
        api.settingsSetInt(idY, y);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset"))
    {
        api.settingsSetInt(idX, -1);
        api.settingsSetInt(idY, -1);
    }
    ImGui::PopID();
    ImGui::EndDisabled();
}

const char* RoleName(int role)
{
    switch (role)
    {
    case 0: return "Host";
    case 1: return "Join";
    case 2: return "Spectate";
    default: return "None";
    }
}

void DrawDisplayTab(const EfzDx9PresentParams& params)
{
    if (params.linearScale != nullptr)
    {
        bool linearScale = *params.linearScale != FALSE;
        if (ImGui::Checkbox("Smooth scaling", &linearScale))
        {
            *params.linearScale = linearScale ? TRUE : FALSE;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Blurs the image slightly instead of keeping hard pixel edges.");
        }
    }

    static const char *modes[] = {
        "Windowed", "Borderless fullscreen", "Exclusive fullscreen" };
    int requestedMode = static_cast<int>(params.requestedMode);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Display mode", &requestedMode, modes,
                     IM_ARRAYSIZE(modes))
        && params.requestPresentationMode != nullptr)
    {
        params.requestPresentationMode(
            static_cast<QohPresentationMode>(requestedMode),
            params.requestPresentationModeUserData);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            requestedMode == QOH_PRESENTATION_EXCLUSIVE
                ? "Uses a real Direct3D 9 fullscreen device at the selected "
                  "monitor's current resolution and refresh. Alt-Tab may "
                  "temporarily blank the game while the device is reacquired; "
                  "latency improvements depend on the system, and Windows may "
                  "still apply Fullscreen Optimizations."
                : "The configured presentation hotkey toggles Windowed and "
                  "your last selected fullscreen mode.");
    }

    const char *backendNames[] = {
        "Auto", "Classic D3D9", "D3D9Ex FlipEx (experimental)", "GDI" };
    const char *readinessNames[] = {
        "Not requested", "Pending", "Ready", "Fallback ready", "Failed" };
    ImGui::Text("Requested: %s", modes[params.requestedMode]);
    ImGui::Text("Effective: %s", modes[g_presenter.effectiveMode]);
    ImGui::Text("Requested renderer: %s",
        backendNames[g_presenter.requestedBackend]);
    ImGui::Text("Effective renderer: %s",
        backendNames[g_presenter.effectiveBackend]);
    ImGui::Text("Readiness: %s", readinessNames[g_presenter.readiness]);
    ImGui::Text("Adapter %u: %s", g_presenter.adapterOrdinal,
        g_presenter.adapterDescription[0]
            ? g_presenter.adapterDescription : "unavailable");
    ImGui::Text("Monitor: %p", static_cast<void *>(g_presenter.monitor));
    ImGui::Text("Backbuffer: %u x %u @ %u Hz",
        g_presenter.presentParams.BackBufferWidth,
        g_presenter.presentParams.BackBufferHeight,
        g_presenter.presentParams.FullScreen_RefreshRateInHz);
    if (g_presenter.fallbackReason[0])
        ImGui::TextWrapped("Fallback: %s", g_presenter.fallbackReason);

    ImGui::Text("Window: %d x %d", params.clientWidth, params.clientHeight);
    ImGui::Text("Image: %d x %d at %d, %d",
        params.dstW, params.dstH, params.dstX, params.dstY);
}

void DrawWindowDetails(const EfzDx9PresentParams& params)
{
    ImGui::SeparatorText(qi18n::tr("Window"));
    ImGui::Text("Present: %lu", static_cast<unsigned long>(params.presentSerial));
    ImGui::Text("Focus serial: %lu", static_cast<unsigned long>(params.focusSerial));
    ImGui::Text("Source: %d x %d (pitch %d)", params.width, params.height, params.pitch);
    ImGui::Text("Overlay device: %d x %d  rebuilds %lu  resets %lu",
        static_cast<int>(g_presenter.presentParams.BackBufferWidth),
        static_cast<int>(g_presenter.presentParams.BackBufferHeight),
        static_cast<unsigned long>(g_diag.creates),
        static_cast<unsigned long>(g_diag.resets));
    ImGui::Text("Failures %lu  losses %lu  nested presents %lu  last 0x%08lX",
        static_cast<unsigned long>(g_diag.failures),
        static_cast<unsigned long>(g_diag.losses),
        static_cast<unsigned long>(g_diag.reentrantPresents),
        static_cast<unsigned long>(g_diag.lastError));
    ImGui::Checkbox("ImGui demo window", &g_presenter.showDemoWindow);
}

void DrawSessionTab(const ModApi& api, DebugMenuEditState& edit)
{
    const bool ready = SessionReady(api);
    if (!ready)
    {
        ImGui::TextDisabled("Session API unavailable.");
    }

    BmeSessionStatus status = {};
    if (ready && api.sessionGetStatus(&status) != 0)
    {
        ImGui::Text("Phase: %s", status.phaseName[0] ? status.phaseName : "?");
        ImGui::SameLine(210.0f);
        ImGui::Text("Role: %s", RoleName(status.role));
        ImGui::Text("Ping: %d ms", status.pingMs);
        ImGui::SameLine(150.0f);
        ImGui::Text("Delay: %d F", status.delayFrames);
        ImGui::SameLine(270.0f);
        ImGui::Text("Wins: %d - %d", status.p1Wins, status.p2Wins);
        if (status.port != 0 || status.localPort != 0)
        {
            ImGui::Text("Target port: %u", static_cast<unsigned>(status.port));
            ImGui::SameLine(180.0f);
            ImGui::Text("Local UDP: %u", static_cast<unsigned>(status.localPort));
        }
        if (status.errorMsg[0] != '\0')
        {
            ImGui::TextWrapped("Error: %s", status.errorMsg);
        }
    }

    ImGui::SeparatorText(qi18n::tr("Connect"));
    ImGui::BeginDisabled(!ready || api.sessionStart == nullptr);
    ImGui::InputText("Nickname", edit.nickname, sizeof(edit.nickname));
    ImGui::InputText("Address", edit.joinAddress, sizeof(edit.joinAddress));
    ImGui::InputInt("Port", &edit.defaultPort);
    if (edit.defaultPort < 1) edit.defaultPort = 1;
    if (edit.defaultPort > 65535) edit.defaultPort = 65535;

    if (ImGui::Button("Host"))
    {
        api.sessionStart(0, static_cast<uint16_t>(edit.defaultPort), "", edit.nickname);
    }
    ImGui::SameLine();
    if (ImGui::Button("Join"))
    {
        api.sessionStart(1, static_cast<uint16_t>(edit.defaultPort), edit.joinAddress, edit.nickname);
    }
    ImGui::SameLine();
    if (ImGui::Button("Spectate"))
    {
        api.sessionStart(2, static_cast<uint16_t>(edit.defaultPort), edit.joinAddress, edit.nickname);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready || api.sessionCancel == nullptr);
    if (ImGui::Button("Cancel"))
    {
        api.sessionCancel("f1-menu");
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText(qi18n::tr("Delay"));
    BmeDelayMetrics metrics = {};
    if (ready && api.sessionGetDelayMetrics != nullptr && api.sessionGetDelayMetrics(&metrics) != 0)
    {
        ImGui::Text("RTT avg/min/max: %d / %d / %d ms", metrics.averagePingMs, metrics.minPingMs, metrics.maxPingMs);
        ImGui::Text("Recommended: %d F", metrics.recommendedDelay);
        if (edit.sessionDelay < metrics.minDelay || edit.sessionDelay > metrics.maxDelay)
        {
            edit.sessionDelay = metrics.recommendedDelay;
        }
        ImGui::BeginDisabled(api.sessionApplyDelay == nullptr);
        if (ImGui::SliderInt("Gameplay delay", &edit.sessionDelay, metrics.minDelay, metrics.maxDelay, "%d F"))
        {
            api.sessionApplyDelay(edit.sessionDelay);
        }
        ImGui::EndDisabled();
    }
    else
    {
        ImGui::TextDisabled("Delay metrics unavailable.");
    }

    ImGui::SeparatorText(qi18n::tr("Runtime"));
    BmeMatchStats stats = {};
    if (ready && api.sessionGetMatchStats != nullptr && api.sessionGetMatchStats(&stats) != 0)
    {
        ImGui::Text("Frames local/peer: %llu / %llu",
            static_cast<unsigned long long>(stats.localFrame),
            static_cast<unsigned long long>(stats.peerFrame));
        ImGui::Text("Lead: %lld    Mispredicts: %llu",
            static_cast<long long>(stats.inputLead),
            static_cast<unsigned long long>(stats.mispredictCount));
        ImGui::Text("Rollback req/exe/abort: %llu / %llu / %llu",
            static_cast<unsigned long long>(stats.rollbackRequests),
            static_cast<unsigned long long>(stats.rollbackExecuted),
            static_cast<unsigned long long>(stats.rollbackAborted));
        ImGui::Text("Replayed: %llu    Last: %u",
            static_cast<unsigned long long>(stats.framesReplayed),
            static_cast<unsigned>(stats.lastReplayLength));
    }

    if (api.sessionGetEndReasonName != nullptr)
    {
        const char* reason = api.sessionGetEndReasonName();
        ImGui::Text("End reason: %s", reason != nullptr ? reason : "?");
    }

    ImGui::SeparatorText(qi18n::tr("Recent events"));
    ImGui::BeginChild("recent_events", ImVec2(0.0f, 145.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    static char events[8192] = {};
    events[0] = '\0';
    if (ready && api.sessionGetRecentEvents != nullptr)
    {
        api.sessionGetRecentEvents(events, static_cast<int>(sizeof(events)));
    }
    if (events[0] != '\0')
    {
        ImGui::TextUnformatted(events);
    }
    else
    {
        ImGui::TextDisabled("No events.");
    }
    ImGui::EndChild();
}

void DrawSettingsTab(const ModApi& api, DebugMenuEditState& edit)
{
    if (!SettingsReady(api))
    {
        ImGui::TextDisabled("Settings API unavailable.");
        return;
    }

    if (ImGui::Button("Reload from INI"))
    {
        if (api.settingsReload != nullptr)
        {
            api.settingsReload();
        }
        LoadEditStateFromSettings(api, edit);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        if (api.settingsSave != nullptr)
        {
            api.settingsSave();
        }
    }

    if (ImGui::CollapsingHeader("Profile", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputText("Nickname", edit.nickname, sizeof(edit.nickname));
        if (ImGui::Button("Apply profile"))
        {
            if (api.settingsSetString != nullptr)
            {
                api.settingsSetString(SettingStringNickname, edit.nickname);
            }
        }
    }

    if (ImGui::CollapsingHeader("Netplay", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSettingCheckbox(api, SettingBoolApplyRemotePalettes, "Apply remote palettes");
        DrawSettingCheckbox(api, SettingBoolNonArcadeRematch, "Non-arcade rematch");
        DrawSettingCheckbox(api, SettingBoolAllowPracticeKeys, "Allow practice keys");
        DrawSettingCheckbox(api, SettingBoolDeepTelemetry, "Deep telemetry logs");
    }

    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSettingSlider(api, SettingIntBgmVolume, "BGM volume", 0, 100, "%d%%");
        DrawSettingSlider(api, SettingIntSeVolume, "SE volume", 0, 100, "%d%%");
    }

    if (ImGui::CollapsingHeader("HUD", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSettingSlider(api, SettingIntHudFontScale, "HUD font scale", 50, 300, "%d%%");
        DrawSettingCheckbox(api, SettingBoolHudPreview, "Preview HUD offline");
        DrawSettingCheckbox(api, SettingBoolHudRollbackMetrics, "Rollback/desync line");

        if (ImGui::TreeNodeEx("Layout (per element)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextDisabled("Virtual 640x480. -1 = auto. Enter to apply.");
            DrawHudPosRow(api, 0, "P1 name");
            DrawHudPosRow(api, 1, "P2 name");
            DrawHudPosRow(api, 2, "Ping");
            DrawHudPosRow(api, 3, "Delay");
            DrawHudPosRow(api, 4, "Rollback");
            DrawHudPosRow(api, 5, "Status");
            DrawHudPosRow(api, 6, "Practice");
            DrawHudPosRow(api, 7, "Toasts");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Network"))
    {
        ImGui::InputText("Join address", edit.joinAddress, sizeof(edit.joinAddress));
        ImGui::InputText("Hole-punch server", edit.holePunchServer, sizeof(edit.holePunchServer));
        ImGui::InputText("Base URL", edit.baseUrl, sizeof(edit.baseUrl));
        ImGui::InputText("Proxy Base URL", edit.proxyBaseUrl, sizeof(edit.proxyBaseUrl));
        ImGui::InputInt("Default port", &edit.defaultPort);
        if (edit.defaultPort < 1) edit.defaultPort = 1;
        if (edit.defaultPort > 65535) edit.defaultPort = 65535;
        DrawSettingCheckbox(api, SettingBoolEnableHolePunch, "Hole punch");
        DrawSettingCheckbox(api, SettingBoolEnableUpnp, "UPnP");
        if (ImGui::Button("Apply network"))
        {
            if (api.settingsSetString != nullptr)
            {
                api.settingsSetString(SettingStringLastJoinAddress, edit.joinAddress);
                api.settingsSetString(SettingStringHolePunchServer, edit.holePunchServer);
                api.settingsSetString(SettingStringBaseUrl, edit.baseUrl);
                api.settingsSetString(SettingStringProxyBaseUrl, edit.proxyBaseUrl);
            }
            if (api.settingsSetInt != nullptr)
            {
                api.settingsSetInt(SettingIntDefaultPort, edit.defaultPort);
            }
        }
    }

    if (ImGui::CollapsingHeader("Logging"))
    {
        DrawSettingCheckbox(api, SettingBoolFileLogging, "File logging");
        DrawSettingCheckbox(api, SettingBoolConsole, "Console");
        DrawSettingCheckbox(api, SettingBoolPreserveModLog, "Preserve mod log");
    }

    if (ImGui::CollapsingHeader("Transport"))
    {
        DrawSettingCheckbox(api, SettingBoolTlsVerify, "TLS verify");
        DrawSettingCheckbox(api, SettingBoolForceWinInet, "Force WinInet");
        DrawSettingCheckbox(api, SettingBoolForceEmbeddedTls, "Force embedded TLS");
    }

    if (ImGui::CollapsingHeader("Hotkeys"))
    {
        DrawHotkeyInt(api, SettingIntSavestateSaveKey, "Savestate save");
        DrawHotkeyInt(api, SettingIntSavestateLoadKey, "Savestate load");
        DrawHotkeyInt(api, SettingIntPlayerSwapKey, "Player swap");
        DrawHotkeyInt(api, SettingIntFramestepPauseKey, "Framestep pause");
        DrawHotkeyInt(api, SettingIntFramestepStepKey, "Framestep step");
        DrawHotkeyInt(api, SettingIntDelayDecreaseKey, "Delay decrease");
        DrawHotkeyInt(api, SettingIntDelayIncreaseKey, "Delay increase");
        DrawHotkeyInt(api, SettingIntDelayPlayerToggleKey, "Delay player toggle");
        DrawHotkeyInt(api, SettingIntCollisionHitKey, "Hitbox layer");
        DrawHotkeyInt(api, SettingIntCollisionHurtKey, "Hurtbox layer");
        DrawHotkeyInt(api, SettingIntCollisionBoxKey, "Collision layer");
    }
}

void DrawTestingTab(const ModApi& api)
{
    DrawSettingCheckbox(api, SettingBoolHarnessShm, "Harness shared memory");
    DrawSettingCheckbox(api, SettingBoolStressTest, "Determinism stress test");

    ImGui::SeparatorText(qi18n::tr("Bot match"));
    const bool ready = BotReady(api);
    if (!ready)
    {
        ImGui::TextDisabled("Bot API unavailable.");
        return;
    }

    static const char* kPersonalities[] = { "Neutral Poker", "Block Trainer", "Idle Dummy", "Smart Fighter" };
    static int s_p1Personality = 0;
    static int s_p2Personality = 1;

    const bool p1Active = api.isBotActive(0) != 0;
    const bool p2Active = api.isBotActive(1) != 0;

    ImGui::PushID("p1_bot");
    ImGui::Combo("P1 personality", &s_p1Personality, kPersonalities, IM_ARRAYSIZE(kPersonalities));
    bool p1Toggle = p1Active;
    if (ImGui::Checkbox("P1 bot active", &p1Toggle))
    {
        api.setBotActive(0, p1Toggle ? 1 : 0, s_p1Personality);
    }
    ImGui::PopID();

    ImGui::PushID("p2_bot");
    ImGui::Combo("P2 personality", &s_p2Personality, kPersonalities, IM_ARRAYSIZE(kPersonalities));
    bool p2Toggle = p2Active;
    if (ImGui::Checkbox("P2 bot active", &p2Toggle))
    {
        api.setBotActive(1, p2Toggle ? 1 : 0, s_p2Personality);
    }
    ImGui::PopID();

    if (api.isBotLogging != nullptr && api.setBotLogging != nullptr)
    {
        bool logToggle = api.isBotLogging() != 0;
        if (ImGui::Checkbox("Desync CSV logging", &logToggle))
        {
            api.setBotLogging(logToggle ? 1 : 0);
        }
    }

    if (api.getLastFrame != nullptr)
    {
        uint64_t lastFrame = 0;
        uint32_t lastHash = 0;
        uint8_t p1In = 0;
        uint8_t p2In = 0;
        const int valid = api.getLastFrame(&lastFrame, &lastHash, &p1In, &p2In);
        if (valid)
        {
            ImGui::Text("Frame %llu  hash 0x%08X",
                static_cast<unsigned long long>(lastFrame),
                static_cast<unsigned>(lastHash));
            ImGui::Text("P1 in: 0x%02X    P2 in: 0x%02X",
                static_cast<unsigned>(p1In),
                static_cast<unsigned>(p2In));
        }
        else
        {
            ImGui::TextDisabled("No sampled frame.");
        }
    }

    if (api.getInputStatus != nullptr)
    {
        static const char* kSourceNames[] = { "Idle", "Local", "Bot", "Replay", "Remote" };
        uint8_t resolved = 0;
        uint8_t source = 0;
        api.getInputStatus(0, &resolved, &source);
        const char* srcName = (source < IM_ARRAYSIZE(kSourceNames)) ? kSourceNames[source] : "?";
        ImGui::Text("P1 resolved: 0x%02X (%s)", resolved, srcName);
        api.getInputStatus(1, &resolved, &source);
        srcName = (source < IM_ARRAYSIZE(kSourceNames)) ? kSourceNames[source] : "?";
        ImGui::Text("P2 resolved: 0x%02X (%s)", resolved, srcName);
    }
}

void HudPosRow(const char *label, int &storeX, int &storeY,
               int defX, int defY, const char *keyX, const char *keyY)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    int x = (storeX < 0) ? defX : storeX;
    int y = (storeY < 0) ? defY : storeY;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("X", &x, 0, 640))
    {
        storeX = x;
        HudIniSet(keyX, x);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("Y", &y, 0, 480))
    {
        storeY = y;
        HudIniSet(keyY, y);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset"))
    {
        storeX = -1;
        storeY = -1;
        HudIniSet(keyX, -1);
        HudIniSet(keyY, -1);
    }
    ImGui::PopID();
}

void DrawHudLayoutTab(const EfzDx9PresentParams &params)
{
    (void)params;
    HudLayoutEnsureLoaded();

    ImGui::TextWrapped(
        "Move the online overlay around - player names and the "
        "ping / delay / rollback line. Positions are in the game's 640x480 "
        "screen and are remembered between sessions. This only changes what "
        "you see; it never affects the match itself.");
    ImGui::Separator();

    ImGui::Checkbox("Show a sample overlay", &g_hudPreview);
    ImGui::TextDisabled("  Draws example names and a ping line so you can "
                        "place them without being in a match.");

    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputText("Player 1 nickname", g_hudPreviewPlayerOne,
                     sizeof(g_hudPreviewPlayerOne));
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputText("Player 2 nickname", g_hudPreviewPlayerTwo,
                     sizeof(g_hudPreviewPlayerTwo));
    ImGui::TextDisabled("  UTF-8 preview only. These names are never saved or sent.");
    ImGui::Text("Cyrillic: %s    Japanese: %s",
                g_overlayFontStatus.cyrillic ? "ready" : "font missing",
                g_overlayFontStatus.japanese ? "ready" : "font missing");
    if (!g_overlayFontStatus.japanese)
    {
        ImGui::TextDisabled("  Install a Japanese/CJK font, then restart the game.");
    }

    ImGui::SeparatorText(qi18n::tr("Menu size"));
    int menuScalePct = g_overlayUiSettings.menuFontScalePct;
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::SliderInt(
            "Menu scale", &menuScalePct,
            kMenuMinFontScalePct, kMenuMaxFontScalePct, "%d%%"))
    {
        g_overlayUiSettings.menuFontScalePct = menuScalePct;
        ImGui::GetIO().FontGlobalScale =
            static_cast<float>(menuScalePct) / 100.0f;
        OverlayIniSet("ImGuiFontScalePct", menuScalePct);
    }
    ImGui::TextDisabled("  Changes all text and controls in this menu.");

    ImGui::SeparatorText(qi18n::tr("In-game text size"));
    int scalePct = g_hudLayout.fontScalePct;
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::SliderInt(
            "Nickname scale", &scalePct, 50, 300, "%d%%"))
    {
        g_hudLayout.fontScalePct = scalePct;
        HudIniSet("FontScalePct", scalePct);
    }
    ImGui::TextDisabled(
        "  Changes names, match information and replay status in-game.");

    ImGui::Separator();
    HudPosRow("Player 1 name", g_hudLayout.p1x, g_hudLayout.p1y,
              kHudDefP1X, kHudDefP1Y, "P1NameX", "P1NameY");
    HudPosRow("Player 2 name", g_hudLayout.p2x, g_hudLayout.p2y,
              kHudDefP2X, kHudDefP2Y, "P2NameX", "P2NameY");
    HudPosRow("Ping / delay / rollback line", g_hudLayout.sx, g_hudLayout.sy,
              kHudDefSessionX, kHudDefSessionY, "SessionX", "SessionY");
    ImGui::TextDisabled("  Ping, delay and rollback share one line; this moves "
                        "all three together.");
    ImGui::TextDisabled("  A replay's frame counter and speed use this same "
                        "spot. They never show at the same time.");
    HudPosRow("Match header", g_hudLayout.hx, g_hudLayout.hy,
              kHudDefHeaderX, kHudDefHeaderY, "HeaderX", "HeaderY");

    ImGui::Separator();
    if (ImGui::Button("Reset all to defaults"))
    {
        g_overlayUiSettings.menuFontScalePct = kMenuDefFontScalePct;
        ImGui::GetIO().FontGlobalScale =
            static_cast<float>(kMenuDefFontScalePct) / 100.0f;
        OverlayIniSet("ImGuiFontScalePct", kMenuDefFontScalePct);
        g_hudLayout.fontScalePct = kHudDefFontScalePct;
        HudIniSet("FontScalePct", kHudDefFontScalePct);
        g_hudLayout.p1x = g_hudLayout.p1y = -1;
        HudIniSet("P1NameX", -1);
        HudIniSet("P1NameY", -1);
        g_hudLayout.p2x = g_hudLayout.p2y = -1;
        HudIniSet("P2NameX", -1);
        HudIniSet("P2NameY", -1);
        g_hudLayout.sx = g_hudLayout.sy = -1;
        HudIniSet("SessionX", -1);
        HudIniSet("SessionY", -1);
        g_hudLayout.hx = g_hudLayout.hy = -1;
        HudIniSet("HeaderX", -1);
        HudIniSet("HeaderY", -1);
    }
    ImGui::TextDisabled("Hiding the game's SCORE/TIME display is set up in the "
                        "launcher and only applies online.");
}

bool BeginTabBody(const char *id)
{
    return ImGui::BeginChild(
        id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
}

void EndTabBody()
{
    ImGui::EndChild();
}

void TrainingTextDisabledWrapped(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(
        ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void TrainingTextColoredWrapped(
    const ImVec4 &color, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void TrainingBulletTextWrapped(const char *format, ...)
{
    ImGui::Bullet();
    va_list args;
    va_start(args, format);
    ImGui::TextWrappedV(format, args);
    va_end(args);
}

const char *TrainingDisableReasonName(DWORD reason)
{
    switch (reason)
    {
    case QOH99_TRAINING_DISABLE_NONE: return "None";
    case QOH99_TRAINING_DISABLE_NOT_TRAINING_LAUNCH: return "this isn't a Training session";
    case QOH99_TRAINING_DISABLE_ONLINE_MODE: return "an online match is running";
    case QOH99_TRAINING_DISABLE_SPECTATOR_MODE: return "you're spectating";
    case QOH99_TRAINING_DISABLE_SELFTEST_OR_PROBE: return "a self-test is running";
    case QOH99_TRAINING_DISABLE_UNSUPPORTED_BUILD: return "this game version isn't supported";
    case QOH99_TRAINING_DISABLE_UNSUPPORTED_STEP_BOUNDARY: return "this game version isn't supported";
    case QOH99_TRAINING_DISABLE_UNSAFE_SCENE: return "you're not in a match";
    case QOH99_TRAINING_DISABLE_LAYOUT_INVALID: return "the match isn't ready yet";
    case QOH99_TRAINING_DISABLE_INVARIANT_FAILURE: return "training hit a problem and stopped itself";
    case QOH99_TRAINING_DISABLE_SNAPSHOT_FAILURE: return "training hit a problem and stopped itself";
    default: return "unknown";
    }
}

const char *TrainingLifecycleName(DWORD lifecycle)
{
    switch (lifecycle)
    {
    case QOH99_TRAINING_LIFECYCLE_OFF: return "OFF";
    case QOH99_TRAINING_LIFECYCLE_LAUNCH_REQUESTED: return "STARTING UP";
    case QOH99_TRAINING_LIFECYCLE_CHARACTER_SELECT: return "CHARACTER SELECT";
    case QOH99_TRAINING_LIFECYCLE_MATCH_INITIALIZING: return "LOADING MATCH";
    case QOH99_TRAINING_LIFECYCLE_ACTIVE: return "TRAINING";
    case QOH99_TRAINING_LIFECYCLE_REINITIALIZING: return "RESTARTING";
    case QOH99_TRAINING_LIFECYCLE_RETURNING_TO_SELECT: return "BACK TO SELECT";
    case QOH99_TRAINING_LIFECYCLE_FAILED: return "NOT AVAILABLE";
    default: return "UNKNOWN";
    }
}

ImVec4 TrainingLifecycleColor(DWORD lifecycle)
{
    switch (lifecycle)
    {
    case QOH99_TRAINING_LIFECYCLE_ACTIVE:
        return ImVec4(0.35f, 0.90f, 0.48f, 1.0f);
    case QOH99_TRAINING_LIFECYCLE_CHARACTER_SELECT:
        return ImVec4(0.35f, 0.78f, 1.0f, 1.0f);
    case QOH99_TRAINING_LIFECYCLE_FAILED:
        return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
    case QOH99_TRAINING_LIFECYCLE_MATCH_INITIALIZING:
    case QOH99_TRAINING_LIFECYCLE_REINITIALIZING:
    case QOH99_TRAINING_LIFECYCLE_RETURNING_TO_SELECT:
        return ImVec4(1.0f, 0.74f, 0.30f, 1.0f);
    default:
        return ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
    }
}

const char *TrainingAdmissionName(DWORD result)
{
    switch (result)
    {
    case QOH99_TRAINING_ADMISSION_NONE: return "No admitted step yet";
    case QOH99_TRAINING_ADMISSION_ADMITTED: return "Admitted";
    case QOH99_TRAINING_ADMISSION_NATIVE_RETURNED_ZERO: return "Native return 0";
    case QOH99_TRAINING_ADMISSION_NATIVE_RETURNED_ONE: return "Native return 1";
    case QOH99_TRAINING_ADMISSION_WRONG_SCENE: return "Wrong scene";
    case QOH99_TRAINING_ADMISSION_WRONG_MATCH_SUBSTATE: return "Wrong match substate";
    case QOH99_TRAINING_ADMISSION_WAIT_COUNT_BLOCKED: return "WaitCount blocked";
    case QOH99_TRAINING_ADMISSION_PAUSED: return "Paused";
    case QOH99_TRAINING_ADMISSION_FROZEN: return "Frozen";
    case QOH99_TRAINING_ADMISSION_REPLAY_OR_SPECTATOR: return "Replay/spectator rejected";
    case QOH99_TRAINING_ADMISSION_INVARIANT_FAILED: return "Invariant failed";
    default: return "Unknown";
    }
}

const char *TrainingCommandResultName(DWORD result)
{
    switch (result)
    {
    case QOH99_TRAINING_COMMAND_RESULT_NONE: return "nothing happened";
    case QOH99_TRAINING_COMMAND_RESULT_APPLIED: return "done";
    case QOH99_TRAINING_COMMAND_RESULT_REJECTED_UNAVAILABLE:
        return "that isn't available right now";
    case QOH99_TRAINING_COMMAND_RESULT_REJECTED_UNSAFE_SCENE:
        return "you're not in a match";
    case QOH99_TRAINING_COMMAND_RESULT_REJECTED_INVALID_SLOT:
        return "that slot can't be used";
    case QOH99_TRAINING_COMMAND_RESULT_REJECTED_INVALID_SETTING:
        return "that setting can't be used";
    case QOH99_TRAINING_COMMAND_RESULT_REJECTED_STALE_LAYOUT:
        return "it was saved in a different match";
    case QOH99_TRAINING_COMMAND_RESULT_NATIVE_VALIDATION_FAILED:
        return "the game refused it";
    case QOH99_TRAINING_COMMAND_RESULT_SNAPSHOT_FAILED:
        return "the save couldn't be taken";
    default: return "unknown";
    }
}

const char *TrainingMacroStateName(DWORD state)
{
    switch (state)
    {
    case QOH99_TRAINING_MACRO_IDLE: return "Ready";
    case QOH99_TRAINING_MACRO_RECORD_ARMED: return "Recording starts on your next input";
    case QOH99_TRAINING_MACRO_RECORDING: return "Recording";
    case QOH99_TRAINING_MACRO_RECORD_STOPPING: return "Finishing the recording";
    case QOH99_TRAINING_MACRO_PLAY_ARMED: return "Playback about to start";
    case QOH99_TRAINING_MACRO_PLAY_STOPPING: return "Stopping playback";
    case QOH99_TRAINING_MACRO_PLAYING: return "Playing back";
    default: return "Unknown";
    }
}

const char *TrainingInputRearmName(DWORD state)
{
    switch (state)
    {
    case QOH99_TRAINING_INPUT_ARMED: return "Armed";
    case QOH99_TRAINING_INPUT_CAPTURED: return "Menu captured";
    case QOH99_TRAINING_INPUT_AWAIT_NEUTRAL: return "Awaiting neutral";
    case QOH99_TRAINING_INPUT_NEUTRAL_SETTLE: return "Neutral settle";
    default: return "Unknown";
    }
}

const char *TrainingGuardClassName(DWORD classification)
{
    switch (classification)
    {
    case QOH99_TRAINING_GUARD_CLASS_UNGUARDABLE: return "Unguardable / none";
    case QOH99_TRAINING_GUARD_CLASS_STAND: return "Stand";
    case QOH99_TRAINING_GUARD_CLASS_CROUCH: return "Crouch";
    case QOH99_TRAINING_GUARD_CLASS_EITHER: return "Either";
    case QOH99_TRAINING_GUARD_CLASS_AIR: return "Air";
    case QOH99_TRAINING_GUARD_CLASS_INVALID: return "Invalid";
    default: return "Unknown";
    }
}

const char *TrainingPostureClassName(DWORD posture)
{
    switch (posture)
    {
    case QOH99_TRAINING_POSTURE_NOT_GUARDABLE: return "no guard requested";
    case QOH99_TRAINING_POSTURE_STANDING: return "standing (grounded)";
    case QOH99_TRAINING_POSTURE_CROUCHING: return "crouching (grounded)";
    case QOH99_TRAINING_POSTURE_AIR: return "airborne";
    case QOH99_TRAINING_POSTURE_UNREADABLE: return "unreadable";
    default: return "unknown";
    }
}

const char *TrainingResourceTriggerName(DWORD trigger)
{
    switch (trigger)
    {
    case QOH99_TRAINING_TRIGGER_OFF: return "Off";
    case QOH99_TRAINING_TRIGGER_AFTER_COMBO: return "After a combo ends";
    case QOH99_TRAINING_TRIGGER_AFTER_RECOVERY: return "After recovering";
    case QOH99_TRAINING_TRIGGER_WHEN_NEUTRAL: return "When both are neutral";
    case QOH99_TRAINING_TRIGGER_ON_RESET_ONLY: return "On reset only";
    default: return "Unknown";
    }
}

const char *TrainingRecoveryModeName(DWORD mode)
{
    switch (mode)
    {
    case QOH99_TRAINING_RECOVERY_OFF: return "Off";
    case QOH99_TRAINING_RECOVERY_EARLIEST: return "Earliest";
    case QOH99_TRAINING_RECOVERY_FIXED_DELAY: return "Fixed delay";
    case QOH99_TRAINING_RECOVERY_RANDOM_DELAY: return "Random delay";
    case QOH99_TRAINING_RECOVERY_LATE: return "Late";
    default: return "Unknown";
    }
}

const char *TrainingRecoveryStateName(DWORD state)
{
    switch (state)
    {
    case QOH99_TRAINING_RECOVERY_STATE_IDLE: return "idle";
    case QOH99_TRAINING_RECOVERY_STATE_WAIT_ELIGIBLE: return "waiting to become eligible";
    case QOH99_TRAINING_RECOVERY_STATE_DELAY: return "counting the delay";
    case QOH99_TRAINING_RECOVERY_STATE_PRE_NEUTRAL: return "releasing to neutral";
    case QOH99_TRAINING_RECOVERY_STATE_TAP1: return "first tap";
    case QOH99_TRAINING_RECOVERY_STATE_NEUTRAL: return "neutral between taps";
    case QOH99_TRAINING_RECOVERY_STATE_TAP2: return "second tap";
    case QOH99_TRAINING_RECOVERY_STATE_AWAIT: return "waiting for the game to take it";
    case QOH99_TRAINING_RECOVERY_STATE_OBSERVING_BUFFER: return "taps still buffered";
    case QOH99_TRAINING_RECOVERY_STATE_COOLDOWN: return "cooling down";
    default: return "unknown";
    }
}

const char *TrainingRecoveryVerdictName(DWORD verdict)
{
    switch (verdict)
    {
    case QOH99_TRAINING_RECOVERY_VERDICT_NONE: return "nothing yet";
    case QOH99_TRAINING_RECOVERY_VERDICT_SUCCEEDED: return "recovered";
    case QOH99_TRAINING_RECOVERY_VERDICT_MISSED: return "missed (window expired)";
    case QOH99_TRAINING_RECOVERY_VERDICT_CANCELLED: return "cancelled";
    case QOH99_TRAINING_RECOVERY_VERDICT_BUFFERED:
        return "recovered from buffered input - ours or the game's, can't tell";
    case QOH99_TRAINING_RECOVERY_VERDICT_AUTOMATIC:
        return "the game recovered on its own";
    case QOH99_TRAINING_RECOVERY_VERDICT_COMMIT_BLOCKED:
        return "the game accepted the input but refused the transition";
    default: return "unknown";
    }
}

const char *TrainingRecoveryBranchName(DWORD branch)
{
    switch (branch)
    {
    case QOH99_TRAINING_RECOVERY_BRANCH_AIR_22: return "Air (down, down)";
    case QOH99_TRAINING_RECOVERY_BRANCH_GROUND_88: return "Ground (up, up)";
    case QOH99_TRAINING_RECOVERY_BRANCH_WALL_66: return "Wall (forward, forward)";
    case QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_66: return "Terminal forward";
    case QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_44: return "Terminal back";
    case QOH99_TRAINING_RECOVERY_BRANCH_THROW_ESCAPE: return "Throw escape (back + C)";
    default: return "unknown";
    }
}

const char *TrainingRecoveryEvaluatorReasonText(DWORD reason)
{
    switch (reason)
    {
    case QOH99_TRAINING_RECOVERY_EVALUATOR_QUALIFIED:
        return "The DLL has verified itself against this game build, so the "
               "recovery options on Dummy > Recovery are live.";
    case QOH99_TRAINING_RECOVERY_EVALUATOR_NOT_ATTEMPTED:
        return "Recovery cannot fire. The injected DLL never got as far as its "
               "build-verification check -- something earlier in training setup "
               "refused -- so it allows no setting except \"Off\" and nothing on "
               "this panel can make the dummy recover. That is the honest state "
               "of the feature, not a fault in this menu and not something a "
               "different setting will fix.";
    case QOH99_TRAINING_RECOVERY_EVALUATOR_ADDRESS_UNRESOLVED:
        return "Recovery is refused: the DLL could not locate the game's own "
               "recovery code in this build, so it will not send input at it.";
    case QOH99_TRAINING_RECOVERY_EVALUATOR_PROLOGUE_MISMATCH:
        return "Recovery is refused: the DLL located the game's recovery code "
               "but the bytes there are not what it expects for this build.";
    default:
        return "Recovery is refused for a reason this menu does not recognise, "
               "which means the DLL is newer than this overlay.";
    }
}

bool TrainingRecoveryEvaluatorAllowsFiring(DWORD reason)
{
    return reason == QOH99_TRAINING_RECOVERY_EVALUATOR_QUALIFIED;
}

const char *TrainingComboEpisodeName(DWORD state)
{
    switch (state)
    {
    case QOH99_TRAINING_COMBO_EPISODE_IDLE: return "idle";
    case QOH99_TRAINING_COMBO_EPISODE_IN_EPISODE: return "being hit";
    case QOH99_TRAINING_COMBO_EPISODE_SETTLING: return "settling";
    default: return "unknown";
    }
}

void TrainingAppendNeutralReasons(
    DWORD mask, char *out, size_t outSize)
{
    struct ReasonName { DWORD bit; const char *text; };
    static const ReasonName kReasons[] = {
        { QOH99_TRAINING_NEUTRAL_FIGHTER_UNREADABLE, "unreadable" },
        { QOH99_TRAINING_NEUTRAL_SLOT_OUT_OF_RANGE, "slot out of range" },
        { QOH99_TRAINING_NEUTRAL_ALTERNATE_FORM, "alternate form active" },
        { QOH99_TRAINING_NEUTRAL_ACTION_NOT_FREE_MOVEMENT,
          "action not idle/walk/crouch" },
        { QOH99_TRAINING_NEUTRAL_DISPATCH_NOT_FREE_MOVEMENT,
          "dispatch not idle/walk/crouch" },
        { QOH99_TRAINING_NEUTRAL_INIT_NOT_RUNNING, "action still starting" },
        { QOH99_TRAINING_NEUTRAL_TRANSITION_PENDING, "transition pending" },
        { QOH99_TRAINING_NEUTRAL_ACTION_LOCK_SET, "lock field set" },
        { QOH99_TRAINING_NEUTRAL_POSTURE_NOT_GROUNDED, "not grounded" },
        { QOH99_TRAINING_NEUTRAL_TERMINAL_LATCHED, "terminal latched" },
        { QOH99_TRAINING_NEUTRAL_ROUND_RESULT_LATCHED, "round result latched" },
        { QOH99_TRAINING_NEUTRAL_FATAL_RECORDED, "fatal recorded" },
        { QOH99_TRAINING_NEUTRAL_COM_OWNED, "CPU owns this side" },
        { QOH99_TRAINING_NEUTRAL_RECOVERY_LOCK_SET, "recovery lock set" },
        { QOH99_TRAINING_NEUTRAL_RECOVERY_PHASE_LATCH, "recovery phase latch" },
        { QOH99_TRAINING_NEUTRAL_COMBO_ACTIVE, "combo counter nonzero" },
        { QOH99_TRAINING_NEUTRAL_DAMAGE_UNSETTLED, "damage still settling" },
        { QOH99_TRAINING_NEUTRAL_ROUND_RESULT_STATE, "round result state" },
        { QOH99_TRAINING_NEUTRAL_FATAL_DISABLE_FLAG, "F4 damage flag set" },
    };
    if (out == nullptr || outSize == 0)
    {
        return;
    }
    out[0] = '\0';
    if (mask == 0)
    {
        std::snprintf(out, outSize, "every clause passed");
        return;
    }
    size_t used = 0;
    for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kReasons)); ++i)
    {
        if ((mask & kReasons[i].bit) == 0)
        {
            continue;
        }
        const int written = std::snprintf(
            out + used, outSize - used, "%s%s",
            used == 0 ? "" : ", ", kReasons[i].text);
        if (written <= 0 || static_cast<size_t>(written) >= outSize - used)
        {
            out[outSize - 1] = '\0';
            return;
        }
        used += static_cast<size_t>(written);
    }
}

bool TrainingFeatureReady(
    const Qoh99TrainingStatusV1 &status,
    DWORD capability)
{
    return status.available != 0
        && status.inMatchReady != 0
        && (status.capabilityFlags & capability) != 0;
}

bool TrainingReturnToSelectReady(
    const Qoh99TrainingStatusV1 &status)
{
    return status.explicitTrainingPresent != 0
        && status.scene == 2
        && (status.capabilityFlags
            & QOH99_TRAINING_CAP_RETURN_TO_SELECT) != 0;
}

void DrawTrainingUnavailableReason(
    const Qoh99TrainingStatusV1 &status,
    DWORD capability)
{
    if ((status.capabilityFlags & capability) == 0)
    {
        TrainingTextDisabledWrapped(
            "Not available in this version of the game.");
    }
    else if (!status.inMatchReady)
    {
        TrainingTextDisabledWrapped("Available once the match has started.");
    }
    else if (!status.available)
    {
        TrainingTextDisabledWrapped("Not available right now - %s.",
            status.disableText[0] != '\0'
                ? status.disableText
                : TrainingDisableReasonName(status.disableReason));
    }
}

void DrawRequestedEffective(
    const char *label,
    DWORD requested,
    DWORD effective)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(290.0f);
    if (requested && effective)
    {
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.48f, 1.0f), "On");
    }
    else if (requested)
    {
        ImGui::TextColored(
            ImVec4(1.0f, 0.40f, 0.35f, 1.0f), "On, but not working");
    }
    else
    {
        ImGui::TextDisabled("Off");
    }
}

void DrawEffectiveInvariant(const char *label, DWORD effective)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(290.0f);
    if (effective)
    {
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.48f, 1.0f), "OK");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.40f, 0.35f, 1.0f), "NOT WORKING");
    }
}

void DrawLatestTrainingCommand(const Qoh99TrainingStatusV1 &status)
{
    const bool lastFailed =
        status.lastAcknowledgedCommandId != 0
        && status.lastCommandResult != QOH99_TRAINING_COMMAND_RESULT_NONE
        && status.lastCommandResult != QOH99_TRAINING_COMMAND_RESULT_APPLIED;
    const bool recentlyDropped =
        g_trainingUi.lastRejectedCommandId != 0
        && GetTickCount() - g_trainingUi.lastRejectedTick < 8000u;

    if (lastFailed)
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.40f, 0.35f, 1.0f), "That didn't go through - %s.",
            TrainingCommandResultName(status.lastCommandResult));
        if (status.resultText[0] != '\0')
        {
            ImGui::TextWrapped("%s", status.resultText);
        }
    }
    else if (recentlyDropped)
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.40f, 0.35f, 1.0f),
            "Too many changes at once - that one was dropped. Try again.");
    }

}

void DrawTrainingDetails(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Training"));

    const unsigned long long stepSerial =
        (static_cast<unsigned long long>(status.realStepSerialHigh) << 32)
        | status.realStepSerialLow;
    ImGui::TextWrapped("Generation %lu  |  real step %llu",
        static_cast<unsigned long>(status.trainingGeneration), stepSerial);
    DrawEffectiveInvariant(
        "Native ExtraFunction commands disabled",
        status.extraFunctionsDisabled);
    DrawEffectiveInvariant("WaitCount forced to zero", status.waitCountZero);
    DrawEffectiveInvariant("Native pause neutral", status.pauseNeutral);
    DrawEffectiveInvariant("Native freeze neutral", status.freezeNeutral);
    ImGui::TextWrapped("Last admission: %s (native return %lu)",
        TrainingAdmissionName(status.lastAdmissionResult),
        static_cast<unsigned long>(status.lastNativeReturn));
    ImGui::TextWrapped("Input ownership: %s",
        TrainingInputRearmName(status.inputRearmState));
    if (status.lastAcknowledgedCommandId == 0)
    {
        ImGui::TextDisabled("No command acknowledged yet.");
    }
    else
    {
        ImGui::TextWrapped("Last command #%lu: %s",
            static_cast<unsigned long>(status.lastAcknowledgedCommandId),
            TrainingCommandResultName(status.lastCommandResult));
    }
    ImGui::TextWrapped("Queue: %lu / %lu    Drops: %lu",
        static_cast<unsigned long>(status.commandQueueDepth),
        static_cast<unsigned long>(status.commandQueueCapacity),
        static_cast<unsigned long>(status.commandQueueDrops));
    if (status.stickyValidationErrors != 0)
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.40f, 0.35f, 1.0f),
            "Sticky validation errors: 0x%08lX",
            static_cast<unsigned long>(status.stickyValidationErrors));
    }
}

constexpr float kTrainingReadoutP1Column = 190.0f;
constexpr float kTrainingReadoutP2Column = 330.0f;

void DrawTrainingReadoutHeader(const char *label)
{
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(kTrainingReadoutP1Column);
    ImGui::TextDisabled("P1");
    ImGui::SameLine(kTrainingReadoutP2Column);
    ImGui::TextDisabled("P2");
}

void DrawTrainingReadoutRow(
    const char *label, const char *p1Text, const char *p2Text)
{
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kTrainingReadoutP1Column);
    ImGui::TextUnformatted(p1Text);
    ImGui::SameLine(kTrainingReadoutP2Column);
    ImGui::TextUnformatted(p2Text);
    ImGui::EndGroup();
}

void DrawTrainingReadoutPair(
    const char *label, unsigned long p1, unsigned long p2)
{
    char p1Text[32] = {};
    char p2Text[32] = {};
    std::snprintf(p1Text, sizeof(p1Text), "%lu", p1);
    std::snprintf(p2Text, sizeof(p2Text), "%lu", p2);
    DrawTrainingReadoutRow(label, p1Text, p2Text);
}

void DrawTrainingRefills(const Qoh99TrainingStatusV1 &status)
{
    const bool refillReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_REFILL);

    int target = g_trainingUi.refillTarget;
    if (target < QOH99_TRAINING_REFILL_BOTH
        || target > QOH99_TRAINING_REFILL_P2)
    {
        target = QOH99_TRAINING_REFILL_BOTH;
    }

    ImGui::SeparatorText(qi18n::tr("Refill now"));
    ImGui::BeginDisabled(!refillReady);
    static const char *kRefillTargets[] = {
        "Both sides", "P1 only", "P2 only"
    };
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo(
            "Side##refill_target", &target, kRefillTargets,
            IM_ARRAYSIZE(kRefillTargets)))
    {
        g_trainingUi.refillTarget = target;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Which side the buttons below refill. Refilling only the dummy\n"
            "is useful when you want to keep your own low-health state.");
    }

    const DWORD targetValue = static_cast<DWORD>(target);
    if (ImGui::SmallButton("Health##refill"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_REFILL_LIFE, 0, targetValue);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Fills the life bar back to full, once, right now - including\n"
            "the red \"recently lost\" segment. Nobody can be knocked out in\n"
            "training anyway; this is for starting a fresh attempt.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Guard gauge##refill"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_REFILL_GUARD_GAUGE, 0,
            targetValue);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Sets the guard gauge back to full. It also refills on its own,\n"
            "slowly, while the round runs.\n"
            "A guard crush that has already happened still doubles the next\n"
            "hit - refilling the gauge does not take that back.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("EX meter##refill"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_REFILL_EX_METER, 0, targetValue);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Gives nine full EX stocks. The small progress bar under them\n"
            "empties, because that is the game's own full-meter state.\n"
            "If it refuses, a super is being paid for at that moment -\n"
            "try again once the animation finishes.");
    }
    ImGui::EndDisabled();
    TrainingTextDisabledWrapped(
        "The configured Refill resources control refills all three at once.");

    if (!refillReady)
    {
        DrawTrainingUnavailableReason(status, QOH99_TRAINING_CAP_REFILL);
        if (!TrainingHasExtendedStatus(status))
        {
            DrawTrainingStaleDllReason(
                status, QOH99_TRAINING_STATUS_V1_COMBAT_TIER);
        }
        return;
    }

    if (!TrainingHasExtendedStatus(status))
    {
        return;
    }
    if (status.combatReadoutValid == 0)
    {
        TrainingTextDisabledWrapped(
            "Current health, guard gauge and meter can't be read right now.");
        return;
    }

    ImGui::Spacing();
    DrawTrainingReadoutHeader("Right now");
    char p1Text[64] = {};
    char p2Text[64] = {};
    const int p1Life = static_cast<int>(static_cast<INT32>(status.p1EffectiveLife));
    const int p2Life = static_cast<int>(static_cast<INT32>(status.p2EffectiveLife));
    if (status.lifeMax != 0)
    {
        std::snprintf(p1Text, sizeof(p1Text), "%d / %lu",
            p1Life, static_cast<unsigned long>(status.lifeMax));
        std::snprintf(p2Text, sizeof(p2Text), "%d / %lu",
            p2Life, static_cast<unsigned long>(status.lifeMax));
    }
    else
    {
        std::snprintf(p1Text, sizeof(p1Text), "%d", p1Life);
        std::snprintf(p2Text, sizeof(p2Text), "%d", p2Life);
    }
    DrawTrainingReadoutRow("Health", p1Text, p2Text);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The value the game's own knock-out test compares, which is\n"
            "current life plus reserve minus damage that has not settled yet.\n"
            "The components are broken out below whenever they differ.");
    }

    if (TrainingHasFighterStatus(status))
    {
        const int p1Pending =
            static_cast<int>(static_cast<INT32>(status.p1LifePendingDamage));
        const int p2Pending =
            static_cast<int>(static_cast<INT32>(status.p2LifePendingDamage));
        const int p1Reserve =
            static_cast<int>(static_cast<INT32>(status.p1LifeReserve));
        const int p2Reserve =
            static_cast<int>(static_cast<INT32>(status.p2LifeReserve));
        const int p1Settle =
            static_cast<int>(static_cast<INT32>(status.p1LifeSettleTimer));
        const int p2Settle =
            static_cast<int>(static_cast<INT32>(status.p2LifeSettleTimer));
        if (p1Pending != 0 || p1Reserve != 0 || p1Settle != 0
            || p2Pending != 0 || p2Reserve != 0 || p2Settle != 0)
        {
            std::snprintf(p1Text, sizeof(p1Text), "%d, +%d res, -%d pend",
                static_cast<int>(static_cast<INT32>(status.p1LifeCurrent)),
                p1Reserve, p1Pending);
            std::snprintf(p2Text, sizeof(p2Text), "%d, +%d res, -%d pend",
                static_cast<int>(static_cast<INT32>(status.p2LifeCurrent)),
                p2Reserve, p2Pending);
            DrawTrainingReadoutRow("  current / reserve", p1Text, p2Text);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Current is what the bar has already lost; \"pend\" is the\n"
                    "red segment a recent hit has taken but not yet subtracted.\n"
                    "Health above is current + reserve - pending, which is the\n"
                    "number the game itself tests.");
            }
            std::snprintf(p1Text, sizeof(p1Text), "%d", p1Settle);
            std::snprintf(p2Text, sizeof(p2Text), "%d", p2Settle);
            DrawTrainingReadoutRow("  settle countdown", p1Text, p2Text);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Counts down before pending damage is subtracted. Its unit\n"
                    "is not established, so it is shown as a bare number.");
            }
        }
    }

    DrawTrainingReadoutPair(
        "Guard gauge",
        static_cast<unsigned long>(status.p1GuardGauge),
        static_cast<unsigned long>(status.p2GuardGauge));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Out of 100. Refills slowly on its own during a round.");
    }
    std::snprintf(p1Text, sizeof(p1Text), "%lu stocks (+%lu)",
        static_cast<unsigned long>(status.p1MeterStocks),
        static_cast<unsigned long>(status.p1MeterFraction));
    std::snprintf(p2Text, sizeof(p2Text), "%lu stocks (+%lu)",
        static_cast<unsigned long>(status.p2MeterStocks),
        static_cast<unsigned long>(status.p2MeterFraction));
    DrawTrainingReadoutRow("EX meter", p1Text, p2Text);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Completed stocks (0-9), then progress towards the next one\n"
            "out of 53.");
    }
}

void DrawTrainingMeterCharge(const Qoh99TrainingStatusV1 &status)
{
    if (!ImGui::TreeNode("Charge meter by holding a control"))
    {
        return;
    }
    const bool tierReady = TrainingHasMeterChargeStatus(status);
    const bool featureReady = tierReady && TrainingFeatureReady(
        status, QOH99_TRAINING_CAP_METER_CHARGE);
    ImGui::BeginDisabled(!featureReady);

    int target = tierReady
        ? static_cast<int>(status.meterChargeTarget)
        : QOH99_TRAINING_REFILL_BOTH;
    int interval = tierReady
        ? static_cast<int>(status.meterChargeInterval) : 2;
    static const char *targets[] = {
        "Both sides", "P1 only", "P2 only" };
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("Side##meter_charge", &target, targets,
                     IM_ARRAYSIZE(targets)))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_METER_CHARGE_CONFIG,
            static_cast<DWORD>(target),
            static_cast<DWORD>(interval));
    }
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Frames per step##meter_charge",
                         &interval, 1, 60))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_METER_CHARGE_CONFIG,
            static_cast<DWORD>(target),
            static_cast<DWORD>(interval));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "Counts admitted Training simulation frames, not render frames.\n"
            "At the default 2, an empty bar takes about 106 admitted frames\n"
            "to fill if meter does not change naturally.");
    }
    TrainingTextDisabledWrapped(
        "Hold the configured charge control during gameplay to fill the small "
        "progress bar. At 53 it adds one EX stock and restarts from 0; at nine "
        "stocks it stops.");
    ImGui::EndDisabled();

    if (!tierReady)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_METER_CHARGE_TIER);
    }
    else if (!featureReady)
    {
        DrawTrainingUnavailableReason(
            status, QOH99_TRAINING_CAP_METER_CHARGE);
    }
    ImGui::TreePop();
}

void DrawTrainingResourceValues(const Qoh99TrainingStatusV1 &status)
{
    const bool capReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_REFILL);
    const bool tierOk = TrainingHasRecoveryStatus(status);
    const bool boundsOk =
        tierOk && status.resourcePolicyValid != 0 && status.lifeSetMax != 0;
    const bool valuesReady = capReady && boundsOk;

    if (!g_trainingUi.resourceDraftsSeeded && boundsOk)
    {
        g_trainingUi.lifeSetValue = static_cast<int>(status.lifeSetMax);
        g_trainingUi.exStocksValue =
            static_cast<int>(QOH99_TRAINING_EX_STOCKS_FULL);
        g_trainingUi.exFractionValue = 0;
        g_trainingUi.guardGaugeValue =
            static_cast<int>(QOH99_TRAINING_GUARD_GAUGE_FULL);
        g_trainingUi.resourceDraftsSeeded = true;
    }

    if (!ImGui::TreeNode("Set an exact value"))
    {
        return;
    }
    ImGui::PushID("resource_values");
    ImGui::BeginDisabled(!valuesReady);

    int target = g_trainingUi.resourceTarget;
    if (target < QOH99_TRAINING_REFILL_BOTH
        || target > QOH99_TRAINING_REFILL_P2)
    {
        target = QOH99_TRAINING_REFILL_BOTH;
    }
    static const char *kTargets[] = { "Both sides", "P1 only", "P2 only" };
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo(
            "Side##value_target", &target, kTargets, IM_ARRAYSIZE(kTargets)))
    {
        g_trainingUi.resourceTarget = target;
    }
    const DWORD targetValue = static_cast<DWORD>(target);

    const int lifeMax = boundsOk ? static_cast<int>(status.lifeSetMax) : 1;
    if (g_trainingUi.lifeSetValue < 1)
    {
        g_trainingUi.lifeSetValue = 1;
    }
    if (g_trainingUi.lifeSetValue > lifeMax)
    {
        g_trainingUi.lifeSetValue = lifeMax;
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt(
        "Health##set_life", &g_trainingUi.lifeSetValue, 1, lifeMax);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The lowest you can set is 1, not 0. Nobody may reach a knocked-out\n"
            "frame in training - that would end the round - and the game's own\n"
            "knockout test reads exactly this number.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply##set_life_apply"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_LIFE_VALUE, targetValue,
            static_cast<DWORD>(g_trainingUi.lifeSetValue));
    }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt(
        "EX stocks##set_ex_stocks", &g_trainingUi.exStocksValue, 0,
        static_cast<int>(QOH99_TRAINING_EX_STOCKS_FULL));
    const bool exFull = g_trainingUi.exStocksValue
        >= static_cast<int>(QOH99_TRAINING_EX_STOCKS_FULL);
    if (exFull)
    {
        g_trainingUi.exFractionValue = 0;
    }
    ImGui::BeginDisabled(exFull);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt(
        "  towards next##set_ex_fraction", &g_trainingUi.exFractionValue, 0,
        static_cast<int>(QOH99_TRAINING_EX_FRACTION_MAX));
    ImGui::EndDisabled();
    if (exFull)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(always 0 at nine stocks)");
    }
    if (ImGui::SmallButton("Apply##set_ex_apply"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_EX_METER, targetValue,
            static_cast<DWORD>(g_trainingUi.exStocksValue),
            static_cast<DWORD>(g_trainingUi.exFractionValue));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Refused while a super is being paid for at that moment - try\n"
            "again once the animation finishes. That is a \"not right now\",\n"
            "not a failure: writing stocks mid-payment would make it free.\n"
            "Nine stocks with nothing towards the next one cannot pay the\n"
            "game's small fractional cost, so it is not \"infinite meter\".");
    }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt(
        "Guard gauge##set_guard", &g_trainingUi.guardGaugeValue, 0,
        static_cast<int>(QOH99_TRAINING_GUARD_GAUGE_FULL));
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply##set_guard_apply"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_VALUE, targetValue,
            static_cast<DWORD>(g_trainingUi.guardGaugeValue));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Anything below full climbs back on its own during a round, at\n"
            "roughly one point every eighth frame - about thirteen seconds from\n"
            "empty to full. Set it low to practise a guard crush, not to hold\n"
            "it there.");
    }

    ImGui::EndDisabled();
    ImGui::PopID();

    if (!capReady)
    {
        DrawTrainingUnavailableReason(status, QOH99_TRAINING_CAP_REFILL);
    }
    else if (!tierOk)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_RECOVERY_TIER);
    }
    else if (!boundsOk)
    {
        TrainingTextDisabledWrapped(
            "The health maximum for this match can't be read right now, so "
            "there is no honest range to offer.");
    }
    ImGui::TreePop();
}

void DrawTrainingResourcePolicies(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Restore automatically"));

    const bool capReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_REFILL);
    const bool tierOk = TrainingHasRecoveryStatus(status);
    const bool policiesReady = capReady && tierOk;

    const auto enabledForBoth = [](DWORD trigger, DWORD target) {
        return trigger
                   == static_cast<DWORD>(
                       QOH99_TRAINING_TRIGGER_WHEN_NEUTRAL)
            && target
                   == static_cast<DWORD>(QOH99_TRAINING_REFILL_BOTH);
    };
    const bool healthEnabled = enabledForBoth(
        status.healthTrigger, status.healthTriggerTarget);
    const bool exEnabled = enabledForBoth(
        status.exTrigger, status.exTriggerTarget);
    const bool guardEnabled = enabledForBoth(
        status.guardGaugeTrigger, status.guardGaugeTriggerTarget);
    const bool restoreFullHp = healthEnabled && !exEnabled && !guardEnabled;
    const bool restoreAll = healthEnabled && exEnabled && guardEnabled;

    ImGui::PushID("policy_rows");
    ImGui::BeginDisabled(!policiesReady);

    const auto setRestorePreset = [&](bool health, bool ex, bool guard) {
        static const DWORD kPolicyKinds[] = {
            QOH99_TRAINING_COMMAND_SET_HEALTH_POLICY,
            QOH99_TRAINING_COMMAND_SET_EX_POLICY,
            QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_POLICY,
        };
        const bool enabled[] = { health, ex, guard };
        for (int i = 0; i < IM_ARRAYSIZE(kPolicyKinds); ++i)
        {
            EnqueueTrainingCommand(
                status, kPolicyKinds[i],
                static_cast<DWORD>(QOH99_TRAINING_REFILL_BOTH),
                static_cast<DWORD>(enabled[i]
                    ? QOH99_TRAINING_TRIGGER_WHEN_NEUTRAL
                    : QOH99_TRAINING_TRIGGER_OFF));
        }
    };

    bool restoreFullHpDraft = restoreFullHp;
    if (ImGui::Checkbox(
            "Restore full HP##restore_full_hp", &restoreFullHpDraft))
    {
        setRestorePreset(restoreFullHpDraft, false, false);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Keeps both health bars full while both characters are idle, "
            "walking or crouching.");
    }

    bool restoreAllDraft = restoreAll;
    if (ImGui::Checkbox("Restore all##restore_all", &restoreAllDraft))
    {
        setRestorePreset(
            restoreAllDraft, restoreAllDraft, restoreAllDraft);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Keeps health, EX stocks and guard full for both characters while\n"
            "they are idle, walking or crouching.");
    }

    if (TrainingHasExRestoreTargetStatus(status))
    {
        ImGui::Spacing();
        if (status.exRestorePinned != 0u)
        {
            TrainingTextColoredWrapped(
                ImVec4(0.55f, 0.80f, 1.0f, 1.0f),
                "EX restores to %lu stock%s + %lu, not full - that is what you "
                "last charged to.",
                static_cast<unsigned long>(status.exRestoreStocks),
                status.exRestoreStocks == 1u ? "" : "s",
                static_cast<unsigned long>(status.exRestoreFraction));
            if (ImGui::Button("Restore EX to full again##ex_restore_unpin"))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_SET_EX_RESTORE_TARGET,
                    0u, 0u);
            }
        }
        else
        {
            TrainingTextDisabledWrapped(
                "EX restores to full. Hold the configured charge control to a "
                "level and refills will return to that level instead.");
        }
    }

    ImGui::EndDisabled();
    ImGui::PopID();

    if (!capReady)
    {
        DrawTrainingUnavailableReason(status, QOH99_TRAINING_CAP_REFILL);
    }
    if (!tierOk)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_RECOVERY_TIER);
        return;
    }

    if (!restoreFullHp && !restoreAll)
    {
        return;
    }
    ImGui::Spacing();
    if (status.resourcePolicyValid == 0)
    {
        ImGui::TextDisabled("Nothing is being restored right now.");
        return;
    }
    ImGui::TextDisabled(restoreAll
        ? "Missing resources refill once both characters are settled."
        : "Missing health refills once both characters are settled.");
}

void DrawTrainingHoldControls(const Qoh99TrainingStatusV1 &status)
{
    const bool tierOk = TrainingHasRecoveryStatus(status);
    const bool holdReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_RESET) && tierOk;

    ImGui::PushID("hold_controls");
    ImGui::BeginDisabled(!holdReady);
    if (ImGui::Button("Pause / Resume##hold_toggle"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_TOGGLE_PAUSE, 0, 0);
    }
    ImGui::EndDisabled();
    ImGui::PopID();

    if (!tierOk)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_RECOVERY_TIER);
        return;
    }

    const bool userHeld = (status.holdReasons & QOH99_TRAINING_HOLD_USER) != 0;
    const bool menuHeld = (status.holdReasons & QOH99_TRAINING_HOLD_MENU) != 0;
    if (userHeld && menuHeld)
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.72f, 0.30f, 1.0f),
            "Paused, and this menu is holding it too - still paused when you "
            "close the menu.");
    }
    else if (menuHeld)
    {
        TrainingTextColoredWrapped(
            ImVec4(0.60f, 0.75f, 1.0f, 1.0f),
            "Held while this menu is open - it resumes when you close it.");
    }
    else if (userHeld)
    {
        ImGui::TextColored(
            ImVec4(1.0f, 0.72f, 0.30f, 1.0f), "Paused.");
    }
    else
    {
        ImGui::TextDisabled("Running.");
    }
}

void DrawTrainingFighterReadout(const Qoh99TrainingStatusV1 &status)
{
    if (!TrainingHasFighterStatus(status))
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_FIGHTER_TIER);
        return;
    }
    if (status.fighterReadoutValid == 0)
    {
        TrainingTextDisabledWrapped(
            "Character data can't be read right now - this needs a live "
            "training match.");
        return;
    }

    ImGui::SeparatorText(qi18n::tr("Identity and position"));
    char p1Text[64] = {};
    char p2Text[64] = {};
    DrawTrainingReadoutHeader("Field");
    DrawTrainingReadoutPair(
        "Character id",
        static_cast<unsigned long>(status.p1Character),
        static_cast<unsigned long>(status.p2Character));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Numeric only. The id-to-name catalog lives in the injected DLL\n"
            "and cannot be linked here, and a hand-copied second copy would\n"
            "be free to drift out of agreement with it.");
    }
    DrawTrainingReadoutPair(
        "Facing byte",
        static_cast<unsigned long>(status.p1Facing),
        static_cast<unsigned long>(status.p2Facing));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "1 mirrors that side's left/right input. What is proven is the\n"
            "input-mask swap, not which way the character is drawn, so it is\n"
            "shown as the raw byte and not as \"facing left/right\".");
    }
    std::snprintf(p1Text, sizeof(p1Text), "%d, %d",
        static_cast<int>(static_cast<INT32>(status.p1PositionX)),
        static_cast<int>(static_cast<INT32>(status.p1PositionY)));
    std::snprintf(p2Text, sizeof(p2Text), "%d, %d",
        static_cast<int>(static_cast<INT32>(status.p2PositionX)),
        static_cast<int>(static_cast<INT32>(status.p2PositionY)));
    DrawTrainingReadoutRow("Position X, Y", p1Text, p2Text);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Signed. Named position because training writes these two words to\n"
            "place a character and then validates the result through the game's\n"
            "own world clamp and collision-rect helpers.");
    }

    ImGui::SeparatorText(qi18n::tr("Engine fields"));
    ImGui::TextWrapped(
        "Raw engine fields. Their meanings are not established yet - the "
        "numbers are exact, the names are placeholders. Nothing here tells you "
        "whether a character can act.");
    ImGui::Spacing();
    DrawTrainingReadoutHeader("Field");

    struct RawRow
    {
        const char *label;
        DWORD p1;
        DWORD p2;
        const char *tip;
    };
    const RawRow rows[] = {
        { "action", status.p1ActionState, status.p2ActionState,
          "Offset 48. Its value set is unresolved: there is no verified table\n"
          "of neutral / attacking / recovering, so only the number is shown." },
        { "frame", status.p1ActionFrame, status.p2ActionFrame,
          "Offset 52. A counter with no proven origin or reset rule." },
        { "lock end", status.p1ActionLockEnd, status.p2ActionLockEnd,
          "Offset 56. NOT hitstun, freeze, recovery, or frames-until-actionable\n"
          "- whatever writes it has never been traced." },
        { "anim", status.p1AnimFrame, status.p2AnimFrame,
          "Offset 64." },
        { "next", status.p1NextAction, status.p2NextAction,
          "Offset 72." },
        { "transition", status.p1TransitionFlag, status.p2TransitionFlag,
          "Offset 76." },
        { "prev", status.p1PrevAction, status.p2PrevAction,
          "Offset 80." },
    };
    for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(rows)); ++i)
    {
        ImGui::PushID(i);
        std::snprintf(p1Text, sizeof(p1Text), "%lu  (0x%lX)",
            static_cast<unsigned long>(rows[i].p1),
            static_cast<unsigned long>(rows[i].p1));
        std::snprintf(p2Text, sizeof(p2Text), "%lu  (0x%lX)",
            static_cast<unsigned long>(rows[i].p2),
            static_cast<unsigned long>(rows[i].p2));
        DrawTrainingReadoutRow(rows[i].label, p1Text, p2Text);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", rows[i].tip);
        }
        ImGui::PopID();
    }

    std::snprintf(p1Text, sizeof(p1Text), "0x%02lX  (&7 = %lu)",
        static_cast<unsigned long>(status.p1MoveGateByte & 0xFFu),
        static_cast<unsigned long>(status.p1MoveGateByte & 0x7u));
    std::snprintf(p2Text, sizeof(p2Text), "0x%02lX  (&7 = %lu)",
        static_cast<unsigned long>(status.p2MoveGateByte & 0xFFu),
        static_cast<unsigned long>(status.p2MoveGateByte & 0x7u));
    DrawTrainingReadoutRow("move gate", p1Text, p2Text);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Offset 156. Its low three bits take part in posture and move-class\n"
            "dispatch, and the model says in as many words that this is NOT\n"
            "proven actionability. Do not read it as \"can act\".");
    }
}

void DrawTrainingDamageReadout(const Qoh99TrainingStatusV1 &status)
{
    if (!TrainingHasExtendedStatus(status))
    {
        DrawTrainingStaleDllReason(
                status, QOH99_TRAINING_STATUS_V1_COMBAT_TIER);
        return;
    }

    ImGui::SeparatorText(qi18n::tr("Last hit"));
    const bool described = status.lastHitVictimSlot <= 3;
    if (!described)
    {
        if (status.damageEventSerial == 0 && status.damageDeclinedCount == 0)
        {
            ImGui::TextDisabled("Nothing has hit yet this session.");
        }
        else
        {
            TrainingTextDisabledWrapped(
                "No hit is described right now - the last one belonged to a "
                "timeline that a save state load or a return to character "
                "select discarded.");
            TrainingTextDisabledWrapped(
                "Applied %lu  |  not applied %lu (this session)",
                static_cast<unsigned long>(status.damageEventSerial),
                static_cast<unsigned long>(status.damageDeclinedCount));
        }
    }
    else
    {
        const bool applied = status.lastHitAdmitted != 0;
        ImGui::TextUnformatted("Damage");
        ImGui::SameLine(290.0f);
        ImGui::Text("%d",
            static_cast<int>(
                static_cast<int32_t>(status.lastHitDamage)));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "The number the game handed to its own damage routine.\n"
                "This is the hit's nominal damage, before training clamps a\n"
                "lethal hit so the round cannot end - so it is not the same\n"
                "as the amount the life bar moved.");
        }

        ImGui::TextUnformatted("Applied to the life bar");
        ImGui::SameLine(290.0f);
        if (applied)
        {
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.48f, 1.0f), "Yes");
        }
        else
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.55f, 0.30f, 1.0f), "No");
        }
        if (!applied)
        {
            TrainingTextDisabledWrapped(
                "The game's own damage routine did not apply that entry - "
                "either it declined it because that character is already "
                "finished for the round, or the life record did not move by "
                "the amount asked for. The number above is what was "
                "requested, not what landed.");
        }

        const int moved = static_cast<int>(
            static_cast<int32_t>(status.lastHitPendingDelta));
        ImGui::TextUnformatted("Life record moved by");
        ImGui::SameLine(290.0f);
        ImGui::Text("%d", moved);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "How far the life record actually moved once training's own\n"
                "clamp had run - not what the hit asked for. On a character\n"
                "already at zero life under the endless round this is zero,\n"
                "however large the hit was.");
        }
        if (applied
            && moved != static_cast<int>(
                   static_cast<int32_t>(status.lastHitDamage)))
        {
            TrainingTextDisabledWrapped(
                "The game applied that hit in full; training's endless-round "
                "clamp then held the life bar at zero, so only the difference "
                "above stayed on the bar.");
        }

        ImGui::TextUnformatted("Credited attacker / victim");
        ImGui::SameLine(290.0f);
        ImGui::Text("slot %lu  ->  slot %lu",
            static_cast<unsigned long>(status.lastHitAttackerSlot),
            static_cast<unsigned long>(status.lastHitVictimSlot));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "The slot the game CREDITS for the hit. On some hit paths\n"
                "that is a value cached on the victim rather than the\n"
                "attacking character's own slot.");
        }

        ImGui::TextUnformatted("Raw fatal-cause word");
        ImGui::SameLine(290.0f);
        ImGui::Text("0x%08lX",
            static_cast<unsigned long>(status.lastHitCause));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Metadata the game stores only when a hit would take a\n"
                "character below zero life. It is not an enumerated hit\n"
                "type - the values it can hold are not a closed set - so it\n"
                "is shown exactly as stored and is not given a name.");
        }

        TrainingTextDisabledWrapped(
            "Applied %lu  |  not applied %lu (this session)",
            static_cast<unsigned long>(status.damageEventSerial),
            static_cast<unsigned long>(status.damageDeclinedCount));
    }

    ImGui::SeparatorText(qi18n::tr("Combos"));
    if (status.combatReadoutValid == 0)
    {
        ImGui::TextDisabled("Combo counters can't be read right now.");
        return;
    }

    DrawTrainingReadoutHeader("Being taken");
    DrawTrainingReadoutPair(
        "Hits",
        static_cast<unsigned long>(status.p1ComboHitsTaken),
        static_cast<unsigned long>(status.p2ComboHitsTaken));
    DrawTrainingReadoutPair(
        "Damage",
        static_cast<unsigned long>(status.p1ComboDamageTaken),
        static_cast<unsigned long>(status.p2ComboDamageTaken));

    char p1Text[48] = {};
    char p2Text[48] = {};
    std::snprintf(p1Text, sizeof(p1Text), "%lu hits, %lu damage",
        static_cast<unsigned long>(status.p1LastComboHitsTaken),
        static_cast<unsigned long>(status.p1LastComboDamageTaken));
    std::snprintf(p2Text, sizeof(p2Text), "%lu hits, %lu damage",
        static_cast<unsigned long>(status.p2LastComboHitsTaken),
        static_cast<unsigned long>(status.p2LastComboDamageTaken));
    DrawTrainingReadoutRow("Last combo taken", p1Text, p2Text);

    ImGui::Spacing();
    DrawTrainingReadoutHeader("Best dealt this match");
    std::snprintf(p1Text, sizeof(p1Text), "%lu hits, %lu damage",
        static_cast<unsigned long>(status.p1MaxComboHitsDealt),
        static_cast<unsigned long>(status.p1MaxComboDamageDealt));
    std::snprintf(p2Text, sizeof(p2Text), "%lu hits, %lu damage",
        static_cast<unsigned long>(status.p2MaxComboHitsDealt),
        static_cast<unsigned long>(status.p2MaxComboDamageDealt));
    DrawTrainingReadoutRow("Best combo", p1Text, p2Text);

    ImGui::Spacing();
    ImGui::TextWrapped(
        "\"Being taken\" counts the hits landing ON that side; \"best dealt\" "
        "counts the side that landed them. Chip damage on block is damage but "
        "is not counted as a combo hit - that is the game's own accounting. "
        "Hit counts stop at 99 and combo damage at 9999, so a very long combo "
        "can sit at the cap.");
}

constexpr int kActionStateLogSides = 2;
// Field 0 is fighter+84 (switched on by sub_4017F0); field 1 is fighter+48 (gated on by sub_47CAE0).
constexpr int kActionStateLogFields = 2;
constexpr int kActionStateLogCapacity = 192;
constexpr int kActionStateLogFieldDispatch = 0;
constexpr int kActionStateLogFieldAction = 1;

struct TrainingActionStateEntry
{
    DWORD character;
    DWORD value;
    DWORD entries;
    DWORD framesHeld;
    UINT64 firstStep;
    UINT64 lastStep;
};

struct TrainingActionStateLog
{
    TrainingActionStateEntry
        entries[kActionStateLogSides][kActionStateLogFields]
               [kActionStateLogCapacity];
    DWORD count[kActionStateLogSides][kActionStateLogFields];
    DWORD dropped[kActionStateLogSides][kActionStateLogFields];
    DWORD lastValue[kActionStateLogSides][kActionStateLogFields];
    DWORD lastCharacter[kActionStateLogSides][kActionStateLogFields];
    bool haveLast[kActionStateLogSides][kActionStateLogFields];
    DWORD generation;
    UINT64 lastStepSerial;
    bool haveStep;
    UINT64 sampledSteps;
    UINT64 gapSteps;
    UINT64 unlicensedSteps;
};

TrainingActionStateLog g_actionStateLog = {};

struct TrainingActionStateLogFilters
{
    bool showP1 = true;
    bool showP2 = true;
    bool showDispatch = true;
    bool showAction = true;
};

TrainingActionStateLogFilters g_actionStateLogFilters;

void ResetTrainingActionStateLog(DWORD generation)
{
    std::memset(&g_actionStateLog, 0, sizeof(g_actionStateLog));
    g_actionStateLog.generation = generation;
}

void RecordTrainingActionStateSample(
    int side, int field, DWORD character, DWORD value, UINT64 step)
{
    TrainingActionStateEntry *const table =
        g_actionStateLog.entries[side][field];
    DWORD &count = g_actionStateLog.count[side][field];
    int index = -1;
    for (DWORD i = 0; i < count; ++i)
    {
        if (table[i].character == character && table[i].value == value)
        {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0)
    {
        if (count >= static_cast<DWORD>(kActionStateLogCapacity))
        {
            ++g_actionStateLog.dropped[side][field];
            return;
        }
        index = static_cast<int>(count);
        ++count;
        table[index].character = character;
        table[index].value = value;
        table[index].entries = 0;
        table[index].framesHeld = 0;
        table[index].firstStep = step;
    }
    TrainingActionStateEntry &entry = table[index];
    ++entry.framesHeld;
    entry.lastStep = step;
    const bool newOccurrence =
        !g_actionStateLog.haveLast[side][field]
        || g_actionStateLog.lastValue[side][field] != value
        || g_actionStateLog.lastCharacter[side][field] != character;
    if (newOccurrence)
    {
        ++entry.entries;
    }
    g_actionStateLog.lastValue[side][field] = value;
    g_actionStateLog.lastCharacter[side][field] = character;
    g_actionStateLog.haveLast[side][field] = true;
}

void SampleTrainingActionStateLog()
{
    /* While the device worker holds g_presenter the game thread must not read any of it. */
    if (deviceState() == kDeviceWorking)
    {
        return;
    }

    Qoh99TrainingStatusV1 status;
    if (!ReadTrainingStatus(status) || status.explicitTrainingPresent == 0)
    {
        return;
    }
    if (status.trainingGeneration != g_actionStateLog.generation)
    {
        ResetTrainingActionStateLog(status.trainingGeneration);
    }
    if (status.inMatchReady == 0)
    {
        return;
    }

    const UINT64 step =
        (static_cast<UINT64>(status.realStepSerialHigh) << 32)
        | static_cast<UINT64>(status.realStepSerialLow);
    if (g_actionStateLog.haveStep)
    {
        if (step == g_actionStateLog.lastStepSerial)
        {
            return;
        }
        if (step < g_actionStateLog.lastStepSerial)
        {
            ResetTrainingActionStateLog(status.trainingGeneration);
        }
        else
        {
            g_actionStateLog.gapSteps +=
                step - g_actionStateLog.lastStepSerial - 1;
        }
    }
    g_actionStateLog.lastStepSerial = step;
    g_actionStateLog.haveStep = true;
    ++g_actionStateLog.sampledSteps;

    const bool actionLicensed =
        TrainingHasFighterStatus(status) && status.fighterReadoutValid != 0;
    const bool dispatchLicensed =
        TrainingHasRecoveryStatus(status) && status.resourcePolicyValid != 0;
    if (!actionLicensed || !dispatchLicensed)
    {
        ++g_actionStateLog.unlicensedSteps;
    }
    if (!actionLicensed && !dispatchLicensed)
    {
        return;
    }

    const DWORD characters[kActionStateLogSides] = {
        status.p1Character, status.p2Character
    };
    const DWORD values[kActionStateLogSides][kActionStateLogFields] = {
        { status.p1DispatchAction, status.p1ActionState },
        { status.p2DispatchAction, status.p2ActionState },
    };
    const bool licensed[kActionStateLogFields] = {
        dispatchLicensed, actionLicensed
    };
    for (int side = 0; side < kActionStateLogSides; ++side)
    {
        for (int field = 0; field < kActionStateLogFields; ++field)
        {
            if (!licensed[field])
            {
                continue;
            }
            RecordTrainingActionStateSample(
                side, field, characters[side], values[side][field], step);
        }
    }
}

constexpr float kIdleTestP1Column = 210.0f;
constexpr float kIdleTestP2Column = 400.0f;

const ImVec4 kIdleTestFailColor(1.0f, 0.45f, 0.40f, 1.0f);
const ImVec4 kIdleTestDivergedColor(1.0f, 0.72f, 0.30f, 1.0f);

void DrawIdleTestHeader(const char *label)
{
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(kIdleTestP1Column);
    ImGui::TextDisabled("P1");
    ImGui::SameLine(kIdleTestP2Column);
    ImGui::TextDisabled("P2");
}

void DrawIdleTestRow(
    const char *label,
    const char *p1Text, const ImVec4 *p1Color,
    const char *p2Text, const ImVec4 *p2Color)
{
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kIdleTestP1Column);
    if (p1Color != nullptr)
    {
        ImGui::TextColored(*p1Color, "%s", p1Text);
    }
    else
    {
        ImGui::TextUnformatted(p1Text);
    }
    ImGui::SameLine(kIdleTestP2Column);
    if (p2Color != nullptr)
    {
        ImGui::TextColored(*p2Color, "%s", p2Text);
    }
    else
    {
        ImGui::TextUnformatted(p2Text);
    }
    ImGui::EndGroup();
}

void DrawIdleTestClauseRow(
    const char *label, const char *p1Text, bool p1Failed,
    const char *p2Text, bool p2Failed)
{
    DrawIdleTestRow(
        label,
        p1Text, p1Failed ? &kIdleTestFailColor : nullptr,
        p2Text, p2Failed ? &kIdleTestFailColor : nullptr);
}

const char *IdleTestGroupReason(bool tierOk, bool licensed)
{
    if (!tierOk)
    {
        return "not published";
    }
    if (!licensed)
    {
        return "not readable now";
    }
    return nullptr;
}

void FormatIdleTestUnsigned(
    char *out, size_t outSize, const char *reason, DWORD value)
{
    if (reason != nullptr)
    {
        std::snprintf(out, outSize, "%s", reason);
        return;
    }
    std::snprintf(out, outSize, "%lu", static_cast<unsigned long>(value));
}

void FormatIdleTestAction(
    char *out, size_t outSize, const char *reason, DWORD value)
{
    if (reason != nullptr)
    {
        std::snprintf(out, outSize, "%s", reason);
        return;
    }
    std::snprintf(out, outSize, "%lu  (0x%lX)",
        static_cast<unsigned long>(value),
        static_cast<unsigned long>(value));
}

void FormatIdleTestSigned(
    char *out, size_t outSize, const char *reason, DWORD value)
{
    if (reason != nullptr)
    {
        std::snprintf(out, outSize, "%s", reason);
        return;
    }
    std::snprintf(out, outSize, "%d",
        static_cast<int>(static_cast<INT32>(value)));
}

const char *IdleTestBitState(bool evaluated, bool failed)
{
    if (!evaluated)
    {
        return "not evaluated";
    }
    return failed ? "bit set" : "bit clear";
}

void FormatIdleTestUnpublished(
    char *out, size_t outSize, bool evaluated, bool failed)
{
    std::snprintf(out, outSize, "not published (%s)",
        IdleTestBitState(evaluated, failed));
}

void DrawTrainingIdleTestRawBlock(const Qoh99TrainingStatusV1 &status)
{
    ImGui::TextWrapped(
        "+48 is what recovery eligibility gates on (sub_47CAE0). +84 is what "
        "the state dispatcher switches on (sub_4017F0). They are not always "
        "equal -- 48 and 49, the airdashes, are real +48 values with no case "
        "in the +84 switch at all, so neither field alone is the whole "
        "picture.");
    ImGui::Spacing();

    const bool fighterTier = TrainingHasFighterStatus(status);
    const bool fighterLive = fighterTier && status.fighterReadoutValid != 0;
    const bool neutralTier = TrainingHasRecoveryStatus(status);
    const bool neutralLive = neutralTier && status.resourcePolicyValid != 0;
    const bool combatLive = fighterTier && status.combatReadoutValid != 0;
    const char *const fighterReason =
        IdleTestGroupReason(fighterTier, fighterLive);
    const char *const neutralReason =
        IdleTestGroupReason(neutralTier, neutralLive);
    const char *const combatReason =
        IdleTestGroupReason(fighterTier, combatLive);

    const DWORD p1Mask = neutralLive ? status.p1NeutralReasonMask : 0u;
    const DWORD p2Mask = neutralLive ? status.p2NeutralReasonMask : 0u;
    const DWORD globalMask = neutralLive ? status.neutralGlobalReasonMask : 0u;

    char p1Text[80] = {};
    char p2Text[80] = {};

    ImGui::SeparatorText(qi18n::tr("Detailed settled-state test"));
    ImGui::TextWrapped(
        "This detailed test needs BOTH characters settled - idle, walking or "
        "crouching. Automatic restore also becomes ready after both characters "
        "stay in state 0 for 11 frames, so lingering bookkeeping shown below "
        "cannot make Restore all restore only one resource.");
    ImGui::TextWrapped("Restored %lu times, refused %lu",
        static_cast<unsigned long>(status.resourcePolicyFires),
        static_cast<unsigned long>(status.resourcePolicyRejects));
    TrainingTextDisabledWrapped(
        "Policies: health %s, EX %s, guard gauge %s",
        TrainingResourceTriggerName(status.healthTrigger),
        TrainingResourceTriggerName(status.exTrigger),
        TrainingResourceTriggerName(status.guardGaugeTrigger));

    if (!neutralLive)
    {
        TrainingTextDisabledWrapped(
            "The detailed test can't be evaluated right now. Automatic restore "
            "also requires readable fighter state and fails closed without it.");
    }
    else
    {
        TrainingTextColoredWrapped(
            status.bothFightersNeutral
                ? ImVec4(0.35f, 0.90f, 0.48f, 1.0f)
                : ImVec4(0.60f, 0.60f, 0.60f, 1.0f),
            status.bothFightersNeutral
                ? "Both idle, walking or crouching (held %lu frames)"
                : "Not both idle, walking or crouching (held %lu frames)",
            static_cast<unsigned long>(status.neutralHoldSteps));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "A working definition, not a complete one. Every state it accepts\n"
                "is one you can act out of: it takes the four ids the RE catalog\n"
                "names as free movement -- idle, walk forward, walk back, crouch --\n"
                "and all four are actionable. The reverse does not hold, and that\n"
                "is deliberate: it refuses plenty of states you can also act from,\n"
                "so read it as a conservative floor. Blocking, dashing, jumping,\n"
                "attacking, being hit and being thrown all read as not neutral.");
        }
        char reasons[256] = {};
        TrainingAppendNeutralReasons(status.p1NeutralReasonMask, reasons,
                                    sizeof(reasons));
        ImGui::TextWrapped("P1: %s", reasons);
        TrainingAppendNeutralReasons(status.p2NeutralReasonMask, reasons,
                                    sizeof(reasons));
        ImGui::TextWrapped("P2: %s", reasons);
        if (status.neutralGlobalReasonMask != 0)
        {
            TrainingAppendNeutralReasons(status.neutralGlobalReasonMask,
                                        reasons, sizeof(reasons));
            ImGui::TextWrapped("Match: %s", reasons);
        }
        TrainingTextDisabledWrapped("Combo tracking: P1 %s, P2 %s",
            TrainingComboEpisodeName(status.p1ComboEpisodeState),
            TrainingComboEpisodeName(status.p2ComboEpisodeState));
    }
    ImGui::Spacing();

    ImGui::SeparatorText(qi18n::tr("Clauses of the idle test"));
    DrawIdleTestHeader("Clause");

    std::snprintf(p1Text, sizeof(p1Text), "%s",
        !neutralLive
            ? "not evaluated"
            : ((p1Mask & QOH99_TRAINING_NEUTRAL_FIGHTER_UNREADABLE)
                   ? "read failed" : "read ok"));
    std::snprintf(p2Text, sizeof(p2Text), "%s",
        !neutralLive
            ? "not evaluated"
            : ((p2Mask & QOH99_TRAINING_NEUTRAL_FIGHTER_UNREADABLE)
                   ? "read failed" : "read ok"));
    DrawIdleTestClauseRow("fighter readable",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_FIGHTER_UNREADABLE) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_FIGHTER_UNREADABLE) != 0);

    DrawIdleTestClauseRow("slot in range",
        "slot 0", (p1Mask & QOH99_TRAINING_NEUTRAL_SLOT_OUT_OF_RANGE) != 0,
        "slot 1", (p2Mask & QOH99_TRAINING_NEUTRAL_SLOT_OUT_OF_RANGE) != 0);

    FormatIdleTestUnpublished(p1Text, sizeof(p1Text), neutralLive,
        (p1Mask & QOH99_TRAINING_NEUTRAL_ALTERNATE_FORM) != 0);
    FormatIdleTestUnpublished(p2Text, sizeof(p2Text), neutralLive,
        (p2Mask & QOH99_TRAINING_NEUTRAL_ALTERNATE_FORM) != 0);
    DrawIdleTestClauseRow("alternate form (+1456)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_ALTERNATE_FORM) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_ALTERNATE_FORM) != 0);

    FormatIdleTestAction(p1Text, sizeof(p1Text), fighterReason,
        status.p1ActionState);
    FormatIdleTestAction(p2Text, sizeof(p2Text), fighterReason,
        status.p2ActionState);
    DrawIdleTestClauseRow("action (+48)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_ACTION_NOT_FREE_MOVEMENT) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_ACTION_NOT_FREE_MOVEMENT) != 0);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "fighter +48. This is the field sub_47CAE0 gates recovery\n"
            "eligibility on, and it carries ids the +84 dispatcher has no case\n"
            "for at all (48 and 49, the airdashes). The clause accepts the four\n"
            "catalogued free-movement ids {0, 26, 27, 28} and nothing else.");
    }

    FormatIdleTestAction(p1Text, sizeof(p1Text), neutralReason,
        status.p1DispatchAction);
    FormatIdleTestAction(p2Text, sizeof(p2Text), neutralReason,
        status.p2DispatchAction);
    DrawIdleTestClauseRow("dispatch (+84)",
        p1Text,
        (p1Mask & QOH99_TRAINING_NEUTRAL_DISPATCH_NOT_FREE_MOVEMENT) != 0,
        p2Text,
        (p2Mask & QOH99_TRAINING_NEUTRAL_DISPATCH_NOT_FREE_MOVEMENT) != 0);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "fighter +84. This is the field the state dispatcher sub_4017F0\n"
            "switches on, across 101 cases. It is NOT the same field as +48\n"
            "above and the two are not always equal.");
    }

    if (fighterLive && neutralLive)
    {
        const bool p1Diverged = status.p1ActionState != status.p1DispatchAction;
        const bool p2Diverged = status.p2ActionState != status.p2DispatchAction;
        DrawIdleTestRow("+48 vs +84",
            p1Diverged ? "DIVERGED" : "equal",
            p1Diverged ? &kIdleTestDivergedColor : nullptr,
            p2Diverged ? "DIVERGED" : "equal",
            p2Diverged ? &kIdleTestDivergedColor : nullptr);
    }

    FormatIdleTestSigned(p1Text, sizeof(p1Text), neutralReason,
        status.p1InitStatus);
    FormatIdleTestSigned(p2Text, sizeof(p2Text), neutralReason,
        status.p2InitStatus);
    DrawIdleTestClauseRow("init status (+88)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_INIT_NOT_RUNNING) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_INIT_NOT_RUNNING) != 0);

    FormatIdleTestUnsigned(p1Text, sizeof(p1Text), fighterReason,
        status.p1TransitionFlag);
    FormatIdleTestUnsigned(p2Text, sizeof(p2Text), fighterReason,
        status.p2TransitionFlag);
    DrawIdleTestClauseRow("transition (+76)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_TRANSITION_PENDING) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_TRANSITION_PENDING) != 0);

    FormatIdleTestUnsigned(p1Text, sizeof(p1Text), fighterReason,
        status.p1ActionLockEnd);
    FormatIdleTestUnsigned(p2Text, sizeof(p2Text), fighterReason,
        status.p2ActionLockEnd);
    DrawIdleTestClauseRow("lock end (+56)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_ACTION_LOCK_SET) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_ACTION_LOCK_SET) != 0);

    if (fighterReason != nullptr)
    {
        std::snprintf(p1Text, sizeof(p1Text), "%s", fighterReason);
        std::snprintf(p2Text, sizeof(p2Text), "%s", fighterReason);
    }
    else
    {
        std::snprintf(p1Text, sizeof(p1Text), "0x%02lX  &7=%lu  &0x0F=%lu",
            static_cast<unsigned long>(status.p1MoveGateByte & 0xFFu),
            static_cast<unsigned long>(status.p1MoveGateByte & 0x07u),
            static_cast<unsigned long>(status.p1MoveGateByte & 0x0Fu));
        std::snprintf(p2Text, sizeof(p2Text), "0x%02lX  &7=%lu  &0x0F=%lu",
            static_cast<unsigned long>(status.p2MoveGateByte & 0xFFu),
            static_cast<unsigned long>(status.p2MoveGateByte & 0x07u),
            static_cast<unsigned long>(status.p2MoveGateByte & 0x0Fu));
    }
    DrawIdleTestClauseRow("posture (+156)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_POSTURE_NOT_GROUNDED) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_POSTURE_NOT_GROUNDED) != 0);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "fighter +156. The clause reads the low three bits and accepts 1\n"
            "or 2. The low NIBBLE is shown beside it because the game's manual\n"
            "recovery block switches on the nibble, not on & 7.\n"
            "This is NOT proven actionability. Do not read it as \"can act\".");
    }

    FormatIdleTestUnsigned(p1Text, sizeof(p1Text), neutralReason,
        status.p1TerminalReady);
    FormatIdleTestUnsigned(p2Text, sizeof(p2Text), neutralReason,
        status.p2TerminalReady);
    DrawIdleTestClauseRow("terminal ready (+1528)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_TERMINAL_LATCHED) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_TERMINAL_LATCHED) != 0);

    FormatIdleTestUnpublished(p1Text, sizeof(p1Text), neutralLive,
        (p1Mask & QOH99_TRAINING_NEUTRAL_ROUND_RESULT_LATCHED) != 0);
    FormatIdleTestUnpublished(p2Text, sizeof(p2Text), neutralLive,
        (p2Mask & QOH99_TRAINING_NEUTRAL_ROUND_RESULT_LATCHED) != 0);
    DrawIdleTestClauseRow("round result (+1364/+1368)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_ROUND_RESULT_LATCHED) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_ROUND_RESULT_LATCHED) != 0);

    FormatIdleTestUnpublished(p1Text, sizeof(p1Text), neutralLive,
        (p1Mask & QOH99_TRAINING_NEUTRAL_FATAL_RECORDED) != 0);
    FormatIdleTestUnpublished(p2Text, sizeof(p2Text), neutralLive,
        (p2Mask & QOH99_TRAINING_NEUTRAL_FATAL_RECORDED) != 0);
    DrawIdleTestClauseRow("fatal cause (+1376)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_FATAL_RECORDED) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_FATAL_RECORDED) != 0);

    std::snprintf(p1Text, sizeof(p1Text), "%lu  (mirror %lu)",
        static_cast<unsigned long>(status.p1ComPrimary),
        static_cast<unsigned long>(status.p1ComMirror));
    std::snprintf(p2Text, sizeof(p2Text), "%lu  (mirror %lu)",
        static_cast<unsigned long>(status.p2ComPrimary),
        static_cast<unsigned long>(status.p2ComMirror));
    DrawIdleTestClauseRow("CPU owns (+1400)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_COM_OWNED) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_COM_OWNED) != 0);

    FormatIdleTestUnsigned(p1Text, sizeof(p1Text), neutralReason,
        status.p1RecoveryLock);
    FormatIdleTestUnsigned(p2Text, sizeof(p2Text), neutralReason,
        status.p2RecoveryLock);
    DrawIdleTestClauseRow("recovery lock",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_RECOVERY_LOCK_SET) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_RECOVERY_LOCK_SET) != 0);

    FormatIdleTestUnsigned(p1Text, sizeof(p1Text), neutralReason,
        status.p1RecoveryPhaseLatch);
    FormatIdleTestUnsigned(p2Text, sizeof(p2Text), neutralReason,
        status.p2RecoveryPhaseLatch);
    DrawIdleTestClauseRow("recovery phase latch",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_RECOVERY_PHASE_LATCH) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_RECOVERY_PHASE_LATCH) != 0);

    FormatIdleTestUnsigned(p1Text, sizeof(p1Text), neutralReason,
        status.p1ComboHitsNative);
    FormatIdleTestUnsigned(p2Text, sizeof(p2Text), neutralReason,
        status.p2ComboHitsNative);
    DrawIdleTestClauseRow("combo hits (game's own)",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_COMBO_ACTIVE) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_COMBO_ACTIVE) != 0);

    if (combatReason != nullptr)
    {
        std::snprintf(p1Text, sizeof(p1Text), "%s", combatReason);
        std::snprintf(p2Text, sizeof(p2Text), "%s", combatReason);
    }
    else
    {
        std::snprintf(p1Text, sizeof(p1Text), "%d / %d / %d",
            static_cast<int>(static_cast<INT32>(status.p1LifePendingDamage)),
            static_cast<int>(static_cast<INT32>(status.p1LifeReserve)),
            static_cast<int>(static_cast<INT32>(status.p1LifeSettleTimer)));
        std::snprintf(p2Text, sizeof(p2Text), "%d / %d / %d",
            static_cast<int>(static_cast<INT32>(status.p2LifePendingDamage)),
            static_cast<int>(static_cast<INT32>(status.p2LifeReserve)),
            static_cast<int>(static_cast<INT32>(status.p2LifeSettleTimer)));
    }
    DrawIdleTestClauseRow("life +4 / +8 / +12",
        p1Text, (p1Mask & QOH99_TRAINING_NEUTRAL_DAMAGE_UNSETTLED) != 0,
        p2Text, (p2Mask & QOH99_TRAINING_NEUTRAL_DAMAGE_UNSETTLED) != 0);

    ImGui::Spacing();
    ImGui::TextDisabled("Match-wide clauses (one value, not one per side)");
    FormatIdleTestUnpublished(p1Text, sizeof(p1Text), neutralLive,
        (globalMask & QOH99_TRAINING_NEUTRAL_ROUND_RESULT_STATE) != 0);
    DrawIdleTestRow("round result state (0x88098C)",
        p1Text,
        (globalMask & QOH99_TRAINING_NEUTRAL_ROUND_RESULT_STATE) != 0
            ? &kIdleTestFailColor : nullptr,
        "", nullptr);
    FormatIdleTestUnpublished(p1Text, sizeof(p1Text), neutralLive,
        (globalMask & QOH99_TRAINING_NEUTRAL_FATAL_DISABLE_FLAG) != 0);
    DrawIdleTestRow("F4 damage flag (0x6275A0)",
        p1Text,
        (globalMask & QOH99_TRAINING_NEUTRAL_FATAL_DISABLE_FLAG) != 0
            ? &kIdleTestFailColor : nullptr,
        "", nullptr);

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Five clauses read words this status block does not carry -- +1456, "
        "+1364/+1368, +1376, 0x88098C and 0x6275A0 -- so those rows show "
        "\"not published\" and the clause bit rather than a number. The bit is "
        "still the authority; closing that gap properly means appending a new "
        "status tier, not printing a zero here.");
    if (!neutralLive)
    {
        TrainingTextDisabledWrapped(
            "The idle test itself is not being evaluated right now, so no row "
            "is marked as failing. It fails closed on purpose.");
    }

    ImGui::SeparatorText(qi18n::tr("Also useful, not clauses of the idle test"));
    DrawIdleTestHeader("Field");
    FormatIdleTestUnsigned(p1Text, sizeof(p1Text), fighterReason,
        status.p1ActionFrame);
    FormatIdleTestUnsigned(p2Text, sizeof(p2Text), fighterReason,
        status.p2ActionFrame);
    DrawIdleTestRow("action frame (+52)", p1Text, nullptr, p2Text, nullptr);
    FormatIdleTestUnsigned(p1Text, sizeof(p1Text), fighterReason,
        status.p1AnimFrame);
    FormatIdleTestUnsigned(p2Text, sizeof(p2Text), fighterReason,
        status.p2AnimFrame);
    DrawIdleTestRow("anim (+64)", p1Text, nullptr, p2Text, nullptr);
    FormatIdleTestAction(p1Text, sizeof(p1Text), fighterReason,
        status.p1NextAction);
    FormatIdleTestAction(p2Text, sizeof(p2Text), fighterReason,
        status.p2NextAction);
    DrawIdleTestRow("next (+72)", p1Text, nullptr, p2Text, nullptr);
    FormatIdleTestAction(p1Text, sizeof(p1Text), fighterReason,
        status.p1PrevAction);
    FormatIdleTestAction(p2Text, sizeof(p2Text), fighterReason,
        status.p2PrevAction);
    DrawIdleTestRow("prev (+80)", p1Text, nullptr, p2Text, nullptr);
    FormatIdleTestSigned(p1Text, sizeof(p1Text), combatReason,
        status.p1LifeCurrent);
    FormatIdleTestSigned(p2Text, sizeof(p2Text), combatReason,
        status.p2LifeCurrent);
    DrawIdleTestRow("life current (+0)", p1Text, nullptr, p2Text, nullptr);
    if (fighterReason != nullptr)
    {
        std::snprintf(p1Text, sizeof(p1Text), "%s", fighterReason);
        std::snprintf(p2Text, sizeof(p2Text), "%s", fighterReason);
    }
    else
    {
        std::snprintf(p1Text, sizeof(p1Text), "%d, %d",
            static_cast<int>(static_cast<INT32>(status.p1PositionX)),
            static_cast<int>(static_cast<INT32>(status.p1PositionY)));
        std::snprintf(p2Text, sizeof(p2Text), "%d, %d",
            static_cast<int>(static_cast<INT32>(status.p2PositionX)),
            static_cast<int>(static_cast<INT32>(status.p2PositionY)));
    }
    DrawIdleTestRow("position X, Y", p1Text, nullptr, p2Text, nullptr);
    std::snprintf(p1Text, sizeof(p1Text), "%lu",
        static_cast<unsigned long>(status.p1Facing));
    std::snprintf(p2Text, sizeof(p2Text), "%lu",
        static_cast<unsigned long>(status.p2Facing));
    DrawIdleTestRow("facing byte", p1Text, nullptr, p2Text, nullptr);
    std::snprintf(p1Text, sizeof(p1Text), "%lu",
        static_cast<unsigned long>(status.p1Character));
    std::snprintf(p2Text, sizeof(p2Text), "%lu",
        static_cast<unsigned long>(status.p2Character));
    DrawIdleTestRow("character id", p1Text, nullptr, p2Text, nullptr);
}

struct TrainingActionStateRowRef
{
    int side;
    int field;
    int index;
};

bool TrainingActionStateRowNewerFirst(
    const TrainingActionStateRowRef &a, const TrainingActionStateRowRef &b)
{
    const TrainingActionStateEntry &ea =
        g_actionStateLog.entries[a.side][a.field][a.index];
    const TrainingActionStateEntry &eb =
        g_actionStateLog.entries[b.side][b.field][b.index];
    if (ea.lastStep != eb.lastStep)
    {
        return ea.lastStep > eb.lastStep;
    }
    return ea.value < eb.value;
}

const char *TrainingActionStateFieldName(int field)
{
    return field == kActionStateLogFieldDispatch ? "+84" : "+48";
}

void BuildTrainingActionStateLogText(
    const Qoh99TrainingStatusV1 &status,
    const TrainingActionStateRowRef *rows, int rowCount,
    std::string &out)
{
    char line[192] = {};
    out.clear();
    out.reserve(64u * 1024u);
    out += "QOH99 training - distinct action states seen this session\n";
    std::snprintf(line, sizeof(line),
        "generation %lu, P1 char %lu, P2 char %lu\n",
        static_cast<unsigned long>(g_actionStateLog.generation),
        static_cast<unsigned long>(status.p1Character),
        static_cast<unsigned long>(status.p2Character));
    out += line;
    out += "+84 = fighter+84, what the state dispatcher (sub_4017F0) "
           "switches on\n";
    out += "+48 = fighter+48, what recovery eligibility (sub_47CAE0) "
           "gates on\n";
    out += "they are not always equal; 48 and 49 exist only at +48\n";
    std::snprintf(line, sizeof(line),
        "steps sampled %llu, steps missed %llu, steps with a field "
        "not licensed %llu\n",
        static_cast<unsigned long long>(g_actionStateLog.sampledSteps),
        static_cast<unsigned long long>(g_actionStateLog.gapSteps),
        static_cast<unsigned long long>(g_actionStateLog.unlicensedSteps));
    out += line;
    out += "frames and seen are exact when steps missed is 0, "
           "lower bounds otherwise\n";
    out += "field side char value hex   seen frames first_step last_step\n";
    for (int i = 0; i < rowCount; ++i)
    {
        const TrainingActionStateEntry &entry =
            g_actionStateLog.entries[rows[i].side][rows[i].field]
                                    [rows[i].index];
        std::snprintf(line, sizeof(line),
            "%-5s P%d   %4lu %5lu 0x%03lX %4lu %6lu %10llu %10llu\n",
            TrainingActionStateFieldName(rows[i].field),
            rows[i].side + 1,
            static_cast<unsigned long>(entry.character),
            static_cast<unsigned long>(entry.value),
            static_cast<unsigned long>(entry.value),
            static_cast<unsigned long>(entry.entries),
            static_cast<unsigned long>(entry.framesHeld),
            static_cast<unsigned long long>(entry.firstStep),
            static_cast<unsigned long long>(entry.lastStep));
        out += line;
    }
}

void DrawTrainingActionStateLog(const Qoh99TrainingStatusV1 &status)
{
    ImGui::TextWrapped(
        "Every distinct value of +84 and of +48 seen since this training "
        "session started, newest first. Sampled once per real game step while "
        "the menu is CLOSED, which is why a twelve-frame hitstun state that "
        "can never be read live still lands in the list.");
    TrainingTextDisabledWrapped(
        "\"Frames\" is the number of real steps the value was observed on and "
        "\"Seen\" the number of separate times it was entered. Both are exact "
        "when \"steps missed\" below is 0 and lower bounds otherwise.");
    ImGui::Spacing();

    ImGui::TextWrapped(
        "%lu distinct rows | %llu steps sampled | %llu steps missed "
        "| %llu steps with a field not licensed",
        static_cast<unsigned long>(
            g_actionStateLog.count[0][0] + g_actionStateLog.count[0][1]
            + g_actionStateLog.count[1][0] + g_actionStateLog.count[1][1]),
        static_cast<unsigned long long>(g_actionStateLog.sampledSteps),
        static_cast<unsigned long long>(g_actionStateLog.gapSteps),
        static_cast<unsigned long long>(g_actionStateLog.unlicensedSteps));
    DWORD droppedTotal = 0;
    for (int side = 0; side < kActionStateLogSides; ++side)
    {
        for (int field = 0; field < kActionStateLogFields; ++field)
        {
            droppedTotal += g_actionStateLog.dropped[side][field];
        }
    }
    if (droppedTotal != 0)
    {
        TrainingTextColoredWrapped(kIdleTestDivergedColor,
            "Table full: %lu further distinct values were not recorded.",
            static_cast<unsigned long>(droppedTotal));
    }

    ImGui::PushID("action_state_log");
    ImGui::Checkbox("P1", &g_actionStateLogFilters.showP1);
    ImGui::SameLine();
    ImGui::Checkbox("P2", &g_actionStateLogFilters.showP2);
    ImGui::SameLine();
    ImGui::Checkbox("+84", &g_actionStateLogFilters.showDispatch);
    ImGui::SameLine();
    ImGui::Checkbox("+48", &g_actionStateLogFilters.showAction);

    TrainingActionStateRowRef rows[
        kActionStateLogSides * kActionStateLogFields
        * kActionStateLogCapacity];
    int rowCount = 0;
    for (int side = 0; side < kActionStateLogSides; ++side)
    {
        if (side == 0 && !g_actionStateLogFilters.showP1) continue;
        if (side == 1 && !g_actionStateLogFilters.showP2) continue;
        for (int field = 0; field < kActionStateLogFields; ++field)
        {
            if (field == kActionStateLogFieldDispatch
                && !g_actionStateLogFilters.showDispatch)
            {
                continue;
            }
            if (field == kActionStateLogFieldAction
                && !g_actionStateLogFilters.showAction)
            {
                continue;
            }
            const DWORD count = g_actionStateLog.count[side][field];
            for (DWORD i = 0; i < count; ++i)
            {
                rows[rowCount].side = side;
                rows[rowCount].field = field;
                rows[rowCount].index = static_cast<int>(i);
                ++rowCount;
            }
        }
    }
    std::sort(rows, rows + rowCount, TrainingActionStateRowNewerFirst);

    if (ImGui::Button("Copy as text"))
    {
        std::string text;
        BuildTrainingActionStateLogText(status, rows, rowCount, text);
        ImGui::SetClipboardText(text.c_str());
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Copies the visible rows, honouring the four filters above,\n"
            "as plain text.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        ResetTrainingActionStateLog(g_actionStateLog.generation);
        ImGui::PopID();
        return;
    }

    if (rowCount == 0)
    {
        TrainingTextDisabledWrapped(
            "Nothing recorded yet. Close the menu and play -- the game does "
            "not step while this menu is holding it.");
        ImGui::PopID();
        return;
    }

    if (ImGui::BeginTable("action_state_log", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
            ImVec2(0.0f, 220.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Field");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Char");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Hex");
        ImGui::TableSetupColumn("Seen");
        ImGui::TableSetupColumn("Frames");
        ImGui::TableHeadersRow();
        for (int i = 0; i < rowCount; ++i)
        {
            const TrainingActionStateEntry &entry =
                g_actionStateLog.entries[rows[i].side][rows[i].field]
                                        [rows[i].index];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                TrainingActionStateFieldName(rows[i].field));
            ImGui::TableNextColumn();
            ImGui::Text("P%d", rows[i].side + 1);
            ImGui::TableNextColumn();
            ImGui::Text("%lu", static_cast<unsigned long>(entry.character));
            ImGui::TableNextColumn();
            ImGui::Text("%lu", static_cast<unsigned long>(entry.value));
            ImGui::TableNextColumn();
            ImGui::Text("0x%lX", static_cast<unsigned long>(entry.value));
            ImGui::TableNextColumn();
            ImGui::Text("%lu", static_cast<unsigned long>(entry.entries));
            ImGui::TableNextColumn();
            ImGui::Text("%lu", static_cast<unsigned long>(entry.framesHeld));
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void DrawTrainingCriticalControls(const Qoh99TrainingStatusV1 &status);
void DrawTrainingReduceControls(const Qoh99TrainingStatusV1 &status);
void DrawTrainingFaultlessControls(const Qoh99TrainingStatusV1 &status);
void DrawTrainingParryControls(const Qoh99TrainingStatusV1 &status);

void DrawTrainingTab(const Qoh99TrainingStatusV1 &status)
{
    if (!ImGui::BeginTabBar("qoh99_quick_tabs"))
    {
        return;
    }
    if (ImGui::BeginTabItem(qi18n::tr("Position")))
    {
        {
            ImGui::SeparatorText(qi18n::tr("Position"));
            const bool resetReady =
                TrainingFeatureReady(status, QOH99_TRAINING_CAP_RESET);
            ImGui::BeginDisabled(!resetReady);
            if (ImGui::Button("Round Start"))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
                    QOH99_TRAINING_POSITION_ROUND_START);
            }
            ImGui::SameLine();
            if (ImGui::Button("Left Corner"))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
                    QOH99_TRAINING_POSITION_LEFT_CORNER);
            }
            ImGui::SameLine();
            if (ImGui::Button("Center"))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
                    QOH99_TRAINING_POSITION_CENTER);
            }
            ImGui::SameLine();
            if (ImGui::Button("Right Corner"))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
                    QOH99_TRAINING_POSITION_RIGHT_CORNER);
            }
            ImGui::SameLine();
            if (ImGui::Button("Touching"))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
                    QOH99_TRAINING_POSITION_VERY_CLOSE);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Both characters at midscreen with their boxes touching,\n"
                    "for tight blocking and throw practice.");
            }
            ImGui::EndDisabled();

            const bool neutralResetReady =
                resetReady && TrainingHasRecoveryStatus(status);
            ImGui::BeginDisabled(!neutralResetReady);
            if (ImGui::Button(
                    "Round start + stand up##reset_round_start_neutral"))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_RESET_ROUND_START_NEUTRAL,
                    QOH99_TRAINING_REFILL_BOTH, 0);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(
                    "Round-start places, plus a request to drop both characters\n"
                    "back into their idle state. The game may decline the idle\n"
                    "part - mid-reaction, for instance - and if it does the\n"
                    "placement still stands and the line below says which parts\n"
                    "landed. The configured Round-start reset action does the\n"
                    "same.");
            }
            ImGui::EndDisabled();
            TrainingTextDisabledWrapped(
                "Resets place the characters only - health, meter, the guard "
                "gauge, the round count and the timer are left exactly as they "
                "are.");
            if (!resetReady)
            {
                DrawTrainingUnavailableReason(
                    status, QOH99_TRAINING_CAP_RESET);
            }
            else if (!neutralResetReady)
            {
                DrawTrainingStaleDllReason(
                    status, QOH99_TRAINING_STATUS_V1_RECOVERY_TIER);
            }
            else if (status.roundStartNeutralValid != 0)
            {
                struct NeutralBit { DWORD bit; const char *text; };
                static const NeutralBit kBits[] = {
                    { QOH99_TRAINING_ROUND_START_POSITION, "placement" },
                    { QOH99_TRAINING_ROUND_START_FACING, "facing" },
                    { QOH99_TRAINING_ROUND_START_ACTION, "idle state" },
                    { QOH99_TRAINING_ROUND_START_NO_PENDING, "nothing queued" },
                    { QOH99_TRAINING_ROUND_START_GROUNDED, "on the ground" },
                };
                char missing[192] = {};
                size_t used = 0;
                for (int i = 0; i < IM_ARRAYSIZE(kBits); ++i)
                {
                    if ((status.roundStartNeutralMask & kBits[i].bit) != 0)
                    {
                        continue;
                    }
                    const int written = std::snprintf(
                        missing + used, sizeof(missing) - used, "%s%s",
                        used == 0 ? "" : ", ", kBits[i].text);
                    if (written <= 0
                        || static_cast<size_t>(written)
                               >= sizeof(missing) - used)
                    {
                        missing[sizeof(missing) - 1] = '\0';
                        break;
                    }
                    used += static_cast<size_t>(written);
                }
                if (used != 0)
                {
                    TrainingTextColoredWrapped(
                        ImVec4(1.0f, 0.72f, 0.30f, 1.0f),
                        "Last idle reset: the game did not take %s.", missing);
                }
            }

        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(qi18n::tr("Resources")))
    {
        {
            ImGui::SeparatorText(qi18n::tr("Health, meter and guard gauge"));
            DrawTrainingResourcePolicies(status);
            ImGui::Spacing();
            DrawTrainingRefills(status);
            ImGui::Spacing();
            DrawTrainingResourceValues(status);
            ImGui::Spacing();
            DrawTrainingMeterCharge(status);

            ImGui::Spacing();
            if (ImGui::TreeNode("Critical hits"))
            {
                DrawTrainingCriticalControls(status);
                ImGui::TreePop();
            }

            ImGui::Spacing();
            if (ImGui::TreeNode("Damage reduce"))
            {
                DrawTrainingReduceControls(status);
                ImGui::TreePop();
            }

        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(qi18n::tr("Session")))
    {
        {
            ImGui::SeparatorText(qi18n::tr("Sides"));
            {
                const bool swapReady = TrainingHasPlayerSwapStatus(status);
                bool dummyIsP1 = swapReady && status.playerSwap != 0u;
                ImGui::BeginDisabled(!swapReady);
                if (ImGui::Checkbox("Dummy is P1 (you play P2)",
                                    &dummyIsP1))
                {
                    EnqueueTrainingCommand(
                        status, QOH99_TRAINING_COMMAND_SET_PLAYER_SWAP, 0,
                        dummyIsP1 ? 1u : 0u);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Choose which fighter is the dummy. The dummy is the\n"
                        "one that blocks, techs and plays back recordings - you\n"
                        "control the other one.\n\n"
                        "Recording always records the dummy, so with this on you\n"
                        "record P1. While you are recording you are moving the\n"
                        "dummy yourself, so the dummy settings apply to your own\n"
                        "fighter for the length of the recording - it is the one\n"
                        "you are recording against.\n\n"
                        "Cannot be changed while a recording is running or\n"
                        "playing back. Resets when you return to character\n"
                        "select.");
                }
            }

            ImGui::Spacing();
            ImGui::SeparatorText(qi18n::tr("Pause"));
            DrawTrainingHoldControls(status);

            ImGui::Spacing();
            ImGui::SeparatorText(qi18n::tr("Leave Training"));
            const bool returnReady = TrainingReturnToSelectReady(status);
            ImGui::BeginDisabled(!returnReady);
            if (ImGui::Button("Return to Character Select"))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_RETURN_TO_CHARACTER_SELECT,
                    0, 0);
            }
            ImGui::EndDisabled();
            if (!returnReady)
            {
                ImGui::TextWrapped(
                    "Available once a Training match is active.");
            }
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void DrawDummyRecoverySection(
    const Qoh99TrainingStatusV1 &status, bool dummyReady)
{
    ImGui::SeparatorText(qi18n::tr("Throw tech"));
    const bool combatToolsTier = TrainingHasCombatToolsStatus(status);
    const bool throwTechReady = combatToolsTier
        && TrainingFeatureReady(status, QOH99_TRAINING_CAP_THROW_TECH);
    SeedTrainingCombatToolDrafts(status);
    ImGui::BeginDisabled(!throwTechReady);
    if (ImGui::Checkbox(
            "Automatic throw tech##throw_tech",
            &g_trainingUi.throwTechEnabled))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_THROW_TECH_CHANCE, 0,
            g_trainingUi.throwTechEnabled
                ? QOH99_TRAINING_TRIGGER_MAX_CHANCE_PERCENT : 0u);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "The dummy immediately escapes every throw that can be teched.");
    }
    ImGui::EndDisabled();
    if (!combatToolsTier)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER);
    }
    else if (!throwTechReady)
    {
        DrawTrainingUnavailableReason(status, QOH99_TRAINING_CAP_THROW_TECH);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("Recovery after being hit"));

    const bool tierOk = TrainingHasRecoveryStatus(status);
    const bool recoveryReady = dummyReady && tierOk;

    TrainingTextDisabledWrapped(
        "The dummy taps a real direction twice, like a player would. The game "
        "can refuse it - nothing is written into the character to force one.");

    ImGui::PushID("recovery_policies");
    ImGui::BeginDisabled(!recoveryReady);

    static const char *kRecoveryModes[] = {
        "Off", "Earliest", "Fixed delay", "Random delay", "Late"
    };
    struct SlotRow
    {
        DWORD slot;
        const char *label;
        DWORD published;
        const char *tip;
    };
    const SlotRow slots[] = {
        { QOH99_TRAINING_RECOVERY_SLOT_AIR, "In the air (down, down)##air",
          status.recoveryModeAir,
          "Taps down, down while airborne after a hit." },
        { QOH99_TRAINING_RECOVERY_SLOT_GROUND,
          "Off the ground (up, up)##ground",
          status.recoveryModeGround,
          "Taps up, up after a knockdown.\n"
          "The dummy releases up afterwards and holds neutral, so a leftover\n"
          "jump cannot come out of the buffered taps." },
        { QOH99_TRAINING_RECOVERY_SLOT_WALL,
          "Off a wall (forward, forward)##wall",
          status.recoveryModeWall,
          "Taps forward, forward after hitting a wall. Forward is worked out\n"
          "from which way the dummy is facing, so it works in either corner." },
        { QOH99_TRAINING_RECOVERY_SLOT_TERMINAL,
           "At the end of a knockdown##terminal",
           status.recoveryModeTerminal,
           "The extra option at the very end of a knockdown. Which direction it\n"
           "takes is the setting below." },
    };

    for (int i = 0; i < IM_ARRAYSIZE(slots); ++i)
    {
        ImGui::PushID(static_cast<int>(slots[i].slot));
        int mode = static_cast<int>(slots[i].published);
        if (mode < QOH99_TRAINING_RECOVERY_OFF
            || mode > QOH99_TRAINING_RECOVERY_LATE)
        {
            mode = QOH99_TRAINING_RECOVERY_OFF;
        }
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo(
                slots[i].label, &mode, kRecoveryModes,
                IM_ARRAYSIZE(kRecoveryModes)))
        {
            g_trainingUi.requestedRecoveryMode[slots[i].slot] =
                static_cast<DWORD>(mode);
            g_trainingUi.recoveryRequestPending = true;
            g_trainingUi.recoveryRequestTick = GetTickCount();
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_RECOVERY_POLICY,
                slots[i].slot, static_cast<DWORD>(mode));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", slots[i].tip);
        }
        ImGui::PopID();
    }

    static const char *kTerminalChoices[] = {
        "Leave it alone", "Forward", "Back", "Random"
    };
    int terminal = static_cast<int>(status.recoveryTerminalChoice);
    if (terminal < QOH99_TRAINING_TERMINAL_PRESERVE
        || terminal > QOH99_TRAINING_TERMINAL_RANDOM)
    {
        terminal = QOH99_TRAINING_TERMINAL_PRESERVE;
    }
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo(
            "End-of-knockdown direction##terminal_choice", &terminal,
            kTerminalChoices,
            IM_ARRAYSIZE(kTerminalChoices)))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_TERMINAL_RECOVERY, 0,
            static_cast<DWORD>(terminal));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "\"Leave it alone\" is the default and the only setting that does\n"
            "not touch the game's own end-of-knockdown behaviour at all.");
    }

    ImGui::Spacing();
    if (!g_trainingUi.recoveryDraftsSeeded && tierOk)
    {
        g_trainingUi.recoveryDelaySteps =
            static_cast<int>(status.recoveryDelaySteps);
        g_trainingUi.recoveryRandomMin =
            static_cast<int>(status.recoveryRandomMinSteps);
        g_trainingUi.recoveryRandomMax =
            static_cast<int>(status.recoveryRandomMaxSteps);
        g_trainingUi.recoveryDraftsSeeded = true;
    }

    const int kDelayMax =
        static_cast<int>(QOH99_TRAINING_RECOVERY_DELAY_STEPS_MAX);
    bool timingChanged = false;
    ImGui::SetNextItemWidth(180.0f);
    timingChanged |= ImGui::SliderInt(
        "Delay (frames)##recovery_delay", &g_trainingUi.recoveryDelaySteps,
        0, kDelayMax);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "Used by \"Fixed delay\". Counted in frames on which the game would\n"
            "actually accept the recovery, not in frames since the hit.");
    }
    ImGui::SetNextItemWidth(180.0f);
    timingChanged |= ImGui::SliderInt(
        "Random from##recovery_min", &g_trainingUi.recoveryRandomMin,
        0, kDelayMax);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "Used by \"Random delay\". The same save state always replays the\n"
            "same sequence of random delays, so a setup stays reproducible.");
    }
    ImGui::SetNextItemWidth(180.0f);
    timingChanged |= ImGui::SliderInt(
        "Random to##recovery_max", &g_trainingUi.recoveryRandomMax,
        0, kDelayMax);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "Used by \"Random delay\". The same save state always replays the\n"
            "same sequence of random delays, so a setup stays reproducible.");
    }
    if (timingChanged)
    {
        if (g_trainingUi.recoveryRandomMax < g_trainingUi.recoveryRandomMin)
        {
            g_trainingUi.recoveryRandomMax = g_trainingUi.recoveryRandomMin;
        }
        const DWORD packed =
            (static_cast<DWORD>(g_trainingUi.recoveryRandomMin) & 0xFFFFu)
            | ((static_cast<DWORD>(g_trainingUi.recoveryRandomMax) & 0xFFFFu)
               << 16);
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_RECOVERY_TIMING, 0,
            static_cast<DWORD>(g_trainingUi.recoveryDelaySteps), packed);
    }

    ImGui::EndDisabled();
    ImGui::PopID();

    if (!dummyReady)
    {
        DrawTrainingUnavailableReason(status, QOH99_TRAINING_CAP_DUMMY_GUARD);
    }
    else if (!tierOk)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_RECOVERY_TIER);
    }

    const bool haveEvaluatorReason =
        TrainingHasRecoveryDiagnosticStatus(status);
    if (recoveryReady && haveEvaluatorReason
        && !TrainingRecoveryEvaluatorAllowsFiring(
               status.recoveryEvaluatorReason))
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
            "These settings will not take yet.");
        ImGui::TextWrapped("%s",
            TrainingRecoveryEvaluatorReasonText(
                status.recoveryEvaluatorReason));
        TrainingTextDisabledWrapped(
            "Reported by the injected DLL itself. Diagnostics > Recovery shows "
            "the same answer beside everything else that would have to be "
            "true.");
    }

    if (recoveryReady && !haveEvaluatorReason
        && g_trainingUi.recoveryRequestPending
        && GetTickCount() - g_trainingUi.recoveryRequestTick > 1500u)
    {
        const DWORD published[QOH99_TRAINING_RECOVERY_SLOT_COUNT] = {
            status.recoveryModeAir, status.recoveryModeGround,
            status.recoveryModeWall, status.recoveryModeTerminal,
            status.recoveryModeThrowEscape
        };
        bool refused = false;
        for (int i = 0;
             i < QOH99_TRAINING_RECOVERY_SLOT_THROW_ESCAPE; ++i)
        {
            if (published[i] != g_trainingUi.requestedRecoveryMode[i])
            {
                refused = true;
            }
        }
        if (refused)
        {
            TrainingTextColoredWrapped(
                ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
                "The game DLL turned that back off.");
            ImGui::TextWrapped(
                "Recovery is held back until the DLL has verified, against the "
                "exact game build you are running, that it is reading the right "
                "code for it. Until then only \"Off\" is accepted - that is the "
                "honest state, not a bug in this menu.");
        }
        else
        {
            g_trainingUi.recoveryRequestPending = false;
        }
    }

}

bool RecoveryActionInBranchSet(DWORD branch, DWORD action)
{
    switch (branch)
    {
    case QOH99_TRAINING_RECOVERY_BRANCH_AIR_22:
        return action == 25 || action == 38
            || action == 56 || action == 57 || action == 58;
    case QOH99_TRAINING_RECOVERY_BRANCH_GROUND_88:
        return action < 200
            && (action == 25 || action == 35 || action == 38
                || action == 56 || action == 57 || action == 58);
    case QOH99_TRAINING_RECOVERY_BRANCH_WALL_66:
        return action == 37;
    case QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_66:
    case QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_44:
    case QOH99_TRAINING_RECOVERY_BRANCH_THROW_ESCAPE:
        return action >= 200
            || action == 25 || action == 35 || action == 37 || action == 38
            || action == 56 || action == 57 || action == 58;
    default:
        return false;
    }
}

void DrawRecoveryTerm(const char *label, bool satisfied, bool known)
{
    ImGui::SameLine();
    if (!known)
    {
        ImGui::TextDisabled("%s ?", label);
        return;
    }
    ImGui::TextColored(
        satisfied ? ImVec4(0.35f, 0.90f, 0.48f, 1.0f)
                  : ImVec4(0.85f, 0.45f, 0.45f, 1.0f),
        satisfied ? "%s ok" : "%s NO", label);
}

void DrawTrainingRecoveryStandDown(
    const Qoh99TrainingStatusV1 &status, bool haveDiagnostics)
{
    if (!haveDiagnostics)
    {
        return;
    }
    if (status.recoveryStandDownMask
        == QOH99_TRAINING_RECOVERY_STAND_DOWN_NONE)
    {
        return;
    }

    ImGui::SeparatorText(qi18n::tr("The dummy stood down on the last frame that ran"));
    struct StandDownRow { DWORD bit; const char *text; };
    static const StandDownRow kRows[] = {
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_NOT_READABLE,
          "The dummy could not be read on that frame, so nothing below was "
          "checked at all." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_LIFECYCLE_DOWN,
          "Training was not running at that point." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_SANDBOX_INACTIVE,
          "Recovery only runs in offline training - never in an online "
          "match or a replay." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_MENU_CAPTURED,
          "This menu had your controller. A frame step still runs one "
          "frame while it is open, so stepping through a knockdown with this "
          "panel open can NEVER produce a recovery. Close the menu and use the "
          "pause key instead." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_OPERATION_NEUTRAL,
          "A training action was still finishing - a reset, or loading a "
          "saved position." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_MACRO_PLAYBACK,
          "A recording was playing back. Recovery stays off for the whole "
          "playback: if the recording holds a direction, the dummy would be "
          "left with half a double tap, which does nothing." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_MACRO_OWNS_P1,
          "A recording was being made, so your controller was moving the "
          "dummy." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_COM_CONTROLLED,
          "The dummy was under the game's own COM control, which ignores "
          "anything training sends it." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_DIRECTION_SUPPRESSED,
          "The game was ignoring every direction (it does this between "
          "rounds), so a tap sent then would have counted as neutral and "
          "done nothing." },
        { QOH99_TRAINING_RECOVERY_STAND_DOWN_KO_LATCH,
          "The dummy was already knocked out. That blocks every state "
          "change, including a successful recovery, so the game can read the "
          "double tap and still not move." },
    };
    for (int i = 0; i < IM_ARRAYSIZE(kRows); ++i)
    {
        if ((status.recoveryStandDownMask & kRows[i].bit) == 0)
        {
            continue;
        }
        ImGui::Bullet();
        ImGui::TextWrapped("%s", kRows[i].text);
    }
    if ((status.holdReasons & QOH99_TRAINING_HOLD_MENU) != 0)
    {
        TrainingTextDisabledWrapped(
            "The menu is holding the game right now, so these are the values "
            "from the last frame that actually ran, not from this instant.");
    }
}

void DrawTrainingRecoveryDebug(const Qoh99TrainingStatusV1 &status)
{
    if (!TrainingHasRecoveryStatus(status))
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_RECOVERY_TIER);
        return;
    }

    const bool haveDiagnostics = TrainingHasRecoveryDiagnosticStatus(status);
    if (haveDiagnostics)
    {
        const bool live = TrainingRecoveryEvaluatorAllowsFiring(
            status.recoveryEvaluatorReason);
        TrainingTextColoredWrapped(
            live ? ImVec4(0.35f, 0.90f, 0.48f, 1.0f)
                 : ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
            "%s", live ? "Recovery is live." : "Recovery is switched off at "
                                               "the source.");
        ImGui::TextWrapped("%s",
            TrainingRecoveryEvaluatorReasonText(
                status.recoveryEvaluatorReason));
        ImGui::Separator();
    }
    else
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER);
        ImGui::Separator();
    }

    ImGui::TextWrapped(
        "Attempts %lu   recovered %lu   missed %lu   cancelled %lu",
        static_cast<unsigned long>(status.recoveryAttempts),
        static_cast<unsigned long>(status.recoverySucceeded),
        static_cast<unsigned long>(status.recoveryMissed),
        static_cast<unsigned long>(status.recoveryCancelled));
    ImGui::TextWrapped(
        "Buffered %lu   the game's own %lu   refused after input %lu",
        static_cast<unsigned long>(status.recoveryBufferedObserved),
        static_cast<unsigned long>(status.recoveryAutomaticObserved),
        static_cast<unsigned long>(status.recoveryCommitBlocked));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "\"Buffered\" is deliberately its own bucket, not a success and not\n"
            "the game's own recovery: the game keeps a finished double tap\n"
            "matchable for the rest of its window, so a recovery can fire with\n"
            "no new input and there is no way to tell whose it was.\n"
            "\"The game's own\" is counted, never suppressed or forced.");
    }
    ImGui::TextWrapped("Last outcome: %s",
        TrainingRecoveryVerdictName(status.recoveryLastVerdict));
    if (haveDiagnostics)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(after %lu tap%s, %s, %lu frames left)",
            static_cast<unsigned long>(status.recoveryTapsEmitted),
            status.recoveryTapsEmitted == 1 ? "" : "s",
            TrainingRecoveryBranchName(status.recoveryBranch),
            static_cast<unsigned long>(status.recoveryStepsRemaining));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Taps counted for the attempt this outcome belongs to.\n"
                "\"Missed\" after 2 taps means the game saw a real double tap\n"
                "and refused it -- look at the terms below.\n"
                "\"Missed\" after 0 taps means the attempt never tapped at\n"
                "all -- look at the stand-down reasons instead.");
        }
    }
    ImGui::TextWrapped("Last resulting action: %lu  (0x%lX)",
        static_cast<unsigned long>(status.recoveryLastResultAction),
        static_cast<unsigned long>(status.recoveryLastResultAction));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Numeric only. There is no verified table of state names, so a\n"
            "label here would be an invention presented as a measurement.");
    }

    DrawTrainingRecoveryStandDown(status, haveDiagnostics);

    if (status.recoveryReadoutValid == 0)
    {
        TrainingTextDisabledWrapped(
            "Live recovery state can't be read right now - this needs a live "
            "training match.");
        return;
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Now: %s", TrainingRecoveryStateName(status.recoveryState));
    ImGui::SameLine();
    ImGui::TextDisabled("(%s, %lu frames left)",
        TrainingRecoveryBranchName(status.recoveryBranch),
        static_cast<unsigned long>(status.recoveryStepsRemaining));

    ImGui::SeparatorText(qi18n::tr("Why each option can or cannot fire (dummy side)"));
    TrainingTextDisabledWrapped(
        "Read from the dummy's own fields this frame. \"?\" means the injected "
        "DLL checks it but this menu is not given the number.");

    /* Manual recovery switches on (+156 & 0x0F) and only cases 1, 2, 4 reach it; terminal and throw-escape gates use (+156 & 7) in {1,2}. */
    const DWORD manualPosture = status.p2MoveGateByte & 0x0Fu;
    const DWORD terminalPosture = status.p2MoveGateByte & 0x07u;
    const bool postureOk =
        manualPosture == 1 || manualPosture == 2 || manualPosture == 4;
    const bool groundedPosture =
        terminalPosture == 1 || terminalPosture == 2;
    const bool airPermit = (status.p2AuthoredFrameFlags & 0x08u) != 0;
    const bool groundPermit = (status.p2AuthoredFrameFlags & 0x04u) != 0;
    const bool lockClear = status.p2RecoveryLock == 0;
    const bool humanOwned = status.p2ComPrimary == 0 && status.p2ComMirror == 0;
    const bool terminalReady = (status.p2TerminalReady & 1u) != 0;

    const DWORD kBranchMode[QOH99_TRAINING_RECOVERY_BRANCH_COUNT] = {
        status.recoveryModeAir, status.recoveryModeGround,
        status.recoveryModeWall, status.recoveryModeTerminal,
        status.recoveryModeTerminal, status.recoveryModeThrowEscape
    };
    static const char *const kModeText[] = {
        "off", "earliest", "fixed delay", "random delay", "late"
    };

    for (DWORD branch = 0;
         branch < QOH99_TRAINING_RECOVERY_BRANCH_COUNT; ++branch)
    {
        if (branch == QOH99_TRAINING_RECOVERY_BRANCH_THROW_ESCAPE)
        {
            continue;
        }
        ImGui::PushID(static_cast<int>(branch));
        const bool dllEligible =
            (status.recoveryEligibleMask & (1u << branch)) != 0;
        ImGui::TextColored(
            dllEligible ? ImVec4(0.35f, 0.90f, 0.48f, 1.0f)
                        : ImVec4(0.60f, 0.60f, 0.60f, 1.0f),
            "%-26s", TrainingRecoveryBranchName(branch));
        if (haveDiagnostics)
        {
            DrawRecoveryTerm(
                "armable", (status.recoveryArmableMask & (1u << branch)) != 0,
                true);
        }
        DrawRecoveryTerm(
            "state", RecoveryActionInBranchSet(branch, status.p2ActionState),
            true);
        if (!RecoveryActionInBranchSet(branch, status.p2ActionState))
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(+48 is %lu)",
                static_cast<unsigned long>(status.p2ActionState));
        }
        DrawRecoveryTerm("player-controlled", humanOwned, true);
        if (branch == QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_66
            || branch == QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_44
            || branch == QOH99_TRAINING_RECOVERY_BRANCH_THROW_ESCAPE)
        {
            DrawRecoveryTerm("end-of-knockdown", terminalReady, true);
            DrawRecoveryTerm("posture", groundedPosture, true);
        }
        else
        {
            DrawRecoveryTerm("posture", postureOk, true);
            const bool airBranch =
                branch == QOH99_TRAINING_RECOVERY_BRANCH_AIR_22;
            DrawRecoveryTerm(
                airBranch ? "+158 & 0x08" : "+158 & 0x04, 0x08 clear",
                airBranch ? airPermit : (groundPermit && !airPermit),
                true);
            DrawRecoveryTerm("not locked out", lockClear, true);
            if (!lockClear)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(lock is %lu)",
                    static_cast<unsigned long>(status.p2RecoveryLock));
            }
            if (branch == QOH99_TRAINING_RECOVERY_BRANCH_WALL_66)
            {
                DrawRecoveryTerm(
                    "touching a wall",
                    (status.p2WallFlags & 3u) != 0,
                    TrainingHasWallStatus(status));
            }
        }
        if (branch == QOH99_TRAINING_RECOVERY_BRANCH_THROW_ESCAPE
            && status.recoveryContactTestUnknown != 0)
        {
            DrawRecoveryTerm("throw contact", false, false);
        }
        {
            const DWORD mode = kBranchMode[branch];
            const char *const modeText =
                mode < IM_ARRAYSIZE(kModeText) ? kModeText[mode] : "unknown";
            const bool terminalFamily =
                branch == QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_66
                || branch == QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_44;
            if (terminalFamily
                && status.recoveryTerminalChoice
                       == QOH99_TRAINING_TERMINAL_PRESERVE)
            {
                TrainingTextDisabledWrapped(
                    "    setting: terminal direction is \"Leave it alone\", so "
                    "this is never attempted whatever the mode says");
            }
            else if (mode == QOH99_TRAINING_RECOVERY_OFF)
            {
                ImGui::TextDisabled(
                    "    setting: off -- not attempted");
            }
            else
            {
                ImGui::TextDisabled("    setting: %s", modeText);
            }
        }
        ImGui::PopID();
    }
    TrainingTextDisabledWrapped(
        "The name in colour is the injected DLL's own answer, and it is the one "
        "that counts: it also checks things this menu never sees, including the "
        "newest entry in the input history and the round-transition input mute.");

    ImGui::SeparatorText(qi18n::tr("Raw fields"));
    DrawTrainingReadoutHeader("Field");
    char p1Text[64] = {};
    char p2Text[64] = {};
    DrawTrainingReadoutPair("action (+48)",
        static_cast<unsigned long>(status.p1ActionState),
        static_cast<unsigned long>(status.p2ActionState));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The state the recovery routine gates on. Numeric only: the public\n"
            "names of the reaction states are unresolved, and control flow only\n"
            "proves which numbers belong to which branch.\n"
            "Anything from 200 up is a throw -- you cannot recover out of one.");
    }
    std::snprintf(p1Text, sizeof(p1Text), "0x%02lX",
        static_cast<unsigned long>(status.p1AuthoredFrameFlags & 0xFFu));
    std::snprintf(p2Text, sizeof(p2Text), "0x%02lX",
        static_cast<unsigned long>(status.p2AuthoredFrameFlags & 0xFFu));
    DrawTrainingReadoutRow("frame flags (+158)", p1Text, p2Text);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Bit 0x08 is the frame's air-recovery permission, 0x04 the ground\n"
            "one. They are authored per animation frame, which is why a\n"
            "recovery window opens and closes without anything else changing.");
    }
    std::snprintf(p1Text, sizeof(p1Text), "0x%02lX  (%s)",
        static_cast<unsigned long>(status.p1MoveGateByte & 0xFFu),
        TrainingPostureClassName(status.p1PostureClass));
    std::snprintf(p2Text, sizeof(p2Text), "0x%02lX  (%s)",
        static_cast<unsigned long>(status.p2MoveGateByte & 0xFFu),
        TrainingPostureClassName(status.p2PostureClass));
    DrawTrainingReadoutRow("posture (+156)", p1Text, p2Text);
    DrawTrainingReadoutPair("terminal ready (+1528)",
        static_cast<unsigned long>(status.p1TerminalReady),
        static_cast<unsigned long>(status.p2TerminalReady));
    /* The game normalises Forward/Back against +170 before recording them. */
    {
        const char *const kWall[4] = {
            "none", "world LEFT", "world RIGHT", "both"
        };
        if (TrainingHasWallStatus(status))
        {
            std::snprintf(p1Text, sizeof(p1Text), "%s  (facing byte %lu)",
                kWall[status.p1WallFlags & 3u],
                static_cast<unsigned long>(status.p1Facing));
            std::snprintf(p2Text, sizeof(p2Text), "%s  (facing byte %lu)",
                kWall[status.p2WallFlags & 3u],
                static_cast<unsigned long>(status.p2Facing));
        }
        else
        {
            std::snprintf(p1Text, sizeof(p1Text), "not published");
            std::snprintf(p2Text, sizeof(p2Text), "not published");
        }
        DrawTrainingReadoutRow("wall contact (+1390)", p1Text, p2Text);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "World side, not \"in front of me\". The game turns Forward/Back\n"
            "into world left/right before it records them, and a facing byte of\n"
            "1 is the side whose left/right input is mirrored -- the two\n"
            "together are what make the wall command readable.\n"
            "The recovery routine never reads this field: it is a sanity check.\n"
            "Wall reaction state 37 is the game's own proxy for wall contact, so\n"
            "a 37 with no wall here means a stale reaction.");
    }
    DrawTrainingReadoutPair("dispatch action (+84)",
        static_cast<unsigned long>(status.p1DispatchAction),
        static_cast<unsigned long>(status.p2DispatchAction));
    std::snprintf(p1Text, sizeof(p1Text), "%d",
        static_cast<int>(static_cast<INT32>(status.p1InitStatus)));
    std::snprintf(p2Text, sizeof(p2Text), "%d",
        static_cast<int>(static_cast<INT32>(status.p2InitStatus)));
    DrawTrainingReadoutRow("init status (+88)", p1Text, p2Text);
    DrawTrainingReadoutPair("recovery lock",
        static_cast<unsigned long>(status.p1RecoveryLock),
        static_cast<unsigned long>(status.p2RecoveryLock));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "A per-character lock the game sets from the attack that hit, and\n"
            "clears itself when the combo ends. Nonzero means manual recovery\n"
            "is locked out. Training only ever READS it.");
    }
    DrawTrainingReadoutPair("recovery phase latch",
        static_cast<unsigned long>(status.p1RecoveryPhaseLatch),
        static_cast<unsigned long>(status.p2RecoveryPhaseLatch));
    DrawTrainingReadoutPair("combo hits (game's own)",
        static_cast<unsigned long>(status.p1ComboHitsNative),
        static_cast<unsigned long>(status.p2ComboHitsNative));
}

void EnqueueTrainingTriggerConfig(
    const Qoh99TrainingStatusV1 &status, int event)
{
    if (g_trainingUi.triggerMoveIndex[event] > 0
        && !TrainingActionTriggerCatalogMatches(status))
    {
        return;
    }
    const DWORD packed =
        (static_cast<DWORD>(g_trainingUi.triggerDelaySteps[event]) & 0xFFFFu)
        | ((static_cast<DWORD>(g_trainingUi.triggerChancePercent[event])
            & 0xFFFFu) << 16u);
    EnqueueTrainingCommand(
        status, QOH99_TRAINING_COMMAND_SET_TRIGGER_CONFIG,
        static_cast<DWORD>(event),
        g_trainingUi.triggerMoveIndex[event] > 0
            ? QOH99_TRAINING_TRIGGER_MOVE_VALUE_FLAG
                | static_cast<DWORD>(g_trainingUi.triggerMoveIndex[event])
            : static_cast<DWORD>(g_trainingUi.triggerMacroSlot[event]),
        packed);
}

bool TrainingMoveWasAlreadyListed(
    DWORD character, uint16_t moveIndex, qtraining::TrainingTriggerEvent event)
{
    const qtraining::TrainingMoveView move =
        qtraining::trainingMove(static_cast<uint8_t>(character), moveIndex);
    if (!qtraining::trainingMoveTriggerEligible(move, event))
    {
        return true;
    }
    for (uint16_t earlier = 1u; earlier < moveIndex; ++earlier)
    {
        const qtraining::TrainingMoveView candidate = qtraining::trainingMove(
            static_cast<uint8_t>(character), earlier);
        if (qtraining::trainingMoveTriggerEligible(candidate, event)
            && candidate.input == move.input
            && candidate.context == move.context)
        {
            return true;
        }
    }
    return false;
}

const char *TrainingMoveContextLabel(qtraining::TrainingMoveContext context)
{
    switch (context)
    {
    case qtraining::TrainingMoveContext::Ground: return "Ground";
    case qtraining::TrainingMoveContext::Air: return "Air";
    case qtraining::TrainingMoveContext::Either: return "Ground or air";
    case qtraining::TrainingMoveContext::Close: return "Close range";
    case qtraining::TrainingMoveContext::Guard: return "While guarding";
    case qtraining::TrainingMoveContext::FollowUp: return "Follow-up";
    }
    return "Unknown context";
}

const char *TrainingReactionPreview(
    const Qoh99TrainingStatusV1 &status, int event, char *scratch,
    size_t scratchSize)
{
    if (g_trainingUi.triggerMoveIndex[event] > 0
        && TrainingActionTriggerCatalogMatches(status))
    {
        const qtraining::TrainingMoveView move = qtraining::trainingMove(
            static_cast<uint8_t>(status.p2Character),
            static_cast<uint16_t>(g_trainingUi.triggerMoveIndex[event]));
        if (move.valid()) return move.input.data();
        return "Unavailable move";
    }
    if (g_trainingUi.triggerMacroSlot[event]
        == static_cast<int>(QOH99_TRAINING_TRIGGER_RANDOM_RECORDING))
    {
        return "Random recording";
    }
    if (g_trainingUi.triggerMacroSlot[event] > 0)
    {
        std::snprintf(
            scratch, scratchSize, "Recording %d",
            g_trainingUi.triggerMacroSlot[event]);
        return scratch;
    }
    return "Off";
}

void DrawDummyReactionsSection(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Triggers"));
    TrainingTextDisabledWrapped(
        "Pick what the dummy does after something happens to it.");

    const bool tierReady = TrainingHasCombatToolsStatus(status);
    const bool reactionsReady = tierReady
        && TrainingFeatureReady(status, QOH99_TRAINING_CAP_TRIGGERS);
    const bool moveCharacterReady = status.p2Character <= 0xFFu
        && qtraining::trainingMoveSelectableCharacterId(
               static_cast<uint8_t>(status.p2Character))
            != qtraining::kTrainingMoveInvalidCharacter;
    const bool movesReady = reactionsReady
        && TrainingActionTriggerCatalogMatches(status)
        && moveCharacterReady;
    SeedTrainingCombatToolDrafts(status);

    static const char *const kEventNames[] = {
        "On hit",
        "After block",
        "After hitstun",
        "On wakeup",
        "After air tech",
        "After ground tech",
        "After throw tech",
        "Guard cancel"
    };
    static const char *const kEventTips[] = {
        "The moment your attack connects.",
        "As soon as it can move after blocking.",
        "As soon as it can move after being hit.",
        "Getting up with no tech. Auto recovery uses a tech row instead.",
        "As soon as it can move after teching in the air.",
        "As soon as it can move after teching on the ground.",
        "As soon as it can move after teching a throw.",
        "During blockstun, when the game allows it. Specials and supers only."
    };
    static_assert(IM_ARRAYSIZE(kEventNames)
                      == QOH99_TRAINING_TRIGGER_EVENT_COUNT,
                  "reaction labels must mirror the trigger event ABI");

    ImGui::BeginDisabled(!reactionsReady);
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable(
            "training_reactions", 4, tableFlags, ImVec2(0.0f, 0.0f)))
    {
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 2.1f);
        ImGui::TableSetupColumn("Response", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Delay", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Chance", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        for (int event = 0; event < QOH99_TRAINING_TRIGGER_EVENT_COUNT; ++event)
        {
            ImGui::PushID(event);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kEventNames[event]);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", kEventTips[event]);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            char previewScratch[32] = {};
            const char *preview = TrainingReactionPreview(
                status, event, previewScratch, sizeof(previewScratch));
            if (ImGui::BeginCombo("##response", preview))
            {
                const bool offSelected =
                    g_trainingUi.triggerMacroSlot[event] == 0
                    && g_trainingUi.triggerMoveIndex[event] == 0;
                if (ImGui::Selectable("Off", offSelected))
                {
                    g_trainingUi.triggerMacroSlot[event] = 0;
                    g_trainingUi.triggerMoveIndex[event] = 0;
                    EnqueueTrainingTriggerConfig(status, event);
                }

                if (movesReady)
                {
                    ImGui::SeparatorText(qi18n::tr("Character moves"));
                    const auto triggerEvent =
                        static_cast<qtraining::TrainingTriggerEvent>(event);
                    const uint16_t moveCount = qtraining::trainingMoveCount(
                        static_cast<uint8_t>(status.p2Character));
                    for (uint16_t moveIndex = 1u;
                         moveIndex <= moveCount; ++moveIndex)
                    {
                        if (TrainingMoveWasAlreadyListed(
                                status.p2Character, moveIndex, triggerEvent))
                        {
                            continue;
                        }
                        const qtraining::TrainingMoveView move =
                            qtraining::trainingMove(
                                static_cast<uint8_t>(status.p2Character),
                                moveIndex);
                        const bool selected =
                            g_trainingUi.triggerMoveIndex[event]
                                == static_cast<int>(moveIndex);
                        ImGui::PushID(static_cast<int>(moveIndex));
                        if (ImGui::Selectable(move.input.data(), selected))
                        {
                            g_trainingUi.triggerMoveIndex[event] = moveIndex;
                            g_trainingUi.triggerMacroSlot[event] = 0;
                            EnqueueTrainingTriggerConfig(status, event);
                        }
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip(
                                "%s", TrainingMoveContextLabel(move.context));
                        }
                        ImGui::PopID();
                    }
                }

                ImGui::SeparatorText(qi18n::tr("Recordings"));
                {
                    const int random = static_cast<int>(
                        QOH99_TRAINING_TRIGGER_RANDOM_RECORDING);
                    const bool selected =
                        g_trainingUi.triggerMoveIndex[event] == 0
                        && g_trainingUi.triggerMacroSlot[event] == random;
                    if (ImGui::Selectable("Random recording", selected))
                    {
                        g_trainingUi.triggerMoveIndex[event] = 0;
                        g_trainingUi.triggerMacroSlot[event] = random;
                        EnqueueTrainingTriggerConfig(status, event);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "Plays one of the recorded slots (*), picked\n"
                            "again every time the trigger fires.");
                    }
                }
                for (int slot = 1;
                     slot <= static_cast<int>(QOH99_TRAINING_SLOT_COUNT);
                     ++slot)
                {
                    const bool occupied =
                        (status.validMacroSlots & (1u << (slot - 1))) != 0;
                    char label[24] = {};
                    std::snprintf(label, sizeof(label), "Recording %d%s", slot,
                                  occupied ? " *" : "");
                    const bool selected =
                        g_trainingUi.triggerMoveIndex[event] == 0
                        && g_trainingUi.triggerMacroSlot[event] == slot;
                    ImGui::PushID(1000 + slot);
                    if (ImGui::Selectable(label, selected))
                    {
                        g_trainingUi.triggerMoveIndex[event] = 0;
                        g_trainingUi.triggerMacroSlot[event] = slot;
                        EnqueueTrainingTriggerConfig(status, event);
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt(
                    "##delay", &g_trainingUi.triggerDelaySteps[event], 1.0f, 0,
                    static_cast<int>(QOH99_TRAINING_TRIGGER_MAX_DELAY_STEPS),
                    "%d", ImGuiSliderFlags_AlwaysClamp))
            {
                EnqueueTrainingTriggerConfig(status, event);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Frames to wait first.");
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt(
                    "##chance", &g_trainingUi.triggerChancePercent[event],
                    1.0f, 0,
                    static_cast<int>(
                        QOH99_TRAINING_TRIGGER_MAX_CHANCE_PERCENT),
                    "%d%%", ImGuiSliderFlags_AlwaysClamp))
            {
                EnqueueTrainingTriggerConfig(status, event);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "How often it answers. At 0 the row never fires, even\n"
                    "with a response set.");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    if (reactionsReady && !movesReady)
    {
        TrainingTextDisabledWrapped(
            "Character moves need matching files. Recordings still work.");
    }

    if (!tierReady)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER);
    }
    else if (!reactionsReady)
    {
        DrawTrainingUnavailableReason(status, QOH99_TRAINING_CAP_TRIGGERS);
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!reactionsReady);
    if (ImGui::Button("Turn everything off##triggers_clear"))
    {
        for (int event = 0; event < QOH99_TRAINING_TRIGGER_EVENT_COUNT;
             ++event)
        {
            g_trainingUi.triggerMacroSlot[event] = 0;
            g_trainingUi.triggerMoveIndex[event] = 0;
            EnqueueTrainingTriggerConfig(status, event);
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Sets every row above back to Off.");
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::TreeNode("Details"))
    {
        TrainingTextDisabledWrapped(
            "Delay counts from the moment named in each row's tooltip.");
        TrainingTextDisabledWrapped(
            "If two could fire at once: Guard cancel first, then wakeup and "
            "tech rows, then After block and After hitstun, then On hit.");
        TrainingTextDisabledWrapped(
            "Recordings can start instantly. Character moves wait until the "
            "dummy is allowed to act.");
        if (reactionsReady)
        {
            ImGui::Spacing();
            TrainingTextDisabledWrapped(
                "Scheduler: queued %lu, played %lu, skipped by chance %lu, "
                "missed %lu.",
                static_cast<unsigned long>(status.triggerPendingCount),
                static_cast<unsigned long>(status.triggerStartedCount),
                static_cast<unsigned long>(status.triggerChanceSkippedCount),
                static_cast<unsigned long>(status.triggerDroppedCount));
        }
        ImGui::TreePop();
    }
}

bool TrainingContactPolicyReady(const Qoh99TrainingStatusV1 &status,
                                DWORD capability, DWORD available)
{
    return TrainingHasContactPolicyStatus(status)
        && (status.capabilityFlags & capability) != 0u
        && available != 0u;
}

void DrawTrainingContactPolicyUnavailable(
    const Qoh99TrainingStatusV1 &status)
{
    if (!TrainingHasContactPolicyStatus(status))
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_CONTACT_POLICY_TIER);
        return;
    }
    TrainingTextDisabledWrapped(
        "Unavailable: the native seam this needs did not qualify against the "
        "game build you are running. Everything else in Training is "
        "unaffected and this mechanic stays native.");
}

void DrawTrainingFaultlessControls(const Qoh99TrainingStatusV1 &status)
{
    const bool ready = TrainingContactPolicyReady(
        status, QOH99_TRAINING_CAP_FAULTLESS_POLICY, status.faultlessAvailable);
    static const char *kModes[] = {
        "Manual", "Always when legal", "Never" };
    int mode = TrainingHasContactPolicyStatus(status)
        ? static_cast<int>(status.faultlessPolicy) : 0;
    if (mode < 0 || mode > 2) mode = 0;

    ImGui::BeginDisabled(!ready);
    if (ImGui::Combo("Faultless##faultless_policy", &mode, kModes,
                     IM_ARRAYSIZE(kModes)))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
            QOH99_TRAINING_CONTACT_SLOT_FAULTLESS,
            static_cast<DWORD>(mode));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "\"Always when legal\" does NOT make the dummy block. It upgrades a\n"
            "block the dummy was already making. Pair it with a Guard setting.\n"
            "The native character list and the meter cost both still apply.");
    }
    ImGui::EndDisabled();

    if (!ready)
    {
        DrawTrainingContactPolicyUnavailable(status);
        return;
    }

    if (mode == QOH99_TRAINING_FAULTLESS_ALWAYS_WHEN_LEGAL
        && status.guardMode == QOH99_TRAINING_GUARD_OFF)
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.72f, 0.30f, 1.0f),
            "Guard is off, so nothing will happen. Faultless upgrades a block "
            "the dummy is already making - it does not start one. Set Guard "
            "above to anything except \"No guard\".");
    }

    if (status.faultlessSupportedDummy != 0u)
    {
        const bool enough = status.dummyFractionalMeter > 6u;
        TrainingTextColoredWrapped(
            enough ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                   : ImVec4(1.0f, 0.72f, 0.30f, 1.0f),
            "Supported. Meter %lu (needs more than 6).",
            static_cast<unsigned long>(status.dummyFractionalMeter));
    }
    else
    {
        TrainingTextDisabledWrapped(
            "This character has no Faultless Defence. Mizuho, Ruriko, Kaede, "
            "Tomoko, Kotone, Kanako and True Heroine Kotone do.");
    }
    if (status.lastFaultlessFinalMode != 0u
        || status.lastFaultlessNativeMode != 0u)
    {
        TrainingTextDisabledWrapped(
            "Last contact: native mode %lu, final mode %lu.",
            static_cast<unsigned long>(status.lastFaultlessNativeMode),
            static_cast<unsigned long>(status.lastFaultlessFinalMode));
    }
}

void DrawTrainingParryControls(const Qoh99TrainingStatusV1 &status)
{
    const bool ready = TrainingContactPolicyReady(
        status, QOH99_TRAINING_CAP_PARRY_POLICY, status.parryAvailable);
    static const char *kModes[] = {
        "Manual", "Always when legal", "Never" };
    int mode = TrainingHasContactPolicyStatus(status)
        ? static_cast<int>(status.parryPolicy) : 0;
    if (mode < 0 || mode > 2) mode = 0;

    ImGui::BeginDisabled(!ready);
    if (ImGui::Combo("Parry##parry_policy", &mode, kModes,
                     IM_ARRAYSIZE(kModes)))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
            QOH99_TRAINING_CONTACT_SLOT_PARRY, static_cast<DWORD>(mode));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "\"Always when legal\" forces the native decision AFTER the game has\n"
            "already checked the character, the posture and whether the attack\n"
            "can be parried at all. It is not a simulated forward-tap, and it\n"
            "will not parry an attack the game says is unparryable.");
    }
    ImGui::EndDisabled();

    if (!ready)
    {
        DrawTrainingContactPolicyUnavailable(status);
        return;
    }

    if (status.parrySupportedDummy != 0u)
    {
        TrainingTextColoredWrapped(
            ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Supported.");
    }
    else
    {
        TrainingTextDisabledWrapped(
            "This character cannot parry. Saori, Azusa, Aoi, LF'97 Aoi, Ayaka "
            "and Rapid Ayaka can.");
    }
    if (status.lastParryFinalResult != 0u || status.lastParryNativeResult != 0u)
    {
        TrainingTextDisabledWrapped(
            "Last parryable contact: native %lu, final %lu (2 = parried).",
            static_cast<unsigned long>(status.lastParryNativeResult),
            static_cast<unsigned long>(status.lastParryFinalResult));
    }
}

void DrawTrainingReduceControls(const Qoh99TrainingStatusV1 &status)
{
    const bool ready = TrainingHasReducePolicyStatus(status)
        && TrainingContactPolicyReady(
               status, QOH99_TRAINING_CAP_REDUCE_POLICY,
               status.reduceAvailable);
    static const char *kModes[] = {
        "Manual", "Always strong (70%)", "Always light (90%)", "Off",
    };

    ImGui::BeginDisabled(!ready);
    int p1 = ready ? static_cast<int>(status.reduceP1Policy) : 0;
    int p2 = ready ? static_cast<int>(status.reduceP2Policy) : 0;
    if (p1 < 0 || p1 >= IM_ARRAYSIZE(kModes)) p1 = 0;
    if (p2 < 0 || p2 >= IM_ARRAYSIZE(kModes)) p2 = 0;

    if (ImGui::Combo("P1 is hit##reduce_p1", &p1, kModes,
                     IM_ARRAYSIZE(kModes)))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
            QOH99_TRAINING_CONTACT_SLOT_REDUCE_P1, static_cast<DWORD>(p1));
    }
    if (ImGui::Combo("P2 is hit##reduce_p2", &p2, kModes,
                     IM_ARRAYSIZE(kModes)))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
            QOH99_TRAINING_CONTACT_SLOT_REDUCE_P2, static_cast<DWORD>(p2));
    }
    ImGui::EndDisabled();

    if (!ready)
    {
        DrawTrainingContactPolicyUnavailable(status);
        return;
    }

    TrainingTextDisabledWrapped(
        "Mashing attack buttons while being comboed reduces each hit: about "
        "two quick taps inside 10 frames = 70%% damage, inside 25 = 90%%. "
        "Off gives clean numbers for damage testing.");
}

void DrawTrainingCriticalControls(const Qoh99TrainingStatusV1 &status)
{
    const bool ready = TrainingContactPolicyReady(
        status, QOH99_TRAINING_CAP_CRITICAL_POLICY, status.criticalAvailable);
    static const char *kModes[] = {
        "Normal chance",
        "Always (one critical)",
        "No random criticals",
        "Fully disabled",
        "Advanced: force the roll",
    };

    ImGui::BeginDisabled(!ready);
    int p1 = TrainingHasContactPolicyStatus(status)
        ? static_cast<int>(status.criticalP1Policy) : 0;
    int p2 = TrainingHasContactPolicyStatus(status)
        ? static_cast<int>(status.criticalP2Policy) : 0;
    if (p1 < 0 || p1 >= IM_ARRAYSIZE(kModes)) p1 = 0;
    if (p2 < 0 || p2 >= IM_ARRAYSIZE(kModes)) p2 = 0;

    if (ImGui::Combo("P1 attacks##crit_p1", &p1, kModes, IM_ARRAYSIZE(kModes)))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
            QOH99_TRAINING_CONTACT_SLOT_CRITICAL_P1, static_cast<DWORD>(p1));
    }
    if (ImGui::Combo("P2 attacks##crit_p2", &p2, kModes, IM_ARRAYSIZE(kModes)))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
            QOH99_TRAINING_CONTACT_SLOT_CRITICAL_P2, static_cast<DWORD>(p2));
    }
    if (ImGui::Button("Copy P1 to P2##crit_copy"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
            QOH99_TRAINING_CONTACT_SLOT_CRITICAL_P2, static_cast<DWORD>(p1));
    }
    ImGui::EndDisabled();

    if (!ready)
    {
        DrawTrainingContactPolicyUnavailable(status);
        return;
    }

    TrainingTextDisabledWrapped(
        "Native chance rises with the combo: 1 in 32 for hits 1-8, 1 in 8 for "
        "9-16, 1 in 4 from 17.");

    if (p1 == QOH99_TRAINING_CRITICAL_FORCE_RANDOM_ROLL
        || p2 == QOH99_TRAINING_CRITICAL_FORCE_RANDOM_ROLL)
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.72f, 0.30f, 1.0f),
            "\"Force the roll\" stacks with the guard-crush critical and can "
            "produce roughly quadruple damage. \"Always\" deliberately does "
            "not.");
    }
    if (p1 == QOH99_TRAINING_CRITICAL_NEVER_ANY
        || p2 == QOH99_TRAINING_CRITICAL_NEVER_ANY)
    {
        TrainingTextDisabledWrapped(
            "\"Fully disabled\" also suppresses the guard-crush critical. The "
            "guard crush itself still happens and the gauge still resets.");
    }

    if (status.lastCriticalDivisor != 0u)
    {
        ImGui::Spacing();
        if (ImGui::TreeNode("Last critical decision"))
        {
            TrainingTextDisabledWrapped(
                "Attacker P%lu, incoming hit %lu, divisor %lu.",
                static_cast<unsigned long>(
                    status.lastCriticalAttackerSlot + 1u),
                static_cast<unsigned long>(status.lastCriticalIncomingHit),
                static_cast<unsigned long>(status.lastCriticalDivisor));
            TrainingTextDisabledWrapped(
                "Native sample %lu, used %lu. Guard-crush latch %lu.",
                static_cast<unsigned long>(status.lastCriticalNativeSample),
                static_cast<unsigned long>(status.lastCriticalFinalSample),
                static_cast<unsigned long>(
                    status.lastCriticalGuardCrushLatch));
            TrainingTextDisabledWrapped(
                "Damage %lu -> %lu, critical %s.",
                static_cast<unsigned long>(status.lastCriticalDamageBefore),
                static_cast<unsigned long>(status.lastCriticalDamageAfter),
                status.lastCriticalFinalFlag != 0u ? "yes" : "no");
            ImGui::TreePop();
        }
    }
}

void DrawDummyTab(const Qoh99TrainingStatusV1 &status)
{
    const bool ready =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_DUMMY_GUARD);

    if (!ImGui::BeginTabBar("qoh99_dummy_tabs"))
    {
        return;
    }
    if (ImGui::BeginTabItem(qi18n::tr("Guard & stance")))
    {
        {
            ImGui::SeparatorText(qi18n::tr("Defence"));
            ImGui::BeginDisabled(!ready);
            static const char *kGuardModes[] = {
                "Off", "All", "Adaptive", "Stand", "Crouch", "Air"
            };
            static const DWORD kGuardModeValues[] = {
                QOH99_TRAINING_GUARD_OFF,
                QOH99_TRAINING_GUARD_AUTO_RECORD,
                QOH99_TRAINING_GUARD_AUTO_HEURISTIC,
                QOH99_TRAINING_GUARD_ALWAYS_STAND,
                QOH99_TRAINING_GUARD_ALWAYS_CROUCH,
                QOH99_TRAINING_GUARD_ALWAYS_AIR
            };
            int guardMode = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kGuardModeValues); ++i)
            {
                if (status.guardMode == kGuardModeValues[i])
                {
                    guardMode = i;
                }
            }
            if (ImGui::Combo(
                    "Guard##guard_mode", &guardMode, kGuardModes,
                    IM_ARRAYSIZE(kGuardModes)))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_SET_GUARD_MODE, 0,
                    kGuardModeValues[guardMode]);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "\"All\" reads the attack that is actually out: it stands up\n"
                    "for a grounded overhead instead of eating it, blocks in the\n"
                    "air without jumping, and blocks properly again the moment it\n"
                    "lands. \"Adaptive\" is cheaper and only guesses stand-or-crouch\n"
                    "from whether the attacker is grounded or airborne, so an\n"
                    "overhead beats it. Stand and Crouch simply hold back or\n"
                    "down-back the whole time; Air holds up-back, so the dummy\n"
                    "jumps backwards on its own. None of them turns on the CPU\n"
                    "opponent, so nothing else about the dummy behaviour changes.\n\n"
                    "While you record a slot you are moving the dummy yourself,\n"
                    "so for the length of the recording this applies to your own\n"
                    "fighter instead - the one you are recording against.");
            }

            {
                const bool timingReady =
                    ready && TrainingHasGuardTimingStatus(status);
                static const char *kTimingModes[] = {
                    "Always", "After being hit", "Until first block",
                };
                static const char *kTimingWindows[] = {
                    "30", "60", "90", "120", "180", "240", "300",
                };
                static const int kTimingWindowValues[] = {
                    30, 60, 90, 120, 180, 240, 300,
                };
                ImGui::BeginDisabled(!timingReady);
                int timing = timingReady
                    ? static_cast<int>(status.guardTimingMode) : 0;
                if (timing < 0 || timing > 2) timing = 0;
                DWORD window = timingReady ? status.guardTimingWindow : 60u;
                if (window < 1u || window > 600u) window = 60u;
                if (ImGui::Combo("Guard timing##guard_timing", &timing,
                                 kTimingModes, IM_ARRAYSIZE(kTimingModes)))
                {
                    EnqueueTrainingCommand(
                        status, QOH99_TRAINING_COMMAND_SET_GUARD_TIMING, 0,
                        static_cast<DWORD>(timing), window);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "WHEN the guard mode is armed. \"After being hit\": "
                        "the dummy takes the\nfirst hit, then blocks until "
                        "it has stood free for the window; stun and\n"
                        "knockdown do not spend it. \"Until first block\": "
                        "the dummy blocks ONE\nhit and stops - if the next "
                        "attack still lands, the string had a gap.\nIt "
                        "re-arms after standing free for the window.");
                }
                if (timing != 0)
                {
                    int windowIndex = 1;
                    for (int i = 0; i < IM_ARRAYSIZE(kTimingWindowValues);
                         ++i)
                    {
                        if (kTimingWindowValues[i]
                            == static_cast<int>(window))
                        {
                            windowIndex = i;
                            break;
                        }
                    }
                    if (ImGui::Combo("Window (frames)##guard_window",
                                     &windowIndex, kTimingWindows,
                                     IM_ARRAYSIZE(kTimingWindows)))
                    {
                        EnqueueTrainingCommand(
                            status,
                            QOH99_TRAINING_COMMAND_SET_GUARD_TIMING, 0,
                            static_cast<DWORD>(timing),
                            static_cast<DWORD>(
                                kTimingWindowValues[windowIndex]));
                    }
                }
                ImGui::EndDisabled();
            }

            if (status.guardMode == QOH99_TRAINING_GUARD_AUTO_HEURISTIC)
            {
                TrainingTextColoredWrapped(
                    ImVec4(1.0f, 0.72f, 0.30f, 1.0f),
                    "\"Auto (basic)\" is a rough rule, not a real blocker.");
                TrainingTextDisabledWrapped(
                    "Guards low against a grounded attacker, high against an "
                    "airborne one. It does not read the attack itself.");
                if (ImGui::TreeNode("What it gets wrong"))
                {
                    TrainingBulletTextWrapped(
                        "Grounded overheads - the common miss. Use "
                        "\"Auto (reads the move)\" for those.");
                    TrainingBulletTextWrapped("Throws. Nothing blocks those.");
                    TrainingBulletTextWrapped(
                        "Projectiles and helpers - it only watches the "
                        "opponent.");
                    TrainingBulletTextWrapped(
                        "Anything switching ground/air on the contact frame.");
                    TrainingTextDisabledWrapped(
                        "It only holds a direction, so the worst case is the "
                        "dummy getting hit.");
                    ImGui::TreePop();
                }
            }

            if (status.guardMode == QOH99_TRAINING_GUARD_ALWAYS_AIR)
            {
                TrainingTextColoredWrapped(
                    ImVec4(0.55f, 0.80f, 1.0f, 1.0f),
                    "\"Air guard\" makes the dummy jump. That is intentional.");
                TrainingTextDisabledWrapped(
                    "It holds up-back, so the dummy jumps away and blocks the "
                    "whole time - the only way to see the game's air block.");
                if (ImGui::TreeNode("Why it jumps"))
                {
                    TrainingTextDisabledWrapped(
                        "Up-back is both a jump request and a legal guard on "
                        "the ground and in the air, so it keeps blocking on "
                        "the way up, at the top, coming down and on landing.");
                    TrainingTextDisabledWrapped(
                        "It retreats while it does this - the direction that "
                        "blocks is the one pointing away. Try "
                        "\"Auto (reads the move)\" if you want air blocking "
                        "without the jumping.");
                    ImGui::TreePop();
                }
            }

            bool guardDuringPlayback = status.guardDuringPlayback != 0;
            if (ImGui::Checkbox(
                    "Keep guarding during playback", &guardDuringPlayback))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_SET_GUARD_DURING_PLAYBACK, 0,
                    guardDuringPlayback ? 1u : 0u);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Guard and stance stay on while a recording plays back, but only\n"
                    "on directions the recording itself leaves free - so the dummy\n"
                    "answers anything the recording does not, and a recorded motion or\n"
                    "jump is never overwritten. Recovery is the exception: it needs\n"
                    "both directions to itself, so it stands down entirely during\n"
                    "playback rather than half-performing a tap that matches nothing.");
            }

            const bool postureReady = ready && TrainingHasExtendedStatus(status);
            ImGui::Spacing();
            ImGui::SeparatorText(qi18n::tr("Stance"));
            ImGui::BeginDisabled(!postureReady);

            static const char *kStances[] = { "Stand", "Crouch", "Jump" };
            int stance = static_cast<int>(status.dummyStance);
            if (stance < QOH99_TRAINING_STANCE_STAND
                || stance > QOH99_TRAINING_STANCE_JUMP)
            {
                stance = QOH99_TRAINING_STANCE_STAND;
            }
            if (ImGui::Combo(
                    "Stance##stance", &stance, kStances, IM_ARRAYSIZE(kStances)))
            {
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_SET_STANCE, 0,
                    static_cast<DWORD>(stance));
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "The dummy simply holds down, or holds up, the whole time - it\n"
                    "is only held input, nothing is written into the character.\n"
                    "A guard setting other than \"No guard\" overrides this, because\n"
                    "guarding decides both directions for itself. If you want a\n"
                    "guarding dummy in the air, use the \"Air guard\" guard setting;\n"
                    "\"Jump\" here is for a dummy that is NOT guarding.");
            }

            ImGui::EndDisabled();

            ImGui::EndDisabled();
            if (!ready)
            {
                DrawTrainingUnavailableReason(
                    status, QOH99_TRAINING_CAP_DUMMY_GUARD);
            }
            else if (!postureReady)
            {
                DrawTrainingStaleDllReason(
                        status, QOH99_TRAINING_STATUS_V1_COMBAT_TIER);
            }

            if (postureReady && status.dummyTech == QOH99_TRAINING_TECH_HOLD_GUARD)
            {
                TrainingTextColoredWrapped(
                    ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
                    "The old \"auto-tech: hold back\" setting is still on in the DLL.");
                ImGui::TextWrapped(
                    "It never recovered anything - it only made the dummy hold back, "
                    "which reads as blocking. It has been retired; pick a setting on "
                    "the Recovery sub-tab instead.");
            }

            ImGui::Spacing();
            ImGui::SeparatorText(qi18n::tr("Faultless Defence"));
            DrawTrainingFaultlessControls(status);

            ImGui::Spacing();
            ImGui::SeparatorText(qi18n::tr("Blocking"));
            DrawTrainingParryControls(status);

            ImGui::Spacing();
            TrainingTextDisabledWrapped(
                "Guard and stance are only held directions on the dummy. The CPU never "
                "takes over, and neither is ever written into a recording, so anything "
                "you record still plays back exactly as you recorded it.");

            TrainingTextDisabledWrapped(
                "The dummy keeps guarding while the menu is open; only your own "
                "controls are held back, so a block you set up stays set up.");
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(qi18n::tr("Recovery")))
    {
        DrawDummyRecoverySection(status, ready);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void DrawMacroSlotSelector(const Qoh99TrainingStatusV1 &status, bool ready)
{
    const DWORD selected =
        std::min<DWORD>(status.selectedMacroSlot, QOH99_TRAINING_SLOT_COUNT - 1);
    ImGui::BeginDisabled(!ready);
    for (DWORD slot = 0; slot < QOH99_TRAINING_SLOT_COUNT; ++slot)
    {
        if (slot != 0)
        {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(slot));
        const bool valid = (status.validMacroSlots & (1u << slot)) != 0;
        if (slot == selected)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImVec4(0.24f, 0.52f, 0.80f, 1.0f));
        }
        char label[16] = {};
        std::snprintf(
            label, sizeof(label), "%lu%s",
            static_cast<unsigned long>(slot + 1), valid ? " *" : "");
        if (ImGui::Button(label, ImVec2(52.0f, 0.0f)))
        {
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SELECT_MACRO_SLOT, slot, 0);
        }
        if (slot == selected)
        {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
}

void DrawRecordingTab(const Qoh99TrainingStatusV1 &status)
{
    const bool ready =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_MACROS);
    ImGui::TextUnformatted("Slot");
    DrawMacroSlotSelector(status, ready);
    ImGui::TextDisabled("* = this slot has a recording");
    if (!ready)
    {
        DrawTrainingUnavailableReason(status, QOH99_TRAINING_CAP_MACROS);
    }

    const DWORD slot =
        std::min<DWORD>(status.selectedMacroSlot, QOH99_TRAINING_SLOT_COUNT - 1);
    const bool recording =
        status.macroState == QOH99_TRAINING_MACRO_RECORD_ARMED
        || status.macroState == QOH99_TRAINING_MACRO_RECORDING
        || status.macroState == QOH99_TRAINING_MACRO_RECORD_STOPPING;
    const bool playing =
        status.macroState == QOH99_TRAINING_MACRO_PLAY_ARMED
        || status.macroState == QOH99_TRAINING_MACRO_PLAYING
        || status.macroState == QOH99_TRAINING_MACRO_PLAY_STOPPING;
    const bool preRecording =
        status.macroState == QOH99_TRAINING_MACRO_PRE_RECORD;

    ImGui::BeginDisabled(!ready);
    if (ImGui::Button(recording ? "Stop Recording" : "Record"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_TOGGLE_RECORD, slot, 0);
    }
    ImGui::SameLine();
    const bool slotHasMacro =
        (status.validMacroSlots & (1u << slot)) != 0;
    const bool randomPlayback =
        status.playbackMode == QOH99_TRAINING_PLAY_RANDOM
        || status.playbackMode == QOH99_TRAINING_PLAY_RANDOM_LOOP;
    const bool canPlay = randomPlayback
        ? (status.validMacroSlots & 0xFFu) != 0u
        : slotHasMacro;
    ImGui::BeginDisabled(!playing && !canPlay);
    if (ImGui::Button(playing ? "Stop Playback" : "Play"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_TOGGLE_PLAYBACK, slot, 0);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(status.macroState != QOH99_TRAINING_MACRO_IDLE
                         || !TrainingHasStatusMessages(status));
    if (ImGui::Button(preRecording ? "Positioning..." : "Pre-record"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_BEGIN_PRE_RECORD, 0, 0);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "Move the dummy into position first. Record then starts the\n"
            "recording from there; Play or Stop leave without keeping it.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(status.macroState == QOH99_TRAINING_MACRO_IDLE);
    if (ImGui::Button("Stop"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_STOP_MACRO, slot, 0);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!slotHasMacro);
    if (ImGui::Button("Clear"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_CLEAR_MACRO, slot, 0);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    int playbackMode = static_cast<int>(
        std::min<DWORD>(status.playbackMode,
                        QOH99_TRAINING_PLAY_RANDOM_LOOP));
    ImGui::TextUnformatted("Repeat");
    ImGui::SameLine(150.0f);
    if (ImGui::RadioButton(
            "Once", playbackMode == QOH99_TRAINING_PLAY_ONCE))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_PLAYBACK_MODE, 0,
            QOH99_TRAINING_PLAY_ONCE);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "Loop", playbackMode == QOH99_TRAINING_PLAY_LOOP))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_PLAYBACK_MODE, 0,
            QOH99_TRAINING_PLAY_LOOP);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "Random", playbackMode == QOH99_TRAINING_PLAY_RANDOM))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_PLAYBACK_MODE, 0,
            QOH99_TRAINING_PLAY_RANDOM);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", qi18n::tr(
            "Plays one recorded slot, picked at random."));
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "Random loop", playbackMode == QOH99_TRAINING_PLAY_RANDOM_LOOP))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_PLAYBACK_MODE, 0,
            QOH99_TRAINING_PLAY_RANDOM_LOOP);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", qi18n::tr(
            "Picks a new recorded slot after every run."));
    }

    const DWORD facingPolicy =
        std::min<DWORD>(
            status.facingPolicy, QOH99_TRAINING_FACING_ABSOLUTE);
    ImGui::TextUnformatted("Directions");
    ImGui::SameLine(150.0f);
    if (ImGui::RadioButton(
            "Mirror with side",
            facingPolicy == QOH99_TRAINING_FACING_RELATIVE))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_FACING_POLICY, 0,
            QOH99_TRAINING_FACING_RELATIVE);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Forward stays forward after the dummy crosses up or switches\n"
            "sides. Use this for most combo and punish practice.");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(
            "Exactly as recorded",
            facingPolicy == QOH99_TRAINING_FACING_ABSOLUTE))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_FACING_POLICY, 0,
            QOH99_TRAINING_FACING_ABSOLUTE);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Left stays left and right stays right no matter which side\n"
            "the dummy ends up on.");
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText(qi18n::tr("This slot"));
    ImGui::Text("%s", TrainingMacroStateName(status.macroState));
    ImGui::Text("Length: %lu of %lu frames",
        static_cast<unsigned long>(status.macroFrameCounts[slot]),
        static_cast<unsigned long>(status.macroFrameCapacity));
    const float capacity = status.macroFrameCapacity != 0
        ? static_cast<float>(status.macroFrameCapacity) : 1.0f;
    const float used = (std::min)(
        1.0f, static_cast<float>(status.macroFrameCounts[slot]) / capacity);
    ImGui::ProgressBar(used, ImVec2(-1.0f, 0.0f));
    ImGui::Spacing();
    ImGui::TextWrapped(
        "While you record, your controls drive the dummy and your own "
        "character stands still. Recordings keep only the directions and "
        "buttons, and are lost when you quit the game.");
}

void DrawStateSlotSelector(const Qoh99TrainingStatusV1 &status, bool ready)
{
    const DWORD selected =
        std::min<DWORD>(status.selectedStateSlot, QOH99_TRAINING_SLOT_COUNT - 1);
    ImGui::BeginDisabled(!ready);
    for (DWORD slot = 0; slot < QOH99_TRAINING_SLOT_COUNT; ++slot)
    {
        if (slot != 0)
        {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(slot));
        const bool valid = (status.validStateSlots & (1u << slot)) != 0;
        const bool stale = (status.staleStateSlots & (1u << slot)) != 0;
        if (slot == selected)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImVec4(0.24f, 0.52f, 0.80f, 1.0f));
        }
        char label[16] = {};
        std::snprintf(
            label, sizeof(label), "%lu %c",
            static_cast<unsigned long>(slot + 1),
            stale ? 'S' : (valid ? 'V' : '-'));
        if (ImGui::Button(label, ImVec2(52.0f, 0.0f)))
        {
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SELECT_STATE_SLOT, slot, 0);
        }
        if (slot == selected)
        {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("V = saved, S = saved but no longer usable, - = empty");
}

void DrawStateTab(const Qoh99TrainingStatusV1 &status)
{
    const bool ready =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_SAVESTATES);
    ImGui::TextUnformatted("Slot");
    DrawStateSlotSelector(status, ready);
    if (!ready)
    {
        DrawTrainingUnavailableReason(status, QOH99_TRAINING_CAP_SAVESTATES);
    }

    const DWORD slot =
        std::min<DWORD>(status.selectedStateSlot, QOH99_TRAINING_SLOT_COUNT - 1);
    const bool valid = (status.validStateSlots & (1u << slot)) != 0;
    const bool stale = (status.staleStateSlots & (1u << slot)) != 0;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Save"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SAVE_STATE, slot, 0);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid || stale);
    if (ImGui::Button("Load"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_LOAD_STATE, slot, 0);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid && !stale);
    if (ImGui::Button("Clear"))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_CLEAR_STATE, slot, 0);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (stale)
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
            "This slot was saved with a different match or different "
            "characters, so it can't be loaded. Save over it to reuse it.");
    }
    if (status.lastCommandResult
            == QOH99_TRAINING_COMMAND_RESULT_REJECTED_STALE_LAYOUT
        || status.lastCommandResult
            == QOH99_TRAINING_COMMAND_RESULT_SNAPSHOT_FAILED)
    {
        ImGui::TextWrapped("Last save/load didn't work - %s%s%s",
            TrainingCommandResultName(status.lastCommandResult),
            status.resultText[0] ? " - " : "",
            status.resultText);
    }
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Save states live in memory for this session only. They're lost when "
        "you quit, they never touch your disk, and they never leave your PC.");
    TrainingTextDisabledWrapped(
        "Keys 2 and 3 save and load the selected slot without opening this "
        "menu, and 6 / 7 step through the slots.");
}

void DrawSaveStateDetails(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Save states"));
    ImGui::TextWrapped("Rollback layout generation: %lu",
        static_cast<unsigned long>(status.rollbackLayoutGeneration));
    ImGui::TextWrapped("Character/team fingerprint: %08lX%08lX",
        static_cast<unsigned long>(status.characterTeamFingerprintHigh),
        static_cast<unsigned long>(status.characterTeamFingerprintLow));
}

void DrawInputViewerSettings()
{
    ImGui::SeparatorText(qi18n::tr("Input viewer"));

    if (g_presenter.replayTransportStatus != nullptr)
    {
        ImGui::Checkbox("Show replay frame and speed##replay_transport_visible",
                        &g_replayTransportVisible);
    }

    bool visible = g_inputDisplayVisible;
    if (ImGui::Checkbox("Show the input viewer##input_display_visible",
                        &visible))
    {
        g_inputDisplayVisible = visible;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The configured Input viewer control toggles this at any time.\n"
            "It is a display setting only - it never reaches the game, so it\n"
            "cannot affect a replay or a training session.");
    }

    if (g_presenter.inputDisplayApiVersion != QOH99_INPUT_DISPLAY_API_VERSION
        || g_presenter.inputDisplayProvider == nullptr)
    {
        TrainingTextDisabledWrapped(
            "Nothing is sending inputs right now, so there is nothing to\n"
            "show. The viewer only ever appears during replay playback and in\n"
            "training - never in netplay, offline versus or spectating.");
        return;
    }

    Qoh99InputDisplaySnapshotV1 snapshot;
    std::memset(&snapshot, 0, sizeof(snapshot));
    const bool polled = g_presenter.inputDisplayProvider(
                            &snapshot, sizeof(snapshot),
                            g_presenter.inputDisplayUserData) != FALSE;
    if (!polled || !InputDisplaySnapshotRenderable(snapshot))
    {
        TrainingTextDisabledWrapped(
            "Nothing has been drawn yet - that is what an idle session between\n"
            "matches looks like.");
        return;
    }

    ImGui::TextDisabled("Source: %s",
        (snapshot.mode == QOH99_INPUT_DISPLAY_MODE_TRAINING)
            ? "training" : "replay playback");
    if (snapshot.mode == QOH99_INPUT_DISPLAY_MODE_TRAINING)
    {
        TrainingTextDisabledWrapped(
            "Showing the side you are driving: P%lu. It moves to P2 on its own\n"
            "while a recording is running, because that is where your stick is\n"
            "being routed.",
            static_cast<unsigned long>(snapshot.drivenLane + 1u));
    }
    else
    {
        ImGui::TextDisabled("Showing both players, straight from the file.");
    }
}

void DrawInputDetails(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Input"));
    ImGui::TextWrapped("P1: 0x%08lX    facing: %lu    character: %lu",
        static_cast<unsigned long>(status.p1FinalLogicalMask),
        static_cast<unsigned long>(status.p1Facing),
        static_cast<unsigned long>(status.p1Character));
    ImGui::TextWrapped("P2: 0x%08lX    facing: %lu    character: %lu",
        static_cast<unsigned long>(status.p2FinalLogicalMask),
        static_cast<unsigned long>(status.p2Facing),
        static_cast<unsigned long>(status.p2Character));
    ImGui::TextWrapped("Guard permission: 0x%02lX (%s), misses: %lu",
        static_cast<unsigned long>(status.guardPermission & 0xFFu),
        TrainingGuardClassName(status.guardClassification),
        static_cast<unsigned long>(status.guardPredictionMisses));

    if (g_presenter.inputDisplayApiVersion == QOH99_INPUT_DISPLAY_API_VERSION
        && g_presenter.inputDisplayProvider != nullptr)
    {
        Qoh99InputDisplaySnapshotV1 producer;
        std::memset(&producer, 0, sizeof(producer));
        if (g_presenter.inputDisplayProvider(
                &producer, sizeof(producer),
                g_presenter.inputDisplayUserData) != FALSE)
        {
            ImGui::TextWrapped("Producer step %lu%s (session %lu)",
                static_cast<unsigned long>(producer.stepSerialLow),
                (producer.stepSerialHigh != 0) ? " (wrapped)" : "",
                static_cast<unsigned long>(producer.producerGeneration));
        }
    }
}

void DrawViewTab(
    const EfzDx9PresentParams &params,
    const Qoh99TrainingStatusV1 &status,
    bool trainingPresent)
{

    if (!ImGui::BeginTabBar("qoh99_view_tabs"))
    {
        return;
    }
    if (ImGui::BeginTabItem(qi18n::tr("Overlays")))
    {
        {
            if (trainingPresent)
            {
                const bool tierReady = TrainingHasCombatToolsStatus(status);
                const bool featureReady = tierReady && TrainingFeatureReady(
                    status, QOH99_TRAINING_CAP_FRAME_ADVANTAGE);
                bool enabled = tierReady
                    && status.frameAdvantageEnabled != 0;
                ImGui::BeginDisabled(!featureReady);
                if (ImGui::Checkbox("Show frame advantage", &enabled))
                {
                    EnqueueTrainingCommand(
                        status,
                        QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED,
                        0, enabled ? 1u : 0u);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(
                        "Shows who recovers first after a hit or block.\n"
                        "Positive numbers favour the attacker.");
                }
                ImGui::EndDisabled();
                if (!tierReady)
                {
                    DrawTrainingStaleDllReason(
                        status, QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER);
                }
                else if (!featureReady)
                {
                    DrawTrainingUnavailableReason(
                        status, QOH99_TRAINING_CAP_FRAME_ADVANTAGE);
                }

                const bool hitboxReady =
                    TrainingFeatureReady(status, QOH99_TRAINING_CAP_HITBOXES);
                const DWORD hitboxMode = status.hitboxRequested;
                const bool hitboxVanilla = hitboxMode == 2u;
                bool hitboxes = hitboxMode != 0u;
                ImGui::BeginDisabled(!hitboxReady);
                if (ImGui::Checkbox("Show hitboxes", &hitboxes))
                {
                    EnqueueTrainingCommand(
                        status, QOH99_TRAINING_COMMAND_SET_HITBOX_DISPLAY, 0,
                        hitboxes ? (hitboxVanilla ? 2u : 1u) : 0u);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Draws attack, hurt and collision boxes.\n"
                        "Also toggled by the configured training control.");
                }

                if (hitboxes && !status.hitboxEffective)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.55f, 0.30f, 1.0f), "(not showing yet)");
                }

                int hitboxStyle = hitboxVanilla ? 1 : 0;
                ImGui::BeginDisabled(!hitboxes);
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::Combo("Hitbox style", &hitboxStyle,
                                 "Enhanced\0Game's own\0\0"))
                {
                    EnqueueTrainingCommand(
                        status, QOH99_TRAINING_COMMAND_SET_HITBOX_DISPLAY, 0,
                        hitboxStyle == 1 ? 2u : 1u);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Enhanced: adds the clash boxes the game never draws, "
                        "shows projectile hurtboxes, and dims an attack box "
                        "that cannot connect yet.\n"
                        "Game's own: the original F7 display, kept so the two "
                        "can be compared.");
                }

                if (hitboxStyle == 0 && ImGui::TreeNode("Colour key"))
                {
                    struct HitboxLegendRow { ImU32 color; const char *label; };
                    static const HitboxLegendRow kLegend[] = {
                        { IM_COL32(0xFF, 0xFF, 0xFF, 0xFF),
                          "White - collision / pushbox" },
                        { IM_COL32(0x00, 0xDC, 0x00, 0xFF),
                          "Green - hurtbox, can be hit" },
                        { IM_COL32(0xFF, 0x28, 0x28, 0xFF),
                          "Red - active hitbox" },
                        { IM_COL32(0x80, 0x30, 0x30, 0xFF),
                          "Dark red, unfilled - attack box that cannot "
                          "connect yet" },
                        { IM_COL32(0xFF, 0xFF, 0x28, 0xFF),
                          "Yellow - clash box; this is what trades, and the "
                          "game never draws it" },
                        { IM_COL32(0x28, 0xC8, 0xFF, 0xFF),
                          "Blue - attack-vs-attack reversal" },
                        { IM_COL32(0xFF, 0x28, 0xFF, 0xFF),
                          "Magenta - throw / capture" },
                    };
                    for (const HitboxLegendRow &row : kLegend)
                    {
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        const float h = ImGui::GetTextLineHeight();
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(p.x, p.y + 2.0f),
                            ImVec2(p.x + h, p.y + h - 1.0f),
                            (row.color & 0x00FFFFFFu) | (63u << 24));
                        ImGui::GetWindowDrawList()->AddRect(
                            ImVec2(p.x, p.y + 2.0f),
                            ImVec2(p.x + h, p.y + h - 1.0f), row.color);
                        ImGui::Dummy(ImVec2(h + 6.0f, h));
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", row.label);
                    }
                    TrainingTextDisabledWrapped(
                        "A yellow box overlapping the opponent with no red box "
                        "up is a move that can trade before it can hit. CLASH "
                        "marks the frame one actually resolves.");
                    ImGui::TreePop();
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                if (!hitboxReady)
                {
                    DrawTrainingUnavailableReason(
                        status, QOH99_TRAINING_CAP_HITBOXES);
                }
                TrainingTextDisabledWrapped(
                    "Configured training controls toggle hitboxes, refill "
                    "health, EX meter and guard gauge, and open this menu. "
                    "The refills are on Quick. Native developer keys "
                    "are held back while training is running.");
            }

            ImGui::Spacing();
            DrawInputViewerSettings();
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(qi18n::tr("Game HUD")))
    {
        DrawHudLayoutTab(params);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(qi18n::tr("Window")))
    {
        DrawDisplayTab(params);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void DrawDummyDetails(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Dummy"));
    ImGui::TextWrapped("Classification: %s",
        TrainingGuardClassName(status.guardClassification));
    ImGui::TextWrapped(
        "Permission: 0x%02lX  [stand %s, crouch %s, air %s]",
        static_cast<unsigned long>(status.guardPermission & 0xFFu),
        (status.guardPermission & QOH99_TRAINING_GUARD_PERMISSION_STAND)
            ? "yes" : "no",
        (status.guardPermission & QOH99_TRAINING_GUARD_PERMISSION_CROUCH)
            ? "yes" : "no",
        (status.guardPermission & QOH99_TRAINING_GUARD_PERMISSION_AIR)
            ? "yes" : "no");
    ImGui::TextWrapped("P2 logical input: 0x%08lX    facing: %lu",
        static_cast<unsigned long>(status.p2FinalLogicalMask),
        static_cast<unsigned long>(status.p2Facing));
    ImGui::TextWrapped("Prediction misses: %lu",
        static_cast<unsigned long>(status.guardPredictionMisses));
    if (TrainingHasRecoveryStatus(status))
    {
        ImGui::TextWrapped("Posture: P1 %s   P2 %s",
            TrainingPostureClassName(status.p1PostureClass),
            TrainingPostureClassName(status.p2PostureClass));
        ImGui::TextWrapped(
            "Auto (basic): %lu posture/height disagreements, "
            "%lu ambiguous attackers, %lu frames with no guard allowed",
            static_cast<unsigned long>(
                status.guardHeuristicPostureDivergences),
            static_cast<unsigned long>(
                status.guardHeuristicAmbiguousAttackers),
            static_cast<unsigned long>(
                status.guardHeuristicUnguardablePostures));
    }
    else
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_RECOVERY_TIER);
    }
}

void DrawTrainingStatusSummary(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Training status"));
    if (status.available != 0)
    {
        ImGui::TextColored(
            ImVec4(0.35f, 0.90f, 0.48f, 1.0f), "Training controls are ready.");
    }
    else
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
            "Training controls are unavailable on this screen.");
    }

    ImGui::Text("Current screen: %s",
        status.inMatchReady != 0 ? "Training match" : "Character select or loading");

    const bool bothPlayerControlled = status.comMask == 0
        && status.p1ComPrimary == 0 && status.p1ComMirror == 0
        && status.p2ComPrimary == 0 && status.p2ComMirror == 0;
    DrawEffectiveInvariant(
        "Both sides player-controlled", bothPlayerControlled ? 1u : 0u);
    DrawRequestedEffective(
        "Rounds never end", status.endlessRoundRequested,
        status.endlessRoundEffective);
    DrawRequestedEffective(
        "Timer never runs out", status.infiniteTimeRequested,
        status.infiniteTimeEffective);

    ImGui::SeparatorText(qi18n::tr("Pause state"));
    const bool userHeld = (status.holdReasons & QOH99_TRAINING_HOLD_USER) != 0;
    const bool menuHeld = (status.holdReasons & QOH99_TRAINING_HOLD_MENU) != 0;
    if (userHeld)
    {
        ImGui::TextUnformatted("Paused by you.");
    }
    else if (menuHeld)
    {
        ImGui::TextUnformatted("Held while this menu is open.");
    }
    else
    {
        ImGui::TextUnformatted("Running.");
    }
    TrainingTextDisabledWrapped(
        "Use Step 1 frame at the top-right to advance once without closing the menu.");

    ImGui::SeparatorText(qi18n::tr("Last menu change"));
    if (status.lastAcknowledgedCommandId == 0)
    {
        ImGui::TextDisabled("No changes have been applied yet.");
    }
    else if (status.lastCommandResult == QOH99_TRAINING_COMMAND_RESULT_APPLIED)
    {
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.48f, 1.0f), "Applied.");
    }
    else
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
            "Not applied - %s.",
            TrainingCommandResultName(status.lastCommandResult));
    }
}

void DrawTrainingDamageSummary(const Qoh99TrainingStatusV1 &status)
{
    if (!TrainingHasExtendedStatus(status))
    {
        ImGui::TextDisabled("Damage information is unavailable right now.");
        return;
    }

    ImGui::SeparatorText(qi18n::tr("Last hit"));
    if (status.lastHitVictimSlot > 3)
    {
        ImGui::TextDisabled("Nothing has hit yet.");
    }
    else
    {
        ImGui::Text("Damage: %d",
            static_cast<int>(static_cast<int32_t>(status.lastHitDamage)));
        ImGui::Text("Life lost: %d",
            static_cast<int>(static_cast<int32_t>(status.lastHitPendingDelta)));
        ImGui::Text("Applied: %s", status.lastHitAdmitted != 0 ? "Yes" : "No");
    }

    ImGui::SeparatorText(qi18n::tr("Combos"));
    if (status.combatReadoutValid == 0)
    {
        ImGui::TextDisabled("Combo information is unavailable right now.");
        return;
    }
    DrawTrainingReadoutHeader("Current combo taken");
    DrawTrainingReadoutPair(
        "Hits", static_cast<unsigned long>(status.p1ComboHitsTaken),
        static_cast<unsigned long>(status.p2ComboHitsTaken));
    DrawTrainingReadoutPair(
        "Damage", static_cast<unsigned long>(status.p1ComboDamageTaken),
        static_cast<unsigned long>(status.p2ComboDamageTaken));

    char p1Text[48] = {};
    char p2Text[48] = {};
    std::snprintf(p1Text, sizeof(p1Text), "%lu hits, %lu damage",
        static_cast<unsigned long>(status.p1MaxComboHitsDealt),
        static_cast<unsigned long>(status.p1MaxComboDamageDealt));
    std::snprintf(p2Text, sizeof(p2Text), "%lu hits, %lu damage",
        static_cast<unsigned long>(status.p2MaxComboHitsDealt),
        static_cast<unsigned long>(status.p2MaxComboDamageDealt));
    DrawTrainingReadoutRow("Best combo dealt", p1Text, p2Text);
}

void DrawTrainingCharacterSummary(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Live character values"));
    if (TrainingHasExtendedStatus(status) && status.combatReadoutValid != 0)
    {
        DrawTrainingReadoutHeader("Resource");
        DrawTrainingReadoutPair(
            "Health", static_cast<unsigned long>(status.p1EffectiveLife),
            static_cast<unsigned long>(status.p2EffectiveLife));
        DrawTrainingReadoutPair(
            "Guard", static_cast<unsigned long>(status.p1GuardGauge),
            static_cast<unsigned long>(status.p2GuardGauge));

        char p1Meter[48] = {};
        char p2Meter[48] = {};
        std::snprintf(p1Meter, sizeof(p1Meter), "%lu stock%s + %lu / 53",
            static_cast<unsigned long>(status.p1MeterStocks),
            status.p1MeterStocks == 1 ? "" : "s",
            static_cast<unsigned long>(status.p1MeterFraction));
        std::snprintf(p2Meter, sizeof(p2Meter), "%lu stock%s + %lu / 53",
            static_cast<unsigned long>(status.p2MeterStocks),
            status.p2MeterStocks == 1 ? "" : "s",
            static_cast<unsigned long>(status.p2MeterFraction));
        DrawTrainingReadoutRow("EX meter", p1Meter, p2Meter);
    }
    else
    {
        ImGui::TextDisabled("Resource values are unavailable right now.");
    }

    ImGui::SeparatorText(qi18n::tr("State and frame"));
    if (!TrainingHasFighterStatus(status) || status.fighterReadoutValid == 0)
    {
        ImGui::TextDisabled("State information is unavailable right now.");
        return;
    }
    DrawTrainingReadoutHeader("Value");
    DrawTrainingReadoutPair(
        "Current state", static_cast<unsigned long>(status.p1ActionState),
        static_cast<unsigned long>(status.p2ActionState));
    DrawTrainingReadoutPair(
        "State frame", static_cast<unsigned long>(status.p1ActionFrame),
        static_cast<unsigned long>(status.p2ActionFrame));
}

void DrawTrainingRestoreSummary(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Automatic restore"));
    if (!TrainingHasRecoveryStatus(status) || status.resourcePolicyValid == 0)
    {
        ImGui::TextDisabled("Restore status is unavailable right now.");
        return;
    }

    ImGui::Text("Health: %s",
        status.healthTrigger == QOH99_TRAINING_TRIGGER_OFF ? "Off" : "On");
    ImGui::Text("EX meter: %s",
        status.exTrigger == QOH99_TRAINING_TRIGGER_OFF ? "Off" : "On");
    ImGui::Text("Guard: %s",
        status.guardGaugeTrigger == QOH99_TRAINING_TRIGGER_OFF ? "Off" : "On");

    const bool anyRestore = status.healthTrigger != QOH99_TRAINING_TRIGGER_OFF
        || status.exTrigger != QOH99_TRAINING_TRIGGER_OFF
        || status.guardGaugeTrigger != QOH99_TRAINING_TRIGGER_OFF;
    if (!anyRestore)
    {
        ImGui::TextDisabled("Nothing is set to restore automatically.");
        return;
    }

    ImGui::SeparatorText(qi18n::tr("Current check"));
    if (status.bothFightersNeutral != 0)
    {
        ImGui::TextColored(
            ImVec4(0.35f, 0.90f, 0.48f, 1.0f),
            "Both characters are settled.");
        ImGui::Text("Settled frames: %lu",
            static_cast<unsigned long>(status.neutralHoldSteps));
    }
    else
    {
        ImGui::TextUnformatted(
            "Waiting for both characters to become settled.");
    }
    TrainingTextDisabledWrapped(
        "Idle, walking and crouching count as settled for automatic restore.");
}

void DrawTrainingRecoverySummary(const Qoh99TrainingStatusV1 &status)
{
    if (!TrainingHasRecoveryStatus(status))
    {
        ImGui::TextDisabled("Recovery status is unavailable right now.");
        return;
    }

    if (TrainingHasRecoveryDiagnosticStatus(status)
        && !TrainingRecoveryEvaluatorAllowsFiring(status.recoveryEvaluatorReason))
    {
        TrainingTextColoredWrapped(
            ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
            "Recovery controls are unavailable for this game build.");
    }

    ImGui::SeparatorText(qi18n::tr("Settings"));
    ImGui::Text("Air: %s", TrainingRecoveryModeName(status.recoveryModeAir));
    ImGui::Text("Ground: %s", TrainingRecoveryModeName(status.recoveryModeGround));
    ImGui::Text("Wall: %s", TrainingRecoveryModeName(status.recoveryModeWall));
    ImGui::Text("Knockdown: %s",
        TrainingRecoveryModeName(status.recoveryModeTerminal));
    ImGui::SeparatorText(qi18n::tr("Current state"));
    if (status.recoveryReadoutValid != 0)
    {
        ImGui::Text("Now: %s", TrainingRecoveryStateName(status.recoveryState));
        if (status.recoveryState != QOH99_TRAINING_RECOVERY_STATE_IDLE)
        {
            ImGui::Text("Attempt: %s",
                TrainingRecoveryBranchName(status.recoveryBranch));
        }
    }
    else
    {
        ImGui::TextDisabled("Waiting for a live training match.");
    }
    ImGui::Text("Last result: %s",
        TrainingRecoveryVerdictName(status.recoveryLastVerdict));
    ImGui::Text("Attempts: %lu   Recovered: %lu   Missed: %lu",
        static_cast<unsigned long>(status.recoveryAttempts),
        static_cast<unsigned long>(status.recoverySucceeded),
        static_cast<unsigned long>(status.recoveryMissed));
}

void DrawOverlayStatusSummary(const EfzDx9PresentParams &params)
{
    HudLayoutEnsureLoaded();
    ImGui::SeparatorText(qi18n::tr("Overlay status"));
    ImGui::TextColored(
        ImVec4(0.35f, 0.90f, 0.48f, 1.0f), "Overlay rendering is ready.");
    ImGui::Text("Game image: %d x %d", params.width, params.height);
    ImGui::Text("Menu scale: %d%%",
        g_overlayUiSettings.menuFontScalePct);
    ImGui::Text("Nickname scale: %d%%", g_hudLayout.fontScalePct);
    ImGui::Text("Cyrillic names: %s",
        g_overlayFontStatus.cyrillic ? "Ready" : "Font unavailable");
    ImGui::Text("Japanese names: %s",
        g_overlayFontStatus.japanese ? "Ready" : "Font unavailable");
    TrainingTextDisabledWrapped(
        "Display and text-size options are under View.");
}

void DrawNativeHudControls(const Qoh99TrainingStatusV1 &status)
{
    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("Native game HUD"));
    if (!TrainingHasNativeHudStatus(status))
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER);
    }
    else if (status.nativeHudOwner == 0u)
    {
        TrainingTextDisabledWrapped(
            "These controls exist only in Training and local Replay "
            "playback. Ordinary VS, VS CPU, Free Play, netplay and spectating "
            "are never affected by them.");
    }
    else
    {
        TrainingTextDisabledWrapped(
            "Hides QOH's own match HUD. Only whole components can be hidden: "
            "each one is a single native call, and splitting a call's layers "
            "needs evidence this build does not have yet. Anything that "
            "cannot be hidden stays on screen and is listed below.");

        struct HudToggle
        {
            const char *label;
            const char *tip;
            int maskIndex;
            DWORD bits;
        };
        static const HudToggle kToggles[] = {
            { "Elapsed time (bottom)",
              "The bottom-centre TIME clock.", 0,
              QOH99_TRAINING_HUD_GLOBAL_BOTTOM_ELAPSED_TIME },
            { "Round timer (top)",
              "The round clock at the top of the screen. A different element "
              "from the bottom-centre TIME readout.", 0,
              QOH99_TRAINING_HUD_GLOBAL_TOP_ROUND_TIMER },
            { "FPS counter",
              "QOH's own FPS readout. It shares one native emitter with the "
              "top round timer and is told apart by its descriptor table.", 0,
              QOH99_TRAINING_HUD_GLOBAL_FPS },
            { "P1 score", "The 1P SCORE readout.", 1,
              QOH99_TRAINING_HUD_PLAYER_SCORE },
            { "P2 score", "The 2P SCORE readout.", 2,
              QOH99_TRAINING_HUD_PLAYER_SCORE },
            { "P1 portrait and life",
              "Portrait and all three life layers ride one native call, so "
              "they go together.", 1,
              QOH99_TRAINING_HUD_PLAYER_PORTRAIT_LIFE },
            { "P2 portrait and life",
              "Portrait and all three life layers ride one native call, so "
              "they go together.", 2,
              QOH99_TRAINING_HUD_PLAYER_PORTRAIT_LIFE },
            { "P1 name", "The character name.", 1,
              QOH99_TRAINING_HUD_PLAYER_NAME },
            { "P2 name", "The character name.", 2,
              QOH99_TRAINING_HUD_PLAYER_NAME },
            { "P1 round wins", "The round-win markers.", 1,
              QOH99_TRAINING_HUD_PLAYER_ROUND_WINS },
            { "P2 round wins", "The round-win markers.", 2,
              QOH99_TRAINING_HUD_PLAYER_ROUND_WINS },
            { "P1 EX gauge (POWER)",
              "The 1P POWER bar, stocks and spend animation. One native call, "
              "so they go together.", 1,
              QOH99_TRAINING_HUD_PLAYER_EX },
            { "P2 EX gauge (POWER)",
              "The 2P POWER bar, stocks and spend animation. One native call, "
              "so they go together.", 2,
              QOH99_TRAINING_HUD_PLAYER_EX },
            { "P1 combo readout",
              "The 1P hit count, damage and reaction label.", 1,
              QOH99_TRAINING_HUD_PLAYER_COMBO },
            { "P2 combo readout",
              "The 2P hit count, damage and reaction label.", 2,
              QOH99_TRAINING_HUD_PLAYER_COMBO },
        };

        const DWORD masks[3] = { status.nativeHudRequestedGlobalMask,
                                 status.nativeHudRequestedP1Mask,
                                 status.nativeHudRequestedP2Mask };
        const DWORD stillDrawn[3] = { status.nativeHudUnavailableGlobalMask,
                                      status.nativeHudUnavailableP1Mask,
                                      status.nativeHudUnavailableP2Mask };
        for (const HudToggle &toggle : kToggles)
        {
            ImGui::PushID(toggle.label);
            bool visible = (masks[toggle.maskIndex] & toggle.bits) != 0u;
            if (ImGui::Checkbox(toggle.label, &visible))
            {
                const DWORD next = visible
                    ? (masks[toggle.maskIndex] | toggle.bits)
                    : (masks[toggle.maskIndex] & ~toggle.bits);
                EnqueueTrainingCommand(
                    status, QOH99_TRAINING_COMMAND_SET_NATIVE_HUD_MASK,
                    static_cast<DWORD>(toggle.maskIndex), next);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", toggle.tip);
            if ((stillDrawn[toggle.maskIndex] & toggle.bits) != 0u)
            {
                ImGui::SameLine();
                ImGui::TextColored(kIdleTestDivergedColor, "(still drawn)");
            }
            ImGui::PopID();
        }

        ImGui::PushID("hud_guard");
        bool guardVisible =
            ((status.nativeHudRequestedP1Mask
              | status.nativeHudRequestedP2Mask)
             & QOH99_TRAINING_HUD_PLAYER_GUARD) != 0u;
        if (ImGui::Checkbox("Guard gauges (both sides)", &guardVisible))
        {
            const DWORD p1 = guardVisible
                ? (status.nativeHudRequestedP1Mask
                   | QOH99_TRAINING_HUD_PLAYER_GUARD)
                : (status.nativeHudRequestedP1Mask
                   & ~QOH99_TRAINING_HUD_PLAYER_GUARD);
            const DWORD p2 = guardVisible
                ? (status.nativeHudRequestedP2Mask
                   | QOH99_TRAINING_HUD_PLAYER_GUARD)
                : (status.nativeHudRequestedP2Mask
                   & ~QOH99_TRAINING_HUD_PLAYER_GUARD);
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_NATIVE_HUD_MASK, 1u, p1);
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_NATIVE_HUD_MASK, 2u, p2);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "One native call draws both gauges, so they cannot be hidden "
                "separately.");
        }
        ImGui::PopID();

        const DWORD denied = status.nativeHudUnavailableGlobalMask
            | status.nativeHudUnavailableP1Mask
            | status.nativeHudUnavailableP2Mask;
        if (denied != 0u)
        {
            TrainingTextColoredWrapped(kIdleTestDivergedColor,
                "Some hidden elements are still drawn: this build has not "
                "qualified them, so they fail visible rather than guess.");
        }
        TrainingTextDisabledWrapped(
            "Not controllable yet: the ROUND/READY/GO/K.O. announcements, the "
            "pause overlay, and splitting any component into its own layers "
            "(portrait from life, EX stocks from the fraction bar, and so "
            "on). Those need evidence this build does not have yet.");
        TrainingTextDisabledWrapped(
            "Hiding the EX gauge or the combo readout does not stop the game "
            "tracking meter or combos -- those producers advance real state, "
            "so the call still runs and only its drawing is dropped.");
    }

}

/* Each fighter slot owns a 256-byte table of four 16-colour variants; the selected one starts at entry 16 * paletteIndex. Sprite pixels are 4-bit indices. */
struct PaletteSlotInfo
{
    const char *label;
    uintptr_t table;      /* 4 variants * 16 entries * 4 bytes (B,G,R,X) */
    uintptr_t indexByte;  /* holds 16 * paletteIndex */
};

const PaletteSlotInfo kPaletteSlots[] = {
    {"P1", 9048032u, 0x623EAFu},   /* 0x8A0FE0 */
    {"P2", 9050080u, 0x62692Fu},   /* 0x8A17E0 */
    {"P3", 9049056u, 0x4ECBEFu},   /* 0x8A13E0 */
    {"P4", 9051200u, 0x617ACFu},   /* 0x8A1C40 */
};

constexpr size_t kPaletteTableBytes = 256;
constexpr int kPaletteVariants = 4;
constexpr int kPaletteColors = 16;

bool PaletteRangeWritable(uintptr_t address, size_t bytes)
{
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void *>(address), &info,
                     sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.State != MEM_COMMIT) return false;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
        | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & writable) == 0) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    return address + bytes <= base + info.RegionSize;
}

/* Per-player 256-entry DWORD colour LUT built by sub_44E230: P1 0x8A3340, P2 0x8A2B40, P3 0x8A2F40, P4 0x8A4F40. */
typedef int(__fastcall *Qoh99PaletteToLutFn)(int src, int dest, int count,
                                             int mode);
const Qoh99PaletteToLutFn kQoh99PaletteToLut =
    reinterpret_cast<Qoh99PaletteToLutFn>(0x0044E230);

const uintptr_t kQoh99PaletteModeFlag = 0x6236B0u;

const uintptr_t kPaletteLut[4] = {
    0x8A3340u, 0x8A2B40u, 0x8A2F40u, 0x8A4F40u,
};

void PaletteRebuildRemap();

unsigned g_paletteGeneration = 0;

void PalettePublishSlot(int slotIndex)
{
    ++g_paletteGeneration;
    if (slotIndex < 0 || slotIndex >= 4) return;
    const PaletteSlotInfo &slot = kPaletteSlots[slotIndex];
    if (!PaletteRangeWritable(slot.table, kPaletteTableBytes)
        || !PaletteRangeWritable(kPaletteLut[slotIndex], 256u * 4u)
        || !PaletteRangeWritable(kQoh99PaletteModeFlag, 4))
    {
        return;
    }
    const int mode =
        *reinterpret_cast<const int *>(kQoh99PaletteModeFlag);
    kQoh99PaletteToLut(static_cast<int>(slot.table),
                       static_cast<int>(kPaletteLut[slotIndex]), 256, mode);
    PALLOG("[palette] %s LUT 0x%08X rebuilt from table 0x%08X (mode %d)",
          slot.label, static_cast<unsigned>(kPaletteLut[slotIndex]),
          static_cast<unsigned>(slot.table), mode);
    PaletteRebuildRemap();
}

struct PaletteBackup
{
    bool valid = false;
    unsigned char bytes[kPaletteTableBytes] = {};
    bool lutValid = false;
    std::uint32_t lut[256] = {};
};
PaletteBackup g_paletteBackup[4];

constexpr size_t kPaletteRemapEntries = 65536;
std::uint16_t *g_paletteRemap = nullptr;
volatile LONG g_paletteRemapActive = 0;

std::uint32_t g_paletteDrawnLut[4][256] = {};
bool g_paletteDrawnValid = false;

void PaletteRebuildRemap()
{
    if (g_paletteRemap == nullptr)
    {
        g_paletteRemap = new (std::nothrow) std::uint16_t[kPaletteRemapEntries];
        if (g_paletteRemap == nullptr) return;
        for (size_t i = 0; i < kPaletteRemapEntries; ++i)
            g_paletteRemap[i] = static_cast<std::uint16_t>(i);
    }
    for (size_t i = 0; i < kPaletteRemapEntries; ++i)
        g_paletteRemap[i] = static_cast<std::uint16_t>(i);

    bool any = false;
    if (g_paletteDrawnValid)
    {
        for (int slotIndex = 0; slotIndex < 4; ++slotIndex)
        {
            const uintptr_t lut = kPaletteLut[slotIndex];
            if (!PaletteRangeWritable(lut, 256u * 4u)) continue;
            const std::uint32_t *live =
                reinterpret_cast<const std::uint32_t *>(lut);
            for (int i = 0; i < 256; ++i)
            {
                const std::uint16_t was =
                    static_cast<std::uint16_t>(g_paletteDrawnLut[slotIndex][i]);
                const std::uint16_t now =
                    static_cast<std::uint16_t>(live[i]);
                if (was == now) continue;
                g_paletteRemap[was] = now;
                any = true;
            }
        }
    }
    InterlockedExchange(&g_paletteRemapActive, any ? 1 : 0);
}

void PaletteNoteCanonicalFrame()
{
    for (int slotIndex = 0; slotIndex < 4; ++slotIndex)
    {
        const uintptr_t lut = kPaletteLut[slotIndex];
        if (!PaletteRangeWritable(lut, 256u * 4u)) continue;
        std::memcpy(g_paletteDrawnLut[slotIndex],
                    reinterpret_cast<const void *>(lut),
                    sizeof(g_paletteDrawnLut[slotIndex]));
        g_paletteDrawnValid = true;
    }
    InterlockedExchange(&g_paletteRemapActive, 0);
}

const std::uint16_t *PaletteFrameRemap()
{
    return InterlockedCompareExchange(&g_paletteRemapActive, 0, 0) != 0
        ? g_paletteRemap : nullptr;
}

char g_paletteStem[4][64] = {};

bool PaletteApplyLiveOverride(RGBQUAD *palette)
{
    (void)palette;
    return false;
}

/* JASC-PAL text file `<RESOURCE>_<n>.pal`, n = 1..4: lines JASC-PAL, 0100, 16, then one "R G B" line per colour. */
constexpr int kPaletteFileEntries = kPaletteColors;

const char *const kPaletteResourceNames[34] = {
    "SAORI",   "MIZUHO",  "RURIKO",  "HATUNE",  "KAEDE",
    "AZUSA",   "CHIZURU", "MIZUKI",  "AKARI",   "SERIKA",
    "TOMOKO",  "SIHO",    "AOI",     "YUKI",    "SERIO",
    "MULTI",   "KOTONE",  "REMY",    "RIO",     "AYAKA",
    "RINA",    "O_AKARI", "M_MULTI", "O_CHIZURU", "KANAKO",
    "C_SERIKA", "S_SIHO", "S_KOTONE", "LF97_AOI", "H_HATUNE",
    "TIRIA",   "R_AYAKA", "CORIN",   "YOSIE",
};

std::string PaletteDirectory()
{
    const std::string root = WrapperModuleDirectory();
    if (root.empty()) return {};
    const std::string dir = root + "\\palettes";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string PalettePath(const char *stem, int variant)
{
    const std::string dir = PaletteDirectory();
    if (dir.empty() || stem == nullptr || stem[0] == '\0') return {};
    std::string safe;
    for (const char *p = stem; *p != '\0' && safe.size() < 48; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_';
        safe.push_back(ok ? static_cast<char>(c) : '_');
    }
    if (safe.empty()) return {};
    char suffix[8] = {};
    std::snprintf(suffix, sizeof(suffix), "_%d.pal", variant + 1);
    return dir + "\\" + safe + suffix;
}

bool PaletteSaveVariant(const unsigned char *table, int variant,
                        const char *stem, std::string &message)
{
    const std::string path = PalettePath(stem, variant);
    if (path.empty())
    {
        message = "Give the palette a name first.";
        PALLOG("[palette] save refused: the character has no file name yet");
        return false;
    }
    std::FILE *file = nullptr;
    const errno_t opened = fopen_s(&file, path.c_str(), "w");
    if (opened != 0 || file == nullptr)
    {
        message = "Could not write " + path;
        WLOG_ISSUE("[palette] SAVE FAILED %s: open for write refused (errno %d)",
              path.c_str(), static_cast<int>(opened));
        return false;
    }
    std::fprintf(file, "JASC-PAL\n0100\n%d\n", kPaletteFileEntries);
    const unsigned char *base = table + variant * kPaletteColors * 4;
    for (int i = 0; i < kPaletteColors; ++i)
    {
        std::fprintf(file, "%u %u %u\n", base[i * 4 + 2], base[i * 4 + 1],
                     base[i * 4 + 0]);
    }
    std::fclose(file);
    message = "Saved " + path;
    PALLOG("[palette] saved variant %d to %s", variant + 1, path.c_str());
    return true;
}

bool PaletteLoadVariant(unsigned char *table, int variant, const char *stem,
                        std::string &message)
{
    const std::string path = PalettePath(stem, variant);
    if (path.empty())
    {
        message = "Give the palette a name first.";
        PALLOG("[palette] load refused: the character has no file name yet");
        return false;
    }
    std::FILE *file = nullptr;
    const errno_t opened = fopen_s(&file, path.c_str(), "r");
    if (opened != 0 || file == nullptr)
    {
        message = "No such file: " + path;
        PALLOG("[palette] load: %s could not be opened (errno %d)",
              path.c_str(), static_cast<int>(opened));
        return false;
    }

    constexpr long kPaletteFileFloorBytes = 24;
    constexpr long kPaletteFileCeilingBytes =
        16 + 256 * 12;
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        message = "Could not measure " + path;
        return false;
    }
    const long fileBytes = std::ftell(file);
    std::rewind(file);
    if (fileBytes < kPaletteFileFloorBytes
        || fileBytes > kPaletteFileCeilingBytes)
    {
        std::fclose(file);
        message = fileBytes <= 0
            ? std::string("File is empty: ") + path
            : "File is " + std::to_string(fileBytes) + " bytes; a palette is "
              + std::to_string(kPaletteFileFloorBytes) + " to "
              + std::to_string(kPaletteFileCeilingBytes);
        PALLOG("[palette] LOAD REJECTED %s: %ld bytes is outside %ld..%ld",
              path.c_str(), fileBytes, kPaletteFileFloorBytes,
              kPaletteFileCeilingBytes);
        return false;
    }

    char line[256] = {};
    int count = 0;
    const bool header =
        std::fgets(line, sizeof(line), file) != nullptr
        && std::strncmp(line, "JASC-PAL", 8) == 0
        && std::fgets(line, sizeof(line), file) != nullptr
        && std::fgets(line, sizeof(line), file) != nullptr
        && (count = std::atoi(line)) > 0;
    if (!header || count != kPaletteFileEntries)
    {
        std::fclose(file);
        message = "Not a JASC-PAL file with "
            + std::to_string(kPaletteFileEntries) + " colours.";
        PALLOG("[palette] LOAD REJECTED %s: header=%d count=%d, wanted %d",
              path.c_str(), header ? 1 : 0, count, kPaletteFileEntries);
        return false;
    }

    unsigned char staged[kPaletteColors * 4] = {};
    for (int i = 0; i < count; ++i)
    {
        if (std::fgets(line, sizeof(line), file) == nullptr)
        {
            std::fclose(file);
            message = "File ends after " + std::to_string(i) + " colours.";
            PALLOG("[palette] LOAD REJECTED %s: ends after %d of %d colours; "
                  "the live palette was not touched", path.c_str(), i, count);
            return false;
        }
        int r = 0, g = 0, b = 0;
        if (sscanf_s(line, "%d %d %d", &r, &g, &b) != 3)
        {
            std::fclose(file);
            message = "Colour " + std::to_string(i + 1) + " is malformed.";
            PALLOG("[palette] LOAD REJECTED %s: colour %d is malformed; the "
                  "live palette was not touched", path.c_str(), i + 1);
            return false;
        }
        const auto clamp = [](int v) -> unsigned char {
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            return static_cast<unsigned char>(v);
        };
        staged[i * 4 + 0] = clamp(b);
        staged[i * 4 + 1] = clamp(g);
        staged[i * 4 + 2] = clamp(r);
        staged[i * 4 + 3] = 0;
    }
    std::fclose(file);
    std::memcpy(table + variant * kPaletteColors * 4, staged,
                kPaletteColors * 4);
    message = "Loaded " + path;
    PALLOG("[palette] loaded variant %d from %s", variant + 1, path.c_str());
    return true;
}

/* Wire unit: a 256-byte blob of four variants, sixteen B,G,R,X entries each. */
enum class PaletteSource
{
    LocalFile,
    RemotePeer,
};

const char *PaletteSourceName(PaletteSource source)
{
    return source == PaletteSource::LocalFile ? "local file" : "remote peer";
}

/* Index 0 of every variant is the transparent key and must be black. */
bool PaletteValidateTable(const unsigned char *bytes, size_t length,
                          std::string &why)
{
    if (bytes == nullptr)
    {
        why = "no data";
        return false;
    }
    if (length != kPaletteTableBytes)
    {
        why = "wrong size: " + std::to_string(length) + " bytes, expected "
            + std::to_string(kPaletteTableBytes);
        return false;
    }
    for (int variant = 0; variant < kPaletteVariants; ++variant)
    {
        const unsigned char *key = bytes + variant * kPaletteColors * 4;
        if (key[0] != 0 || key[1] != 0 || key[2] != 0)
        {
            why = "variant " + std::to_string(variant + 1)
                + " does not have a black transparent key";
            return false;
        }
    }
    return true;
}

struct PalettePending
{
    bool valid = false;
    PaletteSource source = PaletteSource::LocalFile;
    int variant = -1;
    /* A 64-byte submission carries one variant, in the first 64 bytes of `table`. */
    bool variantOnly = false;
    unsigned char table[kPaletteTableBytes] = {};
};
PalettePending g_palettePending[4];

unsigned g_paletteLocalSlotMask = 0xFu;
bool g_paletteOwnershipAnnounced = false;

/* Producers run on the render, netplay and IPC worker threads; hold the critical section for the whole copy. */
std::mutex g_palettePendingMutex;

constexpr size_t kPaletteVariantBytes =
    static_cast<size_t>(kPaletteColors) * 4u;

bool PaletteSubmit(int slot, const unsigned char *bytes, size_t length,
                   PaletteSource source, std::string &why, int variant = -1)
{
    if (slot < 0 || slot >= 4)
    {
        why = "bad slot";
        return false;
    }
    if (bytes != nullptr && length == kPaletteVariantBytes)
    {
        if (variant < 0 || variant >= kPaletteVariants)
        {
            why = "variant-only palette without a variant index";
            return false;
        }
        if (bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 0)
        {
            why = "variant does not have a black transparent key";
            return false;
        }
        std::lock_guard<std::mutex> guard(g_palettePendingMutex);
        std::memset(g_palettePending[slot].table, 0, kPaletteTableBytes);
        std::memcpy(g_palettePending[slot].table, bytes,
                    kPaletteVariantBytes);
        g_palettePending[slot].source = source;
        g_palettePending[slot].variant = variant;
        g_palettePending[slot].variantOnly = true;
        g_palettePending[slot].valid = true;
        PALLOG("[palette] slot %d accepted a single-variant palette "
               "(variant %d) from the %s; queued for the next match tick",
               slot, variant + 1, PaletteSourceName(source));
        return true;
    }
    if (!PaletteValidateTable(bytes, length, why))
    {
        PALLOG("[palette] slot %d REJECTED a palette from the %s: %s",
              slot, PaletteSourceName(source), why.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(g_palettePendingMutex);
        std::memcpy(g_palettePending[slot].table, bytes, kPaletteTableBytes);
        g_palettePending[slot].source = source;
        g_palettePending[slot].variant = variant;
        g_palettePending[slot].variantOnly = false;
        g_palettePending[slot].valid = true;
    }
    PALLOG("[palette] slot %d accepted a palette from the %s; queued for the "
          "next match start", slot, PaletteSourceName(source));
    return true;
}

constexpr uintptr_t kQohSceneCurrent      = 0x6234ECu;  /* 3 = charsel */
constexpr uintptr_t kQohSelectConfirm     = 0x8716D0u;  /* +4*slot, 2 = palette stage */
constexpr uintptr_t kQohSelectPalette     = 0x870940u;  /* +0x460*slot, 0..3 */
constexpr uintptr_t kQohSelectButtonRing  = 0x8A654Cu;
constexpr uintptr_t kQohSelectButtonHead  = 0x8A65CCu;
constexpr uintptr_t kQohSelectButtonDwell = 0x8A658Cu;
constexpr int kQohSelectStride = 109;

struct PaletteAutoSlot
{
    bool haveDefault = false;
    unsigned char defaultTable[kPaletteTableBytes] = {};
    bool haveCustom = false;
    unsigned char customTable[kPaletteTableBytes] = {};
    int customVariant = -1;
    bool usingCustom = false;
    int defaultCharacter = -1;
    int armedVariant = -1;
    int lastConfirm = -1;
    int lastMask = -1;
    int lastHead = -1;
};
PaletteAutoSlot g_paletteAuto[4];
bool g_paletteAutoInCharSelect = false;
bool g_paletteToggleHeld = false;

bool PaletteAutoEnabled()
{
    return OverlayIniGet("PaletteAutoLoad", 1) != 0;
}

int PaletteToggleVirtualKey()
{
    const int key = OverlayIniGet("PaletteToggleKey", VK_TAB);
    return (key > 0 && key < 256) ? key : VK_TAB;
}

bool PaletteSelectorButton(int slot, uintptr_t &entryAddress,
                           uintptr_t &dwellAddress, int &mask)
{
    const uintptr_t headAddr =
        kQohSelectButtonHead + 4u * kQohSelectStride * slot;
    if (!PaletteRangeWritable(headAddr, 4)) return false;
    const int head = *reinterpret_cast<const int *>(headAddr) & 0xFF;
    const int index = head + kQohSelectStride * slot;
    entryAddress = kQohSelectButtonRing + 4u * index;
    dwellAddress = kQohSelectButtonDwell + 4u * index;
    if (!PaletteRangeWritable(entryAddress, 4)
        || !PaletteRangeWritable(dwellAddress, 4))
    {
        return false;
    }
    mask = *reinterpret_cast<const int *>(entryAddress);
    return true;
}

/* sub_47BF90 pushes forward through a 16-entry ring; lastHead+1..head covers exactly the new entries. */
bool PaletteSelectorSawD(int slot, int head, int lastHead, int &witness)
{
    const uintptr_t ringBase =
        kQohSelectButtonRing + 4u * kQohSelectStride * slot;
    bool saw = false;
    int index = (lastHead < 0) ? head : ((lastHead + 1) & 0xF);
    for (int guard = 0; guard < 16; ++guard)
    {
        const uintptr_t addr = ringBase + 4u * static_cast<unsigned>(index);
        if (PaletteRangeWritable(addr, 4))
        {
            const int mask = *reinterpret_cast<const int *>(addr);
            if ((mask & 0x8) != 0)
            {
                saw = true;
                witness = mask;
            }
        }
        if (index == head) break;
        index = (index + 1) & 0xF;
    }
    return saw;
}

constexpr uintptr_t kQohColourStepFn = 0x4339A0u;
constexpr unsigned char kQohColourStepSig[24] = {
    0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x8D, 0x34,
    0xCD, 0x00, 0x00, 0x00, 0x00, 0x2B, 0xF1, 0x57,
    0x89, 0x54, 0x24, 0x18, 0x89, 0x4C, 0x24, 0x14,
};

/* Same id space as byte_623EAE (0..33, alternates included), the index into kPaletteResourceNames; -1 when the seat is out of range. */
static int PaletteSeatCharacterId(int seat)
{
    if (seat < 0 || seat >= 4) return -1;
    typedef char(__fastcall *QohResolveCharFn)(int, int);
    const QohResolveCharFn resolve =
        reinterpret_cast<QohResolveCharFn>(0x429400u);
    const int row = *reinterpret_cast<const int *>(0x870954u + 0x460u * seat);
    const int col = *reinterpret_cast<const int *>(0x87095Cu + 0x460u * seat);
    const unsigned char *const rowChar =
        reinterpret_cast<const unsigned char *>(0x49EEE4u);
    const int id = static_cast<unsigned char>(resolve(rowChar[row], col));
    return id < IM_ARRAYSIZE(kPaletteResourceNames) ? id : -1;
}

static bool PaletteVariantFileExists(int characterId, int variant)
{
    if (characterId < 0 || characterId >= IM_ARRAYSIZE(kPaletteResourceNames)
        || variant < 0 || variant >= kPaletteVariants)
        return false;
    const std::string path =
        PalettePath(kPaletteResourceNames[characterId], variant);
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool PaletteArmedSeatWearsCustom(int seat)
{
    if (seat < 0 || seat >= 4 || g_paletteAuto[seat].armedVariant < 0)
        return false;
    if (g_paletteLocalSlotMask != 0xFu) return true;
    return PaletteVariantFileExists(PaletteSeatCharacterId(seat),
                                    g_paletteAuto[seat].armedVariant);
}

static int __fastcall PaletteColourStepHook(int side, int step)
{
    if (side < 0 || side >= 4) return 1;
    typedef char(__fastcall *QohResolveCharFn)(int, int);
    const QohResolveCharFn resolve =
        reinterpret_cast<QohResolveCharFn>(0x429400u);
    const auto colourAt = [](int seat) -> int * {
        return reinterpret_cast<int *>(0x870940u + 0x460u * seat);
    };
    const auto rowAt = [](int seat) -> const int * {
        return reinterpret_cast<const int *>(0x870954u + 0x460u * seat);
    };
    const auto colAt = [](int seat) -> const int * {
        return reinterpret_cast<const int *>(0x87095Cu + 0x460u * seat);
    };
    const int *const seatState = reinterpret_cast<const int *>(0x8716D0u);
    const unsigned char *const rowChar =
        reinterpret_cast<const unsigned char *>(0x49EEE4u);

    int value = *colourAt(side) + step;
    for (;;)
    {
        *colourAt(side) = value;
        int seat = side;
        bool retry = false;
        for (int i = 0; i < 3; ++i)
        {
            seat = (seat + 1) % 4;
            if (*colourAt(side) > 3) *colourAt(side) -= 4;
            if (*colourAt(side) < 0) *colourAt(side) += 4;
            if (seatState[seat] < 2) continue;
            if (PaletteArmedSeatWearsCustom(seat)) continue;
            const char theirs = resolve(rowChar[*rowAt(seat)], *colAt(seat));
            const char mine = resolve(rowChar[*rowAt(side)], *colAt(side));
            if (theirs == mine && *colourAt(seat) == *colourAt(side))
            {
                value = *colourAt(side) + (step >= 0 ? 1 : -1);
                retry = true;
                break;
            }
        }
        if (!retry) break;
    }
    return 1;
}

static void PaletteColourStepInstallOnce()
{
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    unsigned char *const target =
        reinterpret_cast<unsigned char *>(kQohColourStepFn);
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(target, &info, sizeof(info)) == 0
        || info.State != MEM_COMMIT
        || std::memcmp(target, kQohColourStepSig,
                       sizeof(kQohColourStepSig)) != 0)
    {
        PALLOG("[palette] same-colour unlock NOT installed: the code at "
               "0x%08X is not the build this hook was verified on; the "
               "vanilla rule stays", static_cast<unsigned>(kQohColourStepFn));
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        PALLOG("[palette] same-colour unlock NOT installed: VirtualProtect "
               "refused; the vanilla rule stays");
        return;
    }
    const uint32_t rel = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&PaletteColourStepHook)
        - (kQohColourStepFn + 5u));
    target[0] = 0xE9;
    std::memcpy(target + 1, &rel, 4);
    VirtualProtect(target, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    PALLOG("[palette] same-colour unlock installed: a seat that confirms "
           "its colour with D no longer blocks that slot for the others");
}

void PaletteAutoCharSelectTick()
{
    PaletteColourStepInstallOnce();
    for (int slot = 0; slot < 4; ++slot)
    {
        PaletteAutoSlot &self = g_paletteAuto[slot];
        const uintptr_t confirmAddr = kQohSelectConfirm + 4u * slot;
        if (!PaletteRangeWritable(confirmAddr, 4)) continue;
        const int confirm = *reinterpret_cast<const int *>(confirmAddr);
        const int previousConfirm = self.lastConfirm;
        /* Selector substate: 1 = picking a character, 2 = picking its colour, 3/4 = confirmed. */
        if (confirm != self.lastConfirm)
        {
            self.lastConfirm = confirm;
            PALLOG("[palette] slot %d selector substate -> %d%s", slot, confirm,
                  confirm == 2 ? " (colour pick; A = own palette, D = custom)"
                               : "");
            if (confirm < 2 && self.armedVariant >= 0)
            {
                PALLOG("[palette] slot %d left the colour stage; dropping the "
                       "armed custom variant %d", slot, self.armedVariant + 1);
                self.armedVariant = -1;
            }
        }

        const uintptr_t headAddr =
            kQohSelectButtonHead + 4u * kQohSelectStride * slot;
        if (!PaletteRangeWritable(headAddr, 4))
        {
            if (self.lastMask != -2)
            {
                self.lastMask = -2;
                PALLOG("[palette] slot %d selector button ring is NOT readable"
                       " (head addr 0x%08X); D cannot be seen", slot,
                       static_cast<unsigned>(headAddr));
            }
            continue;
        }
        const int head = *reinterpret_cast<const int *>(headAddr) & 0xF;

        {
            uintptr_t entry = 0, dwell = 0;
            int mask = 0;
            if (PaletteSelectorButton(slot, entry, dwell, mask)
                && (mask != self.lastMask || head != self.lastHead))
            {
                PALLOG("[palette] slot %d selector buttons: head=%d mask=0x%X"
                       " (A=%d B=%d C=%d D=%d) substate=%d",
                       slot, head, mask, (mask & 1) ? 1 : 0,
                       (mask & 2) ? 1 : 0, (mask & 4) ? 1 : 0,
                       (mask & 8) ? 1 : 0, confirm);
                self.lastMask = mask;
            }
        }

        /* The palette stage reads (dword_8A654C[head] & 7) - 1: D alone (8) is inert, A|D (9) masks to the native confirm. */
        const bool atColourStage = (confirm == 2 || previousConfirm == 2);
        if (head != self.lastHead || self.lastHead < 0)
        {
            int witness = 0;
            const bool sawD =
                PaletteSelectorSawD(slot, head, self.lastHead, witness);
            self.lastHead = head;
            if (sawD && atColourStage && self.armedVariant < 0)
            {
                const uintptr_t paletteAddr = kQohSelectPalette + 0x460u * slot;
                int variant = 0;
                if (PaletteRangeWritable(paletteAddr, 4))
                {
                    variant = *reinterpret_cast<const int *>(paletteAddr)
                        % kPaletteVariants;
                    if (variant < 0) variant += kPaletteVariants;
                }
                if (g_paletteLocalSlotMask == 0xFu)
                {
                    const int characterId = PaletteSeatCharacterId(slot);
                    if (!PaletteVariantFileExists(characterId, variant))
                    {
                        PALLOG("[palette] slot %d pressed D at colour %d but "
                               "there is no %s_%d.pal in the palettes folder; "
                               "the default palette stays and the slot stays "
                               "unique",
                               slot, variant + 1,
                               characterId >= 0
                                   ? kPaletteResourceNames[characterId]
                                   : "(unknown)",
                               variant + 1);
                        continue;
                    }
                }
                self.armedVariant = variant;
                PALLOG("[palette] slot %d armed custom variant %d via %s "
                       "(ring mask 0x%X, substate %d->%d); the match start "
                       "will load it",
                       slot, variant + 1,
                       (witness & 0x1) ? "D (confirmed)" : "D",
                       witness, previousConfirm, confirm);
            }
            else if (sawD && !atColourStage)
            {
                PALLOG("[palette] slot %d saw D (ring mask 0x%X) at substate "
                       "%d, which is not the colour pick; ignored",
                       slot, witness, confirm);
            }
        }
    }
}

bool PaletteTableLooksAuthored(const unsigned char *table)
{
    for (int variant = 0; variant < kPaletteVariants; ++variant)
    {
        const unsigned char *entries = table + variant * kPaletteColors * 4;
        for (int colour = 1; colour < kPaletteColors; ++colour)
        {
            if (entries[colour * 4 + 0] != 0 || entries[colour * 4 + 1] != 0
                || entries[colour * 4 + 2] != 0)
            {
                return true;
            }
        }
    }
    return false;
}

constexpr int kPaletteUndoDepth = 32;
constexpr unsigned long long kPaletteUndoBurstMs = 400ull;
struct PaletteUndoState
{
    unsigned char undo[kPaletteUndoDepth][kPaletteTableBytes];
    unsigned char redo[kPaletteUndoDepth][kPaletteTableBytes];
    int undoCount = 0;
    int redoCount = 0;
    unsigned char lastSeen[kPaletteTableBytes];
    bool lastSeenValid = false;
    int character = -1;
    ULONGLONG lastEditMs = 0;
};
PaletteUndoState g_paletteUndoHistory[4];

bool PaletteUndoContext(int slot, unsigned char *&table, int &character)
{
    if (slot < 0 || slot >= 4) return false;
    const PaletteSlotInfo &info = kPaletteSlots[slot];
    if (!PaletteRangeWritable(info.table, kPaletteTableBytes)
        || !PaletteRangeWritable(info.indexByte - 1u, 1))
        return false;
    table = reinterpret_cast<unsigned char *>(info.table);
    character = static_cast<int>(
        *reinterpret_cast<const unsigned char *>(info.indexByte - 1u));
    return true;
}

void PaletteUndoExternalSync(int slot)
{
    unsigned char *table = nullptr;
    int character = 0;
    if (!PaletteUndoContext(slot, table, character)) return;
    PaletteUndoState &h = g_paletteUndoHistory[slot];
    if (h.character != character)
    {
        h.undoCount = 0;
        h.redoCount = 0;
        h.character = character;
    }
    std::memcpy(h.lastSeen, table, kPaletteTableBytes);
    h.lastSeenValid = true;
    h.lastEditMs = 0;
}

void PaletteUndoRecord(int slot)
{
    unsigned char *table = nullptr;
    int character = 0;
    if (!PaletteUndoContext(slot, table, character)) return;
    PaletteUndoState &h = g_paletteUndoHistory[slot];
    if (!h.lastSeenValid || h.character != character)
    {
        h.undoCount = 0;
        h.redoCount = 0;
        h.character = character;
        std::memcpy(h.lastSeen, table, kPaletteTableBytes);
        h.lastSeenValid = true;
        h.lastEditMs = GetTickCount64();
        return;
    }
    if (std::memcmp(h.lastSeen, table, kPaletteTableBytes) == 0) return;
    const ULONGLONG now = GetTickCount64();
    if (h.undoCount == 0 || h.lastEditMs == 0
        || now - h.lastEditMs >= kPaletteUndoBurstMs)
    {
        if (h.undoCount == kPaletteUndoDepth)
        {
            std::memmove(h.undo[0], h.undo[1],
                         static_cast<size_t>(kPaletteUndoDepth - 1)
                             * kPaletteTableBytes);
            --h.undoCount;
        }
        std::memcpy(h.undo[h.undoCount], h.lastSeen, kPaletteTableBytes);
        ++h.undoCount;
    }
    h.redoCount = 0;
    std::memcpy(h.lastSeen, table, kPaletteTableBytes);
    h.lastEditMs = now;
}

bool PaletteStudioUndo(int slot)
{
    unsigned char *table = nullptr;
    int character = 0;
    if (!PaletteUndoContext(slot, table, character)) return false;
    PaletteUndoState &h = g_paletteUndoHistory[slot];
    if (h.character != character || h.undoCount == 0) return false;
    if (h.redoCount < kPaletteUndoDepth)
    {
        std::memcpy(h.redo[h.redoCount], table, kPaletteTableBytes);
        ++h.redoCount;
    }
    --h.undoCount;
    std::memcpy(table, h.undo[h.undoCount], kPaletteTableBytes);
    std::memcpy(h.lastSeen, table, kPaletteTableBytes);
    h.lastEditMs = 0;
    PalettePublishSlot(slot);
    PALLOG("[palette] undo: slot %d back one colour change (%d left)",
           slot, h.undoCount);
    return true;
}

bool PaletteStudioRedo(int slot)
{
    unsigned char *table = nullptr;
    int character = 0;
    if (!PaletteUndoContext(slot, table, character)) return false;
    PaletteUndoState &h = g_paletteUndoHistory[slot];
    if (h.character != character || h.redoCount == 0) return false;
    if (h.undoCount == kPaletteUndoDepth)
    {
        std::memmove(h.undo[0], h.undo[1],
                     static_cast<size_t>(kPaletteUndoDepth - 1)
                         * kPaletteTableBytes);
        --h.undoCount;
    }
    std::memcpy(h.undo[h.undoCount], table, kPaletteTableBytes);
    ++h.undoCount;
    --h.redoCount;
    std::memcpy(table, h.redo[h.redoCount], kPaletteTableBytes);
    std::memcpy(h.lastSeen, table, kPaletteTableBytes);
    h.lastEditMs = 0;
    PalettePublishSlot(slot);
    PALLOG("[palette] redo: slot %d forward one colour change (%d left)",
           slot, h.redoCount);
    return true;
}

void PaletteAutoMatchTick()
{
    for (int slot = 0; slot < 4; ++slot)
    {
        PaletteAutoSlot &self = g_paletteAuto[slot];
        const PaletteSlotInfo &info = kPaletteSlots[slot];
        if (!PaletteRangeWritable(info.table, kPaletteTableBytes)
            || !PaletteRangeWritable(info.indexByte, 1))
        {
            continue;
        }
        unsigned char *const table =
            reinterpret_cast<unsigned char *>(info.table);

        const uintptr_t characterAddr = info.indexByte - 1u;
        const int character = PaletteRangeWritable(characterAddr, 1)
            ? static_cast<int>(
                  *reinterpret_cast<const unsigned char *>(characterAddr))
            : -1;
        if (self.haveDefault && character != self.defaultCharacter)
        {
            PALLOG("[palette] slot %d character changed (%d -> %d); dropping "
                   "the captured palettes so nothing stale is applied",
                   slot, self.defaultCharacter, character);
            const int keepArmed = self.armedVariant;
            self = PaletteAutoSlot{};
            self.armedVariant = keepArmed;
        }

        if (!self.haveDefault)
        {
            if (!PaletteTableLooksAuthored(table)) continue;
            std::memcpy(self.defaultTable, table, kPaletteTableBytes);
            self.haveDefault = true;
            self.usingCustom = false;
            self.defaultCharacter = character;
            PALLOG("[palette] slot %d captured character %d's own palette on "
                   "the first filled frame (table 0x%08X); TAB restores this",
                   slot, character, static_cast<unsigned>(info.table));
        }

        if (self.haveDefault && self.haveCustom && self.usingCustom
            && std::memcmp(table, self.defaultTable,
                           kPaletteTableBytes) == 0
            && std::memcmp(self.customTable, self.defaultTable,
                           kPaletteTableBytes) != 0)
        {
            std::memcpy(table, self.customTable, kPaletteTableBytes);
            PalettePublishSlot(slot);
            PaletteUndoExternalSync(slot);
            PALLOG("[palette] slot %d re-applied the custom palette after the "
                   "game reloaded its table (rematch/round reload path)",
                   slot);
        }

        if (self.armedVariant >= 0)
        {
            const int variant = self.armedVariant;
            self.armedVariant = -1;
            if ((g_paletteLocalSlotMask & (1u << slot)) == 0u)
            {
                PALLOG("[palette] slot %d armed variant %d, but this machine "
                       "does not own that slot; waiting for its owner's "
                       "palette instead of loading a local file",
                       slot, variant + 1);
                continue;
            }
            if (character < 0
                || character >= IM_ARRAYSIZE(kPaletteResourceNames))
            {
                PALLOG("[palette] slot %d armed but character id %d is out of "
                      "range; nothing loaded", slot, character);
            }
            else
            {
                unsigned char staged[kPaletteTableBytes];
                std::memcpy(staged, self.defaultTable, kPaletteTableBytes);
                std::string message;
                if (PaletteLoadVariant(staged, variant,
                                       kPaletteResourceNames[character],
                                       message))
                {
                    std::string why;
                    (void)PaletteSubmit(slot, staged, sizeof(staged),
                                        PaletteSource::LocalFile, why,
                                        variant);
                }
                else
                {
                    std::memcpy(table, self.defaultTable, kPaletteTableBytes);
                    self.haveCustom = false;
                    self.usingCustom = false;
                    PalettePublishSlot(slot);
                    PaletteUndoExternalSync(slot);
                }
                PALLOG("[palette] slot %d character %s variant %d: %s", slot,
                      kPaletteResourceNames[character], variant + 1,
                      message.c_str());
            }
        }

        PalettePending pending{};
        {
            std::lock_guard<std::mutex> guard(g_palettePendingMutex);
            if (!g_palettePending[slot].valid) continue;
            pending = g_palettePending[slot];
            g_palettePending[slot].valid = false;
        }
        if (pending.variantOnly)
        {
            std::memcpy(self.customTable, self.defaultTable,
                        kPaletteTableBytes);
            const int variant =
                (pending.variant >= 0 && pending.variant < kPaletteVariants)
                    ? pending.variant : 0;
            std::memcpy(self.customTable
                            + static_cast<size_t>(variant)
                                  * kPaletteVariantBytes,
                        pending.table, kPaletteVariantBytes);
        }
        else
        {
            std::memcpy(self.customTable, pending.table, kPaletteTableBytes);
        }
        self.haveCustom = true;
        self.customVariant = pending.variant;
        self.usingCustom = true;
        std::memcpy(table, self.customTable, kPaletteTableBytes);
        PalettePublishSlot(slot);
        PaletteUndoExternalSync(slot);
        PALLOG("[palette] slot %d now drawing the custom palette (%s)",
              slot, PaletteSourceName(pending.source));
    }
}

void PaletteAutoToggle()
{
    for (int slot = 0; slot < 4; ++slot)
    {
        if (g_paletteLocalSlotMask != 0xFu
            && (g_paletteLocalSlotMask & (1u << slot)) != 0u)
        {
            continue;
        }
        PaletteAutoSlot &self = g_paletteAuto[slot];
        if (!self.haveCustom || !self.haveDefault) continue;
        const PaletteSlotInfo &info = kPaletteSlots[slot];
        if (!PaletteRangeWritable(info.table, kPaletteTableBytes)) continue;
        const bool wantCustom = !self.usingCustom;
        const unsigned char *const source =
            wantCustom ? self.customTable : self.defaultTable;
        if (!PaletteTableLooksAuthored(source))
        {
            PALLOG("[palette] slot %d REFUSED to switch to the %s palette: it "
                   "is entirely black, which would render the fighter as a "
                   "silhouette; keeping what is on screen",
                   slot, wantCustom ? "custom" : "game's own");
            continue;
        }
        self.usingCustom = wantCustom;
        std::memcpy(reinterpret_cast<void *>(info.table), source,
                    kPaletteTableBytes);
        PalettePublishSlot(slot);
        PaletteUndoExternalSync(slot);
        PALLOG("[palette] slot %d switched to the %s palette", slot,
              self.usingCustom ? "custom" : "game's own");
    }
}

void PaletteAutoTick(bool appActive)
{
    if (!PaletteRangeWritable(kQohSceneCurrent, 4)) return;
    const int scene = *reinterpret_cast<const int *>(kQohSceneCurrent);

    if (!PaletteAutoEnabled())
    {
        static int s_announcedForScene = -1;
        if (scene == 3 && s_announcedForScene != scene)
        {
            s_announcedForScene = scene;
            PALLOG("[palette] character select reached but custom palettes are "
                  "OFF: D will do nothing. Set PaletteAutoLoad=1 in "
                  "ddraw_wrapper.ini [Overlay], or tick 'Enable' in "
                  "Diagnostics > Palettes.");
        }
        if (scene != 3) s_announcedForScene = -1;
        return;
    }

    if (scene == 3)
    {
        if (!g_paletteAutoInCharSelect)
        {
            g_paletteAutoInCharSelect = true;
            PALLOG("[palette] character select reached; D arms a custom "
                  "palette while a colour is being picked");
            for (PaletteAutoSlot &self : g_paletteAuto)
            {
                self = PaletteAutoSlot{};
            }
            {
                std::lock_guard<std::mutex> guard(g_palettePendingMutex);
                for (PalettePending &pending : g_palettePending)
                {
                    pending = PalettePending{};
                }
            }
            for (int slot = 0; slot < 4; ++slot)
            {
                const uintptr_t table = kPaletteSlots[slot].table;
                if (PaletteRangeWritable(table, kPaletteTableBytes))
                {
                    std::memset(reinterpret_cast<void *>(table), 0,
                                kPaletteTableBytes);
                }
            }
            PALLOG("[palette] cleared the four match palette tables; the "
                   "authored-table wait now gates on THIS match's fill, not "
                   "on the previous match's leftovers");
        }
        PaletteAutoCharSelectTick();
        return;
    }
    g_paletteAutoInCharSelect = false;
    if (scene != 2) return;

    PaletteAutoMatchTick();

    const bool down = appActive
        && (GetAsyncKeyState(PaletteToggleVirtualKey()) & 0x8000) != 0;
    if (down && !g_paletteToggleHeld) PaletteAutoToggle();
    g_paletteToggleHeld = down;
}

void PaletteTakeSnapshot(int slotIndex, const unsigned char *table)
{
    PaletteBackup &backup = g_paletteBackup[slotIndex];
    std::memcpy(backup.bytes, table, kPaletteTableBytes);
    backup.valid = true;
    const uintptr_t lut = kPaletteLut[slotIndex];
    backup.lutValid = PaletteRangeWritable(lut, 256u * 4u);
    if (backup.lutValid)
    {
        std::memcpy(backup.lut, reinterpret_cast<const void *>(lut),
                    sizeof(backup.lut));
    }
    PaletteRebuildRemap();
}

void DrawPaletteVariantRow(int slotIndex, unsigned char *table, int variant,
                           bool live, std::string &message)
{
    ImGui::PushID(variant);
    const float swatch = ImGui::GetFontSize() * 1.6f;

    if (live)
    {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f),
                           "Variant %d -- on screen", variant + 1);
    }
    else
    {
        ImGui::TextDisabled("Variant %d", variant + 1);
    }
    ImGui::SameLine(ImGui::GetFontSize() * 12.0f);
    if (ImGui::SmallButton("Save"))
    {
        (void)PaletteSaveVariant(table, variant,
                                 g_paletteStem[slotIndex], message);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Load"))
    {
        if (PaletteLoadVariant(table, variant, g_paletteStem[slotIndex],
                               message)
            && live)
        {
            PalettePublishSlot(slotIndex);
        }
    }
    ImGui::SameLine();
    {
        const PaletteAutoSlot &captured = g_paletteAuto[slotIndex];
        const uintptr_t characterAddr =
            kPaletteSlots[slotIndex].indexByte - 1u;
        const bool haveAuthored = captured.haveDefault
            && PaletteRangeWritable(characterAddr, 1)
            && captured.defaultCharacter == static_cast<int>(
                   *reinterpret_cast<const unsigned char *>(characterAddr));
        if (haveAuthored && ImGui::SmallButton("Default"))
        {
            const size_t offset =
                static_cast<size_t>(variant) * kPaletteColors * 4u;
            std::memcpy(table + offset, captured.defaultTable + offset,
                        kPaletteColors * 4u);
            if (live) PalettePublishSlot(slotIndex);
            message = "Loaded the game's own colours for this variant";
        }
    }

    unsigned char *base = table + variant * kPaletteColors * 4;
    for (int color = 1; color < kPaletteColors; ++color)
    {
        unsigned char *entry = base + color * 4;
        const ImVec4 shown(static_cast<float>(entry[2]) / 255.0f,
                           static_cast<float>(entry[1]) / 255.0f,
                           static_cast<float>(entry[0]) / 255.0f, 1.0f);
        ImGui::PushID(color);
        if (color > 1) ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::ColorButton("##sw", shown,
                               ImGuiColorEditFlags_NoAlpha
                                   | ImGuiColorEditFlags_NoTooltip,
                               ImVec2(swatch, swatch)))
        {
            ImGui::OpenPopup("pick");
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Colour %d\n#%02X%02X%02X\nClick to edit",
                              color, entry[2], entry[1], entry[0]);
        }
        if (ImGui::BeginPopup("pick"))
        {
            float rgb[3] = {shown.x, shown.y, shown.z};
            ImGui::Text("Variant %d, colour %d", variant + 1, color);
            if (ImGui::ColorPicker3("##picker", rgb,
                                    ImGuiColorEditFlags_NoSidePreview
                                        | ImGuiColorEditFlags_NoSmallPreview))
            {
                entry[2] = static_cast<unsigned char>(rgb[0] * 255.0f + 0.5f);
                entry[1] = static_cast<unsigned char>(rgb[1] * 255.0f + 0.5f);
                entry[0] = static_cast<unsigned char>(rgb[2] * 255.0f + 0.5f);
                if (live) PalettePublishSlot(slotIndex);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                PALLOG("[palette] %s variant %d colour %d -> %02X%02X%02X",
                      kPaletteSlots[slotIndex].label, variant, color,
                      entry[2], entry[1], entry[0]);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
}

void DrawPaletteSlotEditor(int slotIndex,
                          const Qoh99TrainingStatusV1 &status)
{
    const PaletteSlotInfo &slot = kPaletteSlots[slotIndex];
    if (!PaletteRangeWritable(slot.table, kPaletteTableBytes)
        || !PaletteRangeWritable(slot.indexByte, 1))
    {
        ImGui::TextDisabled("%s -- not mapped (no match running)", slot.label);
        return;
    }

    unsigned char *const table =
        reinterpret_cast<unsigned char *>(slot.table);
    PaletteBackup &backup = g_paletteBackup[slotIndex];
    if (!backup.valid)
    {
        PaletteTakeSnapshot(slotIndex, table);
        PALLOG("[palette] %s table=0x%08X key=%02X%02X%02X snapshot taken",
              slot.label, static_cast<unsigned>(slot.table),
              table[2], table[1], table[0]);
    }

    bool looksLikePalette = true;
    for (int variant = 1; variant < kPaletteVariants; ++variant)
    {
        if (std::memcmp(table, table + variant * kPaletteColors * 4, 3) != 0)
        {
            looksLikePalette = false;
            break;
        }
    }

    const unsigned char rawIndex =
        *reinterpret_cast<unsigned char *>(slot.indexByte);
    const int activeVariant = (rawIndex / 16) % kPaletteVariants;

    ImGui::PushID(slotIndex);
    if (!looksLikePalette)
    {
        ImGui::TextDisabled(
            "%s -- no palette here (the four variants disagree on the "
            "transparent key). No character loaded, or a wrong address.",
            slot.label);
        ImGui::PopID();
        return;
    }

    static DWORD s_stemCharacter[4] = {0xFFFFFFFFu, 0xFFFFFFFFu,
                                       0xFFFFFFFFu, 0xFFFFFFFFu};
    static std::string s_message[4];
    const DWORD character = slotIndex == 0 ? status.p1Character
        : (slotIndex == 1 ? status.p2Character : 0xFFFFFFFFu);
    if (character != s_stemCharacter[slotIndex])
    {
        s_stemCharacter[slotIndex] = character;
        PaletteTakeSnapshot(slotIndex, table);
        if (character < IM_ARRAYSIZE(kPaletteResourceNames))
        {
            std::snprintf(g_paletteStem[slotIndex],
                          sizeof(g_paletteStem[slotIndex]), "%s",
                          kPaletteResourceNames[character]);
        }
        PALLOG("[palette] %s character changed to %lu; snapshot re-taken",
              slot.label, static_cast<unsigned long>(character));
    }

    const bool open = ImGui::CollapsingHeader(
        slot.label, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetFontSize() * 4.0f);
    ImGui::TextDisabled("variant %d on screen", activeVariant + 1);
    if (!open)
    {
        ImGui::PopID();
        return;
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::InputText("file name", g_paletteStem[slotIndex],
                     static_cast<int>(sizeof(g_paletteStem[slotIndex])));
    ImGui::SameLine();
    if (ImGui::Button("Restore"))
    {
        std::memcpy(table, backup.bytes, kPaletteTableBytes);
        PalettePublishSlot(slotIndex);
        s_message[slotIndex] = "Restored to the colours this match started on";
        PALLOG("[palette] %s restored to the snapshot", slot.label);
    }

    DrawPaletteVariantRow(slotIndex, table, activeVariant, true,
                          s_message[slotIndex]);
    for (int variant = 0; variant < kPaletteVariants; ++variant)
    {
        if (variant == activeVariant) continue;
        DrawPaletteVariantRow(slotIndex, table, variant, false,
                              s_message[slotIndex]);
    }

    if (!s_message[slotIndex].empty())
    {
        TrainingTextDisabledWrapped("%s", s_message[slotIndex].c_str());
    }
    ImGui::PopID();
    ImGui::Spacing();
}

void DrawPaletteDelivery()
{
    const LONG frames = g_palettePresentDiag.frames;
    const LONG indexed = g_palettePresentDiag.indexedFrames;
    const LONG withPalette = g_palettePresentDiag.paletteFrames;
    const LONG overridden = g_palettePresentDiag.overrideFrames;
    const LONG bpp = g_palettePresentDiag.lastBpp;
    const LONG kind = g_palettePresentDiag.lastFormatKind;

    if (frames == 0)
    {
        TrainingTextColoredWrapped(kIdleTestDivergedColor,
            "The accelerated presenter has not run. Colours here cannot "
            "reach the screen at all on this path.");
        return;
    }
    if (indexed == 0)
    {
        TrainingTextColoredWrapped(kIdleTestDivergedColor,
            "The game is presenting %ld-bit frames (format kind %ld), not "
            "8-bit indexed. There is no palette at present time: the colours "
            "were already resolved when the sprite was drawn, so editing a "
            "palette cannot change a frame that has been composed. This is "
            "the case where a runtime override has to move to the blit, not "
            "the present.", bpp * 8, kind);
        return;
    }
    if (withPalette == 0)
    {
        TrainingTextColoredWrapped(kIdleTestDivergedColor,
            "Frames are 8-bit indexed but arrive with NO palette attached, "
            "so the presenter is drawing them as greyscale indices. Nothing "
            "to override.");
        return;
    }
    if (overridden == 0)
    {
        TrainingTextDisabledWrapped(
            "Delivery is fine -- %ld indexed presents, %ld with a palette -- "
            "but no slot is being overridden yet. A slot is overridden only "
            "once you change one of its colours, and only after this tab has "
            "read it once.", indexed, withPalette);
        return;
    }
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
        "Your colours are reaching the screen (%ld presents overridden).",
        overridden);
}

void DrawPaletteTab(const Qoh99TrainingStatusV1 &status)
{
    ImGui::SeparatorText(qi18n::tr("Is it reaching the screen?"));
    DrawPaletteDelivery();

    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("How this works"));
    TrainingTextDisabledWrapped(
        "Sprites are 4-bit: every pixel is an index 0-15, and the variant a "
        "player picked just offsets that index. So a character has four "
        "16-colour sets and only one of them is on screen at a time.");
    TrainingTextDisabledWrapped(
        "The game copies the chosen set into its live palette only at a scene "
        "change, which never happens while training is paused. So a character "
        "you have edited is painted by the presenter instead, every frame -- "
        "that is why edits show at once, and why an edited character stops "
        "fading with the rest of the screen until you press Restore.");

    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("Files"));
    TrainingTextDisabledWrapped(
        "Save and Load use JASC-PAL, which Aseprite, GraphicsGale and "
        "Paint.NET all read. One file per variant, named <name>_1.pal to "
        "<name>_4.pal, in the palettes folder beside the game. The name is "
        "filled in from the character's own resource name.");

    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("Load at character select"));
    bool autoLoad = PaletteAutoEnabled();
    if (ImGui::Checkbox("Enable (offline only)", &autoLoad))
    {
        OverlayIniSet("PaletteAutoLoad", autoLoad ? 1 : 0);
        PALLOG("[palette] auto-load %s", autoLoad ? "ENABLED" : "disabled");
    }
    TrainingTextDisabledWrapped(
        "With this on, press D on the palette you want at character select: "
        "it confirms, and <name>_<that number>.pal is applied when the match "
        "starts. Key 0x%02X swaps that character between your palette and the "
        "game's own; it does nothing for a character with no custom palette "
        "loaded. Both settings live in ddraw_wrapper.ini under [Overlay] as "
        "PaletteAutoLoad and PaletteToggleKey.",
        PaletteToggleVirtualKey());

    ImGui::Spacing();
    for (int i = 0; i < 4; ++i)
    {
        DrawPaletteSlotEditor(i, status);
    }
}

void DrawTrainingDebugTools(const Qoh99TrainingStatusV1 &status)
{
    const bool overlayTier = TrainingHasDebugOverlayStatus(status);
    const bool logTier = TrainingHasStateLogStatus(status);

    ImGui::SeparatorText(qi18n::tr("Live panel"));
    bool overlay = overlayTier && status.debugOverlayEnabled != 0u;
    ImGui::BeginDisabled(!overlayTier);
    if (ImGui::Checkbox("Show debug panel over the game", &overlay))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_DEBUG_OVERLAY_ENABLED,
            0, overlay ? 1u : 0u);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "A small read-only panel with both fighters' action states,\n"
            "whether each can act, and the wake-up reversal phase.\n"
            "It only draws during live training.");
    }
    if (!overlayTier)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("State-change log"));
    TrainingTextDisabledWrapped(
        "Writes training_state.log into this session's log folder, next to "
        "dll.log. One line per CHANGE only -- an action transition, a change "
        "in what is armed or queued, or a change in why the dummy is not "
        "pressing anything. A quiet round writes nothing.");

    bool logging = logTier && status.stateChangeLogEnabled != 0u;
    ImGui::BeginDisabled(!logTier);
    if (ImGui::Checkbox("Record state changes to a file", &logging))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_STATE_CHANGE_LOG_ENABLED,
            0, logging ? 1u : 0u);
    }
    ImGui::EndDisabled();
    if (!logTier)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER);
        return;
    }
    if (status.stateChangeLogFailed != 0u)
    {
        TrainingTextColoredWrapped(kIdleTestDivergedColor,
            "The log file could not be opened, so nothing is being "
            "recorded. This needs a session log folder, which only a caster "
            "launch provides.");
    }
    else if (status.stateChangeLogEnabled != 0u)
    {
        ImGui::Text("Recording -- %lu change%s written.",
                    static_cast<unsigned long>(status.stateChangeLogRecords),
                    status.stateChangeLogRecords == 1u ? "" : "s");
    }

    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("Frame trace"));
    TrainingTextDisabledWrapped(
        "Writes frametrace.csv into this session's log folder. One row per "
        "tick per fighter with the CHR frame actually on screen: action, "
        "cursor, hold, local frame, and that frame's authored duration, "
        "activity flags and sprite. This is for comparing a move's authored "
        "frame list against what the engine really plays back -- it writes "
        "every tick, so leave it off unless you are capturing.");

    const bool traceTier = TrainingHasFrameTraceStatus(status);
    bool tracing = traceTier && status.frameTraceEnabled != 0u;
    ImGui::BeginDisabled(!traceTier);
    if (ImGui::Checkbox("Record a per-tick frame trace", &tracing))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_FRAME_TRACE_ENABLED,
            0, tracing ? 1u : 0u);
    }
    ImGui::EndDisabled();
    if (!traceTier)
    {
        DrawTrainingStaleDllReason(
            status, QOH99_TRAINING_STATUS_V1_FRAME_TRACE_TIER);
    }
    else if (status.frameTraceFailed != 0u)
    {
        TrainingTextColoredWrapped(kIdleTestDivergedColor,
            "The trace file could not be opened, so nothing is being "
            "recorded. This needs a session log folder, which only a caster "
            "launch provides.");
    }
    else if (status.frameTraceCapped != 0u)
    {
        ImGui::Text("Row cap reached -- %lu rows written; capture stopped.",
                    static_cast<unsigned long>(status.frameTraceRows));
    }
    else if (status.frameTraceEnabled != 0u)
    {
        ImGui::Text("Recording -- %lu row%s written.",
                    static_cast<unsigned long>(status.frameTraceRows),
                    status.frameTraceRows == 1u ? "" : "s");
    }

    ImGui::Spacing();
    DrawNativeHudControls(status);
    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("Why the dummy is not responding"));
    ImGui::TextWrapped("%s",
        TrainingSuppressionReasonText(status.triggerSuppressionReason));
    TrainingTextDisabledWrapped(
        "This is the DLL's own reason for the current step. If a reaction "
        "looks broken, this line and the log say which of the two it is: no "
        "response configured for that situation, or a response that is "
        "configured and still waiting.");

}

void DrawDiagnosticsTab(
    const EfzDx9PresentParams &params,
    const Qoh99TrainingStatusV1 &status,
    bool trainingPresent)
{
    if (trainingPresent)
    {
        if (!ImGui::BeginTabBar("qoh99_diagnostics_tabs"))
        {
            return;
        }
        if (ImGui::BeginTabItem(qi18n::tr("Status")))
        {
            {
                DrawTrainingStatusSummary(status);
                ImGui::Spacing();
                DrawOverlayStatusSummary(params);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(qi18n::tr("Combat")))
        {
            if (ImGui::CollapsingHeader("Damage",
                    ImGuiTreeNodeFlags_DefaultOpen))
            {
                {
                    DrawTrainingDamageSummary(status);
                }
            }
            if (ImGui::CollapsingHeader("Characters",
                    ImGuiTreeNodeFlags_DefaultOpen))
            {
                {
                    DrawTrainingCharacterSummary(status);
                }
            }
            if (ImGui::CollapsingHeader("Restore",
                    ImGuiTreeNodeFlags_DefaultOpen))
            {
                {
                    DrawTrainingRestoreSummary(status);
                }
            }
            if (ImGui::CollapsingHeader("Recovery",
                    ImGuiTreeNodeFlags_DefaultOpen))
            {
                {
                DrawTrainingRecoverySummary(status);
                ImGui::Spacing();
                if (ImGui::TreeNode("Why recovery did or did not fire"))
                {
                    DrawTrainingRecoveryDebug(status);
                    ImGui::TreePop();
                }
            }
        }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(qi18n::tr("Tools")))
        {
            DrawTrainingDebugTools(status);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(qi18n::tr("Palettes")))
        {
            DrawPaletteTab(status);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(qi18n::tr("Bridge")))
        {
            TrainingTextDisabledWrapped(
                "Raw values to paste into a bug report. Nothing here is a "
                "setting.");
            if (ImGui::TreeNode("Training"))
            {
                DrawTrainingDetails(status);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Dummy##bridge_dummy"))
            {
                DrawDummyDetails(status);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Fighters"))
            {
                DrawTrainingFighterReadout(status);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Damage##bridge_damage"))
            {
                DrawTrainingDamageReadout(status);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Idle test"))
            {
                DrawTrainingIdleTestRawBlock(status);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Action state log"))
            {
                DrawTrainingActionStateLog(status);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Input##bridge_input"))
            {
                DrawInputDetails(status);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Save states##bridge_state"))
            {
                DrawSaveStateDetails(status);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Overlay device"))
            {
                DrawWindowDetails(params);
                ImGui::TreePop();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    else
    {
        {
            DrawOverlayStatusSummary(params);

            if (status.nativeHudOwner != 0u)
            {
                DrawNativeHudControls(status);
            }

            ImGui::Spacing();
            if (ImGui::TreeNode("Overlay device"))
            {
                DrawWindowDetails(params);
                ImGui::TreePop();
            }
        }
    }

}

void DrawTrainingHelpTab()
{
    if (BeginTabBody("training_help"))
    {
        ImGui::TextWrapped(
            "Training controls are grouped by task. Pick a page on the left, "
            "make the change, then close the menu to resume.");

        ImGui::SeparatorText(qi18n::tr("The pages"));
        TrainingBulletTextWrapped(
            "Quick - place the fighters, pause, step one frame, and refill or "
            "set health, EX and guard gauge.");
        TrainingBulletTextWrapped(
            "Dummy - how it stands and guards, and how it techs out of air, "
            "ground and wall knockdowns.");
        TrainingBulletTextWrapped(
            "Triggers - what the dummy does after a hit, block, knockdown, "
            "tech or throw: a recording or one of its own moves.");
        TrainingBulletTextWrapped(
            "Recording - record and replay dummy input, and save or restore "
            "the current setup.");
        TrainingBulletTextWrapped(
            "View - frame advantage, hitboxes, the input viewer, the HUD and "
            "display options.");
        TrainingBulletTextWrapped("Keys - every Training and wrapper action.");

        ImGui::SeparatorText(qi18n::tr("Important"));
        TrainingBulletTextWrapped(
            "Opening this menu holds your input; your own pause is preserved.");
        TrainingBulletTextWrapped(
            "Recordings and save states last only for this Training session.");
        TrainingBulletTextWrapped(
            "Bindings are changed in the launcher's Controls page.");
        TrainingBulletTextWrapped(
            "Rejected actions and their reason appear at the top of the "
            "window, on every page.");
        TrainingBulletTextWrapped(
            "Advanced, under the page list, adds a Diagnostics page with the "
            "engineering readouts. Nothing is disabled by leaving it off.");
    }
    EndTabBody();
}

void DrawNetplayKeysTab()
{
    ImGui::SeparatorText(qi18n::tr("Wrapper controls"));
    TrainingBulletTextWrapped("Overlay menu - open or close this menu");
    TrainingBulletTextWrapped(
        "Presentation mode - windowed or preferred fullscreen");
    TrainingBulletTextWrapped("Input viewer - show or hide the input viewer");
    TrainingTextDisabledWrapped(
        "These global bindings are configured on the launcher's Controls page "
        "and are handled by the wrapper rather than the game.");

    ImGui::SeparatorText(qi18n::tr("While a replay is playing"));
    TrainingBulletTextWrapped("Replay pause - pause or resume");
    TrainingBulletTextWrapped("Replay step - advance one frame");
    TrainingBulletTextWrapped("Replay previous frame - go back one frame and pause");
    TrainingBulletTextWrapped("Replay restart - return to the beginning and pause");
    TrainingBulletTextWrapped(
        "Replay slower / faster - change playback speed");
    TrainingBulletTextWrapped(
        "Replay boxes - show or hide attack, hurt, and collision boxes");
    TrainingTextDisabledWrapped(
        "These reach the replay player directly and keep working even if this "
        "overlay cannot draw. The launcher's Controls page lists them too, and "
        "that list is the authoritative one.");

    Qoh99ReplayTransportStatusV2 status{};
    if (ReplayTransportPoll(status))
    {
        ImGui::SeparatorText(qi18n::tr("Replay controls"));
        ImGui::BeginDisabled(status.mayHold == 0u);
        if (ImGui::Button(status.paused != 0u ? "Resume" : "Pause"))
        {
            ReplayTransportSubmit(
                status, QOH99_REPLAY_TRANSPORT_TOGGLE_PAUSE);
        }
        ImGui::SameLine();
        if (ImGui::Button("Frame step"))
        {
            ReplayTransportSubmit(status, QOH99_REPLAY_TRANSPORT_STEP);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Previous frame"))
        {
            ReplayTransportSubmit(status, QOH99_REPLAY_TRANSPORT_STEP_BACK);
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart replay"))
        {
            ReplayTransportSubmit(status, QOH99_REPLAY_TRANSPORT_RESTART);
        }
        if (ImGui::Button("Toggle collision boxes"))
        {
            ReplayTransportSubmit(
                status, QOH99_REPLAY_TOGGLE_COLLISION_DISPLAY);
        }
        TrainingTextDisabledWrapped(
            "Frame step advances exactly one replay frame while paused. "
            "Previous frame and Restart replay return to a paused frame. "
            "Hitboxes use our custom overlay, including secondary collision "
            "and projectile boxes.");
    }
}

void DrawNetplayHelpTab()
{
    ImGui::TextWrapped(
        "This is a presentation-only overlay. It draws on top of the game and "
        "never influences the match: nothing on these tabs can reach the "
        "simulation.");
    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("What is here"));
    TrainingBulletTextWrapped(
        "View > Overlay places the online name plates and the ping line.");
    TrainingBulletTextWrapped(
        "View > Input viewer switches the input strip on and off, in replays "
        "as well as in training.");
    TrainingBulletTextWrapped(
        "View > Screen holds smooth scaling and borderless fullscreen.");
    TrainingBulletTextWrapped(
        "Diagnostics shows whether the overlay and its nickname fonts are ready.");
    ImGui::Spacing();
    ImGui::SeparatorText(qi18n::tr("What is not here"));
    ImGui::TextWrapped(
        "Everything configurable lives in the launcher: Player and files, "
        "Connection, Delay and rollback, Audio, In-game display and keys, Game "
        "config, Extra functions, Let people watch your matches, Replays and "
        "storage, and the Controls page.");
    TrainingTextDisabledWrapped(
        "The practice controls - dummy, recording, save states, frame step - "
        "only appear when the game was launched into training.");
}

void DrawTrainingHotkeysTab(const Qoh99TrainingStatusV1 &status)
{
    (void)status;
    ImGui::SeparatorText(qi18n::tr("Wrapper"));
    TrainingBulletTextWrapped("Overlay menu - open or close this menu");
    TrainingBulletTextWrapped(
        "Presentation mode - windowed or preferred fullscreen");
    TrainingBulletTextWrapped("Input viewer - show or hide the input viewer");

    ImGui::SeparatorText(qi18n::tr("Training"));
    TrainingBulletTextWrapped("Return to character select");
    TrainingBulletTextWrapped("Reset - reset to round start");
    TrainingBulletTextWrapped(
        "Left + Reset - reset to the left corner, touching");
    TrainingBulletTextWrapped(
        "Right + Reset - reset to the right corner, touching");
    TrainingBulletTextWrapped(
        "Down + Reset - reset to midscreen, touching");
    TrainingBulletTextWrapped(
        "Up + Reset - reset to midscreen, touching");
    TrainingBulletTextWrapped(
        "Round-start reset - also ask for the idle state");
    TrainingBulletTextWrapped("Save state / Load state - current slot");
    TrainingBulletTextWrapped("Record / Play - start or stop");
    TrainingBulletTextWrapped("Previous slot / Next slot");
    TrainingBulletTextWrapped("Pause - pause or resume the match");
    TrainingBulletTextWrapped(
        "Frame step - advance exactly one frame (and pause)");
    TrainingBulletTextWrapped(
        "Restore all - toggles the automatic restore that keeps health, EX "
        "meter and guard gauge full while both characters are idle");
    TrainingBulletTextWrapped(
        "Charge EX fraction - hold to fill continuously and add stocks");
    TrainingBulletTextWrapped("Toggle hitboxes - show or hide hitboxes");
    TrainingBulletTextWrapped(
        "Toggle debug overlay - show or hide live action, wake-up, and throw details");
    TrainingBulletTextWrapped(
        "Cycle dummy stance - stand, crouch, jump");
    TrainingBulletTextWrapped(
        "Cycle guard mode - next dummy guard setting");

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Every binding above comes from the launcher's Controls page. Edge "
        "actions fire once per press; fractional charge repeats while held. "
        "Fight controls are unchanged.");
}

void DrawPersistentTrainingFrameStep(const Qoh99TrainingStatusV1 &status)
{
    const char *const label = "Step 1 frame";
    const ImGuiStyle &style = ImGui::GetStyle();
    const float buttonWidth = ImGui::CalcTextSize(label).x
        + style.FramePadding.x * 2.0f;
    const float rightEdge = ImGui::GetWindowContentRegionMax().x;
    const float targetX = rightEdge - buttonWidth;
    if (targetX > ImGui::GetCursorPosX())
    {
        ImGui::SetCursorPosX(targetX);
    }

    const bool ready = TrainingHasRecoveryStatus(status)
        && TrainingFeatureReady(status, QOH99_TRAINING_CAP_RESET);
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button(label))
    {
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_FRAME_STEP, 0, 0);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            ready
                ? "Runs exactly one frame while keeping this menu open. "
                  "Closing the menu resumes unless you paused it yourself."
                : "Frame stepping is not available on this screen yet.");
    }
}

enum TrainingMenuPage
{
    kTrainingPageQuick = 0,
    kTrainingPageDummy,
    kTrainingPageReactions,
    kTrainingPageRecording,
    kTrainingPageView,
    kTrainingPagePalette,
    kTrainingPageKeys,
    kTrainingPageHelp,
    kTrainingPageDiagnostics,
    kTrainingPageCount
};

struct TrainingMenuPageInfo
{
    TrainingMenuPage page;
    const char *label;
    const char *hint;
    bool needsTraining;
    bool advancedOnly;
};

const TrainingMenuPageInfo kTrainingMenuPages[] = {
    { kTrainingPageQuick, "Quick",
      "Position, pause, health, meter and guard gauge.", true, false },
    { kTrainingPageDummy, "Dummy",
      "How the dummy stands, guards and techs.", true, false },
    { kTrainingPageReactions, "Triggers",
      "What the dummy does after a hit, block, knockdown or throw.",
      true, false },
    { kTrainingPageRecording, "Recording",
      "Record and replay dummy inputs; save and load states.", true, false },
    { kTrainingPageView, "View",
      "Overlays, hitboxes, input viewer and window size.", false, false },
    { kTrainingPagePalette, "Palette",
      "Edit each fighter's four palettes; save and load .pal files.",
      false, false },
    { kTrainingPageKeys, "Keys", "What every key does.", false, false },
    { kTrainingPageHelp, "Help", "How to use this menu.", false, false },
    { kTrainingPageDiagnostics, "Diagnostics",
      "Engineering readouts, state log and the debug panel.",
      false, true },
};

void TrainingMenuSetNativeStyle(bool nativeStyle);

bool TrainingMenuPageVisible(const TrainingMenuPageInfo &info,
                             bool trainingPresent, bool advanced)
{
    if (info.needsTraining && !trainingPresent) return false;
    if (info.advancedOnly && !advanced) return false;
    return true;
}

TrainingMenuPage DrawTrainingMenuSidebar(bool trainingPresent, bool &advanced)
{
    static bool loaded = false;
    static int selected = kTrainingPageQuick;
    static bool advancedEnabled = false;
    if (!loaded)
    {
        loaded = true;
        selected = OverlayIniGet("MenuPage", kTrainingPageQuick);
        if (selected < 0 || selected >= kTrainingPageCount)
        {
            selected = kTrainingPageQuick;
        }
        advancedEnabled = OverlayIniGet("MenuAdvanced", 0) != 0;
    }
    advanced = advancedEnabled;

    const float width = ImGui::GetFontSize() * 8.0f;
    ImGui::BeginChild("qoh99_menu_nav", ImVec2(width, 0.0f),
                      ImGuiChildFlags_Borders);
    for (const TrainingMenuPageInfo &info : kTrainingMenuPages)
    {
        if (!TrainingMenuPageVisible(info, trainingPresent, advanced)) continue;
        if (ImGui::Selectable(qi18n::tr(info.label), selected == info.page))
        {
            selected = info.page;
            OverlayIniSet("MenuPage", selected);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", info.hint);
        }
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Advanced", &advancedEnabled))
    {
        advanced = advancedEnabled;
        OverlayIniSet("MenuAdvanced", advancedEnabled ? 1 : 0);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Shows the Diagnostics page and the raw engineering readouts.\n"
            "Nothing is disabled by leaving this off.");
    }

    if (trainingPresent)
    {
        if (ImGui::Button("In-game menu"))
        {
            TrainingMenuSetNativeStyle(true);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Switch back to the in-game training menu.\n"
                "Its Diagnostics page brings you back here.");
        }
    }
    ImGui::EndChild();

    for (const TrainingMenuPageInfo &info : kTrainingMenuPages)
    {
        if (info.page != selected) continue;
        if (TrainingMenuPageVisible(info, trainingPresent, advanced))
        {
            return static_cast<TrainingMenuPage>(selected);
        }
        break;
    }
    return trainingPresent ? kTrainingPageQuick : kTrainingPageView;
}

bool &TrainingMenuNativeStyleRef()
{
    static bool loaded = false;
    static bool nativeStyle = true;
    if (!loaded)
    {
        loaded = true;
        nativeStyle = OverlayIniGet("MenuStyle", 1) != 0;
    }
    return nativeStyle;
}

bool TrainingMenuUseNativeStyle()
{
    return TrainingMenuNativeStyleRef();
}

void TrainingMenuSetNativeStyle(bool nativeStyle)
{
    TrainingMenuNativeStyleRef() = nativeStyle;
    OverlayIniSet("MenuStyle", nativeStyle ? 1 : 0);
}

namespace NativeMenuStyle
{
constexpr ImU32 kScrim       = IM_COL32(0, 0, 0, 130);
constexpr ImU32 kPanel       = IM_COL32(12, 14, 28, 226);
constexpr ImU32 kBorder      = IM_COL32(232, 232, 240, 255);
constexpr ImU32 kBorderInner = IM_COL32(70, 76, 110, 255);
constexpr ImU32 kText        = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kTextDim     = IM_COL32(150, 154, 176, 255);
constexpr ImU32 kTextValue   = IM_COL32(255, 232, 140, 255);
constexpr ImU32 kCursor      = IM_COL32(255, 232, 140, 255);
constexpr ImU32 kRowActive   = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kTabActive   = IM_COL32(255, 232, 140, 255);
constexpr ImU32 kTabIdle     = IM_COL32(150, 154, 176, 255);
constexpr ImU32 kWarn        = IM_COL32(255, 150, 90, 255);
}

enum NativeRowKind
{
    kNativeRowHeading = 0,
    kNativeRowInfo,
    kNativeRowToggle,
    kNativeRowChoice,
    kNativeRowAction,
    kNativeRowSpacer,
    kNativeRowSubTabs,
};

struct NativeRow
{
    NativeRowKind kind;
    const char *label;
    const char *hint;
    bool enabled;
    int value;
    const char *const *choices;
    int choiceCount;
    const int *values;
    bool choiceIsSlot;
    const char *const *choiceHints;
    const char *valueText;
    DWORD command;
    DWORD slot;
    DWORD actionValue;
};

constexpr int kNativeMenuMaxRows = 160;

struct NativeMenuPageBuild
{
    NativeRow rows[kNativeMenuMaxRows];
    char owned[kNativeMenuMaxRows][40];
    char ownedLabel[kNativeMenuMaxRows][40];
    int count = 0;
    int dropped = 0;
    int preferredRow = -1;
    bool columnar = false;

    NativeRow *push(NativeRowKind kind, const char *label)
    {
        if (count >= kNativeMenuMaxRows)
        {
            ++dropped;
            return nullptr;
        }
        NativeRow &row = rows[count++];
        row = NativeRow();
        row.kind = kind;
        row.label = label;
        row.enabled = true;
        return &row;
    }

    void heading(const char *label) { push(kNativeRowHeading, label); }
    void spacer() { push(kNativeRowSpacer, ""); }

    void info(const char *label, const char *value)
    {
        NativeRow *row = push(kNativeRowInfo, label);
        if (row == nullptr) return;
        row->valueText = value;
    }

    void toggle(const char *label, bool on, bool enabled, DWORD command,
                DWORD slotIndex, const char *hint)
    {
        NativeRow *row = push(kNativeRowToggle, label);
        if (row == nullptr) return;
        row->value = on ? 1 : 0;
        row->enabled = enabled;
        row->command = command;
        row->slot = slotIndex;
        row->hint = hint;
    }

    void choice(const char *label, int value, const char *const *choices,
                int choiceCount, bool enabled, DWORD command, DWORD slotIndex,
                const char *hint,
                const char *const *choiceHints = nullptr)
    {
        NativeRow *row = push(kNativeRowChoice, label);
        if (row == nullptr) return;
        row->value = value;
        row->choices = choices;
        row->choiceHints = choiceHints;
        row->choiceCount = choiceCount;
        row->enabled = enabled;
        row->command = command;
        row->slot = slotIndex;
        row->hint = hint;
        row->choiceIsSlot = false;
    }

    void choiceSlot(const char *label, int value, const char *const *choices,
                    int choiceCount, bool enabled, DWORD command,
                    const char *hint)
    {
        const int before = count;
        choice(label, value, choices, choiceCount, enabled, command, 0, hint);
        if (count > before) rows[count - 1].choiceIsSlot = true;
    }

    void choiceMapped(const char *label, int current,
                      const char *const *choices, const int *values,
                      int choiceCount, bool enabled, DWORD command,
                      DWORD slotIndex, const char *hint,
                      const char *const *choiceHints = nullptr)
    {
        int index = -1;
        for (int i = 0; i < choiceCount; ++i)
        {
            if (values[i] == current) { index = i; break; }
        }
        NativeRow *row = push(kNativeRowChoice, label);
        if (row == nullptr) return;
        row->value = index;
        row->choices = choices;
        row->choiceHints = choiceHints;
        row->values = values;
        row->choiceCount = choiceCount;
        row->enabled = enabled;
        row->command = command;
        row->slot = slotIndex;
        row->hint = hint;
    }

    void action(const char *label, bool enabled, DWORD command,
                DWORD slotIndex, DWORD actionValue, const char *hint)
    {
        NativeRow *row = push(kNativeRowAction, label);
        if (row == nullptr) return;
        row->enabled = enabled;
        row->command = command;
        row->slot = slotIndex;
        row->actionValue = actionValue;
        row->hint = hint;
    }

    void submenu(const char *label, const char *summary, bool enabled,
                 DWORD tag, const char *hint)
    {
        const int index = count;
        NativeRow *row = push(kNativeRowAction, label);
        if (row == nullptr) return;
        std::snprintf(ownedLabel[index], sizeof(ownedLabel[index]), "%s",
                      label != nullptr ? label : "");
        row->label = ownedLabel[index];
        row->enabled = enabled;
        row->command = QOH99_TRAINING_COMMAND_NONE;
        row->slot = tag;
        row->hint = hint;
        std::snprintf(owned[index], sizeof(owned[index]), "%s",
                      summary != nullptr ? summary : "");
        row->valueText = owned[index];
    }
};

bool NativeRowSelectable(const NativeRow &row)
{
    return row.kind == kNativeRowToggle || row.kind == kNativeRowChoice
        || row.kind == kNativeRowAction || row.kind == kNativeRowSubTabs;
}

struct NativeMenuState
{
    bool loaded = false;
    int page = kTrainingPageQuick;
    int row = 0;
    bool advanced = false;
    int lastRowCount = 0;
    int scroll = 0;
    bool tabFocus = false;
    int submenu = -1;
    bool submenuChild = false;
    bool focusPreferred = false;
    bool mouseEngaged = false;
    float lastMouseX = -1.0e30f;
    float lastMouseY = -1.0e30f;
    int subTab[kTrainingPageCount] = {};
};

NativeMenuState g_nativeMenu;

constexpr int kNativeMenuMaxSubTabs = 12;
float g_nativeMenuSubTabHitX[kNativeMenuMaxSubTabs][2] = {};
int g_nativeMenuSubTabHitCount = 0;

void NativeMenuEnsureLoaded()
{
    if (g_nativeMenu.loaded) return;
    g_nativeMenu.loaded = true;
    g_nativeMenu.page = OverlayIniGet("MenuPage", kTrainingPageQuick);
    for (int page = 0; page < kTrainingPageCount; ++page)
    {
        char key[24];
        std::snprintf(key, sizeof(key), "MenuSubTab%d", page);
        g_nativeMenu.subTab[page] = OverlayIniGet(key, 0);
    }
    if (g_nativeMenu.page < 0 || g_nativeMenu.page >= kTrainingPageCount)
    {
        g_nativeMenu.page = kTrainingPageQuick;
    }
    g_nativeMenu.advanced = OverlayIniGet("MenuAdvanced", 0) != 0;
}

struct NativeMenuInput
{
    bool up;
    bool down;
    bool left;
    bool right;
    bool commit;
    bool back;
    bool pagePrev;
    bool pageNext;
};

NativeMenuInput NativeMenuReadInput()
{
    struct KeySlot
    {
        bool down;
    };
    static KeySlot slots[16] = {};
    const ULONGLONG nowMs = GetTickCount64();

    NativeMenuInput idle = {};

    if (!g_presenter.menuVisible) return idle;
    if (g_presenter.window != nullptr
        && GetForegroundWindow() != g_presenter.window)
    {
        return idle;
    }

    constexpr ULONGLONG kPollIntervalMs = 8ull;
    static ULONGLONG lastPollMs = 0ull;
    if (nowMs - lastPollMs < kPollIntervalMs) return idle;
    lastPollMs = nowMs;

    const auto pressed = [&](int slot, int vk) {
        const SHORT raw = GetAsyncKeyState(vk);
        const bool down = (raw & 0x8000) != 0;
        const bool tappedSincePoll = (raw & 0x0001) != 0;
        KeySlot &state = slots[slot];
        if (!down)
        {
            state.down = false;
            return tappedSincePoll;
        }
        if (state.down) return false;
        state.down = true;
        return true;
    };

    NativeMenuInput in;
    in.up = pressed(0, VK_UP);
    in.down = pressed(1, VK_DOWN);
    in.left = pressed(2, VK_LEFT);
    in.right = pressed(3, VK_RIGHT);
    in.commit = pressed(4, VK_RETURN)
        | pressed(5, VK_SPACE);
    in.back = pressed(6, VK_BACK);
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool tab = pressed(7, VK_TAB);
    in.pagePrev = pressed(8, 'Q')
        | pressed(9, VK_PRIOR)
        | (tab && shift);
    in.pageNext = pressed(10, 'E')
        | pressed(11, VK_NEXT)
        | (tab && !shift);
    return in;
}

int NativeMenuAdvanceRow(const NativeMenuPageBuild &build, int from, int step)
{
    if (build.count <= 0) return 0;
    int index = from;
    for (int guard = 0; guard < build.count; ++guard)
    {
        index += step;
        if (index < 0) index = build.count - 1;
        if (index >= build.count) index = 0;
        if (NativeRowSelectable(build.rows[index])) return index;
    }
    return from;
}

int NativeMenuFirstSelectable(const NativeMenuPageBuild &build)
{
    for (int i = 0; i < build.count; ++i)
    {
        if (NativeRowSelectable(build.rows[i])) return i;
    }
    return 0;
}

int NativeMenuLastSelectable(const NativeMenuPageBuild &build)
{
    for (int i = build.count - 1; i >= 0; --i)
    {
        if (NativeRowSelectable(build.rows[i])) return i;
    }
    return 0;
}

struct NativeWrappedText
{
    enum { kMaxLines = 32 };
    const char *begin[kMaxLines];
    const char *end[kMaxLines];
    int count;
    bool overflowed;
};

void NativeMenuWrapText(ImFont *font, float size, const char *text,
                        float wrapWidth, NativeWrappedText &out)
{
    out.count = 0;
    out.overflowed = false;
    if (font == nullptr || text == nullptr || text[0] == '\0'
        || wrapWidth <= 0.0f)
    {
        return;
    }
    const char *cursor = text;
    const char *end = text + std::strlen(text);
    while (cursor < end)
    {
        if (out.count >= NativeWrappedText::kMaxLines)
        {
            out.overflowed = true;
            break;
        }
        const char *lineEnd = font->CalcWordWrapPositionA(
            size / font->FontSize, cursor, end, wrapWidth);
        if (lineEnd <= cursor) lineEnd = cursor + 1;
        out.begin[out.count] = cursor;
        out.end[out.count] = lineEnd;
        ++out.count;
        cursor = lineEnd;
        while (cursor < end && *cursor == ' ') ++cursor;
    }
}

struct NativeTextScroll
{
    unsigned key;
    ULONGLONG since;
};

const ULONGLONG kNativeTextScrollHoldMs = 1100;
const ULONGLONG kNativeTextScrollStepMs = 550;

unsigned NativeTextScrollKey(const char *text, float wrapWidth, int maxLines)
{
    unsigned hash = 2166136261u;
    for (const char *c = text; c != nullptr && *c != '\0'; ++c)
    {
        hash = (hash ^ static_cast<unsigned>(
                           static_cast<unsigned char>(*c))) * 16777619u;
    }
    hash = (hash ^ static_cast<unsigned>(wrapWidth * 4.0f)) * 16777619u;
    return (hash ^ static_cast<unsigned>(maxLines)) * 16777619u;
}

int NativeMenuTextWrapped(ImDrawList *dl, ImFont *font, float size,
                          ImVec2 pos, float wrapWidth, ImU32 color,
                          const char *text, int maxLines,
                          NativeTextScroll *scroll = nullptr)
{
    if (dl == nullptr || font == nullptr || text == nullptr
        || text[0] == '\0' || maxLines < 1 || wrapWidth <= 0.0f)
    {
        return 0;
    }
    const float lineStep = size * 1.18f;
    NativeWrappedText wrapped;
    NativeMenuWrapText(font, size, text, wrapWidth, wrapped);
    if (wrapped.count == 0) return 0;

    auto drawLine = [&](int line, float y, float width, bool clip) {
        if (clip)
        {
            const std::string fitted =
                ClipTextToWidth(font, size, wrapped.begin[line], width);
            if (!fitted.empty())
            {
                dl->AddText(font, size,
                            ImVec2(std::floor(pos.x), std::floor(y)),
                            color, fitted.c_str());
            }
            return;
        }
        dl->AddText(font, size, ImVec2(std::floor(pos.x), std::floor(y)),
                    color, wrapped.begin[line], wrapped.end[line]);
    };

    if (scroll == nullptr || wrapped.count <= maxLines)
    {
        const int drawn = wrapped.count < maxLines ? wrapped.count : maxLines;
        for (int i = 0; i < drawn; ++i)
        {
            const bool clip = i == maxLines - 1
                && (wrapped.count > maxLines || wrapped.overflowed);
            drawLine(i, pos.y + lineStep * static_cast<float>(i),
                     wrapWidth, clip);
        }
        return drawn;
    }

    const float markWidth = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "^").x;
    const float narrowed = wrapWidth - markWidth - size * 0.25f;
    const float textWidth = narrowed > size * 4.0f ? narrowed : wrapWidth;
    if (textWidth != wrapWidth)
    {
        NativeMenuWrapText(font, size, text, textWidth, wrapped);
    }

    const int maxOffset = wrapped.count - maxLines;
    const unsigned key = NativeTextScrollKey(text, wrapWidth, maxLines);
    const ULONGLONG now = GetTickCount64();
    if (scroll->key != key || scroll->since == 0)
    {
        scroll->key = key;
        scroll->since = now;
    }
    const ULONGLONG cycle = kNativeTextScrollHoldMs * 2ull
        + kNativeTextScrollStepMs * static_cast<ULONGLONG>(maxOffset);
    ULONGLONG phase = (now - scroll->since) % cycle;
    int offset = 0;
    if (phase >= kNativeTextScrollHoldMs)
    {
        phase -= kNativeTextScrollHoldMs;
        const ULONGLONG steps = phase / kNativeTextScrollStepMs;
        offset = steps >= static_cast<ULONGLONG>(maxOffset)
            ? maxOffset
            : static_cast<int>(steps) + 1;
    }

    for (int i = 0; i < maxLines; ++i)
    {
        const int line = offset + i;
        const bool clip = wrapped.overflowed && line == wrapped.count - 1;
        drawLine(line, pos.y + lineStep * static_cast<float>(i),
                 textWidth, clip);
    }
    const float markX = pos.x + wrapWidth - markWidth;
    if (offset > 0)
    {
        dl->AddText(font, size,
                    ImVec2(std::floor(markX), std::floor(pos.y)), color, "^");
    }
    if (offset < maxOffset)
    {
        dl->AddText(font, size,
                    ImVec2(std::floor(markX),
                           std::floor(pos.y + lineStep
                                      * static_cast<float>(maxLines - 1))),
                    color, "v");
    }
    return maxLines;
}

void NativeMenuText(ImDrawList *dl, ImFont *font, float size, ImVec2 pos,
                    ImU32 color, const char *text)
{
    dl->AddText(font, size, ImVec2(std::floor(pos.x), std::floor(pos.y)), color,
                text);
}

float NativeMenuTextWidth(ImFont *font, float size, const char *text)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

void NativeMenuCursor(ImDrawList *dl, ImVec2 origin, float size, ImU32 color)
{
    const float h = std::floor(size * 0.62f);
    const float w = std::floor(size * 0.46f);
    const float y = std::floor(origin.y + (size - h) * 0.5f);
    const float x = std::floor(origin.x);
    dl->AddTriangleFilled(ImVec2(x, y), ImVec2(x + w, y + h * 0.5f),
                          ImVec2(x, y + h), color);
}

struct NativeMenuMetrics
{
    ImVec2 panelMin;
    ImVec2 panelMax;
    float fontSize;
    float lineHeight;
    float pad;
    int tabLines;
    float tabGap;
};

constexpr int kNativeMenuMinBodyRows = 8;

int NativeMenuTabLineCount(ImFont *font, float fontSize, float width,
                           float gap, const int *pages, int pageCount)
{
    int lines = 1;
    float x = 0.0f;
    for (int i = 0; i < pageCount; ++i)
    {
        const float w =
            font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f,
                                qi18n::tr(
                                    kTrainingMenuPages[pages[i]].label)).x;
        if (w > width) return -1;
        if (x > 0.0f && x + w > width)
        {
            ++lines;
            x = 0.0f;
        }
        x += w + gap;
    }
    return lines;
}

NativeMenuMetrics NativeMenuComputeMetrics(const EfzDx9PresentParams &params,
                                           ImFont *font, const int *pages,
                                           int pageCount)
{
    NativeMenuMetrics m;
    const float frameW = static_cast<float>(params.dstW > 0 ? params.dstW : 640);
    const float frameH = static_cast<float>(params.dstH > 0 ? params.dstH : 480);
    const float frameX = static_cast<float>(params.dstX);
    const float frameY = static_cast<float>(params.dstY);

    const float insetX = std::floor(frameW * 0.05f);
    const float insetTop = std::floor(frameH * 0.10f);
    const float insetBottom = std::floor(frameH * 0.045f);
    m.panelMin = ImVec2(frameX + insetX, frameY + insetTop);
    m.panelMax = ImVec2(frameX + frameW - insetX, frameY + frameH - insetBottom);

    float scale = std::floor(frameH / 240.0f);
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 6.0f) scale = 6.0f;

    for (;;)
    {
        m.fontSize = 13.0f * scale;
        m.lineHeight = std::floor(m.fontSize * 1.22f);
        m.pad = std::floor(4.0f * scale);
        m.tabGap = std::floor(m.fontSize * 0.7f);

        const float contentW =
            (m.panelMax.x - m.pad * 2.0f) - (m.panelMin.x + m.pad * 2.0f);
        const float panelH = m.panelMax.y - m.panelMin.y;

        m.tabLines = NativeMenuTabLineCount(font, m.fontSize, contentW,
                                            m.tabGap, pages, pageCount);
        if (m.tabLines > 0 && contentW > 0.0f)
        {
            const float chrome =
                m.pad * 2.0f
                + m.lineHeight * (1.0f + static_cast<float>(m.tabLines))
                + m.pad
                + m.lineHeight * 2.0f;
            const float body = panelH - chrome;
            const float minBodyRows =
                g_nativeMenu.page == kTrainingPagePalette
                    ? 21.0f
                    : static_cast<float>(kNativeMenuMinBodyRows);
            if (m.tabLines == 1 && body >= m.lineHeight * minBodyRows)
            {
                break;
            }
        }
        if (scale <= 1.0f)
        {
            if (m.tabLines <= 0) m.tabLines = 1;
            break;
        }
        scale -= 1.0f;
    }
    return m;
}

void NativeMenuDrawTabs(ImDrawList *dl, ImFont *font,
                        const NativeMenuMetrics &m, ImVec2 origin,
                        float width, const int *pages, int pageCount,
                        int activePage, bool focused, int &hoveredPage)
{
    hoveredPage = -1;
    if (pageCount <= 0) return;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float x = origin.x;
    float y = origin.y;
    for (int i = 0; i < pageCount; ++i)
    {
        const char *label = qi18n::tr(kTrainingMenuPages[pages[i]].label);
        const float w =
            font->CalcTextSizeA(m.fontSize, FLT_MAX, 0.0f, label).x;
        if (x > origin.x && x + w > origin.x + width)
        {
            x = origin.x;
            y += m.lineHeight;
        }
        const bool active = pages[i] == activePage;
        const ImVec2 tabMin(x, y);
        const ImVec2 tabMax(x + w, y + m.fontSize);
        if (mouse.x >= tabMin.x && mouse.x <= tabMax.x
            && mouse.y >= tabMin.y - 2.0f && mouse.y <= tabMax.y + 2.0f)
        {
            hoveredPage = pages[i];
        }
        if (active && focused)
        {
            dl->AddRectFilled(
                ImVec2(tabMin.x - m.tabGap * 0.35f, tabMin.y - 1.0f),
                ImVec2(tabMax.x + m.tabGap * 0.35f, tabMax.y + 1.0f),
                NativeMenuStyle::kRowActive);
        }
        NativeMenuText(dl, font, m.fontSize, tabMin,
                       active ? NativeMenuStyle::kTabActive
                              : NativeMenuStyle::kTabIdle,
                       label);
        if (active)
        {
            const float uy = std::floor(tabMax.y + 2.0f);
            dl->AddLine(ImVec2(tabMin.x, uy), ImVec2(tabMax.x, uy),
                        NativeMenuStyle::kTabActive, 1.0f);
        }
        if (active && focused && pageCount > 1)
        {
            const float arrowGap = m.tabGap * 0.35f;
            const float aw =
                font->CalcTextSizeA(m.fontSize, FLT_MAX, 0.0f, "<").x;
            NativeMenuText(dl, font, m.fontSize,
                           ImVec2(tabMin.x - arrowGap - aw, tabMin.y),
                           NativeMenuStyle::kCursor, "<");
            NativeMenuText(dl, font, m.fontSize,
                           ImVec2(tabMax.x + arrowGap, tabMin.y),
                           NativeMenuStyle::kCursor, ">");
        }
        x += w + m.tabGap;
    }
}

void NativeMenuDrawRow(ImDrawList *dl, ImFont *font,
                       const NativeMenuMetrics &m, const NativeRow &row,
                       ImVec2 origin, float width, bool selected)
{
    const float indent = std::floor(m.fontSize * 1.1f);
    switch (row.kind)
    {
    case kNativeRowSpacer:
        return;
    case kNativeRowHeading:
    {
        NativeMenuText(dl, font, m.fontSize, origin,
                       NativeMenuStyle::kTextDim, qi18n::tr(row.label));
        const float w = NativeMenuTextWidth(font, m.fontSize, row.label);
        const float uy = std::floor(origin.y + m.fontSize + 1.0f);
        dl->AddLine(ImVec2(origin.x, uy), ImVec2(origin.x + width, uy),
                    NativeMenuStyle::kBorderInner, 1.0f);
        (void)w;
        return;
    }
    case kNativeRowSubTabs:
    {
        const float pillPadX = m.fontSize * 0.55f;
        const float pillTop = origin.y - 2.0f;
        const float pillBottom = origin.y + m.fontSize + 2.0f;
        const float rounding = 0.0f;
        if (selected)
        {
            NativeMenuCursor(dl, ImVec2(origin.x, origin.y), m.fontSize,
                             NativeMenuStyle::kCursor);
        }
        float x = origin.x + indent + pillPadX;
        g_nativeMenuSubTabHitCount = 0;
        for (int i = 0; i < row.choiceCount
                        && i < kNativeMenuMaxSubTabs; ++i)
        {
            const bool activeTab = i == row.value;
            const float w =
                NativeMenuTextWidth(font, m.fontSize, row.choices[i]);
            const ImVec2 a(x - pillPadX, pillTop);
            const ImVec2 b(x + w + pillPadX, pillBottom);
            if (activeTab)
            {
                dl->AddRectFilled(a, b, NativeMenuStyle::kTabActive,
                                  rounding);
                if (selected)
                {
                    dl->AddRect(ImVec2(a.x - 2.0f, a.y - 2.0f),
                                ImVec2(b.x + 2.0f, b.y + 2.0f),
                                NativeMenuStyle::kCursor, rounding, 0,
                                1.0f);
                }
                NativeMenuText(dl, font, m.fontSize, ImVec2(x, origin.y),
                               NativeMenuStyle::kPanel, row.choices[i]);
            }
            else
            {
                dl->AddRect(a, b, NativeMenuStyle::kBorderInner, rounding,
                            0, 1.0f);
                NativeMenuText(dl, font, m.fontSize, ImVec2(x, origin.y),
                               NativeMenuStyle::kTabIdle, row.choices[i]);
            }
            g_nativeMenuSubTabHitX[i][0] = a.x;
            g_nativeMenuSubTabHitX[i][1] = b.x;
            ++g_nativeMenuSubTabHitCount;
            x = b.x + m.fontSize * 0.7f + pillPadX;
        }
        const float ruleY = std::floor(pillBottom + 3.0f);
        dl->AddLine(ImVec2(origin.x, ruleY),
                    ImVec2(origin.x + width, ruleY),
                    NativeMenuStyle::kBorderInner, 1.0f);
        return;
    }
    default:
        break;
    }

    if (selected)
    {
        dl->AddRectFilled(
            ImVec2(origin.x - m.pad * 0.5f, origin.y - 1.0f),
            ImVec2(origin.x + width + m.pad * 0.5f, origin.y + m.fontSize + 1.0f),
            NativeMenuStyle::kRowActive);
        NativeMenuCursor(dl, ImVec2(origin.x, origin.y), m.fontSize,
                         NativeMenuStyle::kCursor);
    }

    const ImU32 labelColor = row.enabled ? NativeMenuStyle::kText
                                         : NativeMenuStyle::kTextDim;
    NativeMenuText(dl, font, m.fontSize, ImVec2(origin.x + indent, origin.y),
                   labelColor, qi18n::tr(row.label));

    const char *value = nullptr;
    char toggleBuf[8];
    if (row.kind == kNativeRowToggle)
    {
        std::snprintf(toggleBuf, sizeof(toggleBuf), "%s",
                      qi18n::tr(row.value != 0 ? "ON" : "OFF"));
        value = toggleBuf;
    }
    else if (row.kind == kNativeRowChoice && row.choices != nullptr)
    {
        value = (row.value >= 0 && row.value < row.choiceCount)
            ? qi18n::tr(row.choices[row.value])
            : qi18n::tr("(other)");
    }
    else if (row.kind == kNativeRowInfo || row.kind == kNativeRowAction)
    {
        value = qi18n::tr(row.valueText);
    }

    if (value != nullptr && value[0] != '\0')
    {
        const float w = NativeMenuTextWidth(font, m.fontSize, value);
        ImU32 color = NativeMenuStyle::kTextValue;
        if (!row.enabled) color = NativeMenuStyle::kTextDim;
        else if (row.kind == kNativeRowInfo) color = NativeMenuStyle::kText;
        NativeMenuText(dl, font, m.fontSize,
                       ImVec2(origin.x + width - w, origin.y), color, value);

        if (selected && row.enabled && row.kind == kNativeRowChoice
            && row.choiceCount > 1)
        {
            const float arrowGap = std::floor(m.fontSize * 0.45f);
            NativeMenuText(dl, font, m.fontSize,
                           ImVec2(origin.x + width - w - arrowGap
                                      - NativeMenuTextWidth(font, m.fontSize, "<"),
                                  origin.y),
                           NativeMenuStyle::kCursor, "<");
            NativeMenuText(dl, font, m.fontSize,
                           ImVec2(origin.x + width + arrowGap, origin.y),
                           NativeMenuStyle::kCursor, ">");
        }
    }
}

struct NativeMenuFlow
{
    int line[kNativeMenuMaxRows];
    int column[kNativeMenuMaxRows];
    int columns;
    int lines;
};

NativeMenuFlow NativeMenuComputeFlow(const NativeMenuPageBuild &build,
                                     ImFont *font,
                                     const NativeMenuMetrics &m,
                                     float contentW)
{
    NativeMenuFlow flow;
    flow.columns = 1;
    flow.lines = 0;

    if (build.columnar)
    {
        float widest = 0.0f;
        for (int i = 0; i < build.count; ++i)
        {
            if (build.rows[i].kind == kNativeRowHeading
                || build.rows[i].kind == kNativeRowSpacer)
            {
                continue;
            }
            const float w =
                NativeMenuTextWidth(font, m.fontSize, build.rows[i].label);
            if (w > widest) widest = w;
        }
        const float cellW = widest + m.fontSize * 2.0f;
        if (cellW > 0.0f && contentW > 0.0f)
        {
            flow.columns = static_cast<int>(contentW / cellW);
        }
        if (flow.columns < 1) flow.columns = 1;
        if (flow.columns > 4) flow.columns = 4;
    }

    int line = 0;
    int column = 0;
    for (int i = 0; i < build.count; ++i)
    {
        const NativeRow &row = build.rows[i];
        const bool spans = row.kind == kNativeRowHeading
            || row.kind == kNativeRowSpacer || flow.columns == 1;
        if (spans)
        {
            if (column != 0)
            {
                ++line;
                column = 0;
            }
            flow.line[i] = line;
            flow.column[i] = 0;
            ++line;
            continue;
        }
        flow.line[i] = line;
        flow.column[i] = column;
        if (++column >= flow.columns)
        {
            column = 0;
            ++line;
        }
    }
    if (column != 0) ++line;
    flow.lines = line;
    return flow;
}

int NativeMenuAdvanceFlow(const NativeMenuPageBuild &build,
                          const NativeMenuFlow &flow, int from, int step,
                          bool byLine)
{
    if (build.count <= 0) return 0;
    if (!byLine || flow.columns <= 1)
    {
        return NativeMenuAdvanceRow(build, from, step);
    }
    const int wantColumn = flow.column[from];
    for (int guard = 1; guard <= flow.lines; ++guard)
    {
        const int target = flow.line[from] + step * guard;
        if (target < 0 || target >= flow.lines) break;
        int best = -1;
        for (int i = 0; i < build.count; ++i)
        {
            if (flow.line[i] != target) continue;
            if (!NativeRowSelectable(build.rows[i])) continue;
            if (best < 0 || std::abs(flow.column[i] - wantColumn)
                                < std::abs(flow.column[best] - wantColumn))
            {
                best = i;
            }
        }
        if (best >= 0) return best;
    }
    return from;
}

void OverlayFilterSettingsEnsure()
{
    if (g_overlayFilterLoaded) return;
    g_overlayFilterLoaded = true;
    g_hitboxKindMask[0] = static_cast<unsigned>(
        OverlayIniGet("HitboxFilterP1", 0xFF)) & 0xFFu;
    g_hitboxKindMask[1] = static_cast<unsigned>(
        OverlayIniGet("HitboxFilterP2", 0xFF)) & 0xFFu;
    unsigned sides = static_cast<unsigned>(
        OverlayIniGet("FrameAdvantageSides", 3)) & 3u;
    g_frameAdvSideMask = sides == 0u ? 3u : sides;
    sides = static_cast<unsigned>(
        OverlayIniGet("InputViewerSides", 3)) & 3u;
    g_inputViewerSideMask = sides == 0u ? 3u : sides;
}

constexpr DWORD kNativeMenuStyleRowTag = 0xF001u;
constexpr DWORD kNativeRefillTargetRowTag = 0xF002u;
constexpr DWORD kNativeDiagnosticsRowTag = 0xF003u;
constexpr DWORD kNativeInputViewerRowTag = 0xF004u;
constexpr DWORD kNativeMenuBackRowTag = 0xF005u;
constexpr DWORD kNativeTriggerOpenRowTag = 0xF010u;
constexpr DWORD kNativeTriggerResponseRowTag = 0xF006u;
constexpr DWORD kNativeTriggerSetResponseTag = 0xF007u;
constexpr DWORD kNativeResponseSlotFlag = 0x8000u;
constexpr DWORD kNativeHudMaskRowTag = 0xF008u;
constexpr DWORD kNativeHudGuardRowTag = 0xF030u;
constexpr DWORD kNativeBoxFilterRowTag = 0xF050u;
constexpr DWORD kNativeFrameAdvSidesRowTag = 0xF058u;
constexpr DWORD kNativeInputViewerSidesRowTag = 0xF059u;
constexpr DWORD kNativeGuardTimingModeRowTag = 0xF05Au;
constexpr DWORD kNativeGuardTimingWindowRowTag = 0xF05Bu;
constexpr DWORD kNativeSetValueRowTag = 0xF00Bu;
constexpr DWORD kNativeMeterChargeSideRowTag = 0xF00Eu;
constexpr DWORD kNativeMeterChargeRateRowTag = 0xF00Fu;
constexpr DWORD kNativeRecoveryTimingRowTag = 0xF020u;
constexpr DWORD kNativeTriggerSlotRowTag = 0xF100u;
constexpr DWORD kNativeTriggerDelayRowTag = 0xF200u;
constexpr DWORD kNativeTriggerChanceRowTag = 0xF300u;

static_assert(kNativeTriggerOpenRowTag
                  + QOH99_TRAINING_TRIGGER_EVENT_COUNT
              <= kNativeRecoveryTimingRowTag,
              "trigger-open range runs into the recovery-timing tags");
static_assert(kNativeRecoveryTimingRowTag + 3u <= kNativeHudGuardRowTag,
              "recovery-timing range runs into the HUD guard tag");
static_assert(kNativeHudGuardRowTag < kNativeTriggerSlotRowTag,
              "HUD guard tag runs into the trigger-slot tags");
static_assert(kNativeHudMaskRowTag + 3u <= kNativeSetValueRowTag,
              "native-HUD range runs into the set-value tags");
static_assert(kNativeSetValueRowTag + 3u <= kNativeMeterChargeSideRowTag,
              "set-value range runs into the meter-charge tags");
static_assert(kNativeMeterChargeRateRowTag < kNativeTriggerOpenRowTag,
              "meter-charge tags run into the trigger-open range");
static_assert(kNativeTriggerSlotRowTag
                  + QOH99_TRAINING_TRIGGER_EVENT_COUNT
              <= kNativeTriggerDelayRowTag,
              "trigger-slot range runs into the trigger-delay tags");
static_assert(kNativeTriggerDelayRowTag
                  + QOH99_TRAINING_TRIGGER_EVENT_COUNT
              <= kNativeTriggerChanceRowTag,
              "trigger-delay range runs into the trigger-chance tags");

const char *const kRefillTargetNames[] = { "Both", "P1", "P2" };

int g_nativeRefillTarget = QOH99_TRAINING_REFILL_BOTH;

int g_nativeSetValueIndex[3] = {4, 4, 4};

const char *const kNativeSetValueNames[] = {
    "10%", "25%", "50%", "75%", "100%",
};
const int kNativeSetValuePercent[] = { 10, 25, 50, 75, 100 };

const char *const kNativeChargeRateNames[] = {
    "1f", "2f", "4f", "8f", "15f", "30f", "60f",
};
const int kNativeChargeRateValues[] = { 1, 2, 4, 8, 15, 30, 60 };

const char *const kNativeRecoveryDelayNames[] = {
    "0", "1", "2", "3", "5", "8", "12", "20", "30",
};
const int kNativeRecoveryDelayValues[] = { 0, 1, 2, 3, 5, 8, 12, 20, 30 };

const char *const kNativeRestoreNames[] = { "Off", "When neutral" };
const char *const kNativeRestoreHints[] = {
    "Never restored automatically.",
    "Topped up whenever neither side is busy.",
};
const int kNativeRestoreValues[] = {
    QOH99_TRAINING_TRIGGER_OFF,
    QOH99_TRAINING_TRIGGER_WHEN_NEUTRAL,
};

const char *const kNativeCriticalModeNames[] = {
    "Normal chance", "Always", "No random", "Disabled", "Force roll",
};
const char *const kNativeCriticalModeHints[] = {
    "Leaves the game's own chance alone.",
    "The next hit that can crit, does.",
    "Keeps criticals but removes the random roll.",
    "No criticals at all.",
    "Advanced: drive the roll directly.",
};

const char *const kNativeGuardTimingNames[] = {
    "Always", "After being hit", "Until first block",
};
const char *const kNativeGuardTimingHints[] = {
    "The guard mode applies whenever it can - the old behaviour.",
    "The dummy takes the first hit, then blocks for the window below; the "
    "pressure ending lets it drop back to taking hits.",
    "The dummy blocks ONE hit and then stops - if your next attack still "
    "lands, the string had a gap. Re-arms after it stands free for the "
    "window below.",
};
const char *const kNativeGuardWindowNames[] = {
    "30", "60", "90", "120", "180", "240", "300",
};
const int kNativeGuardWindowValues[] = { 30, 60, 90, 120, 180, 240, 300 };

const char *const kNativeReduceModeNames[] = {
    "Manual", "Always strong", "Always light", "Off",
};
const char *const kNativeReduceModeHints[] = {
    "The real mash check decides: 2 quick taps in 10 frames = 70%, "
    "in 25 = 90%.",
    "Every hit lands at 70%, as if the mash were perfect.",
    "Every hit lands at 90%, the loose-mash grade held constantly.",
    "Mashing never reduces damage: clean numbers for damage testing.",
};

void NativeMenuBuildQuick(const Qoh99TrainingStatusV1 &status,
                          NativeMenuPageBuild &out)
{
    const bool resetReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_RESET);
    const bool refillReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_REFILL);
    const bool policiesReady =
        refillReady && status.resourcePolicyValid != 0;

    out.heading("POSITION");
    out.action("Round Start", resetReady,
               QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
               QOH99_TRAINING_POSITION_ROUND_START,
               "Both characters at the round-start anchors.");
    out.action("Left Corner", resetReady,
               QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
               QOH99_TRAINING_POSITION_LEFT_CORNER, nullptr);
    out.action("Center", resetReady,
               QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
               QOH99_TRAINING_POSITION_CENTER, nullptr);
    out.action("Right Corner", resetReady,
               QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
               QOH99_TRAINING_POSITION_RIGHT_CORNER, nullptr);
    out.action("Touching", resetReady,
               QOH99_TRAINING_COMMAND_RESET_POSITION, 0,
               QOH99_TRAINING_POSITION_VERY_CLOSE,
               "Midscreen with boxes touching, for tight blocking and throws.");
    out.action("Reset to Neutral", resetReady,
               QOH99_TRAINING_COMMAND_RESET_ROUND_START_NEUTRAL,
               QOH99_TRAINING_REFILL_BOTH, 0,
               "Round-start positions AND both characters back to idle.");

    out.spacer();
    out.heading("REFILL NOW");
    out.choice("Apply to", g_nativeRefillTarget, kRefillTargetNames, 3,
               true, QOH99_TRAINING_COMMAND_NONE, kNativeRefillTargetRowTag,
               "Which side the three refills below act on.");
    out.action("Refill Health", refillReady,
               QOH99_TRAINING_COMMAND_REFILL_LIFE, 0,
               static_cast<DWORD>(g_nativeRefillTarget), nullptr);
    out.action("Refill EX Meter", refillReady,
               QOH99_TRAINING_COMMAND_REFILL_EX_METER, 0,
               static_cast<DWORD>(g_nativeRefillTarget), nullptr);
    out.action("Refill Guard Gauge", refillReady,
               QOH99_TRAINING_COMMAND_REFILL_GUARD_GAUGE, 0,
               static_cast<DWORD>(g_nativeRefillTarget), nullptr);

    out.spacer();
    out.heading("RESTORE AUTOMATICALLY");
    out.choiceMapped("Health", static_cast<int>(status.healthTrigger),
                     kNativeRestoreNames, kNativeRestoreValues, 2,
                     policiesReady,
                     QOH99_TRAINING_COMMAND_SET_HEALTH_POLICY,
                     QOH99_TRAINING_REFILL_BOTH, nullptr,
                     kNativeRestoreHints);
    out.choiceMapped("EX meter", static_cast<int>(status.exTrigger),
                     kNativeRestoreNames, kNativeRestoreValues, 2,
                     policiesReady,
                     QOH99_TRAINING_COMMAND_SET_EX_POLICY,
                     QOH99_TRAINING_REFILL_BOTH, nullptr,
                     kNativeRestoreHints);
    out.choiceMapped("Guard gauge",
                     static_cast<int>(status.guardGaugeTrigger),
                     kNativeRestoreNames, kNativeRestoreValues, 2,
                     policiesReady,
                     QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_POLICY,
                     QOH99_TRAINING_REFILL_BOTH, nullptr,
                     kNativeRestoreHints);

    out.spacer();
    out.heading("SET VALUES");
    const bool valuesReady = refillReady && status.lifeSetMax != 0u;
    out.choice("Health", g_nativeSetValueIndex[0], kNativeSetValueNames,
               IM_ARRAYSIZE(kNativeSetValueNames), valuesReady,
               QOH99_TRAINING_COMMAND_NONE, kNativeSetValueRowTag + 0u,
               "Applies to the side chosen above. Never below 1 -- a knocked-"
               "out frame would end the round.");
    out.choice("EX stocks", g_nativeSetValueIndex[1], kNativeSetValueNames,
               IM_ARRAYSIZE(kNativeSetValueNames), refillReady,
               QOH99_TRAINING_COMMAND_NONE, kNativeSetValueRowTag + 1u,
               "Whole stocks, with nothing towards the next one.");
    out.choice("Guard gauge", g_nativeSetValueIndex[2], kNativeSetValueNames,
               IM_ARRAYSIZE(kNativeSetValueNames), refillReady,
               QOH99_TRAINING_COMMAND_NONE, kNativeSetValueRowTag + 2u,
               nullptr);
    if (status.exRestorePinned != 0u)
    {
        out.action("Restore EX to full again", refillReady,
                   QOH99_TRAINING_COMMAND_SET_EX_RESTORE_TARGET, 0, 0,
                   "EX currently restores to what you last charged to.");
    }

    out.spacer();
    out.heading("CRITICAL HITS");
    {
        const bool criticalReady = TrainingContactPolicyReady(
            status, QOH99_TRAINING_CAP_CRITICAL_POLICY,
            status.criticalAvailable);
        out.choice("P1 attacks",
                   static_cast<int>(
                       status.criticalP1Policy
                               < IM_ARRAYSIZE(kNativeCriticalModeNames)
                           ? status.criticalP1Policy : 0u),
                   kNativeCriticalModeNames,
                   IM_ARRAYSIZE(kNativeCriticalModeNames), criticalReady,
                   QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
                   QOH99_TRAINING_CONTACT_SLOT_CRITICAL_P1,
                   "How P1's attacks roll for a critical.",
                   kNativeCriticalModeHints);
        out.choice("P2 attacks",
                   static_cast<int>(
                       status.criticalP2Policy
                               < IM_ARRAYSIZE(kNativeCriticalModeNames)
                           ? status.criticalP2Policy : 0u),
                   kNativeCriticalModeNames,
                   IM_ARRAYSIZE(kNativeCriticalModeNames), criticalReady,
                   QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
                   QOH99_TRAINING_CONTACT_SLOT_CRITICAL_P2,
                   "How P2's attacks roll for a critical.",
                   kNativeCriticalModeHints);
    }

    out.spacer();
    out.heading("EX CHARGE CONTROL");
    {
        DWORD chargeTarget = status.meterChargeTarget;
        if (chargeTarget > QOH99_TRAINING_REFILL_P2)
            chargeTarget = QOH99_TRAINING_REFILL_BOTH;
        out.choice("Side", static_cast<int>(chargeTarget),
                   kRefillTargetNames, 3, refillReady,
                   QOH99_TRAINING_COMMAND_NONE, kNativeMeterChargeSideRowTag,
                   "Who gains meter while the charge control is held.");
        out.choiceMapped("Frames per step",
                         static_cast<int>(status.meterChargeInterval),
                         kNativeChargeRateNames, kNativeChargeRateValues,
                         IM_ARRAYSIZE(kNativeChargeRateValues), refillReady,
                         QOH99_TRAINING_COMMAND_NONE,
                         kNativeMeterChargeRateRowTag,
                         "How long the bar takes to move one step.");
    }

    out.spacer();
    out.heading("SESSION");
    out.toggle("Pause",
               (status.holdReasons & QOH99_TRAINING_HOLD_USER) != 0,
               TrainingHasRecoveryStatus(status) && resetReady,
               QOH99_TRAINING_COMMAND_TOGGLE_PAUSE, 0,
               "Freeze the match without closing this menu.");
    out.action("Frame Step", TrainingHasRecoveryStatus(status) && resetReady,
               QOH99_TRAINING_COMMAND_FRAME_STEP, 0, 0,
               "Run exactly one frame with the menu open.");
    {
        const bool swapReady = TrainingHasPlayerSwapStatus(status);
        const bool swapped = swapReady && status.playerSwap != 0u;
        out.toggle(swapped ? "Dummy is P1 (you play P2)"
                           : "Dummy is P2 (you play P1)",
                   swapped, swapReady,
                   QOH99_TRAINING_COMMAND_SET_PLAYER_SWAP, 0,
                   "Which fighter is the dummy: the one that blocks and "
                   "plays back recordings. Recording records the dummy, and "
                   "your own fighter uses the dummy settings while you do. "
                   "Resets at character select.");
    }
    out.action("Return to Character Select",
               TrainingReturnToSelectReady(status),
               QOH99_TRAINING_COMMAND_RETURN_TO_CHARACTER_SELECT, 0, 0,
               nullptr);
}

const char *const kNativeGuardModeNames[] = {
    "Off", "All", "Adaptive", "Stand", "Crouch", "Air",
};
const char *const kNativeGuardModeHints[] = {
    "The dummy never guards.",
    "Reads the move that is out and blocks it correctly.",
    "Guesses stand or crouch from where the attacker is.",
    "Always holds back. Loses to lows.",
    "Always holds down-back. Loses to overheads.",
    "Holds up-back, so it jumps backwards and blocks in the air.",
};
const int kNativeGuardModeValues[] = {
    QOH99_TRAINING_GUARD_OFF,
    QOH99_TRAINING_GUARD_AUTO_RECORD,
    QOH99_TRAINING_GUARD_AUTO_HEURISTIC,
    QOH99_TRAINING_GUARD_ALWAYS_STAND,
    QOH99_TRAINING_GUARD_ALWAYS_CROUCH,
    QOH99_TRAINING_GUARD_ALWAYS_AIR,
};

const char *const kNativeStanceNames[] = { "Stand", "Crouch", "Jump" };
const char *const kNativeStanceHints[] = {
    "The dummy stands.",
    "The dummy crouches.",
    "The dummy jumps repeatedly.",
};
const char *const kNativeTechNames[] = { "Off", "On" };

const char *const kNativeContactModeNames[] = {
    "Manual", "Always when legal", "Never",
};
const char *const kNativeContactModeHints[] = {
    "Follows what the dummy would really do.",
    "Uses it every time the game allows it.",
    "Never uses it, even when the input would.",
};

const char *const kNativeRecoveryNames[] = {
    "Off", "Earliest", "Fixed delay", "Random delay", "Late",
};
const int kNativeRecoveryValues[] = {
    QOH99_TRAINING_RECOVERY_OFF,
    QOH99_TRAINING_RECOVERY_EARLIEST,
    QOH99_TRAINING_RECOVERY_FIXED_DELAY,
    QOH99_TRAINING_RECOVERY_RANDOM_DELAY,
    QOH99_TRAINING_RECOVERY_LATE,
};

const char *const kNativeRecoveryHints[] = {
    "The dummy stays down for the full knockdown.",
    "Recovers the first frame it is allowed to.",
    "Recovers after the fixed delay set below.",
    "Recovers after a random delay in the range set below.",
    "Recovers as late as the game allows.",
};

const char *const kNativeTerminalNames[] = {
    "Leave it alone", "Forward", "Back", "Random",
};
const char *const kNativeTerminalHints[] = {
    "The only setting that does not touch the game's own behaviour.",
    "Gets up rolling forward.",
    "Gets up rolling back.",
    "Picks a direction each time.",
};

void NativeMenuBuildDummy(const Qoh99TrainingStatusV1 &status,
                          NativeMenuPageBuild &out)
{
    const bool guardReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_DUMMY_GUARD);
    const bool recoveryReady = TrainingHasRecoveryStatus(status)
        && status.recoveryReadoutValid != 0;

    out.heading("DEFENCE");
    out.choiceMapped("Guard mode", static_cast<int>(status.guardMode),
                     kNativeGuardModeNames, kNativeGuardModeValues,
                     IM_ARRAYSIZE(kNativeGuardModeValues), guardReady,
                     QOH99_TRAINING_COMMAND_SET_GUARD_MODE, 0, nullptr,
                     kNativeGuardModeHints);
    out.toggle("Guard during playback", status.guardDuringPlayback != 0,
               guardReady, QOH99_TRAINING_COMMAND_SET_GUARD_DURING_PLAYBACK, 0,
               "Let the dummy guard while a recording is playing back.");
    {
        const bool timingReady =
            guardReady && TrainingHasGuardTimingStatus(status);
        const DWORD timingMode = timingReady ? status.guardTimingMode : 0u;
        out.choice("Guard timing",
                   static_cast<int>(timingMode <= 2u ? timingMode : 0u),
                   kNativeGuardTimingNames, 3, timingReady,
                   QOH99_TRAINING_COMMAND_NONE, kNativeGuardTimingModeRowTag,
                   nullptr, kNativeGuardTimingHints);
        out.choiceMapped("Guard window",
                         timingReady
                             ? static_cast<int>(status.guardTimingWindow)
                             : 60,
                         kNativeGuardWindowNames, kNativeGuardWindowValues,
                         IM_ARRAYSIZE(kNativeGuardWindowValues),
                         timingReady && timingMode != 0u,
                         QOH99_TRAINING_COMMAND_NONE,
                         kNativeGuardTimingWindowRowTag,
                         "Frames: how long guard stays armed after a hit, "
                         "and how long the dummy must stand free before "
                         "\"until first block\" re-arms.");
    }

    out.choice("Faultless Defence",
               static_cast<int>(status.faultlessPolicy <= 2u
                                    ? status.faultlessPolicy : 0u),
               kNativeContactModeNames, 3,
               TrainingContactPolicyReady(
                   status, QOH99_TRAINING_CAP_FAULTLESS_POLICY,
                   status.faultlessAvailable),
               QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
               QOH99_TRAINING_CONTACT_SLOT_FAULTLESS, nullptr,
               kNativeContactModeHints);
    out.choice("Blocking",
               static_cast<int>(status.parryPolicy <= 2u
                                    ? status.parryPolicy : 0u),
               kNativeContactModeNames, 3,
               TrainingContactPolicyReady(
                   status, QOH99_TRAINING_CAP_PARRY_POLICY,
                   status.parryAvailable),
               QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
               QOH99_TRAINING_CONTACT_SLOT_PARRY, nullptr,
               kNativeContactModeHints);
    {
        const bool reduceReady = TrainingHasReducePolicyStatus(status)
            && TrainingContactPolicyReady(
                   status, QOH99_TRAINING_CAP_REDUCE_POLICY,
                   status.reduceAvailable);
        out.choice("P1 damage reduce",
                   static_cast<int>(status.reduceP1Policy <= 3u
                                        ? status.reduceP1Policy : 0u),
                   kNativeReduceModeNames, 4, reduceReady,
                   QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
                   QOH99_TRAINING_CONTACT_SLOT_REDUCE_P1, nullptr,
                   kNativeReduceModeHints);
        out.choice("P2 damage reduce",
                   static_cast<int>(status.reduceP2Policy <= 3u
                                        ? status.reduceP2Policy : 0u),
                   kNativeReduceModeNames, 4, reduceReady,
                   QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY,
                   QOH99_TRAINING_CONTACT_SLOT_REDUCE_P2, nullptr,
                   kNativeReduceModeHints);
    }

    out.spacer();
    out.heading("STANCE");
    out.choice("Stance",
               static_cast<int>(status.dummyStance <= QOH99_TRAINING_STANCE_JUMP
                                    ? status.dummyStance : 0u),
               kNativeStanceNames, 3, guardReady,
               QOH99_TRAINING_COMMAND_SET_STANCE, 0,
               "What the dummy holds when idle.", kNativeStanceHints);

    out.spacer();
    out.heading("THROW TECH");
    static const char *const kChanceNames[] = {
        "0%", "25%", "50%", "75%", "100%" };
    static const int kChanceValues[] = { 0, 25, 50, 75, 100 };
    out.choiceMapped("Tech chance",
                     static_cast<int>(status.throwTechChancePercent),
                     kChanceNames, kChanceValues, 5, guardReady,
                     QOH99_TRAINING_COMMAND_SET_THROW_TECH_CHANCE, 0,
                     "How often the dummy techs a throw.");

    out.spacer();
    out.heading("RECOVERY AFTER BEING HIT");
    out.choiceMapped("In the air", static_cast<int>(status.recoveryModeAir),
                     kNativeRecoveryNames, kNativeRecoveryValues,
                     IM_ARRAYSIZE(kNativeRecoveryValues), recoveryReady,
                     QOH99_TRAINING_COMMAND_SET_RECOVERY_POLICY,
                     QOH99_TRAINING_RECOVERY_SLOT_AIR, nullptr,
                     kNativeRecoveryHints);
    out.choiceMapped("On the ground",
                     static_cast<int>(status.recoveryModeGround),
                     kNativeRecoveryNames, kNativeRecoveryValues,
                     IM_ARRAYSIZE(kNativeRecoveryValues), recoveryReady,
                     QOH99_TRAINING_COMMAND_SET_RECOVERY_POLICY,
                     QOH99_TRAINING_RECOVERY_SLOT_GROUND, nullptr,
                     kNativeRecoveryHints);
    out.choiceMapped("Off a wall", static_cast<int>(status.recoveryModeWall),
                     kNativeRecoveryNames, kNativeRecoveryValues,
                     IM_ARRAYSIZE(kNativeRecoveryValues), recoveryReady,
                     QOH99_TRAINING_COMMAND_SET_RECOVERY_POLICY,
                     QOH99_TRAINING_RECOVERY_SLOT_WALL, nullptr,
                     kNativeRecoveryHints);
    out.choiceMapped("End of knockdown",
                     static_cast<int>(status.recoveryModeTerminal),
                     kNativeRecoveryNames, kNativeRecoveryValues,
                     IM_ARRAYSIZE(kNativeRecoveryValues), recoveryReady,
                     QOH99_TRAINING_COMMAND_SET_RECOVERY_POLICY,
                     QOH99_TRAINING_RECOVERY_SLOT_TERMINAL, nullptr,
                     kNativeRecoveryHints);
    out.choice("Knockdown direction",
               static_cast<int>(status.recoveryTerminalChoice <= 3u
                                    ? status.recoveryTerminalChoice : 0u),
               kNativeTerminalNames, 4, recoveryReady,
               QOH99_TRAINING_COMMAND_SET_TERMINAL_RECOVERY, 0, nullptr,
               kNativeTerminalHints);

    const DWORD modes[4] = {
        status.recoveryModeAir, status.recoveryModeGround,
        status.recoveryModeWall, status.recoveryModeTerminal,
    };
    bool usesFixed = false;
    bool usesRandom = false;
    for (const DWORD mode : modes)
    {
        if (mode == QOH99_TRAINING_RECOVERY_FIXED_DELAY) usesFixed = true;
        if (mode == QOH99_TRAINING_RECOVERY_RANDOM_DELAY) usesRandom = true;
    }
    if (recoveryReady && (usesFixed || usesRandom))
    {
        out.spacer();
        out.heading("RECOVERY TIMING");
        if (usesFixed)
        {
            out.choiceMapped("Fixed delay",
                             static_cast<int>(status.recoveryDelaySteps),
                             kNativeRecoveryDelayNames,
                             kNativeRecoveryDelayValues,
                             IM_ARRAYSIZE(kNativeRecoveryDelayValues), true,
                             QOH99_TRAINING_COMMAND_NONE,
                             kNativeRecoveryTimingRowTag + 0u,
                             "Frames to wait before recovering.");
        }
        if (usesRandom)
        {
            out.choiceMapped("Random from",
                             static_cast<int>(status.recoveryRandomMinSteps),
                             kNativeRecoveryDelayNames,
                             kNativeRecoveryDelayValues,
                             IM_ARRAYSIZE(kNativeRecoveryDelayValues), true,
                             QOH99_TRAINING_COMMAND_NONE,
                             kNativeRecoveryTimingRowTag + 1u,
                             "The same save state always replays the same "
                             "sequence, so a setup stays reproducible.");
            out.choiceMapped("Random to",
                             static_cast<int>(status.recoveryRandomMaxSteps),
                             kNativeRecoveryDelayNames,
                             kNativeRecoveryDelayValues,
                             IM_ARRAYSIZE(kNativeRecoveryDelayValues), true,
                             QOH99_TRAINING_COMMAND_NONE,
                             kNativeRecoveryTimingRowTag + 2u,
                             "Never below the shortest delay above - a range "
                             "that runs backwards is refused.");
        }
    }
}

const char *const kNativeTriggerSlotNames[] = {
    "Off", "1", "2", "3", "4", "5", "6", "7", "8",
};

const char *const kNativeTriggerEventNames[] = {
    "Getting hit",
    "Recovering from a block",
    "Recovering from a hit",
    "Waking up",
    "Teching in the air",
    "Teching on the ground",
    "Teching a throw",
    "Blocking (guard cancel)",
};
static_assert(IM_ARRAYSIZE(kNativeTriggerEventNames)
                  == QOH99_TRAINING_TRIGGER_EVENT_COUNT,
              "the in-game trigger list must mirror the trigger event ABI");

const char *const kNativeTriggerEventHints[] = {
    "The moment your attack connects.",
    "As soon as it can move after blocking.",
    "As soon as it can move after being hit.",
    "Getting up with no tech. Auto recovery uses a tech row instead.",
    "As soon as it can move after teching in the air.",
    "As soon as it can move after teching on the ground.",
    "As soon as it can move after teching a throw.",
    "During blockstun, when the game allows it. Specials and supers only.",
};

const char *const kNativeTriggerDelayNames[] = {
    "0f", "1f", "2f", "4f", "8f", "16f", "30f",
};
const int kNativeTriggerDelayValues[] = { 0, 1, 2, 4, 8, 16, 30 };
const char *const kNativeTriggerChanceNames[] = {
    "0%", "25%", "50%", "75%", "100%",
};
const int kNativeTriggerChanceValues[] = { 0, 25, 50, 75, 100 };

void NativeMenuTriggerSummary(const Qoh99TrainingStatusV1 &status, int event,
                              char *out, size_t outBytes)
{
    char scratch[32];
    const char *response =
        TrainingReactionPreview(status, event, scratch, sizeof(scratch));
    if (g_trainingUi.triggerMoveIndex[event] <= 0
        && g_trainingUi.triggerMacroSlot[event] <= 0)
    {
        std::snprintf(out, outBytes, "Off");
        return;
    }
    std::snprintf(out, outBytes, "%s - %df - %d%%", response,
                  g_trainingUi.triggerDelaySteps[event],
                  g_trainingUi.triggerChancePercent[event]);
}

void NativeMenuBuildTriggers(const Qoh99TrainingStatusV1 &status,
                             NativeMenuPageBuild &out)
{
    const bool ready = TrainingFeatureReady(status, QOH99_TRAINING_CAP_MACROS);
    constexpr int events = QOH99_TRAINING_TRIGGER_EVENT_COUNT;

    const int open = g_nativeMenu.submenu;
    if (open < 0 || open >= events)
    {
        g_nativeMenu.submenuChild = false;
        out.heading("WHEN THE DUMMY IS...");
        for (int event = 0; event < events; ++event)
        {
            char summary[40];
            NativeMenuTriggerSummary(status, event, summary, sizeof(summary));
            out.submenu(kNativeTriggerEventNames[event], summary, ready,
                        kNativeTriggerOpenRowTag + static_cast<DWORD>(event),
                        kNativeTriggerEventHints[event]);
        }
        return;
    }

    if (g_nativeMenu.submenuChild)
    {
        out.columnar = true;
        out.heading("MOVE");
        out.action("< Back", true, QOH99_TRAINING_COMMAND_NONE,
                   kNativeMenuBackRowTag, 0, "Backspace also goes back.");
        out.spacer();
        out.action("Off", true, QOH99_TRAINING_COMMAND_NONE,
                   kNativeTriggerSetResponseTag, 0,
                   "The dummy does nothing after this event.");
        if (g_trainingUi.triggerMoveIndex[open] <= 0
            && g_trainingUi.triggerMacroSlot[open] <= 0)
        {
            out.preferredRow = out.count - 1;
        }

        const bool moveCharacterReady = status.p2Character <= 0xFFu
            && qtraining::trainingMoveSelectableCharacterId(
                   static_cast<uint8_t>(status.p2Character))
                != qtraining::kTrainingMoveInvalidCharacter;
        if (moveCharacterReady && TrainingActionTriggerCatalogMatches(status))
        {
            out.heading("MOVES");
            const auto triggerEvent =
                static_cast<qtraining::TrainingTriggerEvent>(open);
            const uint16_t moveCount = qtraining::trainingMoveCount(
                static_cast<uint8_t>(status.p2Character));
            for (uint16_t moveIndex = 1u; moveIndex <= moveCount; ++moveIndex)
            {
                if (TrainingMoveWasAlreadyListed(
                        status.p2Character, moveIndex, triggerEvent))
                {
                    continue;
                }
                const qtraining::TrainingMoveView move =
                    qtraining::trainingMove(
                        static_cast<uint8_t>(status.p2Character), moveIndex);
                if (!move.valid()) continue;
                out.action(move.input.data(), ready,
                           QOH99_TRAINING_COMMAND_NONE,
                           kNativeTriggerSetResponseTag,
                           static_cast<DWORD>(moveIndex),
                           TrainingMoveContextLabel(move.context));
                if (g_trainingUi.triggerMoveIndex[open]
                    == static_cast<int>(moveIndex))
                {
                    out.preferredRow = out.count - 1;
                }
            }
        }

        out.heading("RECORDINGS");
        {
            const int random =
                static_cast<int>(QOH99_TRAINING_TRIGGER_RANDOM_RECORDING);
            out.action("Random recording", ready,
                       QOH99_TRAINING_COMMAND_NONE,
                       kNativeTriggerSetResponseTag,
                       kNativeResponseSlotFlag | static_cast<DWORD>(random),
                       (status.validMacroSlots & 0xFFu) != 0u
                           ? "Plays one recorded slot, picked each time it fires."
                           : "Nothing recorded yet - record a slot first.");
            if (g_trainingUi.triggerMoveIndex[open] <= 0
                && g_trainingUi.triggerMacroSlot[open] == random)
            {
                out.preferredRow = out.count - 1;
            }
        }
        for (int slot = 1;
             slot <= static_cast<int>(QOH99_TRAINING_SLOT_COUNT); ++slot)
        {
            const bool occupied =
                (status.validMacroSlots & (1u << (slot - 1))) != 0;
            char label[24];
            std::snprintf(label, sizeof(label), "Slot %d%s", slot,
                          occupied ? "" : " (empty)");
            out.submenu(label, "", ready, kNativeTriggerSetResponseTag,
                        occupied ? "This slot holds a recording."
                                 : "Nothing recorded here yet.");
            if (out.count > 0)
            {
                out.rows[out.count - 1].actionValue =
                    kNativeResponseSlotFlag | static_cast<DWORD>(slot);
                if (g_trainingUi.triggerMoveIndex[open] <= 0
                    && g_trainingUi.triggerMacroSlot[open] == slot)
                {
                    out.preferredRow = out.count - 1;
                }
            }
        }
        return;
    }

    out.heading(kNativeTriggerEventNames[open]);
    out.action("< Back", true, QOH99_TRAINING_COMMAND_NONE,
               kNativeMenuBackRowTag, 0, "Backspace also goes back.");
    out.spacer();
    char response[40];
    NativeMenuTriggerSummary(status, open, response, sizeof(response));
    out.submenu("Move", response, ready, kNativeTriggerResponseRowTag,
                "A character move or a recording. Opens a list.");
    out.preferredRow = out.count - 1;
    out.choiceMapped("Delay", g_trainingUi.triggerDelaySteps[open],
                     kNativeTriggerDelayNames, kNativeTriggerDelayValues,
                     IM_ARRAYSIZE(kNativeTriggerDelayValues), ready,
                     QOH99_TRAINING_COMMAND_NONE,
                     kNativeTriggerDelayRowTag + static_cast<DWORD>(open),
                     "Frames to wait before the dummy acts.");
    out.choiceMapped("Chance", g_trainingUi.triggerChancePercent[open],
                     kNativeTriggerChanceNames, kNativeTriggerChanceValues,
                     IM_ARRAYSIZE(kNativeTriggerChanceValues), ready,
                     QOH99_TRAINING_COMMAND_NONE,
                     kNativeTriggerChanceRowTag + static_cast<DWORD>(open),
                     "How often this trigger fires.");
}

const char *const kNativePlaybackNames[] = {
    "Once", "Loop", "Random", "Random loop" };
const char *const kNativeFacingNames[] = { "Relative", "Absolute" };
const char *const kNativeFacingHints[] = {
    "Inputs mirror when the sides swap.",
    "Inputs replay exactly as recorded.",
};
const char *const kNativePlaybackHints[] = {
    "Plays the recording once.",
    "Repeats it until stopped.",
    "Plays one recorded slot, picked at random.",
    "Picks a new recorded slot after every run.",
};
static_assert(IM_ARRAYSIZE(kNativePlaybackNames)
                  == IM_ARRAYSIZE(kNativePlaybackHints)
              && IM_ARRAYSIZE(kNativePlaybackNames)
                  == QOH99_TRAINING_PLAY_RANDOM_LOOP + 1,
              "one name and one hint per playback mode");
const char *const kNativeSlotNames[] = { "1", "2", "3", "4", "5", "6", "7", "8" };

void NativeMenuBuildRecording(const Qoh99TrainingStatusV1 &status,
                              NativeMenuPageBuild &out)
{
    const bool macroReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_MACROS);
    const bool stateReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_SAVESTATES);
    const int macroSlots =
        (std::min)(static_cast<int>(QOH99_TRAINING_SLOT_COUNT), 8);
    const int stateSlots =
        (std::min)(static_cast<int>(QOH99_TRAINING_SLOT_COUNT), 8);

    out.heading("RECORDING");
    out.choiceSlot("Slot", static_cast<int>(status.selectedMacroSlot),
                   kNativeSlotNames, macroSlots, macroReady,
                   QOH99_TRAINING_COMMAND_SELECT_MACRO_SLOT, nullptr);
    const bool preRecording =
        status.macroState == QOH99_TRAINING_MACRO_PRE_RECORD;
    const bool recording =
        status.macroState == QOH99_TRAINING_MACRO_RECORD_ARMED
        || status.macroState == QOH99_TRAINING_MACRO_RECORDING;
    out.action(preRecording ? "Record from here"
               : recording  ? "Stop recording"
                            : "Record",
               macroReady, QOH99_TRAINING_COMMAND_TOGGLE_RECORD, 0, 0,
               preRecording
                   ? "The dummy is yours to move. This starts keeping what "
                     "you do from here."
               : recording
                   ? "Stop and keep what has been recorded."
                   : "Press once to place the dummy, again to start "
                     "recording it.");
    out.action(preRecording ? "Cancel" : "Play", macroReady,
               QOH99_TRAINING_COMMAND_TOGGLE_PLAYBACK, 0, 0,
               preRecording ? "Leave without recording anything."
                            : nullptr);
    out.action(preRecording ? "Cancel" : "Stop", macroReady,
               QOH99_TRAINING_COMMAND_STOP_MACRO, 0, 0,
               preRecording ? "Leave without recording anything."
                            : nullptr);
    const DWORD macroSlot =
        (std::min)(status.selectedMacroSlot,
                   static_cast<DWORD>(QOH99_TRAINING_SLOT_COUNT - 1u));
    out.action("Clear slot", macroReady, QOH99_TRAINING_COMMAND_CLEAR_MACRO,
               macroSlot, 0, nullptr);
    out.choice("Playback", static_cast<int>(status.playbackMode),
               kNativePlaybackNames, IM_ARRAYSIZE(kNativePlaybackNames),
               macroReady,
               QOH99_TRAINING_COMMAND_SET_PLAYBACK_MODE, 0, nullptr,
               kNativePlaybackHints);
    out.choice("Facing", static_cast<int>(status.facingPolicy),
               kNativeFacingNames, 2, macroReady,
               QOH99_TRAINING_COMMAND_SET_FACING_POLICY, 0, nullptr,
               kNativeFacingHints);
    {
        static const char *const kAfterResetNames[] = {
            "Off", "Play slot", "Record slot" };
        static const char *const kAfterResetHints[] = {
            "Nothing starts by itself.",
            "Play the selected slot as soon as a load or reset settles.",
            "Start recording into the selected slot after a load or reset.",
        };
        const bool afterResetReady = macroReady
            && TrainingHasMacroAfterResetStatus(status);
        out.choice("After load / reset",
                   static_cast<int>(afterResetReady
                                        && status.macroAfterReset <= 2u
                                        ? status.macroAfterReset : 0u),
                   kAfterResetNames, 3, afterResetReady,
                   QOH99_TRAINING_COMMAND_SET_MACRO_AFTER_RESET, 0, nullptr,
                   kAfterResetHints);
    }

    out.spacer();
    out.heading("SAVE STATES");
    const DWORD stateSlot =
        (std::min)(status.selectedStateSlot,
                   static_cast<DWORD>(QOH99_TRAINING_SLOT_COUNT - 1u));
    const bool stateSaved =
        (status.validStateSlots & (1u << stateSlot)) != 0u;
    const bool stateStale =
        (status.staleStateSlots & (1u << stateSlot)) != 0u;
    out.choiceSlot("Slot", static_cast<int>(status.selectedStateSlot),
                   kNativeSlotNames, stateSlots, stateReady,
                   QOH99_TRAINING_COMMAND_SELECT_STATE_SLOT,
                   "Which slot Save, Load and Clear act on.");
    out.action("Save", stateReady, QOH99_TRAINING_COMMAND_SAVE_STATE,
               stateSlot, 0,
               "Store the match exactly as it is now in the selected slot.");
    out.action("Load", stateReady && stateSaved && !stateStale,
               QOH99_TRAINING_COMMAND_LOAD_STATE, stateSlot, 0,
               stateStale
                   ? "Saved in a different match or with different "
                     "characters, so it cannot be loaded. Save over it to "
                     "reuse it."
                   : "Put the match back to what the selected slot holds.");
    out.action("Clear", stateReady && (stateSaved || stateStale),
               QOH99_TRAINING_COMMAND_CLEAR_STATE, stateSlot, 0,
               "Empty the selected slot.");
}

const char *const kNativeHitboxStyleNames[] = { "Off", "Enhanced", "Game's own" };
const char *const kNativeHitboxStyleHints[] = {
    "No boxes are drawn.",
    "Adds the clash boxes the game never draws.",
    "The original F7 display, for comparison.",
};
const char *const kNativeOnOffNames[] = { "Off", "On" };
const char *const kNativeSideChoiceNames[] = { "Off", "P1", "P2", "Both" };

void NativeMenuBuildView(const Qoh99TrainingStatusV1 &status,
                         NativeMenuPageBuild &out)
{
    const bool hitboxReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_HITBOXES);
    const bool advantageReady =
        TrainingFeatureReady(status, QOH99_TRAINING_CAP_FRAME_ADVANTAGE);

    OverlayFilterSettingsEnsure();
    out.heading("BOXES");
    out.choice("Hitbox display",
               static_cast<int>(status.hitboxRequested <= 2u
                                    ? status.hitboxRequested : 0u),
               kNativeHitboxStyleNames, 3, hitboxReady,
               QOH99_TRAINING_COMMAND_SET_HITBOX_DISPLAY, 0, nullptr,
               kNativeHitboxStyleHints);
    static const char *const kBoxKindLabels[8] = {
        "Collision boxes", "Hurtboxes", "Active hitboxes",
        "Inactive attack boxes", "Clash boxes", "Reflect boxes",
        "Throw boxes", "Axis line",
    };
    static const char *const kBoxKindHints[8] = {
        "The white pushbox: bodies cannot overlap.",
        "Where this player can be hit.",
        "Attack boxes that can connect right now.",
        "Attack boxes in startup, or after their hit is spent.",
        "Where trades and clashes are decided.",
        "Boxes that can deflect the other side's attack.",
        "Throw and grab boxes.",
        "The game's own centre line, per player.",
    };
    const bool customBoxes = status.hitboxRequested == 1u;
    for (int kind = 0; kind < 8; ++kind)
    {
        const unsigned bit = 1u << kind;
        const int sides = ((g_hitboxKindMask[0] & bit) ? 1 : 0)
            | ((g_hitboxKindMask[1] & bit) ? 2 : 0);
        out.choice(kBoxKindLabels[kind], sides, kNativeSideChoiceNames, 4,
                   hitboxReady && customBoxes, QOH99_TRAINING_COMMAND_NONE,
                   kNativeBoxFilterRowTag + static_cast<DWORD>(kind),
                   kBoxKindHints[kind]);
    }

    out.spacer();
    out.heading("INFO BELOW");
    out.choice("Frame advantage",
               status.frameAdvantageEnabled == 0
                   ? 0 : static_cast<int>(g_frameAdvSideMask & 3u),
               kNativeSideChoiceNames, 4, advantageReady,
               QOH99_TRAINING_COMMAND_NONE, kNativeFrameAdvSidesRowTag,
               "Who recovers first after a block or hit -- and whose "
               "attacks report it.");
    out.choice("Input viewer",
               EFZDX9_Presenter_IsInputDisplayVisible() == FALSE
                   ? 0 : static_cast<int>(g_inputViewerSideMask & 3u),
               kNativeSideChoiceNames, 4, true,
               QOH99_TRAINING_COMMAND_NONE, kNativeInputViewerSidesRowTag,
               "The on-screen input history strip, per side.");

    out.spacer();
    out.heading("GAME HUD");
    const bool hudReady = TrainingHasNativeHudStatus(status)
        && status.nativeHudOwner != 0u;
    const DWORD hudMasks[3] = { status.nativeHudRequestedGlobalMask,
                                status.nativeHudRequestedP1Mask,
                                status.nativeHudRequestedP2Mask };
    struct NativeHudRow {
        const char *label;
        int maskIndex;
        DWORD bits;
        const char *hint;
    };
    static const NativeHudRow kHudRows[] = {
        {"Elapsed time (bottom)", 0,
         QOH99_TRAINING_HUD_GLOBAL_BOTTOM_ELAPSED_TIME,
         "The bottom-centre TIME clock."},
        {"Round timer (top)", 0, QOH99_TRAINING_HUD_GLOBAL_TOP_ROUND_TIMER,
         "The round clock at the top. A different element from TIME."},
        {"FPS counter", 0, QOH99_TRAINING_HUD_GLOBAL_FPS,
         "QOH's own FPS readout."},
        {"P1 score", 1, QOH99_TRAINING_HUD_PLAYER_SCORE,
         "The 1P SCORE readout."},
        {"P2 score", 2, QOH99_TRAINING_HUD_PLAYER_SCORE,
         "The 2P SCORE readout."},
        {"P1 portrait and life", 1,
         QOH99_TRAINING_HUD_PLAYER_PORTRAIT_LIFE,
         "Portrait and all three life layers ride one native call."},
        {"P2 portrait and life", 2,
         QOH99_TRAINING_HUD_PLAYER_PORTRAIT_LIFE,
         "Portrait and all three life layers ride one native call."},
        {"P1 name", 1, QOH99_TRAINING_HUD_PLAYER_NAME,
         "The character name."},
        {"P2 name", 2, QOH99_TRAINING_HUD_PLAYER_NAME,
         "The character name."},
        {"P1 round wins", 1, QOH99_TRAINING_HUD_PLAYER_ROUND_WINS,
         "The round-win markers."},
        {"P2 round wins", 2, QOH99_TRAINING_HUD_PLAYER_ROUND_WINS,
         "The round-win markers."},
        {"P1 EX gauge (POWER)", 1, QOH99_TRAINING_HUD_PLAYER_EX,
         "The 1P POWER bar, stocks and spend animation. One native call."},
        {"P2 EX gauge (POWER)", 2, QOH99_TRAINING_HUD_PLAYER_EX,
         "The 2P POWER bar, stocks and spend animation. One native call."},
        {"P1 combo readout", 1, QOH99_TRAINING_HUD_PLAYER_COMBO,
         "The 1P hit count, damage and reaction label."},
        {"P2 combo readout", 2, QOH99_TRAINING_HUD_PLAYER_COMBO,
         "The 2P hit count, damage and reaction label."},
    };
    for (const NativeHudRow &hud : kHudRows)
    {
        NativeRow *row = out.push(kNativeRowToggle, hud.label);
        if (row == nullptr) break;
        row->value = (hudMasks[hud.maskIndex] & hud.bits) != 0u ? 1 : 0;
        row->enabled = hudReady;
        row->command = QOH99_TRAINING_COMMAND_NONE;
        row->slot = kNativeHudMaskRowTag + static_cast<DWORD>(hud.maskIndex);
        row->actionValue = hud.bits;
        row->hint = hud.hint;
    }
    {
        NativeRow *row = out.push(kNativeRowToggle, "Guard gauges (both)");
        if (row != nullptr)
        {
            row->value = ((status.nativeHudRequestedP1Mask
                           | status.nativeHudRequestedP2Mask)
                          & QOH99_TRAINING_HUD_PLAYER_GUARD) != 0u ? 1 : 0;
            row->enabled = hudReady;
            row->command = QOH99_TRAINING_COMMAND_NONE;
            row->slot = kNativeHudGuardRowTag;
            row->hint = "One native call draws both, so they switch together.";
        }
    }
}

void NativeMenuBuildKeys(const Qoh99TrainingStatusV1 &status,
                         NativeMenuPageBuild &out)
{
    (void)status;
    out.heading("TRAINING CONTROLS");
    out.info("Open this menu", "configured overlay-menu control");
    out.info("Toggle hitboxes", "configured training control");
    out.info("Refill health / meter / guard", "configured training controls");
    out.info("Record / play", "configured training controls");
    out.spacer();
    out.heading("IN THIS MENU");
    out.info("Move", "Up / Down");
    out.info("Change a value", "Left / Right");
    out.info("Confirm", "Enter or Space");
    out.info("Change page", "Q / E");
    out.spacer();
    out.info("Rebind", "caster > Controls");
}

void NativeMenuBuildHelp(const Qoh99TrainingStatusV1 &status,
                         NativeMenuPageBuild &out)
{
    (void)status;
    out.heading("TRAINING");
    out.info("Quick", "position, refills, frame step");
    out.info("Dummy", "guard, stance, throw tech");
    out.info("Triggers", "what the dummy does after a hit");
    out.info("Recording", "record inputs, save states");
    out.info("View", "boxes, frame advantage, input viewer");
    out.spacer();
    out.heading("HOTKEYS ON A CONTROLLER");
    out.info("Bind them in", "caster > Controls > Mode controls > Training");
    out.info("Pad capture appears", "when your player profile uses a pad");
    out.spacer();
    out.heading("BOX COLOURS");
    out.info("White", "collision / pushbox");
    out.info("Green", "hurtbox");
    out.info("Red", "active hitbox");
    out.info("Dark red", "cannot connect yet");
    out.info("Yellow", "clash - what trades");
    out.info("Blue", "attack-vs-attack reversal");
    out.info("Magenta", "throw / capture");
}

void NativeMenuBuildDiagnostics(const Qoh99TrainingStatusV1 &status,
                                NativeMenuPageBuild &out)
{
    (void)status;
    out.heading("DIAGNOSTICS");
    out.toggle("Diagnostics", false, true, QOH99_TRAINING_COMMAND_NONE,
               kNativeDiagnosticsRowTag,
               "Opens the classic menu with the engineering pages enabled.");
    out.spacer();
    out.info("Turning this on", "switches to the classic menu");
    out.info("To come back", "classic menu > In-game menu");
    out.spacer();
    out.heading("OVERLAY ELEMENTS");
    out.toggle("Input viewer",
               EFZDX9_Presenter_IsInputDisplayVisible() != FALSE, true,
               QOH99_TRAINING_COMMAND_NONE, kNativeInputViewerRowTag, nullptr);

}

void NativeMenuCommitRow(const Qoh99TrainingStatusV1 &status, NativeRow &row,
                         int delta, bool activate)
{
    if (!row.enabled) return;

    if (row.slot == kNativeRefillTargetRowTag)
    {
        int next = g_nativeRefillTarget + (delta != 0 ? delta : 1);
        while (next < 0) next += 3;
        g_nativeRefillTarget = next % 3;
        return;
    }
    if (row.slot == kNativeMenuStyleRowTag)
    {
        TrainingMenuSetNativeStyle(false);
        return;
    }
    if (row.slot == kNativeDiagnosticsRowTag)
    {
        OverlayIniSet("MenuAdvanced", 1);
        g_nativeMenu.advanced = true;
        TrainingMenuSetNativeStyle(false);
        return;
    }
    if (row.slot == kNativeInputViewerRowTag)
    {
        EFZDX9_Presenter_ToggleInputDisplay();
        return;
    }
    if (row.slot == kNativeGuardTimingModeRowTag
        || row.slot == kNativeGuardTimingWindowRowTag)
    {
        DWORD mode = status.guardTimingMode;
        if (mode > 2u) mode = 0u;
        DWORD window = status.guardTimingWindow;
        if (window < 1u || window > 600u) window = 60u;
        int step = delta;
        if (step == 0) step = 1;
        if (row.slot == kNativeGuardTimingModeRowTag)
        {
            int next = static_cast<int>(mode) + step;
            while (next < 0) next += 3;
            mode = static_cast<DWORD>(next % 3);
        }
        else
        {
            int index = 0;
            const int count = IM_ARRAYSIZE(kNativeGuardWindowValues);
            for (int i = 0; i < count; ++i)
            {
                if (kNativeGuardWindowValues[i]
                    == static_cast<int>(window))
                {
                    index = i;
                    break;
                }
            }
            int next = index + step;
            while (next < 0) next += count;
            window = static_cast<DWORD>(
                kNativeGuardWindowValues[next % count]);
        }
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_GUARD_TIMING, 0, mode,
            window);
        return;
    }
    if (row.slot >= kNativeBoxFilterRowTag
        && row.slot < kNativeBoxFilterRowTag + 8u)
    {
        OverlayFilterSettingsEnsure();
        const int kind = static_cast<int>(row.slot - kNativeBoxFilterRowTag);
        const unsigned bit = 1u << kind;
        int sides = ((g_hitboxKindMask[0] & bit) ? 1 : 0)
            | ((g_hitboxKindMask[1] & bit) ? 2 : 0);
        const int step = delta == 0 ? 1 : delta;
        sides = ((sides + step) % 4 + 4) % 4;
        if (sides & 1) g_hitboxKindMask[0] |= bit;
        else g_hitboxKindMask[0] &= ~bit;
        if (sides & 2) g_hitboxKindMask[1] |= bit;
        else g_hitboxKindMask[1] &= ~bit;
        OverlayIniSet("HitboxFilterP1",
                      static_cast<int>(g_hitboxKindMask[0]));
        OverlayIniSet("HitboxFilterP2",
                      static_cast<int>(g_hitboxKindMask[1]));
        return;
    }
    if (row.slot == kNativeFrameAdvSidesRowTag)
    {
        OverlayFilterSettingsEnsure();
        int sides = status.frameAdvantageEnabled == 0
            ? 0 : static_cast<int>(g_frameAdvSideMask & 3u);
        const int step = delta == 0 ? 1 : delta;
        sides = ((sides + step) % 4 + 4) % 4;
        if (sides == 0)
        {
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED,
                0, 0u);
        }
        else
        {
            g_frameAdvSideMask = static_cast<unsigned>(sides);
            OverlayIniSet("FrameAdvantageSides", sides);
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED,
                0, 1u);
        }
        return;
    }
    if (row.slot == kNativeInputViewerSidesRowTag)
    {
        OverlayFilterSettingsEnsure();
        const bool visible =
            EFZDX9_Presenter_IsInputDisplayVisible() != FALSE;
        int sides = visible
            ? static_cast<int>(g_inputViewerSideMask & 3u) : 0;
        const int step = delta == 0 ? 1 : delta;
        sides = ((sides + step) % 4 + 4) % 4;
        if (sides == 0)
        {
            if (visible) EFZDX9_Presenter_ToggleInputDisplay();
        }
        else
        {
            g_inputViewerSideMask = static_cast<unsigned>(sides);
            OverlayIniSet("InputViewerSides", sides);
            if (!visible) EFZDX9_Presenter_ToggleInputDisplay();
        }
        return;
    }
    if (row.slot == kNativeMeterChargeSideRowTag
        || row.slot == kNativeMeterChargeRateRowTag)
    {
        DWORD target = status.meterChargeTarget;
        if (target > QOH99_TRAINING_REFILL_P2) target = QOH99_TRAINING_REFILL_BOTH;
        DWORD interval = status.meterChargeInterval;
        if (interval < 1u || interval > 60u) interval = 2u;

        int step = delta;
        if (step == 0) step = 1;
        if (row.slot == kNativeMeterChargeSideRowTag)
        {
            int next = static_cast<int>(target) + step;
            while (next < 0) next += 3;
            target = static_cast<DWORD>(next % 3);
        }
        else
        {
            int index = 0;
            const int count = IM_ARRAYSIZE(kNativeChargeRateValues);
            for (int i = 0; i < count; ++i)
            {
                if (kNativeChargeRateValues[i] == static_cast<int>(interval))
                {
                    index = i;
                    break;
                }
            }
            int next = index + step;
            while (next < 0) next += count;
            interval = static_cast<DWORD>(kNativeChargeRateValues[next % count]);
        }
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_METER_CHARGE_CONFIG, target,
            interval);
        return;
    }
    if (row.slot >= kNativeRecoveryTimingRowTag
        && row.slot < kNativeRecoveryTimingRowTag + 3u)
    {
        const int which =
            static_cast<int>(row.slot - kNativeRecoveryTimingRowTag);
        const int cap =
            static_cast<int>(QOH99_TRAINING_RECOVERY_DELAY_STEPS_MAX);
        int values[3] = {
            static_cast<int>(status.recoveryDelaySteps),
            static_cast<int>(status.recoveryRandomMinSteps),
            static_cast<int>(status.recoveryRandomMaxSteps),
        };
        for (int &value : values)
        {
            if (value < 0) value = 0;
            if (value > cap) value = cap;
        }

        int index = 0;
        const int count = IM_ARRAYSIZE(kNativeRecoveryDelayValues);
        for (int i = 0; i < count; ++i)
        {
            if (kNativeRecoveryDelayValues[i] == values[which])
            {
                index = i;
                break;
            }
        }
        int step = delta;
        if (step == 0) step = 1;
        int next = index + step;
        while (next < 0) next += count;
        values[which] = kNativeRecoveryDelayValues[next % count];
        if (values[which] > cap) values[which] = cap;

        if (values[2] < values[1])
        {
            if (which == 1) values[2] = values[1];
            else values[1] = values[2];
        }
        const DWORD packed =
            (static_cast<DWORD>(values[1]) & 0xFFFFu)
            | ((static_cast<DWORD>(values[2]) & 0xFFFFu) << 16);
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_RECOVERY_TIMING, 0,
            static_cast<DWORD>(values[0]), packed);
        return;
    }
    if (row.slot >= kNativeSetValueRowTag
        && row.slot < kNativeSetValueRowTag + 3u)
    {
        const int which = static_cast<int>(row.slot - kNativeSetValueRowTag);
        int step = delta;
        if (step == 0) step = 1;
        int next = g_nativeSetValueIndex[which] + step;
        const int count = IM_ARRAYSIZE(kNativeSetValuePercent);
        while (next < 0) next += count;
        next %= count;
        g_nativeSetValueIndex[which] = next;
        const int pct = kNativeSetValuePercent[next];
        const DWORD target = static_cast<DWORD>(g_nativeRefillTarget);
        if (which == 0)
        {
            const DWORD maxLife = status.lifeSetMax;
            DWORD value = static_cast<DWORD>(
                (static_cast<uint64_t>(maxLife) * pct) / 100u);
            if (value < 1u) value = 1u;
            if (value > maxLife) value = maxLife;
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_LIFE_VALUE, target, value);
        }
        else if (which == 1)
        {
            /* Whole stocks go in `value` and the fraction in `flags`; the game zeroes the fraction at full stocks and the DLL rejects a full-plus-fraction pair. */
            const DWORD stocks = static_cast<DWORD>(
                (static_cast<uint64_t>(QOH99_TRAINING_EX_STOCKS_FULL) * pct)
                / 100u);
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_EX_METER, target, stocks,
                0u);
        }
        else
        {
            const DWORD gauge = static_cast<DWORD>(
                (static_cast<uint64_t>(QOH99_TRAINING_GUARD_GAUGE_FULL) * pct)
                / 100u);
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_VALUE, target,
                gauge);
        }
        return;
    }
    if (row.slot >= kNativeHudMaskRowTag
        && row.slot < kNativeHudMaskRowTag + 3u)
    {
        const DWORD index = row.slot - kNativeHudMaskRowTag;
        const DWORD masks[3] = { status.nativeHudRequestedGlobalMask,
                                 status.nativeHudRequestedP1Mask,
                                 status.nativeHudRequestedP2Mask };
        const DWORD current = masks[index];
        const DWORD next = row.value != 0 ? (current & ~row.actionValue)
                                          : (current | row.actionValue);
        EnqueueTrainingCommand(
            status, QOH99_TRAINING_COMMAND_SET_NATIVE_HUD_MASK, index, next);
        return;
    }
    if (row.slot == kNativeHudGuardRowTag)
    {
        const DWORD wanted = row.value != 0 ? 0u : QOH99_TRAINING_HUD_PLAYER_GUARD;
        const DWORD masks[2] = { status.nativeHudRequestedP1Mask,
                                 status.nativeHudRequestedP2Mask };
        for (DWORD side = 0; side < 2u; ++side)
        {
            const DWORD next =
                (masks[side] & ~QOH99_TRAINING_HUD_PLAYER_GUARD) | wanted;
            EnqueueTrainingCommand(
                status, QOH99_TRAINING_COMMAND_SET_NATIVE_HUD_MASK,
                side + 1u, next);
        }
        return;
    }
    if (row.slot == kNativeMenuBackRowTag)
    {
        if (!activate) return;
        if (g_nativeMenu.submenuChild) g_nativeMenu.submenuChild = false;
        else g_nativeMenu.submenu = -1;
        g_nativeMenu.row = 0;
        g_nativeMenu.scroll = 0;
        return;
    }
    if (row.slot == kNativeTriggerResponseRowTag)
    {
        if (!activate) return;
        g_nativeMenu.submenuChild = true;
        g_nativeMenu.row = 0;
        g_nativeMenu.scroll = 0;
        g_nativeMenu.focusPreferred = true;
        return;
    }
    if (row.slot == kNativeTriggerSetResponseTag)
    {
        if (!activate) return;
        const int event = g_nativeMenu.submenu;
        if (event < 0 || event >= QOH99_TRAINING_TRIGGER_EVENT_COUNT) return;
        if ((row.actionValue & kNativeResponseSlotFlag) != 0u)
        {
            g_trainingUi.triggerMoveIndex[event] = 0;
            g_trainingUi.triggerMacroSlot[event] =
                static_cast<int>(row.actionValue & ~kNativeResponseSlotFlag);
        }
        else
        {
            g_trainingUi.triggerMoveIndex[event] =
                static_cast<int>(row.actionValue);
            g_trainingUi.triggerMacroSlot[event] = 0;
        }
        EnqueueTrainingTriggerConfig(status, event);
        g_nativeMenu.submenuChild = false;
        g_nativeMenu.row = 0;
        g_nativeMenu.scroll = 0;
        return;
    }
    if (row.slot >= kNativeTriggerOpenRowTag
        && row.slot < kNativeTriggerOpenRowTag
                          + QOH99_TRAINING_TRIGGER_EVENT_COUNT)
    {
        if (!activate) return;
        g_nativeMenu.submenu =
            static_cast<int>(row.slot - kNativeTriggerOpenRowTag);
        g_nativeMenu.row = 0;
        g_nativeMenu.scroll = 0;
        g_nativeMenu.focusPreferred = true;
        return;
    }
    if (row.slot >= kNativeTriggerSlotRowTag)
    {
        DWORD base = 0;
        int *field = nullptr;
        const int *values = row.values;
        int count = row.choiceCount;
        if (row.slot >= kNativeTriggerChanceRowTag)
        {
            base = kNativeTriggerChanceRowTag;
        }
        else if (row.slot >= kNativeTriggerDelayRowTag)
        {
            base = kNativeTriggerDelayRowTag;
        }
        else
        {
            base = kNativeTriggerSlotRowTag;
        }
        const int event = static_cast<int>(row.slot - base);
        if (event < 0 || event >= QOH99_TRAINING_TRIGGER_EVENT_COUNT) return;
        if (base == kNativeTriggerChanceRowTag)
            field = &g_trainingUi.triggerChancePercent[event];
        else if (base == kNativeTriggerDelayRowTag)
            field = &g_trainingUi.triggerDelaySteps[event];
        else
            field = &g_trainingUi.triggerMacroSlot[event];

        int step = delta;
        if (step == 0) step = 1;
        int next = row.value < 0 ? 0 : row.value + step;
        while (next < 0) next += count;
        if (count > 0) next %= count;
        *field = values != nullptr ? values[next] : next;
        EnqueueTrainingTriggerConfig(status, event);
        return;
    }

    switch (row.kind)
    {
    case kNativeRowToggle:
        EnqueueTrainingCommand(status, row.command, row.slot,
                               row.value != 0 ? 0u : 1u);
        break;
    case kNativeRowChoice:
    {
        if (row.choiceCount <= 1) break;
        int step = delta;
        if (step == 0) step = 1;
        int next = row.value < 0 ? 0 : row.value + step;
        while (next < 0) next += row.choiceCount;
        next %= row.choiceCount;
        const DWORD payload = row.values != nullptr
            ? static_cast<DWORD>(row.values[next])
            : static_cast<DWORD>(next);
        if (row.choiceIsSlot)
            EnqueueTrainingCommand(status, row.command, payload, 0u);
        else
            EnqueueTrainingCommand(status, row.command, row.slot, payload);
        break;
    }
    case kNativeRowAction:
        if (activate)
        {
            EnqueueTrainingCommand(status, row.command, row.slot,
                                   row.actionValue);
        }
        break;
    default:
        break;
    }
}

struct PaletteStudioState
{
    bool hidden = false;
    int slot = 0;
    int variant = 0;
    int cursor = 0;
    int gridCol = 0;
    std::string message;
    bool pickerOpen = false;
    int valueEditRow = -1;
    char valueEditBuffer[8] = {};
    bool valueEditFocus = false;
};
PaletteStudioState g_paletteStudio;

enum
{
    kStudioRowSlot = 0,
    kStudioRowVariant,
    kStudioRowGrid0,
    kStudioRowGrid1,
    kStudioRowGrid2,
    kStudioRowR,
    kStudioRowG,
    kStudioRowB,
    kStudioRowBright,
    kStudioRowUndo,
    kStudioRowSave,
    kStudioRowLoad,
    kStudioRowLoadDefault,
    kStudioRowRestore,
    kStudioRowCount
};

int PaletteStudioSelectedColor()
{
    int row = 0;
    if (g_paletteStudio.cursor >= kStudioRowGrid0
        && g_paletteStudio.cursor <= kStudioRowGrid2)
    {
        row = g_paletteStudio.cursor - kStudioRowGrid0;
    }
    const int color = 1 + row * 5 + g_paletteStudio.gridCol;
    return color < kPaletteColors ? color : kPaletteColors - 1;
}

/* P1's D button from the native binding table: pointer at 0x626860, 4 players x 8 actions x {sourceType, code, callback}, action 7 = D (NativeBindingTable.h). Keyboard descriptors carry a DIK scancode. */
int PaletteStudioHideVk()
{
    const int configured = OverlayIniGet("PaletteEditorHideKey", 0);
    if (configured > 0 && configured < 256) return configured;
    if (!PaletteRangeWritable(0x626860u, 4)) return 0;
    const unsigned table = *reinterpret_cast<const unsigned *>(0x626860u);
    if (table == 0u || !PaletteRangeWritable(table + 84u, 12)) return 0;
    const unsigned *descriptor =
        reinterpret_cast<const unsigned *>(
            static_cast<uintptr_t>(table) + 84u);
    if (descriptor[0] != 0u) return 0;
    const UINT vk = MapVirtualKeyA(descriptor[1], MAPVK_VSC_TO_VK);
    return (vk > 0u && vk < 256u) ? static_cast<int>(vk) : 0;
}

unsigned PaletteStudioUsableSlots()
{
    unsigned usable = 0;
    for (int slot = 0; slot < 4; ++slot)
    {
        if ((g_paletteLocalSlotMask & (1u << slot)) == 0u) continue;
        const PaletteSlotInfo &info = kPaletteSlots[slot];
        if (!PaletteRangeWritable(info.table, kPaletteTableBytes)
            || !PaletteRangeWritable(info.indexByte, 1))
        {
            continue;
        }
        usable |= 1u << slot;
    }
    return usable;
}

void PaletteStudioRefreshStem(int slot)
{
    const PaletteSlotInfo &info = kPaletteSlots[slot];
    if (!PaletteRangeWritable(info.indexByte - 1u, 1)) return;
    const int character = static_cast<int>(
        *reinterpret_cast<const unsigned char *>(info.indexByte - 1u));
    if (character >= 0 && character < IM_ARRAYSIZE(kPaletteResourceNames))
    {
        std::snprintf(g_paletteStem[slot], sizeof(g_paletteStem[slot]),
                      "%s", kPaletteResourceNames[character]);
    }
}

void PaletteStudioTouch(int slot)
{
    PaletteUndoRecord(slot);
    PalettePublishSlot(slot);
}

void PaletteStudioAdjustChannel(int channelIndex, int direction, bool fast)
{
    const PaletteSlotInfo &info = kPaletteSlots[g_paletteStudio.slot];
    unsigned char *const table =
        reinterpret_cast<unsigned char *>(info.table);
    unsigned char *entry = table
        + g_paletteStudio.variant * kPaletteColors * 4
        + PaletteStudioSelectedColor() * 4;
    const int step = direction * (fast ? 8 : 1);
    if (channelIndex < 3)
    {
        const int byteIndex = 2 - channelIndex;
        int value = entry[byteIndex] + step;
        value = value < 0 ? 0 : (value > 255 ? 255 : value);
        entry[byteIndex] = static_cast<unsigned char>(value);
    }
    else
    {
        float r = entry[2] / 255.0f;
        float g = entry[1] / 255.0f;
        float b = entry[0] / 255.0f;
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(r, g, b, h, s, v);
        v += (fast ? 0.08f : 0.01f) * direction;
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
        entry[2] = static_cast<unsigned char>(r * 255.0f + 0.5f);
        entry[1] = static_cast<unsigned char>(g * 255.0f + 0.5f);
        entry[0] = static_cast<unsigned char>(b * 255.0f + 0.5f);
    }
    PaletteStudioTouch(g_paletteStudio.slot);
}

bool PaletteStudioDefaultAvailable()
{
    const PaletteAutoSlot &captured = g_paletteAuto[g_paletteStudio.slot];
    if (!captured.haveDefault) return false;
    const PaletteSlotInfo &info = kPaletteSlots[g_paletteStudio.slot];
    if (!PaletteRangeWritable(info.indexByte - 1u, 1)) return false;
    const int character = static_cast<int>(
        *reinterpret_cast<const unsigned char *>(info.indexByte - 1u));
    return captured.defaultCharacter == character;
}

void PaletteStudioLoadDefaultVariant()
{
    PaletteStudioState &st = g_paletteStudio;
    if (!PaletteStudioDefaultAvailable())
    {
        const PaletteAutoSlot &captured = g_paletteAuto[st.slot];
        st.message = "No authored capture for this character yet";
        PALLOG("[palette] studio: load default refused (haveDefault=%d "
               "defaultCharacter=%d)",
               captured.haveDefault ? 1 : 0, captured.defaultCharacter);
        return;
    }
    const PaletteAutoSlot &captured = g_paletteAuto[st.slot];
    const PaletteSlotInfo &info = kPaletteSlots[st.slot];
    unsigned char *const table =
        reinterpret_cast<unsigned char *>(info.table);
    const size_t offset =
        static_cast<size_t>(st.variant) * kPaletteColors * 4u;
    std::memcpy(table + offset, captured.defaultTable + offset,
                kPaletteColors * 4u);
    PaletteStudioTouch(st.slot);
    st.message = "Loaded the game's own colours for this palette";
    PALLOG("[palette] studio: %s palette %d reset to the authored default",
           info.label, st.variant + 1);
}

void PaletteStudioLoadDefaultColor()
{
    PaletteStudioState &st = g_paletteStudio;
    if (!PaletteStudioDefaultAvailable()) return;
    const PaletteAutoSlot &captured = g_paletteAuto[st.slot];
    const PaletteSlotInfo &info = kPaletteSlots[st.slot];
    unsigned char *const table =
        reinterpret_cast<unsigned char *>(info.table);
    const size_t offset =
        static_cast<size_t>(st.variant) * kPaletteColors * 4u
        + static_cast<size_t>(PaletteStudioSelectedColor()) * 4u;
    std::memcpy(table + offset, captured.defaultTable + offset, 4u);
    PaletteStudioTouch(st.slot);
}

void PaletteStudioSetChannelValue(int channelIndex, int value)
{
    const PaletteSlotInfo &info = kPaletteSlots[g_paletteStudio.slot];
    unsigned char *const table =
        reinterpret_cast<unsigned char *>(info.table);
    unsigned char *entry = table
        + g_paletteStudio.variant * kPaletteColors * 4
        + PaletteStudioSelectedColor() * 4;
    if (channelIndex < 3)
    {
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        entry[2 - channelIndex] = static_cast<unsigned char>(value);
    }
    else
    {
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        float r = entry[2] / 255.0f;
        float g = entry[1] / 255.0f;
        float b = entry[0] / 255.0f;
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(r, g, b, h, s, v);
        v = static_cast<float>(value) / 100.0f;
        ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
        entry[2] = static_cast<unsigned char>(r * 255.0f + 0.5f);
        entry[1] = static_cast<unsigned char>(g * 255.0f + 0.5f);
        entry[0] = static_cast<unsigned char>(b * 255.0f + 0.5f);
    }
    PaletteStudioTouch(g_paletteStudio.slot);
}

void PaletteStudioSetVariant(int variant)
{
    variant = (variant % kPaletteVariants + kPaletteVariants)
        % kPaletteVariants;
    g_paletteStudio.variant = variant;
    const PaletteSlotInfo &info = kPaletteSlots[g_paletteStudio.slot];
    /* The variant byte holds 16*variant and is hashed gameplay state in netplay, so this path must stay offline-only. */
    if (PaletteRangeWritable(info.indexByte, 1))
    {
        *reinterpret_cast<unsigned char *>(info.indexByte) =
            static_cast<unsigned char>(16 * variant);
    }
    PALLOG("[palette] studio: %s now previewing variant %d",
           info.label, variant + 1);
}

bool NativeMenuPageWantsSubTabs(int page)
{
    return page == kTrainingPageQuick || page == kTrainingPageDummy
        || page == kTrainingPageRecording || page == kTrainingPageView;
}

const char *NativeMenuSubTabShortLabel(const char *heading)
{
    static const struct { const char *full; const char *brief; } kMap[] = {
        { "RECOVERY AFTER BEING HIT", "Recovery" },
        { "RECOVERY TIMING", "Timing" },
        { "CRITICAL HITS", "Crits" },
        { "THROW TECH", "Throws" },
        { "RESTORE AUTOMATICALLY", "Auto restore" },
        { "REFILL NOW", "Refill" },
        { "SET VALUES", "Values" },
        { "EX CHARGE CONTROL", "EX charge" },
        { "SAVE STATES", "States" },
        { "GAME HUD", "HUD" },
        { "POSITION", "Position" },
        { "SESSION", "Session" },
        { "GUARD", "Guard" },
        { "STANCE", "Stance" },
        { "DEFENCE", "Defence" },
        { "RECORDING", "Recording" },
        { "BOXES", "Overlay" },
        { "INFO BELOW", "Info below" },
    };
    for (const auto &entry : kMap)
    {
        if (std::strcmp(entry.full, heading) == 0) return entry.brief;
    }
    return heading;
}

void NativeMenuApplySubTabs(NativeMenuPageBuild &build, int page)
{
    if (!NativeMenuPageWantsSubTabs(page)) return;
    int headAt[kNativeMenuMaxSubTabs];
    static const char *s_labels[kNativeMenuMaxSubTabs];
    int heads = 0;
    for (int i = 0; i < build.count && heads < kNativeMenuMaxSubTabs; ++i)
    {
        const NativeRow &row = build.rows[i];
        if (row.kind == kNativeRowHeading && row.label != nullptr
            && row.label[0] != '\0')
        {
            headAt[heads] = i;
            s_labels[heads] =
                qi18n::tr(NativeMenuSubTabShortLabel(row.label));
            ++heads;
        }
    }
    for (int k = 0; k < heads && heads >= 2;)
    {
        const int from = headAt[k] + 1;
        const int to = k + 1 < heads ? headAt[k + 1] : build.count;
        int selectable = 0;
        for (int i = from; i < to; ++i)
        {
            if (NativeRowSelectable(build.rows[i])) ++selectable;
        }
        if (selectable >= 3)
        {
            ++k;
            continue;
        }
        const int removeAt = k == 0 ? 1 : k;
        for (int m = removeAt; m + 1 < heads; ++m)
        {
            headAt[m] = headAt[m + 1];
            s_labels[m] = s_labels[m + 1];
        }
        --heads;
        k = 0;
    }
    if (heads < 2) return;

    int &active = g_nativeMenu.subTab[page];
    if (active < 0 || active >= heads) active = 0;

    const int begin = headAt[active] + 1;
    const int end = active + 1 < heads ? headAt[active + 1] : build.count;
    int keep = end - begin;
    if (keep < 0) keep = 0;

    NativeRow strip = NativeRow();
    strip.kind = kNativeRowSubTabs;
    strip.label = "";
    strip.enabled = true;
    strip.choices = s_labels;
    strip.choiceCount = heads;
    strip.value = active;
    strip.hint = "Left/Right or click: switch section";

    std::memmove(&build.rows[1], &build.rows[begin],
                 static_cast<size_t>(keep) * sizeof(NativeRow));
    build.rows[0] = strip;
    build.count = 1 + keep;
    while (build.count > 1
           && (build.rows[1].kind == kNativeRowSpacer))
    {
        std::memmove(&build.rows[1], &build.rows[2],
                     static_cast<size_t>(build.count - 2)
                         * sizeof(NativeRow));
        --build.count;
    }
    while (build.count > 1
           && build.rows[build.count - 1].kind == kNativeRowSpacer)
    {
        --build.count;
    }
}

bool g_nativeMenuPaletteVisited = false;

bool NativeMenuPaletteModalActive()
{
    return g_paletteStudio.pickerOpen || g_paletteStudio.valueEditRow >= 0;
}

void NativeMenuPaletteOpenValueEdit(int rowIndex)
{
    PaletteStudioState &st = g_paletteStudio;
    const PaletteSlotInfo &info = kPaletteSlots[st.slot];
    const unsigned char *entry =
        reinterpret_cast<const unsigned char *>(info.table)
        + st.variant * kPaletteColors * 4
        + PaletteStudioSelectedColor() * 4;
    int current = 0;
    switch (rowIndex)
    {
        case kStudioRowR: current = entry[2]; break;
        case kStudioRowG: current = entry[1]; break;
        case kStudioRowB: current = entry[0]; break;
        default:
        {
            float r = entry[2] / 255.0f, g = entry[1] / 255.0f,
                  b = entry[0] / 255.0f, h, s, v;
            ImGui::ColorConvertRGBtoHSV(r, g, b, h, s, v);
            current = static_cast<int>(v * 100.0f + 0.5f);
            break;
        }
    }
    st.valueEditRow = rowIndex;
    st.valueEditFocus = true;
    st.pickerOpen = false;
    std::snprintf(st.valueEditBuffer, sizeof(st.valueEditBuffer), "%d",
                  current);
}

void NativeMenuPaletteStylePush()
{
    using namespace NativeMenuStyle;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 14, 28, 246));
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(12, 14, 28, 255));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(28, 32, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(30, 34, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                          IM_COL32(44, 50, 84, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                          IM_COL32(54, 60, 98, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30, 34, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          IM_COL32(60, 66, 104, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          IM_COL32(82, 88, 130, 255));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, kTabActive);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kTabActive);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kTabActive);
}

void NativeMenuPaletteStylePop()
{
    ImGui::PopStyleColor(14);
    ImGui::PopStyleVar(2);
}

void NativeMenuPaletteEnter()
{
    PaletteStudioState &st = g_paletteStudio;
    const unsigned usable = PaletteStudioUsableSlots();
    if (usable == 0u) return;
    if ((usable & (1u << st.slot)) == 0u)
    {
        for (int slot = 0; slot < 4; ++slot)
            if (usable & (1u << slot)) { st.slot = slot; break; }
    }
    const PaletteSlotInfo &info = kPaletteSlots[st.slot];
    st.variant = (*reinterpret_cast<unsigned char *>(info.indexByte) / 16)
        % kPaletteVariants;
    if (st.cursor >= kStudioRowCount) st.cursor = 0;
    st.message.clear();
    st.pickerOpen = false;
    st.valueEditRow = -1;
    PaletteStudioRefreshStem(st.slot);
    PaletteBackup &backup = g_paletteBackup[st.slot];
    unsigned char *const table =
        reinterpret_cast<unsigned char *>(info.table);
    if (!backup.valid) PaletteTakeSnapshot(st.slot, table);
    PaletteAutoSlot &captured = g_paletteAuto[st.slot];
    if (!captured.haveDefault && !captured.haveCustom
        && PaletteTableLooksAuthored(table)
        && PaletteRangeWritable(info.indexByte - 1u, 1))
    {
        std::memcpy(captured.defaultTable, table, kPaletteTableBytes);
        captured.haveDefault = true;
        captured.usingCustom = false;
        captured.defaultCharacter = static_cast<int>(
            *reinterpret_cast<const unsigned char *>(info.indexByte - 1u));
        PALLOG("[palette] studio: captured %s's authored colours on page "
               "entry (the auto capture was missing)", info.label);
    }
    PaletteUndoExternalSync(st.slot);
}

int NativeMenuPaletteHeldLR(bool &fastStep)
{
    static ULONGLONG heldSince = 0;
    static ULONGLONG lastRepeat = 0;
    static int heldDirection = 0;
    const bool left = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
    const bool right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    fastStep = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const int direction = right ? 1 : (left ? -1 : 0);
    const ULONGLONG nowMs = GetTickCount64();
    if (direction == 0 || direction != heldDirection)
    {
        heldDirection = direction;
        heldSince = nowMs;
        lastRepeat = 0;
        return direction;
    }
    if (nowMs - heldSince >= 260ull && nowMs - lastRepeat >= 35ull)
    {
        lastRepeat = nowMs;
        return direction;
    }
    return 0;
}

void NativeMenuPaletteInput(const NativeMenuInput &in)
{
    PaletteStudioState &st = g_paletteStudio;
    const unsigned usable = PaletteStudioUsableSlots();
    const bool offline = g_paletteLocalSlotMask == 0xFu;

    if (NativeMenuPaletteModalActive()) return;

    static bool dWasDown = false;
    const int hideVk = PaletteStudioHideVk();
    const bool dDown = hideVk != 0
        && (GetAsyncKeyState(hideVk) & 0x8000) != 0;
    const bool dEdge = dDown && !dWasDown;
    dWasDown = dDown;
    if (dEdge)
    {
        st.hidden = !st.hidden;
        return;
    }
    if (st.hidden)
    {
        if (in.back || in.commit) st.hidden = false;
        return;
    }
    if (usable == 0u) return;

    {
        static bool zWasDown = false, yWasDown = false;
        const bool ctrlHeld =
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool zDown =
            ctrlHeld && (GetAsyncKeyState('Z') & 0x8000) != 0;
        const bool yDown =
            ctrlHeld && (GetAsyncKeyState('Y') & 0x8000) != 0;
        if (zDown && !zWasDown)
        {
            st.message = PaletteStudioUndo(st.slot)
                ? "Undid one colour change" : "Nothing to undo";
        }
        if (yDown && !yWasDown)
        {
            st.message = PaletteStudioRedo(st.slot)
                ? "Redid one colour change" : "Nothing to redo";
        }
        zWasDown = zDown;
        yWasDown = yDown;
    }

    const auto selectable = [&](int row) {
        if (row == kStudioRowVariant && !offline) return false;
        return true;
    };
    if (in.up)
    {
        if (st.cursor == kStudioRowSlot)
        {
            g_nativeMenu.tabFocus = true;
            return;
        }
        int row = st.cursor;
        do { row = (row + kStudioRowCount - 1) % kStudioRowCount; }
        while (!selectable(row));
        st.cursor = row;
    }
    if (in.down)
    {
        if (st.cursor == kStudioRowRestore)
        {
            g_nativeMenu.tabFocus = true;
            return;
        }
        int row = st.cursor;
        do { row = (row + 1) % kStudioRowCount; }
        while (!selectable(row));
        st.cursor = row;
    }

    int direction = in.right ? 1 : (in.left ? -1 : 0);
    bool fastStep = false;
    if (st.cursor >= kStudioRowR && st.cursor <= kStudioRowBright)
    {
        direction = NativeMenuPaletteHeldLR(fastStep);
    }

    switch (st.cursor)
    {
        case kStudioRowSlot:
        {
            if (direction == 0) break;
            int slot = st.slot;
            for (int step = 0; step < 4; ++step)
            {
                slot = (slot + direction + 4) % 4;
                if (usable & (1u << slot)) break;
            }
            if (slot != st.slot && (usable & (1u << slot)))
            {
                st.slot = slot;
                NativeMenuPaletteEnter();
            }
            break;
        }
        case kStudioRowVariant:
            if (direction != 0 && offline)
                PaletteStudioSetVariant(st.variant + direction);
            break;
        case kStudioRowGrid0:
        case kStudioRowGrid1:
        case kStudioRowGrid2:
            if (direction != 0)
                st.gridCol = (st.gridCol + direction + 5) % 5;
            if (in.commit)
            {
                st.pickerOpen = true;
                st.valueEditRow = -1;
            }
            break;
        case kStudioRowR:
        case kStudioRowG:
        case kStudioRowB:
        case kStudioRowBright:
            if (direction != 0)
                PaletteStudioAdjustChannel(st.cursor - kStudioRowR,
                                           direction, fastStep);
            if (in.commit) NativeMenuPaletteOpenValueEdit(st.cursor);
            break;
        case kStudioRowUndo:
            if (direction < 0 || (in.commit && direction == 0))
                st.message = PaletteStudioUndo(st.slot)
                    ? "Undid one colour change" : "Nothing to undo";
            else if (direction > 0)
                st.message = PaletteStudioRedo(st.slot)
                    ? "Redid one colour change" : "Nothing to redo";
            break;
        case kStudioRowSave:
            if (in.commit)
            {
                const PaletteSlotInfo &info = kPaletteSlots[st.slot];
                (void)PaletteSaveVariant(
                    reinterpret_cast<unsigned char *>(info.table),
                    st.variant, g_paletteStem[st.slot], st.message);
            }
            break;
        case kStudioRowLoad:
            if (in.commit)
            {
                const PaletteSlotInfo &info = kPaletteSlots[st.slot];
                if (PaletteLoadVariant(
                        reinterpret_cast<unsigned char *>(info.table),
                        st.variant, g_paletteStem[st.slot], st.message))
                {
                    PaletteStudioTouch(st.slot);
                }
            }
            break;
        case kStudioRowLoadDefault:
            if (in.commit) PaletteStudioLoadDefaultVariant();
            break;
        case kStudioRowRestore:
            if (in.commit)
            {
                PaletteBackup &backup = g_paletteBackup[st.slot];
                const PaletteSlotInfo &info = kPaletteSlots[st.slot];
                if (backup.valid)
                {
                    std::memcpy(reinterpret_cast<void *>(info.table),
                                backup.bytes, kPaletteTableBytes);
                    PaletteStudioTouch(st.slot);
                    st.message =
                        "Restored the colours this match started on";
                }
            }
            break;
        default: break;
    }
}

constexpr uintptr_t kQohFighterBases[4] = {
    0x623E00u, 0x626880u, 0x4ECB40u, 0x617A20u,
};
constexpr int kPaletteSpriteCanvas = 512;

struct PaletteSpritePreview
{
    IDirect3DTexture9 *texture = nullptr;
    IDirect3DDevice9 *device = nullptr;
    unsigned hash = 0;
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool hasContent = false;
};
PaletteSpritePreview g_palettePreview;

struct PaletteSpriteSource
{
    uintptr_t actor = 0;
    int relX = 0, relY = 0;
    uintptr_t frameRec = 0;
    unsigned composite = 0;
    int transform = 0;
    int offX = 0, offY = 0;
    int variantByte = 0;
};

void PaletteSpriteTransform(int x, int y, int w, int h, int transform,
                            int &outX, int &outY)
{
    switch (transform & 7)
    {
        default:
        case 0: outX = x;         outY = y;         break;
        case 1: outX = h - 1 - y; outY = x;         break;
        case 2: outX = w - 1 - x; outY = h - 1 - y; break;
        case 3: outX = y;         outY = w - 1 - x; break;
        case 4: outX = w - 1 - x; outY = y;         break;
        case 5: outX = y;         outY = x;         break;
        case 6: outX = x;         outY = h - 1 - y; break;
        case 7: outX = h - 1 - y; outY = w - 1 - x; break;
    }
}

bool PaletteSpriteReadActor(uintptr_t actor, PaletteSpriteSource &src)
{
    if (!PaletteRangeWritable(actor, 1440)) return false;
    const uintptr_t frameRec =
        *reinterpret_cast<const uintptr_t *>(actor + 1432u);
    if (frameRec == 0 || !PaletteRangeWritable(frameRec, 64)) return false;
    const unsigned composite = *reinterpret_cast<const unsigned *>(frameRec);
    const unsigned compositeCount =
        *reinterpret_cast<const unsigned *>(actor + 12u);
    if (composite >= compositeCount) return false;
    const uintptr_t compTable =
        *reinterpret_cast<const uintptr_t *>(actor + 8u);
    const uintptr_t tileTable = *reinterpret_cast<const uintptr_t *>(actor);
    if (compTable == 0 || tileTable == 0) return false;
    if (!PaletteRangeWritable(compTable + 12u * composite, 12)) return false;
    src.actor = actor;
    src.frameRec = frameRec;
    src.composite = composite;
    src.transform =
        (*reinterpret_cast<const unsigned char *>(frameRec + 21u) & 7)
        ^ (*reinterpret_cast<const unsigned char *>(actor + 170u) != 0 ? 4
                                                                       : 0);
    src.offX = *reinterpret_cast<const short *>(frameRec + 4u);
    src.offY = *reinterpret_cast<const short *>(frameRec + 6u);
    /* +175 is the worn 16*variant byte on fighters AND on their clones. */
    src.variantByte =
        *reinterpret_cast<const unsigned char *>(actor + 175u) & 0x30;
    return true;
}

/* Pools: 0x526FA0 x120 and 0x618160 x30; +174 == -1 marks a free slot;
   actor+1416 is the colour LUT. */
int PaletteSpriteGather(int slot, PaletteSpriteSource *out, int cap)
{
    if (slot < 0 || slot >= 4 || cap < 1) return 0;
    if (!PaletteRangeWritable(kQohSceneCurrent, 4)
        || *reinterpret_cast<const int *>(kQohSceneCurrent) != 2)
        return 0;
    const uintptr_t fighter = kQohFighterBases[slot];
    if (!PaletteSpriteReadActor(fighter, out[0])) return 0;
    int count = 1;
    const uintptr_t lut =
        *reinterpret_cast<const uintptr_t *>(fighter + 1416u);
    if (lut == 0) return count;
    const int fighterX = *reinterpret_cast<const int *>(fighter + 136u);
    const int fighterY = *reinterpret_cast<const int *>(fighter + 140u);
    static const struct { uintptr_t base; int slots; } kPools[2] = {
        {0x526FA0u, 120},
        {0x618160u, 30},
    };
    for (int pool = 0; pool < 2 && count < cap; ++pool)
    {
        for (int i = 0; i < kPools[pool].slots && count < cap; ++i)
        {
            const uintptr_t actor =
                kPools[pool].base + 1532u * static_cast<uintptr_t>(i);
            if (!PaletteRangeWritable(actor, 1440)) break;
            if (*reinterpret_cast<const signed char *>(actor + 174u) == -1)
                continue;
            if (*reinterpret_cast<const uintptr_t *>(actor + 1416u) != lut)
                continue;
            PaletteSpriteSource src{};
            if (!PaletteSpriteReadActor(actor, src)) continue;
            src.relX =
                *reinterpret_cast<const int *>(actor + 136u) - fighterX;
            src.relY =
                *reinterpret_cast<const int *>(actor + 140u) - fighterY;
            if (src.relX < -192 || src.relX > 192
                || src.relY < -224 || src.relY > 224)
                continue;
            out[count++] = src;
        }
    }
    return count;
}

bool NativeMenuPaletteSpriteRefresh(int slot)
{
    PaletteSpritePreview &pv = g_palettePreview;
    if (slot < 0 || slot >= 4) return false;
    if (g_presenter.device == nullptr) return false;
    PaletteSpriteSource sources[9];
    const int count = PaletteSpriteGather(slot, sources, 9);
    if (count == 0) return false;
    const PaletteSlotInfo &info = kPaletteSlots[slot];
    if (!PaletteRangeWritable(info.table, kPaletteTableBytes)) return false;

    unsigned hash = 2166136261u;
    const auto fold = [&hash](unsigned value) {
        hash = (hash ^ value) * 16777619u;
    };
    fold(static_cast<unsigned>(slot));
    fold(g_paletteGeneration);
    fold(static_cast<unsigned>(count));
    for (int i = 0; i < count; ++i)
    {
        fold(static_cast<unsigned>(sources[i].actor));
        fold(static_cast<unsigned>(sources[i].frameRec));
        fold(sources[i].composite);
        fold(static_cast<unsigned>(sources[i].transform));
        fold(static_cast<unsigned>(sources[i].offX * 131 + sources[i].offY));
        fold(static_cast<unsigned>(sources[i].relX * 131 + sources[i].relY));
        fold(static_cast<unsigned>(sources[i].variantByte));
    }

    if (pv.texture != nullptr && pv.device != g_presenter.device)
    {
        pv.texture->Release();
        pv.texture = nullptr;
        pv.device = nullptr;
    }
    if (pv.texture != nullptr && pv.hash == hash) return pv.hasContent;
    if (pv.texture == nullptr)
    {
        if (FAILED(g_presenter.device->CreateTexture(
                kPaletteSpriteCanvas, kPaletteSpriteCanvas, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pv.texture, nullptr)))
        {
            pv.texture = nullptr;
            return false;
        }
        pv.device = g_presenter.device;
    }

    static unsigned s_pixels[kPaletteSpriteCanvas * kPaletteSpriteCanvas];
    std::memset(s_pixels, 0, sizeof(s_pixels));
    const unsigned char *const palette =
        reinterpret_cast<const unsigned char *>(info.table);
    int minX = kPaletteSpriteCanvas, minY = kPaletteSpriteCanvas;
    int maxX = -1, maxY = -1;
    const int bias = 128;
    for (int s = 0; s < count; ++s)
    {
        const PaletteSpriteSource &source = sources[s];
        const uintptr_t compTable =
            *reinterpret_cast<const uintptr_t *>(source.actor + 8u);
        const uintptr_t tileTable =
            *reinterpret_cast<const uintptr_t *>(source.actor);
        const uintptr_t comp = compTable + 12u * source.composite;
        const unsigned firstTile =
            *reinterpret_cast<const unsigned *>(comp);
        unsigned tileCount =
            *reinterpret_cast<const unsigned *>(comp + 4u);
        int canvasW =
            *reinterpret_cast<const unsigned short *>(comp + 8u);
        int canvasH =
            *reinterpret_cast<const unsigned short *>(comp + 10u);
        if (canvasW == 0) canvasW = 256;
        if (canvasH == 0) canvasH = 256;
        if (tileCount > 512u) tileCount = 512u;
        const int baseX = source.offX + source.relX + bias;
        const int baseY = source.offY + source.relY + bias;
        for (unsigned tileIndex = 0; tileIndex < tileCount; ++tileIndex)
        {
            const uintptr_t tile =
                tileTable + 260u * (firstTile + tileIndex);
            if (!PaletteRangeWritable(tile, 260)) break;
            const int tx = *reinterpret_cast<const short *>(tile);
            const int ty = *reinterpret_cast<const short *>(tile + 2u);
            const unsigned char *src =
                reinterpret_cast<const unsigned char *>(tile + 4u);
            for (int py = 0; py < 16; ++py)
            {
                for (int px = 0; px < 16; ++px)
                {
                    const unsigned char pix = src[py * 16 + px];
                    if (pix == 0) continue;
                    int dx = 0, dy = 0;
                    PaletteSpriteTransform(tx + px, ty + py, canvasW,
                                           canvasH, source.transform,
                                           dx, dy);
                    dx += baseX;
                    dy += baseY;
                    if (dx < 0 || dy < 0 || dx >= kPaletteSpriteCanvas
                        || dy >= kPaletteSpriteCanvas)
                        continue;
                    int index = source.variantByte + pix;
                    if (index > 63) index = 63;
                    const unsigned char *entry = palette + index * 4;
                    s_pixels[dy * kPaletteSpriteCanvas + dx] =
                        0xFF000000u
                        | (static_cast<unsigned>(entry[2]) << 16)
                        | (static_cast<unsigned>(entry[1]) << 8)
                        | entry[0];
                    if (dx < minX) minX = dx;
                    if (dy < minY) minY = dy;
                    if (dx > maxX) maxX = dx;
                    if (dy > maxY) maxY = dy;
                }
            }
        }
    }

    pv.hash = hash;
    pv.hasContent = maxX >= minX && maxY >= minY;
    if (!pv.hasContent) return false;
    pv.minX = minX;
    pv.minY = minY;
    pv.maxX = maxX;
    pv.maxY = maxY;

    D3DLOCKED_RECT locked{};
    if (FAILED(pv.texture->LockRect(0, &locked, nullptr, 0)))
    {
        pv.hasContent = false;
        return false;
    }
    for (int y = 0; y < kPaletteSpriteCanvas; ++y)
    {
        std::memcpy(static_cast<unsigned char *>(locked.pBits)
                        + static_cast<size_t>(y) * locked.Pitch,
                    s_pixels + static_cast<size_t>(y) * kPaletteSpriteCanvas,
                    static_cast<size_t>(kPaletteSpriteCanvas) * 4u);
    }
    pv.texture->UnlockRect(0);
    return true;
}

void NativeMenuPaletteBody(ImDrawList *dl, ImFont *font,
                           const NativeMenuMetrics &m, float contentX,
                           float contentW, float y, const ImVec2 &mouse,
                           bool mouseActive, bool clicked)
{
    using namespace NativeMenuStyle;
    PaletteStudioState &st = g_paletteStudio;
    const bool mouseFree = mouseActive
        && !ImGui::GetIO().WantCaptureMouse
        && st.valueEditRow < 0
        && !st.pickerOpen;
    const bool doubleClicked =
        mouseFree && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    float editRowY = -1.0f;
    const unsigned usable = PaletteStudioUsableSlots();
    if (usable == 0u)
    {
        NativeMenuText(dl, font, m.fontSize, ImVec2(contentX, y), kTextDim,
                       "No palette to edit here: no owned fighter is "
                       "loaded, or this seat only watches.");
        return;
    }
    const PaletteSlotInfo &info = kPaletteSlots[st.slot];
    unsigned char *const table =
        reinterpret_cast<unsigned char *>(info.table);
    const bool offline = g_paletteLocalSlotMask == 0xFu;
    const float rowStep = m.lineHeight;
    const bool previewShown = NativeMenuPaletteSpriteRefresh(st.slot);
    const float rowsW = previewShown ? contentW * 0.70f : contentW;
    const float valueX = contentX + contentW * 0.5f;
    char value[64];

    const auto row = [&](int index, const char *label, const char *text,
                         bool enabled) -> int {
        if (index == st.valueEditRow) editRowY = y;
        const bool hovered = mouseFree
            && mouse.x >= contentX - m.pad
            && mouse.x <= contentX + rowsW
            && mouse.y >= y && mouse.y <= y + rowStep * 0.9f;
        if (hovered && enabled)
        {
            st.cursor = index;
            g_nativeMenu.tabFocus = false;
        }
        const bool selected = st.cursor == index;
        if (selected)
        {
            dl->AddRectFilled(
                ImVec2(contentX - m.pad, y - rowStep * 0.08f),
                ImVec2(contentX + rowsW, y + rowStep * 0.86f),
                kRowActive);
            NativeMenuCursor(dl,
                             ImVec2(contentX - m.pad * 0.4f,
                                    y + m.fontSize * 0.22f),
                             m.fontSize * 0.55f, kCursor);
        }
        NativeMenuText(dl, font, m.fontSize,
                       ImVec2(contentX + m.pad, y),
                       enabled ? kText : kTextDim, label);
        if (text != nullptr)
        {
            NativeMenuText(dl, font, m.fontSize, ImVec2(valueX, y),
                           enabled ? kTextValue : kTextDim, text);
        }
        y += rowStep;
        const bool channelValue =
            index >= kStudioRowR && index <= kStudioRowBright;
        const bool action = index >= kStudioRowSave;
        if (hovered && enabled && doubleClicked && channelValue) return 2;
        if (hovered && enabled && clicked)
        {
            if (action) return 1;
            if (mouse.x < valueX) return 0;
            const float valueWidth = text != nullptr
                ? font->CalcTextSizeA(m.fontSize, FLT_MAX, 0.0f, text).x
                : 0.0f;
            return mouse.x >= valueX + valueWidth * 0.5f ? 1 : -1;
        }
        return 0;
    };

    std::snprintf(value, sizeof(value), "< %s  (%s) >", info.label,
                  g_paletteStem[st.slot]);
    {
        const int step = row(kStudioRowSlot, "Slot", value, true);
        if (step != 0)
        {
            int slot = st.slot;
            for (int i = 0; i < 4; ++i)
            {
                slot = (slot + step + 4) % 4;
                if (usable & (1u << slot)) break;
            }
            if (slot != st.slot && (usable & (1u << slot)))
            {
                st.slot = slot;
                NativeMenuPaletteEnter();
            }
        }
    }
    std::snprintf(value, sizeof(value), "< %d of %d >%s",
                  st.variant + 1, kPaletteVariants,
                  offline ? "" : "  netplay: fixed");
    {
        const int step = row(kStudioRowVariant, "Palette", value, offline);
        if (step != 0 && offline)
            PaletteStudioSetVariant(st.variant + step);
    }
    y += rowStep * 0.25f;
    const float previewTop = y;

    const float cell = m.lineHeight * 1.35f;
    const float cellGap = m.lineHeight * 0.30f;
    for (int gridRow = 0; gridRow < 3; ++gridRow)
    {
        const bool rowSelected = st.cursor == kStudioRowGrid0 + gridRow;
        if (rowSelected)
        {
            NativeMenuCursor(dl,
                             ImVec2(contentX - m.pad * 0.4f,
                                    y + cell * 0.4f),
                             m.fontSize * 0.55f, kCursor);
        }
        for (int col = 0; col < 5; ++col)
        {
            const int color = 1 + gridRow * 5 + col;
            const unsigned char *entry = table
                + st.variant * kPaletteColors * 4 + color * 4;
            const ImVec2 a(contentX + m.pad + col * (cell + cellGap), y);
            const ImVec2 b(a.x + cell, y + cell);
            const bool hovered = mouseFree
                && mouse.x >= a.x && mouse.x <= b.x
                && mouse.y >= a.y && mouse.y <= b.y;
            if (hovered && clicked)
            {
                st.cursor = kStudioRowGrid0 + gridRow;
                st.gridCol = col;
                g_nativeMenu.tabFocus = false;
                st.pickerOpen = true;
                st.valueEditRow = -1;
            }
            dl->AddRectFilled(a, b,
                              IM_COL32(entry[2], entry[1], entry[0], 255));
            const bool selected =
                st.cursor == kStudioRowGrid0 + gridRow
                && st.gridCol == col;
            dl->AddRect(ImVec2(a.x - 1, a.y - 1),
                        ImVec2(b.x + 1, b.y + 1),
                        selected ? kCursor
                                 : (hovered ? kTextDim : kBorderInner),
                        0.0f, 0, (selected || hovered) ? 2.0f : 1.0f);
        }
        y += cell + cellGap;
    }
    y += rowStep * 0.35f;

    const int color = PaletteStudioSelectedColor();
    unsigned char *entry = table + st.variant * kPaletteColors * 4
        + color * 4;
    float r = entry[2] / 255.0f, g = entry[1] / 255.0f,
          b = entry[0] / 255.0f, h, s, v;
    ImGui::ColorConvertRGBtoHSV(r, g, b, h, s, v);
    std::snprintf(value, sizeof(value), "colour %d    #%02X%02X%02X",
                  color, entry[2], entry[1], entry[0]);
    NativeMenuText(dl, font, m.fontSize * 0.92f,
                   ImVec2(contentX + m.pad, y), kTextDim, value);
    y += rowStep;

    const bool coarse = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto channelRow = [&](int rowIndex, int channel,
                                const char *label, const char *text) {
        const int step = row(rowIndex, label, text, true);
        if (step == 2) NativeMenuPaletteOpenValueEdit(rowIndex);
        else if (step != 0)
            PaletteStudioAdjustChannel(channel, step, coarse);
    };
    std::snprintf(value, sizeof(value), "< %d >", entry[2]);
    channelRow(kStudioRowR, 0, "Red", value);
    std::snprintf(value, sizeof(value), "< %d >", entry[1]);
    channelRow(kStudioRowG, 1, "Green", value);
    std::snprintf(value, sizeof(value), "< %d >", entry[0]);
    channelRow(kStudioRowB, 2, "Blue", value);
    std::snprintf(value, sizeof(value), "< %d%% >",
                  static_cast<int>(v * 100.0f + 0.5f));
    channelRow(kStudioRowBright, 3, "Brightness", value);
    y += rowStep * 0.25f;

    {
        const PaletteUndoState &history = g_paletteUndoHistory[st.slot];
        std::snprintf(value, sizeof(value), "< %d | %d >",
                      history.undoCount, history.redoCount);
        const int step = row(kStudioRowUndo, "Undo / redo", value,
                             history.undoCount > 0
                                 || history.redoCount > 0);
        if (step < 0)
        {
            st.message = PaletteStudioUndo(st.slot)
                ? "Undid one colour change" : "Nothing to undo";
        }
        else if (step > 0)
        {
            st.message = PaletteStudioRedo(st.slot)
                ? "Redid one colour change" : "Nothing to redo";
        }
    }
    y += rowStep * 0.25f;

    std::snprintf(value, sizeof(value), "%s_%d.pal",
                  g_paletteStem[st.slot], st.variant + 1);
    if (row(kStudioRowSave, "Save palette", value, true) != 0)
    {
        (void)PaletteSaveVariant(table, st.variant,
                                 g_paletteStem[st.slot], st.message);
    }
    if (row(kStudioRowLoad, "Load palette", value, true) != 0)
    {
        if (PaletteLoadVariant(table, st.variant,
                               g_paletteStem[st.slot], st.message))
        {
            PaletteStudioTouch(st.slot);
        }
    }
    if (row(kStudioRowLoadDefault, "Load default palette",
            PaletteStudioDefaultAvailable() ? "game's own colours"
                                            : "not captured yet",
            true) != 0)
    {
        PaletteStudioLoadDefaultVariant();
    }
    if (row(kStudioRowRestore, "Restore match colours", nullptr, true) != 0)
    {
        PaletteBackup &backup = g_paletteBackup[st.slot];
        if (backup.valid)
        {
            std::memcpy(reinterpret_cast<void *>(info.table),
                        backup.bytes, kPaletteTableBytes);
            PaletteStudioTouch(st.slot);
            st.message = "Restored the colours this match started on";
        }
    }

    const char *hint = st.message.empty()
        ? "D hide menu    Shift fast    click colour = picker    "
          "double-click value = type it    Ctrl+Z/Y undo-redo"
        : st.message.c_str();
    const float hintSize = m.fontSize * 0.85f;
    const float hintLine = hintSize * 1.18f;
    const float hintTop = m.panelMax.y - m.pad - hintLine * 4.0f;
    static NativeTextScroll paletteHintScroll;
    static NativeTextScroll paletteUsageScroll;
    NativeMenuTextWrapped(dl, font, hintSize,
                          ImVec2(contentX + m.pad, hintTop),
                          rowsW - m.pad * 2.0f, kTextDim, hint, 2,
                          &paletteHintScroll);
    NativeMenuTextWrapped(
        dl, font, hintSize,
        ImVec2(contentX + m.pad, hintTop + hintLine * 2.0f),
        rowsW - m.pad * 2.0f, kTextDim,
        "At character select: press the D attack while picking a colour "
        "to wear the palette saved for that slot.", 2,
        &paletteUsageScroll);

    if (previewShown)
    {
        const PaletteSpritePreview &pv = g_palettePreview;
        const float srcW = static_cast<float>(pv.maxX - pv.minX + 1);
        const float srcH = static_cast<float>(pv.maxY - pv.minY + 1);
        const float boxW = contentW * 0.26f;
        float boxH = (m.panelMax.y - m.pad * 2.0f) - previewTop
            - m.lineHeight * 3.2f;
        if (boxH > boxW * 1.25f) boxH = boxW * 1.25f;
        if (boxH >= m.lineHeight * 3.0f && srcW > 0.0f && srcH > 0.0f)
        {
            float scale = boxW / srcW;
            if (srcH * scale > boxH) scale = boxH / srcH;
            if (scale > 3.0f) scale = 3.0f;
            const float drawW = srcW * scale;
            const float drawH = srcH * scale;
            const ImVec2 p0(contentX + contentW - boxW, previewTop);
            const ImVec2 q0(p0.x + (boxW - drawW) * 0.5f, p0.y);
            const ImVec2 q1(q0.x + drawW, q0.y + drawH);
            const float inv =
                1.0f / static_cast<float>(kPaletteSpriteCanvas);
            dl->AddImage((ImTextureID)(uintptr_t)pv.texture, q0, q1,
                         ImVec2(pv.minX * inv, pv.minY * inv),
                         ImVec2((pv.maxX + 1) * inv, (pv.maxY + 1) * inv));
            char caption[48];
            std::snprintf(caption, sizeof(caption), "%s - live",
                          info.label);
            NativeMenuText(dl, font, m.fontSize * 0.8f,
                           ImVec2(p0.x + (boxW - drawW) * 0.5f,
                                  q1.y + 3.0f),
                           kTextDim, caption);
        }
    }

    if (st.pickerOpen)
    {
        NativeMenuPaletteStylePush();
        ImGui::SetNextWindowPos(
            ImVec2(contentX + m.pad * 2.0f,
                   m.panelMin.y + m.lineHeight * 4.5f),
            ImGuiCond_Appearing);
        char pickerTitle[96];
        std::snprintf(pickerTitle, sizeof(pickerTitle),
                      "%s  -  palette %d  -  colour %d"
                      "###qoh99_palette_picker",
                      info.label, st.variant + 1, color);
        bool keepOpen = true;
        if (ImGui::Begin(pickerTitle, &keepOpen,
                         ImGuiWindowFlags_NoCollapse
                             | ImGuiWindowFlags_AlwaysAutoResize))
        {
            float rgb[3] = {entry[2] / 255.0f, entry[1] / 255.0f,
                            entry[0] / 255.0f};
            const float pickerWidth =
                m.fontSize * 12.0f < 340.0f ? m.fontSize * 12.0f : 340.0f;
            ImGui::SetNextItemWidth(pickerWidth);
            if (ImGui::ColorPicker3(
                    "##picker", rgb,
                    ImGuiColorEditFlags_NoSidePreview
                        | ImGuiColorEditFlags_NoSmallPreview
                        | ImGuiColorEditFlags_DisplayRGB
                        | ImGuiColorEditFlags_DisplayHSV
                        | ImGuiColorEditFlags_DisplayHex))
            {
                entry[2] = static_cast<unsigned char>(
                    rgb[0] * 255.0f + 0.5f);
                entry[1] = static_cast<unsigned char>(
                    rgb[1] * 255.0f + 0.5f);
                entry[0] = static_cast<unsigned char>(
                    rgb[2] * 255.0f + 0.5f);
                PaletteStudioTouch(st.slot);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                PALLOG("[palette] %s palette %d colour %d -> %02X%02X%02X "
                       "(picker)",
                       info.label, st.variant + 1, color, entry[2],
                       entry[1], entry[0]);
            }
            ImGui::Spacing();
            {
                float h, s, v;
                ImGui::ColorConvertRGBtoHSV(
                    entry[2] / 255.0f, entry[1] / 255.0f,
                    entry[0] / 255.0f, h, s, v);
                int brightPct = static_cast<int>(v * 100.0f + 0.5f);
                int satPct = static_cast<int>(s * 100.0f + 0.5f);
                bool changed = false;
                ImGui::SetNextItemWidth(pickerWidth * 0.62f);
                changed |= ImGui::SliderInt("Brightness", &brightPct,
                                            0, 100, "%d%%");
                ImGui::SetNextItemWidth(pickerWidth * 0.62f);
                changed |= ImGui::SliderInt("Saturation", &satPct,
                                            0, 100, "%d%%");
                if (changed)
                {
                    float r, g, b;
                    ImGui::ColorConvertHSVtoRGB(
                        h, static_cast<float>(satPct) / 100.0f,
                        static_cast<float>(brightPct) / 100.0f, r, g, b);
                    entry[2] = static_cast<unsigned char>(
                        r * 255.0f + 0.5f);
                    entry[1] = static_cast<unsigned char>(
                        g * 255.0f + 0.5f);
                    entry[0] = static_cast<unsigned char>(
                        b * 255.0f + 0.5f);
                    PaletteStudioTouch(st.slot);
                }
            }
            {
                ImGui::Text("now");
                ImGui::SameLine();
                const ImVec4 current(entry[2] / 255.0f, entry[1] / 255.0f,
                                     entry[0] / 255.0f, 1.0f);
                ImGui::ColorButton("##now", current,
                                   ImGuiColorEditFlags_NoAlpha
                                       | ImGuiColorEditFlags_NoTooltip
                                       | ImGuiColorEditFlags_NoPicker);
                if (PaletteStudioDefaultAvailable())
                {
                    const unsigned char *authored =
                        g_paletteAuto[st.slot].defaultTable
                        + st.variant * kPaletteColors * 4 + color * 4;
                    ImGui::SameLine();
                    ImGui::Text("  default");
                    ImGui::SameLine();
                    const ImVec4 def(authored[2] / 255.0f,
                                     authored[1] / 255.0f,
                                     authored[0] / 255.0f, 1.0f);
                    if (ImGui::ColorButton("##def", def,
                                           ImGuiColorEditFlags_NoAlpha
                                               | ImGuiColorEditFlags_NoPicker))
                    {
                        PaletteStudioLoadDefaultColor();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Click to restore this colour");
                }
            }
            if (ImGui::SmallButton("< prev"))
            {
                const int previous = (color - 1 + 14) % 15;
                st.cursor = kStudioRowGrid0 + previous / 5;
                st.gridCol = previous % 5;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("next >"))
            {
                const int next = (color - 1 + 1) % 15;
                st.cursor = kStudioRowGrid0 + next / 5;
                st.gridCol = next % 5;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("undo"))
                (void)PaletteStudioUndo(st.slot);
            ImGui::SameLine();
            if (ImGui::SmallButton("redo"))
                (void)PaletteStudioRedo(st.slot);
            ImGui::SameLine();
            if (ImGui::SmallButton("close")) keepOpen = false;
            if (ImGui::GetIO().KeyCtrl
                && ImGui::IsKeyPressed(ImGuiKey_Z, false))
                (void)PaletteStudioUndo(st.slot);
            if (ImGui::GetIO().KeyCtrl
                && ImGui::IsKeyPressed(ImGuiKey_Y, false))
                (void)PaletteStudioRedo(st.slot);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) keepOpen = false;
        }
        ImGui::End();
        NativeMenuPaletteStylePop();
        if (!keepOpen) st.pickerOpen = false;
    }

    if (st.valueEditRow >= 0 && editRowY > 0.0f)
    {
        NativeMenuPaletteStylePush();
        ImGui::SetNextWindowPos(
            ImVec2(valueX - m.pad, editRowY - m.pad * 0.5f),
            ImGuiCond_Always);
        if (ImGui::Begin("###qoh99_palette_value", nullptr,
                         ImGuiWindowFlags_NoTitleBar
                             | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoMove
                             | ImGuiWindowFlags_NoSavedSettings))
        {
            if (st.valueEditFocus)
            {
                ImGui::SetKeyboardFocusHere();
                st.valueEditFocus = false;
            }
            ImGui::SetNextItemWidth(m.fontSize * 4.5f);
            const bool applied = ImGui::InputText(
                "##value", st.valueEditBuffer,
                sizeof(st.valueEditBuffer),
                ImGuiInputTextFlags_CharsDecimal
                    | ImGuiInputTextFlags_EnterReturnsTrue
                    | ImGuiInputTextFlags_AutoSelectAll);
            const bool abandoned =
                ImGui::IsItemDeactivated() && !applied;
            ImGui::SameLine();
            ImGui::TextUnformatted(
                st.valueEditRow == kStudioRowBright ? "%  (0-100)"
                                                    : "(0-255)");
            if (applied)
            {
                PaletteStudioSetChannelValue(
                    st.valueEditRow - kStudioRowR,
                    std::atoi(st.valueEditBuffer));
                st.valueEditRow = -1;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || abandoned)
            {
                st.valueEditRow = -1;
            }
        }
        ImGui::End();
        NativeMenuPaletteStylePop();
    }
}

bool DrawNativeTrainingMenu(const EfzDx9PresentParams &params,
                            const Qoh99TrainingStatusV1 &status)
{
    ImFont *font = g_trainingPixelFont;
    if (qi18n::language() != qi18n::Language::English) font = nullptr;
    if (font == nullptr) font = ImGui::GetFont();
    if (font == nullptr) return false;
    if (params.dstW <= 0 || params.dstH <= 0) return false;

    NativeMenuEnsureLoaded();

    int pages[kTrainingPageCount];
    int pageCount = 0;
    for (const TrainingMenuPageInfo &info : kTrainingMenuPages)
    {
        if (info.page != kTrainingPageDiagnostics
            && !TrainingMenuPageVisible(info, true, g_nativeMenu.advanced))
        {
            continue;
        }
        pages[pageCount++] = info.page;
    }
    if (pageCount == 0) return false;

    int pageIndex = 0;
    for (int i = 0; i < pageCount; ++i)
    {
        if (pages[i] == g_nativeMenu.page) { pageIndex = i; break; }
    }
    g_nativeMenu.page = pages[pageIndex];

    NativeMenuInput in = NativeMenuReadInput();
    InterlockedExchange(&g_menuNavigationPending, 0);
    const auto took = [](int nav) {
        if (nav < 0 || nav >= QOH99_MENU_NAV_COUNT) return false;
        for (;;)
        {
            const LONG current = g_menuNavigationCount[nav];
            if (current <= 0) return false;
            if (InterlockedCompareExchange(&g_menuNavigationCount[nav],
                                           current - 1, current) == current)
            {
                return true;
            }
        }
    };
    constexpr int kMenuDuplicateFrames = 30;
    static int debtAgainstSubmitted[QOH99_MENU_NAV_COUNT] = {};
    static int debtAgainstOwnEdge[QOH99_MENU_NAV_COUNT] = {};
    const auto navigate = [&](int nav, bool ownEdge) {
        if (nav < 0 || nav >= QOH99_MENU_NAV_COUNT) return ownEdge;
        const bool submitted = took(nav);
        if (debtAgainstSubmitted[nav] > 0) --debtAgainstSubmitted[nav];
        if (debtAgainstOwnEdge[nav] > 0) --debtAgainstOwnEdge[nav];
        if (ownEdge)
        {
            if (debtAgainstOwnEdge[nav] > 0)
            {
                debtAgainstOwnEdge[nav] = 0;
            }
            else
            {
                debtAgainstSubmitted[nav] =
                    submitted ? 0 : kMenuDuplicateFrames;
                return true;
            }
        }
        if (submitted)
        {
            if (debtAgainstSubmitted[nav] > 0)
            {
                debtAgainstSubmitted[nav] = 0;
                return false;
            }
            debtAgainstOwnEdge[nav] = kMenuDuplicateFrames;
            return true;
        }
        return false;
    };
    in.up = navigate(QOH99_MENU_NAV_UP, in.up);
    in.down = navigate(QOH99_MENU_NAV_DOWN, in.down);
    in.left = navigate(QOH99_MENU_NAV_LEFT, in.left);
    in.right = navigate(QOH99_MENU_NAV_RIGHT, in.right);
    in.commit = navigate(QOH99_MENU_NAV_CONFIRM, in.commit);
    in.back = navigate(QOH99_MENU_NAV_BACK, in.back);
    if (g_nativeMenu.page == kTrainingPagePalette
        && NativeMenuPaletteModalActive())
    {
        in = NativeMenuInput{};
    }
    const bool stepPagePrev =
        in.pagePrev || (g_nativeMenu.tabFocus && in.left);
    const bool stepPageNext =
        in.pageNext || (g_nativeMenu.tabFocus && in.right);
    if (stepPagePrev || stepPageNext)
    {
        pageIndex += stepPageNext ? 1 : -1;
        if (pageIndex < 0) pageIndex = pageCount - 1;
        if (pageIndex >= pageCount) pageIndex = 0;
        g_nativeMenu.page = pages[pageIndex];
        g_nativeMenu.row = 0;
        g_nativeMenu.scroll = 0;
        g_nativeMenu.submenu = -1;
        g_nativeMenu.submenuChild = false;
        g_paletteStudio.hidden = false;
        g_nativeMenuPaletteVisited = false;
        OverlayIniSet("MenuPage", g_nativeMenu.page);
    }
    if (in.back)
    {
        if (g_nativeMenu.submenuChild)
        {
            g_nativeMenu.submenuChild = false;
            g_nativeMenu.row = 0;
            g_nativeMenu.scroll = 0;
        }
        else if (g_nativeMenu.submenu >= 0)
        {
            g_nativeMenu.submenu = -1;
            g_nativeMenu.row = 0;
            g_nativeMenu.scroll = 0;
        }
        else
        {
            g_nativeMenu.tabFocus = !g_nativeMenu.tabFocus;
            if (!g_nativeMenu.tabFocus)
            {
                g_nativeMenu.row = 0;
                g_nativeMenu.scroll = 0;
            }
        }
    }

    if (g_nativeMenu.page == kTrainingPagePalette)
    {
        if (!g_nativeMenuPaletteVisited)
        {
            g_nativeMenuPaletteVisited = true;
            NativeMenuPaletteEnter();
        }
    }
    else
    {
        g_nativeMenuPaletteVisited = false;
        g_paletteStudio.hidden = false;
    }

    NativeMenuPageBuild build;
    switch (g_nativeMenu.page)
    {
    case kTrainingPageQuick:      NativeMenuBuildQuick(status, build); break;
    case kTrainingPageDummy:      NativeMenuBuildDummy(status, build); break;
    case kTrainingPageReactions:  NativeMenuBuildTriggers(status, build); break;
    case kTrainingPageRecording:  NativeMenuBuildRecording(status, build); break;
    case kTrainingPageView:       NativeMenuBuildView(status, build); break;
    case kTrainingPagePalette:
        build.heading("PALETTE");
        break;
    case kTrainingPageKeys:       NativeMenuBuildKeys(status, build); break;
    case kTrainingPageHelp:       NativeMenuBuildHelp(status, build); break;
    case kTrainingPageDiagnostics:
        NativeMenuBuildDiagnostics(status, build);
        break;
    default:
        build.heading(kTrainingMenuPages[g_nativeMenu.page].label);
        build.info("Status", "no controls yet");
        break;
    }

    if (g_nativeMenu.submenu < 0 && !g_nativeMenu.submenuChild)
    {
        NativeMenuApplySubTabs(build, g_nativeMenu.page);
    }

    const int firstRow = NativeMenuFirstSelectable(build);
    const int lastRow = NativeMenuLastSelectable(build);
    bool hasSelectableRow = false;
    for (int i = 0; i < build.count; ++i)
    {
        if (NativeRowSelectable(build.rows[i])) { hasSelectableRow = true; break; }
    }
    if (g_nativeMenu.row < 0 || g_nativeMenu.row >= build.count
        || !NativeRowSelectable(build.rows[g_nativeMenu.row]))
    {
        g_nativeMenu.row = firstRow;
    }
    if (g_nativeMenu.focusPreferred)
    {
        g_nativeMenu.focusPreferred = false;
        if (build.preferredRow >= 0 && build.preferredRow < build.count
            && NativeRowSelectable(build.rows[build.preferredRow]))
        {
            g_nativeMenu.row = build.preferredRow;
            g_nativeMenu.scroll = 0;
        }
    }
    if (!hasSelectableRow
        && g_nativeMenu.page != kTrainingPagePalette)
    {
        g_nativeMenu.tabFocus = true;
    }

    if (g_nativeMenu.tabFocus)
    {
        if (g_nativeMenu.page == kTrainingPagePalette)
        {
            if (in.down || in.commit)
            {
                g_nativeMenu.tabFocus = false;
                g_paletteStudio.cursor = kStudioRowSlot;
            }
            else if (in.up)
            {
                g_nativeMenu.tabFocus = false;
                g_paletteStudio.cursor = kStudioRowRestore;
            }
        }
        else if ((in.down || in.commit) && hasSelectableRow)
        {
            g_nativeMenu.tabFocus = false;
            g_nativeMenu.row = firstRow;
            g_nativeMenu.scroll = 0;
        }
        else if (in.up && hasSelectableRow)
        {
            g_nativeMenu.tabFocus = false;
            g_nativeMenu.row = lastRow;
        }
        else if (!hasSelectableRow)
        {
            if (in.down) ++g_nativeMenu.scroll;
            if (in.up && g_nativeMenu.scroll > 0) --g_nativeMenu.scroll;
        }
    }
    else if (g_nativeMenu.page == kTrainingPagePalette)
    {
        NativeMenuPaletteInput(in);
    }
    else
    {
        const NativeMenuFlow navFlow = NativeMenuComputeFlow(
            build, font, NativeMenuComputeMetrics(params, font, pages,
                                                  pageCount),
            (params.dstW > 0 ? static_cast<float>(params.dstW) : 640.0f)
                * 0.86f);
        if (in.up)
        {
            if (g_nativeMenu.row == firstRow) g_nativeMenu.tabFocus = true;
            else
            {
                g_nativeMenu.row = NativeMenuAdvanceFlow(
                    build, navFlow, g_nativeMenu.row, -1, true);
            }
        }
        else if (in.down)
        {
            const int next = NativeMenuAdvanceFlow(
                build, navFlow, g_nativeMenu.row, 1, true);
            if (g_nativeMenu.row == lastRow || next == g_nativeMenu.row)
                g_nativeMenu.tabFocus = true;
            else
                g_nativeMenu.row = next;
        }
        else if (navFlow.columns > 1 && (in.left || in.right))
        {
            g_nativeMenu.row = NativeMenuAdvanceRow(
                build, g_nativeMenu.row, in.right ? 1 : -1);
        }
    }
    g_nativeMenu.lastRowCount = build.count;

    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    const bool mouseMoved =
        mousePos.x != g_nativeMenu.lastMouseX
        || mousePos.y != g_nativeMenu.lastMouseY;
    g_nativeMenu.lastMouseX = mousePos.x;
    g_nativeMenu.lastMouseY = mousePos.y;
    const bool keyboardUsed = in.up || in.down || in.left || in.right
        || in.commit || in.back || in.pagePrev || in.pageNext;
    const bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (keyboardUsed) g_nativeMenu.mouseEngaged = false;
    if (mouseMoved || mouseClicked) g_nativeMenu.mouseEngaged = true;
    const bool mouseActive = g_nativeMenu.mouseEngaged;

    ImDrawList *dl = (g_nativeMenu.page == kTrainingPagePalette
                      && NativeMenuPaletteModalActive())
        ? ImGui::GetBackgroundDrawList()
        : ImGui::GetForegroundDrawList();
    if (dl == nullptr) return false;

    if (g_nativeMenu.page == kTrainingPagePalette && g_paletteStudio.hidden)
    {
        ImFont *hintFont = font;
        NativeMenuText(dl, hintFont,
                       static_cast<float>(params.dstH) * 0.03f,
                       ImVec2(static_cast<float>(params.dstX)
                                  + static_cast<float>(params.dstW) * 0.015f,
                              static_cast<float>(params.dstY)
                                  + static_cast<float>(params.dstH) * 0.95f),
                       NativeMenuStyle::kTextDim,
                       "D - back to the palette editor");
        return true;
    }

    const NativeMenuMetrics m =
        NativeMenuComputeMetrics(params, font, pages, pageCount);

    dl->AddRectFilled(
        ImVec2(static_cast<float>(params.dstX),
               static_cast<float>(params.dstY)),
        ImVec2(static_cast<float>(params.dstX + params.dstW),
               static_cast<float>(params.dstY + params.dstH)),
        NativeMenuStyle::kScrim);
    dl->AddRectFilled(m.panelMin, m.panelMax, NativeMenuStyle::kPanel);
    float borderThickness = std::floor(m.fontSize / 13.0f);
    if (borderThickness < 1.0f) borderThickness = 1.0f;
    dl->AddRect(m.panelMin, m.panelMax, NativeMenuStyle::kBorder, 0.0f, 0,
                borderThickness);

    dl->PushClipRect(ImVec2(m.panelMin.x + 1.0f, m.panelMin.y + 1.0f),
                     ImVec2(m.panelMax.x - 1.0f, m.panelMax.y - 1.0f), true);

    const float contentX = m.panelMin.x + m.pad * 2.0f;
    const float contentW = (m.panelMax.x - m.pad * 2.0f) - contentX;
    float y = m.panelMin.y + m.pad;

    char title[64];
    if (g_nativeMenu.submenuChild)
    {
        std::snprintf(title, sizeof(title), "TRAINING  >  %s  >  Move",
                      kTrainingMenuPages[g_nativeMenu.page].label);
    }
    else if (g_nativeMenu.submenu >= 0)
    {
        std::snprintf(title, sizeof(title), "TRAINING  >  %s",
                      kTrainingMenuPages[g_nativeMenu.page].label);
    }
    else
    {
        std::snprintf(title, sizeof(title), "TRAINING");
    }
    NativeMenuText(dl, font, m.fontSize, ImVec2(contentX, y),
                   NativeMenuStyle::kText, title);
    y += m.lineHeight;

    int hoveredPage = -1;
    NativeMenuDrawTabs(dl, font, m, ImVec2(contentX, y), contentW, pages,
                       pageCount, g_nativeMenu.page, g_nativeMenu.tabFocus,
                       hoveredPage);
    y += m.lineHeight * static_cast<float>(m.tabLines);
    dl->AddLine(ImVec2(contentX, std::floor(y)),
                ImVec2(contentX + contentW, std::floor(y)),
                NativeMenuStyle::kBorderInner, 1.0f);
    y += m.pad;

    const bool paletteModal = g_nativeMenu.page == kTrainingPagePalette
        && NativeMenuPaletteModalActive();
    const bool clicked = mouseClicked && mouseActive && !paletteModal;
    if (hoveredPage >= 0 && clicked)
    {
        g_nativeMenu.page = hoveredPage;
        g_nativeMenu.row = 0;
        g_nativeMenu.scroll = 0;
        g_nativeMenu.tabFocus = true;
        OverlayIniSet("MenuPage", g_nativeMenu.page);
    }

    if (g_nativeMenu.page == kTrainingPagePalette)
    {
        NativeMenuPaletteBody(dl, font, m, contentX, contentW, y,
                              mousePos, mouseActive, clicked);
        dl->PopClipRect();
        return true;
    }

    const float rowsTop = y;
    const float footerTop = m.panelMax.y - m.pad - m.lineHeight * 2.9f;
    int visibleRows =
        static_cast<int>(std::floor((footerTop - rowsTop) / m.lineHeight));
    if (visibleRows < 1) visibleRows = 1;

    const ImVec2 &mouse = mousePos;
    int mouseRow = -1;
    const NativeMenuFlow flow =
        NativeMenuComputeFlow(build, font, m, contentW);
    const int cursorLine =
        (g_nativeMenu.row >= 0 && g_nativeMenu.row < build.count)
            ? flow.line[g_nativeMenu.row] : 0;
    if (!g_nativeMenu.tabFocus)
    {
        if (cursorLine < g_nativeMenu.scroll)
            g_nativeMenu.scroll = cursorLine;
        else if (cursorLine >= g_nativeMenu.scroll + visibleRows)
            g_nativeMenu.scroll = cursorLine - visibleRows + 1;
    }
    const int maxLineScroll =
        flow.lines > visibleRows ? flow.lines - visibleRows : 0;
    if (g_nativeMenu.scroll > maxLineScroll) g_nativeMenu.scroll = maxLineScroll;
    if (g_nativeMenu.scroll < 0) g_nativeMenu.scroll = 0;

    const float cellW = flow.columns > 0
        ? contentW / static_cast<float>(flow.columns) : contentW;
    for (int i = 0; i < build.count; ++i)
    {
        const int line = flow.line[i] - g_nativeMenu.scroll;
        if (line < 0 || line >= visibleRows) continue;
        const NativeRow &row = build.rows[i];
        const bool spans = row.kind == kNativeRowHeading
            || row.kind == kNativeRowSpacer || flow.columns == 1;
        const float rowX = spans
            ? contentX
            : contentX + cellW * static_cast<float>(flow.column[i]);
        const float rowW = spans ? contentW : cellW - m.pad;
        const float rowY = rowsTop + m.lineHeight * static_cast<float>(line);
        if (mouseActive && NativeRowSelectable(row) && mouse.y >= rowY
            && mouse.y < rowY + m.lineHeight && mouse.x >= rowX
            && mouse.x <= rowX + rowW)
        {
            mouseRow = i;
        }
        NativeMenuDrawRow(dl, font, m, row, ImVec2(rowX, rowY), rowW,
                          !g_nativeMenu.tabFocus && i == g_nativeMenu.row);
    }
    if (mouseRow >= 0)
    {
        g_nativeMenu.row = mouseRow;
        g_nativeMenu.tabFocus = false;
    }

    if (g_nativeMenu.scroll > 0)
    {
        NativeMenuText(dl, font, m.fontSize,
                       ImVec2(contentX + contentW, rowsTop),
                       NativeMenuStyle::kTextDim, "^");
    }
    if (g_nativeMenu.scroll + visibleRows < flow.lines)
    {
        NativeMenuText(
            dl, font, m.fontSize,
            ImVec2(contentX + contentW,
                   rowsTop + m.lineHeight * static_cast<float>(visibleRows - 1)),
            NativeMenuStyle::kTextDim, "v");
    }

    float footerY = footerTop;
    static NativeTextScroll footerScroll;
    if (g_nativeMenu.tabFocus)
    {
        NativeMenuTextWrapped(dl, font, m.fontSize,
                              ImVec2(contentX, footerY), contentW,
                              NativeMenuStyle::kTextDim,
                              kTrainingMenuPages[g_nativeMenu.page].hint, 2,
                              &footerScroll);
    }
    else if (g_nativeMenu.row >= 0 && g_nativeMenu.row < build.count)
    {
        const NativeRow &row = build.rows[g_nativeMenu.row];
        const char *hint = row.hint;
        if (row.choiceHints != nullptr && row.value >= 0
            && row.value < row.choiceCount
            && row.choiceHints[row.value] != nullptr)
        {
            hint = row.choiceHints[row.value];
        }
        if (hint != nullptr) hint = qi18n::tr(hint);
        if (hint != nullptr && hint[0] != '\0')
        {
            NativeMenuTextWrapped(dl, font, m.fontSize,
                                  ImVec2(contentX, footerY), contentW,
                                  NativeMenuStyle::kTextDim, hint, 2,
                                  &footerScroll);
        }
    }
    footerY += m.lineHeight * 1.9f;
    char legend[128];
    if (g_nativeMenu.tabFocus)
    {
        std::snprintf(legend, sizeof(legend),
                      hasSelectableRow
                          ? "Left/Right page   Down enter page"
                          : "Left/Right page   Up/Down scroll");
    }
    else if (g_nativeMenu.submenu >= 0 || g_nativeMenu.submenuChild)
    {
        std::snprintf(legend, sizeof(legend),
                      "Up/Down move   Left/Right change   %s back",
                      g_menuBackLabel);
    }
    else
    {
        std::snprintf(legend, sizeof(legend),
                      "Up/Down move   Left/Right change   %s ok",
                      g_menuConfirmLabel);
    }
    {
        const std::string fitted =
            ClipTextToWidth(font, m.fontSize, legend, contentW);
        NativeMenuText(dl, font, m.fontSize, ImVec2(contentX, footerY),
                       NativeMenuStyle::kTextDim,
                       fitted.empty() ? legend : fitted.c_str());
    }

    if (build.dropped > 0)
    {
        char note[64];
        std::snprintf(note, sizeof(note), "%d control(s) did not fit",
                      build.dropped);
        NativeMenuText(dl, font, m.fontSize,
                       ImVec2(contentX, footerTop - m.lineHeight),
                       NativeMenuStyle::kWarn, note);
    }

    dl->PopClipRect();

    if (!g_nativeMenu.tabFocus && g_nativeMenu.row >= 0
        && g_nativeMenu.row < build.count)
    {
        NativeRow &row = build.rows[g_nativeMenu.row];
        if (row.kind == kNativeRowSubTabs)
        {
            int next = row.value;
            if (in.left) next = row.value - 1;
            else if (in.right || in.commit) next = row.value + 1;
            else if (mouseRow == g_nativeMenu.row && clicked)
            {
                next = row.value + 1;
                for (int i = 0; i < g_nativeMenuSubTabHitCount; ++i)
                {
                    if (mousePos.x >= g_nativeMenuSubTabHitX[i][0]
                        && mousePos.x <= g_nativeMenuSubTabHitX[i][1])
                    {
                        next = i;
                        break;
                    }
                }
            }
            if (next != row.value && row.choiceCount > 0)
            {
                const int count = row.choiceCount;
                g_nativeMenu.subTab[g_nativeMenu.page] =
                    ((next % count) + count) % count;
                g_nativeMenu.scroll = 0;
                char key[24];
                std::snprintf(key, sizeof(key), "MenuSubTab%d",
                              g_nativeMenu.page);
                OverlayIniSet(key, g_nativeMenu.subTab[g_nativeMenu.page]);
            }
        }
        else if (in.left) NativeMenuCommitRow(status, row, -1, false);
        else if (in.right) NativeMenuCommitRow(status, row, 1, false);
        else if (in.commit || (mouseRow == g_nativeMenu.row && clicked))
        {
            NativeMenuCommitRow(status, row, 0, true);
        }
    }
    return true;
}

void BuildDebugMenu(const EfzDx9PresentParams &params)
{
    HudLayoutEnsureLoaded();

    Qoh99TrainingStatusV1 training = {};
    const bool trainingPresent =
        ReadTrainingStatus(training)
        && training.explicitTrainingPresent != 0;
    if (trainingPresent)
    {
        RefreshTrainingUiForStatus(training);
    }

    if (trainingPresent && TrainingMenuUseNativeStyle())
    {
        if (DrawNativeTrainingMenu(params, training))
        {
            SyncTrainingMenuCapture();
            return;
        }
    }

    ImGui::SetNextWindowSize(
        trainingPresent ? ImVec2(640.0f, 480.0f) : ImVec2(600.0f, 470.0f),
        ImGuiCond_FirstUseEver);
    const char *title = trainingPresent
        ? "QOH99 Training & Debug###qoh99_training_debug_menu"
        : "QOH99 Netplay Debug###qoh99_debug_menu";
    if (ImGui::Begin(
            title, &g_presenter.menuVisible, ImGuiWindowFlags_NoCollapse))
    {
        if (trainingPresent)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Use the overlay-menu control to close.");
            ImGui::SameLine();
            DrawPersistentTrainingFrameStep(training);
        }
        else
        {
            ImGui::TextDisabled(
                "Use the configured overlay-menu control to close. "
                "This menu is offline only.");
        }
        if (trainingPresent)
        {
            ImGui::TextColored(
                TrainingLifecycleColor(training.lifecycleState),
                "%s", TrainingLifecycleName(training.lifecycleState));
            if (!training.available)
            {
                ImGui::TextWrapped("Training controls are off - %s.",
                    training.disableText[0] != '\0'
                        ? training.disableText
                        : TrainingDisableReasonName(training.disableReason));
            }
            if (training.statusText[0] != '\0')
            {
                ImGui::TextWrapped("%s", training.statusText);
            }
            DrawLatestTrainingCommand(training);
        }
        if (g_presenter.trainingUiRejectedApiVersion != 0)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.40f, 0.35f, 1.0f),
                "Training bridge version mismatch: ddraw.dll speaks v%lu, the "
                "injected DLL speaks v%lu - reinstall a matching build.",
                static_cast<unsigned long>(QOH99_TRAINING_UI_API_VERSION),
                static_cast<unsigned long>(
                    g_presenter.trainingUiRejectedApiVersion));
        }
        if (g_presenter.inputDisplayRejectedApiVersion != 0)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.40f, 0.35f, 1.0f),
                "Input viewer bridge version mismatch: ddraw.dll speaks v%lu, "
                "the injected DLL speaks v%lu - reinstall a matching build.",
                static_cast<unsigned long>(QOH99_INPUT_DISPLAY_API_VERSION),
                static_cast<unsigned long>(
                    g_presenter.inputDisplayRejectedApiVersion));
        }
        bool advanced = false;
        const TrainingMenuPage page =
            DrawTrainingMenuSidebar(trainingPresent, advanced);
        ImGui::SameLine();
        if (ImGui::BeginChild("qoh99_menu_page", ImVec2(0.0f, 0.0f)))
        {
            switch (page)
            {
            case kTrainingPageQuick:
                DrawTrainingTab(training);
                break;
            case kTrainingPageDummy:
                DrawDummyTab(training);
                break;
            case kTrainingPageReactions:
                DrawDummyReactionsSection(training);
                break;
            case kTrainingPageRecording:
                DrawRecordingTab(training);
                ImGui::Spacing();
                ImGui::SeparatorText(qi18n::tr("Save states"));
                DrawStateTab(training);
                break;
            case kTrainingPageView:
                DrawViewTab(params, training, trainingPresent);
                break;
            case kTrainingPagePalette:
                DrawPaletteSlotEditor(0, training);
                DrawPaletteSlotEditor(1, training);
                DrawPaletteDelivery();
                break;
            case kTrainingPageKeys:
                if (trainingPresent) DrawTrainingHotkeysTab(training);
                else DrawNetplayKeysTab();
                break;
            case kTrainingPageHelp:
                if (trainingPresent) DrawTrainingHelpTab();
                else DrawNetplayHelpTab();
                break;
            case kTrainingPageDiagnostics:
                DrawDiagnosticsTab(params, training, trainingPresent);
                break;
            default:
                break;
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    SyncTrainingMenuCapture();

    if (g_presenter.showDemoWindow)
    {
        ImGui::ShowDemoWindow(&g_presenter.showDemoWindow);
    }
}

bool IsInputMessage(UINT msg)
{
    switch (msg)
    {
    case WM_CHAR:
    case WM_IME_CHAR:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSCHAR:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_SETCURSOR:
        return true;
    default:
        return false;
    }
}

bool SubmitImeCharacter(HWND hwnd, WPARAM value)
{
    ImGuiIO &io = ImGui::GetIO();
    if (IsWindowUnicode(hwnd))
    {
        if (value > 0 && value < 0x10000u)
        {
            io.AddInputCharacterUTF16(static_cast<unsigned short>(value));
            return true;
        }
        return false;
    }

    char encoded[2] = {};
    int encodedBytes = 1;
    if ((value & 0xFF00u) != 0u)
    {
        encoded[0] = static_cast<char>((value >> 8u) & 0xFFu);
        encoded[1] = static_cast<char>(value & 0xFFu);
        encodedBytes = 2;
    }
    else
    {
        encoded[0] = static_cast<char>(value & 0xFFu);
    }

    UINT codePage = CP_ACP;
    const HKL layout = GetKeyboardLayout(0);
    const LCID locale = MAKELCID(HIWORD(layout), SORT_DEFAULT);
    (void)GetLocaleInfoA(
        locale, LOCALE_RETURN_NUMBER | LOCALE_IDEFAULTANSICODEPAGE,
        reinterpret_cast<LPSTR>(&codePage), sizeof(codePage));
    wchar_t converted[2] = {};
    const int convertedUnits = MultiByteToWideChar(
        codePage, MB_PRECOMPOSED, encoded, encodedBytes,
        converted, static_cast<int>(sizeof(converted) / sizeof(converted[0])));
    for (int i = 0; i < convertedUnits; ++i)
    {
        io.AddInputCharacterUTF16(static_cast<unsigned short>(converted[i]));
    }
    return convertedUnits > 0;
}
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern "C" BOOL EFZDX9_Presenter_HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *outResult)
{
    // ImGui state belongs to the worker while a device request is in flight.
    // Window messages arrive on the game thread, so they must not touch it.
    if (!deviceReady())
    {
        return FALSE;
    }
    if (TrainingMenuShouldDraw() && g_presenter.trainingMenuHandleMessage != nullptr)
    {
        if (g_presenter.trainingMenuHandleMessage(hwnd, msg, wParam, lParam, outResult, g_presenter.trainingMenuUserData))
        {
            return TRUE;
        }
    }

    if (!g_presenter.menuVisible)
    {
        return FALSE;
    }

    if (g_presenter.imguiInitialized && msg == WM_IME_CHAR)
    {
        (void)SubmitImeCharacter(hwnd, wParam);
        if (outResult != nullptr)
        {
            *outResult = 0;
        }
        return TRUE;
    }

    if (g_presenter.imguiInitialized && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        if (outResult != nullptr)
        {
            *outResult = 1;
        }
        return TRUE;
    }

    if (IsInputMessage(msg))
    {
        if (outResult != nullptr)
        {
            *outResult = 0;
        }
        return TRUE;
    }

    return FALSE;
}

extern "C" BOOL EFZDX9_Presenter_IsMenuVisible(void)
{
    return g_presenter.menuVisible ? TRUE : FALSE;
}

extern "C" BOOL EFZDX9_Presenter_ShouldSuppressGameInput(void)
{
    return (g_presenter.menuVisible || TrainingMenuShouldDraw())
        ? TRUE : FALSE;
}

namespace
{
void MarkD3D9ExDecline(HRESULT hr, const char *operation)
{
    const bool exActive = g_presenter.deviceEx != nullptr
        && g_presenter.effectiveBackend == QOH_RENDERER_D3D9EX_FLIPEX;
    if (!exActive)
    {
        return;
    }

    ++g_diag.losses;
    g_diag.lastError = hr;
    g_presenter.deviceLost = true;
    const bool transient = hr == D3DERR_DEVICELOST
        || hr == D3DERR_DEVICENOTRESET;
    if (!transient)
    {
        qoh99_wlog_issue();
        g_presenter.exDisabledAfterHardFailure = true;
        std::snprintf(
            g_presenter.fallbackReason,
            sizeof(g_presenter.fallbackReason),
            "D3D9Ex %s hard-failed (0x%08lX); switching the complete backend",
            operation, static_cast<unsigned long>(hr));
    }
    DXLOG("[d3d9ex] %s FAILED hr=0x%08lX; %s",
          operation, static_cast<unsigned long>(hr),
          transient
              ? "holding FlipEx until the worker attempts device recovery"
              : "switching the complete backend to Classic D3D9 (GDI mixing suppressed)");
}

BOOL PresentFrameLocked(const EfzDx9PresentParams &params)
{
    g_presenter.appActive = params.appActive != FALSE;
    if (!EnsureFrameTexture(params.width, params.height))
    {
        const HRESULT failureHr = g_diag.lastError;
        recordD3dPresentation(params, 0, 0, 0, 0, 0, failureHr,
                              "frame texture unavailable");
        DXLOG("[d3d9] accelerated present declined: frame texture %dx%d unavailable",
              params.width, params.height);
        MarkD3D9ExDecline(failureHr, "frame texture creation");
        return FALSE;
    }
    const LONGLONG uploadStartQpc = presentationQpcNow();
    if (!UploadFrame(params))
    {
        const LONGLONG uploadEndQpc = presentationQpcNow();
        const HRESULT failureHr = g_diag.lastError;
        recordD3dPresentation(params, uploadStartQpc, uploadEndQpc,
                              0, 0, 0, failureHr, "frame upload failed");
        DXLOG("[d3d9] accelerated present declined: frame upload failed (%dx%d bpp=%d)",
              params.width, params.height, params.bytesPerPixel);
        MarkD3D9ExDecline(failureHr, "frame upload");
        return FALSE;
    }
    const LONGLONG uploadEndQpc = presentationQpcNow();

    g_presenter.device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    const LONGLONG beginSceneQpc = presentationQpcNow();
    const HRESULT beginHr = g_presenter.device->BeginScene();
    if (FAILED(beginHr))
    {
        recordD3dPresentation(params, uploadStartQpc, uploadEndQpc,
                              beginSceneQpc, 0, 0, beginHr,
                              "BeginScene failed");
        g_diag.lastError = beginHr;
        DXLOG("[d3d9] accelerated present declined: BeginScene hr=0x%08lX",
              static_cast<unsigned long>(beginHr));
        MarkD3D9ExDecline(beginHr, "BeginScene");
        return FALSE;
    }

    RenderFrameQuad(params);

    if (g_presenter.menuVisible)
    {
        RenderTrainingMenu(&params);
    }

    if (PresenterNeedsImGuiPath())
    {
        if (g_inputResyncPending && g_presenter.imguiInitialized)
        {
            g_inputResyncPending = false;
            ImGui::GetIO().ClearEventsQueue();
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
        HudLayoutEnsureLoaded();
        ImGui::GetIO().FontGlobalScale = static_cast<float>(
            g_overlayUiSettings.menuFontScalePct) / 100.0f;
        {
            ImGuiIO &navIo = ImGui::GetIO();
            const bool nativeMenuOwnsKeys =
                g_presenter.menuVisible && TrainingMenuUseNativeStyle();
            if (nativeMenuOwnsKeys)
            {
                navIo.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
            }
            else
            {
                navIo.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            }
        }
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawNetplayHud(params);
        DrawGenericOverlay(params);
        DrawInputDisplay(params);
        DrawReplayTransport(params);
        DrawTrainingFrameAdvantageOverlay(params);
        DrawTrainingStatusMessages(params);
        DrawTrainingDebugOverlay(params);
        if (g_presenter.menuVisible)
        {
            BuildDebugMenu(params);
        }
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    if (!g_presenter.menuVisible)
    {
        RenderTrainingMenu(&params);
    }

    g_presenter.device->EndScene();
    const LONGLONG presentEntryQpc = presentationQpcNow();
    const HRESULT presentHr = g_presenter.deviceEx != nullptr
        && g_presenter.effectiveBackend == QOH_RENDERER_D3D9EX_FLIPEX
            ? g_presenter.deviceEx->PresentEx(
                nullptr, nullptr, nullptr, nullptr, 0u)
            : g_presenter.device->Present(
                nullptr, nullptr, nullptr, nullptr);
    const LONGLONG presentReturnQpc = presentationQpcNow();
    if (presentHr == D3DERR_DEVICELOST || presentHr == D3DERR_DEVICENOTRESET)
    {
        recordD3dPresentation(params, uploadStartQpc, uploadEndQpc,
                              beginSceneQpc, presentEntryQpc,
                              presentReturnQpc, presentHr, "device lost");
        ++g_diag.losses;
        g_diag.lastError = presentHr;
        g_presenter.deviceLost = true;
        DXLOG("[%s] Present%s hr=0x%08lX (device lost, losses=%lu); %s",
              g_presenter.deviceEx != nullptr ? "d3d9ex" : "d3d9",
              g_presenter.deviceEx != nullptr ? "Ex" : "",
              static_cast<unsigned long>(presentHr),
              static_cast<unsigned long>(g_diag.losses),
              g_presenter.deviceEx != nullptr
                  ? "holding the FlipEx backend until the worker recovers it"
                  : "falling back to GDI until the worker recovers it");
        return FALSE;
    }
    if (FAILED(presentHr))
    {
        recordD3dPresentation(params, uploadStartQpc, uploadEndQpc,
                              beginSceneQpc, presentEntryQpc,
                              presentReturnQpc, presentHr, "Present failed");
        g_diag.lastError = presentHr;
        qoh99_wlog_issue();
        if (g_presenter.deviceEx != nullptr)
        {
            ++g_diag.losses;
            g_presenter.deviceLost = true;
            g_presenter.exDisabledAfterHardFailure = true;
            std::snprintf(
                g_presenter.fallbackReason,
                sizeof(g_presenter.fallbackReason),
                "D3D9Ex PresentEx hard-failed (0x%08lX); switching the complete backend",
                static_cast<unsigned long>(presentHr));
        }
        DXLOG("[%s] %s FAILED hr=0x%08lX; %s",
              g_presenter.deviceEx != nullptr ? "d3d9ex" : "d3d9",
              g_presenter.deviceEx != nullptr ? "PresentEx" : "Present",
              static_cast<unsigned long>(presentHr),
              g_presenter.deviceEx != nullptr
                  ? "switching the complete backend (GDI mixing suppressed)"
                  : "falling back to GDI");
        return FALSE;
    }

    DXLOGV("[%s] %s hr=0x%08lX",
           g_presenter.deviceEx != nullptr ? "d3d9ex" : "d3d9",
           g_presenter.deviceEx != nullptr ? "PresentEx" : "Present",
           static_cast<unsigned long>(presentHr));
    recordD3dPresentation(params, uploadStartQpc, uploadEndQpc,
                          beginSceneQpc, presentEntryQpc, presentReturnQpc,
                          presentHr, nullptr);
    return TRUE;
}

void FlushPendingMenuCapture()
{
    if (!g_menuCaptureSyncPending || deviceState() == kDeviceWorking)
    {
        return;
    }
    g_menuCaptureSyncPending = false;
    SyncTrainingMenuCapture();
}

void PresenterAfterFrame()
{
    if (g_deferredRequestValid)
    {
        g_deferredRequestValid = false;
        RequestDeviceWork(g_deferredRequestWindow, g_deferredRequestWidth,
                          g_deferredRequestHeight, g_deferredRequestMode,
                          g_deferredRequestBackend);
    }
    FlushPendingMenuCapture();
}
}

extern "C" void EFZDX9_Presenter_ProcessDeviceWorkOnWindowThread(HWND window)
{
    if (window == nullptr || !IsWindow(window))
        return;
    DWORD windowThread = GetWindowThreadProcessId(window, nullptr);
    DWORD currentThread = GetCurrentThreadId();
    if (windowThread == 0 || currentThread != windowThread)
    {
        DXLOG("[d3d9] lifecycle thread mismatch current=%lu window=%lu hwnd=%p",
              static_cast<unsigned long>(currentThread),
              static_cast<unsigned long>(windowThread),
              static_cast<void *>(window));
        PostMessageA(window, WM_QOH99_D3D_DEVICE_WORK, 0, 0);
        return;
    }
    if (deviceState() == kDeviceInFrame)
    {
        PostMessageA(window, WM_QOH99_D3D_DEVICE_WORK, 0, 0);
        return;
    }
    if (InterlockedCompareExchange(&g_deviceState, kDeviceWorking,
                                   kDevicePending) != kDevicePending)
        return;

    const LONG requestGeneration =
        InterlockedCompareExchange(&g_deviceRequestGeneration, 0, 0);
    const HWND requestedWindow = g_deviceRequestWindow;
    const int requestedWidth = g_deviceRequestWidth;
    const int requestedHeight = g_deviceRequestHeight;
    const QohPresentationMode requestedMode = g_deviceRequestMode;
    const QohRendererBackend requestedBackend = g_deviceRequestBackend;
    g_deviceRetryDelayMs = kDeviceRetryBackoffMs;
    const bool accelerated = ProvisionDevice(
        requestedWindow, requestedWidth, requestedHeight,
        requestedMode, requestedBackend);
    if (accelerated)
    {
        g_inputResyncPending = true;
        InterlockedExchange(&g_deviceState, kDeviceReady);
    }
    else if (g_presenter.readiness == QOH_RENDERER_FALLBACK_READY)
    {
        ++g_diag.failures;
        g_deviceLastFailureTick = GetTickCount();
        InterlockedExchange(&g_deviceState, kDeviceFallbackReady);
    }
    else
    {
        ++g_diag.failures;
        g_deviceLastFailureTick = GetTickCount();
        g_presenter.readiness = QOH_RENDERER_FAILED;
        InterlockedExchange(&g_deviceState, kDeviceFailed);
    }

    if (InterlockedCompareExchange(&g_deviceRequestGeneration, 0, 0)
        != requestGeneration)
    {
        g_presenter.readiness = QOH_RENDERER_PENDING;
        InterlockedExchange(&g_deviceState, kDevicePending);
        PostMessageA(g_deviceRequestWindow, WM_QOH99_D3D_DEVICE_WORK, 0, 0);
    }
}

extern "C" BOOL EFZDX9_Presenter_Present(const EfzDx9PresentParams *params)
{
    if (params == nullptr || params->window == nullptr || params->pixels == nullptr
        || params->width <= 0 || params->height <= 0 || params->pitch <= 0)
    {
        return FALSE;
    }

    if (g_presentDepth != 0)
    {
        ++g_diag.reentrantPresents;
        DXLOGV("[d3d9] accelerated present declined: re-entrant present (count=%lu)",
               static_cast<unsigned long>(g_diag.reentrantPresents));
        return FALSE;
    }

    FlushPendingMenuCapture();

    SampleTrainingActionStateLog();

    // While the worker owns g_presenter the game thread must not read or write any of it.
    if (!deviceReady())
    {
        const LONG state = deviceState();
        if (!(g_presenter.readiness == QOH_RENDERER_FALLBACK_READY
              && g_presenter.requestedMode == params->activeMode))
            RequestDeviceWork(params->window, params->clientWidth,
                              params->clientHeight, params->activeMode,
                              g_presenter.requestedBackend);
        DXLOGV("[d3d9] accelerated present declined: no usable device (state=%ld overlayWanted=%d "
               "creates=%lu failures=%lu lastError=0x%08lX)",
               state, static_cast<int>(PresenterNeedsAnyOverlay()),
               static_cast<unsigned long>(g_diag.creates),
               static_cast<unsigned long>(g_diag.failures),
               static_cast<unsigned long>(g_diag.lastError));
        if (state == kDeviceFailed)
        {
            DXLOG("[d3d9] accelerated present declined: device provisioning FAILED "
                  "(failures=%lu lastError=0x%08lX); retrying after backoff",
                  static_cast<unsigned long>(g_diag.failures),
                  static_cast<unsigned long>(g_diag.lastError));
        }
        return FALSE;
    }

    if (g_presenter.presentParams.Windowed
        && params->clientWidth > 0 && params->clientHeight > 0
        && (static_cast<UINT>(params->clientWidth)
                != g_presenter.presentParams.BackBufferWidth
            || static_cast<UINT>(params->clientHeight)
                != g_presenter.presentParams.BackBufferHeight))
    {
        const DWORD now = GetTickCount();
        if (g_lastResizeRequestTick == 0
            || (now - g_lastResizeRequestTick) >= kResizeCoalesceMs)
        {
            g_lastResizeRequestTick = now;
            RequestDeviceWork(params->window, params->clientWidth,
                              params->clientHeight, params->activeMode,
                              g_presenter.requestedBackend);
        }
        DXLOGV("[d3d9] accelerated present declined: back buffer %ux%u != client %dx%d "
               "(Reset queued)",
               static_cast<unsigned>(g_presenter.presentParams.BackBufferWidth),
               static_cast<unsigned>(g_presenter.presentParams.BackBufferHeight),
               params->clientWidth, params->clientHeight);
        return FALSE;
    }
    g_lastResizeRequestTick = 0;

    if (g_presenter.deviceLost || g_presenter.window != params->window)
    {
        RequestDeviceWork(params->window, params->clientWidth,
                          params->clientHeight, params->activeMode,
                          g_presenter.requestedBackend);
        DXLOG("[d3d9] accelerated present declined: %s (lastError=0x%08lX)",
              g_presenter.deviceLost ? "device is lost"
                                     : "target window changed",
              static_cast<unsigned long>(g_diag.lastError));
        return FALSE;
    }

    // Frame barrier: claim the state word for the whole frame so no thread can
    // Reset or Release the device (or invalidate the font atlas) under an open ImGui frame.
    if (InterlockedCompareExchange(&g_deviceState, kDeviceInFrame, kDeviceReady)
        != kDeviceReady)
    {
        DXLOGV("[d3d9] accelerated present declined: the device worker claimed the presenter "
               "for this frame");
        return FALSE;
    }
    g_frameThreadId = GetCurrentThreadId();
    ++g_presentDepth;

    const BOOL result = PresentFrameLocked(*params);

    --g_presentDepth;
    InterlockedExchange(&g_deviceState, kDeviceReady);
    PresenterAfterFrame();
    return result;
}

extern "C" void EFZDX9_Presenter_RecordGdiPresentation(
    const EfzDx9PresentParams *params,
    LONGLONG entryQpc,
    LONGLONG returnQpc,
    HRESULT result)
{
    if (params == nullptr) return;
    PresentationTraceEntry *entry = nullptr;
    if (g_presentationTraceSize != 0u)
    {
        const size_t last = (g_presentationTraceWrite
                             + kPresentationTraceCapacity - 1u)
            % kPresentationTraceCapacity;
        if (g_presentationTrace[last].serial == params->presentSerial
            && g_presentationTrace[last].kind
                == static_cast<DWORD>(params->presentationKind)
            && FAILED(g_presentationTrace[last].result))
            entry = &g_presentationTrace[last];
    }
    if (entry == nullptr) entry = &appendPresentationTrace(*params);
    entry->gdiEntryQpc = entryQpc;
    entry->gdiReturnQpc = returnQpc;
    entry->result = result;
    if (entry->fallbackReason[0] == '\0')
    {
        std::snprintf(entry->fallbackReason, sizeof(entry->fallbackReason), "%s",
                      g_presenter.fallbackReason[0] != '\0'
                          ? g_presenter.fallbackReason
                          : "accelerated presenter declined");
    }
    if ((g_presentationTraceTotal % kPresentationTraceCapacity) == 0u)
        logPresentationTraceSummary();
}

extern "C" void EFZDX9_Presenter_SetActive(BOOL active)
{
    g_presenter.appActive = active != FALSE;
    if (!deviceReady())
    {
        g_inputResyncPending = true;
        if (active && g_deviceRequestWindow != nullptr)
            RequestDeviceWork(g_deviceRequestWindow, g_deviceRequestWidth,
                              g_deviceRequestHeight, g_deviceRequestMode,
                              g_deviceRequestBackend);
        return;
    }
    if (g_presenter.imguiInitialized)
    {
        ImGui::GetIO().AddFocusEvent(active != FALSE);
    }
    if (active && g_presenter.deviceLost)
        RequestDeviceWork(g_presenter.window, g_deviceRequestWidth,
                          g_deviceRequestHeight,
                          g_presenter.requestedMode,
                          g_presenter.requestedBackend);
}

extern "C" void EFZDX9_Presenter_Shutdown(void)
{
    if (g_presenter.window != nullptr && IsWindow(g_presenter.window))
    {
        const DWORD owner =
            GetWindowThreadProcessId(g_presenter.window, nullptr);
        if (owner != 0 && owner != GetCurrentThreadId())
        {
            SendMessageA(g_presenter.window, WM_QOH99_D3D_SHUTDOWN, 0, 0);
            return;
        }
    }
    if (deviceState() == kDeviceWorking || deviceState() == kDeviceInFrame)
    {
        DXLOG("[d3d9] shutdown deferred: presenter is busy "
              "(state=%ld)", deviceState());
        return;
    }
    DXLOG("[d3d9] presenter shutdown: creates=%lu resets=%lu failures=%lu "
          "losses=%lu reentrant=%lu lastError=0x%08lX",
          static_cast<unsigned long>(g_diag.creates),
          static_cast<unsigned long>(g_diag.resets),
          static_cast<unsigned long>(g_diag.failures),
          static_cast<unsigned long>(g_diag.losses),
          static_cast<unsigned long>(g_diag.reentrantPresents),
          static_cast<unsigned long>(g_diag.lastError));
    InterlockedExchange(&g_deviceState, kDeviceIdle);
    g_presentDepth = 0;
    g_deferredRequestValid = false;
    g_menuCaptureSyncPending = false;
    DestroyDevice();
    DiscardExclusiveExitWindowFrame();
    g_presenter.menuVisible = false;
    g_presenter.showDemoWindow = false;
    g_presenter.appActive = true;
}

extern "C" void EFZDX9_Presenter_ToggleMenu(void)
{
    g_presenter.menuVisible = !g_presenter.menuVisible;
    if (g_presenter.menuVisible)
    {
        g_trainingUi.refreshDraftsOnNextDraw = true;
    }

    if (deviceState() == kDeviceWorking)
    {
        g_menuCaptureSyncPending = true;
        return;
    }
    g_menuCaptureSyncPending = false;
    SyncTrainingMenuCapture();
}

extern "C" void EFZDX9_Presenter_ToggleInputDisplay(void)
{
    if (g_presenter.menuVisible && g_presenter.imguiInitialized
        && ImGui::GetIO().WantTextInput)
        return;
    g_inputDisplayVisible = !g_inputDisplayVisible;
}

extern "C" BOOL EFZDX9_Presenter_IsInputDisplayVisible(void)
{
    return g_inputDisplayVisible ? TRUE : FALSE;
}

extern "C" void __stdcall EFZDX9_SetReplayDisplayFlags(DWORD flags)
{
    g_inputDisplayVisible = (flags & QOH99_REPLAY_DISPLAY_INPUTS) != 0u;
    g_replayTransportVisible = (flags & QOH99_REPLAY_DISPLAY_TRANSPORT) != 0u;
}

extern "C" DWORD __stdcall EFZDX9_QueryReplayDisplayFlags(void)
{
    return (g_inputDisplayVisible ? QOH99_REPLAY_DISPLAY_INPUTS : 0u)
        | (g_replayTransportVisible ? QOH99_REPLAY_DISPLAY_TRANSPORT : 0u);
}

extern "C" BOOL __stdcall EFZDX9_Presenter_SubmitMenuNavigation(
    DWORD navigation)
{
    if (!g_presenter.menuVisible) return FALSE;
    if (navigation == QOH99_MENU_NAV_NONE
        || navigation >= QOH99_MENU_NAV_COUNT)
    {
        return FALSE;
    }
    InterlockedOr(&g_menuNavigationPending,
                  static_cast<LONG>(1u << navigation));
    volatile LONG& slot = g_menuNavigationCount[navigation];
    for (;;)
    {
        const LONG current = slot;
        if (current >= kMenuNavigationMaxPending) break;
        if (InterlockedCompareExchange(&slot, current + 1, current) == current)
            break;
    }
    return TRUE;
}

extern "C" void __stdcall EFZDX9_Presenter_SetMenuNavigationLabels(
    const char *confirmLabel, const char *backLabel)
{
    const auto copy = [](char *dst, size_t cap, const char *src) {
        if (src == nullptr || src[0] == '\0') return;
        size_t i = 0;
        for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
        dst[i] = '\0';
    };
    copy(g_menuConfirmLabel, sizeof(g_menuConfirmLabel), confirmLabel);
    copy(g_menuBackLabel, sizeof(g_menuBackLabel), backLabel);
}

extern "C" BOOL EFZDX9_Presenter_SubmitReplayHotkey(
    DWORD command, BOOL repeat)
{
    if (g_presenter.menuVisible)
        return FALSE;
    if (command != QOH99_REPLAY_TRANSPORT_TOGGLE_PAUSE
        && command != QOH99_REPLAY_TRANSPORT_STEP
        && command != QOH99_REPLAY_TRANSPORT_STEP_BACK
        && command != QOH99_REPLAY_TRANSPORT_RESTART
        && command != QOH99_REPLAY_TRANSPORT_SPEED_SLOWER
        && command != QOH99_REPLAY_TRANSPORT_SPEED_FASTER
        && command != QOH99_REPLAY_TOGGLE_COLLISION_DISPLAY
        && command != QOH99_REPLAY_TAKEOVER_EXIT
        && command != QOH99_REPLAY_TAKEOVER_P1
        && command != QOH99_REPLAY_TAKEOVER_P2
        && command != QOH99_REPLAY_TAKEOVER_RETRY
        && command != QOH99_REPLAY_TAKEOVER_DELAY_DECREASE
        && command != QOH99_REPLAY_TAKEOVER_DELAY_INCREASE)
        return FALSE;
    if (repeat && command != QOH99_REPLAY_TRANSPORT_STEP
        && command != QOH99_REPLAY_TRANSPORT_STEP_BACK)
        return FALSE;
    Qoh99ReplayTransportStatusV2 status;
    if (g_presenter.replayTransportSubmit != nullptr
        && ReplayTransportPoll(status))
    {
        ReplayTransportSubmit(status, command);
        return TRUE;
    }
    if (g_presenter.spectatorHotkeySink != nullptr
        && (command == QOH99_REPLAY_TRANSPORT_TOGGLE_PAUSE
            || command == QOH99_REPLAY_TRANSPORT_STEP
            || command == QOH99_REPLAY_TRANSPORT_SPEED_SLOWER
            || command == QOH99_REPLAY_TRANSPORT_SPEED_FASTER))
    {
        return g_presenter.spectatorHotkeySink(
                   command, repeat, g_presenter.spectatorHotkeyUserData) != 0
            ? TRUE : FALSE;
    }
    return FALSE;
}

extern "C" BOOL EFZDX9_Presenter_PrepareDevice(
    HWND window, int clientWidth, int clientHeight,
    QohPresentationMode mode, QohRendererBackend backend)
{
    if ((g_presenter.readiness == QOH_RENDERER_READY
         || g_presenter.readiness == QOH_RENDERER_FALLBACK_READY)
        && g_presenter.requestedMode == mode
        && g_presenter.requestedBackend == backend)
    {
        return TRUE;
    }
    RequestDeviceWork(window, clientWidth, clientHeight, mode, backend);
    const DWORD owner = GetWindowThreadProcessId(window, nullptr);
    if (owner == GetCurrentThreadId())
    {
        EFZDX9_Presenter_ProcessDeviceWorkOnWindowThread(window);
    }
    else
    {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutA(window, WM_QOH99_D3D_DEVICE_WORK, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &ignored);
    }
    return (g_presenter.readiness == QOH_RENDERER_READY
            || g_presenter.readiness == QOH_RENDERER_FALLBACK_READY)
        ? TRUE : FALSE;
}

extern "C" BOOL EFZDX9_Presenter_HasDevice(void)
{
    return deviceReady() ? TRUE : FALSE;
}

extern "C" void EFZDX9_Presenter_RequestDeviceIfNeeded(
    HWND window, int clientWidth, int clientHeight,
    QohPresentationMode mode, QohRendererBackend backend)
{
    if (deviceReady() && g_presenter.window == window
        && g_presenter.requestedMode == mode
        && g_presenter.requestedBackend == backend)
    {
        return;
    }
    if (g_presenter.readiness == QOH_RENDERER_FALLBACK_READY
        && g_presenter.requestedMode == mode
        && g_presenter.requestedBackend == backend)
        return;
    RequestDeviceWork(window, clientWidth, clientHeight, mode, backend);
}

extern "C" BOOL EFZDX9_Presenter_GetPresentationStatus(
    EfzDx9PresentationStatus *outStatus, DWORD outSize)
{
    if (outStatus == nullptr || outSize < sizeof(*outStatus))
        return FALSE;
    ZeroMemory(outStatus, sizeof(*outStatus));
    outStatus->structSize = sizeof(*outStatus);
    outStatus->requestedMode = g_presenter.requestedMode;
    outStatus->effectiveMode = g_presenter.effectiveMode;
    outStatus->requestedBackend = g_presenter.requestedBackend;
    outStatus->effectiveBackend = g_presenter.effectiveBackend;
    outStatus->readiness = g_presenter.readiness;
    outStatus->adapterOrdinal = g_presenter.adapterOrdinal;
    outStatus->monitor = g_presenter.monitor;
    outStatus->backBufferWidth = g_presenter.presentParams.BackBufferWidth;
    outStatus->backBufferHeight = g_presenter.presentParams.BackBufferHeight;
    outStatus->refreshHz = g_presenter.presentParams.FullScreen_RefreshRateInHz;
    outStatus->resetGeneration = g_presenter.resetGeneration;
    outStatus->lastError = g_diag.lastError;
    std::snprintf(outStatus->adapterDescription,
                  sizeof(outStatus->adapterDescription), "%s",
                  g_presenter.adapterDescription);
    std::snprintf(outStatus->fallbackReason,
                  sizeof(outStatus->fallbackReason), "%s",
                  g_presenter.fallbackReason);
    return TRUE;
}

extern "C" __declspec(dllexport)
void __stdcall EFZDDRAW_SetPaletteOwnership(unsigned localSlotMask)
{
    const unsigned mask = localSlotMask & 0xFu;
    if (g_paletteOwnershipAnnounced && mask == g_paletteLocalSlotMask) return;
    g_paletteLocalSlotMask = mask;
    g_paletteOwnershipAnnounced = true;
    PALLOG("[palette] this machine may load local palette files for slots: "
           "%s%s%s%s%s",
           (mask & 1u) ? "P1 " : "", (mask & 2u) ? "P2 " : "",
           (mask & 4u) ? "P3 " : "", (mask & 8u) ? "P4 " : "",
           mask == 0u ? "(none - every palette comes from the recording or "
                        "the network)" : "");
}

extern "C" __declspec(dllexport)
int __stdcall EFZDDRAW_QueryPalette(int slot, unsigned char *out, int capacity,
                                    int *variantOut)
{
    if (slot < 0 || slot >= 4) return 0;
    if (out == nullptr || capacity < static_cast<int>(kPaletteTableBytes))
        return 0;
    const PaletteAutoSlot &self = g_paletteAuto[slot];
    if (!self.haveCustom) return 0;
    std::memcpy(out, self.customTable, kPaletteTableBytes);
    if (variantOut != nullptr) *variantOut = self.customVariant;
    return static_cast<int>(kPaletteTableBytes);
}

extern "C" __declspec(dllexport)
BOOL __stdcall EFZDDRAW_SubmitPalette(int slot, const unsigned char *bytes,
                                      int length, int variant, int remote)
{
    std::string why;
    if (!PaletteAutoEnabled())
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            PALLOG("[palette] a palette arrived from the %s for slot %d, but "
                   "custom palettes are OFF (PaletteAutoLoad=0), so it will "
                   "not be drawn", remote ? "network/replay" : "local file",
                   slot);
        }
    }
    const bool ok = PaletteSubmit(
        slot, bytes, length < 0 ? 0u : static_cast<size_t>(length),
        remote ? PaletteSource::RemotePeer : PaletteSource::LocalFile, why,
        variant);
    if (!ok)
    {
        PALLOG("[palette] slot %d submit from the %s REFUSED: %s", slot,
               remote ? "network/replay" : "local", why.c_str());
        return FALSE;
    }
    return TRUE;
}

extern "C" void __stdcall EFZDX9_RegisterNetplayHudProvider(
    EfzDx9NetplayHudProvider provider,
    void *userData)
{
    g_presenter.hudProvider = provider;
    g_presenter.hudProviderUserData = userData;
}

extern "C" void __stdcall EFZDX9_RegisterOverlayProvider(
    EfzDx9OverlayProvider provider,
    void *userData)
{
    g_presenter.overlayProvider = provider;
    g_presenter.overlayProviderUserData = userData;
}

extern "C" void __stdcall EFZDX9_RegisterTrainingUiProvider(
    DWORD apiVersion,
    EfzDx9TrainingStatusProviderV1 statusProvider,
    EfzDx9TrainingCommandEnqueueV1 enqueueCommand,
    EfzDx9TrainingMenuCaptureV1 setMenuCapture,
    void *userData)
{
    if (g_presenter.trainingMenuCapture != nullptr
        && g_trainingUi.capturedState == 1)
    {
        g_presenter.trainingMenuCapture(
            g_trainingUi.capturedGeneration, FALSE,
            g_presenter.trainingUiUserData);
    }

    const bool compatible =
        apiVersion == QOH99_TRAINING_UI_API_VERSION;
    g_presenter.trainingUiRejectedApiVersion =
        (statusProvider != nullptr && !compatible) ? apiVersion : 0;
    g_presenter.trainingUiApiVersion =
        compatible ? apiVersion : 0;
    g_presenter.trainingStatusProvider =
        compatible ? statusProvider : nullptr;
    g_presenter.trainingCommandEnqueue =
        compatible ? enqueueCommand : nullptr;
    g_presenter.trainingMenuCapture =
        compatible ? setMenuCapture : nullptr;
    g_presenter.trainingUiUserData =
        compatible ? userData : nullptr;

    g_trainingUi.generation = 0;
    g_trainingUi.lifecycleState = QOH99_TRAINING_LIFECYCLE_OFF;
    g_trainingUi.scene = 0;
    g_trainingUi.matchSubstate = 0;
    g_trainingUi.statusObserved = false;
    g_trainingUi.refreshDraftsOnNextDraw = true;
    g_trainingUi.capturedGeneration = 0;
    g_trainingUi.capturedState = -1;
    g_trainingUi.lastRejectedCommandId = 0;
    g_trainingUi.lastRejectedTick = 0;
    SyncTrainingMenuCapture();
}

extern "C" void __stdcall EFZDX9_RegisterInputDisplayProvider(
    DWORD apiVersion,
    EfzDx9InputDisplayProviderV1 provider,
    void *userData)
{
    const bool compatible = apiVersion == QOH99_INPUT_DISPLAY_API_VERSION;
    g_presenter.inputDisplayRejectedApiVersion =
        (provider != nullptr && !compatible) ? apiVersion : 0;
    g_presenter.inputDisplayApiVersion = compatible ? apiVersion : 0;
    g_presenter.inputDisplayProvider = compatible ? provider : nullptr;
    g_presenter.inputDisplayUserData = compatible ? userData : nullptr;
}

extern "C" void __stdcall EFZDX9_RegisterReplayTransportProvider(
    DWORD apiVersion,
    EfzDx9ReplayTransportStatusV2Fn status,
    EfzDx9ReplayTransportSubmitV2Fn submit,
    void *userData)
{
    const bool compatible = apiVersion == QOH99_REPLAY_TRANSPORT_API_VERSION;
    g_presenter.replayTransportRejectedApiVersion =
        (status != nullptr && !compatible) ? apiVersion : 0;
    g_presenter.replayTransportApiVersion = compatible ? apiVersion : 0;
    g_presenter.replayTransportStatus = compatible ? status : nullptr;
    g_presenter.replayTransportSubmit = compatible ? submit : nullptr;
    g_presenter.replayTransportUserData = compatible ? userData : nullptr;
    g_replayTakeoverDelayGeneration = 0u;
}

extern "C" void __stdcall EFZDX9_RegisterSpectatorHotkeySink(
    EfzDx9SpectatorHotkeySinkFn sink, void *userData)
{
    g_presenter.spectatorHotkeySink = sink;
    g_presenter.spectatorHotkeyUserData = sink != nullptr ? userData : nullptr;
}

extern "C" void __stdcall EFZDX9_RegisterTrainingMenuBridge(
    EfzDx9TrainingMenuRendererFn renderer,
    EfzDx9TrainingMenuVisibleFn isVisible,
    EfzDx9TrainingMenuHandleMessageFn handleMessage,
    EfzDx9TrainingMenuWantsDrawFn wantsDraw,
    EfzDx9TrainingMenuDeviceNotifyFn deviceNotify,
    void *userData)
{
    g_presenter.trainingMenuRenderer = renderer;
    g_presenter.trainingMenuVisible = isVisible;
    g_presenter.trainingMenuHandleMessage = handleMessage;
    g_presenter.trainingMenuWantsDraw = wantsDraw;
    g_presenter.trainingMenuDeviceNotify = deviceNotify;
    g_presenter.trainingMenuUserData = userData;
}
