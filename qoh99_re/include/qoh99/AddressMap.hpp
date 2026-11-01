#pragma once
#include "Confidence.hpp"
#include "Types.hpp"

namespace qoh99 {

enum class MemClass {
    ReadOnlyTable,
    MutableGameplay,
    RenderCosmetic,
    UnsafeOSPointer,
    WallClockPacing,
    ControlFlow
};

namespace addr {

constexpr GameAddr kSceneCurrent      = 0x6234EC; // 1 menu, 2 match, 3 VS and charselect, 4 opening movie.
constexpr GameAddr kSceneNext         = 0x61801C; // The loop copies next to cur at the frame boundary.
constexpr GameAddr kScene3Substate    = 0x4AA454; // 0 setup, 1 VS list, 2 charselect, 3 confirm.
constexpr GameAddr kScene3ConfirmFlag = 0x4AA450; // This is the VS-list confirm, not 0x49EF00.
constexpr GameAddr kScene3Cursor      = 0x4AA458; // This is the VS-list row, not a per-player character cursor.
constexpr GameAddr kVsListEntryFlag   = 0x49EF00; // This is a transition/setup flag, not the confirm.
constexpr GameAddr kMainMenuSubstate  = 0x4AB8C4; // Substate of sub_48CA20; 4 launches the match.
constexpr GameAddr kMainMenuCursor    = 0x4AB8D0;
constexpr GameAddr kMenuModeState     = 0x4AB784;
constexpr GameAddr kMatchSubstate     = 0x8719A4; // -1 intro, 0 gameplay, 1..4 unknown, 5 win.
// dword_62369C is the applied VS formation: 0 none (Arcade/Story), 1 two-fighter
// rows 0/1, 2 four-fighter 2v2 rows 2..5 (written by sub_432EE0).
constexpr GameAddr kVsFormationRow    = 0x62369C;
constexpr GameAddr kMatchMode         = kVsFormationRow;
constexpr GameAddr kTeamSize          = kVsFormationRow;
constexpr GameAddr kControlMask       = 0x6236CC; // Bit i set means slot i is native CPU (fighter+1400); per VS row: 0x2/0x0/0xE/0xA/0xC/0x0.
constexpr GameAddr kRoundCounter      = 0x871950;
constexpr GameAddr kRoundWinTable     = 0x625628;
constexpr GameAddr kRoundWinTable0    = 0x625620;
constexpr GameAddr kSceneInitBranch   = 0x6236FC;

constexpr GameAddr kP1CharCursor      = 0x870954;
constexpr GameAddr kP2CharCursor      = 0x870DB4;
constexpr GameAddr kCharSelConfirmP1  = 0x8716D0; // Human confirm state advances 1, 2, 4.
constexpr GameAddr kCharSelConfirmP2  = 0x8716D4;
constexpr GameAddr kCharTypeTable     = 0x49EEE4;

constexpr GameAddr kMatchConfigPtr    = 0x613DE0;
constexpr GameAddr kNetConfigBlock    = 0x617498;
constexpr GameAddr kNetConfigBlock20  = 0x616750;
constexpr GameAddr kMatchConfigSnapshot = 0x623520;
constexpr GameAddr kVsModeFlagA       = 0x613F14; // 129 vs COM, 1 vs 2P.
constexpr GameAddr kVsHumanFlag       = 0x4A37C8; // Per-fighter input source-map entry (dword_4A37C4 family), not a vs-human flag.

constexpr GameAddr kFighterPtrTable   = 0x4A37B4;
constexpr GameAddr kFighterBackupTable= 0x623500;
constexpr GameAddr kFighterP1         = 0x623E00;
constexpr GameAddr kFighterP2         = 0x626880;
constexpr GameAddr kFighterP3         = 0x4ECB40;
constexpr GameAddr kFighterP4         = 0x617A20;
constexpr GameAddr kHelperCloneP1     = 0x614060;
constexpr GameAddr kHelperCloneP2     = 0x61465C;
constexpr GameAddr kHelperCloneP3     = 0x614C58;
constexpr GameAddr kHelperCloneP4     = 0x615254;
constexpr GameAddr kEffectPool        = 0x618160;
constexpr GameAddr kActorClonePool    = 0x526FA0;
constexpr GameAddr kShadowPool        = 0x6275C0;
constexpr GameAddr kFighterCopyScratch= 0x6287B4;

constexpr GameAddr kArenaBlock0       = 0x4ED140;
constexpr GameAddr kArenaBlock1       = 0x50A010;
constexpr GameAddr kArenaActivePtr    = 0x6234E8; // Active pointer: use this, and never derive it from the selector.
constexpr GameAddr kArenaSelector     = 0x623700; // Toggled with ^=1 per frame, one flip out of phase with the active pointer.
constexpr GameAddr kArenaMirrorPtr    = 0x6177CC;
constexpr GameAddr kArenaHeadPtr      = 0x623510;
constexpr GameAddr kArenaSwapLatch    = 0x6267D4;

// Effective life = current(+0) + reserve(+8) - pending damage(+4), over a
// 16-byte/player block at dword_6254E0 (sub_42D740).
constexpr GameAddr kBattleLifeArray   = 0x6254E0;
constexpr GameAddr kHealthArray       = 0x6265C8;
constexpr GameAddr kGuardGaugeArray   = kHealthArray;
// EX owner records: the active owner is fighter byte +171 (P1/P3=0, P2/P4=1), not +172.
constexpr GameAddr kExGaugeRecords    = 0x6267E0;
constexpr GameAddr kExGaugeStocks     = kExGaugeRecords + 0x04;
constexpr GameAddr kExGaugeSpendTimer = kExGaugeRecords + 0x08;
constexpr GameAddr kExGaugePendingCosts = kExGaugeRecords + 0x0C;
constexpr GameAddr kExGaugeFraction   = kExGaugeRecords + 0x1C;
constexpr GameAddr kScoreArray        = 0x626670;

constexpr GameAddr kRngSeedGameplay   = 0x4A9938;
constexpr GameAddr kRngSeedCosmetic   = 0x4A993C;
constexpr GameAddr kRngSelector       = 0x4ABB24;

constexpr GameAddr kLogicalMaskTable  = 0x4A2A38; // {0x10000,0x20000,0x40000,0x80000,1,2,4,8}
constexpr GameAddr kKeyRemapTable     = 0x4A2130;
constexpr GameAddr kKeyState          = 0x4AA920;
constexpr GameAddr kDiActiveFlag      = 0x4AA6F8;
constexpr GameAddr kInputBindingsPtr  = 0x626860; // Points to a 96-byte-per-player binding table and is null early in boot.
constexpr GameAddr kJoyDeviceState    = 0x4ECB20;
constexpr GameAddr kInputSourceTable  = 0x4A37C4; // Slot-to-source route: source = dword_4A37C4[fighter+172]; identity for rows 0,1,2,4,5; VS row 3 is [0,2,1,3].

constexpr GameAddr kNetDispatchPtr    = 0x66ECC0;
constexpr GameAddr kSessionInputWord  = 0x66ECC4;
constexpr GameAddr kNetFrameSeq       = 0x66ECCC;
constexpr GameAddr kNetRemoteInput    = 0x66ECD4;
constexpr GameAddr kNetParam5         = 0x66ECD8;
constexpr GameAddr kNetInputRing      = 0x66ECE0;
constexpr GameAddr kNetInputRingCount = 0x66EE28;
constexpr GameAddr kNetSyncPosXRing   = 0x66EE10;
constexpr GameAddr kNetSyncPosYRing   = 0x66EE18;
constexpr GameAddr kNetSyncPosP1      = kNetSyncPosXRing;
constexpr GameAddr kNetSyncPosP2      = kNetSyncPosYRing;
constexpr GameAddr kNetSessionId      = 0x66EE20;
constexpr GameAddr kNetInMatchFlag    = 0x66EE38;
// dword_66EE00: session-created / is-host. A transport that skips the host/join dialogs must write it or the host never leaves scene 0 (sub_45F0E0).
constexpr GameAddr kNetIsHost         = 0x66EE00;
constexpr GameAddr kNetOnMatchStartPtr= 0x66EE08;
constexpr GameAddr kNetCbPtr66EE04    = 0x66EE04;
constexpr GameAddr kNetCbPtr66EBA4    = 0x66EBA4;
constexpr GameAddr kNetCbPtr66ECC8    = 0x66ECC8;
constexpr GameAddr kNetLocalSide      = 0x4AAA44;
constexpr GameAddr kNetClientGotFlag  = 0x4AAA48;
constexpr GameAddr kNetReadyFlag      = 0x4AAA4C;
constexpr GameAddr kNetIsNetworkSession = 0x4AAA50;
constexpr GameAddr kChatCallbackPtr   = 0x8A5C68;

constexpr GameAddr kCameraZoomA       = 0x49B530;
constexpr GameAddr kCameraZoomB       = 0x49B538;
constexpr GameAddr kHudHitDescriptorA = 0x8807C0;
constexpr GameAddr kHudHitDescriptorB = 0x8808CC;
constexpr GameAddr kHudDisplayA       = 0x87D8FC;
constexpr GameAddr kHudDisplayB       = 0x87D890;
constexpr GameAddr kHudTimerDigits    = 0x8A5CA0;
constexpr GameAddr kHudComboDigits    = 0x8A5CA8;
constexpr GameAddr kHudTextBuffer     = 0x623940;
constexpr GameAddr kFrameCounterCosmetic = 0x4AB778;

constexpr GameAddr kFrameTimeRing     = 0x4ABA2C;

constexpr GameAddr kDdraw2Ptr         = 0x66FACC;
constexpr GameAddr kAppActiveFlag     = 0x6248B0;
constexpr GameAddr kSpriteSurfaceTable= 0x68BC00;
constexpr GameAddr kSpriteMetaTable   = 0x68C020;

constexpr GameAddr kOpeningMovieFilename = 0x4AB794;
constexpr GameAddr kForce16BitFlag    = 0x4AB898;
constexpr GameAddr kConfigFileImage   = 0x615860;
constexpr GameAddr kCommandTable      = 0x4A857C;
constexpr GameAddr kDsoundBufferA     = 0x4AAAE4;
constexpr GameAddr kDsoundBufferB     = 0x4AB118;
constexpr GameAddr kMciLastError      = 0x4EC988;

constexpr std::uint32_t kFighterSize     = 0x5FC;
constexpr std::uint32_t kArenaBlockSize  = 0x1CED0;
constexpr std::uint32_t kEntityStride    = 104;
constexpr std::uint32_t kBattleLifeStride= 16;
constexpr std::uint32_t kHealthStride    = 44;
constexpr std::uint32_t kGuardGaugeStride= kHealthStride;
constexpr std::uint32_t kExGaugeRecordStride = 0x20;
constexpr std::uint32_t kExGaugeStorageRecordCount = 4;
constexpr std::uint32_t kExGaugeActiveOwnerCount = 2;
constexpr std::uint32_t kExGaugePendingCostLanes = 4;
constexpr std::uint32_t kExGaugeStockMax = 9;
constexpr std::uint32_t kExGaugeFractionMax = 53;
constexpr std::uint32_t kCmdChildSize    = 0x200;
constexpr std::uint32_t kConfigWireSize  = 4232;
constexpr std::uint32_t kConfigBlockSize = 0x334;

}

// Evidence VAs for the December build. Runtime code resolves through a build profile; never infer a mapping from a common offset.
namespace fnaddr {
constexpr GameAddr kSceneTitleMenu     = 0x48CA20;
constexpr GameAddr kSceneVsSetup       = 0x43A790;
constexpr GameAddr kSceneMatchDispatch = 0x428430;
constexpr GameAddr kGameplayTick       = 0x421EE0;
constexpr GameAddr kRoundWinTick       = 0x408180;
constexpr GameAddr kApplyVsModeRow     = 0x432EE0; // sub_432EE0 __fastcall(row, init)
constexpr GameAddr kFighterRecordInput = 0x47BF90;
constexpr GameAddr kCommandMatch       = 0x47B910;
constexpr GameAddr kManualRecoveryEvaluator = 0x47CB00;
constexpr GameAddr kThrowContactTest   = 0x47D600;
constexpr GameAddr kThrowEscapeEvaluator = 0x483470;
constexpr GameAddr kThrowTechEvaluator = 0x47D9D0;
constexpr GameAddr kReadLogicalInput   = 0x484BB0;
constexpr GameAddr kApplyLogicalInput  = 0x484C60;
constexpr GameAddr kPollAllInputs      = 0x484AB0;
constexpr GameAddr kNativeKeyboardCallback = 0x484B20;
constexpr GameAddr kNativePresenceProbe = 0x484A40;
constexpr GameAddr kConfigSave          = 0x4853B0;
constexpr GameAddr kInputInjectKey     = 0x45D280;
constexpr GameAddr kNetDispatchMsg     = 0x45D6B0; // sub_45D6B0 __thiscall(msg, playerId, len)
constexpr GameAddr kNetSendMsg         = 0x45D970; // sub_45D970(channel, type, value)
constexpr GameAddr kNetSetupTransport  = 0x45DBA0;
constexpr GameAddr kNetConnectDialogs  = 0x45F060;
constexpr GameAddr kRngDraw            = 0x4914D0;
constexpr GameAddr kRngSrand           = 0x4914C0;
constexpr GameAddr kRngGetGameplaySeed = 0x491510;
constexpr GameAddr kMessagePump        = 0x491760; // sub_491760: do not hook it, because hooking crashes with EIP=0.
constexpr GameAddr kFramePacing        = 0x491030;
constexpr GameAddr kEffectPoolSpawn    = 0x40B820;
constexpr GameAddr kClonePoolSpawn     = 0x42DDB0;
constexpr GameAddr kEmitHudDescriptors = 0x415E10;
constexpr GameAddr kWndProc            = 0x470540;
constexpr GameAddr kNativeDamage        = 0x42D760;
constexpr GameAddr kGuardContactResolver = 0x41D950;
constexpr GameAddr kHitContactResolver  = 0x466770;
constexpr GameAddr kNaturalActionTerminal = 0x412290;
constexpr GameAddr kNativeReactionGate  = 0x468660;
constexpr GameAddr kHudScoreDraw        = 0x42A990;
constexpr GameAddr kHudTimeDraw         = 0x42C040;
constexpr GameAddr kAttackContactResolver = 0x464FA0;
constexpr GameAddr kDamageCriticalScaler = 0x462D60;
constexpr GameAddr kGuardActionRequest  = 0x47C790;
constexpr GameAddr kFaultlessButtonDetector = 0x47C980;
constexpr GameAddr kReduceMashDetector = 0x47D530;
}

struct AddressEntry {
    const char* name;
    const char* origName;
    GameAddr    va;
    std::uint32_t size;
    MemClass    cls;
    Confidence  conf;
    const char* evidence;
};

struct LegacyNameAlias {
    const char* legacyName;
    const char* currentName;
    const char* reason;
};

class AddressMap {
public:
    static const AddressEntry* entries(std::size_t& count);
    static const AddressEntry* find(const char* semanticName);
    static const AddressEntry* findExact(const char* semanticName);
    static const AddressEntry* findByVa(GameAddr va);
    static const LegacyNameAlias* legacyNames(std::size_t& count);
};

}
