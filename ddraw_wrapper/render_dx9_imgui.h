#pragma once

#include <stddef.h>
#include <windows.h>

#include "qoh99_presentation_policy.h"
#include "surface_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (__stdcall *EfzDx9RequestPresentationModeFn)(
    QohPresentationMode mode, void *userData);

typedef enum EfzDx9PresentationKind {
    EFZ_DX9_PRESENT_CANONICAL = 1,
    EFZ_DX9_PRESENT_HELD_REPAINT = 2,
    EFZ_DX9_PRESENT_OTHER_REPAINT = 3
} EfzDx9PresentationKind;

typedef struct EfzDx9PresentParams {
    HWND window;
    const BYTE *pixels;
    int width;
    int height;
    int pitch;
    const RGBQUAD *palette;
    int clientWidth;
    int clientHeight;
    int dstX;
    int dstY;
    int dstW;
    int dstH;
    QohPresentationMode requestedMode;
    QohPresentationMode activeMode;
    BOOL appActive;
    BOOL *linearScale;
    DWORD presentSerial;
    DWORD focusSerial;
    EfzDx9PresentationKind presentationKind;
    EfzDx9RequestPresentationModeFn requestPresentationMode;
    void *requestPresentationModeUserData;
    int bytesPerPixel;        /* 1 = indexed, 2 = packed 16-bit */
    DdwSurfaceFormat format;  /* exact source format; never infer from bpp */
} EfzDx9PresentParams;

typedef struct EfzDx9PresentationStatus {
    DWORD structSize;
    QohPresentationMode requestedMode;
    QohPresentationMode effectiveMode;
    QohRendererBackend requestedBackend;
    QohRendererBackend effectiveBackend;
    QohRendererReadiness readiness;
    UINT adapterOrdinal;
    HMONITOR monitor;
    UINT backBufferWidth;
    UINT backBufferHeight;
    UINT refreshHz;
    DWORD resetGeneration;
    HRESULT lastError;
    char adapterDescription[128];
    char fallbackReason[160];
} EfzDx9PresentationStatus;

#define WM_QOH99_D3D_DEVICE_WORK (WM_APP + 0x54)
#define WM_QOH99_D3D_SHUTDOWN (WM_APP + 0x55)

#define EFZ_DX9_NETPLAY_HUD_MAX_LINES 4
#define EFZ_DX9_NETPLAY_HUD_TEXT_CAP 96

