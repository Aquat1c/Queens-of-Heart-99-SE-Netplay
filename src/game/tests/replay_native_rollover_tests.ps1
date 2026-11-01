# Compile the actual game-thread rollover helper with native/runtime boundaries stubbed.
# Run from an x86 MSVC developer shell. No game, window, or installation is launched.
param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = Get-Content -LiteralPath (Join-Path $sourceRoot 'GameHooks.cpp') -Raw
$first = $source.IndexOf('static int replayBeginNextNativeGameV3() {')
$last = $source.IndexOf('static int doReplayV3(FrameDispatchFn orig) {', $first)
if ($first -lt 0 -or $last -le $first) { throw 'Native rollover helper missing' }
$helper = $source.Substring($first, $last - $first)
foreach ($forbidden in @('netPaceInit(', 'InterlockedExchange(', 'SetWindowPos(', 'DestroyWindow(')) {
    if ($helper.Contains($forbidden)) { throw "Rollover resets process-owned state: $forbidden" }
}
$dispatch = $source.Substring($last, $source.IndexOf('static ', $last + 1) - $last)
if ($dispatch.IndexOf('replayNextGamePendingV3()') -gt $dispatch.IndexOf('replayExitRequested()')) {
    throw 'Rollover must be serviced before the old completed session exits'
}
$inl = Get-Content -LiteralPath (Join-Path $sourceRoot 'training/TrainingGameIntegration.inl') -Raw
$publishFirst = $inl.IndexOf('static void trainingPublishHitboxSnapshot(bool fromRenderPhase) {')
$publishLast = $inl.IndexOf("// Producer B's half", $publishFirst)
$publish = $inl.Substring($publishFirst, $publishLast - $publishFirst)
if ($publish.IndexOf('replayHitboxPresentationAllowed(') -lt 0 -or
    $publish.IndexOf('replayHitboxPresentationAllowed(') -gt $publish.IndexOf('trainingReadHitboxCamera(camera)')) {
    throw 'Result-screen gate must precede geometry reads/publication, including the final recorded tick'
}
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('qoh99-replay-rollover-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testDir | Out-Null
Set-Content -LiteralPath (Join-Path $testDir 'rollover_under_test.inc') -Value $helper
$harness = @'
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>
#define QLOG(...) ((void)0)
constexpr int kNetHoldFrameRet=1;
static bool pending=false, accept=true;
static int begins=0, audioResets=0, paletteResets=0;
static std::vector<int> calls;
static std::map<uintptr_t,uint32_t> nativeState;
void wr32(uintptr_t address,uint32_t value){nativeState[address]=value;}
namespace qgame {
namespace replay {
bool replayNextGamePendingV3(){return pending;}
bool replayBeginNextGameV3(){++begins;if(!accept)return false;pending=false;calls.push_back(1);return true;}
}
void stopDirectBoot(){calls.push_back(2);}
void paletteClearReplayHold(){++paletteResets;}
}
namespace qreplay {
struct ReplayBootstrapObservationV2{int marker=0;};
struct ConfirmedGameStartV2{int marker=0;};
}
struct ReplayTakeoverNativeSnapshot{bool valid=false;};
struct SeekCache{int count=0;void clear(){count=0;}};
enum class ReplayBootstrapPhase{WaitingForStart,DirectBoot,NativeSetup,Complete};
static ReplayTakeoverNativeSnapshot g_replayTakeoverSnapshot, g_replayStartSnapshot;
static qreplay::ReplayBootstrapObservationV2 g_replayStartObservation;
static qreplay::ConfirmedGameStartV2 g_replayGameStartV2;
static SeekCache g_replaySeekCache;
static void* g_replayTakeoverInputWindow=nullptr;
static bool g_replayHalted, g_replayBootstrapSelectorLifecycleValid, g_replayClosePosted;
static bool g_replayObserveBootstrapDiffered, g_netGameplaySegmentActive, g_netGameplaySegmentSeen, g_netGameplayRefreshPending;
static uint64_t g_replayTicks,g_replayHiddenTicks,g_replayBufferingSlots,g_replayObservedBootstrapFingerprint;
static uint64_t g_replayObserveFrames,g_replayObservePreMismatch,g_replayObservePostMismatch,g_ovrFrame;
static uint32_t g_replayObservedInitialPhase,g_replayBootstrapSlots,g_replayBootstrapSelectorLaunch;
static uint32_t g_replayWinScreenTicks,g_replayObserveFirstPostMatch,g_replayObserveLastPostMismatch;
static uint32_t g_netEpoch,g_netBootstrapTransientsEpoch,g_netFpuInitializedEpoch,g_netFrame;
static std::array<uint32_t,2> g_replayBootstrapSelectorConfirms,g_replayBootstrapKnockoutLatches;
static int g_replayTerminalSerial,g_netInject[2];
static ReplayBootstrapPhase g_replayBootstrapPhase;
static bool g_audioV2Enabled=true, hitboxCache=true, rollbackReady=true, canonicalAudioDirty=true;
// Process/viewer state deliberately has no reset API in this harness.
static int speed=8000, ownedSerial=111, publishedSerial=110, window=123;
static bool requestedHitboxes=true, requestedInputs=true;
void replayTakeoverStopInputRuntime(){calls.push_back(3);}
void replayReleaseCustomHitboxes(){calls.push_back(4);hitboxCache=false;}
void netPaletteResetForSession(){++paletteResets;}
void reregisterFrameData(){calls.push_back(5);rollbackReady=false;}
void resetCanonicalAudioSession(){canonicalAudioDirty=false;}
namespace qaudio {
struct Audio{void resetSession(){++audioResets;calls.push_back(6);}};
Audio& runtime(){static Audio a;return a;}
}
#include "rollover_under_test.inc"
static int failures=0;
#define CHECK(x) do{if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);++failures;}}while(0)
int main(){
 g_replayTakeoverSnapshot.valid=g_replayStartSnapshot.valid=true;
 g_replayStartObservation.marker=g_replayGameStartV2.marker=42;
 g_replaySeekCache.count=256;g_replayBootstrapPhase=ReplayBootstrapPhase::Complete;
 g_replayBootstrapSelectorConfirms={4,4};g_replayBootstrapKnockoutLatches={1,1};
 g_replayBootstrapSelectorLifecycleValid=true;g_replayTerminalSerial=110;g_replayWinScreenTicks=61;
 g_netFrame=4000;g_netEpoch=g_netBootstrapTransientsEpoch=g_netFpuInitializedEpoch=7;
 g_netGameplaySegmentActive=g_netGameplaySegmentSeen=true;g_netInject[0]=3;g_netInject[1]=9;
 nativeState={{0x49EF00u,1u},{0x4AA454u,2u},{0x8718B8u,17u},{0x8716E8u,9u},{0x4AA450u,1u}};
 CHECK(replayBeginNextNativeGameV3()==1 && begins==0 && calls.empty());
 pending=true;accept=false;
 CHECK(replayBeginNextNativeGameV3()==1 && begins==1 && calls.empty());
 CHECK(g_replayStartSnapshot.valid && g_netFrame==4000);
 accept=true;
 CHECK(replayBeginNextNativeGameV3()==2);
 CHECK(calls==std::vector<int>({1,2,3,4,5,6}));
 CHECK(!pending && !g_replayStartSnapshot.valid && !g_replayTakeoverSnapshot.valid);
 CHECK(g_replayStartObservation.marker==0 && g_replayGameStartV2.marker==0 && g_replaySeekCache.count==0);
 CHECK(g_replayBootstrapPhase==ReplayBootstrapPhase::WaitingForStart);
 CHECK(!g_replayBootstrapSelectorLifecycleValid && g_replayBootstrapSelectorConfirms[0]==0 && g_replayBootstrapKnockoutLatches[1]==0);
 CHECK(g_replayTerminalSerial==0 && g_replayWinScreenTicks==0 && g_netFrame==0);
 CHECK(g_netEpoch==1 && g_netBootstrapTransientsEpoch==0 && g_netFpuInitializedEpoch==0);
 CHECK(!g_netGameplaySegmentActive && !g_netGameplaySegmentSeen && g_netInject[0]==0 && g_netInject[1]==0);
 CHECK(!hitboxCache && !rollbackReady && !canonicalAudioDirty && audioResets==1 && paletteResets==1);
 CHECK(nativeState[0x49EF00u]==1u && nativeState[0x4AA454u]==0u);
 CHECK(nativeState[0x8718B8u]==0u && nativeState[0x8716E8u]==0u && nativeState[0x4AA450u]==0u);
 CHECK(speed==8000 && ownedSerial==111 && publishedSerial==110 && window==123 && requestedHitboxes && requestedInputs);
 const auto count=calls.size();
 CHECK(replayBeginNextNativeGameV3()==1 && calls.size()==count && audioResets==1);
 std::printf("native replay rollover: %d failed checks\n",failures);return failures?1:0;
}
'@
Set-Content -LiteralPath (Join-Path $testDir 'rollover_test.cpp') -Value $harness
Push-Location $testDir
try {
    & $Compiler /nologo /std:c++17 /EHsc /MT rollover_test.cpp /Fe:rollover_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $LASTEXITCODE" }
    & (Join-Path $testDir 'rollover_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Native rollover regression failed: $LASTEXITCODE" }
} finally { Pop-Location }
