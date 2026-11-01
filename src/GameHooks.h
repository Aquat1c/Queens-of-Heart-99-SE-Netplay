#pragma once
#include "input/InputLaunchConfig.h"
#include "netplay/ConfigurationProvenance.h"
#include "replay/ReplayTypes.h"

namespace qoh99 {

enum class RunMode {
    Off,
    Observe,
    SelfTest,
    Netplay,
    Spectator,
    Replay,
};

enum class PresentationDisposition : unsigned long {
    Normal = 0,
    SuppressHistorical = 1,
    CorrectedFinal = 2,
    HiddenNewForward = 3,
    VisibleFastForward = 4,
};

PresentationDisposition currentPresentationDisposition();
PresentationDisposition queryPresentationDisposition();
unsigned long presentationDispositionQueryCount();
bool ownsNetplayPacing();

bool ownsNetworkPacing();

bool tryAcquirePresentationSlot();

void observePresentationCommit();

void observePresentationFailure(unsigned long reason);

void reportWrapperPresentationCounters(
    unsigned long normalVisible,
    unsigned long suppressed,
    unsigned long correctedFinal);

bool installHooks(RunMode mode);

void armNetplay(int localPlayer, int delay, int rollback,
                int remoteDelay, int remoteRollback,
                const qnetplay::config_provenance::ConfigurationProvenanceWire&
                    configurationProvenance,
                bool serveSpectators,
                const char* localName, unsigned long localNameCodePage,
                const char* remoteName, unsigned long remoteNameCodePage);

void armSpectator();
void setSpectatorWantsLatest(bool enabled);

void armReplay(unsigned long long manifestDigest,
               unsigned long speedPermille,
               unsigned long matchOrdinal);

void armRecorder(bool enabled, unsigned long channelGeneration);

void setSpectatorServiceMode(unsigned char serviceMode);

void spectatorSubmitPlaybackCommand(unsigned char command,
                                    unsigned long long argument);

struct SpectatorPlaybackReadout {
    bool active = false;
    bool paused = false;
    bool fastForward = false;
    bool catchingUp = false;
    bool sourceContactFresh = false;
    bool hasTerminal = false;
    bool complete = false;
    unsigned long speedPermille = 1000;
    unsigned long playbackBacklogRecords = 0;
    unsigned long receiveBacklogRecords = 0;
    unsigned long liveTargetRecords = 0;
    unsigned long long estimatedSlotsToLive = 0;
    double estimatedSecondsToLive = 0.0;
    double backlogSeconds = 0.0;
    unsigned long long underruns = 0;
    unsigned long long truncatedSlices = 0;
    unsigned long long sourceContactAgeMs = 0;
    unsigned terminalReason = 0;
    unsigned long currentMatchId = 0;
    unsigned long sourceMatchId = 0;
    bool behindLiveKnown = false;
    unsigned long long behindLiveRecords = 0;
    unsigned long cushionBonusRecords = 0;
    unsigned long catchupSpeedPermille = 1000;
};
void spectatorPlaybackStatus(SpectatorPlaybackReadout& out);

void armTraining(bool enabled);

void setUnattendedHarnessLaunch(bool enabled);

void enableOfflineVsScoreboard();

void harnessFrontendPressTick();

// Hooks cannot be removed at runtime: the DLL stays pinned so hooks and transport workers are never unmapped while executing.
void removeHooks();

void setNativeHudSuppression(bool hideScore, bool hideTimer);
void setModularNativeHudEligible(bool eligible);
bool setModularNativeHudTrainingMasks(uint32_t globalMask, uint32_t p1Mask,
                                      uint32_t p2Mask);
void getModularNativeHudRequested(uint32_t* owner, uint32_t* globalMask,
                                  uint32_t* p1Mask, uint32_t* p2Mask);
void getModularNativeHudStatus(uint32_t* owner, uint32_t* globalMask,
                               uint32_t* p1Mask, uint32_t* p2Mask,
                               uint32_t* unavailableGlobal,
                               uint32_t* unavailableP1,
                               uint32_t* unavailableP2);

void setOfflineExtraInputConfig(uint32_t slot,
                                const qinput::InputLaunchConfig& config);
void setAudioVolumes(uint8_t musicVolume, uint8_t soundVolume);
void setInputLaunchConfig(const qinput::InputLaunchConfig& config,
                           bool spectator);
void setReplayTakeoverInputConfig(
    const qinput::InputLaunchConfig& config);
void setModeControls(const qinput::CompiledModeControlsV1& controls);

void setSessionAuthority(bool isHost);

void setNativeDeveloperKeyLaunchPolicy(bool allowInteractiveOfflineKeys);

void setNetplayExtraFunctionPolicy(bool disableExtraFunctions);

void setSessionDir(const char* dir);

}
