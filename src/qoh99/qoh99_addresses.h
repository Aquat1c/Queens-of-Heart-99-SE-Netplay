#pragma once
#include "qoh99/generated/AddressConstants.hpp"
#include "qoh99/generated/NativeSymbolCatalog.hpp"
#include <cstdint>

namespace qoh99 { namespace ga {

namespace addr = ::qoh99::generated_model::addr;
namespace size = ::qoh99::generated_model::size;

constexpr uintptr_t kImageBase = ::qoh99::generated_model::kImageBaseVa;

constexpr uintptr_t kScene          = addr::kSceneCurrent;      // 1 menu / 2 match / 3 vs+charsel / 4 replay
constexpr uintptr_t kSceneNext      = addr::kSceneNext;         // next scene (loop copies next->cur)
constexpr uintptr_t kScene3Sub      = addr::kScene3Substate;    // 0 setup /1 VS-list /2 charsel /3 confirm
constexpr uintptr_t kScene3Confirm  = addr::kScene3ConfirmFlag; // confirm; in charsel = GO BACK
constexpr uintptr_t kScene3Cursor   = addr::kScene3Cursor;      // VS-list row; not a per-player character cursor
constexpr uintptr_t kVsConfirmed    = addr::kVsListEntryFlag;
constexpr uintptr_t kMainMenuSub    = addr::kMainMenuSubstate;
constexpr uintptr_t kMainMenuCursor = addr::kMainMenuCursor;
constexpr uintptr_t kMatchSub       = addr::kMatchSubstate;     // -1 intro / 0 gameplay / 5 round-win
constexpr uintptr_t kTeamSize       = addr::kVsFormationRow;
constexpr uintptr_t kControlMask    = addr::kControlMask;
constexpr uintptr_t kRoundCounter   = addr::kRoundCounter;

constexpr uintptr_t kP1CharCursor   = addr::kP1CharCursor;      // dword_870954[280*player]
constexpr uintptr_t kP2CharCursor   = addr::kP2CharCursor;
constexpr uintptr_t kHuman0Confirm  = addr::kCharSelConfirmP1;  // P1 charsel confirm 1->2->4
constexpr uintptr_t kHuman1Confirm  = addr::kCharSelConfirmP2;  // P2 charsel confirm 1->2->4

constexpr uintptr_t kInputMaskTable = addr::kLogicalMaskTable;  // {0x10000,0x20000,0x40000,0x80000,1,2,4,8}
// &kInputMaskTable[8], the loop sentinel.
constexpr uintptr_t kInputMaskEnd   = kInputMaskTable + 0x20;
constexpr uintptr_t kInputBindBase  = addr::kInputBindingsPtr;
constexpr uintptr_t kJoyDeviceState = addr::kJoyDeviceState;
constexpr uintptr_t kKeyState       = addr::kKeyState;
constexpr uintptr_t kRemoteInput    = addr::kNetRemoteInput;    // unpacked remote logical input (buttons|dirs<<16)
constexpr uintptr_t kRemoteSysWord  = addr::kSessionInputWord;  // remote system bits (packed>>12 & 3)
constexpr uintptr_t kNetFrameSeq    = addr::kNetFrameSeq;       // frame seq (HIWORD of packed net word)

constexpr uintptr_t kNetInputRing   = addr::kNetInputRing;
constexpr uintptr_t kNetRingCount   = addr::kNetInputRingCount;
constexpr uintptr_t kNetSessionId   = addr::kNetSessionId;
constexpr uintptr_t kNetLocalSide   = addr::kNetLocalSide;      // local side (msg type 3)
constexpr uintptr_t kNetClientGot   = addr::kNetClientGotFlag;  // host<-client connected (we set on peer-lock)
constexpr uintptr_t kNetReadyMerge  = addr::kNetReadyFlag;      // ready/merge enable (msg type 7)
constexpr uintptr_t kNetIsNetwork   = addr::kNetIsNetworkSession; // 1=network session
constexpr uintptr_t kNetConfigDst   = addr::kNetConfigBlock;    // type-6 config landing (0x334 block)
constexpr uintptr_t kMatchConfigPtr = addr::kMatchConfigPtr;    // POINTER to match config (deref +40/+44/+48/+80)

constexpr uintptr_t kArenaBase      = addr::kArenaBlock0;       // base of 2x 0x1CED0 blocks
constexpr uintptr_t kArenaBlockSize = size::kArenaBlockSize;
constexpr uintptr_t kArenaSelector  = addr::kArenaSelector;     // ^=1 per frame; selects active block
constexpr uintptr_t kArenaActivePtr = addr::kArenaActivePtr;
constexpr uintptr_t kArenaMirrorPtr = addr::kArenaMirrorPtr;
constexpr uintptr_t kSwapLatch      = addr::kArenaSwapLatch;

constexpr uintptr_t kFighterSize    = size::kFighterSize;
constexpr uintptr_t kFighterPtrTbl  = addr::kFighterPtrTable;
constexpr uintptr_t kFighterBackTbl = addr::kFighterBackupTable;
constexpr uintptr_t kFighterP1      = addr::kFighterP1;
constexpr uintptr_t kFighterP2      = addr::kFighterP2;
constexpr uintptr_t kFighterP3      = addr::kFighterP3;
constexpr uintptr_t kFighterP4      = addr::kFighterP4;
constexpr uintptr_t kHelperP1       = addr::kHelperCloneP1;     // striker/assist clone of P1 (0x5FC)
constexpr uintptr_t kHelperP2       = addr::kHelperCloneP2;     // striker/assist clone of P2
constexpr uintptr_t kHelperP3       = addr::kHelperCloneP3;     // striker/assist clone of P3
constexpr uintptr_t kHelperP4       = addr::kHelperCloneP4;     // striker/assist clone of P4

constexpr uintptr_t kHealth         = addr::kHealthArray;       // [11*player] stride 44, 0..100
constexpr uintptr_t kScore          = addr::kScoreArray;
constexpr uintptr_t kRngSeedGame    = addr::kRngSeedGameplay;
constexpr uintptr_t kRngSeedCosm    = addr::kRngSeedCosmetic;
constexpr uintptr_t kRngSelector    = addr::kRngSelector;       // 0->gameplay seed, else cosmetic

static_assert(kImageBase       == 0x400000u, "kImageBase moved");
static_assert(kScene           == 0x6234ECu, "kScene moved");
static_assert(kSceneNext       == 0x61801Cu, "kSceneNext moved");
static_assert(kScene3Sub       == 0x4AA454u, "kScene3Sub moved");
static_assert(kScene3Confirm   == 0x4AA450u, "kScene3Confirm moved");
static_assert(kScene3Cursor    == 0x4AA458u, "kScene3Cursor moved");
static_assert(kVsConfirmed     == 0x49EF00u, "kVsConfirmed moved");
static_assert(kMainMenuSub     == 0x4AB8C4u, "kMainMenuSub moved");
static_assert(kMainMenuCursor  == 0x4AB8D0u, "kMainMenuCursor moved");
static_assert(kMatchSub        == 0x8719A4u, "kMatchSub moved");
static_assert(kTeamSize        == 0x62369Cu, "kTeamSize moved");
static_assert(kControlMask     == 0x6236CCu, "kControlMask moved");
static_assert(kRoundCounter    == 0x871950u, "kRoundCounter moved");
static_assert(kP1CharCursor    == 0x870954u, "kP1CharCursor moved");
static_assert(kP2CharCursor    == 0x870DB4u, "kP2CharCursor moved");
static_assert(kHuman0Confirm   == 0x8716D0u, "kHuman0Confirm moved");
static_assert(kHuman1Confirm   == 0x8716D4u, "kHuman1Confirm moved");
static_assert(kInputMaskTable  == 0x4A2A38u, "kInputMaskTable moved");
static_assert(kInputMaskEnd    == 0x4A2A58u, "kInputMaskEnd moved");
static_assert(kInputBindBase   == 0x626860u, "kInputBindBase moved");
static_assert(kJoyDeviceState  == 0x4ECB20u, "kJoyDeviceState moved");
static_assert(kKeyState        == 0x4AA920u, "kKeyState moved");
static_assert(kRemoteInput     == 0x66ECD4u, "kRemoteInput moved");
static_assert(kRemoteSysWord   == 0x66ECC4u, "kRemoteSysWord moved");
static_assert(kNetFrameSeq     == 0x66ECCCu, "kNetFrameSeq moved");
static_assert(kNetInputRing    == 0x66ECE0u, "kNetInputRing moved");
static_assert(kNetRingCount    == 0x66EE28u, "kNetRingCount moved");
static_assert(kNetSessionId    == 0x66EE20u, "kNetSessionId moved");
static_assert(kNetLocalSide    == 0x4AAA44u, "kNetLocalSide moved");
static_assert(kNetClientGot    == 0x4AAA48u, "kNetClientGot moved");
static_assert(kNetReadyMerge   == 0x4AAA4Cu, "kNetReadyMerge moved");
static_assert(kNetIsNetwork    == 0x4AAA50u, "kNetIsNetwork moved");
static_assert(kNetConfigDst    == 0x617498u, "kNetConfigDst moved");
static_assert(kMatchConfigPtr  == 0x613DE0u, "kMatchConfigPtr moved");
static_assert(kArenaBase       == 0x4ED140u, "kArenaBase moved");
static_assert(kArenaBlockSize  == 0x1CED0u,  "kArenaBlockSize moved");
static_assert(kArenaSelector   == 0x623700u, "kArenaSelector moved");
static_assert(kArenaActivePtr  == 0x6234E8u, "kArenaActivePtr moved");
static_assert(kArenaMirrorPtr  == 0x6177CCu, "kArenaMirrorPtr moved");
static_assert(kSwapLatch       == 0x6267D4u, "kSwapLatch moved");
static_assert(kFighterSize     == 0x5FCu,    "kFighterSize moved");
static_assert(kFighterPtrTbl   == 0x4A37B4u, "kFighterPtrTbl moved");
static_assert(kFighterBackTbl  == 0x623500u, "kFighterBackTbl moved");
static_assert(kFighterP1       == 0x623E00u, "kFighterP1 moved");
static_assert(kFighterP2       == 0x626880u, "kFighterP2 moved");
static_assert(kFighterP3       == 0x4ECB40u, "kFighterP3 moved");
static_assert(kFighterP4       == 0x617A20u, "kFighterP4 moved");
static_assert(kHelperP1        == 0x614060u, "kHelperP1 moved");
static_assert(kHelperP2        == 0x61465Cu, "kHelperP2 moved");
static_assert(kHelperP3        == 0x614C58u, "kHelperP3 moved");
static_assert(kHelperP4        == 0x615254u, "kHelperP4 moved");
static_assert(kHealth          == 0x6265C8u, "kHealth moved");
static_assert(kScore           == 0x626670u, "kScore moved");
static_assert(kRngSeedGame     == 0x4A9938u, "kRngSeedGame moved");
static_assert(kRngSeedCosm     == 0x4A993Cu, "kRngSeedCosm moved");
static_assert(kRngSelector     == 0x4ABB24u, "kRngSelector moved");

}}
