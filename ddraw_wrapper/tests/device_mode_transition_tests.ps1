# Run from an MSVC developer shell. No window, graphics driver, or game is used.
# The production request/provision functions run against a fake D3D boundary.
param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$wrapperDir = Split-Path -Parent $PSScriptRoot
$source = Get-Content -LiteralPath (Join-Path $wrapperDir 'render_dx9_imgui.cpp') -Raw
$first = $source.IndexOf('bool ProvisionDevice(HWND window, int clientWidth, int clientHeight,')
if ($first -lt 0) { throw 'Device provision function not found' }
$last = $source.IndexOf('// "Menu is capturing input"', $first)
if ($last -le $first) { throw 'Device request function boundary not found' }
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('qoh99-device-mode-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testDir | Out-Null
Set-Content -LiteralPath (Join-Path $testDir 'device_under_test.inc') -Value $source.Substring($first, $last - $first)
$create = $source.LastIndexOf('bool CreateDevice(HWND window, int clientWidth, int clientHeight,')
$createBody = $source.IndexOf('{', $create) + 1
$createReady = $source.IndexOf('g_presenter.requestedMode = mode;', $createBody)
if ($create -lt 0 -or $createReady -le $createBody) { throw 'Device destruction boundary not found' }
Set-Content -LiteralPath (Join-Path $testDir 'destroy_before_create.inc') -Value $source.Substring($createBody, $createReady - $createBody)
$restore = $source.IndexOf('struct ExclusiveExitWindowFrame {')
if ($restore -lt 0 -or $restore -ge $first) { throw 'Exit frame preservation helper not found' }
Set-Content -LiteralPath (Join-Path $testDir 'exit_frame_restore.inc') -Value $source.Substring($restore, $first - $restore)
$harness = @'
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <cstdint>
#include "qoh99_presentation_policy.h"
using HWND = void*;
using LONG = long;
using DWORD = unsigned long;
using UINT = unsigned int;
using HRESULT = long;
constexpr HRESULT D3DERR_DEVICELOST=-1, D3DERR_DEVICENOTRESET=-2, S_PRESENT_OCCLUDED=1;
#define SUCCEEDED(hr) ((hr)>=0)
#define HRESULT_FROM_WIN32(hr) (-(long)(hr))
constexpr LONG kDeviceReady=1,kDeviceInFrame=2,kDeviceFailed=3,kDeviceWorking=4,kDevicePending=5;
constexpr unsigned WM_QOH99_D3D_DEVICE_WORK=1,kDeviceLostRetryMs=100;
HRESULT deviceStatus=0;
struct Device { HRESULT TestCooperativeLevel() { return deviceStatus; } HRESULT CheckDeviceState(HWND) { return deviceStatus; } } device;
struct Parameters { bool Windowed=true; UINT BackBufferWidth=640, BackBufferHeight=480; };
struct State {
 Device *device=nullptr,*deviceEx=nullptr; HWND window=nullptr;
 QohPresentationMode requestedMode=QOH_PRESENTATION_WINDOWED,effectiveMode=QOH_PRESENTATION_WINDOWED;
 QohRendererBackend requestedBackend=QOH_RENDERER_AUTO;
 QohRendererReadiness readiness=QOH_RENDERER_READY;
 bool exDisabledAfterHardFailure=false,deviceLost=false;
 Parameters presentParams;
} g_presenter;
struct { HRESULT lastError=0; } g_diag;
LONG g_deviceState=kDeviceReady,g_deviceRequestGeneration=0;
DWORD g_frameThreadId=1,g_deviceLastFailureTick=0,g_deviceRetryDelayMs=100;
DWORD currentThreadId=1;
HWND g_deferredRequestWindow=nullptr,g_deviceRequestWindow=nullptr;
int g_deferredRequestWidth=0,g_deferredRequestHeight=0,g_deviceRequestWidth=0,g_deviceRequestHeight=0;
QohPresentationMode g_deferredRequestMode=QOH_PRESENTATION_WINDOWED,g_deviceRequestMode=QOH_PRESENTATION_WINDOWED;
QohRendererBackend g_deferredRequestBackend=QOH_RENDERER_AUTO,g_deviceRequestBackend=QOH_RENDERER_AUTO;
bool g_deferredRequestValid=false;
int creates=0,resets=0,posts=0,failures=0;
bool createSucceeds=true,resetSucceeds=true;
bool clobberDuringCreate=false;
bool exclusiveRequestDuringCreate=false;
LONG deviceState() { return g_deviceState; }
bool IsWindow(HWND h) { return h!=nullptr; }
DWORD GetCurrentThreadId() { return currentThreadId; }
DWORD GetTickCount() { return 1000; }
DWORD GetLastError() { return 1; }
LONG InterlockedIncrement(LONG* v) { return ++*v; }
LONG InterlockedExchange(LONG* v,LONG x) { LONG old=*v;*v=x;return old; }
bool PostMessageA(HWND,unsigned,unsigned,unsigned) { ++posts;return true; }
void NotifyDeviceObjectsValid() {}
void LogDeviceWindowGeometry(const char*,HWND,QohPresentationMode,int,int) {}
struct RECT { int left,top,right,bottom; } windowRect;
LONG windowStyle=0x14CF0000,windowExStyle=0x40000;
constexpr unsigned SWP_NOZORDER=1,SWP_NOACTIVATE=2,SWP_FRAMECHANGED=4;
constexpr int GWL_STYLE=-16,GWL_EXSTYLE=-20;
constexpr LONG WS_EX_TOPMOST=8;
const HWND HWND_NOTOPMOST=(HWND)-2;
bool GetWindowRect(HWND,RECT* out) { *out=windowRect;return true; }
LONG GetWindowLongA(HWND,int field) { return field==GWL_STYLE?windowStyle:windowExStyle; }
LONG SetWindowLongA(HWND,int field,LONG value) { LONG& dest=field==GWL_STYLE?windowStyle:windowExStyle;LONG old=dest;dest=value;return old; }
bool SetWindowPos(HWND,HWND order,int x,int y,int width,int height,unsigned) {
 if(order==HWND_NOTOPMOST) windowExStyle&=~WS_EX_TOPMOST;
 windowRect={x,y,x+width,y+height};return true;
}
void DestroyDevice() {
 if(g_presenter.device && !g_presenter.presentParams.Windowed) {
  windowRect={0,0,2560,1440};
  windowStyle|=0x80000000;windowExStyle|=WS_EX_TOPMOST;
 }
 g_presenter.device=nullptr;
}
#include "exit_frame_restore.inc"
bool RequestDeviceWork(HWND,int,int,QohPresentationMode,QohRendererBackend);
bool CreateDevice(HWND w,int width,int height,QohPresentationMode mode,QohRendererBackend backend) {
 HWND window=w;
 int clientWidth=width,clientHeight=height;
#include "destroy_before_create.inc"
 ++creates;
 if(exclusiveRequestDuringCreate)
  RequestDeviceWork(w,2560,1440,QOH_PRESENTATION_EXCLUSIVE,backend);
 if(clobberDuringCreate) windowRect={0,0,2560,1440};
 if(!createSucceeds) { g_presenter.device=nullptr;return false; }
 g_presenter.window=w;g_presenter.device=&device;g_presenter.effectiveMode=mode;
 g_presenter.requestedMode=mode;g_presenter.requestedBackend=backend;
 g_presenter.presentParams={mode!=QOH_PRESENTATION_EXCLUSIVE,(UINT)width,(UINT)height};return true;
}
bool ResetDevice(int width,int height,QohPresentationMode mode,QohRendererBackend) {
 ++resets;if(!resetSucceeds) return false;
 g_presenter.effectiveMode=mode;
 g_presenter.presentParams={mode!=QOH_PRESENTATION_EXCLUSIVE,(UINT)width,(UINT)height};return true;
}
#include "device_under_test.inc"
#define CHECK(x) do { if(!(x)) { std::printf("FAIL line%d: %s\n",__LINE__,#x); ++failures; }} while(0)
void prepare(QohPresentationMode mode,int width=2560,int height=1440) {
 g_presenter={};g_presenter.device=&device;g_presenter.window=(HWND)1;
 g_presenter.requestedMode=g_presenter.effectiveMode=mode;
 g_presenter.presentParams={mode!=QOH_PRESENTATION_EXCLUSIVE,(UINT)width,(UINT)height};
 creates=resets=posts=0;g_deviceState=kDeviceReady;
 deviceStatus=0;createSucceeds=resetSucceeds=true;
 clobberDuringCreate=false;
 exclusiveRequestDuringCreate=false;
 currentThreadId=1;
 g_exclusiveExitWindowFrame={};
 windowStyle=0x14CF0000;windowExStyle=0x40000;
 windowRect={100,100,1076,859};
}
bool requestAndProvision(QohPresentationMode mode,int width,int height) {
 CHECK(RequestDeviceWork((HWND)1,width,height,mode,QOH_RENDERER_AUTO));
 CHECK(posts==1 && g_presenter.requestedMode==mode);
 return ProvisionDevice(g_deviceRequestWindow,g_deviceRequestWidth,g_deviceRequestHeight,g_deviceRequestMode,g_deviceRequestBackend);
}
int main() {
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 CHECK(requestAndProvision(QOH_PRESENTATION_WINDOWED,960,720));
 CHECK(g_presenter.presentParams.Windowed && g_presenter.effectiveMode==QOH_PRESENTATION_WINDOWED);
 CHECK(g_presenter.presentParams.BackBufferWidth==960 && g_presenter.presentParams.BackBufferHeight==720);
 CHECK(creates+resets==1);
 CHECK(windowRect.left==100 && windowRect.top==100 && windowRect.right==1076 && windowRect.bottom==859);
 /* Equal dimensions must not hide an actual exclusive/windowed transition. */
 prepare(QOH_PRESENTATION_BORDERLESS);
 CHECK(requestAndProvision(QOH_PRESENTATION_EXCLUSIVE,2560,1440));
 CHECK(!g_presenter.presentParams.Windowed && g_presenter.effectiveMode==QOH_PRESENTATION_EXCLUSIVE);
 CHECK(creates+resets==1);
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 CHECK(requestAndProvision(QOH_PRESENTATION_BORDERLESS,2560,1440));
 CHECK(g_presenter.presentParams.Windowed && g_presenter.effectiveMode==QOH_PRESENTATION_BORDERLESS);
 CHECK(creates+resets==1);
 /* Repeated unchanged modes remain no-ops; ordinary resize still resets. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 CHECK(requestAndProvision(QOH_PRESENTATION_EXCLUSIVE,2560,1440));
 CHECK(creates==0 && resets==0 && !g_presenter.presentParams.Windowed);
 prepare(QOH_PRESENTATION_WINDOWED,640,480);
 CHECK(requestAndProvision(QOH_PRESENTATION_WINDOWED,960,720));
 CHECK(creates==0 && resets==1 && g_presenter.presentParams.Windowed);
 /* A lost old mode must not block a different mode from being created. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 g_presenter.deviceLost=true;deviceStatus=D3DERR_DEVICELOST;
 CHECK(requestAndProvision(QOH_PRESENTATION_WINDOWED,960,720));
 CHECK(creates==1 && resets==0 && g_presenter.effectiveMode==QOH_PRESENTATION_WINDOWED);
 /* Same-mode loss keeps its retry gate, then resets once it is resettable. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 g_presenter.deviceLost=true;deviceStatus=D3DERR_DEVICELOST;
 CHECK(!requestAndProvision(QOH_PRESENTATION_EXCLUSIVE,2560,1440));
 CHECK(creates==0 && resets==0 && g_presenter.effectiveMode==QOH_PRESENTATION_EXCLUSIVE);
 CHECK(g_deviceRetryDelayMs==kDeviceLostRetryMs);
 deviceStatus=D3DERR_DEVICENOTRESET;
 CHECK(ProvisionDevice((HWND)1,2560,1440,QOH_PRESENTATION_EXCLUSIVE,QOH_RENDERER_AUTO));
 CHECK(creates==0 && resets==1);
 /* A failed change must retry even after its target was already published. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 createSucceeds=false;
 CHECK(!requestAndProvision(QOH_PRESENTATION_WINDOWED,960,720));
 CHECK(g_presenter.requestedMode==QOH_PRESENTATION_WINDOWED && g_presenter.device==nullptr);
 createSucceeds=true;
 CHECK(ProvisionDevice((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(creates==2 && g_presenter.effectiveMode==QOH_PRESENTATION_WINDOWED);
 /* An unsuccessful reset still escalates to the existing full rebuild. */
 prepare(QOH_PRESENTATION_WINDOWED,640,480);
 resetSucceeds=false;
 CHECK(requestAndProvision(QOH_PRESENTATION_WINDOWED,960,720));
 CHECK(creates==1 && resets==1 && g_presenter.presentParams.BackBufferWidth==960);
 /* The exclusive driver may change the HWND after queuing or during creation. */
 for(bool duringCreate : {false,true}) {
  prepare(QOH_PRESENTATION_EXCLUSIVE);
  CHECK(RequestDeviceWork((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
  if(duringCreate) clobberDuringCreate=true;
  else {
   windowRect={0,0,2560,1440};
   CHECK(RequestDeviceWork((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
  }
  CHECK(ProvisionDevice((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
  CHECK(windowRect.left==100 && windowRect.top==100 && windowRect.right==1076 && windowRect.bottom==859);
  CHECK(windowStyle==0x14CF0000 && windowExStyle==0x40000);
 }
 /* Overlay requests made inside a frame must capture before deferral too. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 g_deviceState=kDeviceInFrame;
 CHECK(!RequestDeviceWork((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(g_deferredRequestValid);
 windowRect={0,0,2560,1440};
 g_deviceState=kDeviceReady;
 CHECK(RequestDeviceWork((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(ProvisionDevice((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(windowRect.left==100 && windowRect.top==100 && windowRect.right==1076 && windowRect.bottom==859);
 /* A later transition captures the user's newly resized frame. */
 windowRect={300,200,1596,1199};
 g_presenter.effectiveMode=QOH_PRESENTATION_EXCLUSIVE;
 g_presenter.presentParams.Windowed=false;
 CHECK(RequestDeviceWork((HWND)1,1280,960,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 windowRect={0,0,2560,1440};
 CHECK(ProvisionDevice((HWND)1,1280,960,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(windowRect.left==300 && windowRect.top==200 && windowRect.right==1596 && windowRect.bottom==1199);
 /* Requests rejected while another thread presents must not retain geometry. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 g_deviceState=kDeviceInFrame;currentThreadId=2;
 CHECK(!RequestDeviceWork((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 windowRect={300,200,1596,1199};
 g_deviceState=kDeviceReady;currentThreadId=1;
 CHECK(RequestDeviceWork((HWND)1,1280,960,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(ProvisionDevice((HWND)1,1280,960,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(windowRect.left==300 && windowRect.top==200 && windowRect.right==1596 && windowRect.bottom==1199);
 /* A canceled queued exit must not restore its stale frame on a later exit. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 CHECK(RequestDeviceWork((HWND)1,960,720,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(RequestDeviceWork((HWND)1,2560,1440,QOH_PRESENTATION_EXCLUSIVE,QOH_RENDERER_AUTO));
 CHECK(ProvisionDevice((HWND)1,2560,1440,QOH_PRESENTATION_EXCLUSIVE,QOH_RENDERER_AUTO));
 windowRect={300,200,1596,1199};
 CHECK(RequestDeviceWork((HWND)1,1280,960,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(ProvisionDevice((HWND)1,1280,960,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(windowRect.left==300 && windowRect.top==200 && windowRect.right==1596 && windowRect.bottom==1199);
 /* Driver callbacks may request exclusive while the windowed create is active. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 exclusiveRequestDuringCreate=true;
 CHECK(requestAndProvision(QOH_PRESENTATION_WINDOWED,960,720));
 CHECK(windowRect.left==100 && windowRect.top==100 && windowRect.right==1076 && windowRect.bottom==859);
 /* Failed creation restores once and leaves no stale snapshot for retry. */
 prepare(QOH_PRESENTATION_EXCLUSIVE);
 createSucceeds=false;
 CHECK(!requestAndProvision(QOH_PRESENTATION_WINDOWED,960,720));
 CHECK(windowRect.left==100 && windowRect.top==100 && windowRect.right==1076 && windowRect.bottom==859);
 windowRect={300,200,1596,1199};
 createSucceeds=true;
 CHECK(ProvisionDevice((HWND)1,1280,960,QOH_PRESENTATION_WINDOWED,QOH_RENDERER_AUTO));
 CHECK(windowRect.left==300 && windowRect.top==200 && windowRect.right==1596 && windowRect.bottom==1199);
 std::printf("device mode transition: %d failed checks\n",failures);return failures?1:0;
}
'@
Set-Content -LiteralPath (Join-Path $testDir 'device_mode_test.cpp') -Value $harness
Push-Location $testDir
try {
    & $Compiler /nologo /std:c++17 /EHsc /MT "/I$wrapperDir" device_mode_test.cpp /Fe:device_mode_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $LASTEXITCODE" }
    & (Join-Path $testDir 'device_mode_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Device mode regression failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