typedef struct EfzDx9NetplayHudState {
    DWORD structSize;
    BOOL  visible;
    // Every text field in this presentation ABI is UTF-8.
    char  playerOneName[EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    char  playerTwoName[EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    char  matchHeader[EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    char  sessionDetail[EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    int   practiceLineCount;
    char  practiceLines[EFZ_DX9_NETPLAY_HUD_MAX_LINES][EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    int   debugLineCount;
    char  debugLines[EFZ_DX9_NETPLAY_HUD_MAX_LINES][EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    int   toastLineCount;
    char  toastLines[EFZ_DX9_NETPLAY_HUD_MAX_LINES][EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    BOOL  rematchPromptActive;
    BOOL  rematchP1No;      /* current option: FALSE = hai/YES, TRUE = iie/NO */
    BOOL  rematchP1Ready;
    BOOL  rematchP2No;
    BOOL  rematchP2Ready;
} EfzDx9NetplayHudState;

typedef BOOL (__stdcall *EfzDx9NetplayHudProvider)(
    EfzDx9NetplayHudState *outState,
    void *userData);

#define EFZ_DX9_OVERLAY_MAX_COMMANDS 192
#define EFZ_DX9_OVERLAY_TEXT_CAP 128

enum EfzDx9OverlayCommandType {
    EFZ_DX9_OVERLAY_COMMAND_TEXT = 1,
    EFZ_DX9_OVERLAY_COMMAND_RECT_FILLED = 2,
    EFZ_DX9_OVERLAY_COMMAND_RECT = 3
};

enum EfzDx9OverlayTextFlags {
    EFZ_DX9_OVERLAY_TEXT_SHADOW = 0x1,
    EFZ_DX9_OVERLAY_TEXT_ALIGN_CENTER = 0x2,
    EFZ_DX9_OVERLAY_TEXT_ALIGN_RIGHT = 0x4
};

typedef struct EfzDx9OverlayCommand {
    DWORD structSize;
    int type;
    float x;
    float y;
    float w;
    float h;
    float fontSize;
    float thickness;
    DWORD color;
    DWORD flags;
    char text[EFZ_DX9_OVERLAY_TEXT_CAP];
} EfzDx9OverlayCommand;

typedef struct EfzDx9OverlayFrame {
    DWORD structSize;
    BOOL visible;
    float virtualWidth;
    float virtualHeight;
    int commandCount;
    EfzDx9OverlayCommand commands[EFZ_DX9_OVERLAY_MAX_COMMANDS];
} EfzDx9OverlayFrame;

typedef BOOL (__stdcall *EfzDx9OverlayProvider)(
    EfzDx9OverlayFrame *outFrame,
    void *userData);

#define QOH99_TRAINING_UI_API_VERSION 1u
/* Offsets below this are permanent.  See the negotiation rule above. */
#define QOH99_TRAINING_STATUS_V1_BASE_SIZE 568u
#define QOH99_TRAINING_STATUS_V1_COMBAT_TIER 696u
#define QOH99_TRAINING_STATUS_V1_FIGHTER_TIER 812u
#define QOH99_TRAINING_STATUS_V1_RECOVERY_TIER 1072u
#define QOH99_TRAINING_STATUS_V1_WALL_TIER 1080u
#define QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER 1096u
#define QOH99_TRAINING_STATUS_V1_METER_CHARGE_TIER 1104u
#define QOH99_TRAINING_TRIGGER_MAX_DELAY_STEPS 240u
#define QOH99_TRAINING_TRIGGER_MAX_CHANCE_PERCENT 100u
#define QOH99_TRAINING_TRIGGER_MAX_MOVE_INDEX 64u
#define QOH99_TRAINING_TRIGGER_MOVE_VALUE_FLAG 0x80000000u
#define QOH99_TRAINING_TRIGGER_RANDOM_RECORDING 9u
#define QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER 1300u
#define QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER \
    (QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER \
     + ((2u + QOH99_TRAINING_TRIGGER_EVENT_COUNT) * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER \
    (QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER \
     + (16u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER \
    (QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER \
     + (4u * sizeof(DWORD)))
#define QOH99_TRAINING_ACTOR_REPORT_CAPACITY 16u
#define QOH99_TRAINING_ACTOR_OWNER_UNKNOWN 0xFFFFFFFFu
#define QOH99_TRAINING_STATUS_V1_ACTOR_POOL_TIER \
    (QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER \
     + ((3u + 3u * QOH99_TRAINING_ACTOR_REPORT_CAPACITY) * sizeof(DWORD)))
#define QOH99_TRAINING_HUD_GLOBAL_BOTTOM_ELAPSED_TIME 0x00000002u
#define QOH99_TRAINING_HUD_PLAYER_SCORE          0x00000003u
#define QOH99_TRAINING_HUD_PLAYER_PORTRAIT_LIFE  0x0000003Cu
#define QOH99_TRAINING_HUD_PLAYER_GUARD          0x000000C0u
#define QOH99_TRAINING_HUD_PLAYER_NAME           0x00000800u
#define QOH99_TRAINING_HUD_PLAYER_ROUND_WINS     0x00001000u
#define QOH99_TRAINING_HUD_PLAYER_EX             0x00000700u
#define QOH99_TRAINING_HUD_PLAYER_COMBO          0x0001E000u
#define QOH99_TRAINING_HUD_GLOBAL_TOP_ROUND_TIMER 0x00000001u
#define QOH99_TRAINING_HUD_GLOBAL_FPS            0x00002000u
#define QOH99_TRAINING_STATUS_V1_NATIVE_HUD_TIER \
    (QOH99_TRAINING_STATUS_V1_ACTOR_POOL_TIER + (7u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER \
    (QOH99_TRAINING_STATUS_V1_NATIVE_HUD_TIER + (3u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_FRAME_TRACE_TIER \
    (QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER \
     + (4u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_FRAME_ADVANTAGE_GAP_TIER \
    (QOH99_TRAINING_STATUS_V1_FRAME_TRACE_TIER + (2u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_CONTACT_POLICY_TIER \
    (QOH99_TRAINING_STATUS_V1_FRAME_ADVANTAGE_GAP_TIER + (30u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_EX_RESTORE_TARGET_TIER \
    (QOH99_TRAINING_STATUS_V1_CONTACT_POLICY_TIER + (3u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_MACRO_AFTER_RESET_TIER \
    (QOH99_TRAINING_STATUS_V1_EX_RESTORE_TARGET_TIER + (1u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_REDUCE_POLICY_TIER \
    (QOH99_TRAINING_STATUS_V1_MACRO_AFTER_RESET_TIER + (7u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_GUARD_TIMING_TIER \
    (QOH99_TRAINING_STATUS_V1_REDUCE_POLICY_TIER + (3u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_PLAYER_SWAP_TIER \
    (QOH99_TRAINING_STATUS_V1_GUARD_TIMING_TIER + (1u * sizeof(DWORD)))
#define QOH99_TRAINING_STATUS_V1_STATUS_MESSAGE_TIER \
    (QOH99_TRAINING_STATUS_V1_PLAYER_SWAP_TIER + (5u * sizeof(DWORD)))
#define QOH99_TRAINING_SLOT_COUNT 8u
#define QOH99_TRAINING_MACRO_FRAME_CAPACITY 3600u
#define QOH99_TRAINING_COMMAND_QUEUE_CAPACITY 64u

enum Qoh99TrainingPositionPresetV1 {
    QOH99_TRAINING_POSITION_FROM_MODIFIER = 0,
    QOH99_TRAINING_POSITION_ROUND_START = 1,
    QOH99_TRAINING_POSITION_LEFT_CORNER = 2,
    QOH99_TRAINING_POSITION_CENTER = 3,
    QOH99_TRAINING_POSITION_RIGHT_CORNER = 4,
    QOH99_TRAINING_POSITION_VERY_CLOSE = 5
};

enum Qoh99TrainingGuardModeV1 {
    QOH99_TRAINING_GUARD_OFF = 0,
    QOH99_TRAINING_GUARD_AUTO = 1,
    QOH99_TRAINING_GUARD_ALWAYS_STAND = 2,
    QOH99_TRAINING_GUARD_ALWAYS_CROUCH = 3,
    QOH99_TRAINING_GUARD_AUTO_HEURISTIC = 4,
    QOH99_TRAINING_GUARD_ALWAYS_AIR = 5,
    QOH99_TRAINING_GUARD_AUTO_RECORD = 6
};

/* Low three bits of fighter +156: 1 stand, 2 crouch, 4 air; every other value
   requests no guard. Mirrors qtraining::PostureClass. */
enum Qoh99TrainingPostureClassV1 {
    QOH99_TRAINING_POSTURE_NOT_GUARDABLE = 0,
    QOH99_TRAINING_POSTURE_STANDING = 1,
    QOH99_TRAINING_POSTURE_CROUCHING = 2,
    QOH99_TRAINING_POSTURE_AIR = 3,
    QOH99_TRAINING_POSTURE_UNREADABLE = 4
};

enum Qoh99TrainingDummyStanceV1 {
    QOH99_TRAINING_STANCE_STAND = 0,
    QOH99_TRAINING_STANCE_CROUCH = 1,
    QOH99_TRAINING_STANCE_JUMP = 2
};

/* Auto-tech. Values are frozen; HOLD_GUARD is retired and rejected at ingress.
   Mirrors qtraining::DummyTech. */
enum Qoh99TrainingDummyTechV1 {
    QOH99_TRAINING_TECH_OFF = 0,
    QOH99_TRAINING_TECH_HOLD_GUARD = 1
};

enum Qoh99TrainingResourceTriggerV1 {
    QOH99_TRAINING_TRIGGER_OFF = 0,
    QOH99_TRAINING_TRIGGER_AFTER_COMBO = 1,
    QOH99_TRAINING_TRIGGER_AFTER_RECOVERY = 2,
    QOH99_TRAINING_TRIGGER_WHEN_NEUTRAL = 3,
    QOH99_TRAINING_TRIGGER_ON_RESET_ONLY = 4
};

enum Qoh99TrainingTriggerEventV1 {
    QOH99_TRAINING_TRIGGER_EVENT_ON_HIT = 0,
    QOH99_TRAINING_TRIGGER_EVENT_AFTER_BLOCK = 1,
    QOH99_TRAINING_TRIGGER_EVENT_AFTER_HITSTUN = 2,
    QOH99_TRAINING_TRIGGER_EVENT_ON_WAKEUP = 3,
    QOH99_TRAINING_TRIGGER_EVENT_AFTER_AIR_RECOVERY = 4,
    QOH99_TRAINING_TRIGGER_EVENT_AFTER_GROUND_RECOVERY = 5,
    QOH99_TRAINING_TRIGGER_EVENT_AFTER_THROW_TECH = 6,
    QOH99_TRAINING_TRIGGER_EVENT_GUARD_CANCEL = 7,
    QOH99_TRAINING_TRIGGER_EVENT_COUNT = 8
};

enum Qoh99TrainingTriggerOutcomeV1 {
    QOH99_TRAINING_TRIGGER_OUTCOME_NONE = 0,
    QOH99_TRAINING_TRIGGER_OUTCOME_STARTED = 1,
    QOH99_TRAINING_TRIGGER_OUTCOME_CHANCE_SKIPPED = 2,
    QOH99_TRAINING_TRIGGER_OUTCOME_BUSY = 3,
    QOH99_TRAINING_TRIGGER_OUTCOME_EMPTY_SLOT = 4,
    QOH99_TRAINING_TRIGGER_OUTCOME_COLLISION = 5,
    QOH99_TRAINING_TRIGGER_OUTCOME_QUEUE_FULL = 6,
    QOH99_TRAINING_TRIGGER_OUTCOME_UNAVAILABLE_MOVE = 7,
    QOH99_TRAINING_TRIGGER_OUTCOME_WINDOW_CLOSED = 8,
    QOH99_TRAINING_TRIGGER_OUTCOME_CONFIG_CHANGED = 9,
    QOH99_TRAINING_TRIGGER_OUTCOME_SUPERSEDED = 10
};

enum Qoh99TrainingWakeupReversalPhaseV1 {
    QOH99_TRAINING_WAKEUP_IDLE = 0,
    QOH99_TRAINING_WAKEUP_WATCHING = 1,
    QOH99_TRAINING_WAKEUP_WAITING_SUFFIX = 2,
    QOH99_TRAINING_WAKEUP_PRIMING = 3,
    QOH99_TRAINING_WAKEUP_FIRST_RESPONSE = 4,
    QOH99_TRAINING_WAKEUP_RELEASED = 5,
    QOH99_TRAINING_WAKEUP_COMPLETED = 6,
    QOH99_TRAINING_WAKEUP_CANCELLED = 7
};

enum Qoh99TrainingCombatContactKindV1 {
    QOH99_TRAINING_CONTACT_NONE = 0,
    QOH99_TRAINING_CONTACT_HIT = 1,
    QOH99_TRAINING_CONTACT_BLOCK = 2,
    QOH99_TRAINING_CONTACT_THROW = 3
};

enum Qoh99TrainingFrameAdvantageStateV1 {
    QOH99_TRAINING_FRAME_ADVANTAGE_IDLE = 0,
    QOH99_TRAINING_FRAME_ADVANTAGE_MEASURING = 1,
    QOH99_TRAINING_FRAME_ADVANTAGE_COMPLETE = 2,
    QOH99_TRAINING_FRAME_ADVANTAGE_TRADE = 3,
    QOH99_TRAINING_FRAME_ADVANTAGE_TIMED_OUT = 4,
    QOH99_TRAINING_FRAME_ADVANTAGE_UNMEASURABLE = 5
};

#define QOH99_TRAINING_GUARD_GAUGE_FULL 100u
#define QOH99_TRAINING_EX_STOCKS_FULL 9u
#define QOH99_TRAINING_EX_FRACTION_MAX 53u
#define QOH99_TRAINING_RECOVERY_DELAY_STEPS_MAX 240u

enum Qoh99TrainingRecoveryModeV1 {
    QOH99_TRAINING_RECOVERY_OFF = 0,
    QOH99_TRAINING_RECOVERY_EARLIEST = 1,
    QOH99_TRAINING_RECOVERY_FIXED_DELAY = 2,
    QOH99_TRAINING_RECOVERY_RANDOM_DELAY = 3,
    QOH99_TRAINING_RECOVERY_LATE = 4
};

enum Qoh99TrainingRecoveryPolicySlotV1 {
    QOH99_TRAINING_RECOVERY_SLOT_AIR = 0,
    QOH99_TRAINING_RECOVERY_SLOT_GROUND = 1,
    QOH99_TRAINING_RECOVERY_SLOT_WALL = 2,
    QOH99_TRAINING_RECOVERY_SLOT_TERMINAL = 3,
    QOH99_TRAINING_RECOVERY_SLOT_THROW_ESCAPE = 4,
    QOH99_TRAINING_RECOVERY_SLOT_COUNT = 5
};

/* The eligibility bit mask is indexed by these; never renumber.
   Mirrors qtraining::RecoveryBranch. */
enum Qoh99TrainingRecoveryBranchV1 {
    QOH99_TRAINING_RECOVERY_BRANCH_AIR_22 = 0,
    QOH99_TRAINING_RECOVERY_BRANCH_GROUND_88 = 1,
    QOH99_TRAINING_RECOVERY_BRANCH_WALL_66 = 2,
    QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_66 = 3,
    QOH99_TRAINING_RECOVERY_BRANCH_TERMINAL_44 = 4,
    QOH99_TRAINING_RECOVERY_BRANCH_THROW_ESCAPE = 5,
    QOH99_TRAINING_RECOVERY_BRANCH_COUNT = 6
};

enum Qoh99TrainingTerminalRecoveryV1 {
    QOH99_TRAINING_TERMINAL_PRESERVE = 0,
    QOH99_TRAINING_TERMINAL_FORWARD = 1,
    QOH99_TRAINING_TERMINAL_BACK = 2,
    QOH99_TRAINING_TERMINAL_RANDOM = 3
};

enum Qoh99TrainingRecoveryStateV1 {
    QOH99_TRAINING_RECOVERY_STATE_IDLE = 0,
    QOH99_TRAINING_RECOVERY_STATE_WAIT_ELIGIBLE = 1,
    QOH99_TRAINING_RECOVERY_STATE_DELAY = 2,
    QOH99_TRAINING_RECOVERY_STATE_PRE_NEUTRAL = 3,
    QOH99_TRAINING_RECOVERY_STATE_TAP1 = 4,
    QOH99_TRAINING_RECOVERY_STATE_NEUTRAL = 5,
    QOH99_TRAINING_RECOVERY_STATE_TAP2 = 6,
    QOH99_TRAINING_RECOVERY_STATE_AWAIT = 7,
    QOH99_TRAINING_RECOVERY_STATE_OBSERVING_BUFFER = 8,
    QOH99_TRAINING_RECOVERY_STATE_COOLDOWN = 9
};

enum Qoh99TrainingRecoveryVerdictV1 {
    QOH99_TRAINING_RECOVERY_VERDICT_NONE = 0,
    QOH99_TRAINING_RECOVERY_VERDICT_SUCCEEDED = 1,
    QOH99_TRAINING_RECOVERY_VERDICT_MISSED = 2,
    QOH99_TRAINING_RECOVERY_VERDICT_CANCELLED = 3,
    QOH99_TRAINING_RECOVERY_VERDICT_BUFFERED = 4,
    QOH99_TRAINING_RECOVERY_VERDICT_AUTOMATIC = 5,
    QOH99_TRAINING_RECOVERY_VERDICT_COMMIT_BLOCKED = 6
};

enum Qoh99TrainingRecoveryEvaluatorReasonV1 {
    QOH99_TRAINING_RECOVERY_EVALUATOR_QUALIFIED = 0,
    QOH99_TRAINING_RECOVERY_EVALUATOR_NOT_ATTEMPTED = 1,
    QOH99_TRAINING_RECOVERY_EVALUATOR_ADDRESS_UNRESOLVED = 2,
    QOH99_TRAINING_RECOVERY_EVALUATOR_PROLOGUE_MISMATCH = 3,
    QOH99_TRAINING_RECOVERY_EVALUATOR_COUNT = 4
};

enum Qoh99TrainingRecoveryStandDownV1 {
    QOH99_TRAINING_RECOVERY_STAND_DOWN_NONE = 0u,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_NOT_READABLE = 1u << 0,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_LIFECYCLE_DOWN = 1u << 1,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_SANDBOX_INACTIVE = 1u << 2,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_MENU_CAPTURED = 1u << 3,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_OPERATION_NEUTRAL = 1u << 4,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_MACRO_PLAYBACK = 1u << 5,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_MACRO_OWNS_P1 = 1u << 6,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_COM_CONTROLLED = 1u << 7,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_DIRECTION_SUPPRESSED = 1u << 8,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_KO_LATCH = 1u << 9,
    QOH99_TRAINING_RECOVERY_STAND_DOWN_COUNT = 10
};

enum Qoh99TrainingHoldReasonV1 {
    QOH99_TRAINING_HOLD_NONE = 0u,
    QOH99_TRAINING_HOLD_USER = 1u << 0,
    QOH99_TRAINING_HOLD_MENU = 1u << 1
};

enum Qoh99TrainingComboEpisodeStateV1 {
    QOH99_TRAINING_COMBO_EPISODE_IDLE = 0,
    QOH99_TRAINING_COMBO_EPISODE_IN_EPISODE = 1,
    QOH99_TRAINING_COMBO_EPISODE_SETTLING = 2
};

enum Qoh99TrainingNeutralReasonV1 {
    QOH99_TRAINING_NEUTRAL_OK = 0u,
    QOH99_TRAINING_NEUTRAL_FIGHTER_UNREADABLE = 1u << 0,
    QOH99_TRAINING_NEUTRAL_SLOT_OUT_OF_RANGE = 1u << 1,
    QOH99_TRAINING_NEUTRAL_ALTERNATE_FORM = 1u << 2,
    /* +48 / +84 not in the free-movement set {0, 26, 27, 28}. The values are the
           wire contract and never move. */
    QOH99_TRAINING_NEUTRAL_ACTION_NOT_FREE_MOVEMENT = 1u << 3,
    QOH99_TRAINING_NEUTRAL_DISPATCH_NOT_FREE_MOVEMENT = 1u << 4,
    QOH99_TRAINING_NEUTRAL_INIT_NOT_RUNNING = 1u << 5,
    QOH99_TRAINING_NEUTRAL_TRANSITION_PENDING = 1u << 6,
    QOH99_TRAINING_NEUTRAL_ACTION_LOCK_SET = 1u << 7,
    QOH99_TRAINING_NEUTRAL_POSTURE_NOT_GROUNDED = 1u << 8,
    QOH99_TRAINING_NEUTRAL_TERMINAL_LATCHED = 1u << 9,
    QOH99_TRAINING_NEUTRAL_ROUND_RESULT_LATCHED = 1u << 10,
    QOH99_TRAINING_NEUTRAL_FATAL_RECORDED = 1u << 11,
    QOH99_TRAINING_NEUTRAL_COM_OWNED = 1u << 12,
    QOH99_TRAINING_NEUTRAL_RECOVERY_LOCK_SET = 1u << 13,
    QOH99_TRAINING_NEUTRAL_RECOVERY_PHASE_LATCH = 1u << 14,
    QOH99_TRAINING_NEUTRAL_COMBO_ACTIVE = 1u << 15,
    QOH99_TRAINING_NEUTRAL_DAMAGE_UNSETTLED = 1u << 16,
    QOH99_TRAINING_NEUTRAL_ROUND_RESULT_STATE = 1u << 17,
    QOH99_TRAINING_NEUTRAL_FATAL_DISABLE_FLAG = 1u << 18
};

enum Qoh99TrainingRoundStartNeutralBitV1 {
    QOH99_TRAINING_ROUND_START_ACTION = 1u << 0,
    QOH99_TRAINING_ROUND_START_DISPATCH = 1u << 1,
    QOH99_TRAINING_ROUND_START_INIT = 1u << 2,
    QOH99_TRAINING_ROUND_START_NO_PENDING = 1u << 3,
    QOH99_TRAINING_ROUND_START_GROUNDED = 1u << 4,
    QOH99_TRAINING_ROUND_START_NO_TERMINAL = 1u << 5,
    QOH99_TRAINING_ROUND_START_NO_WALL = 1u << 6,
    QOH99_TRAINING_ROUND_START_FACING = 1u << 7,
    QOH99_TRAINING_ROUND_START_NO_RESULT = 1u << 8,
    QOH99_TRAINING_ROUND_START_FATAL_IDLE = 1u << 9,
    QOH99_TRAINING_ROUND_START_NO_COM = 1u << 10,
    QOH99_TRAINING_ROUND_START_LOCKS_CLEAR = 1u << 11,
    QOH99_TRAINING_ROUND_START_POSITION = 1u << 12,
    QOH99_TRAINING_ROUND_START_FRAME_OK = 1u << 13
};

enum Qoh99TrainingRefillTargetV1 {
    QOH99_TRAINING_REFILL_BOTH = 0,
    QOH99_TRAINING_REFILL_P1 = 1,
    QOH99_TRAINING_REFILL_P2 = 2
};

enum Qoh99TrainingGuardPermissionV1 {
    QOH99_TRAINING_GUARD_PERMISSION_STAND = 0x1u,
    QOH99_TRAINING_GUARD_PERMISSION_CROUCH = 0x2u,
    QOH99_TRAINING_GUARD_PERMISSION_AIR = 0x4u
};

enum Qoh99TrainingGuardClassificationV1 {
    QOH99_TRAINING_GUARD_CLASS_UNGUARDABLE = 0,
    QOH99_TRAINING_GUARD_CLASS_STAND = 1,
    QOH99_TRAINING_GUARD_CLASS_CROUCH = 2,
    QOH99_TRAINING_GUARD_CLASS_EITHER = 3,
    QOH99_TRAINING_GUARD_CLASS_AIR = 4,
    QOH99_TRAINING_GUARD_CLASS_INVALID = 5
};

enum Qoh99TrainingMacroStateV1 {
    QOH99_TRAINING_MACRO_IDLE = 0,
    QOH99_TRAINING_MACRO_RECORD_ARMED = 1,
    QOH99_TRAINING_MACRO_RECORDING = 2,
    QOH99_TRAINING_MACRO_RECORD_STOPPING = 3,
    QOH99_TRAINING_MACRO_PLAY_ARMED = 4,
    QOH99_TRAINING_MACRO_PLAYING = 5,
    QOH99_TRAINING_MACRO_PLAY_STOPPING = 6,
    QOH99_TRAINING_MACRO_PRE_RECORD = 7
};

/* qtraining::StatusEvent / StatusMode, mirrored. Appended only, never
   renumbered: an older overlay must keep meaning what it meant. */
enum Qoh99TrainingStatusEventV1 {
    QOH99_TRAINING_STATUS_EVENT_NONE = 0,
    QOH99_TRAINING_STATUS_EVENT_POSITION_RESET = 1,
    QOH99_TRAINING_STATUS_EVENT_STATE_SAVED = 2,
    QOH99_TRAINING_STATUS_EVENT_STATE_LOADED = 3,
    QOH99_TRAINING_STATUS_EVENT_STATE_CLEARED = 4,
    QOH99_TRAINING_STATUS_EVENT_STATE_SLOT = 5,
    QOH99_TRAINING_STATUS_EVENT_MACRO_SLOT = 6,
    QOH99_TRAINING_STATUS_EVENT_MACRO_CLEARED = 7,
    QOH99_TRAINING_STATUS_EVENT_MACRO_RECORDED = 8,
    QOH99_TRAINING_STATUS_EVENT_MACRO_STOPPED = 9,
    QOH99_TRAINING_STATUS_EVENT_ROUND_RESET = 10
};

enum Qoh99TrainingStatusModeV1 {
    QOH99_TRAINING_STATUS_MODE_NONE = 0,
    QOH99_TRAINING_STATUS_MODE_PRE_RECORD = 1,
    QOH99_TRAINING_STATUS_MODE_RECORDING = 2,
    QOH99_TRAINING_STATUS_MODE_PLAYING = 3
};

enum Qoh99TrainingPlaybackModeV1 {
    QOH99_TRAINING_PLAY_ONCE = 0,
    QOH99_TRAINING_PLAY_LOOP = 1,
    QOH99_TRAINING_PLAY_RANDOM = 2,
    QOH99_TRAINING_PLAY_RANDOM_LOOP = 3
};

enum Qoh99TrainingFacingModeV1 {
    QOH99_TRAINING_FACING_RELATIVE = 0,
    QOH99_TRAINING_FACING_ABSOLUTE = 1
};

enum Qoh99TrainingInputRearmStateV1 {
    QOH99_TRAINING_INPUT_ARMED = 0,
    QOH99_TRAINING_INPUT_CAPTURED = 1,
    QOH99_TRAINING_INPUT_AWAIT_NEUTRAL = 2,
    QOH99_TRAINING_INPUT_NEUTRAL_SETTLE = 3
};

enum Qoh99TrainingAdmissionResultV1 {
    QOH99_TRAINING_ADMISSION_NONE = 0,
    QOH99_TRAINING_ADMISSION_ADMITTED = 1,
    QOH99_TRAINING_ADMISSION_NATIVE_RETURNED_ZERO = 2,
    QOH99_TRAINING_ADMISSION_NATIVE_RETURNED_ONE = 3,
    QOH99_TRAINING_ADMISSION_WRONG_SCENE = 4,
    QOH99_TRAINING_ADMISSION_WRONG_MATCH_SUBSTATE = 5,
    QOH99_TRAINING_ADMISSION_WAIT_COUNT_BLOCKED = 6,
    QOH99_TRAINING_ADMISSION_PAUSED = 7,
    QOH99_TRAINING_ADMISSION_FROZEN = 8,
    QOH99_TRAINING_ADMISSION_REPLAY_OR_SPECTATOR = 9,
    QOH99_TRAINING_ADMISSION_INVARIANT_FAILED = 10
};

enum Qoh99TrainingCommandKindV1 {
    QOH99_TRAINING_COMMAND_NONE = 0,
    QOH99_TRAINING_COMMAND_RESET_POSITION = 1,
    QOH99_TRAINING_COMMAND_SAVE_STATE = 2,
    QOH99_TRAINING_COMMAND_LOAD_STATE = 3,
    QOH99_TRAINING_COMMAND_CLEAR_STATE = 4,
    QOH99_TRAINING_COMMAND_CLEAR_MACRO = 5,
    QOH99_TRAINING_COMMAND_TOGGLE_RECORD = 6,
    QOH99_TRAINING_COMMAND_TOGGLE_PLAYBACK = 7,
    QOH99_TRAINING_COMMAND_STOP_MACRO = 8,
    QOH99_TRAINING_COMMAND_SELECT_MACRO_SLOT = 9,
    QOH99_TRAINING_COMMAND_SELECT_STATE_SLOT = 10,
    QOH99_TRAINING_COMMAND_SET_GUARD_MODE = 11,
    QOH99_TRAINING_COMMAND_SET_GUARD_DURING_PLAYBACK = 12,
    QOH99_TRAINING_COMMAND_SET_PLAYBACK_MODE = 13,
    QOH99_TRAINING_COMMAND_SET_FACING_POLICY = 14,
    QOH99_TRAINING_COMMAND_SET_HITBOX_DISPLAY = 15,
    QOH99_TRAINING_COMMAND_RETURN_TO_CHARACTER_SELECT = 16,
    QOH99_TRAINING_COMMAND_SET_HP_REGEN = 17,
    /* Mirrors qtraining::TrainingCommandKind. Appended only, never renumbered. */
    QOH99_TRAINING_COMMAND_SET_STANCE = 18,
    QOH99_TRAINING_COMMAND_SET_TECH = 19,
    /* One-shot refills: slotOrSetting and flags must be zero, value is a Qoh99TrainingRefillTargetV1, and commands never coalesce. */
    QOH99_TRAINING_COMMAND_REFILL_LIFE = 20,
    QOH99_TRAINING_COMMAND_REFILL_GUARD_GAUGE = 21,
    QOH99_TRAINING_COMMAND_REFILL_EX_METER = 22,
    QOH99_TRAINING_COMMAND_SET_LIFE_VALUE = 23,
    QOH99_TRAINING_COMMAND_SET_EX_METER = 24,
    QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_VALUE = 25,
    QOH99_TRAINING_COMMAND_SET_HEALTH_POLICY = 26,
    QOH99_TRAINING_COMMAND_SET_EX_POLICY = 27,
    QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_POLICY = 28,
    QOH99_TRAINING_COMMAND_RESET_ROUND_START_NEUTRAL = 29,
    QOH99_TRAINING_COMMAND_SET_RECOVERY_POLICY = 30,
    QOH99_TRAINING_COMMAND_SET_RECOVERY_TIMING = 31,
    QOH99_TRAINING_COMMAND_SET_TERMINAL_RECOVERY = 32,
    QOH99_TRAINING_COMMAND_TOGGLE_PAUSE = 33,
    QOH99_TRAINING_COMMAND_FRAME_STEP = 34,
    QOH99_TRAINING_COMMAND_SET_METER_CHARGE_CONFIG = 35,
    QOH99_TRAINING_COMMAND_SET_THROW_TECH_CHANCE = 36,
    QOH99_TRAINING_COMMAND_SET_TRIGGER_CONFIG = 37,
    QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED = 38,
    QOH99_TRAINING_COMMAND_SET_DEBUG_OVERLAY_ENABLED = 39,
    QOH99_TRAINING_COMMAND_SET_STATE_CHANGE_LOG_ENABLED = 40,
    /* slotOrSetting 0 global / 1 P1 / 2 P2; value = new VISIBLE mask. */
    QOH99_TRAINING_COMMAND_SET_NATIVE_HUD_MASK = 41,
    QOH99_TRAINING_COMMAND_SET_FRAME_TRACE_ENABLED = 42,
    QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY = 43,
    /* slotOrSetting 0 unpins (restore to full), 1 pins with
       value = (stocks << 16) | fraction. */
    QOH99_TRAINING_COMMAND_SET_EX_RESTORE_TARGET = 44,
    /* value = 0 off, 1 play the selected slot, 2 record into it. */
    QOH99_TRAINING_COMMAND_SET_MACRO_AFTER_RESET = 45,
    /* WHEN the dummy guards: value = Qoh99TrainingGuardTimingV1, flags = the
       window in steps (1..600). Both halves travel together. */
    QOH99_TRAINING_COMMAND_SET_GUARD_TIMING = 46,
    QOH99_TRAINING_COMMAND_SET_PLAYER_SWAP = 47,
    QOH99_TRAINING_COMMAND_BEGIN_PRE_RECORD = 48,
    QOH99_TRAINING_COMMAND_COUNT = 49
};

/* Mirrors qtraining::TrainingContactPolicySlot and the three policy enums. */
enum Qoh99TrainingContactPolicySlotV1 {
    QOH99_TRAINING_CONTACT_SLOT_FAULTLESS = 0,
    QOH99_TRAINING_CONTACT_SLOT_PARRY = 1,
    QOH99_TRAINING_CONTACT_SLOT_CRITICAL_P1 = 2,
    QOH99_TRAINING_CONTACT_SLOT_CRITICAL_P2 = 3,
    QOH99_TRAINING_CONTACT_SLOT_REDUCE_P1 = 4,
    QOH99_TRAINING_CONTACT_SLOT_REDUCE_P2 = 5
};

enum Qoh99TrainingReducePolicyV1 {
    QOH99_TRAINING_REDUCE_NATIVE = 0,
    QOH99_TRAINING_REDUCE_ALWAYS_STRONG = 1,
    QOH99_TRAINING_REDUCE_ALWAYS_LIGHT = 2,
    QOH99_TRAINING_REDUCE_NEVER = 3
};

enum Qoh99TrainingGuardTimingV1 {
    QOH99_TRAINING_GUARD_TIMING_ALWAYS = 0,
    QOH99_TRAINING_GUARD_TIMING_AFTER_BEING_HIT = 1,
    QOH99_TRAINING_GUARD_TIMING_UNTIL_FIRST_BLOCK = 2
};

enum Qoh99TrainingFaultlessPolicyV1 {
    QOH99_TRAINING_FAULTLESS_NATIVE_INPUT = 0,
    QOH99_TRAINING_FAULTLESS_ALWAYS_WHEN_LEGAL = 1,
    QOH99_TRAINING_FAULTLESS_NEVER = 2
};

enum Qoh99TrainingParryPolicyV1 {
    QOH99_TRAINING_PARRY_NATIVE_INPUT = 0,
    QOH99_TRAINING_PARRY_ALWAYS_WHEN_LEGAL = 1,
    QOH99_TRAINING_PARRY_NEVER = 2
};

enum Qoh99TrainingCriticalPolicyV1 {
    QOH99_TRAINING_CRITICAL_NATIVE = 0,
    QOH99_TRAINING_CRITICAL_ALWAYS_SINGLE = 1,
    QOH99_TRAINING_CRITICAL_NEVER_RANDOM = 2,
    QOH99_TRAINING_CRITICAL_NEVER_ANY = 3,
    QOH99_TRAINING_CRITICAL_FORCE_RANDOM_ROLL = 4
};

enum Qoh99TrainingActorPoolV1 {
    QOH99_TRAINING_ACTOR_POOL_EFFECT = 0,
    QOH99_TRAINING_ACTOR_POOL_CLONE = 1,
    QOH99_TRAINING_ACTOR_POOL_HELPER = 2
};

enum Qoh99TrainingSuppressionReasonV1 {
    QOH99_TRAINING_SUPPRESSION_NONE = 0,
    QOH99_TRAINING_SUPPRESSION_NO_EVENT_ARMED = 1,
    QOH99_TRAINING_SUPPRESSION_EVENT_NOT_CONFIGURED = 2,
    QOH99_TRAINING_SUPPRESSION_WAITING_RESPONSE_READY = 3,
    QOH99_TRAINING_SUPPRESSION_WAITING_DELAY = 4,
    QOH99_TRAINING_SUPPRESSION_WAITING_GUARD_CANCEL_WINDOW = 5,
    QOH99_TRAINING_SUPPRESSION_WAITING_WAKEUP_RELEASE = 6,
    QOH99_TRAINING_SUPPRESSION_MACRO_BUSY = 7,
    QOH99_TRAINING_SUPPRESSION_MOVE_UNAVAILABLE = 8,
    QOH99_TRAINING_SUPPRESSION_EMPTY_SLOT = 9,
    QOH99_TRAINING_SUPPRESSION_CHANCE_SKIPPED = 10,
    QOH99_TRAINING_SUPPRESSION_NOT_MEASURABLE = 11
};

enum Qoh99TrainingCommandResultV1 {
    QOH99_TRAINING_COMMAND_RESULT_NONE = 0,
    QOH99_TRAINING_COMMAND_RESULT_APPLIED = 1,
    QOH99_TRAINING_COMMAND_RESULT_REJECTED_UNAVAILABLE = 2,
    QOH99_TRAINING_COMMAND_RESULT_REJECTED_UNSAFE_SCENE = 3,
    QOH99_TRAINING_COMMAND_RESULT_REJECTED_INVALID_SLOT = 4,
    QOH99_TRAINING_COMMAND_RESULT_REJECTED_INVALID_SETTING = 5,
    QOH99_TRAINING_COMMAND_RESULT_REJECTED_STALE_LAYOUT = 6,
    QOH99_TRAINING_COMMAND_RESULT_NATIVE_VALIDATION_FAILED = 7,
    QOH99_TRAINING_COMMAND_RESULT_SNAPSHOT_FAILED = 8
};

enum Qoh99TrainingDisableReasonV1 {
    QOH99_TRAINING_DISABLE_NONE = 0,
    QOH99_TRAINING_DISABLE_NOT_TRAINING_LAUNCH = 1,
    QOH99_TRAINING_DISABLE_ONLINE_MODE = 2,
    QOH99_TRAINING_DISABLE_SPECTATOR_MODE = 3,
    QOH99_TRAINING_DISABLE_SELFTEST_OR_PROBE = 4,
    QOH99_TRAINING_DISABLE_UNSUPPORTED_BUILD = 5,
    QOH99_TRAINING_DISABLE_UNSUPPORTED_STEP_BOUNDARY = 6,
    QOH99_TRAINING_DISABLE_UNSAFE_SCENE = 7,
    QOH99_TRAINING_DISABLE_LAYOUT_INVALID = 8,
    QOH99_TRAINING_DISABLE_INVARIANT_FAILURE = 9,
    QOH99_TRAINING_DISABLE_SNAPSHOT_FAILURE = 10
};

enum Qoh99TrainingLifecycleStateV1 {
    QOH99_TRAINING_LIFECYCLE_OFF = 0,
    QOH99_TRAINING_LIFECYCLE_LAUNCH_REQUESTED = 1,
    QOH99_TRAINING_LIFECYCLE_CHARACTER_SELECT = 2,
    QOH99_TRAINING_LIFECYCLE_MATCH_INITIALIZING = 3,
    QOH99_TRAINING_LIFECYCLE_ACTIVE = 4,
    QOH99_TRAINING_LIFECYCLE_REINITIALIZING = 5,
    QOH99_TRAINING_LIFECYCLE_RETURNING_TO_SELECT = 6,
    QOH99_TRAINING_LIFECYCLE_FAILED = 7
};

enum Qoh99TrainingCapabilityV1 {
    QOH99_TRAINING_CAP_NONE = 0,
    QOH99_TRAINING_CAP_RESET = 1u << 0,
    QOH99_TRAINING_CAP_ENDLESS_ROUND = 1u << 1,
    QOH99_TRAINING_CAP_INFINITE_TIME = 1u << 2,
    QOH99_TRAINING_CAP_DUMMY_GUARD = 1u << 3,
    QOH99_TRAINING_CAP_MACROS = 1u << 4,
    QOH99_TRAINING_CAP_SAVESTATES = 1u << 5,
    QOH99_TRAINING_CAP_HITBOXES = 1u << 6,
    QOH99_TRAINING_CAP_RETURN_TO_SELECT = 1u << 7,
    QOH99_TRAINING_CAP_REFILL = 1u << 8,
    QOH99_TRAINING_CAP_METER_CHARGE = 1u << 9,
    QOH99_TRAINING_CAP_TRIGGERS = 1u << 10,
    QOH99_TRAINING_CAP_FRAME_ADVANTAGE = 1u << 11,
    QOH99_TRAINING_CAP_THROW_TECH = 1u << 12,
    QOH99_TRAINING_CAP_FAULTLESS_POLICY = 1u << 13,
    QOH99_TRAINING_CAP_PARRY_POLICY = 1u << 14,
    QOH99_TRAINING_CAP_CRITICAL_POLICY = 1u << 15,
    QOH99_TRAINING_CAP_REDUCE_POLICY = 1u << 16
};

typedef struct Qoh99TrainingStatusV1 {
    DWORD apiVersion;
    DWORD structSize;
    DWORD explicitTrainingPresent;
    DWORD available;
    DWORD disableReason;
    DWORD lifecycleState;
    DWORD inMatchReady;
    DWORD capabilityFlags;
    DWORD trainingGeneration;
    DWORD realStepSerialLow;
    DWORD realStepSerialHigh;

    DWORD scene;
    DWORD matchSubstate;
    DWORD p1Character;
    DWORD p2Character;
    DWORD p1Facing;
    DWORD p2Facing;
    DWORD p1FinalLogicalMask;
    DWORD p2FinalLogicalMask;

    DWORD comMask;
    DWORD p1ComPrimary;
    DWORD p1ComMirror;
    DWORD p2ComPrimary;
    DWORD p2ComMirror;

    DWORD guardMode;
    DWORD guardPermission;
    DWORD guardClassification;
    DWORD guardPredictionMisses;
    DWORD guardDuringPlayback;

    DWORD endlessRoundRequested;
    DWORD endlessRoundEffective;
    DWORD infiniteTimeRequested;
    DWORD infiniteTimeEffective;
    DWORD extraFunctionsDisabled;
    DWORD waitCountZero;
    DWORD pauseNeutral;
    DWORD freezeNeutral;

    DWORD lastAdmissionResult;
    DWORD lastNativeReturn;
    DWORD inputRearmState;
    DWORD macroState;
    DWORD macroCursor;
    DWORD selectedMacroSlot;
    DWORD playbackMode;
    DWORD facingPolicy;
    DWORD validMacroSlots;
    DWORD macroFrameCounts[QOH99_TRAINING_SLOT_COUNT];
    DWORD macroFrameCapacity;
    DWORD selectedStateSlot;
    DWORD validStateSlots;
    DWORD staleStateSlots;
    DWORD rollbackLayoutGeneration;
    DWORD characterTeamFingerprintLow;
    DWORD characterTeamFingerprintHigh;
    DWORD hitboxRequested;
    DWORD hitboxEffective;
    DWORD menuCaptured;
    DWORD lastAcknowledgedCommandId;
    DWORD lastCommandResult;
    DWORD commandQueueDepth;
    DWORD commandQueueCapacity;
    DWORD commandQueueDrops;
    DWORD stickyValidationErrors;
    char disableText[96];
    char statusText[96];
    char resultText[96];

    /* Appended after the frozen prefix (offsets >= 568). Everything above is frozen;
           read nothing below unless structSize covers it. */
    DWORD combatReadoutValid;

    DWORD dummyStance;
    DWORD dummyTech;

    DWORD p1EffectiveLife;
    DWORD p2EffectiveLife;
    DWORD lifeMax;
    DWORD p1GuardGauge;
    DWORD p2GuardGauge;
    DWORD p1MeterStocks;
    DWORD p2MeterStocks;
    DWORD p1MeterFraction;
    DWORD p2MeterFraction;

    DWORD damageEventSerial;
    DWORD damageDeclinedCount;
    DWORD lastHitDamage;        /* int32 bit pattern; NOMINAL, pre-floor */
    DWORD lastHitPendingDelta;
    DWORD lastHitAttackerSlot;  /* CREDITED attacker, not always the live one */
    /* 0..3 for a described hit; anything else means no hit is described. */
    DWORD lastHitVictimSlot;
    /* RAW fatal-cause word: values outside 0..15 are legal and no decode table exists. */
    DWORD lastHitCause;
    DWORD lastHitAdmitted;

    /* Combo accumulators are indexed by the victim's side, match records by the attacker's. */
    DWORD p1ComboHitsTaken;
    DWORD p2ComboHitsTaken;
    DWORD p1ComboDamageTaken;
    DWORD p2ComboDamageTaken;
    DWORD p1LastComboHitsTaken;
    DWORD p2LastComboHitsTaken;
    DWORD p1LastComboDamageTaken;
    DWORD p2LastComboDamageTaken;
    DWORD p1MaxComboHitsDealt;
    DWORD p2MaxComboHitsDealt;
    DWORD p1MaxComboDamageDealt;
    DWORD p2MaxComboDamageDealt;

    /* Appended after the 696-byte combat tier (offsets >= 696). */
    DWORD p1LifeCurrent;        /* int32 bit pattern, life record +0  */
    DWORD p1LifePendingDamage;  /* int32 bit pattern, life record +4  */
    DWORD p1LifeReserve;        /* int32 bit pattern, life record +8  */
    DWORD p1LifeSettleTimer;    /* int32 bit pattern, life record +12 */
    DWORD p2LifeCurrent;
    DWORD p2LifePendingDamage;
    DWORD p2LifeReserve;
    DWORD p2LifeSettleTimer;

    DWORD fighterReadoutValid;

    DWORD p1ActionState;        /* RAW, fighter +48  */
    DWORD p1ActionFrame;        /* RAW, fighter +52  */
    DWORD p1ActionLockEnd;      /* RAW, fighter +56  */
    DWORD p1AnimFrame;          /* RAW, fighter +64  */
    DWORD p1NextAction;         /* RAW, fighter +72  */
    DWORD p1TransitionFlag;     /* RAW, fighter +76  */
    DWORD p1PrevAction;         /* RAW, fighter +80  */
    DWORD p1MoveGateByte;       /* RAW byte, fighter +156, masked 0xFF */
    DWORD p1PositionX;          /* int32 bit pattern, fighter +136 */
    DWORD p1PositionY;
    DWORD p2ActionState;
    DWORD p2ActionFrame;
    DWORD p2ActionLockEnd;
    DWORD p2AnimFrame;
    DWORD p2NextAction;
    DWORD p2TransitionFlag;
    DWORD p2PrevAction;
    DWORD p2MoveGateByte;
    DWORD p2PositionX;
    DWORD p2PositionY;

    DWORD holdReasons;

    DWORD guardHeuristicPostureDivergences;
    DWORD guardHeuristicAmbiguousAttackers;
    DWORD guardHeuristicUnguardablePostures;
    DWORD p1PostureClass;
    DWORD p2PostureClass;

    DWORD resourcePolicyValid;
    DWORD healthTrigger;
    DWORD exTrigger;
    DWORD guardGaugeTrigger;
    DWORD healthTriggerTarget;
    DWORD exTriggerTarget;
    DWORD guardGaugeTriggerTarget;
    DWORD bothFightersNeutral;
    DWORD neutralHoldSteps;
    DWORD p1NeutralReasonMask;
    DWORD p2NeutralReasonMask;
    DWORD neutralGlobalReasonMask;
    DWORD p1ComboEpisodeState;
    DWORD p2ComboEpisodeState;
    DWORD p1RecoveryLock;
    DWORD p2RecoveryLock;
    DWORD p1RecoveryPhaseLatch;
    DWORD p2RecoveryPhaseLatch;
    DWORD p1ComboHitsNative;
    DWORD p2ComboHitsNative;
    DWORD p1AuthoredFrameFlags;      /* RAW byte, fighter +158 */
    DWORD p2AuthoredFrameFlags;
    DWORD p1TerminalReady;           /* RAW, fighter +1528 */
    DWORD p2TerminalReady;
    DWORD p1DispatchAction;          /* RAW, fighter +84 */
    DWORD p2DispatchAction;
    DWORD p1InitStatus;              /* RAW, fighter +88, int32 bit pattern */
    DWORD p2InitStatus;
    DWORD lifeSetMax;
    DWORD exStocksMax;
    DWORD guardGaugeMax;
    DWORD resourcePolicyFires;
    DWORD resourcePolicyRejects;
    DWORD roundStartNeutralMask;
    DWORD roundStartNeutralValid;

    DWORD recoveryReadoutValid;
    DWORD recoveryModeAir;
    DWORD recoveryModeGround;
    DWORD recoveryModeWall;
    DWORD recoveryModeTerminal;
    DWORD recoveryModeThrowEscape;
    DWORD recoveryTerminalChoice;
    DWORD recoveryDelaySteps;
    DWORD recoveryRandomMinSteps;
    DWORD recoveryRandomMaxSteps;
    DWORD recoveryState;
    DWORD recoveryBranch;
    DWORD recoveryEligibleMask;
    DWORD recoveryLastVerdict;
    DWORD recoveryAttempts;
    DWORD recoverySucceeded;
    DWORD recoveryMissed;
    DWORD recoveryCancelled;
    DWORD recoveryBufferedObserved;
    DWORD recoveryAutomaticObserved;
    DWORD recoveryCommitBlocked;
    DWORD recoveryLastResultAction;
    DWORD recoveryStepsRemaining;
    DWORD recoveryContactTestUnknown;
    /* WALL_TIER: RAW fighter +1390 & 3, world space. */
    DWORD p1WallFlags;
    DWORD p2WallFlags;
    DWORD recoveryEvaluatorReason;
    DWORD recoveryStandDownMask;
    DWORD recoveryArmableMask;
    DWORD recoveryTapsEmitted;
    DWORD meterChargeTarget;
    DWORD meterChargeInterval;
    DWORD combatToolsReadoutValid;

    DWORD throwTechChancePercent;
    DWORD throwTechOpportunities;
    DWORD throwTechNativeAccepted;
    DWORD throwTechAutomaticAccepted;
    DWORD throwTechResultObserved;

    DWORD triggerMacroSlot[QOH99_TRAINING_TRIGGER_EVENT_COUNT];
    DWORD triggerDelaySteps[QOH99_TRAINING_TRIGGER_EVENT_COUNT];
    DWORD triggerChancePercent[QOH99_TRAINING_TRIGGER_EVENT_COUNT];
    DWORD triggerPendingCount;
    DWORD triggerStartedCount;
    DWORD triggerChanceSkippedCount;
    DWORD triggerDroppedCount;
    DWORD triggerLastEvent;
    DWORD triggerLastMacroSlot;
    DWORD triggerLastOutcome;

    DWORD frameAdvantageEnabled;
    DWORD frameAdvantageState;
    DWORD frameAdvantageContactKind;
    DWORD frameAdvantageAttackerOwner;
    DWORD frameAdvantageDefenderOwner;
    DWORD frameAdvantageValid;
    DWORD frameAdvantageValue;
    DWORD frameAdvantageAttackerReadyDelay;
    DWORD frameAdvantageDefenderReadyDelay;
    DWORD frameAdvantageCompletedCount;
    DWORD frameAdvantageTradeCount;
    DWORD frameAdvantageTimeoutCount;

    DWORD triggerMoveCatalogId;
    DWORD triggerMoveIndex[QOH99_TRAINING_TRIGGER_EVENT_COUNT];
    DWORD triggerLastMoveIndex;

    DWORD debugOverlayEnabled;
    DWORD debugStepSerialLow;
    DWORD debugStepSerialHigh;
    DWORD debugP1LogicalMask;
    DWORD debugP2LogicalMask;
    DWORD p1FreezeFrames;
    DWORD p2FreezeFrames;
    DWORD p1ResponseReady;
    DWORD p2ResponseReady;
    DWORD p2GuardCancelReady;
    DWORD wakeupReversalPhase;
    DWORD wakeupReversalMoveIndex;
    DWORD wakeupLockedStepsRemaining;
    DWORD wakeupCommitIndex;
    DWORD wakeupPrefixCursor;
    DWORD wakeupLastOutcome;

    DWORD stateChangeLogEnabled;
    DWORD stateChangeLogRecords;
    DWORD stateChangeLogFailed;
    DWORD triggerSuppressionReason;

    /* ACTOR_POOL_TIER: actorPoolSlot packs Qoh99TrainingActorPoolV1 in the high byte
           and the slot index in the low 24 bits. */
    DWORD actorPoolValid;
    DWORD actorPoolLiveCount;
    DWORD actorPoolReportedCount;
    DWORD actorPoolAction[QOH99_TRAINING_ACTOR_REPORT_CAPACITY];
    DWORD actorPoolOwner[QOH99_TRAINING_ACTOR_REPORT_CAPACITY];
    DWORD actorPoolSlot[QOH99_TRAINING_ACTOR_REPORT_CAPACITY];

    /* NATIVE_HUD_TIER: owner 0 None, 1 Training, 2 Replay. A set bit means visible. */
    DWORD nativeHudOwner;
    DWORD nativeHudGlobalMask;
    DWORD nativeHudP1Mask;
    DWORD nativeHudP2Mask;
    DWORD nativeHudUnavailableGlobalMask;
    DWORD nativeHudUnavailableP1Mask;
    DWORD nativeHudUnavailableP2Mask;

    DWORD nativeHudRequestedGlobalMask;
    DWORD nativeHudRequestedP1Mask;
    DWORD nativeHudRequestedP2Mask;

    DWORD frameTraceEnabled;
    DWORD frameTraceRows;
    DWORD frameTraceFailed;
    DWORD frameTraceCapped;

    DWORD frameAdvantageGapValid;
    DWORD frameAdvantageGapFrames;

    DWORD faultlessPolicy;
    DWORD parryPolicy;
    DWORD criticalP1Policy;
    DWORD criticalP2Policy;

    DWORD faultlessAvailable;
    DWORD parryAvailable;
    DWORD criticalAvailable;

    DWORD faultlessSupportedDummy;
    DWORD parrySupportedDummy;
    DWORD dummyFractionalMeter;

    DWORD lastContactStepLow;
    DWORD lastContactStepHigh;

    DWORD lastFaultlessNativeMode;
    DWORD lastFaultlessFinalMode;
    DWORD lastFaultlessMeterBefore;
    DWORD lastFaultlessReason;

    DWORD lastParryNativeResult;
    DWORD lastParryFinalResult;
    DWORD lastParryReason;

    DWORD lastCriticalAttackerSlot;
    DWORD lastCriticalIncomingHit;
    DWORD lastCriticalDivisor;
    DWORD lastCriticalNativeSample;
    DWORD lastCriticalFinalSample;
    DWORD lastCriticalGuardCrushLatch;
    DWORD lastCriticalNativeFlag;
    DWORD lastCriticalFinalFlag;
    DWORD lastCriticalDamageBefore;
    DWORD lastCriticalDamageAfter;
    DWORD lastCriticalReason;

    DWORD exRestorePinned;
    DWORD exRestoreStocks;
    DWORD exRestoreFraction;

    /* MACRO_AFTER_RESET_TIER: 0 off, 1 play the selected slot, 2 record into it. */
    DWORD macroAfterReset;

    /* Gate on QOH99_TRAINING_STATUS_REDUCE_POLICY_SIZE, never on sizeof. */
    DWORD reduceP1Policy;
    DWORD reduceP2Policy;
    DWORD reduceAvailable;
    DWORD lastReduceVictimSlot;
    DWORD lastReduceNativeVerdict;
    DWORD lastReduceFinalVerdict;
    DWORD lastReduceReason;

    DWORD guardTimingMode;
    DWORD guardTimingWindow;
    DWORD guardTimingArmed;

    DWORD playerSwap;

    DWORD statusEventKind;
    DWORD statusEventDetail;
    DWORD statusEventSequence;
    DWORD statusModeKind;
    DWORD statusModeDetail;
} Qoh99TrainingStatusV1;

typedef struct Qoh99TrainingCommandV1 {
    DWORD apiVersion;
    DWORD structSize;
    DWORD trainingGeneration;
    DWORD commandId;
    DWORD commandKind;
    DWORD slotOrSetting;
    DWORD value;
    DWORD flags;
} Qoh99TrainingCommandV1;

#ifdef __cplusplus
static_assert(sizeof(Qoh99TrainingCommandV1) == 32,
              "TrainingCommandV1 must mirror qtraining::TrainingCommandV1");
static_assert(offsetof(Qoh99TrainingCommandV1, commandKind) == 16,
              "TrainingCommandV1 commandKind offset changed");
static_assert(offsetof(Qoh99TrainingCommandV1, flags) == 28,
              "TrainingCommandV1 flags offset changed");
static_assert(sizeof(Qoh99TrainingStatusV1)
                  == 1104u
                     + ((25u + 3u * QOH99_TRAINING_TRIGGER_EVENT_COUNT)
                        * sizeof(DWORD))
                     + ((2u + QOH99_TRAINING_TRIGGER_EVENT_COUNT)
                        * sizeof(DWORD))
                     + 16u * sizeof(DWORD)
                     + 4u * sizeof(DWORD)
                     + ((3u + 3u * QOH99_TRAINING_ACTOR_REPORT_CAPACITY)
                        * sizeof(DWORD))
                     + 7u * sizeof(DWORD)
                     + 3u * sizeof(DWORD)
                     + 4u * sizeof(DWORD)
                     + 2u * sizeof(DWORD)
                     + 30u * sizeof(DWORD)
                     + 3u * sizeof(DWORD)
                     + 1u * sizeof(DWORD)
                     + 7u * sizeof(DWORD)
                     + 3u * sizeof(DWORD)
                     + 1u * sizeof(DWORD)
                     + 5u * sizeof(DWORD),
               "TrainingStatusV1 must mirror qtraining::TrainingStatusV1");
static_assert(sizeof(Qoh99TrainingStatusV1) % sizeof(DWORD) == 0,
              "TrainingStatusV1 must stay word-copyable by the publisher");
/* The five asserts below are the original v1 offsets; none may move. */
static_assert(offsetof(
                  Qoh99TrainingStatusV1, explicitTrainingPresent) == 8,
              "TrainingStatusV1 explicitTrainingPresent offset changed");
static_assert(offsetof(
                  Qoh99TrainingStatusV1, p1FinalLogicalMask) == 68,
              "TrainingStatusV1 p1FinalLogicalMask offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, macroFrameCounts) == 184,
              "TrainingStatusV1 macroFrameCounts offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, disableText) == 280,
              "TrainingStatusV1 disableText offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, resultText) == 472,
              "TrainingStatusV1 resultText offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, combatReadoutValid)
                  == QOH99_TRAINING_STATUS_V1_BASE_SIZE,
              "TrainingStatusV1 appended block must start at the frozen "
              "prefix size");
static_assert(offsetof(Qoh99TrainingStatusV1, lastHitCause) == 640,
              "TrainingStatusV1 lastHitCause offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, p2MaxComboDamageDealt) == 692,
              "TrainingStatusV1 p2MaxComboDamageDealt offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, p1LifeCurrent)
                  == QOH99_TRAINING_STATUS_V1_COMBAT_TIER,
              "TrainingStatusV1 fighter-tier block must start where the combat "
              "tier ended");
static_assert(offsetof(Qoh99TrainingStatusV1, p2LifeSettleTimer) == 724,
              "TrainingStatusV1 p2LifeSettleTimer offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, fighterReadoutValid) == 728,
              "TrainingStatusV1 fighterReadoutValid offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, p1ActionState) == 732,
              "TrainingStatusV1 p1ActionState offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, p2ActionState) == 772,
              "TrainingStatusV1 p2ActionState offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, p2PositionY) == 808,
              "TrainingStatusV1 p2PositionY offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, holdReasons)
                  == QOH99_TRAINING_STATUS_V1_FIGHTER_TIER,
              "TrainingStatusV1 recovery-tier block must start where the "
              "fighter tier ended");
static_assert(offsetof(
                  Qoh99TrainingStatusV1,
                  guardHeuristicPostureDivergences) == 816,
              "TrainingStatusV1 guard-heuristic group offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, p1PostureClass) == 828,
              "TrainingStatusV1 p1PostureClass offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, resourcePolicyValid) == 836,
              "TrainingStatusV1 resource group offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, bothFightersNeutral) == 864,
              "TrainingStatusV1 bothFightersNeutral offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, lifeSetMax) == 948,
              "TrainingStatusV1 lifeSetMax offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, recoveryReadoutValid) == 976,
              "TrainingStatusV1 recovery group offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, recoveryState) == 1016,
              "TrainingStatusV1 recoveryState offset changed");
static_assert(offsetof(
                  Qoh99TrainingStatusV1, recoveryContactTestUnknown) == 1068,
              "TrainingStatusV1 recoveryContactTestUnknown offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, p1WallFlags)
                  == QOH99_TRAINING_STATUS_V1_RECOVERY_TIER,
              "TrainingStatusV1 wall-tier block must start where the recovery "
              "tier ended");
static_assert(offsetof(Qoh99TrainingStatusV1, p2WallFlags) == 1076,
              "TrainingStatusV1 p2WallFlags offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, recoveryEvaluatorReason)
                  == QOH99_TRAINING_STATUS_V1_WALL_TIER,
              "TrainingStatusV1 recovery-diagnostic block must start where the "
              "wall tier ended");
static_assert(offsetof(Qoh99TrainingStatusV1, recoveryStandDownMask) == 1084,
              "TrainingStatusV1 recoveryStandDownMask offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, recoveryArmableMask) == 1088,
              "TrainingStatusV1 recoveryArmableMask offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, recoveryTapsEmitted) == 1092,
              "TrainingStatusV1 recoveryTapsEmitted offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, meterChargeTarget) == 1096,
              "TrainingStatusV1 meterChargeTarget offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, meterChargeInterval) == 1100,
               "TrainingStatusV1 meterChargeInterval offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, combatToolsReadoutValid)
                  == QOH99_TRAINING_STATUS_V1_METER_CHARGE_TIER,
              "TrainingStatusV1 combat-tools block must start where the meter "
              "charge tier ended");
static_assert(offsetof(Qoh99TrainingStatusV1, throwTechChancePercent) == 1108,
              "TrainingStatusV1 throw-tech settings offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, triggerMacroSlot) == 1128,
              "TrainingStatusV1 trigger settings offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, triggerPendingCount)
                  == 1128u
                     + 3u * QOH99_TRAINING_TRIGGER_EVENT_COUNT
                           * sizeof(DWORD),
              "TrainingStatusV1 trigger counters offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, frameAdvantageEnabled)
                  == 1156u
                     + 3u * QOH99_TRAINING_TRIGGER_EVENT_COUNT
                           * sizeof(DWORD),
              "TrainingStatusV1 frame-advantage offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, triggerMoveCatalogId)
                  == QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER,
              "TrainingStatusV1 action-trigger tier must append after the "
              "frozen combat-tools tier");
static_assert(offsetof(Qoh99TrainingStatusV1, triggerMoveIndex)
                  == QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER
                     + sizeof(DWORD),
              "TrainingStatusV1 action-trigger array offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, triggerLastMoveIndex)
                  == QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER
                     + (1u + QOH99_TRAINING_TRIGGER_EVENT_COUNT)
                           * sizeof(DWORD),
              "TrainingStatusV1 last action-trigger offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, debugOverlayEnabled)
                  == QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER,
              "TrainingStatusV1 debug-overlay tier offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, wakeupLastOutcome)
                  == QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER
                     + 15u * sizeof(DWORD),
              "TrainingStatusV1 debug-overlay tail offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, stateChangeLogEnabled)
                  == QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER,
              "TrainingStatusV1 state-log tier must append after the frozen "
              "debug-overlay tier");
static_assert(offsetof(Qoh99TrainingStatusV1, triggerSuppressionReason)
                  == QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER
                     + 3u * sizeof(DWORD),
              "TrainingStatusV1 state-log tail offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, actorPoolValid)
                  == QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER,
              "TrainingStatusV1 actor-pool tier must append after the frozen "
              "state-log tier");
static_assert(offsetof(Qoh99TrainingStatusV1, actorPoolAction)
                  == QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER
                     + 3u * sizeof(DWORD),
              "TrainingStatusV1 actor-pool array offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, nativeHudOwner)
                  == QOH99_TRAINING_STATUS_V1_ACTOR_POOL_TIER,
              "TrainingStatusV1 native-HUD tier must append after the frozen "
              "actor-pool tier");
static_assert(offsetof(Qoh99TrainingStatusV1, nativeHudRequestedGlobalMask)
                  == QOH99_TRAINING_STATUS_V1_NATIVE_HUD_TIER,
              "TrainingStatusV1 requested tier must append after the frozen "
              "native-HUD tier");
static_assert(offsetof(Qoh99TrainingStatusV1, frameTraceEnabled)
                  == QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER,
              "TrainingStatusV1 frame-trace tier must append after the frozen "
              "requested tier");
static_assert(offsetof(Qoh99TrainingStatusV1, frameTraceCapped)
                  == QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER
                     + 3u * sizeof(DWORD),
              "TrainingStatusV1 frame-trace tail offset changed");
static_assert(offsetof(Qoh99TrainingStatusV1, frameAdvantageGapValid)
                  == QOH99_TRAINING_STATUS_V1_FRAME_TRACE_TIER,
              "TrainingStatusV1 gap tier must append after the frozen "
              "frame-trace tier");
static_assert(offsetof(Qoh99TrainingStatusV1, faultlessPolicy)
                  == QOH99_TRAINING_STATUS_V1_FRAME_ADVANTAGE_GAP_TIER,
              "TrainingStatusV1 contact-policy tier must append after the "
              "frozen gap tier");
static_assert(offsetof(Qoh99TrainingStatusV1, exRestorePinned)
                  == QOH99_TRAINING_STATUS_V1_CONTACT_POLICY_TIER,
              "TrainingStatusV1 EX-restore tier must append after the "
              "frozen contact-policy tier");
static_assert(offsetof(Qoh99TrainingStatusV1, macroAfterReset)
                  == QOH99_TRAINING_STATUS_V1_EX_RESTORE_TARGET_TIER,
              "TrainingStatusV1 macro-after-reset tier must append after the "
              "frozen EX-restore tier");
static_assert(offsetof(Qoh99TrainingStatusV1, reduceP1Policy)
                  == QOH99_TRAINING_STATUS_V1_MACRO_AFTER_RESET_TIER,
              "TrainingStatusV1 reduce-policy tier must append after the "
              "frozen macro-after-reset tier");
static_assert(offsetof(Qoh99TrainingStatusV1, guardTimingMode)
                  == QOH99_TRAINING_STATUS_V1_REDUCE_POLICY_TIER,
              "TrainingStatusV1 guard-timing tier must append after the "
              "frozen reduce-policy tier");
static_assert(offsetof(Qoh99TrainingStatusV1, playerSwap)
                  == QOH99_TRAINING_STATUS_V1_GUARD_TIMING_TIER,
              "TrainingStatusV1 player-swap tier must append after the "
              "frozen guard-timing tier");
static_assert(offsetof(Qoh99TrainingStatusV1, statusEventKind)
                  == QOH99_TRAINING_STATUS_V1_PLAYER_SWAP_TIER,
              "TrainingStatusV1 status-message tier must append after the "
              "frozen player-swap tier");
static_assert(sizeof(Qoh99TrainingStatusV1)
                  == QOH99_TRAINING_STATUS_V1_STATUS_MESSAGE_TIER,
               "the newest named tier must be this build's whole struct");
static_assert(QOH99_TRAINING_STATUS_V1_BASE_SIZE
                      < QOH99_TRAINING_STATUS_V1_COMBAT_TIER
                  && QOH99_TRAINING_STATUS_V1_COMBAT_TIER
                         < QOH99_TRAINING_STATUS_V1_FIGHTER_TIER
                  && QOH99_TRAINING_STATUS_V1_FIGHTER_TIER
                         < QOH99_TRAINING_STATUS_V1_RECOVERY_TIER
                  && QOH99_TRAINING_STATUS_V1_RECOVERY_TIER
                         < QOH99_TRAINING_STATUS_V1_WALL_TIER
                  && QOH99_TRAINING_STATUS_V1_WALL_TIER
                         < QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER
                   && QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER
                          < QOH99_TRAINING_STATUS_V1_METER_CHARGE_TIER
                   && QOH99_TRAINING_STATUS_V1_METER_CHARGE_TIER
                          < QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER
                   && QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER
                          < QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER
                   && QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER
                          < QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER
                   && QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER
                          < QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER
                   && QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER
                          < QOH99_TRAINING_STATUS_V1_ACTOR_POOL_TIER
                   && QOH99_TRAINING_STATUS_V1_ACTOR_POOL_TIER
                          < QOH99_TRAINING_STATUS_V1_NATIVE_HUD_TIER
                   && QOH99_TRAINING_STATUS_V1_NATIVE_HUD_TIER
                          < QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER
                   && QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER
                          < QOH99_TRAINING_STATUS_V1_FRAME_TRACE_TIER
                   && QOH99_TRAINING_STATUS_V1_FRAME_TRACE_TIER
                          < QOH99_TRAINING_STATUS_V1_FRAME_ADVANTAGE_GAP_TIER,
               "named status tiers must ascend and never repeat");
#endif

typedef BOOL (__stdcall *EfzDx9TrainingStatusProviderV1)(
    Qoh99TrainingStatusV1 *outStatus,
    DWORD outSize,
    void *userData);
typedef BOOL (__stdcall *EfzDx9TrainingCommandEnqueueV1)(
    const Qoh99TrainingCommandV1 *command,
    DWORD commandSize,
    void *userData);
typedef BOOL (__stdcall *EfzDx9TrainingMenuCaptureV1)(
    DWORD trainingGeneration,
    BOOL captured,
    void *userData);

#define QOH99_INPUT_DISPLAY_API_VERSION 1u
#define QOH99_INPUT_DISPLAY_SNAPSHOT_V1_BASE_SIZE 1128u
#define QOH99_INPUT_DISPLAY_LANES 4u
#define QOH99_INPUT_DISPLAY_HISTORY 32u
#define QOH99_INPUT_DISPLAY_HOLD_CAP 999u
#define QOH99_INPUT_DISPLAY_NO_LANE 0xFFFFFFFFu

enum Qoh99InputDisplayModeV1 {
    QOH99_INPUT_DISPLAY_MODE_NONE = 0,
    QOH99_INPUT_DISPLAY_MODE_TRAINING = 1,
    QOH99_INPUT_DISPLAY_MODE_REPLAY = 2
};

/* Neutral is 0; compass points are 1..8 screen-clockwise from Right. */
enum Qoh99InputDisplayDirectionV1 {
    QOH99_INPUT_DISPLAY_DIR_NEUTRAL = 0,
    QOH99_INPUT_DISPLAY_DIR_RIGHT = 1,
    QOH99_INPUT_DISPLAY_DIR_DOWN_RIGHT = 2,
    QOH99_INPUT_DISPLAY_DIR_DOWN = 3,
    QOH99_INPUT_DISPLAY_DIR_DOWN_LEFT = 4,
    QOH99_INPUT_DISPLAY_DIR_LEFT = 5,
    QOH99_INPUT_DISPLAY_DIR_UP_LEFT = 6,
    QOH99_INPUT_DISPLAY_DIR_UP = 7,
    QOH99_INPUT_DISPLAY_DIR_UP_RIGHT = 8,
    QOH99_INPUT_DISPLAY_DIR_COUNT = 9
};

#define QOH99_INPUT_LOGICAL_A 0x00001u
#define QOH99_INPUT_LOGICAL_B 0x00002u
#define QOH99_INPUT_LOGICAL_C 0x00004u
#define QOH99_INPUT_LOGICAL_D 0x00008u
#define QOH99_INPUT_LOGICAL_UP 0x10000u
#define QOH99_INPUT_LOGICAL_DOWN 0x20000u
#define QOH99_INPUT_LOGICAL_LEFT 0x40000u
#define QOH99_INPUT_LOGICAL_RIGHT 0x80000u
#define QOH99_INPUT_LOGICAL_FIGHT_MASK 0xF000Fu

typedef struct Qoh99InputDisplayEntryV1 {
    DWORD logicalMask;
    DWORD heldSteps;
} Qoh99InputDisplayEntryV1;

typedef struct Qoh99InputDisplayLaneV1 {
    DWORD active;
    DWORD currentMask;
    DWORD entryCount;
    /* Index of the NEWEST entry.  MEANINGLESS while entryCount == 0, so every
       walk must test entryCount first. */
    DWORD head;
    Qoh99InputDisplayEntryV1 entries[QOH99_INPUT_DISPLAY_HISTORY];
} Qoh99InputDisplayLaneV1;

typedef struct Qoh99InputDisplaySnapshotV1 {
    DWORD apiVersion;
    DWORD structSize;
    DWORD mode;
    DWORD producerGeneration;
    DWORD stepSerialLow;
    DWORD stepSerialHigh;
    DWORD drivenLane;
    DWORD laneCount;
    DWORD historyCapacity;
    DWORD holdCap;
    Qoh99InputDisplayLaneV1 lanes[QOH99_INPUT_DISPLAY_LANES];
} Qoh99InputDisplaySnapshotV1;

#ifdef __cplusplus
static_assert(sizeof(Qoh99InputDisplayEntryV1) == 8,
              "InputDisplayEntryV1 must mirror qinput::InputDisplayEntryV1");
static_assert(sizeof(Qoh99InputDisplayLaneV1) == 272,
              "InputDisplayLaneV1 must mirror qinput::InputDisplayLaneV1");
static_assert(sizeof(Qoh99InputDisplaySnapshotV1) == 1128,
              "InputDisplaySnapshotV1 must mirror "
              "qinput::InputDisplaySnapshotV1");
static_assert(sizeof(Qoh99InputDisplaySnapshotV1)
                  == QOH99_INPUT_DISPLAY_SNAPSHOT_V1_BASE_SIZE,
              "the frozen input display prefix is the whole v1 struct");
static_assert(sizeof(Qoh99InputDisplaySnapshotV1) % sizeof(DWORD) == 0,
              "input display must stay word-copyable by the publisher");
static_assert(offsetof(Qoh99InputDisplayLaneV1, entries) == 16,
              "input display lane header must stay four words");
static_assert(offsetof(Qoh99InputDisplaySnapshotV1, mode) == 8,
              "input display mode offset changed");
static_assert(offsetof(Qoh99InputDisplaySnapshotV1, drivenLane) == 24,
              "input display drivenLane offset changed");
static_assert(offsetof(Qoh99InputDisplaySnapshotV1, lanes) == 40,
              "input display lanes offset changed");
static_assert(offsetof(Qoh99InputDisplaySnapshotV1, lanes)
                  + 3u * sizeof(Qoh99InputDisplayLaneV1) == 856,
              "input display lane stride changed");
#endif

typedef BOOL (__stdcall *EfzDx9InputDisplayProviderV1)(
    Qoh99InputDisplaySnapshotV1 *outSnapshot,
    DWORD outSize,
    void *userData);

#define QOH99_REPLAY_TRANSPORT_API_VERSION 2u

typedef struct Qoh99ReplayTransportStatusV2 {
    DWORD structBytes;
    DWORD apiVersion;
    DWORD generation;
    DWORD active;
    DWORD paused;
    DWORD mayHold;
    DWORD speedPermille;
    DWORD pendingSteps;
    DWORD nextRecord;
    DWORD endpointRecord;
    DWORD phase;
    DWORD droppedCommands;
    DWORD takeoverAvailable;
    DWORD takeoverPhase;
    DWORD takeoverLane;     /* 0 none, 1 P1, 2 P2 */
    DWORD takeoverDelay;
    DWORD takeoverCountdown;
    DWORD takeoverBranchRecord;
    DWORD takeoverElapsed;
    DWORD takeoverBeyondEndpoint;
    DWORD takeoverFault;
} Qoh99ReplayTransportStatusV2;

#ifdef __cplusplus
static_assert(sizeof(Qoh99ReplayTransportStatusV2) == 84,
              "replay transport status must mirror "
              "qreplay::ReplayTransportStatusV2");
static_assert(offsetof(Qoh99ReplayTransportStatusV2, generation) == 8,
              "replay transport generation offset changed");
static_assert(offsetof(Qoh99ReplayTransportStatusV2, takeoverFault) == 80,
              "replay takeover status tail offset changed");
#endif

enum Qoh99ReplayTransportCommandV2 {
    QOH99_REPLAY_TRANSPORT_TOGGLE_PAUSE = 3,
    QOH99_REPLAY_TRANSPORT_STEP = 4,
    QOH99_REPLAY_TRANSPORT_RESTART = 9,
    QOH99_REPLAY_TRANSPORT_SPEED_SLOWER = 13,
    QOH99_REPLAY_TRANSPORT_SPEED_FASTER = 14,
    QOH99_REPLAY_TAKEOVER_EXIT = 15,
    QOH99_REPLAY_TAKEOVER_P1 = 16,
    QOH99_REPLAY_TAKEOVER_P2 = 17,
    QOH99_REPLAY_TAKEOVER_RETRY = 18,
    QOH99_REPLAY_TAKEOVER_DELAY_DECREASE = 19,
    QOH99_REPLAY_TAKEOVER_DELAY_INCREASE = 20,
    QOH99_REPLAY_TAKEOVER_SET_DELAY = 21,
    QOH99_REPLAY_TOGGLE_COLLISION_DISPLAY = 22,
    QOH99_REPLAY_TRANSPORT_STEP_BACK = 23
};

typedef int (__stdcall *EfzDx9ReplayTransportStatusV2Fn)(
    Qoh99ReplayTransportStatusV2 *out,
    DWORD structBytes,
    void *userData);

typedef int (__stdcall *EfzDx9ReplayTransportSubmitV2Fn)(
    DWORD generation,
    DWORD command,
    ULONGLONG argument,
    void *userData);

struct IDirect3DDevice9;

/* Exported only for binary compatibility; QOH99 training must not register it. */
typedef BOOL (__stdcall *EfzDx9TrainingMenuVisibleFn)(void *userData);
typedef BOOL (__stdcall *EfzDx9TrainingMenuHandleMessageFn)(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT *outResult,
    void *userData);
typedef void (__stdcall *EfzDx9TrainingMenuRendererFn)(
    struct IDirect3DDevice9 *device,
    HWND hwnd,
    const EfzDx9PresentParams *params,
    void *userData);
typedef BOOL (__stdcall *EfzDx9TrainingMenuWantsDrawFn)(void *userData);
// phase: 0 = pre-reset (invalidate), 1 = post-reset (recreate). The mod must
// release its D3DPOOL_DEFAULT ImGui objects before the wrapper calls device->Reset().
typedef void (__stdcall *EfzDx9TrainingMenuDeviceNotifyFn)(int phase, void *userData);

BOOL EFZDX9_Presenter_PrepareDevice(
    HWND window, int clientWidth, int clientHeight,
    QohPresentationMode mode, QohRendererBackend backend);
BOOL EFZDX9_Presenter_HasDevice(void);
void EFZDX9_Presenter_RequestDeviceIfNeeded(
    HWND window, int clientWidth, int clientHeight,
    QohPresentationMode mode, QohRendererBackend backend);
void EFZDX9_Presenter_ProcessDeviceWorkOnWindowThread(HWND window);
BOOL EFZDX9_Presenter_GetPresentationStatus(
    EfzDx9PresentationStatus *outStatus, DWORD outSize);
BOOL EFZDX9_Presenter_SubmitReplayHotkey(DWORD command, BOOL repeat);
BOOL EFZDX9_Presenter_HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *outResult);
BOOL EFZDX9_Presenter_IsMenuVisible(void);
BOOL EFZDX9_Presenter_ShouldSuppressGameInput(void);
BOOL EFZDX9_Presenter_Present(const EfzDx9PresentParams *params);
void EFZDX9_Presenter_RecordGdiPresentation(
    const EfzDx9PresentParams *params,
    LONGLONG entryQpc,
    LONGLONG returnQpc,
    HRESULT result);
void EFZDX9_Presenter_SetActive(BOOL active);
void EFZDX9_Presenter_Shutdown(void);
void EFZDX9_Presenter_ToggleMenu(void);
void EFZDX9_Presenter_ToggleInputDisplay(void);
BOOL EFZDX9_Presenter_IsInputDisplayVisible(void);
enum Qoh99ReplayDisplayFlags {
    QOH99_REPLAY_DISPLAY_INPUTS = 4u,
    QOH99_REPLAY_DISPLAY_TRANSPORT = 8u
};
void __stdcall EFZDX9_SetReplayDisplayFlags(DWORD flags);
DWORD __stdcall EFZDX9_QueryReplayDisplayFlags(void);
__declspec(dllexport) DWORD __stdcall EFZDDRAW_QueryWrapperHotkey(
    const char *name, BYTE *outDiks, DWORD cap);
void __stdcall EFZDX9_RegisterNetplayHudProvider(
    EfzDx9NetplayHudProvider provider,
    void *userData);
enum Qoh99MenuNavigationV1 {
    QOH99_MENU_NAV_NONE = 0,
    QOH99_MENU_NAV_UP = 1,
    QOH99_MENU_NAV_DOWN = 2,
    QOH99_MENU_NAV_LEFT = 3,
    QOH99_MENU_NAV_RIGHT = 4,
    QOH99_MENU_NAV_CONFIRM = 5,
    QOH99_MENU_NAV_BACK = 6,
    QOH99_MENU_NAV_COUNT = 7
};

BOOL __stdcall EFZDX9_Presenter_SubmitMenuNavigation(DWORD navigation);

void __stdcall EFZDX9_Presenter_SetMenuNavigationLabels(
    const char *confirmLabel,
    const char *backLabel);

void __stdcall EFZDX9_RegisterOverlayProvider(
    EfzDx9OverlayProvider provider,
    void *userData);
void __stdcall EFZDX9_RegisterTrainingUiProvider(
    DWORD apiVersion,
    EfzDx9TrainingStatusProviderV1 statusProvider,
    EfzDx9TrainingCommandEnqueueV1 enqueueCommand,
    EfzDx9TrainingMenuCaptureV1 setMenuCapture,
    void *userData);
void __stdcall EFZDX9_RegisterInputDisplayProvider(
    DWORD apiVersion,
    EfzDx9InputDisplayProviderV1 provider,
    void *userData);
void __stdcall EFZDX9_RegisterReplayTransportProvider(
    DWORD apiVersion,
    EfzDx9ReplayTransportStatusV2Fn status,
    EfzDx9ReplayTransportSubmitV2Fn submit,
    void *userData);
typedef int (__stdcall *EfzDx9SpectatorHotkeySinkFn)(
    DWORD command, BOOL repeat, void *userData);
void __stdcall EFZDX9_RegisterSpectatorHotkeySink(
    EfzDx9SpectatorHotkeySinkFn sink, void *userData);
void __stdcall EFZDX9_RegisterTrainingMenuBridge(
    EfzDx9TrainingMenuRendererFn renderer,
    EfzDx9TrainingMenuVisibleFn isVisible,
    EfzDx9TrainingMenuHandleMessageFn handleMessage,
    EfzDx9TrainingMenuWantsDrawFn wantsDraw,
    EfzDx9TrainingMenuDeviceNotifyFn deviceNotify,
    void *userData);

#ifdef __cplusplus
}
#endif
