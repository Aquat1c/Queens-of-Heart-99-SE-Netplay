#pragma once

#include "IpcConfig.h"
#include "../net/Protocol.h"
#include "../net/RelayProtocol.h"
#include "../netplay/WireProtocol.h"
#include "../replay/ReplayIpcWire.h"
#include "../replay/ReplayTypes.h"
#include "../spectator/SpectatorProtocol.h"

#define QOH99_BUILD_CONTRACT_MARKER \
    "QOH99_NETPLAY_CONTRACT_IPC25_SIZE14965_RUNTIME_9F3A7C52D6E4B801_CTRL21_COMPATIDENTITY1_RUNTIMEARTIFACTS1_UDP_QNP14_QSP4_SPECMETA5_LIVEPRESENCE1_FXCARVE1_CONFIGSAVE1_BGMWAVE0_FRONTDELAY1_RUNTIMEDELAY1_NAMES1_TRANSITIONEXACT2_BOOTGATE1_HIDDENREPLAY2_NATIVEFPSISO1_PACINGPROOF1_PACINGSLEW1_SEMHOLD2_PRESENTCLOCK2_PACEOWNER1_PRESENTPACE1_POSTPUBLISH3_CANONPUBLISH1_PUBLISHFAIL1_CHECKPOINTLIGHT1_NATIVEHUDHIDE1_SIMFRONTIER1_HIDDENFWD1_ROUTEPROBE1_INPUT2_MODECONTROLS1_TRAINING1_WINECOMPAT1_IATCLOCK1_HOOKTX1_SPECGRANT1_SPECSIDECAR1_SPECCONVERGE1_SPECHANDOFF2_SPECTROUTES2_SPECTCTRL13_SPECPUB2_SPECRELAYWIRE3_SPECHARD1_SPECRETENTION1_SPECTOPOLOGYCLIENT1_SPECCLIENTPUB1_REPLAYQ99R2_REPLAYIPC3_LAYOUTPARTS1_FCMDPARSER1_DRIVECARVE1_AUDIOMANIFEST2_CONTENTROOTCHR1_AUDIONOTDETERMINISM2_STATEPROOF2_PTRALIAS1_REPLAYVIEWER2"

namespace qipc {

inline constexpr char kBuildContractMarker[] =
    QOH99_BUILD_CONTRACT_MARKER;

static_assert(kIpcVersion == 25, "update the build-contract marker");
static_assert(sizeof(IpcConfig) == 14965, "update the build-contract marker");
static_assert(
    kReplayPlaybackIpcSchemaVersion == qreplay::ipc::v3::kWireVersion
        && kReplayPlaybackRequiredFeatureBits
            == qreplay::ipc::v3::kPlaybackRequiredFeaturesV3,
    "update the replay playback IPC/build contract");
static_assert(kRuntimeCompatibilityId == 0x9F3A7C52D6E4B801ull,
              "update the build-contract marker");
static_assert(qnet::kProtocolVersion == 21,
              "update the build-contract marker");
static_assert(kSpectatorTopologyRouteDirect == qnet::SPECTATE_ROUTE_DIRECT
                  && kSpectatorTopologyRoutePunched
                      == qnet::SPECTATE_ROUTE_PUNCHED
                  && kSpectatorTopologyRouteRelay
                      == qnet::SPECTATE_ROUTE_RELAY,
              "update the spectator topology IPC route mapping");
static_assert(qnet::relay::kSpectatorVersion == 3,
              "update the build-contract marker");
static_assert(qnetplay::wire::kVersion == 14,
              "update the build-contract marker");
static_assert(qnetplay::spectator::kSpectatorVersion == 4,
              "update the build-contract marker");
static_assert(qnetplay::spectator::kReadyMetadataVersion == 5,
              "update the build-contract marker");
static_assert(qreplay::kContainerVersionMajor == 1
                  && qreplay::kContainerVersionMinor == 0,
              "update the build-contract marker");
static_assert(qreplay::kContainerVersionMajorV2 == 2
                  && qreplay::kContainerVersionMinorV2 == 0,
              "update the build-contract marker");

}
