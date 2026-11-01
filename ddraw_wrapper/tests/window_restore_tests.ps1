# Run from an MSVC developer shell: powershell -File tests/window_restore_tests.ps1
# Compile the production deferred-restore functions with window-system stubs.
# This tests late message delivery without opening windows or loading the game.
param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$wrapperDir = Split-Path -Parent $PSScriptRoot
$source = Get-Content -LiteralPath (Join-Path $wrapperDir 'ddraw.c') -Raw
$first = $source.IndexOf('static void apply_display_restore(MyDDraw *dd)')
if ($first -lt 0) { throw 'Deferred restore function not found' }
$last = $source.IndexOf('static BYTE wrapper_dik_from_window_lparam', $first)
if ($last -le $first) { throw 'Deferred restore function boundary not found' }
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('qoh99-window-restore-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testDir | Out-Null
Set-Content -LiteralPath (Join-Path $testDir 'restore_under_test.inc') -Value $source.Substring($first, $last - $first)
$harness = @'
#include <stdio.h>
#include "qoh99_presentation_policy.h"
typedef int BOOL;
typedef unsigned long DWORD;
typedef long LONG;
typedef void* HWND;
typedef struct RECT { long left, top, right, bottom; } RECT;
typedef struct MyDDraw { HWND hWnd; QohPresentationMode presentationMode; RECT savedWinRect; BOOL hasSavedWinRect; } MyDDraw;
#define TRUE 1
#define FALSE 0
#define HWND_TOP ((HWND)0)
#define SWP_NOZORDER 1
#define SWP_FRAMECHANGED 2
#define SWP_SHOWWINDOW 4
#define DISPLAY_RESTORE_AFTER_PRESENTS 2u
#define WM_QOH99_APPLY_DISPLAY 1
#define LOG(...) ((void)0)
static QohPresentationMode g_restorePresentationMode;
static LONG g_displayRestorePending, g_displayRestoreDone;
static int maximized, posts, resizes, fullscreen, repaints, builds, failures;
static int width, height, savedWidth, savedHeight;
static int IsWindow(HWND h) { return h != 0; }
static int IsZoomed(HWND h) { return h != 0 && maximized; }
static LONG InterlockedCompareExchange(LONG* p, LONG desired, LONG expected) { LONG old=*p; if(old==expected)*p=desired; return old; }
static LONG InterlockedExchange(LONG* p, LONG value) { LONG old=*p; *p=value; return old; }
static int PostMessageA(HWND h, unsigned m, unsigned w, unsigned l) { (void)h;(void)m;(void)w;(void)l; ++posts; return 1; }
static BOOL build_windowed_restore_rect(MyDDraw* dd, RECT* r, int* w, int* h) { (void)dd; ++builds; r->left=10; r->top=20; r->right=10+savedWidth; r->bottom=20+savedHeight; *w=savedWidth; *h=savedHeight; return TRUE; }
static void set_fullscreen_window_mode(MyDDraw* dd, QohPresentationMode mode) { dd->presentationMode=mode; ++fullscreen; width=1920; height=1080; }
static void present_current(MyDDraw* dd) { (void)dd; ++repaints; }
static int SetWindowPos(HWND h, HWND top, int x, int y, int w, int ht, unsigned flags) { (void)h;(void)top;(void)x;(void)y;(void)flags; ++resizes; width=w; height=ht; return 1; }
static const char* wrapper_ini_path(void) { return "test.ini"; }
#include "restore_under_test.inc"
#define CHECK(x) do { if(!(x)) { printf("FAIL line%d: %s\n",__LINE__,#x); ++failures; }} while(0)
static void reset(void) { maximized=posts=resizes=fullscreen=repaints=builds=0; savedWidth=width=640;savedHeight=height=480; g_restorePresentationMode=QOH_PRESENTATION_WINDOWED; g_displayRestorePending=1;g_displayRestoreDone=0; }
int main(void) {
 MyDDraw dd={(HWND)1,QOH_PRESENTATION_WINDOWED,{0},FALSE};
 reset();
 qoh99_display_restore_tick(&dd,1u);
 CHECK(posts==0 && g_displayRestorePending==1);
 /* First replay frame is paused; maximize, then resume to publish frame two. */
 maximized=1;width=2560;height=1377;
 qoh99_display_restore_tick(&dd,2u);
 CHECK(posts==1 && g_displayRestoreDone==1 && g_displayRestorePending==0);
 apply_display_restore(&dd);
 CHECK(width==2560 && height==1377 && resizes==0 && fullscreen==0 && builds==0);
 qoh99_display_restore_tick(&dd,3u); CHECK(posts==1);
 /* Maximize after the message was queued must win too. */
 reset();qoh99_display_restore_tick(&dd,2u);maximized=1;width=2560;height=1377;
 apply_display_restore(&dd);CHECK(width==2560 && height==1377 && resizes==0);
 /* Even a saved maximized client must not become a normal clamped rectangle. */
 reset();maximized=1;width=savedWidth=2560;height=savedHeight=1377;
 qoh99_display_restore_tick(&dd,2u);apply_display_restore(&dd);
 CHECK(builds==0 && resizes==0 && width==2560 && height==1377);
 /* Unchanged normal launches still restore persisted size and mode. */
 reset();savedWidth=960;savedHeight=720;apply_display_restore(&dd);
 CHECK(resizes==1 && width==960 && height==720 && repaints==1);
 reset();g_restorePresentationMode=QOH_PRESENTATION_BORDERLESS;
 apply_display_restore(&dd);CHECK(fullscreen==1 && dd.hasSavedWinRect && width==1920 && height==1080);
 dd.presentationMode=QOH_PRESENTATION_WINDOWED;
 reset();g_restorePresentationMode=QOH_PRESENTATION_BORDERLESS;maximized=1;width=2560;height=1377;
 apply_display_restore(&dd);CHECK(fullscreen==0 && resizes==0 && width==2560 && height==1377);
 printf("window restore: %d failed checks\n",failures);return failures?1:0;
}
'@
Set-Content -LiteralPath (Join-Path $testDir 'window_restore_test.c') -Value $harness
Push-Location $testDir
try {
    & $Compiler /nologo /TC /MT "/I$wrapperDir" window_restore_test.c /Fe:window_restore_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $LASTEXITCODE" }
    & (Join-Path $testDir 'window_restore_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Window restore regression failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
