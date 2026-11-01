
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
import zipfile
from typing import Callable, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rolehost

if os.name != "nt":
    raise SystemExit("live_game_e2e.py requires Windows")

CREATE_NO_WINDOW = 0x08000000
LEGACY_ASSET_PATH_MAX = 127
HARNESS_OWNERSHIP_MARKER = ".qoh99-live-e2e-owned.json"
HARNESS_OWNERSHIP_SCHEMA = 1
HARNESS_ARTIFACT_ALLOWANCE_BYTES = 256 * 1024 * 1024
HARNESS_SAFETY_MARGIN_BYTES = 512 * 1024 * 1024
WM_CLOSE = 0x0010
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
STILL_ACTIVE = 259

PROCESS_TERMINATE = 0x0001
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_SET_INFORMATION = 0x0200
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_SUSPEND_RESUME = 0x0800
PROCESS_ACCESS = (
    PROCESS_TERMINATE
    | PROCESS_VM_OPERATION
    | PROCESS_VM_READ
    | PROCESS_VM_WRITE
    | PROCESS_QUERY_INFORMATION
    | PROCESS_SUSPEND_RESUME
)

THREAD_SUSPEND_RESUME = 0x0002
THREAD_GET_CONTEXT = 0x0008
THREAD_SET_CONTEXT = 0x0010
THREAD_QUERY_INFORMATION = 0x0040
THREAD_ACCESS = (
    THREAD_SUSPEND_RESUME
    | THREAD_GET_CONTEXT
    | THREAD_SET_CONTEXT
    | THREAD_QUERY_INFORMATION
)

PAGE_EXECUTE_READWRITE = 0x40

EXCEPTION_DEBUG_EVENT = 1
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_PROCESS_DEBUG_EVENT = 5
EXCEPTION_BREAKPOINT = 0x80000003
EXCEPTION_SINGLE_STEP = 0x80000004
STATUS_WX86_SINGLE_STEP = 0x4000001E
STATUS_WX86_BREAKPOINT = 0x4000001F
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001

WOW64_CONTEXT_i386 = 0x00010000
WOW64_CONTEXT_CONTROL = WOW64_CONTEXT_i386 | 0x00000001
WOW64_CONTEXT_INTEGER = WOW64_CONTEXT_i386 | 0x00000002
WOW64_CONTEXT_CONTROL_INTEGER = WOW64_CONTEXT_CONTROL | 0x00000002
TRAP_FLAG = 0x00000100

DESKTOP_READOBJECTS = 0x0001
DESKTOP_ENUMERATE = 0x0040
DESKTOP_WRITEOBJECTS = 0x0080

GAME_ADDR = {
    "scene_next": 0x61801C,
    "scene": 0x6234EC,
    "scene3_sub": 0x4AA454,
    "main_menu_sub": 0x4AB8C4,
    "main_menu_cursor": 0x4AB8D0,
    "p1_char": 0x870954,
    "p2_char": 0x870DB4,
    "p1_confirm": 0x8716D0,
    "p2_confirm": 0x8716D4,
    "match_start": 0x8716E8,
    "match_sub": 0x8719A4,
    "game_mode": 0x62369C,
    "vs_setup": 0x6236A0,
    "frame_counter": 0x4AB778,
    "guard_p1": 0x6265C8,
    "guard_p2": 0x6265F4,
    "life_current_p1": 0x6254E0,
    "life_damage_p1": 0x6254E4,
    "life_reserve_p1": 0x6254E8,
    "life_current_p2": 0x6254F0,
    "life_damage_p2": 0x6254F4,
    "life_reserve_p2": 0x6254F8,
    "ko_sequencer": 0x88098C,
    "round_counter": 0x871950,
    "round_wins_p1": 0x625628,
    "round_wins_p2": 0x62562C,
    "winner": 0x6236A8,
    "control_mask": 0x6236CC,
    "p1_x": 0x623E88,
    "p2_x": 0x626908,
    "p1_action": 0x623E30,
    "p2_action": 0x6268B0,
}

QOH_CONFIG_IMAGE_SIZE = 0x1F6C
QOH_CONFIG_PROFILE_BASE = 4
QOH_CONFIG_PROFILE_STRIDE = 0x2FC
QOH_CONFIG_PROFILE_COUNT = 5
QOH_CONFIG_PRESERVED_OFFSETS = (
    36,
    40,
    44,
    48,
    60,
    64,
    84,
    88,
    92,
    96,
    376,
)
QOH_CONFIG_NORMALIZED_OFFSETS = (60, 64, 84, 88, 92, 96)
QOH_CONFIG_NOSOUND_OFFSET = 376
QOH_CONFIG_NOSOUND_BIT = 0x00000002
QOH_CONFIG_STRESS_OFFSETS = (
    36,
    40,
    44,
    48,
    60,
    64,
    68,
    84,
    88,
    92,
    96,
    108,
    376,
)
QOH_CONFIG_STRESS_SPEC = {
    "host": {
        "profile": 1,
        "values": {
            36: 2,
            40: 4,
            44: 0,
            48: 2,
            60: 3,
            64: 1,
            68: 1,
            84: 3,
            88: 1,
            92: 4,
            96: 2,
            108: 1,
            376: 3,
        },
    },
    "client": {
        "profile": 3,
        "values": {
            36: 0,
            40: 0,
            44: 0,
            48: 0,
            60: 1,
            64: 1,
            68: 0,
            84: 4,
            88: 2,
            92: 3,
            96: 1,
            108: 0,
            376: 2,
        },
    },
}

NETPLAY_GO_RE = re.compile(
    r"\[netplay\] GO:.*?"
    r"\btimer=(?P<timer>\d+) "
    r"ai=(?P<ai>\d+) "
    r"life=(?P<life>\d+) "
    r"roundsMinusOne=(?P<rounds>\d+) "
    r"local=P(?P<local>[12])\b"
)
LAYOUT_CONFIG_RE = re.compile(
    r"\[layout-config\] valid=1 "
    r"profile=(?P<profile>\d+)\b.*?"
    r"\btimerMode36=(?P<timer>\d+) "
    r"aiDifficulty40=(?P<ai>\d+) "
    r"lifeMultiplier44=(?P<life>\d+) "
    r"roundsToWinMinusOne48=(?P<rounds>\d+)\b.*?"
    r"\blocal60=(?P<local60>\d+) "
    r"local64=(?P<local64>\d+) "
    r"local68=(?P<local68>\d+) "
    r"local84=(?P<local84>\d+) "
    r"local88=(?P<local88>\d+) "
    r"local92=(?P<local92>\d+) "
    r"local96=(?P<local96>\d+) "
    r"local108=(?P<local108>\d+) "
    r"local376=(?P<local376>[0-9A-Fa-f]{8})\b"
)
LAYOUT_REGION_RE = re.compile(
    r"\[layout-region\] index=(?P<index>\d+) "
    r"name=(?P<name>\S+) size=(?P<size>\d+) "
    r"present=(?P<present>[01]) hash=(?P<hash>[0-9A-Fa-f]{16})\b"
)
LAYOUT_COMMAND_MUTATION_RE = re.compile(
    r"\[test-hook\] layout command-history mutation "
    r"generation=(?P<generation>\d+) "
    r"target=(?P<target>inactive-p4|active-p2) "
    r"fighter=(?P<fighter>[0-9A-Fa-f]{8}) "
    r"child=(?P<child>[0-9A-Fa-f]{8}) "
    r"offset=(?P<offset>\d+) "
    r"before=(?P<before>[0-9A-Fa-f]{2}) "
    r"after=(?P<after>[0-9A-Fa-f]{2}) "
    r"rawBefore=(?P<raw_before>[0-9A-Fa-f]{16}) "
    r"rawAfter=(?P<raw_after>[0-9A-Fa-f]{16})\b"
)
LAYOUT_COMPLETE_RE = re.compile(
    r"\[netplay\] layout rendezvous complete "
    r"generation=(?P<generation>\d+) "
    r"epoch=(?P<epoch>\d+) "
    r"localGeneration=(?P<local_generation>\d+) "
    r"digest=(?P<digest>[0-9A-Fa-f]{16})\b"
)
AUTOMATCH_LIFECYCLE_RE = re.compile(
    r"\[test-automatch\] segment=(?P<segment>\d+) "
    r"(?P<event>ENTER|EXIT) gameplay frame=(?P<frame>\d+)"
)
AUTOMATCH_ROUND_BEGIN_RE = re.compile(
    r"\[test-automatch\] round=(?P<round>\d+) BEGIN "
    r"frame=(?P<frame>\d+) segment=(?P<segment>\d+) "
    r"counter=(?P<counter>\d+)"
)
AUTOMATCH_INPUT_RE = re.compile(
    r"\[test-automatch\] input frame=(?P<frame>\d+) "
    r"local=P(?P<player>[12]) .*?mask=(?P<mask>[0-9A-Fa-f]{8})\b"
)
LIFECYCLE_DISPATCH_RETURN_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[lifecycle\] dispatch-return frame=(?P<frame>\d+) "
    r"result=(?P<result>-?\d+) elapsedMs=(?P<elapsed>\d+) "
    r"scene=(?P<scene_before>\d+)->(?P<scene_after>\d+) "
    r"next=(?P<next_before>\d+)->(?P<next_after>\d+) "
    r"matchSub=(?P<match_before>-?\d+)->(?P<match_after>-?\d+) "
    r"scene3Sub=(?P<scene3_before>-?\d+)->(?P<scene3_after>-?\d+)",
    re.MULTILINE,
)
GAMEPLAY_ENTRY_RE = re.compile(
    r"\[lifecycle\] dispatch-return frame=(?P<frame>\d+) .*? gameplay=0->1\b"
)
WINDOW_TIMELINE_RE = re.compile(
    r"\| f L(?P<local>\d+)/R(?P<remote>\d+) A(?P<ack>\d+)"
)
PACING_WORLD_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[pacing-world\] frame=(?P<frame>\d+) "
    r"elapsedUs=(?P<elapsed_us>\d+) visible=(?P<visible>\d+) "
    r"visibleFps=(?P<visible_fps>\d+\.\d{2}) "
    r"gameplay=(?P<gameplay>\d+) "
    r"nativeAdvance=(?P<native_advance>\d+) "
    r"nativeMiss=(?P<native_miss>\d+) "
    r"nativeFps=(?P<native_fps>\d+\.\d{2}) "
    r"qohFps=(?P<qoh_fps>\d+) qohCounter=(?P<qoh_counter>\d+) "
    r"intervalUs=(?P<interval_min>\d+)/(?P<interval_p95>\d+)/"
    r"(?P<interval_max>\d+) burst=(?P<burst>\d+) "
    r"late=(?P<late>\d+) driverCalls=(?P<driver_calls>\d+) "
    r"stalls=(?P<stalls>\d+) exactWaits=(?P<exact_waits>\d+) "
    r"deadlineWaits=(?P<deadline_waits>\d+) "
    r"spacingWaits=(?P<spacing_waits>\d+) "
    r"deadlineRebases=(?P<deadline_rebases>\d+) "
    r"paceDebtUs=(?P<pace_debt_us>\d+) "
    r"rollbacks=(?P<rollbacks>\d+) "
    r"replayFrames=(?P<replay_frames>\d+) "
    r"correctionBatches=(?P<correction_batches>\d+) "
    r"correctionWorkUs=(?P<correction_work_us>\d+) "
    r"scene=(?P<scene>\d+) matchSub=(?P<match_sub>-?\d+) "
    r"result=(?P<result>\d+) round=(?P<round>\d+) "
    r"finishTimers=(?P<finish_timer_p1>-?\d+)/"
    r"(?P<finish_timer_p2>-?\d+) "
    r"P1=\[x:(?P<p1_x>-?\d+) y:(?P<p1_y>-?\d+) "
    r"action:(?P<p1_action>\d+) "
    r"anim:(?P<p1_anim>\d+)/(?P<p1_anim_frame>\d+) "
    r"left:(?P<p1_left>\d+) life:(?P<p1_life>-?\d+)\] "
    r"P2=\[x:(?P<p2_x>-?\d+) y:(?P<p2_y>-?\d+) "
    r"action:(?P<p2_action>\d+) "
    r"anim:(?P<p2_anim>\d+)/(?P<p2_anim_frame>\d+) "
    r"left:(?P<p2_left>\d+) life:(?P<p2_life>-?\d+)\] "
    r"predDepth=(?P<prediction_depth>\d+) "
    r"maxRb=(?P<max_rollback>\d+) "
    r"debt=(?P<cadence_debt>\d+) "
    r"hiddenFwd=(?P<hidden_forward_ticks>\d+) "
    r"catchupBatches=(?P<catchup_batches>\d+) "
    r"peerAdvRaw=(?P<peer_adv_raw>-?\d+) "
    r"peerAdvEst=(?P<peer_adv_est>-?\d+) "
    r"peerFresh=(?P<peer_fresh>[01]) "
    r"faHolds=(?P<frame_advantage_holds>\d+)$",
    re.MULTILINE,
)
PACING_PRESENT_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[pacing-present\] frame=(?P<frame>\d+) "
    r"elapsedUs=(?P<elapsed_us>\d+) visible=(?P<visible>\d+) "
    r"visibleFps=(?P<visible_fps>\d+(?:\.\d+)?) "
    r"gameplay=(?P<gameplay>\d+) "
    r"intervalUs=(?P<interval_min>\d+)/(?P<interval_p95>\d+)/"
    r"(?P<interval_max>\d+) burst=(?P<burst>\d+) "
    r"late=(?P<late>\d+)$",
    re.MULTILINE,
)
PACING_LEDGER_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[pacing-ledger\] "
    r"commitGrants=(?P<commit_grants>\d+) "
    r"noCommitFallbackGrants=(?P<no_commit_fallback_grants>\d+) "
    r"spectatorBatchFallbackGrants="
    r"(?P<spectator_batch_fallback_grants>\d+) "
    r"duplicateCommitCallbacks=(?P<duplicate_commit_callbacks>\d+) "
    r"duplicatePublishCallbacks=(?P<duplicate_publish_callbacks>\d+) "
    r"hiddenCommitBypasses=(?P<hidden_commit_bypasses>\d+) "
    r"unownedCommitCallbacks=(?P<unowned_commit_callbacks>\d+) "
    r"totalGrants=(?P<total_grants>\d+) "
    r"waitCallbacks=(?P<wait_callbacks>\d+) "
    r"publishCallbacks=(?P<publish_callbacks>\d+) "
    r"publishFailures=(?P<publish_failures>\d+) "
    r"clockFaults=(?P<clock_faults>\d+)$",
    re.MULTILINE,
)
PACING_PUBLISH_FAILURES_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[pacing-publish-failures\] "
    r"invalid=(?P<invalid>\d+) "
    r"noTarget=(?P<no_target>\d+) "
    r"noSource=(?P<no_source>\d+) "
    r"emptyClient=(?P<empty_client>\d+) "
    r"getDc=(?P<get_dc>\d+) "
    r"gdi=(?P<gdi>\d+) "
    r"disposition=(?P<disposition>\d+)$",
    re.MULTILINE,
)
PRESENTATION_WRAPPER_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[presentation-wrapper\] "
    r"normalVisiblePresents=(?P<normal>\d+) "
    r"suppressedPresents=(?P<suppressed>\d+) "
    r"correctedFinalPresents=(?P<corrected>\d+) "
    r"dispositionQueries=(?P<queries>\d+)$",
    re.MULTILINE,
)
PRESENTATION_GATE_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[presentation-gate\] hidden tick verified "
    r"frame=(?P<frame>\d+) queryBefore=(?P<query_before>\d+) "
    r"queryAfter=(?P<query_after>\d+) verified=(?P<verified>\d+) "
    r"wrapper normalVisiblePresents=(?P<normal>\d+) "
    r"suppressedPresents=(?P<suppressed>\d+) "
    r"correctedFinalPresents=(?P<corrected>\d+)$",
    re.MULTILINE,
)
PRESENTATION_GATE_NOTHING_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[presentation-gate\] hidden tick drew nothing "
    r"frame=(?P<frame>\d+) \(no disposition query; gate already proven "
    r"this session, verified=(?P<verified>\d+) "
    r"missing=(?P<missing>\d+)\) . legal non-rendering lifecycle tick, "
    r"nothing could leak$",
    re.MULTILINE,
)
PRESENTATION_GRACEFUL_SUMMARY_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[presentation-summary\] "
    r"hiddenReplayNativeTicks=(?P<hidden>\d+) "
    r"correctedFinalNativeTicks=(?P<corrected_native>\d+) "
    r"hiddenForwardNativeTicks=(?P<hidden_forward>\d+) "
    r"verifiedHiddenPresentationTicks=(?P<verified>\d+) "
    r"missingPresentationGateTicks=(?P<missing>\d+) "
    r"dispositionQueries=(?P<queries>\d+) "
    r"wrapper normalVisiblePresents=(?P<normal>\d+) "
    r"suppressedPresents=(?P<suppressed>\d+) "
    r"correctedFinalPresents=(?P<corrected>\d+)$",
    re.MULTILINE,
)
PRESENTATION_SESSION_SUMMARY_RE = re.compile(
    r"^\[(?P<clock>\d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[hooks\] removed \(selftest totals: ok=(?P<ok>\d+) "
    r"bad=(?P<bad>\d+), audioSuppressed=(?P<audio>\d+) "
    r"hiddenReplayNativeTicks=(?P<hidden>\d+) "
    r"correctedFinalNativeTicks=(?P<corrected_native>\d+) "
    r"verifiedHiddenPresentationTicks=(?P<verified>\d+) "
    r"missingPresentationGateTicks=(?P<missing>\d+) "
    r"dispositionQueries=(?P<queries>\d+) "
    r"wrapper normalVisiblePresents=(?P<normal>\d+) "
    r"suppressedPresents=(?P<suppressed>\d+) "
    r"correctedFinalPresents=(?P<corrected>\d+)\)$",
    re.MULTILINE,
)
TEST_DELAY_REQUEST_RE = re.compile(
    r"\[test-delay\] REQUEST local=P(?P<player>[12]) "
    r"gameplayTick=(?P<tick>\d+) frame=(?P<frame>\d+) "
    r"target=(?P<delay>\d+) step=(?P<step>\d+)/(?P<total>\d+)"
)
TEST_DELAY_APPLIED_RE = re.compile(
    r"\[test-delay\] APPLIED local=P(?P<player>[12]) "
    r"gameplayTick=(?P<tick>\d+) frame=(?P<frame>\d+) "
    r"delay=(?P<delay>\d+)"
)
TEST_NET_ENABLED_RE = re.compile(
    r"\[test-net\] ENABLED profile=(?P<profile>[a-z0-9-]+) "
    r"seed=(?P<seed>[0-9A-Fa-f]{8}) "
    r"role=(?P<role>host|client|viewer)\b"
)
TEST_NET_STATS_RE = re.compile(
    r"\[test-net\] STATS profile=(?P<profile>[a-z0-9-]+) "
    r"logical=(?P<logical>\d+) queued=(?P<queued>\d+) "
    r"sent=(?P<sent>\d+) dropped=(?P<dropped>\d+) "
    r"duplicated=(?P<duplicated>\d+) "
    r"reordered=(?P<reordered>\d+) pending=(?P<pending>\d+) "
    r"queueOverflows=(?P<overflows>\d+) lanes=(?P<lanes>\d+)"
)
TEST_NET_LANE_STATS_RE = re.compile(
    r"\[test-net\] LANE-STATS domain="
    r"(?P<domain>player|spectator-source|spectator-viewer) "
    r"key=(?P<key>[0-9A-Fa-f]{16}) seed=(?P<seed>[0-9A-Fa-f]{8}) "
    r"logical=(?P<logical>\d+) queued=(?P<queued>\d+) "
    r"sent=(?P<sent>\d+) dropped=(?P<dropped>\d+) "
    r"duplicated=(?P<duplicated>\d+) "
    r"reordered=(?P<reordered>\d+) pending=(?P<pending>\d+) "
    r"queueOverflows=(?P<overflows>\d+)"
)
CONFIRMED_HASH_MATCH_RE = re.compile(
    r"\[netplay\] confirmed hash MATCH count=(?P<count>\d+) "
    r"layout=(?P<layout>\d+) epoch=(?P<epoch>\d+) "
    r"frame=(?P<frame>\d+) hash=(?P<hash>[0-9A-Fa-f]{16})\b"
)
TERMINAL_CODE_RE = re.compile(
    r"\[netplay\] TERMINAL code=(?P<code>\d+) "
    r"detail=(?P<detail>\d+) frame=(?P<frame>\d+):"
)

CANONICAL_TARGET_FPS = 60.6

CHARSELECT_TRANSITION = {
    "p1_char": 0,
    "p2_char": 0,
    "p1_confirm": 4,
    "p2_confirm": 4,
    "match_start": 1,
    "scene_next": 2,
    "game_mode": 0,
    "vs_setup": 0,
}

REQUIRED_DLL_MARKERS = (
    "CHARSELECT reached",
    "[netplay] GO:",
    "[hooks] rollback online",
    "[netplay] layout rendezvous complete",
)

SPECTATOR_REQUIRED_DLL_MARKERS = (
    "[spectator-viewer] READY",
    "[spectator] PLAY",
    "[spectator] playback buffer primed",
    "[spectator] gameplay-entry rendezvous complete",
)

SPECTATOR_SOURCE_REQUIRED_DLL_MARKERS = (
    "[spectator-source] service mode=2",
    "[netlink] spectator service = SIDECAR",
    "[spectator.publish] armed",
    "[spectator-sidecar] start published",
)

CASTER_SPECTATOR_ACCESS_READY_RE = re.compile(
    r"^\[spectator-contract\] v=1 state=access-ready "
    r"transport=direct-udp receiveOnly=1 port=(?P<port>\d+) "
    r"session=(?P<session>[1-9]\d*)\s*$",
    re.MULTILINE,
)
SPECTATOR_PREFLIGHT_RE = re.compile(
    r"\[spectate\] preflight ok=(?P<ok>[01]) "
    r"failure=(?P<failure>\d+) inconclusive=(?P<inconclusive>[01]) "
    r"route=(?P<route>[a-z0-9-]+)\b"
)
SPECTATOR_PROVED_SOCKET_RE = re.compile(
    r"\[spectate\] adopted the proved viewer socket, local port "
    r"(?P<port>\d+)\b"
)
SPECTATOR_VIEWER_COUNT_RE = re.compile(
    r"\[spectator-contract\] v=(?P<version>[12]) state=viewer-count "
    r"count=(?P<count>\d+) capacity=(?P<capacity>\d+)\b"
)
SPECTATOR_HOST_RELEASE_RE = re.compile(
    r"\[spectator-admission\] release acknowledged "
    r"viewer=(?P<viewer>#[0-9A-Fa-f]+) present=(?P<present>[01])\b"
)
SPECTATOR_CASTER_RELEASE_RE = re.compile(
    r"\[spectator-admission\] viewer release after caster lifecycle "
    r"viewer=(?P<viewer>#[0-9A-Fa-f]+) "
    r"acknowledged=(?P<acknowledged>[01]) detail='(?P<detail>[^']*)'"
)
SPECTATOR_AUTHENTICATED_RECORDS_RE = re.compile(
    r"\[spectator\] playback buffer primed records="
    r"(?P<records>\d+)\b"
)
SPECTATOR_CATCHUP_RE = re.compile(
    r"\[spectator\] catch-up receiveBacklog=(?P<receive_backlog>\d+) "
    r"playbackBacklog=(?P<playback_backlog>\d+) "
    r"tickBudget=(?P<tick_budget>\d+) executed=(?P<executed>\d+) "
    r"sliceTruncated=(?P<slice_truncated>[01])\b"
)
SPECTATOR_CATCHUP_CONVERGED_RE = re.compile(
    r"\[spectator\] converged to live target after "
    r"(?P<elapsed_ms>\d+)ms \(playbackBacklog="
    r"(?P<playback_backlog>\d+) liveTarget=(?P<live_target>\d+)"
)
SPECTATOR_ARCHIVE_COMPLETE_RE = re.compile(
    r"\[spectator\] archive complete records=(?P<records>\d+) "
    r"reason=(?P<reason>\d+) finalHash="
    r"(?P<hash>[0-9A-Fa-f]{16})\b"
)
SPECTATOR_PUBLISH_SEALED_RE = re.compile(
    r"\[spectator\.publish\] sealed reason=(?P<reason>\d+) "
    r"cursor=(?P<cursor>\d+) prefix=(?P<prefix>[0-9A-Fa-f]{16}) "
    r"postTail=(?P<hash>[0-9A-Fa-f]{16}) "
    r"published=(?P<published>\d+) written=(?P<written>\d+) "
    r"highWater=(?P<high_water>\d+)\b"
)
SPECTATOR_ROUTE_FORCE_RE = re.compile(
    r"\[spectate-route-force\] mode=(?P<mode>auto|direct|punch|relay) "
    r"directRaceMs=(?P<direct_race_ms>\d+) "
    r"punch=(?P<punch>[01]) relay=(?P<relay>[01])\b"
)
SPECTATOR_E0_SOURCE_STATUS_RE = re.compile(
    r"\[spectator-e0\] source event=status phase=(?P<phase>[a-z-]+) "
    r"serving=(?P<serving>[01]) confirmed_cursor=(?P<confirmed>\d+) "
    r"first_available_cursor=(?P<first_available>\d+) "
    r"viewers=(?P<viewers>\d+) capacity=(?P<capacity>\d+)\b"
)
SPECTATOR_E0_VIEWER_STATUS_RE = re.compile(
    r"\[spectator-e0\] qsp event=(?P<event>[a-z-]+).*?"
    r"viewer=(?P<viewer>[0-9A-F]{16}) "
    r"generation=(?P<generation>\d+) "
    r"route_label=(?P<route>[a-z-]+) "
    r"route_label_id=(?P<route_id>\d+) "
    r"endpoint=(?P<endpoint>[^\s]+) "
    r"endpoint_provenance=(?P<provenance>[^\s]+) "
    r"state=(?P<state>[a-z-]+) "
    r"bootstrap_cursor=(?P<bootstrap>\d+) "
    r"ack_cursor=(?P<ack>\d+) "
    r"backlog_records=(?P<backlog>\d+) "
    r"bytes_sent=(?P<bytes_sent>\d+)\b"
)
SPECTATOR_E0_VIEWER_NOT_ACTIVE_RE = re.compile(
    r"\[spectator-e0\] qsp event=viewer-not-active "
    r"disconnect_evidence=terminal-outcome .*?"
    r"viewer=(?P<viewer>[0-9A-F]{16}) "
    r"generation=(?P<generation>\d+) .*?"
    r"disconnect_reason=(?P<reason>[a-z-]+) "
    r"terminal_ack_cursor=(?P<ack>\d+)\b"
)
SPECTATOR_RUNTIME_HANDOFF_RE = re.compile(
    r"\[spectator-viewer\] handoff viewer=(?P<viewer>[0-9A-Fa-f]{16}) "
    r"game=[0-9A-Fa-f]{16} gen=(?P<generation>\d+) "
    r"resume=(?P<resume>\d+)\b"
)
SPECTATOR_RETENTION_ENTER_RE = re.compile(
    r"\[spectator-retention\] enter "
    r"remainingMs=(?P<remaining_ms>\d+) "
    r"viewers=(?P<viewers>\d+) phase=(?P<phase>[A-Za-z0-9_-]+)\b"
)
SPECTATOR_RETENTION_LATE_ADMISSION_RE = re.compile(
    r"\[spectator-retention\] late-admission "
    r"viewers=(?P<viewers>\d+) capacity=(?P<capacity>\d+) "
    r"remainingMs=(?P<remaining_ms>\d+) "
    r"phase=(?P<phase>[A-Za-z0-9_-]+)\b"
)
SPECTATOR_RETENTION_EXPIRY_RE = re.compile(
    r"\[spectator-retention\] expiry remainingMs=0 "
    r"viewers=(?P<viewers>\d+)\b"
)
SPECTATOR_RETENTION_STOP_RE = re.compile(
    r"\[spectator-retention\] stop ok=(?P<ok>[01])\b"
)

CHARS_ENTER_RE = re.compile(
    r"\[test-automatch\] segment=(\d+) ENTER gameplay frame=\d+ "
    r"epoch=\d+ replay=\d+ chars=\[(\d+),(\d+)\]"
)
CASTER_SPECTATOR_LAUNCH_CONTRACT_RE = re.compile(
    r"\[spectator-contract\] v=(?P<version>1) state=launch-ready "
    r"receiveOnly=1 session=(?P<session>\d+) pid=(?P<pid>\d+) "
    r"hiddenDesktop=(?P<hidden_desktop>[01])\b"
)
CASTER_ROUTE_CONTRACT_RE = re.compile(
    r"\[route-contract\] v=(?P<version>1) "
    r"role=(?P<role>host|client) state=committed "
    r"session=(?P<session>\d+) "
    r"route=(?P<route>punched-direct|relay)\b"
)
CASTER_ROUTE_FALLBACK_CONTRACT_RE = re.compile(
    r"\[route-fallback-contract\] v=(?P<version>1) "
    r"role=client from=direct to=automatic-relay "
    r"stage=(?P<stage>tcp-control|udp-rendezvous) "
    r"reason=(?P<reason>connect-failed|validation-failed) "
    r"error=(?P<error>\d+) room=(?P<room>[0-9A-Fa-f]{32})\b"
)
CASTER_ROUTE_STATS_RE = re.compile(
    r"\[route-stats\] role=(?P<role>host|client|race) "
    r"phase=(?P<phase>\w+) routeId=(?P<route_id>\d+) "
    r"label=(?P<label>.+?) "
    r"p50us=(?P<p50>\d+) p95us=(?P<p95>\d+) p99us=(?P<p99>\d+) "
    r"jitterus=(?P<jitter>\d+) lossPermille=(?P<loss>\d+) "
    r"samples=(?P<samples>\d+) sent=(?P<sent>\d+)"
)
CASTER_ROUTE_LABEL_RE = re.compile(
    r"\[route-label\] role=(?P<role>host|client) session=(?P<session>\d+) "
    r"kind=(?P<kind>punched-direct|relay) label=(?P<label>.+?) "
    r"routeId=(?P<route_id>\d+)\b"
)
CASTER_ROUTE_RECOMMEND_RE = re.compile(
    r"\[route-recommend\] role=(?P<role>host|client) "
    r"evidence p95us=(?P<p95us>\d+) p95ms=(?P<p95ms>\d+) "
    r"jitterus=(?P<jitter>\d+) lossPermille=(?P<loss>\d+) "
    r"samples=(?P<samples>\d+) complete=(?P<complete>[01]) \| "
    r"required host=(?P<req_host>\d+) client=(?P<req_client>\d+) \| "
    r"current hostD(?P<host_d>\d+)/R(?P<host_r>\d+) "
    r"clientD(?P<client_d>\d+)/R(?P<client_r>\d+) "
    r"budget host=(?P<budget_host>\d+) client=(?P<budget_client>\d+) "
    r"class host=(?P<host_class>[a-z-]+) client=(?P<client_class>[a-z-]+)"
)
DLL_ROUTE_RUNTIME_CLASS_RE = re.compile(
    r"\[route-runtime-class\] p95ms=(?P<p95ms>\d+) "
    r"required host=(?P<req_host>\d+) client=(?P<req_client>\d+) "
    r"budget host=(?P<budget_host>\d+) client=(?P<budget_client>\d+) "
    r"class host=(?P<host_class>[a-z-]+) client=(?P<client_class>[a-z-]+)"
)
DLL_ROUTE_SAMPLE_RE = re.compile(
    r"\[route-sample\] label=(?P<label>.+?) "
    r"p50us=(?P<p50>\d+) p95us=(?P<p95>\d+) p99us=(?P<p99>\d+) "
    r"jitterus=(?P<jitter>\d+) lossPermille=(?P<loss>\d+) "
    r"samples=(?P<samples>\d+) sent=(?P<sent>\d+) "
    r"answered=(?P<answered>\d+) drops=(?P<drops>\d+) \| "
    r"prelaunch p50us=(?P<pre_p50>\d+) p95us=(?P<pre_p95>\d+) "
    r"jitterus=(?P<pre_jitter>\d+) lossPermille=(?P<pre_loss>\d+) "
    r"samples=(?P<pre_samples>\d+)"
)

FORBIDDEN_MARKERS = (
    "[netplay] TERMINAL",
    "CONFIRMED DESYNC",
    "layout rendezvous mismatch",
    "post-load gameplay layout/content differs",
    "post-load layout rendezvous timed out",
    "canonical gameplay state changed during layout rendezvous",
    "[CRASH]",
    " FAULTED",
)

class WindowsProcessControl:

    creation_flags = CREATE_NO_WINDOW

    @staticmethod
    def stop(
        process: subprocess.Popen[bytes],
        terminate_timeout: float = 3.0,
        kill_timeout: float = 2.0,
    ) -> None:
        if process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=terminate_timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=kill_timeout)

def relay_service_command(
    relay_caster: str, port: int, bind: str = "127.0.0.1"
) -> list[str]:
    return [
        relay_caster,
        "relay",
        "--bind",
        bind,
        "--port",
        str(port),
        "--verbose",
    ]

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)

def win_error(label: str) -> OSError:
    return ctypes.WinError(ctypes.get_last_error(), label)

class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]

class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.POINTER(wintypes.BYTE)),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", wintypes.HMODULE),
        ("szModule", wintypes.WCHAR * 256),
        ("szExePath", wintypes.WCHAR * 260),
    ]

class WOW64_FLOATING_SAVE_AREA(ctypes.Structure):
    _fields_ = [
        ("ControlWord", wintypes.DWORD),
        ("StatusWord", wintypes.DWORD),
        ("TagWord", wintypes.DWORD),
        ("ErrorOffset", wintypes.DWORD),
        ("ErrorSelector", wintypes.DWORD),
        ("DataOffset", wintypes.DWORD),
        ("DataSelector", wintypes.DWORD),
        ("RegisterArea", wintypes.BYTE * 80),
        ("Cr0NpxState", wintypes.DWORD),
    ]

class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [
        ("ContextFlags", wintypes.DWORD),
        ("Dr0", wintypes.DWORD),
        ("Dr1", wintypes.DWORD),
        ("Dr2", wintypes.DWORD),
        ("Dr3", wintypes.DWORD),
        ("Dr6", wintypes.DWORD),
        ("Dr7", wintypes.DWORD),
        ("FloatSave", WOW64_FLOATING_SAVE_AREA),
        ("SegGs", wintypes.DWORD),
        ("SegFs", wintypes.DWORD),
        ("SegEs", wintypes.DWORD),
        ("SegDs", wintypes.DWORD),
        ("Edi", wintypes.DWORD),
        ("Esi", wintypes.DWORD),
        ("Ebx", wintypes.DWORD),
        ("Edx", wintypes.DWORD),
        ("Ecx", wintypes.DWORD),
        ("Eax", wintypes.DWORD),
        ("Ebp", wintypes.DWORD),
        ("Eip", wintypes.DWORD),
        ("SegCs", wintypes.DWORD),
        ("EFlags", wintypes.DWORD),
        ("Esp", wintypes.DWORD),
        ("SegSs", wintypes.DWORD),
        ("ExtendedRegisters", wintypes.BYTE * 512),
    ]

class EXCEPTION_RECORD(ctypes.Structure):
    pass

EXCEPTION_RECORD._fields_ = [
    ("ExceptionCode", wintypes.DWORD),
    ("ExceptionFlags", wintypes.DWORD),
    ("ExceptionRecord", ctypes.POINTER(EXCEPTION_RECORD)),
    ("ExceptionAddress", ctypes.c_void_p),
    ("NumberParameters", wintypes.DWORD),
    ("ExceptionInformation", ctypes.c_size_t * 15),
]

class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [
        ("ExceptionRecord", EXCEPTION_RECORD),
        ("dwFirstChance", wintypes.DWORD),
    ]

class DEBUG_EVENT_UNION(ctypes.Union):
    _fields_ = [
        ("Exception", EXCEPTION_DEBUG_INFO),
        ("raw", wintypes.BYTE * 160),
    ]

class DEBUG_EVENT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("dwDebugEventCode", wintypes.DWORD),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
        ("u", DEBUG_EVENT_UNION),
    ]

kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.Process32FirstW.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(PROCESSENTRY32W),
]
kernel32.Process32FirstW.restype = wintypes.BOOL
kernel32.Process32NextW.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(PROCESSENTRY32W),
]
kernel32.Process32NextW.restype = wintypes.BOOL
kernel32.Module32FirstW.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(MODULEENTRY32W),
]
kernel32.Module32FirstW.restype = wintypes.BOOL
kernel32.Module32NextW.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(MODULEENTRY32W),
]
kernel32.Module32NextW.restype = wintypes.BOOL
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.GetCurrentProcess.argtypes = []
kernel32.GetCurrentProcess.restype = wintypes.HANDLE
kernel32.GetProcessAffinityMask.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_size_t),
]
kernel32.GetProcessAffinityMask.restype = wintypes.BOOL
kernel32.SetProcessAffinityMask.argtypes = [
    wintypes.HANDLE,
    ctypes.c_size_t,
]
kernel32.SetProcessAffinityMask.restype = wintypes.BOOL
kernel32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenThread.restype = wintypes.HANDLE
kernel32.SuspendThread.argtypes = [wintypes.HANDLE]
kernel32.SuspendThread.restype = wintypes.DWORD
kernel32.ResumeThread.argtypes = [wintypes.HANDLE]
kernel32.ResumeThread.restype = wintypes.DWORD
kernel32.GetProcessIdOfThread.argtypes = [wintypes.HANDLE]
kernel32.GetProcessIdOfThread.restype = wintypes.DWORD
kernel32.ReadProcessMemory.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.WriteProcessMemory.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
kernel32.WriteProcessMemory.restype = wintypes.BOOL
kernel32.GetExitCodeProcess.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(wintypes.DWORD),
]
kernel32.GetExitCodeProcess.restype = wintypes.BOOL
kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel32.TerminateProcess.restype = wintypes.BOOL
kernel32.VirtualProtectEx.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    ctypes.c_size_t,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
]
kernel32.VirtualProtectEx.restype = wintypes.BOOL
kernel32.FlushInstructionCache.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    ctypes.c_size_t,
]
kernel32.FlushInstructionCache.restype = wintypes.BOOL
kernel32.DebugActiveProcess.argtypes = [wintypes.DWORD]
kernel32.DebugActiveProcess.restype = wintypes.BOOL
kernel32.DebugActiveProcessStop.argtypes = [wintypes.DWORD]
kernel32.DebugActiveProcessStop.restype = wintypes.BOOL
kernel32.DebugSetProcessKillOnExit.argtypes = [wintypes.BOOL]
kernel32.DebugSetProcessKillOnExit.restype = wintypes.BOOL
kernel32.WaitForDebugEvent.argtypes = [
    ctypes.POINTER(DEBUG_EVENT),
    wintypes.DWORD,
]
kernel32.WaitForDebugEvent.restype = wintypes.BOOL
kernel32.ContinueDebugEvent.argtypes = [
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
]
kernel32.ContinueDebugEvent.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL

ntdll.NtSuspendProcess.argtypes = [wintypes.HANDLE]
ntdll.NtSuspendProcess.restype = wintypes.LONG
ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
ntdll.NtResumeProcess.restype = wintypes.LONG

if ctypes.sizeof(ctypes.c_void_p) == 8:
    _get_thread_context = kernel32.Wow64GetThreadContext
    _set_thread_context = kernel32.Wow64SetThreadContext
else:
    _get_thread_context = kernel32.GetThreadContext
    _set_thread_context = kernel32.SetThreadContext
_get_thread_context.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(WOW64_CONTEXT),
]
_get_thread_context.restype = wintypes.BOOL
_set_thread_context.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(WOW64_CONTEXT),
]
_set_thread_context.restype = wintypes.BOOL

user32.OpenDesktopW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.BOOL,
    wintypes.DWORD,
]
user32.OpenDesktopW.restype = wintypes.HANDLE
user32.CloseDesktop.argtypes = [wintypes.HANDLE]
user32.CloseDesktop.restype = wintypes.BOOL
user32.SetThreadDesktop.argtypes = [wintypes.HANDLE]
user32.SetThreadDesktop.restype = wintypes.BOOL
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumDesktopWindows.argtypes = [
    wintypes.HANDLE,
    WNDENUMPROC,
    wintypes.LPARAM,
]
user32.EnumDesktopWindows.restype = wintypes.BOOL
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.EnumWindows.restype = wintypes.BOOL
user32.GetWindowThreadProcessId.argtypes = [
    wintypes.HWND,
    ctypes.POINTER(wintypes.DWORD),
]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextW.argtypes = [
    wintypes.HWND,
    wintypes.LPWSTR,
    ctypes.c_int,
]
user32.GetWindowTextW.restype = ctypes.c_int
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindow.restype = wintypes.BOOL
user32.PostMessageW.argtypes = [
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.PostMessageW.restype = wintypes.BOOL

class HarnessFailure(RuntimeError):
    pass

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def qoh_selected_profile_snapshot(
    path: Path,
    offsets: tuple[int, ...] = QOH_CONFIG_PRESERVED_OFFSETS,
) -> dict[str, object]:
    image = path.read_bytes()
    if len(image) != QOH_CONFIG_IMAGE_SIZE:
        raise HarnessFailure(
            f"{path} has size {len(image)}, expected {QOH_CONFIG_IMAGE_SIZE}"
        )
    profile = struct.unpack_from("<I", image, 0)[0]
    if profile >= QOH_CONFIG_PROFILE_COUNT:
        raise HarnessFailure(
            f"{path} selects invalid QOH config profile {profile}"
        )
    record = QOH_CONFIG_PROFILE_BASE + profile * QOH_CONFIG_PROFILE_STRIDE
    values = {
        offset: struct.unpack_from("<I", image, record + offset)[0]
        for offset in offsets
    }
    return {
        "profile": profile,
        "record_offset": record,
        "values": values,
        "file_sha256": sha256(path),
        "mtime_ns": path.stat().st_mtime_ns,
    }

def mutate_qoh_selected_profile(
    path: Path,
    profile: int,
    values: dict[int, int],
) -> dict[str, object]:
    image = bytearray(path.read_bytes())
    if len(image) != QOH_CONFIG_IMAGE_SIZE:
        raise HarnessFailure(
            f"{path} has size {len(image)}, expected {QOH_CONFIG_IMAGE_SIZE}"
        )
    if not 0 <= profile < QOH_CONFIG_PROFILE_COUNT:
        raise HarnessFailure(f"invalid QOH config profile {profile}")
    for offset, value in values.items():
        if (
            offset < 0
            or offset % 4 != 0
            or offset + 4 > QOH_CONFIG_PROFILE_STRIDE
        ):
            raise HarnessFailure(
                f"invalid QOH profile DWORD offset {offset}"
            )
        if not 0 <= value <= 0xFFFFFFFF:
            raise HarnessFailure(
                f"invalid QOH profile DWORD value {value} at +{offset}"
            )

    struct.pack_into("<I", image, 0, profile)
    record = QOH_CONFIG_PROFILE_BASE + profile * QOH_CONFIG_PROFILE_STRIDE
    for offset, value in values.items():
        struct.pack_into("<I", image, record + offset, value)
    path.write_bytes(image)

    readback = qoh_selected_profile_snapshot(
        path, tuple(sorted(values))
    )
    if (
        int(readback["profile"]) != profile
        or dict(readback["values"]) != values
    ):
        raise HarnessFailure(
            f"{path} did not retain the requested config-stress mutation"
        )
    return readback

def mute_test_install_audio(install: Path) -> dict[str, object]:
    key = install / "System" / "QOHcnf.key"
    image = bytearray(key.read_bytes())
    if len(image) != QOH_CONFIG_IMAGE_SIZE:
        raise HarnessFailure(
            f"{key} has size {len(image)}, expected {QOH_CONFIG_IMAGE_SIZE}"
        )
    profile = struct.unpack_from("<I", image, 0)[0]
    if profile >= QOH_CONFIG_PROFILE_COUNT:
        raise HarnessFailure(
            f"{key} selects invalid QOH config profile {profile}"
        )
    record = QOH_CONFIG_PROFILE_BASE + profile * QOH_CONFIG_PROFILE_STRIDE
    flag_address = record + QOH_CONFIG_NOSOUND_OFFSET
    flags_before = struct.unpack_from("<I", image, flag_address)[0]
    flags_after = flags_before | QOH_CONFIG_NOSOUND_BIT
    struct.pack_into("<I", image, flag_address, flags_after)
    key.write_bytes(image)

    command_paths = (
        install / "LocalConfig" / "FCommand.txt",
        install / "FCommand.txt",
    )
    if not command_paths[0].is_file():
        raise HarnessFailure(
            f"test audio mute requires {command_paths[0]}"
        )
    false_line = re.compile(
        rb"(?m)^([ \t]*NoSound[ \t]+)FALSE([ \t]*)(\r?)$"
    )
    true_line = re.compile(
        rb"(?m)^[ \t]*NoSound[ \t]+TRUE[ \t]*\r?$"
    )
    changed_commands: list[str] = []
    for path in command_paths:
        if not path.is_file():
            continue
        original = path.read_bytes()
        muted, replacements = false_line.subn(
            lambda match: (
                match.group(1)
                + b"TRUE"
                + match.group(2)
                + match.group(3)
            ),
            original,
        )
        if replacements > 1 or (
            replacements == 0 and not true_line.search(original)
        ):
            raise HarnessFailure(
                f"{path} does not contain one canonical NoSound setting"
            )
        if replacements == 1:
            path.write_bytes(muted)
        if not true_line.search(path.read_bytes()):
            raise HarnessFailure(
                f"{path} did not retain NoSound TRUE"
            )
        changed_commands.append(str(path.relative_to(install)))

    readback = qoh_selected_profile_snapshot(
        key, (QOH_CONFIG_NOSOUND_OFFSET,)
    )
    readback_flags = int(
        dict(readback["values"])[QOH_CONFIG_NOSOUND_OFFSET]
    )
    if (readback_flags & QOH_CONFIG_NOSOUND_BIT) == 0:
        raise HarnessFailure(f"{key} did not retain the native NoSound bit")
    return {
        "scope": "temporary cloned test install only",
        "profile": profile,
        "flags_before": flags_before,
        "flags_after": readback_flags,
        "no_sound_bit": QOH_CONFIG_NOSOUND_BIT,
        "fcommand_files": changed_commands,
        "verified": True,
    }

def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""

def tail(text: str, lines: int = 100) -> str:
    return "\n".join(text.splitlines()[-lines:])

class LocalAutopunchRelay:

    def __init__(
        self, bind: str = "127.0.0.1", advertise: str = "127.0.0.1"
    ) -> None:
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((bind, 0))
        self.socket.settimeout(0.05)
        self.bind_address = bind
        self.advertised_address = advertise
        self.port = int(self.socket.getsockname()[1])
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._serve,
            name="qoh99-live-autopunch",
            daemon=True,
        )
        self._registrations: dict[
            tuple[str, int], tuple[tuple[str, int], int, float]
        ] = {}
        self._lock = threading.Lock()
        self._stats = {
            "register_packets": 0,
            "lookup_packets": 0,
            "mapping_packets": 0,
            "lookup_misses": 0,
            "malformed_packets": 0,
            "expired_registrations": 0,
            "capacity_drops": 0,
        }
        self._lookup_events: list[dict[str, object]] = []

    _REGISTRATION_LIFETIME_SECONDS = 30.0
    _MAX_REGISTRATIONS = 256
    _MAX_REGISTRATIONS_PER_IP = 16

    def start(self) -> None:
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self.socket.close()

    def stats(self) -> dict[str, object]:
        with self._lock:
            return {
                **self._stats,
                "port": self.port,
                "registrations": [
                    {
                        "lookup_address": f"{key[0]}:{key[1]}",
                        "observed_address": f"{value[0][0]}:{value[0][1]}",
                        "internal_port": value[1],
                    }
                    for key, value in sorted(self._registrations.items())
                ],
                "lookup_events": list(self._lookup_events),
            }

    @staticmethod
    def _mapping(
        internal_port: int, observed: tuple[str, int]
    ) -> bytes:
        return (
            struct.pack("!HH", internal_port, int(observed[1]))
            + socket.inet_aton(observed[0])
        )

    def _remove_expired_locked(self, now: float) -> None:
        expired = [
            key
            for key, (_observed, _internal, seen) in self._registrations.items()
            if now - seen > self._REGISTRATION_LIFETIME_SECONDS
        ]
        for key in expired:
            del self._registrations[key]
        self._stats["expired_registrations"] += len(expired)

    def _find_registration_locked(
        self, ip: str, port: int
    ) -> tuple[tuple[str, int], int, float] | None:
        observed = self._registrations.get((ip, port))
        if observed is not None:
            return observed
        matches = [
            value
            for (registered_ip, _observed_port), value
            in self._registrations.items()
            if registered_ip == ip and value[1] == port
        ]
        return matches[0] if len(matches) == 1 else None

    def _store_registration_locked(
        self, observed: tuple[str, int], internal_port: int, now: float
    ) -> bool:
        key = (observed[0], int(observed[1]))
        if key not in self._registrations:
            same_ip = sum(
                1 for registered_ip, _port in self._registrations
                if registered_ip == observed[0]
            )
            if (
                len(self._registrations) >= self._MAX_REGISTRATIONS
                or same_ip >= self._MAX_REGISTRATIONS_PER_IP
            ):
                self._stats["capacity_drops"] += 1
                return False
        self._registrations[key] = (observed, internal_port, now)
        return True

    def _resolve_lookup_locked(
        self, requester_observed: tuple[str, int], requester_internal_port: int,
        target_ip: str, target_port: int, now: float
    ) -> tuple[bytes, tuple[str, int], bytes, tuple[str, int]] | None:
        self._remove_expired_locked(now)
        requester = self._registrations.get(requester_observed)
        if requester is None or requester[1] != requester_internal_port:
            return None
        target = self._find_registration_locked(target_ip, target_port)
        if target is None:
            return None
        target_observed, target_internal_port, _target_seen = target
        requester_observed_saved, requester_internal_saved, _requester_seen = (
            requester
        )
        return (
            self._mapping(target_internal_port, target_observed),
            requester_observed_saved,
            self._mapping(requester_internal_saved, requester_observed_saved),
            target_observed,
        )

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                packet, sender = self.socket.recvfrom(64)
            except socket.timeout:
                continue
            except OSError:
                return
            now = time.monotonic()
            if len(packet) == 2:
                internal_port = struct.unpack("!H", packet)[0]
                if internal_port == 0:
                    with self._lock:
                        self._stats["malformed_packets"] += 1
                    continue
                key = (sender[0], int(sender[1]))
                with self._lock:
                    self._remove_expired_locked(now)
                    if self._store_registration_locked(
                        (sender[0], int(sender[1])), internal_port, now
                    ):
                        self._stats["register_packets"] += 1
                continue
            if len(packet) != 8:
                with self._lock:
                    self._stats["malformed_packets"] += 1
                continue

            requester_internal_port = struct.unpack("!H", packet[:2])[0]
            target_ip = socket.inet_ntoa(packet[2:6])
            target_port = struct.unpack("!H", packet[6:])[0]
            requester = (sender[0], int(sender[1]))
            with self._lock:
                self._stats["lookup_packets"] += 1
                resolved = self._resolve_lookup_locked(
                    requester, requester_internal_port, target_ip,
                    target_port, now
                )
                if len(self._lookup_events) < 32:
                    self._lookup_events.append(
                        {
                            "requester": f"{requester[0]}:{requester[1]}",
                            "requester_internal_port": requester_internal_port,
                            "target": f"{target_ip}:{target_port}",
                            "resolved": resolved is not None,
                        }
                    )
                if resolved is None:
                    self._stats["lookup_misses"] += 1
                    continue
                requester_mapping, requester_destination, target_mapping, target_destination = resolved
            try:
                self.socket.sendto(
                    requester_mapping, requester_destination,
                )
                self.socket.sendto(
                    target_mapping, target_destination,
                )
            except OSError:
                continue
            with self._lock:
                self._stats["mapping_packets"] += 2

def reserve_relay_port(bind: str = "127.0.0.1") -> int:
    for _ in range(100):
        tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp.bind((bind, 0))
        port = tcp.getsockname()[1]
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            udp.bind((bind, port))
        except OSError:
            tcp.close()
            udp.close()
            continue
        tcp.close()
        udp.close()
        return port
    raise HarnessFailure("could not reserve a TCP+UDP relay port")

def reserve_game_port(bind: str = "127.0.0.1") -> int:
    for port in range(46000, 46998):
        first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            first.bind((bind, port))
        except OSError:
            first.close()
            continue
        first.close()
        return port
    raise HarnessFailure("could not reserve a UDP game/control port")

def process_entries() -> list[tuple[int, int, str]]:
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == INVALID_HANDLE_VALUE:
        raise win_error("CreateToolhelp32Snapshot(process)")
    result: list[tuple[int, int, str]] = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            result.append(
                (
                    int(entry.th32ProcessID),
                    int(entry.th32ParentProcessID),
                    entry.szExeFile,
                )
            )
            ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return result

def child_game_pid(parent_pid: int, image_name: str) -> int | None:
    wanted = image_name.casefold()
    for pid, ppid, image in process_entries():
        if ppid == parent_pid and image.casefold() == wanted:
            return pid
    return None

def module_info(
    pid: int, module_name: str, snapshot_retry_seconds: float = 2.0
) -> tuple[int, int, str] | None:
    deadline = time.monotonic() + max(0.0, snapshot_retry_seconds)
    while True:
        snapshot = kernel32.CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid
        )
        if snapshot != INVALID_HANDLE_VALUE:
            break
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.02)
    wanted = module_name.casefold()
    try:
        entry = MODULEENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Module32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            if entry.szModule.casefold() == wanted:
                base = ctypes.cast(entry.modBaseAddr, ctypes.c_void_p).value
                return int(base or 0), int(entry.modBaseSize), entry.szExePath
            ok = kernel32.Module32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return None

def process_module_map(pid: int) -> list[dict[str, object]]:
    snapshot = kernel32.CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid
    )
    if snapshot == INVALID_HANDLE_VALUE:
        raise win_error(f"CreateToolhelp32Snapshot(modules,{pid})")
    modules: list[dict[str, object]] = []
    try:
        entry = MODULEENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Module32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            base = int(
                ctypes.cast(entry.modBaseAddr, ctypes.c_void_p).value or 0
            )
            size = int(entry.modBaseSize)
            modules.append(
                {
                    "name": entry.szModule,
                    "path": entry.szExePath,
                    "base": base,
                    "size": size,
                }
            )
            ok = kernel32.Module32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    modules.sort(key=lambda module: int(module["base"]))
    return modules

def process_windows(pid: int) -> list[tuple[int, str]]:
    windows: dict[int, str] = {}

    @WNDENUMPROC
    def visit(hwnd: int, _lparam: int) -> bool:
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, len(buf))
        if buf.value:
            windows[int(hwnd)] = buf.value
        return True

    desktop = user32.OpenDesktopW(
        "qoh99np_hidden",
        0,
        False,
        DESKTOP_READOBJECTS | DESKTOP_ENUMERATE,
    )
    if desktop:
        try:
            user32.EnumDesktopWindows(desktop, visit, 0)
        finally:
            user32.CloseDesktop(desktop)
    user32.EnumWindows(visit, 0)
    return sorted(windows.items(), key=lambda item: (item[1], item[0]))

def is_game_window_title(title: str) -> bool:
    return title == "NET QOH99" or re.match(
        r"^QOH99 (?:(?:NETPLAY STOPPED|HOST|CLIENT|SPECTATOR)(?:\s|\(|\b)"
        r"|.{1,20}\(P1\) vs .{1,20}\(P2\)(?:\s|\|))",
        title,
    ) is not None

def post_close_on_named_desktop(
    pid: int, desktop_name: str
) -> list[dict[str, object]]:
    desktop = user32.OpenDesktopW(
        desktop_name,
        0,
        False,
        DESKTOP_READOBJECTS | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS,
    )
    if not desktop:
        raise win_error(f"OpenDesktopW({desktop_name!r})")

    outcome: dict[str, object] = {}

    def worker() -> None:
        try:
            if not user32.SetThreadDesktop(desktop):
                raise win_error(f"SetThreadDesktop({desktop_name!r})")
            windows: dict[int, str] = {}

            @WNDENUMPROC
            def visit(hwnd: int, _lparam: int) -> bool:
                owner = wintypes.DWORD()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                if int(owner.value) != pid:
                    return True
                length = user32.GetWindowTextLengthW(hwnd)
                if length <= 0:
                    return True
                buf = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buf, len(buf))
                if buf.value:
                    windows[int(hwnd)] = buf.value
                return True

            if not user32.EnumWindows(visit, 0):
                raise win_error(
                    f"EnumWindows({desktop_name!r},pid={pid})"
                )
            candidates = [
                item
                for item in sorted(
                    windows.items(), key=lambda item: (item[1], item[0])
                )
                if is_game_window_title(item[1])
            ]
            diagnostics: list[dict[str, object]] = []
            posted = 0
            for hwnd, title in candidates:
                owner = wintypes.DWORD()
                thread_id = user32.GetWindowThreadProcessId(
                    hwnd, ctypes.byref(owner)
                )
                is_window = bool(user32.IsWindow(hwnd))
                ctypes.set_last_error(0)
                posted_ok = bool(
                    is_window
                    and int(owner.value) == pid
                    and user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                )
                error = 0 if posted_ok else ctypes.get_last_error()
                if posted_ok:
                    posted += 1
                diagnostics.append(
                    {
                        "pid": pid,
                        "hwnd": f"0x{hwnd:X}",
                        "title": title,
                        "is_window": is_window,
                        "owner_pid": int(owner.value),
                        "thread_id": int(thread_id),
                        "post": "ok" if posted_ok else "failed",
                        "error": int(error),
                        "desktop": desktop_name,
                    }
                )
            outcome["diagnostics"] = diagnostics
            outcome["posted"] = posted
        except BaseException as exc:
            outcome["error"] = exc

    thread = threading.Thread(
        target=worker,
        name=f"qoh99-close-{desktop_name}-{pid}",
        daemon=True,
    )
    thread.start()
    thread.join(timeout=5.0)
    if thread.is_alive():
        raise HarnessFailure(
            f"hidden-desktop close worker hung for pid={pid}"
        )
    user32.CloseDesktop(desktop)
    error = outcome.get("error")
    if isinstance(error, BaseException):
        raise error
    diagnostics = outcome.get("diagnostics", [])
    if not isinstance(diagnostics, list):
        diagnostics = []
    if int(outcome.get("posted", 0)) == 0:
        raise HarnessFailure(
            f"pid={pid} had no live game window accepting WM_CLOSE on "
            f"desktop {desktop_name!r}; candidates={diagnostics}"
        )
    return diagnostics

def window_titles(pid: int) -> list[str]:
    return sorted({title for _hwnd, title in process_windows(pid)})

def window_thread_process_id(hwnd: int) -> tuple[int, int]:
    owner = wintypes.DWORD()
    thread = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    return int(thread), int(owner.value)

def is_window(hwnd: int) -> bool:
    return bool(user32.IsWindow(hwnd))

def post_close_direct(hwnd: int) -> tuple[bool, int]:
    ctypes.set_last_error(0)
    if user32.PostMessageW(hwnd, WM_CLOSE, 0, 0):
        return True, 0
    return False, int(ctypes.get_last_error())

class ProcessMemory:
    def __init__(
        self, pid: int, request_set_information: bool = False
    ):
        self.pid = pid
        access = PROCESS_ACCESS
        if request_set_information:
            access |= PROCESS_SET_INFORMATION
        self.handle = kernel32.OpenProcess(access, False, pid)
        if not self.handle:
            raise win_error(f"OpenProcess({pid})")
        self.suspended = False

    def close(self) -> None:
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None

    def alive(self) -> bool:
        if not self.handle:
            return False
        code = wintypes.DWORD()
        return bool(
            kernel32.GetExitCodeProcess(self.handle, ctypes.byref(code))
            and code.value == STILL_ACTIVE
        )

    def exit_code(self) -> int:
        if not self.handle:
            raise HarnessFailure(
                f"cannot query exit code for closed process {self.pid}"
            )
        code = wintypes.DWORD()
        if not kernel32.GetExitCodeProcess(self.handle, ctypes.byref(code)):
            raise win_error(f"GetExitCodeProcess({self.pid})")
        return int(code.value)

    def set_affinity(self, mask: int) -> None:
        if not self.handle:
            raise HarnessFailure(
                f"cannot set affinity for closed process {self.pid}"
            )
        if mask <= 0:
            raise HarnessFailure(
                f"invalid zero affinity mask for process {self.pid}"
            )
        if not kernel32.SetProcessAffinityMask(
            self.handle, ctypes.c_size_t(mask)
        ):
            raise win_error(f"SetProcessAffinityMask({self.pid},0x{mask:X})")

    def read(self, address: int, size: int) -> bytes:
        buf = (wintypes.BYTE * size)()
        got = ctypes.c_size_t()
        if not kernel32.ReadProcessMemory(
            self.handle,
            ctypes.c_void_p(address),
            buf,
            size,
            ctypes.byref(got),
        ) or got.value != size:
            raise win_error(f"ReadProcessMemory({self.pid},0x{address:X})")
        return bytes(buf)

    def write(self, address: int, data: bytes) -> None:
        buf = ctypes.create_string_buffer(data)
        wrote = ctypes.c_size_t()
        if not kernel32.WriteProcessMemory(
            self.handle,
            ctypes.c_void_p(address),
            buf,
            len(data),
            ctypes.byref(wrote),
        ) or wrote.value != len(data):
            raise win_error(f"WriteProcessMemory({self.pid},0x{address:X})")

    def read_u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]

    def read_i32(self, address: int) -> int:
        return struct.unpack("<i", self.read(address, 4))[0]

    def write_u32(self, address: int, value: int) -> None:
        self.write(address, struct.pack("<I", value & 0xFFFFFFFF))

    def suspend(self) -> None:
        if self.suspended:
            return
        status = int(ntdll.NtSuspendProcess(self.handle))
        if status < 0:
            raise HarnessFailure(
                f"NtSuspendProcess({self.pid}) failed NTSTATUS=0x{status & 0xFFFFFFFF:08X}"
            )
        self.suspended = True

    def resume(self) -> None:
        if not self.suspended:
            return
        status = int(ntdll.NtResumeProcess(self.handle))
        if status < 0:
            raise HarnessFailure(
                f"NtResumeProcess({self.pid}) failed NTSTATUS=0x{status & 0xFFFFFFFF:08X}"
            )
        self.suspended = False

    def terminate(self) -> None:
        if self.alive():
            kernel32.TerminateProcess(self.handle, 0)

    def read_many(
        self, specs: Iterable[tuple[str, int, str]]
    ) -> dict[str, int]:
        values: dict[str, int] = {}
        for name, address, kind in specs:
            if kind == "i32":
                values[name] = self.read_i32(address)
            else:
                values[name] = self.read_u32(address)
        return values

    def window_titles(self) -> list[str]:
        return window_titles(self.pid)

    def sample_game_state(
        self, specs: Iterable[tuple[str, int, str]]
    ) -> dict[str, object]:
        sample: dict[str, object] = {
            "pid": self.pid,
            "alive": self.alive(),
        }
        if not sample["alive"]:
            sample["titles"] = self.window_titles()
            return sample
        try:
            sample["values"] = self.read_many(specs)
        except OSError as exc:
            sample["read_error"] = str(exc)
        sample["titles"] = self.window_titles()
        return sample

def open_thread_suspended(
    pid: int, tid: int, request_context: bool
) -> "SuspendedThread":
    access = THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION
    if request_context:
        access |= THREAD_GET_CONTEXT
    handle = kernel32.OpenThread(access, False, tid)
    if not handle:
        raise win_error(f"OpenThread({tid})")
    owner = int(kernel32.GetProcessIdOfThread(handle))
    if owner != pid:
        kernel32.CloseHandle(handle)
        if owner == 0:
            raise win_error(f"GetProcessIdOfThread({tid})")
        raise HarnessFailure(
            f"frame thread {tid} belongs to pid={owner}, expected pid={pid}"
        )
    previous = int(kernel32.SuspendThread(handle))
    if previous == 0xFFFFFFFF:
        error = win_error(f"SuspendThread({tid})")
        kernel32.CloseHandle(handle)
        raise error
    suspended = SuspendedThread(pid=pid, tid=tid, handle=int(handle))
    if previous != 0:
        try:
            suspended.resume()
        except Exception:
            pass
        raise HarnessFailure(
            f"frame thread {tid} was already suspended "
            f"(prior count {previous})"
        )
    return suspended

@dataclass
class SuspendedThread:
    pid: int
    tid: int
    handle: int
    active: bool = True

    def resume(self) -> None:
        if not self.active:
            return
        previous = int(kernel32.ResumeThread(self.handle))
        if previous == 0xFFFFFFFF:
            error = win_error(f"ResumeThread({self.tid})")
            kernel32.CloseHandle(self.handle)
            self.handle = 0
            self.active = False
            raise error
        kernel32.CloseHandle(self.handle)
        self.handle = 0
        self.active = False
        if previous != 1:
            raise HarnessFailure(
                f"ResumeThread({self.tid}) observed unexpected prior "
                f"suspend count {previous}"
            )

def resume_suspended_threads(
    threads: Iterable[SuspendedThread],
) -> None:
    first_error: Exception | None = None
    for thread in reversed(tuple(threads)):
        try:
            thread.resume()
        except Exception as exc:
            if first_error is None:
                first_error = exc
    if first_error is not None:
        raise first_error

GAME_STATE_SPECS: tuple[tuple[str, int, str], ...] = tuple(
    (name, address, "i32" if name == "match_sub" else "u32")
    for name, address in GAME_ADDR.items()
)

def game_state(memory: object) -> dict[str, object]:
    sample = memory.sample_game_state(GAME_STATE_SPECS)
    state: dict[str, object] = {
        "pid": sample["pid"],
        "alive": sample["alive"],
    }
    if not state["alive"]:
        state["titles"] = sample["titles"]
        return state
    values = sample.get("values")
    if values is None:
        state["read_error"] = sample["read_error"]
    else:
        values = {name: int(value) for name, value in dict(values).items()}
        state.update(values)
        state["health_p1"] = (
            int(values["life_current_p1"])
            + int(values["life_reserve_p1"])
            - int(values["life_damage_p1"])
        )
        state["health_p2"] = (
            int(values["life_current_p2"])
            + int(values["life_reserve_p2"])
            - int(values["life_damage_p2"])
        )
    state["titles"] = sample["titles"]
    return state

def parse_map_rva(map_path: Path) -> int:
    text = read_text(map_path)
    preferred_match = re.search(
        r"Preferred load address is\s+([0-9A-Fa-f]+)", text
    )
    symbol_match = re.search(
        r"^\s*[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+"
        r"\?netTakeConfirmedHash@qgame@@\S+\s+"
        r"([0-9A-Fa-f]+)\s",
        text,
        re.MULTILINE,
    )
    if not preferred_match or not symbol_match:
        raise HarnessFailure(
            f"{map_path} does not contain Preferred load address and "
            "qgame::netTakeConfirmedHash"
        )
    preferred = int(preferred_match.group(1), 16)
    symbol = int(symbol_match.group(1), 16)
    rva = symbol - preferred
    if rva <= 0 or rva >= 0x10000000:
        raise HarnessFailure(
            f"invalid netTakeConfirmedHash RVA 0x{rva:X} parsed from {map_path}"
        )
    return rva

def parse_map_data_rva(map_path: Path, symbol_fragment: str) -> int:
    text = read_text(map_path)
    preferred_match = re.search(
        r"Preferred load address is\s+([0-9A-Fa-f]+)", text
    )
    symbol_match = re.search(
        r"^\s*[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+\S*"
        + re.escape(symbol_fragment)
        + r"\S*\s+([0-9A-Fa-f]+)\s",
        text,
        re.MULTILINE,
    )
    if not preferred_match or not symbol_match:
        raise HarnessFailure(
            f"{map_path} does not contain data symbol {symbol_fragment!r}"
        )
    preferred = int(preferred_match.group(1), 16)
    symbol = int(symbol_match.group(1), 16)
    rva = symbol - preferred
    if rva <= 0 or rva >= 0x10000000:
        raise HarnessFailure(
            f"invalid {symbol_fragment} RVA 0x{rva:X} parsed from {map_path}"
        )
    return rva

@dataclass
class Breakpoint:
    pid: int
    address: int
    original: bytes
    kind: str
    armed: bool = True

class ConfirmedHashObserver:
    def __init__(
        self,
        processes: dict[int, ProcessMemory],
        dll_name: str,
        take_hash_rva: int,
    ):
        self.processes = processes
        self.dll_name = dll_name
        self.take_hash_rva = take_hash_rva
        self.breakpoints: dict[tuple[int, int], Breakpoint] = {}
        self.pending_rearm: dict[int, tuple[int, int]] = {}
        self.attached: set[int] = set()
        self.successes: dict[int, int] = {pid: 0 for pid in processes}
        self.events = 0
        self.unexpected_exceptions: list[dict[str, int]] = []

    def _write_code(self, pid: int, address: int, data: bytes) -> None:
        process = self.processes[pid]
        old = wintypes.DWORD()
        if not kernel32.VirtualProtectEx(
            process.handle,
            ctypes.c_void_p(address),
            len(data),
            PAGE_EXECUTE_READWRITE,
            ctypes.byref(old),
        ):
            raise win_error(f"VirtualProtectEx({pid},0x{address:X})")
        try:
            process.write(address, data)
            kernel32.FlushInstructionCache(
                process.handle, ctypes.c_void_p(address), len(data)
            )
        finally:
            restored = wintypes.DWORD()
            kernel32.VirtualProtectEx(
                process.handle,
                ctypes.c_void_p(address),
                len(data),
                old.value,
                ctypes.byref(restored),
            )

    def _install(self, pid: int, address: int, kind: str) -> Breakpoint:
        key = (pid, address)
        existing = self.breakpoints.get(key)
        if existing:
            return existing
        original = self.processes[pid].read(address, 1)
        if original == b"\xCC":
            raise HarnessFailure(
                f"unexpected pre-existing INT3 at pid={pid} address=0x{address:08X}"
            )
        self._write_code(pid, address, b"\xCC")
        bp = Breakpoint(pid, address, original, kind, True)
        self.breakpoints[key] = bp
        return bp

    def _restore(self, bp: Breakpoint) -> None:
        if bp.armed:
            self._write_code(bp.pid, bp.address, bp.original)
            bp.armed = False

    def _rearm(self, bp: Breakpoint) -> None:
        if not bp.armed:
            self._write_code(bp.pid, bp.address, b"\xCC")
            bp.armed = True

    def _thread_context(self, tid: int) -> tuple[int, WOW64_CONTEXT]:
        thread = kernel32.OpenThread(THREAD_ACCESS, False, tid)
        if not thread:
            raise win_error(f"OpenThread({tid})")
        context = WOW64_CONTEXT()
        context.ContextFlags = WOW64_CONTEXT_CONTROL_INTEGER
        if not _get_thread_context(thread, ctypes.byref(context)):
            kernel32.CloseHandle(thread)
            raise win_error(f"GetThreadContext({tid})")
        return thread, context

    def attach(self) -> None:
        for pid in self.processes:
            info = None
            attach_deadline = time.monotonic() + 3.0
            while info is None and time.monotonic() < attach_deadline:
                info = module_info(pid, self.dll_name)
                if info is None:
                    time.sleep(0.02)
            if not info:
                raise HarnessFailure(
                    f"{self.dll_name} is not loaded in game pid={pid}"
                )
            base, size, _path = info
            if self.take_hash_rva >= size:
                raise HarnessFailure(
                    f"netTakeConfirmedHash RVA 0x{self.take_hash_rva:X} "
                    f"is outside loaded {self.dll_name} size 0x{size:X}"
                )
            if not kernel32.DebugActiveProcess(pid):
                raise win_error(f"DebugActiveProcess({pid})")
            self.attached.add(pid)
            self._install(pid, base + self.take_hash_rva, "entry")
        if not kernel32.DebugSetProcessKillOnExit(False):
            raise win_error("DebugSetProcessKillOnExit(FALSE)")
        end = time.monotonic() + 1.0
        while time.monotonic() < end:
            if not self.pump(10):
                break

    def pump(self, timeout_ms: int = 0) -> bool:
        event = DEBUG_EVENT()
        ctypes.set_last_error(0)
        if not kernel32.WaitForDebugEvent(ctypes.byref(event), timeout_ms):
            error = ctypes.get_last_error()
            if error in (0, 121):
                return False
            raise win_error("WaitForDebugEvent")

        self.events += 1
        pid = int(event.dwProcessId)
        tid = int(event.dwThreadId)
        continue_status = DBG_CONTINUE
        try:
            if event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT:
                self.attached.discard(pid)
            elif event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT:
                record = event.Exception.ExceptionRecord
                code = int(record.ExceptionCode)
                address = int(record.ExceptionAddress or 0)
                bp = self.breakpoints.get((pid, address))
                is_breakpoint = code in (
                    EXCEPTION_BREAKPOINT,
                    STATUS_WX86_BREAKPOINT,
                )
                is_single_step = code in (
                    EXCEPTION_SINGLE_STEP,
                    STATUS_WX86_SINGLE_STEP,
                )
                if is_breakpoint and bp and bp.armed:
                    self._handle_breakpoint(bp, tid)
                elif (
                    is_single_step
                    and tid in self.pending_rearm
                ):
                    self._handle_single_step(pid, tid)
                elif is_breakpoint:
                    continue_status = DBG_CONTINUE
                else:
                    self.unexpected_exceptions.append(
                        {
                            "pid": pid,
                            "tid": tid,
                            "code": code,
                            "address": address,
                            "first_chance": int(
                                event.Exception.dwFirstChance
                            ),
                        }
                    )
                    continue_status = DBG_EXCEPTION_NOT_HANDLED
        finally:
            if not kernel32.ContinueDebugEvent(pid, tid, continue_status):
                raise win_error(
                    f"ContinueDebugEvent(pid={pid},tid={tid})"
                )
        return True

    def _handle_breakpoint(self, bp: Breakpoint, tid: int) -> None:
        self._restore(bp)
        thread, context = self._thread_context(tid)
        try:
            context.Eip = bp.address
            if bp.kind == "entry":
                return_address = self.processes[bp.pid].read_u32(
                    context.Esp
                )
                self._install(bp.pid, return_address, "return")
            elif bp.kind == "return":
                if context.Eax & 0xFF:
                    self.successes[bp.pid] += 1
            context.EFlags |= TRAP_FLAG
            if not _set_thread_context(thread, ctypes.byref(context)):
                raise win_error(f"SetThreadContext({tid})")
            self.pending_rearm[tid] = (bp.pid, bp.address)
        finally:
            kernel32.CloseHandle(thread)

    def _handle_single_step(self, pid: int, tid: int) -> None:
        pending_pid, address = self.pending_rearm.pop(tid)
        if pending_pid != pid:
            raise HarnessFailure(
                f"single-step pid mismatch: event={pid} pending={pending_pid}"
            )
        bp = self.breakpoints[(pid, address)]
        self._rearm(bp)
        thread, context = self._thread_context(tid)
        try:
            context.EFlags &= ~TRAP_FLAG
            if not _set_thread_context(thread, ctypes.byref(context)):
                raise win_error(f"SetThreadContext(clear TF,{tid})")
        finally:
            kernel32.CloseHandle(thread)

    def detach_all(self) -> None:
        if not self.attached:
            return
        suspended: list[ProcessMemory] = []
        try:
            for pid in list(self.attached):
                process = self.processes[pid]
                if process.alive() and not process.suspended:
                    process.suspend()
                    suspended.append(process)

            while self.pump(0):
                pass

            for bp in list(self.breakpoints.values()):
                try:
                    self._restore(bp)
                except OSError:
                    pass

            for tid, (pid, _address) in list(self.pending_rearm.items()):
                try:
                    thread, context = self._thread_context(tid)
                    try:
                        context.EFlags &= ~TRAP_FLAG
                        _set_thread_context(thread, ctypes.byref(context))
                    finally:
                        kernel32.CloseHandle(thread)
                except OSError:
                    pass
                self.pending_rearm.pop(tid, None)

            for pid in list(self.attached):
                kernel32.DebugActiveProcessStop(pid)
                self.attached.discard(pid)
        finally:
            for process in reversed(suspended):
                try:
                    process.resume()
                except Exception:
                    pass

def clone_game_install(source: Path, destination: Path) -> dict[str, int]:
    if destination.exists():
        raise HarnessFailure(f"refusing to overwrite test install {destination}")
    destination.mkdir(parents=True)
    copied = skipped = 0
    skip_dirs = {"logs", "replays", "__pycache__"}
    skip_names = {
        "qoh99_netplay.dll",
        "qoh99caster.exe",
        "qoh99caster.ini",
        "ddraw.dll",
        "ddraw_wrapper.log",
        "qoherror.txt",
    }

    for root, dirs, files in os.walk(source):
        root_path = Path(root)
        relative = root_path.relative_to(source)
        dirs[:] = [d for d in dirs if d.casefold() not in skip_dirs]
        target_root = destination / relative
        target_root.mkdir(parents=True, exist_ok=True)
        for filename in files:
            source_file = root_path / filename
            folded = filename.casefold()
            if (
                folded in skip_names
                or source_file.suffix.casefold() in {".log", ".dmp"}
            ):
                skipped += 1
                continue
            target_file = target_root / filename
            shutil.copy2(source_file, target_file)
            copied += 1
    (destination / "logs").mkdir(exist_ok=True)
    return {"copied": copied, "skipped": skipped}

NON_CHARACTER_INSTALL_DIRS = {
    "system", "localconfig", "logs", "replays", "licenses", "docs",
    ".cache", "__pycache__",
}

def strip_character_voice_assets(host, install: Path) -> int:
    if not isinstance(host.backend, rolehost.LocalBackend):
        raise HarnessFailure(
            "--strip-character-voices is a local-role lane; the agent has no "
            "verb for it and a remote install must not be mutated in place"
        )
    removed = 0
    for entry in sorted(install.iterdir()):
        if not entry.is_dir() or entry.name.casefold() in (
            NON_CHARACTER_INSTALL_DIRS
        ):
            continue
        for wav in sorted(entry.rglob("*")):
            if wav.is_file() and wav.suffix.casefold() == ".wav":
                wav.unlink()
                removed += 1
    if removed == 0:
        raise HarnessFailure(
            f"--strip-character-voices removed nothing from {install}; the "
            "source install has no character voice lines, so the lane would "
            "assert against an asymmetry that is not there"
        )
    return removed

def path_is_within(candidate: Path, parent: Path) -> bool:
    try:
        candidate.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False

def clone_payload_bytes(source: Path) -> tuple[int, int]:
    skip_dirs = {"logs", "replays", "__pycache__"}
    skip_names = {
        "qoh99_netplay.dll",
        "qoh99caster.exe",
        "qoh99caster.ini",
        "ddraw.dll",
        "ddraw_wrapper.log",
        "qoherror.txt",
    }
    total = 0
    files = 0
    for root, dirs, names in os.walk(source):
        dirs[:] = [d for d in dirs if d.casefold() not in skip_dirs]
        for name in names:
            candidate = Path(root) / name
            if (
                name.casefold() in skip_names
                or candidate.suffix.casefold() in {".log", ".dmp"}
            ):
                continue
            total += candidate.stat().st_size
            files += 1
    return total, files

def write_harness_marker(
    run_root: Path, output_root: Path, role_dirs: list[str]
) -> None:
    resolved_output = output_root.resolve()
    resolved_run = run_root.resolve()
    if resolved_run.parent != resolved_output:
        raise HarnessFailure(
            f"refusing to mark run outside harness root: {resolved_run}"
        )
    if any(not role or Path(role).name != role for role in role_dirs):
        raise HarnessFailure("invalid harness-owned role directory list")
    marker = {
        "schema": HARNESS_OWNERSHIP_SCHEMA,
        "kind": "qoh99-live-game-e2e",
        "output_root": str(resolved_output),
        "run_name": resolved_run.name,
        "role_dirs": sorted(set(role_dirs)),
        "created_utc": datetime.now(timezone.utc).isoformat(),
    }
    (resolved_run / HARNESS_OWNERSHIP_MARKER).write_text(
        json.dumps(marker, indent=2, sort_keys=True), encoding="utf-8"
    )

def load_harness_marker(run_root: Path, output_root: Path) -> dict[str, object] | None:
    resolved_output = output_root.resolve()
    resolved_run = run_root.resolve()
    if resolved_run.parent != resolved_output:
        return None
    marker_path = resolved_run / HARNESS_OWNERSHIP_MARKER
    try:
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(marker, dict):
        return None
    if marker.get("schema") != HARNESS_OWNERSHIP_SCHEMA:
        return None
    if marker.get("kind") != "qoh99-live-game-e2e":
        return None
    if os.path.normcase(str(resolved_output)) != os.path.normcase(
        str(marker.get("output_root", ""))
    ):
        return None
    if marker.get("run_name") != resolved_run.name:
        return None
    roles = marker.get("role_dirs")
    if (
        not isinstance(roles, list)
        or any(
            not isinstance(role, str) or not role
            or Path(role).name != role
            for role in roles
        )
    ):
        return None
    return marker

def owned_clone_paths(run_root: Path, output_root: Path) -> list[Path]:
    marker = load_harness_marker(run_root, output_root)
    if marker is None:
        return []
    roles = marker["role_dirs"]
    assert isinstance(roles, list)
    result: list[Path] = []
    resolved_run = run_root.resolve()
    for role in roles:
        assert isinstance(role, str)
        candidate = run_root / role / "g"
        if not candidate.exists():
            continue
        if (
            not candidate.is_dir()
            or candidate.is_symlink()
            or candidate.resolve().parent.parent != resolved_run
        ):
            continue
        result.append(candidate)
    return result

def remove_owned_game_clones(
    run_root: Path, output_root: Path
) -> dict[str, object]:
    deleted: list[str] = []
    errors: dict[str, str] = {}
    for candidate in owned_clone_paths(run_root, output_root):
        try:
            shutil.rmtree(candidate)
            deleted.append(str(candidate))
        except OSError as error:
            errors[str(candidate)] = f"{type(error).__name__}: {error}"
    return {"deleted": deleted, "errors": errors}

def remove_owned_caster_copies(
    run_root: Path, output_root: Path
) -> dict[str, object]:
    marker = load_harness_marker(run_root, output_root)
    if marker is None:
        return {"deleted": [], "errors": {}}
    roles = marker["role_dirs"]
    assert isinstance(roles, list)
    deleted: list[str] = []
    errors: dict[str, str] = {}
    for role in roles:
        assert isinstance(role, str)
        role_root = run_root / role
        if (
            not role_root.is_dir()
            or role_root.is_symlink()
            or role_root.resolve().parent != run_root.resolve()
        ):
            continue
        for filename in ("qoh99caster.exe", "qoh99caster.ini"):
            candidate = role_root / filename
            if not candidate.exists():
                continue
            if not candidate.is_file() or candidate.is_symlink():
                errors[str(candidate)] = "refusing non-regular owned payload"
                continue
            try:
                candidate.unlink()
                deleted.append(str(candidate))
            except OSError as error:
                errors[str(candidate)] = f"{type(error).__name__}: {error}"
    return {"deleted": deleted, "errors": errors}

def prune_stale_owned_game_clones(
    output_root: Path, keep_full_runs: int, exclude: Path | None = None
) -> dict[str, object]:
    root = output_root.resolve()
    if keep_full_runs < 0:
        raise HarnessFailure("negative retained-workdir limit")
    if not root.is_dir():
        return {"scanned": 0, "pruned": [], "errors": {}}
    excluded = exclude.resolve() if exclude is not None else None
    candidates = [
        child
        for child in root.iterdir()
        if child.is_dir()
        and (excluded is None or child.resolve() != excluded)
        and owned_clone_paths(child, root)
    ]
    candidates.sort(key=lambda item: item.stat().st_mtime_ns, reverse=True)
    pruned: list[str] = []
    errors: dict[str, object] = {}
    for candidate in candidates[keep_full_runs:]:
        result = remove_owned_game_clones(candidate, root)
        pruned.extend(result["deleted"])
        if result["errors"]:
            errors[str(candidate)] = result["errors"]
    return {"scanned": len(candidates), "pruned": pruned, "errors": errors}

def validate_runtime_asset_paths(install: Path) -> dict[str, object]:
    checked = 0
    longest_length = -1
    longest_path = ""
    too_long: list[tuple[int, str]] = []
    for candidate in install.rglob("*"):
        if not candidate.is_file():
            continue
        resolved = str(candidate.resolve(strict=True))
        length = len(resolved)
        checked += 1
        if length > longest_length:
            longest_length = length
            longest_path = resolved
        if length > LEGACY_ASSET_PATH_MAX:
            too_long.append((length, resolved))

    if checked == 0:
        raise HarnessFailure(f"copied game install contains no files: {install}")
    if too_long:
        examples = ", ".join(
            f"{length}:{path}" for length, path in sorted(too_long, reverse=True)[:8]
        )
        raise HarnessFailure(
            f"{len(too_long)} copied runtime asset paths exceed "
            f"{LEGACY_ASSET_PATH_MAX} characters ({examples})"
        )
    return {
        "checked": checked,
        "limit": LEGACY_ASSET_PATH_MAX,
        "longest_length": longest_length,
        "longest_path": longest_path,
    }

LOCAL_HOST = rolehost.LocalRoleHost(
    rolehost.LocalBackend(
        failure=HarnessFailure,
        process_memory=ProcessMemory,
        process_control=WindowsProcessControl,
        child_game_pid=child_game_pid,
        module_info=module_info,
        process_module_map=process_module_map,
        process_windows=process_windows,
        window_titles=window_titles,
        window_thread_process_id=window_thread_process_id,
        is_window=is_window,
        post_close_direct=post_close_direct,
        post_close_on_named_desktop=post_close_on_named_desktop,
        open_thread_suspended=open_thread_suspended,
        sha256=sha256,
        clone_game_install=clone_game_install,
        clone_payload_bytes=clone_payload_bytes,
        validate_runtime_asset_paths=validate_runtime_asset_paths,
        mute_test_install_audio=mute_test_install_audio,
        qoh_selected_profile_snapshot=qoh_selected_profile_snapshot,
        mutate_qoh_selected_profile=mutate_qoh_selected_profile,
        game_state_from_memory=game_state,
    )
)

PLACEMENT_DEFAULTS: dict[str, object] = {
    "lab": (
        "/srv/dev-disk-by-uuid-d1322791-ea4a-4e15-9d9d-402c695927fa"
        "/appdata/qoh99lab"
    ),
    "agent_port": 47311,
    "drive": "Q:",
    "display": ":99",
    "password": None,
    "source_install": None,
}

def _parse_placement(args: argparse.Namespace) -> dict[str, dict[str, object]]:
    placement: dict[str, dict[str, object]] = {}
    for entry in getattr(args, "placement", None) or []:
        parsed = json.loads(entry) if isinstance(entry, str) else entry
        if not isinstance(parsed, dict):
            raise HarnessFailure(
                "--placement must be a JSON object mapping role names to "
                "placement records"
            )
        for role_name, record in parsed.items():
            merged = dict(PLACEMENT_DEFAULTS)
            merged.update(record)
            placement[str(role_name)] = merged
    for entry in getattr(args, "role_host", None) or []:
        if "=" not in entry:
            raise HarnessFailure(
                f"--role-host expects <role>=<ssh-target>, got {entry!r}"
            )
        role_name, target = entry.split("=", 1)
        merged = dict(PLACEMENT_DEFAULTS)
        merged.update(placement.get(role_name.strip(), {}))
        merged["target"] = target.strip()
        placement[role_name.strip()] = merged
    for role_name, record in placement.items():
        if not record.get("target"):
            raise HarnessFailure(
                f"placement for role {role_name!r} has no ssh target"
            )
    return placement

@dataclass
class Role:
    name: str
    root: Path
    install: Path
    game: Path
    caster_exe: Path
    stdout_path: Path
    stderr_path: Path
    process: subprocess.Popen[bytes] | None = None
    stdout_handle: object | None = None
    stderr_handle: object | None = None
    game_memory: ProcessMemory | None = None
    scene_zero_since: float | None = None
    layout_counter: int | None = None
    final_counter: int | None = None
    layout_net_frames: int | None = None
    final_net_frames: int | None = None
    config_before: dict[str, object] | None = None
    dll_base: int | None = None
    host: object = LOCAL_HOST
    mirror: object | None = None
    guard: object | None = None
    caster_alive_hint: bool = False

    def evidence_root(self) -> Path:
        return self.root if self.mirror is None else self.mirror.local_root

    def _require_fresh_evidence(self) -> None:
        if self.guard is not None:
            self.guard.check(self.name, self.mirror, self.caster_alive_hint)

    def latest_log(self, filename: str) -> Path | None:
        logs = sorted(
            (self.evidence_root() / "logs").glob(f"session_*/{filename}"),
            key=lambda p: p.stat().st_mtime_ns,
        )
        return logs[-1] if logs else None

    def archived_log_text(self, filename: str) -> str:
        archives = sorted(
            (self.evidence_root() / "logs").glob("session_*.zip"),
            key=lambda p: p.stat().st_mtime_ns,
        )
        parts: list[str] = []
        for archive in archives:
            try:
                with zipfile.ZipFile(archive) as bundle:
                    parts.append(
                        bundle.read(filename)
                        .decode("utf-8", errors="replace")
                        .replace("\r\n", "\n")
                        .replace("\r", "\n")
                    )
            except (OSError, KeyError, zipfile.BadZipFile):
                continue
        return "\n".join(parts)

    def session_log_text(self, filename: str) -> str:
        path = self.latest_log(filename)
        if path is not None:
            return read_text(path)
        return self.archived_log_text(filename)

    def dll_text(self) -> str:
        self._require_fresh_evidence()
        return self.session_log_text("dll.log")

    def client_text(self) -> str:
        self._require_fresh_evidence()
        return self.session_log_text("client.log")

    def stdout_text(self) -> str:
        self._require_fresh_evidence()
        return read_text(self.evidence_root() / self.stdout_path.name)

    def stderr_text(self) -> str:
        return read_text(self.evidence_root() / self.stderr_path.name)

class LiveGameHarness:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        route_tag = "p" if args.route == "punched-direct" else "r"
        variant_tag = {
            "identical": "i",
            "bak": "b",
            "config-stress": "x",
        }[args.config_variant]
        mutation_tag = {
            "none": "",
            "inactive-p4": "4",
            "active-p2": "2",
        }[args.layout_command_mutation]
        spectator_tag = (
            f"s{args.spectator_count}"
            if args.spectator
            else (
                "u"
                if args.spectator_negative.startswith("unavailable")
                else ""
            )
        )
        if args.spectator_capacity_readmit:
            spectator_tag += "a"
        if args.spectator_active_catchup_pause_seconds:
            spectator_tag += "g"
        if args.spectator_sibling_disconnect:
            spectator_tag += "x"
        spectator_route_tag = {
            "auto": "",
            "direct": "d",
            "punch": "p",
            "relay": "r",
        }[args.spectator_route]
        if args.spectator_routes:
            spectator_route_tag += "m"
        automatch_tag = (
            "d"
            if args.returned_charselect_idle_ticks
            else ("m" if args.full_match_rematch else "")
        )
        network_tag = {
            "none": "",
            "stable-low": "l",
            "stable-120": "t",
            "stable-high": "h",
            "stable-250": "H",
            "stable-300": "G",
            "jitter-low": "j",
            "jitter-high": "J",
            "packet-loss": "p",
            "asym-updown": "u",
            "asym-downup": "n",
        }[args.network_profile]
        nonce = int(time.time() * 1000) & 0xFFFFFF
        run_tag = (
            f"{route_tag}{variant_tag}{mutation_tag}{spectator_tag}"
            f"{spectator_route_tag}"
            f"{automatch_tag}{network_tag}_"
            f"{os.getpid():x}_{nonce:x}"
        )
        self.output_root = Path(args.output_root).resolve()
        self.run_root = self.output_root / run_tag
        self._owned_role_dirs = ["h", "c"]
        if args.spectator:
            self._owned_role_dirs.extend(
                "s" if index == 0 else f"v{index + 1}"
                for index in range(args.spectator_count)
            )
        if args.spectator_capacity_readmit:
            self._owned_role_dirs.extend(("d", "x"))
        self._source_install: Path | None = None
        self._artifact_finalized = False
        self.config_persistence_offsets = (
            QOH_CONFIG_STRESS_OFFSETS
            if args.config_variant == "config-stress"
            else QOH_CONFIG_PRESERVED_OFFSETS
        )
        self.roles: dict[str, Role] = {}
        self.spectator: Role | None = None
        self.spectators: list[Role] = []
        self.auxiliary_roles: list[Role] = []
        self.retired_spectators: list[Role] = []
        self.capacity_denied_role: Role | None = None
        self.capacity_replacement_role: Role | None = None
        self._spectator_player_address: str | None = None
        self.relay: subprocess.Popen[bytes] | None = None
        self.relay_stdout_handle: object | None = None
        self.relay_stderr_handle: object | None = None
        self.local_autopunch: LocalAutopunchRelay | None = None
        self.spectator_port_blocker: socket.socket | None = None
        self._stale_spectator_artifact: Path | None = None
        self.players_closed_for_retention = False
        self.observer: ConfirmedHashObserver | None = None
        self.placement: dict[str, dict[str, object]] = _parse_placement(args)
        self.hosts: dict[str, object] = {"": LOCAL_HOST}
        self.role_host_names: dict[str, str] = {}
        self.distributed_mode = bool(self.placement)
        self._mirrors: list[object] = []
        self._last_mirror_refresh = 0.0
        self._install_stats: dict[str, object] = {}
        self._remote_run_roots: dict[str, str] = {}
        self._remote_source_installs: dict[str, Path] = {}
        self._remote_staged: dict[str, dict[str, Path]] = {}
        self._cleanup_escalated = False
        self.evidence_guard = rolehost.EvidenceGuard(
            enabled=self.distributed_mode,
            staleness_budget_s=float(args.evidence_staleness_budget),
            failure=HarnessFailure,
        )
        self.controller_address = (
            args.controller_address or "127.0.0.1"
        )
        self.relay_bind = args.relay_bind or (
            "0.0.0.0" if self.distributed_mode else "127.0.0.1"
        )
        self.relay_advertise = (
            self.controller_address if self.distributed_mode else "127.0.0.1"
        )
        self.autopunch_bind = args.autopunch_bind or (
            "0.0.0.0" if self.distributed_mode else "127.0.0.1"
        )
        self.autopunch_advertise = self.relay_advertise
        self._distributed_report: dict[str, object] = {
            "enabled": self.distributed_mode,
            "placement": self.placement,
            "controller_address": self.controller_address,
            "hosts": {},
            "skipped_coverage": [],
            "barrier_budget": {
                "remote_barrier_slack_ms": int(
                    args.remote_barrier_slack_ms
                ),
                "evidence_staleness_budget_s": float(
                    args.evidence_staleness_budget
                ),
                "route_p50_agreement_us": int(
                    args.route_p50_agreement_us
                ),
            },
        }
        map_path = Path(args.dll_map)
        self.net_frame_rva = parse_map_data_rva(
            map_path, "?g_netFrame@"
        )
        self.net_gate_rva = parse_map_data_rva(
            map_path, "?g_netGateOwnsTick@"
        )
        self.clock_thread_rva = parse_map_data_rva(
            map_path, "?g_clockThread@"
        )
        self.dispatch_count_rva = (
            parse_map_data_rva(map_path, "?g_dispatchN@")
            if args.returned_charselect_idle_ticks
            else None
        )
        self.presentation_commit_count_rva = (
            parse_map_data_rva(
                map_path, "?g_netPresentationCommitCallbacks@"
            )
            if args.returned_charselect_idle_ticks
            else None
        )
        self.test_pacing_summary_request_rva = (
            parse_map_data_rva(
                map_path, "?g_netTestPacingSummaryRequest@"
            )
            if args.full_match_rematch and args.spectator
            else None
        )
        self.test_pacing_summary_acknowledged_rva = (
            parse_map_data_rva(
                map_path, "?g_netTestPacingSummaryAcknowledged@"
            )
            if args.full_match_rematch and args.spectator
            else None
        )
        self.automatch_test_rva = (
            parse_map_data_rva(map_path, "?g_netAutomatchTest@")
            if args.returned_charselect_idle_ticks
            else None
        )
        self.test_pause_request_rva = parse_map_data_rva(
            map_path, "?g_netTestPauseRequest@"
        )
        self.test_pause_acknowledged_rva = parse_map_data_rva(
            map_path, "?g_netTestPauseAcknowledged@"
        )
        self.test_pause_frame_rva = parse_map_data_rva(
            map_path, "?g_netTestPauseFrame@"
        )
        self.test_pause_held_frame_rva = parse_map_data_rva(
            map_path, "?g_netTestPauseHeldFrame@"
        )
        self.events: list[dict[str, object]] = []
        self.game_affinity_parent_mask: int | None = None
        self.game_affinity_plan_error: str | None = None
        if args.split_game_affinity:
            process_mask = ctypes.c_size_t()
            system_mask = ctypes.c_size_t()
            if kernel32.GetProcessAffinityMask(
                kernel32.GetCurrentProcess(),
                ctypes.byref(process_mask),
                ctypes.byref(system_mask),
            ):
                self.game_affinity_parent_mask = int(process_mask.value)
            else:
                self.game_affinity_plan_error = str(
                    win_error("GetProcessAffinityMask(harness)")
                )
        self.game_affinity_masks = self._plan_game_affinity_masks()
        self.report: dict[str, object] = {
            "route": args.route,
            "config_variant": args.config_variant,
            "layout_command_mutation": args.layout_command_mutation,
            "expected_layout_result": (
                "mismatch"
                if args.layout_command_mutation == "active-p2"
                else "match"
            ),
            "spectator_requested": args.spectator,
            "spectator_count_requested": args.spectator_count,
            "spectator_route_requested": args.spectator_route,
            "spectator_routes_requested": args.spectator_routes,
            "spectator_admission": "public",
            "spectator_negative": args.spectator_negative,
            "spectator_input_mash": args.spectator_input_mash,
            "spectator_capacity_readmit":
                args.spectator_capacity_readmit,
            "spectator_active_catchup_pause_seconds":
                args.spectator_active_catchup_pause_seconds,
            "spectator_sibling_disconnect":
                args.spectator_sibling_disconnect,
            "gameplay_ticks_required": args.gameplay_ticks,
            "full_match_rematch": args.full_match_rematch,
            "force_ko_after_frames": args.force_ko_after_frames,
            "rematch_gameplay_ticks": args.rematch_gameplay_ticks,
            "returned_charselect_idle_ticks":
                args.returned_charselect_idle_ticks,
            "network_profile": args.network_profile,
            "network_seed": args.network_seed,
            "runtime_delay_scripts": {
                "host": args.host_delay_script,
                "client": args.client_delay_script,
            },
            "harness_lifecycle": {
                "output_root": str(self.output_root),
                "ownership_marker": HARNESS_OWNERSHIP_MARKER,
                "keep_workdir_on_failure": args.keep_workdir_on_failure,
                "keep_workdir_limit": args.keep_workdir_limit,
            },
            "game_process_affinity": self._game_affinity_report(),
            "peer_settings": {
                "host": {
                    "delay": args.host_delay,
                    "rollback": args.host_rollback,
                },
                "client": {
                    "delay": args.client_delay,
                    "rollback": args.client_rollback,
                },
            },
            "run_root": str(self.run_root),
            "started": datetime.now().isoformat(),
            "commands": {},
            "runtime_data_rvas": {
                "g_netFrame": f"0x{self.net_frame_rva:X}",
                "g_netGateOwnsTick": f"0x{self.net_gate_rva:X}",
                "g_clockThread": f"0x{self.clock_thread_rva:X}",
                "g_dispatchN": (
                    f"0x{self.dispatch_count_rva:X}"
                    if self.dispatch_count_rva is not None
                    else None
                ),
                "g_netPresentationCommitCallbacks": (
                    f"0x{self.presentation_commit_count_rva:X}"
                    if self.presentation_commit_count_rva is not None
                    else None
                ),
                "g_netTestPacingSummaryRequest": (
                    f"0x{self.test_pacing_summary_request_rva:X}"
                    if self.test_pacing_summary_request_rva is not None
                    else None
                ),
                "g_netTestPacingSummaryAcknowledged": (
                    f"0x{self.test_pacing_summary_acknowledged_rva:X}"
                    if self.test_pacing_summary_acknowledged_rva is not None
                    else None
                ),
                "g_netAutomatchTest": (
                    f"0x{self.automatch_test_rva:X}"
                    if self.automatch_test_rva is not None
                    else None
                ),
                "g_netTestPauseRequest": (
                    f"0x{self.test_pause_request_rva:X}"
                    if self.test_pause_request_rva is not None
                    else None
                ),
                "g_netTestPauseAcknowledged": (
                    f"0x{self.test_pause_acknowledged_rva:X}"
                    if self.test_pause_acknowledged_rva is not None
                    else None
                ),
            },
            "events": self.events,
        }

    def _plan_game_affinity_masks(self) -> dict[str, int]:
        if not self.args.split_game_affinity:
            return {}

        role_names = ["host", "client"]
        if self.args.spectator:
            role_names.extend(
                "spectator" if index == 0 else f"spectator{index + 1}"
                for index in range(self.args.spectator_count)
            )
        reserved_cpus = 2
        cpus_per_role = 2
        pointer_bits = ctypes.sizeof(ctypes.c_size_t) * 8
        if self.game_affinity_parent_mask is None:
            return {}
        usable_cpu_ids = [
            index
            for index in range(pointer_bits)
            if self.game_affinity_parent_mask & (1 << index)
        ]
        required_cpus = reserved_cpus + cpus_per_role * len(role_names)
        if len(usable_cpu_ids) < required_cpus:
            return {}

        masks: dict[str, int] = {}
        for role_index, role_name in enumerate(role_names):
            first_index = reserved_cpus + role_index * cpus_per_role
            role_cpu_ids = usable_cpu_ids[
                first_index:first_index + cpus_per_role
            ]
            masks[role_name] = sum(1 << cpu for cpu in role_cpu_ids)
        return masks

    def _game_affinity_report(self) -> dict[str, object]:
        requested = bool(self.args.split_game_affinity)
        detected_cpus = os.cpu_count() or 0
        role_names = ["host", "client"]
        if self.args.spectator:
            role_names.extend(
                "spectator" if index == 0 else f"spectator{index + 1}"
                for index in range(self.args.spectator_count)
            )
        required_cpus = 2 + 2 * len(role_names)
        pointer_bits = ctypes.sizeof(ctypes.c_size_t) * 8
        allowed_cpus = [
            index
            for index in range(pointer_bits)
            if (
                self.game_affinity_parent_mask is not None
                and self.game_affinity_parent_mask & (1 << index)
            )
        ]
        status = "disabled"
        reason: str | None = None
        if requested:
            if self.game_affinity_masks:
                status = "pending"
            else:
                status = "skipped"
                reason = self.game_affinity_plan_error or (
                    "insufficient allowed logical CPUs: "
                    f"detected={detected_cpus} allowed={len(allowed_cpus)} "
                    f"required={required_cpus} "
                    f"affinityMaskBits={pointer_bits}"
                )
        return {
            "requested": requested,
            "status": status,
            "reason": reason,
            "detected_logical_cpus": detected_cpus,
            "parent_affinity_mask": (
                f"0x{self.game_affinity_parent_mask:X}"
                if self.game_affinity_parent_mask is not None
                else None
            ),
            "allowed_logical_cpus": allowed_cpus,
            "reserved_logical_cpus": allowed_cpus[:2],
            "logical_cpus_per_game": 2,
            "required_logical_cpus": required_cpus,
            "roles": {},
        }

    def _apply_game_affinity(self, role: Role) -> None:
        if not self.args.split_game_affinity:
            return

        affinity_report = self.report["game_process_affinity"]
        assert isinstance(affinity_report, dict)
        role_report = affinity_report["roles"]
        assert isinstance(role_report, dict)
        if not self.game_affinity_masks:
            evidence = {
                "status": "skipped",
                "pid": role.game_memory.pid if role.game_memory else None,
                "reason": affinity_report["reason"],
            }
            role_report[role.name] = evidence
            self.event(
                "game-affinity-skipped",
                role=role.name,
                **evidence,
            )
            return

        if role.game_memory is None:
            raise HarnessFailure(
                f"{role.name} game is unavailable for affinity assignment"
            )
        mask = self.game_affinity_masks[role.name]
        logical_cpus = [
            index
            for index in range(ctypes.sizeof(ctypes.c_size_t) * 8)
            if mask & (1 << index)
        ]
        evidence = {
            "status": "applied",
            "pid": role.game_memory.pid,
            "mask": f"0x{mask:X}",
            "logical_cpus": logical_cpus,
        }
        try:
            role.game_memory.set_affinity(mask)
        except Exception as error:
            evidence["status"] = "failed"
            evidence["error"] = str(error)
            role_report[role.name] = evidence
            affinity_report["status"] = "failed"
            self.event(
                "game-affinity-failed",
                role=role.name,
                **evidence,
            )
            raise
        role_report[role.name] = evidence
        if len(role_report) == len(self.game_affinity_masks):
            affinity_report["status"] = "applied"
        self.event(
            "game-affinity-applied",
            role=role.name,
            **evidence,
        )

    def refresh_evidence(self, force: bool = False) -> None:
        if not self._mirrors:
            return
        now = time.monotonic()
        if not force and now - self._last_mirror_refresh < 0.25:
            return
        self._last_mirror_refresh = now
        for mirror in self._mirrors:
            mirror.refresh()

    def event(self, name: str, **details: object) -> None:
        item = {
            "time": datetime.now().isoformat(),
            "elapsed_seconds": round(time.monotonic() - self.started_mono, 3),
            "name": name,
        }
        item.update(details)
        self.events.append(item)
        print(f"[live-e2e] {name}: {details}", flush=True)

    def runtime_roles(self) -> tuple[Role, ...]:
        roles = list(self.roles.values())
        roles.extend(self.spectators)
        return tuple(roles)

    def prepared_roles(self) -> tuple[Role, ...]:
        roles = list(self.runtime_roles())
        roles.extend(self.retired_spectators)
        for role in (
            self.capacity_denied_role,
            self.capacity_replacement_role,
        ):
            if role is not None:
                roles.append(role)
        roles.extend(self.auxiliary_roles)
        unique: list[Role] = []
        seen: set[int] = set()
        for role in roles:
            identity = id(role)
            if identity in seen:
                continue
            seen.add(identity)
            unique.append(role)
        return tuple(unique)

    def forced_character_selection(self) -> str:
        return ",".join(
            f"{cursor}:{alternate}"
            for cursor, alternate in (
                (
                    self.args.p1_character_cursor,
                    self.args.p1_character_alternate,
                ),
                (
                    self.args.p2_character_cursor,
                    self.args.p2_character_alternate,
                ),
            )
        )

    def artifact_roles(self) -> tuple[Role, ...]:
        return self.prepared_roles()

    def is_spectator_role(self, role: Role) -> bool:
        return (
            role in self.spectators
            or role in self.retired_spectators
            or role in self.auxiliary_roles
            or role is self.capacity_denied_role
            or role is self.capacity_replacement_role
        )

    def runtime_data_address(self, role: Role, rva: int) -> int:
        if not role.game_memory:
            raise HarnessFailure(
                f"{role.name} game is unavailable for runtime data read"
            )
        if role.dll_base is None:
            module = role.host.module_base(
                role.game_memory.pid, Path(self.args.dll).name
            )
            if module is None:
                raise HarnessFailure(
                    f"{role.name} does not have {Path(self.args.dll).name} "
                    "loaded"
                )
            role.dll_base = module[0]
        return role.dll_base + rva

    def runtime_data_u32(self, role: Role, rva: int) -> int:
        assert role.game_memory
        return role.game_memory.read_u32(
            self.runtime_data_address(role, rva)
        )

    def _dwell_rvas(self) -> dict[str, int]:
        return {
            "canonical_frame": self.net_frame_rva,
            "main_dispatch_calls": self.dispatch_count_rva,
            "successful_presentations": self.presentation_commit_count_rva,
        }

    def runtime_data_u32_many(
        self, role: Role, rvas: dict[str, int]
    ) -> dict[str, int]:
        assert role.game_memory
        specs = [
            (name, self.runtime_data_address(role, rva), "u32")
            for name, rva in rvas.items()
        ]
        return role.game_memory.read_many(specs)

    def runtime_data_bool(self, role: Role, rva: int) -> bool:
        assert role.game_memory
        return role.game_memory.read(
            self.runtime_data_address(role, rva), 1
        ) != b"\x00"

    def write_runtime_data_bool(
        self, role: Role, rva: int, value: bool
    ) -> None:
        assert role.game_memory
        address = self.runtime_data_address(role, rva)
        role.game_memory.write(address, b"\x01" if value else b"\x00")
        if self.runtime_data_bool(role, rva) != value:
            raise HarnessFailure(
                f"{role.name} runtime bool write did not stick at "
                f"RVA 0x{rva:X}: expected={value}"
            )

    def write_runtime_data_u32(
        self, role: Role, rva: int, value: int
    ) -> None:
        assert role.game_memory
        address = self.runtime_data_address(role, rva)
        role.game_memory.write_u32(address, value)
        observed = self.runtime_data_u32(role, rva)
        if observed != (value & 0xFFFFFFFF):
            raise HarnessFailure(
                f"{role.name} runtime DWORD write did not stick at "
                f"RVA 0x{rva:X}: expected={value & 0xFFFFFFFF} "
                f"actual={observed}"
            )

    def request_spectator_pacing_summary(
        self, timeout: float = 5.0
    ) -> None:
        role = self.spectator
        if role is None or not self.args.full_match_rematch:
            return
        request_rva = self.test_pacing_summary_request_rva
        acknowledged_rva = self.test_pacing_summary_acknowledged_rva
        if request_rva is None or acknowledged_rva is None:
            raise HarnessFailure(
                "spectator pacing-summary request RVAs are unavailable"
            )

        request_before = self.runtime_data_u32(role, request_rva)
        acknowledged_before = self.runtime_data_u32(
            role, acknowledged_rva
        )
        generation = (
            max(request_before, acknowledged_before) + 1
        ) & 0xFFFFFFFF
        if generation == 0:
            generation = 1
        summaries_before = role.dll_text().count("[pacing-ledger]")
        self.write_runtime_data_u32(role, request_rva, generation)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._scan_failures()
            acknowledged = self.runtime_data_u32(
                role, acknowledged_rva
            )
            summaries_after = role.dll_text().count("[pacing-ledger]")
            if (
                acknowledged == generation
                and summaries_after > summaries_before
            ):
                self.event(
                    "spectator-pacing-summary-captured",
                    generation=generation,
                    summaries_before=summaries_before,
                    summaries_after=summaries_after,
                )
                return
            time.sleep(0.01)
        raise HarnessFailure(
            "spectator did not acknowledge and flush its final pacing "
            f"summary generation={generation}"
        )

    def suspend_role_frame_thread(
        self, role: Role, request_context: bool = False
    ) -> SuspendedThread:
        if not role.game_memory:
            raise HarnessFailure(
                f"{role.name} game is unavailable for frame-thread suspension"
            )
        tid = self.runtime_data_u32(role, self.clock_thread_rva)
        if tid == 0:
            raise HarnessFailure(
                f"{role.name} has not pinned its frame-dispatch thread"
            )
        try:
            return role.host.open_thread_suspended(
                role.game_memory.pid, tid, request_context
            )
        except HarnessFailure as exc:
            raise HarnessFailure(f"{role.name} {exc}") from exc

    @staticmethod
    def _hang_address(
        address: int,
        modules: list[dict[str, object]],
    ) -> dict[str, object]:
        rendered: dict[str, object] = {
            "address": f"0x{address & 0xFFFFFFFF:08X}",
        }
        for module in modules:
            base = int(module["base"])
            size = int(module["size"])
            if base <= address < base + size:
                rva = address - base
                name = str(module["name"])
                rendered.update(
                    {
                        "module": name,
                        "module_path": str(module["path"]),
                        "rva": f"0x{rva:X}",
                        "module_rva": f"{name}+0x{rva:X}",
                    }
                )
                break
        return rendered

    def capture_returned_charselect_dispatcher_freeze(
        self,
        role: Role,
        frozen_seconds: float,
        state: dict[str, object],
        canonical_frame: int,
        dispatch_calls: int,
    ) -> dict[str, object]:
        assert role.game_memory
        snapshot: dict[str, object] = {
            "role": role.name,
            "pid": role.game_memory.pid,
            "frozen_seconds": round(frozen_seconds, 3),
            "canonical_frame": canonical_frame,
            "main_dispatch_calls": dispatch_calls,
            "state": state,
            "context_api": (
                "Wow64GetThreadContext"
                if ctypes.sizeof(ctypes.c_void_p) == 8
                else "GetThreadContext"
            ),
        }
        suspended: SuspendedThread | None = None
        try:
            suspended = self.suspend_role_frame_thread(
                role, request_context=True
            )
            snapshot["tid"] = suspended.tid

            context = WOW64_CONTEXT()
            context.ContextFlags = WOW64_CONTEXT_CONTROL_INTEGER
            if not _get_thread_context(
                suspended.handle, ctypes.byref(context)
            ):
                raise win_error(
                    f"Wow64GetThreadContext({suspended.tid})"
                )

            eip = int(context.Eip)
            esp = int(context.Esp)
            ebp = int(context.Ebp)
            snapshot["registers"] = {
                "eip": f"0x{eip:08X}",
                "esp": f"0x{esp:08X}",
                "ebp": f"0x{ebp:08X}",
            }

            modules: list[dict[str, object]] = []
            try:
                modules = process_module_map(role.game_memory.pid)
                snapshot["module_count"] = len(modules)
            except Exception as error:
                snapshot["module_map_error"] = (
                    f"{type(error).__name__}: {error}"
                )

            frames: list[dict[str, object]] = [
                {
                    "index": 0,
                    "kind": "instruction",
                    **self._hang_address(eip, modules),
                }
            ]
            stack_limit = min(0xFFFFFFFF, esp + 8 * 1024 * 1024)
            current_ebp = ebp
            termination = "maximum-frame-count"
            for index in range(1, 33):
                if current_ebp == 0:
                    termination = "null-ebp"
                    break
                if current_ebp & 0x3:
                    termination = "unaligned-ebp"
                    break
                if current_ebp < esp or current_ebp > stack_limit - 8:
                    termination = "ebp-outside-stack-window"
                    break
                try:
                    next_ebp, return_address = struct.unpack(
                        "<II", role.game_memory.read(current_ebp, 8)
                    )
                except Exception as error:
                    termination = "stack-read-failed"
                    snapshot["stack_read_error"] = (
                        f"0x{current_ebp:08X}: "
                        f"{type(error).__name__}: {error}"
                    )
                    break
                frame = {
                    "index": index,
                    "kind": "return",
                    "frame_pointer": f"0x{current_ebp:08X}",
                    **self._hang_address(return_address, modules),
                }
                frames.append(frame)
                if next_ebp == 0:
                    termination = "null-parent-ebp"
                    break
                if (
                    next_ebp <= current_ebp
                    or next_ebp - current_ebp > 1024 * 1024
                ):
                    termination = "nonconservative-parent-ebp"
                    break
                current_ebp = next_ebp

            snapshot["frames"] = frames
            snapshot["stack_walk"] = {
                "strategy": "conservative-ebp-chain",
                "maximum_frames": 32,
                "stack_window_start": f"0x{esp:08X}",
                "stack_window_end": f"0x{stack_limit:08X}",
                "termination": termination,
            }
        except Exception as error:
            snapshot["capture_error"] = (
                f"{type(error).__name__}: {error}"
            )
        finally:
            if suspended is not None:
                try:
                    suspended.resume()
                    snapshot["frame_thread_resumed"] = True
                except Exception as error:
                    snapshot["frame_thread_resumed"] = False
                    snapshot["resume_error"] = (
                        f"{type(error).__name__}: {error}"
                    )

        hang_snapshots = self.report.setdefault(
            "returned_charselect_hang_snapshots", []
        )
        assert isinstance(hang_snapshots, list)
        hang_snapshots.append(snapshot)
        self.event(
            "returned-charselect-dispatcher-freeze-snapshot",
            **snapshot,
        )
        return snapshot

    @staticmethod
    def _returned_charselect_timeline(
        state: dict[str, object]
    ) -> dict[str, int] | None:
        titles = state.get("titles")
        if not isinstance(titles, list):
            return None
        for title in titles:
            if not isinstance(title, str):
                continue
            match = WINDOW_TIMELINE_RE.search(title)
            if match:
                return {
                    key: int(match.group(key))
                    for key in ("local", "remote", "ack")
                }
        return None

    @staticmethod
    def _returned_charselect_idle_violation(
        state: dict[str, object],
        world_counter: int | None = None,
    ) -> str | None:
        if "read_error" in state:
            return f"state read failed: {state['read_error']}"
        expected = {
            "scene": 3,
            "scene_next": 3,
            "scene3_sub": 2,
            "p1_confirm": 1,
            "p2_confirm": 1,
            "match_start": 0,
        }
        differences = {
            field: {
                "expected": value,
                "actual": state.get(field),
            }
            for field, value in expected.items()
            if state.get(field) != value
        }
        if differences:
            return f"returned character select changed: {differences}"
        if (
            world_counter is not None
            and int(state["frame_counter"]) != world_counter
        ):
            return (
                "inactive match-world counter advanced during character "
                f"select: expected={world_counter} "
                f"actual={state['frame_counter']}"
            )
        return None

    def _returned_pause_diagnostics(
        self,
        roles: tuple[Role, ...],
        include_state: bool = False,
    ) -> dict[str, object]:
        diagnostics: dict[str, object] = {}
        assert self.test_pause_request_rva is not None
        assert self.test_pause_acknowledged_rva is not None
        for role in roles:
            item: dict[str, object] = {}
            memory = role.game_memory
            if memory is None:
                item["error"] = "game memory is unavailable"
                diagnostics[role.name] = item
                continue
            item["pid"] = memory.pid
            item["alive"] = memory.alive()
            reads = (
                ("request", self.test_pause_request_rva),
                ("acknowledged", self.test_pause_acknowledged_rva),
                ("canonical_frame", self.net_frame_rva),
                ("gate", self.net_gate_rva),
            )
            for name, rva in reads:
                try:
                    item[name] = self.runtime_data_u32(role, rva)
                except Exception as error:
                    item[f"{name}_error"] = (
                        f"{type(error).__name__}: {error}"
                    )
            if self.automatch_test_rva is not None:
                try:
                    item["automatch_enabled"] = self.runtime_data_bool(
                        role, self.automatch_test_rva
                    )
                except Exception as error:
                    item["automatch_error"] = (
                        f"{type(error).__name__}: {error}"
                    )
            if include_state:
                try:
                    item["state"] = game_state(memory)
                except Exception as error:
                    item["state_error"] = (
                        f"{type(error).__name__}: {error}"
                    )
            diagnostics[role.name] = item
        return diagnostics

    def _wait_returned_pause_acknowledgements(
        self,
        roles: tuple[Role, ...],
        acknowledged: bool,
        purpose: str,
        timeout: float,
    ) -> dict[str, object]:
        expected = 1 if acknowledged else 0
        deadline = time.monotonic() + timeout
        next_failure_scan = 0.0
        while True:
            diagnostics = self._returned_pause_diagnostics(roles)
            complete = True
            read_errors: dict[str, object] = {}
            for role in roles:
                item = diagnostics[role.name]
                assert isinstance(item, dict)
                errors = {
                    key: value
                    for key, value in item.items()
                    if key.endswith("_error") or key == "error"
                }
                if errors:
                    read_errors[role.name] = errors
                    complete = False
                    continue
                if (
                    int(item["request"]) != expected
                    or int(item["acknowledged"]) != expected
                ):
                    complete = False
            if read_errors:
                raise HarnessFailure(
                    f"{purpose} cooperative pause status read failed: "
                    f"{read_errors}; diagnostics={diagnostics}"
                )
            if complete:
                return diagnostics

            now = time.monotonic()
            if now >= deadline:
                diagnostics = self._returned_pause_diagnostics(
                    roles, include_state=True
                )
                raise HarnessFailure(
                    f"{purpose} timed out waiting for cooperative pause "
                    f"request/ack={expected}; diagnostics={diagnostics}"
                )
            if now >= next_failure_scan:
                self._scan_failures()
                next_failure_scan = now + 0.05
            time.sleep(0.0005)

    def _clear_returned_pause_requests(
        self,
        roles: tuple[Role, ...],
        purpose: str,
        timeout: float = 2.0,
    ) -> dict[str, object]:
        assert self.test_pause_request_rva is not None
        write_errors: dict[str, str] = {}
        for role in roles:
            try:
                self.write_runtime_data_u32(
                    role, self.test_pause_request_rva, 0
                )
            except Exception as error:
                write_errors[role.name] = (
                    f"{type(error).__name__}: {error}"
                )
        if write_errors:
            raise HarnessFailure(
                f"{purpose} could not clear every cooperative pause request: "
                f"{write_errors}; diagnostics="
                f"{self._returned_pause_diagnostics(roles, True)}"
            )
        return self._wait_returned_pause_acknowledgements(
            roles,
            acknowledged=False,
            purpose=purpose,
            timeout=timeout,
        )

    def set_returned_charselect_automatch_cooperatively(
        self,
        roles: tuple[Role, ...],
        enabled: bool,
        purpose: str,
        timeout: float = 8.0,
        wait_for_entry: bool = False,
    ) -> dict[str, object]:
        assert self.automatch_test_rva is not None
        assert self.test_pause_request_rva is not None
        assert self.test_pause_acknowledged_rva is not None
        deadline = time.monotonic() + timeout
        attempt_number = 0
        while time.monotonic() < deadline:
            attempt_number += 1
            transaction: dict[str, object] = {
                "purpose": purpose,
                "attempt": attempt_number,
                "target_automatch_enabled": enabled,
            }
            success = False
            retry = False
            failure: Exception | None = None
            release_failure: Exception | None = None
            original_automatch: dict[str, bool] = {}
            mutation_started = False
            requested_at = time.monotonic()
            try:
                baseline = self._clear_returned_pause_requests(
                    roles, f"{purpose} attempt {attempt_number} baseline"
                )
                transaction["baseline"] = baseline

                for role in roles:
                    self.write_runtime_data_u32(
                        role, self.test_pause_request_rva, 1
                    )
                self.event(
                    "returned-charselect-cooperative-pause-requested",
                    purpose=purpose,
                    attempt=attempt_number,
                    roles=[role.name for role in roles],
                )

                remaining = max(0.001, deadline - time.monotonic())
                acknowledged_status = (
                    self._wait_returned_pause_acknowledgements(
                        roles,
                        acknowledged=True,
                        purpose=(
                            f"{purpose} attempt {attempt_number} acquire"
                        ),
                        timeout=min(2.0, remaining),
                    )
                )
                transaction["acknowledged_after_seconds"] = round(
                    time.monotonic() - requested_at, 6
                )
                transaction["acknowledged_status"] = acknowledged_status

                paused_roles: dict[str, object] = {}
                for role in roles:
                    assert role.game_memory
                    state = game_state(role.game_memory)
                    violation = self._returned_charselect_idle_violation(
                        state
                    )
                    if violation is not None:
                        if "read_error" in state:
                            raise HarnessFailure(
                                f"{role.name} state read failed while "
                                f"acknowledged for {purpose}: {violation}; "
                                f"state={state}"
                            )
                        if wait_for_entry:
                            in_charselect = (
                                state.get("scene") == 3
                                and state.get("scene3_sub") == 2
                            )
                            in_rematch_gameplay = (
                                state.get("scene") == 2
                                and state.get("match_sub") == 0
                                and int(state.get("round_counter", 5)) == 0
                            )
                            if in_charselect or in_rematch_gameplay:
                                raise HarnessFailure(
                                    f"{role.name} automatch consumed the "
                                    "returned character select before the "
                                    f"{purpose} cooperative hold: "
                                    f"{violation}; state={state}"
                                )
                            transaction["retry_reason"] = (
                                f"{role.name}: {violation}"
                            )
                            retry = True
                            break
                        raise HarnessFailure(
                            f"{role.name} cannot be cooperatively paused for "
                            f"{purpose}: {violation}; state={state}"
                        )

                    frame = self.runtime_data_u32(
                        role, self.net_frame_rva
                    )
                    gate = self.runtime_data_u32(role, self.net_gate_rva)
                    if gate != 0:
                        raise HarnessFailure(
                            f"{role.name} acknowledged {purpose} while "
                            f"owning the simulation gate: frame={frame} "
                            f"gate={gate}; state={state}"
                        )
                    original = self.runtime_data_bool(
                        role, self.automatch_test_rva
                    )
                    expected_original = not enabled
                    if original != expected_original:
                        raise HarnessFailure(
                            f"{role.name} automatch state before {purpose} "
                            f"was {original}, expected {expected_original}"
                        )
                    original_automatch[role.name] = original
                    paused_roles[role.name] = {
                        "pid": role.game_memory.pid,
                        "canonical_frame": frame,
                        "gate": gate,
                        "state": state,
                    }

                if not retry:
                    transaction["paused_roles"] = paused_roles
                    self.event(
                        "returned-charselect-cooperative-pause-acknowledged",
                        purpose=purpose,
                        attempt=attempt_number,
                        roles=paused_roles,
                    )
                    mutation_started = True
                    for role in roles:
                        self.write_runtime_data_bool(
                            role, self.automatch_test_rva, enabled
                        )

                    post_mutation: dict[str, object] = {}
                    for role in roles:
                        assert role.game_memory
                        pause_status = self._returned_pause_diagnostics(
                            (role,)
                        )[role.name]
                        assert isinstance(pause_status, dict)
                        before = paused_roles[role.name]
                        assert isinstance(before, dict)
                        state = game_state(role.game_memory)
                        violation = self._returned_charselect_idle_violation(
                            state
                        )
                        if (
                            pause_status.get("request") != 1
                            or pause_status.get("acknowledged") != 1
                            or pause_status.get("gate") != 0
                            or pause_status.get("canonical_frame")
                            != before["canonical_frame"]
                            or violation is not None
                            or not (
                                self.runtime_data_bool(
                                    role, self.automatch_test_rva
                                )
                                == enabled
                            )
                        ):
                            raise HarnessFailure(
                                f"{role.name} cooperative {purpose} "
                                "boundary changed during mutation: "
                                f"before={before} after={pause_status} "
                                f"violation={violation} state={state}"
                            )
                        post_mutation[role.name] = {
                            **pause_status,
                            "state": state,
                        }
                    transaction["post_mutation"] = post_mutation
                    transaction["mutation_applied"] = True
                    success = True
                    self.event(
                        "returned-charselect-cooperative-automatch-written",
                        purpose=purpose,
                        attempt=attempt_number,
                        enabled=enabled,
                        roles=post_mutation,
                    )
            except Exception as error:
                failure = error
                transaction["failure"] = (
                    f"{type(error).__name__}: {error}"
                )
            finally:
                if mutation_started and not success:
                    rollback_errors: dict[str, str] = {}
                    for role in roles:
                        original = original_automatch.get(role.name)
                        if original is None:
                            continue
                        try:
                            self.write_runtime_data_bool(
                                role, self.automatch_test_rva, original
                            )
                        except Exception as error:
                            rollback_errors[role.name] = (
                                f"{type(error).__name__}: {error}"
                            )
                    transaction["mutation_rollback_errors"] = rollback_errors
                try:
                    released = self._clear_returned_pause_requests(
                        roles,
                        f"{purpose} attempt {attempt_number} release",
                    )
                    transaction["released_status"] = released
                    transaction["requests_cleared"] = True
                except Exception as error:
                    release_failure = error
                    transaction["requests_cleared"] = False
                    transaction["release_failure"] = (
                        f"{type(error).__name__}: {error}"
                    )

                transactions = self.report.setdefault(
                    "returned_charselect_cooperative_pause_transactions", []
                )
                assert isinstance(transactions, list)
                span = time.monotonic() - requested_at
                transaction["freeze_seconds"] = round(span, 3)
                self._harness_freeze_seconds = (
                    getattr(self, "_harness_freeze_seconds", 0.0) + span
                )
                transactions.append(transaction)
                self.event(
                    "returned-charselect-cooperative-pause-released",
                    purpose=purpose,
                    attempt=attempt_number,
                    success=success,
                    retry=retry,
                    requests_cleared=transaction["requests_cleared"],
                    diagnostics=self._returned_pause_diagnostics(
                        roles, include_state=not success
                    ),
                )

            if failure is not None or release_failure is not None:
                pieces = []
                if failure is not None:
                    pieces.append(f"transaction failure: {failure}")
                if release_failure is not None:
                    pieces.append(f"release failure: {release_failure}")
                cause = failure or release_failure
                raise HarnessFailure(
                    f"{purpose} cooperative pause failed; "
                    + "; ".join(pieces)
                ) from cause
            if success:
                return transaction
            if not retry:
                raise HarnessFailure(
                    f"{purpose} cooperative pause ended without mutation "
                    "or a retry classification"
                )
            time.sleep(0.0005)

        raise HarnessFailure(
            f"timed out acquiring returned-character-select cooperative "
            f"pause for {purpose}; diagnostics="
            f"{self._returned_pause_diagnostics(roles, True)}"
        )

    def suspend_players_at_returned_charselect(
        self,
        suspended: list[SuspendedThread],
        purpose: str,
        timeout: float = 8.0,
        wait_for_entry: bool = False,
    ) -> None:
        deadline = time.monotonic() + timeout
        roles = tuple(self.roles.values())
        paused: list[tuple[Role, SuspendedThread, int]] = []
        try:
            while time.monotonic() < deadline and not paused:
                attempt: list[
                    tuple[Role, SuspendedThread, int]
                ] = []
                validated_count = 0
                try:
                    for role in roles:
                        assert role.game_memory
                        thread = self.suspend_role_frame_thread(role)
                        attempt.append((role, thread, -1))
                        state = game_state(role.game_memory)
                        violation = (
                            self._returned_charselect_idle_violation(state)
                        )
                        if violation is not None:
                            if wait_for_entry:
                                in_charselect = (
                                    state.get("scene") == 3
                                    and state.get("scene3_sub") == 2
                                )
                                in_rematch_gameplay = (
                                    state.get("scene") == 2
                                    and state.get("match_sub") == 0
                                    and int(
                                        state.get("round_counter", 5)
                                    ) == 0
                                )
                                if in_charselect or in_rematch_gameplay:
                                    raise HarnessFailure(
                                        f"{role.name} automatch consumed the "
                                        "returned character select before "
                                        f"the {purpose} hold: {violation}; "
                                        f"state={state}"
                                    )
                                break
                            raise HarnessFailure(
                                f"{role.name} cannot be suspended for "
                                f"{purpose}: {violation}; state={state}"
                            )
                        frame = self.runtime_data_u32(
                            role, self.net_frame_rva
                        )
                        gate = self.runtime_data_u32(
                            role, self.net_gate_rva
                        )
                        if gate != 0:
                            break
                        attempt[-1] = (role, thread, frame)
                        validated_count += 1

                    if validated_count == len(roles):
                        paused = attempt
                        attempt = []
                finally:
                    resume_suspended_threads(
                        thread for _role, thread, _frame in attempt
                    )
                if not paused:
                    time.sleep(0.0005)

            if len(paused) != len(roles):
                raise HarnessFailure(
                    "could not suspend every player at the unconfirmed "
                    f"returned character-select boundary for {purpose}"
                )

            suspended.extend(thread for _role, thread, _frame in paused)
            self.event(
                "returned-charselect-frame-threads-suspended",
                purpose=purpose,
                roles=[
                    {
                        "name": role.name,
                        "pid": thread.pid,
                        "tid": thread.tid,
                        "canonical_frame": frame,
                    }
                    for role, thread, frame in paused
                ],
            )
        finally:
            if len(paused) != len(roles):
                for _role, thread, _frame in reversed(paused):
                    try:
                        thread.resume()
                    except Exception:
                        pass

    def _require_fresh_characters_per_game(
        self, games_required: int
    ) -> None:
        for name, role in self.roles.items():
            pairs: dict[int, tuple[int, int]] = {}
            for match in CHARS_ENTER_RE.finditer(role.dll_text()):
                segment = int(match.group(1))
                if segment not in pairs:
                    pairs[segment] = (
                        int(match.group(2)),
                        int(match.group(3)),
                    )
            ordered = [pairs[s] for s in sorted(pairs)]
            if len(ordered) < games_required:
                raise HarnessFailure(
                    f"{name} logged {len(ordered)} gameplay character "
                    f"pairs; {games_required} games required: {ordered}"
                )
            for i in range(1, games_required):
                previous = ordered[i - 1]
                current = ordered[i]
                if previous[0] == current[0] or previous[1] == current[1]:
                    raise HarnessFailure(
                        f"{name} game {i + 1} reused a character from the "
                        f"previous game: {previous} -> {current}"
                    )
            self.event(
                "fresh-characters-per-game",
                role=name,
                pairs=[list(p) for p in ordered[:games_required]],
            )

    def wait_for_returned_charselect_idle_dwell(self) -> None:
        required_ticks = self.args.returned_charselect_idle_ticks
        if required_ticks == 0:
            return
        if (
            self.dispatch_count_rva is None
            or self.presentation_commit_count_rva is None
            or self.automatch_test_rva is None
            or self.test_pause_request_rva is None
            or self.test_pause_acknowledged_rva is None
        ):
            raise HarnessFailure(
                "returned-character-select dwell lacks required DLL data RVAs"
            )

        roles = tuple(self.roles.values())
        automatch_disabled = False
        evidence: dict[str, object] | None = None
        disable_transaction: dict[str, object] | None = None
        enable_transaction: dict[str, object] | None = None
        try:
            disable_transaction = (
                self.set_returned_charselect_automatch_cooperatively(
                    roles,
                    enabled=False,
                    purpose="disable-automatch",
                    wait_for_entry=True,
                )
            )
            automatch_disabled = True
            self.event(
                "returned-charselect-automatch-disabled-cooperatively",
                transaction=disable_transaction,
            )

            for role in roles:
                if self.runtime_data_bool(
                    role, self.automatch_test_rva
                ):
                    raise HarnessFailure(
                        f"{role.name} automatch remained enabled after the "
                        "cooperative disable transaction"
                    )

            baselines: dict[str, dict[str, object]] = {}
            for role in roles:
                assert role.game_memory
                state = game_state(role.game_memory)
                violation = self._returned_charselect_idle_violation(state)
                if violation is not None:
                    raise HarnessFailure(
                        f"{role.name} did not begin the dwell unconfirmed: "
                        f"{violation}; state={state}"
                    )
                counters = self.runtime_data_u32_many(role, self._dwell_rvas())
                baselines[role.name] = {
                    "canonical_frame": counters["canonical_frame"],
                    "main_dispatch_calls": counters["main_dispatch_calls"],
                    "successful_presentations": counters[
                        "successful_presentations"
                    ],
                    "world_counter": int(state["frame_counter"]),
                    "presentation_timeline":
                        self._returned_charselect_timeline(state),
                    "state": state,
                }

            self.event(
                "returned-charselect-idle-dwell-armed",
                profile=self.args.network_profile,
                required_ticks=required_ticks,
                automatch_enabled=False,
                baselines=baselines,
            )
            for role in roles:
                counters = self.runtime_data_u32_many(role, self._dwell_rvas())
                baselines[role.name]["canonical_frame"] = (
                    counters["canonical_frame"]
                )
                baselines[role.name]["main_dispatch_calls"] = (
                    counters["main_dispatch_calls"]
                )
                baselines[role.name]["successful_presentations"] = (
                    counters["successful_presentations"]
                )
            started = time.monotonic()
            deadline = started + max(30.0, required_ticks / 20.0)
            last_canonical_progress = {
                role.name: started for role in roles
            }
            last_dispatch_progress = {
                role.name: started for role in roles
            }
            previous_canonical = {
                role.name: int(
                    baselines[role.name]["canonical_frame"]
                )
                for role in roles
            }
            previous_dispatch = {
                role.name: int(
                    baselines[role.name]["main_dispatch_calls"]
                )
                for role in roles
            }
            previous_presentations = {
                role.name: int(
                    baselines[role.name]["successful_presentations"]
                )
                for role in roles
            }
            last_presentation_progress = {
                role.name: started for role in roles
            }
            final_states: dict[str, dict[str, object]] = {}
            final_canonical: dict[str, int] = {}
            final_dispatch: dict[str, int] = {}
            final_presentations: dict[str, int] = {}
            next_failure_scan = started

            while time.monotonic() < deadline:
                now = time.monotonic()
                if now >= next_failure_scan:
                    self._scan_failures()
                    next_failure_scan = now + 0.05
                complete = True
                for role in roles:
                    assert role.game_memory
                    state = game_state(role.game_memory)
                    violation = self._returned_charselect_idle_violation(
                        state,
                        int(baselines[role.name]["world_counter"]),
                    )
                    if violation is not None:
                        raise HarnessFailure(
                            f"{role.name} did not remain idle before rematch: "
                            f"{violation}; state={state}"
                        )
                    if self.runtime_data_bool(
                        role, self.automatch_test_rva
                    ):
                        raise HarnessFailure(
                            f"{role.name} automatch re-enabled itself during "
                            "the returned-character-select dwell"
                        )

                    counters = self.runtime_data_u32_many(
                        role, self._dwell_rvas()
                    )
                    canonical = counters["canonical_frame"]
                    dispatch = counters["main_dispatch_calls"]
                    presentations = counters["successful_presentations"]
                    if canonical < previous_canonical[role.name]:
                        raise HarnessFailure(
                            f"{role.name} canonical timeline moved backwards "
                            "during returned-character-select dwell"
                        )
                    if dispatch < previous_dispatch[role.name]:
                        raise HarnessFailure(
                            f"{role.name} main-dispatch counter moved "
                            "backwards during returned-character-select dwell"
                        )
                    if (
                        presentations
                        < previous_presentations[role.name]
                    ):
                        raise HarnessFailure(
                            f"{role.name} successful-presentation counter "
                            "moved backwards during returned-character-select "
                            "dwell"
                        )
                    if canonical > previous_canonical[role.name]:
                        previous_canonical[role.name] = canonical
                        last_canonical_progress[role.name] = now
                    elif now - last_canonical_progress[role.name] > 3.0:
                        raise HarnessFailure(
                            f"{role.name} canonical/native presentation froze "
                            "for over 3 seconds at returned character select"
                        )
                    if dispatch > previous_dispatch[role.name]:
                        previous_dispatch[role.name] = dispatch
                        last_dispatch_progress[role.name] = now
                    elif now - last_dispatch_progress[role.name] > 2.0:
                        frozen_seconds = (
                            now - last_dispatch_progress[role.name]
                        )
                        self.capture_returned_charselect_dispatcher_freeze(
                            role=role,
                            frozen_seconds=frozen_seconds,
                            state=state,
                            canonical_frame=canonical,
                            dispatch_calls=dispatch,
                        )
                        raise HarnessFailure(
                            f"{role.name} main dispatcher froze for over "
                            "2 seconds at returned character select"
                        )
                    if presentations > previous_presentations[role.name]:
                        previous_presentations[role.name] = presentations
                        last_presentation_progress[role.name] = now
                    elif (
                        now - last_presentation_progress[role.name] > 3.0
                    ):
                        raise HarnessFailure(
                            f"{role.name} physical presentation froze for "
                            "over 3 seconds at returned character select"
                        )

                    final_states[role.name] = state
                    final_canonical[role.name] = canonical
                    final_dispatch[role.name] = dispatch
                    final_presentations[role.name] = presentations
                    if (
                        canonical
                        - int(
                            baselines[role.name]["canonical_frame"]
                        )
                        < required_ticks
                    ):
                        complete = False
                    if (
                        presentations
                        - int(
                            baselines[role.name][
                                "successful_presentations"
                            ]
                        )
                        < required_ticks
                    ):
                        complete = False

                if complete:
                    for role in roles:
                        counters = self.runtime_data_u32_many(
                            role, self._dwell_rvas()
                        )
                        final_canonical[role.name] = (
                            counters["canonical_frame"]
                        )
                        final_dispatch[role.name] = (
                            counters["main_dispatch_calls"]
                        )
                        final_presentations[role.name] = (
                            counters["successful_presentations"]
                        )
                    elapsed = time.monotonic() - started
                    minimum_fps = max(
                        45.0, self.args.minimum_canonical_fps
                    )
                    role_evidence: dict[str, object] = {}
                    for role in roles:
                        baseline = baselines[role.name]
                        canonical_delta = (
                            final_canonical[role.name]
                            - int(baseline["canonical_frame"])
                        )
                        dispatch_delta = (
                            final_dispatch[role.name]
                            - int(baseline["main_dispatch_calls"])
                        )
                        presentation_commit_delta = (
                            final_presentations[role.name]
                            - int(baseline["successful_presentations"])
                        )
                        canonical_fps = canonical_delta / elapsed
                        if not minimum_fps <= canonical_fps <= 70.0:
                            raise HarnessFailure(
                                f"{role.name} returned-character-select "
                                "canonical pacing was outside "
                                f"[{minimum_fps:.1f}, 70.0] fps: "
                                f"{canonical_fps:.3f}"
                            )
                        if dispatch_delta < canonical_delta:
                            raise HarnessFailure(
                                f"{role.name} main dispatcher did not run for "
                                "every canonical dwell frame: dispatchDelta="
                                f"{dispatch_delta} canonicalDelta="
                                f"{canonical_delta}"
                            )
                        if abs(
                            presentation_commit_delta - canonical_delta
                        ) > 1:
                            raise HarnessFailure(
                                f"{role.name} did not successfully publish "
                                "exactly one frame per canonical dwell tick: "
                                f"publishDelta={presentation_commit_delta} "
                                f"canonicalDelta={canonical_delta}"
                            )

                        presentation_start = baseline[
                            "presentation_timeline"
                        ]
                        presentation_end = (
                            self._returned_charselect_timeline(
                                final_states[role.name]
                            )
                        )
                        presentation_delta = (
                            presentation_end["local"]
                            - int(presentation_start["local"])
                            if (
                                isinstance(presentation_start, dict)
                                and presentation_end is not None
                            )
                            else None
                        )
                        role_evidence[role.name] = {
                            "start_canonical_frame":
                                baseline["canonical_frame"],
                            "end_canonical_frame":
                                final_canonical[role.name],
                            "canonical_delta": canonical_delta,
                            "canonical_visible_fps":
                                round(canonical_fps, 3),
                            "start_main_dispatch_calls":
                                baseline["main_dispatch_calls"],
                            "end_main_dispatch_calls":
                                final_dispatch[role.name],
                            "main_dispatch_delta": dispatch_delta,
                            "main_dispatch_calls_per_second": round(
                                dispatch_delta / elapsed, 3
                            ),
                            "start_successful_presentations":
                                baseline["successful_presentations"],
                            "end_successful_presentations":
                                final_presentations[role.name],
                            "successful_presentation_delta":
                                presentation_commit_delta,
                            "native_match_world_counter":
                                baseline["world_counter"],
                            "native_match_world_counter_stable": True,
                            "presentation_timeline_start":
                                presentation_start,
                            "presentation_timeline_end":
                                presentation_end,
                            "presentation_local_delta":
                                presentation_delta,
                            "start_state": baseline["state"],
                            "end_state": final_states[role.name],
                        }
                    evidence = {
                        "profile": self.args.network_profile,
                        "requested_idle_ticks": required_ticks,
                        "elapsed_seconds": round(elapsed, 3),
                        "minimum_canonical_fps": minimum_fps,
                        "maximum_canonical_fps": 70.0,
                        "roles": role_evidence,
                    }
                    break
                time.sleep(0.005)
            else:
                raise HarnessFailure(
                    "timed out while dwelling at returned character select: "
                    f"requiredTicks={required_ticks} "
                    f"canonical={final_canonical} "
                    f"dispatch={final_dispatch}"
                )

            enable_transaction = (
                self.set_returned_charselect_automatch_cooperatively(
                    roles,
                    enabled=True,
                    purpose="re-enable-automatch",
                )
            )
            assert evidence is not None
            evidence["cooperative_disable_transaction"] = (
                disable_transaction
            )
            evidence["cooperative_enable_transaction"] = enable_transaction
            evidence["automatch_reenabled"] = True
            automatch_disabled = False
            self.event(
                "returned-charselect-automatch-enabled-cooperatively",
                transaction=enable_transaction,
            )
        finally:
            active_error = sys.exc_info()[1]
            cleanup_errors: dict[str, str] = {}
            emergency_direct_restore = False
            if automatch_disabled:
                try:
                    restored = (
                        self.set_returned_charselect_automatch_cooperatively(
                            roles,
                            enabled=True,
                            purpose="finally-restore-automatch",
                        )
                    )
                    automatch_disabled = False
                    if evidence is not None:
                        evidence["finally_restore_transaction"] = restored
                except Exception as error:
                    cleanup_errors["cooperative_automatch_restore"] = (
                        f"{type(error).__name__}: {error}"
                    )

            try:
                self._clear_returned_pause_requests(
                    roles, "returned-charselect dwell final cleanup"
                )
            except Exception as error:
                cleanup_errors["pause_request_clear"] = (
                    f"{type(error).__name__}: {error}"
                )

            for role in roles:
                try:
                    if not self.runtime_data_bool(
                        role, self.automatch_test_rva
                    ):
                        emergency_direct_restore = True
                    self.write_runtime_data_bool(
                        role, self.automatch_test_rva, True
                    )
                except Exception as error:
                    cleanup_errors[f"{role.name}_automatch_restore"] = (
                        f"{type(error).__name__}: {error}"
                    )

            cleanup_diagnostics = self._returned_pause_diagnostics(
                roles, include_state=bool(cleanup_errors)
            )
            self.event(
                "returned-charselect-test-controls-cleaned",
                requests_cleared=not (
                    "pause_request_clear" in cleanup_errors
                ),
                automatch_restored=not any(
                    key.endswith("_automatch_restore")
                    for key in cleanup_errors
                ),
                emergency_direct_restore=emergency_direct_restore,
                errors=cleanup_errors,
                diagnostics=cleanup_diagnostics,
            )
            if cleanup_errors and active_error is None:
                raise HarnessFailure(
                    "returned-character-select final cleanup failed: "
                    f"{cleanup_errors}; diagnostics={cleanup_diagnostics}"
                )

        if evidence is None:
            raise HarnessFailure(
                "returned-character-select dwell produced no evidence"
            )
        self.report["returned_charselect_idle_dwell"] = evidence
        self.event(
            "returned-charselect-idle-dwell-complete",
            profile=evidence["profile"],
            requested_idle_ticks=required_ticks,
            elapsed_seconds=evidence["elapsed_seconds"],
            roles=evidence["roles"],
        )

    def prepare_workspace(self, source_install: Path) -> None:
        self.output_root.mkdir(parents=True, exist_ok=True)
        if not self.output_root.is_dir():
            raise HarnessFailure(
                f"harness output root is not a directory: {self.output_root}"
            )
        if self.run_root.exists():
            raise HarnessFailure(
                f"refusing to reuse disposable run directory {self.run_root}"
            )
        self.run_root.mkdir()
        write_harness_marker(
            self.run_root, self.output_root, self._owned_role_dirs
        )
        self._source_install = source_install

        payload_bytes, payload_files = clone_payload_bytes(source_install)
        expected_copy_bytes = payload_bytes * len(self._owned_role_dirs)
        artifact_allowance = max(
            HARNESS_ARTIFACT_ALLOWANCE_BYTES, payload_bytes // 8
        )
        required_free = (
            expected_copy_bytes
            + artifact_allowance
            + HARNESS_SAFETY_MARGIN_BYTES
        )
        before = shutil.disk_usage(self.output_root).free
        cleanup: dict[str, object] = {
            "attempted": False,
            "result": {"scanned": 0, "pruned": [], "errors": {}},
        }
        after = before
        if after < required_free:
            cleanup["attempted"] = True
            retained = (
                max(0, self.args.keep_workdir_limit - 1)
                if self.args.keep_workdir_on_failure
                else 0
            )
            cleanup["result"] = prune_stale_owned_game_clones(
                self.output_root, retained, exclude=self.run_root
            )
            after = shutil.disk_usage(self.output_root).free
        preflight = {
            "clone_payload_bytes_per_role": payload_bytes,
            "clone_payload_files_per_role": payload_files,
            "role_count": len(self._owned_role_dirs),
            "expected_copy_bytes": expected_copy_bytes,
            "artifact_allowance_bytes": artifact_allowance,
            "safety_margin_bytes": HARNESS_SAFETY_MARGIN_BYTES,
            "required_free_bytes": required_free,
            "free_before_bytes": before,
            "free_after_cleanup_bytes": after,
            "owned_stale_cleanup": cleanup,
        }
        self.report["disk_preflight"] = preflight
        if after < required_free:
            raise HarnessFailure(
                "insufficient free space for a safe live-game E2E scenario: "
                f"required={required_free} free={after} "
                f"clonePayloadPerRole={payload_bytes} roles="
                f"{len(self._owned_role_dirs)} artifactAllowance="
                f"{artifact_allowance} safetyMargin="
                f"{HARNESS_SAFETY_MARGIN_BYTES}; only marker-owned stale "
                "game clones were eligible for cleanup"
            )
        self.event("disk-preflight-passed", **preflight)

    def process_exit_information(self) -> dict[str, object]:
        roles: dict[str, object] = {}
        for role in self.artifact_roles():
            process = role.process
            roles[role.name] = {
                "caster_pid": None if process is None else process.pid,
                "caster_exit_code": (
                    None if process is None else process.poll()
                ),
                "game_pid": (
                    None if role.game_memory is None else role.game_memory.pid
                ),
            }
        return {
            "roles": roles,
            "relay_pid": None if self.relay is None else self.relay.pid,
            "relay_exit_code": (
                None if self.relay is None else self.relay.poll()
            ),
        }

    def changed_file_manifest(self) -> dict[str, object]:
        if self._source_install is None:
            return {"status": "source-install-unavailable", "roles": {}}

        skip_dirs = {"logs", "replays", "__pycache__"}
        skip_names = {
            "qoh99_netplay.dll",
            "qoh99caster.exe",
            "qoh99caster.ini",
            "ddraw.dll",
            "ddraw_wrapper.log",
            "qoherror.txt",
        }

        def source_files() -> dict[str, Path]:
            result: dict[str, Path] = {}
            for root, dirs, names in os.walk(self._source_install):
                dirs[:] = [d for d in dirs if d.casefold() not in skip_dirs]
                root_path = Path(root)
                for name in names:
                    path = root_path / name
                    if (
                        name.casefold() in skip_names
                        or path.suffix.casefold() in {".log", ".dmp"}
                    ):
                        continue
                    result[str(path.relative_to(self._source_install))] = path
            return result

        baseline = source_files()
        role_manifests: dict[str, object] = {}
        for clone in owned_clone_paths(self.run_root, self.output_root):
            cloned = {
                str(path.relative_to(clone)): path
                for path in clone.rglob("*")
                if path.is_file()
            }
            changes: list[dict[str, object]] = []
            for relative in sorted(set(baseline) | set(cloned)):
                source = baseline.get(relative)
                candidate = cloned.get(relative)
                if source is None:
                    changes.append({"path": relative, "kind": "created"})
                elif candidate is None:
                    changes.append({"path": relative, "kind": "deleted"})
                else:
                    source_stat = source.stat()
                    candidate_stat = candidate.stat()
                    if (
                        source_stat.st_size != candidate_stat.st_size
                        or source_stat.st_mtime_ns != candidate_stat.st_mtime_ns
                    ):
                        changes.append(
                            {"path": relative, "kind": "modified"}
                        )
            role_manifests[clone.parent.name] = {
                "clone": str(clone),
                "changed_files": changes,
            }
        return {"status": "complete", "roles": role_manifests}

    def preserve_compact_evidence(self) -> dict[str, object]:
        copied: list[str] = []
        errors: dict[str, str] = {}
        for clone in owned_clone_paths(self.run_root, self.output_root):
            role_root = clone.parent
            source_logs = clone / "logs"
            retained = role_root / "game-logs"
            try:
                if source_logs.is_dir():
                    shutil.copytree(source_logs, retained, dirs_exist_ok=True)
                    copied.append(str(retained))
                for filename in (
                    "ddraw_wrapper.log",
                    "qoherror.txt",
                    "qoh99_netplay.log",
                ):
                    source = clone / filename
                    if source.is_file():
                        retained.mkdir(parents=True, exist_ok=True)
                        target = retained / filename
                        shutil.copy2(source, target)
                        copied.append(str(target))
            except OSError as error:
                errors[str(clone)] = f"{type(error).__name__}: {error}"
        return {"copied": copied, "errors": errors}

    def finalize_disposable_artifacts(self, failed: bool) -> list[str]:
        if self._artifact_finalized or not self.run_root.is_dir():
            return []
        manifest = self.changed_file_manifest()
        evidence = self.preserve_compact_evidence()
        retain_full = failed and self.args.keep_workdir_on_failure
        removal: dict[str, object] = {"deleted": [], "errors": {}}
        caster_removal: dict[str, object] = {"deleted": [], "errors": {}}
        bounded: dict[str, object] = {"scanned": 0, "pruned": [], "errors": {}}
        if not retain_full:
            removal = remove_owned_game_clones(self.run_root, self.output_root)
            caster_removal = remove_owned_caster_copies(
                self.run_root, self.output_root
            )
        else:
            bounded = prune_stale_owned_game_clones(
                self.output_root, self.args.keep_workdir_limit - 1,
                exclude=self.run_root,
            )
        failures: list[str] = []
        if evidence["errors"]:
            failures.append("compact evidence copy failed")
        if removal["errors"]:
            failures.append("owned game clone removal failed")
        if caster_removal["errors"]:
            failures.append("owned caster copy removal failed")
        if bounded["errors"]:
            failures.append("bounded stale-clone cleanup failed")
        self.report["artifact_lifecycle"] = {
            "ownership_marker": str(
                self.run_root / HARNESS_OWNERSHIP_MARKER
            ),
            "changed_file_manifest": manifest,
            "compact_evidence": evidence,
            "retain_full_clone": retain_full,
            "clone_removal": removal,
            "caster_copy_removal": caster_removal,
            "bounded_retention": bounded,
            "complete": not failures,
        }
        self._artifact_finalized = True
        return failures

    def host_for(self, role_name: str) -> object:
        return self.hosts[self.role_host_names.get(role_name, "")]

    def host_key_for(self, role_name: str) -> str:
        return self.role_host_names.get(role_name, "")

    def is_remote_role(self, role_name: str) -> bool:
        return bool(self.role_host_names.get(role_name))

    def role_root_for(self, role_name: str, letter: str) -> Path:
        key = self.host_key_for(role_name)
        if not key:
            return self.run_root / letter
        return self.hosts[key].path(
            f"{self._remote_run_roots[key]}\\{letter}"
        )

    def mirror_root_for(self, letter: str) -> Path:
        return self.run_root / letter

    def source_install_for(self, role_name: str) -> Path:
        key = self.host_key_for(role_name)
        if not key:
            assert self._source_install is not None
            return self._source_install
        return self._remote_source_installs[key]

    def staged_caster_for(self, role_name: str) -> Path:
        key = self.host_key_for(role_name)
        if not key:
            return Path(self.args.caster).resolve()
        return self._remote_staged[key]["caster"]

    def staged_wrapper_for(self, role_name: str) -> Path:
        key = self.host_key_for(role_name)
        if not key:
            return Path(self.args.wrapper).resolve()
        return self._remote_staged[key]["wrapper"]

    def staged_dll_for(self, role_name: str) -> str:
        key = self.host_key_for(role_name)
        if not key:
            return str(Path(self.args.dll).resolve())
        return str(self._remote_staged[key]["dll"])

    def endpoint_for(self, role_name: str) -> str:
        key = self.host_key_for(role_name)
        if not key:
            return self.controller_address
        configured = self.placement_for_host(key).get("address")
        if configured:
            return str(configured)
        return key.rsplit("@", 1)[-1]

    def placement_for_host(self, host_key: str) -> dict[str, object]:
        for role_name, record in self.placement.items():
            if self.role_host_names.get(role_name) == host_key:
                return record
        return {}

    def barrier_deadline(self, seconds: float) -> float:
        if not self.distributed_mode:
            return seconds
        return seconds + self.args.remote_barrier_slack_ms / 1000.0

    def bring_up_hosts(self) -> None:
        if not self.distributed_mode:
            return
        self._distributed_report["skipped_coverage"] = [
            {
                "item": "confirmed-hash debugger observer",
                "why": (
                    "Win32 debug events + Wow64GetThreadContext have no "
                    "faithful equivalent for a 32-bit Wine process, which "
                    "has no WOW64 layer at all"
                ),
                "still_covered_by": "the 36 local registrations",
                "replacement": "DLL log-derived confirmed-hash receipts",
            },
            {
                "item": "hidden-desktop WM_CLOSE path",
                "why": (
                    "window stations and desktops are machine-local kernel "
                    "objects with partial Wine support"
                ),
                "still_covered_by": "the 36 local registrations",
                "replacement": "Xvfb supplies invisibility instead",
            },
            {
                "item": "--split-game-affinity",
                "why": (
                    "the mask is planned from the controller's own CPUs, and "
                    "keeping two games off one core is moot across machines"
                ),
                "still_covered_by": "the local affinity registrations",
                "replacement": None,
            },
            {
                "item": "wall-clock phase fps floor on remote roles",
                "why": (
                    "RPC round-trip enters the denominator and biases the "
                    "measured rate downward"
                ),
                "still_covered_by": "the 36 local registrations",
                "replacement": (
                    "validate_phase_fps_log_derived, from the DLL's own "
                    "microsecond clock, asserted at "
                    "--remote-minimum-canonical-fps"
                ),
            },
            {
                "item": "--network-profile impairment",
                "why": (
                    "the profile ledger is DLL evidence; tc/netem is "
                    "invisible to it and no assertion measures netem"
                ),
                "still_covered_by": "the impaired local registrations",
                "replacement": None,
            },
        ]
        scratch = self.output_root / ".ssh-scratch"
        scratch.mkdir(parents=True, exist_ok=True)
        agent_exe = Path(self.args.agent).resolve()
        if not agent_exe.is_file():
            raise HarnessFailure(
                "distributed mode requires --agent naming the built "
                f"qoh99_e2e_agent.exe; {agent_exe} does not exist"
            )

        for role_name, record in self.placement.items():
            key = str(record["target"])
            self.role_host_names[role_name] = key
            if key in self.hosts:
                continue
            host = rolehost.SshRoleHost(
                name=key,
                target=key,
                lab=str(record["lab"]),
                agent_port=int(record["agent_port"]),
                drive=str(record["drive"]),
                display=str(record["display"]),
                password=record.get("password"),
                scratch=scratch,
                failure=HarnessFailure,
            )
            wrapper_digest = host.deploy_wrapper()
            drive_root = host.drive_root()
            staged_digests = host.stage(
                {
                    "agent": agent_exe,
                    "caster": Path(self.args.caster).resolve(),
                    "dll": Path(self.args.dll).resolve(),
                    "wrapper": Path(self.args.wrapper).resolve(),
                    "map": Path(self.args.dll_map).resolve(),
                },
                f"{drive_root}/bin",
            )
            staged_digests["remote_wrapper_script"] = wrapper_digest
            host.start(agent_exe, {})
            self.hosts[key] = host

            drive = str(record["drive"]).rstrip(":") + ":"
            self._remote_run_roots[key] = f"{drive}\\{self.run_root.name}"
            self._remote_staged[key] = {
                "agent": host.path(f"{drive}\\bin\\{agent_exe.name}"),
                "caster": host.path(
                    f"{drive}\\bin\\{Path(self.args.caster).name}"
                ),
                "dll": host.path(
                    f"{drive}\\bin\\{Path(self.args.dll).name}"
                ),
                "wrapper": host.path(
                    f"{drive}\\bin\\{Path(self.args.wrapper).name}"
                ),
            }
            source = record.get("source_install")
            self._remote_source_installs[key] = host.path(
                str(source) if source else f"{drive}\\game"
            )
            host.mkdir(
                host.path(self._remote_run_roots[key]),
                parents=True,
                exist_ok=False,
            )
            handshake = host.handshake()
            self._require_agent_config_model(key, handshake)
            handshake["staged_sha256"] = staged_digests
            handshake["run_root"] = self._remote_run_roots[key]
            self._distributed_report["hosts"][key] = handshake
            self.event(
                "remote-host-ready",
                target=key,
                run_root=self._remote_run_roots[key],
                agent=handshake.get("agent_build"),
                protocol=handshake.get("protocol"),
            )

    def _require_agent_config_model(
        self, key: str, handshake: dict[str, object]
    ) -> None:
        expected = {
            "image_size": QOH_CONFIG_IMAGE_SIZE,
            "profile_base": QOH_CONFIG_PROFILE_BASE,
            "profile_stride": QOH_CONFIG_PROFILE_STRIDE,
            "profile_count": QOH_CONFIG_PROFILE_COUNT,
            "nosound_offset": QOH_CONFIG_NOSOUND_OFFSET,
            "nosound_bit": QOH_CONFIG_NOSOUND_BIT,
        }
        model = handshake.get("config_model")
        if not isinstance(model, dict):
            raise HarnessFailure(
                f"{key}: the remote agent published no config_model; it "
                "predates the QOHcnf.key layout check and cannot be trusted "
                "to write a config image. Rebuild qoh99_e2e_agent from this "
                "tree."
            )
        mismatched = {
            name: {"controller": value, "agent": model.get(name)}
            for name, value in expected.items()
            if int(model.get(name, -1)) != int(value)
        }
        if mismatched:
            raise HarnessFailure(
                f"{key}: the remote agent's QOHcnf.key model disagrees with "
                f"this harness: {mismatched}. config_mutate would write at a "
                "wrong offset in a real config image and confirm it with a "
                "readback using the same wrong arithmetic"
            )
        self._distributed_report.setdefault("config_model", {})[key] = model

    def verify_remote_build_identity(self) -> None:
        if not self.distributed_mode:
            return
        local = {
            "caster": sha256(Path(self.args.caster).resolve()),
            "dll": sha256(Path(self.args.dll).resolve()),
            "wrapper": sha256(Path(self.args.wrapper).resolve()),
        }
        local_game = Path(self.args.game).resolve()
        local_game_hash = sha256(local_game)
        local_game_size = local_game.stat().st_size
        evidence: dict[str, object] = {
            "controller": {**local, "game": local_game_hash},
            "hosts": {},
        }
        for key, host in self.hosts.items():
            if not key:
                continue
            staged = self._remote_staged[key]
            remote = {
                "caster": host.sha256(staged["caster"]),
                "dll": host.sha256(staged["dll"]),
                "wrapper": host.sha256(staged["wrapper"]),
            }
            mismatched = [
                name for name in local if remote[name] != local[name]
            ]
            if mismatched:
                raise HarnessFailure(
                    f"{key}: staged runtime artifacts differ from the "
                    f"controller's build: {mismatched} "
                    f"(controller={local} remote={remote}); the session "
                    "would be refused by the product's own build gate, and "
                    "a differing DLL would silently corrupt every RVA read"
                )
            pe = host.stat(staged["dll"])
            remote_game = self._remote_source_installs[key] / local_game.name
            remote_game_hash = host.sha256(remote_game)
            remote_game_stat = host.stat(remote_game)
            if (
                remote_game_hash != local_game_hash
                or int(remote_game_stat.get("size", 0)) != local_game_size
            ):
                raise HarnessFailure(
                    f"{key}: the remote game executable is a DIFFERENT BUILD "
                    f"({remote_game}: sha256={remote_game_hash} "
                    f"size={remote_game_stat.get('size')}) than this "
                    f"harness's reference ({local_game}: "
                    f"sha256={local_game_hash} size={local_game_size}). "
                    "The shipped content gate would refuse the session with "
                    "a cross-build profile mismatch (for example "
                    "'qoh99-december-candidate' vs 'qoh99-se1126'); stage "
                    "the SE reference install on the remote box. Never stage "
                    "from a checked-in manifest.sha256 -- the one on the lab "
                    "box records stale dist digests and does not describe "
                    "its own directory."
                )
            evidence["hosts"][key] = {
                **remote,
                "game": remote_game_hash,
                "game_size": remote_game_stat.get("size"),
                "dll_pe": pe,
            }
        self.report["remote_build_identity"] = evidence
        self.event("remote-build-identity-verified", evidence=evidence)

    def _display_path(self, role: Role, path: Path) -> str:
        if self.is_remote_role(role.name):
            return str(path)
        return str(path.resolve())

    def _create_role(
        self, name: str, letter: str, game_name: str
    ) -> Role:
        host = self.host_for(name)
        role_root = self.role_root_for(name, letter)
        host.mkdir(role_root)
        caster_copy = role_root / "qoh99caster.exe"
        host.copy(self.staged_caster_for(name), caster_copy)
        install = role_root / "g"
        stats = host.clone_game_install(
            self.source_install_for(name), install
        )
        if self.args.strip_character_voices == name:
            stats["character_voices_removed"] = strip_character_voice_assets(
                host, install
            )
        host.copy(self.staged_wrapper_for(name), install / "ddraw.dll")
        mirror = None
        guard = None
        if self.is_remote_role(name):
            mirror = rolehost.LogMirror(
                role_name=name,
                remote_root=role_root,
                local_root=self.mirror_root_for(letter),
                host=host,
                failure=HarnessFailure,
            )
            self._mirrors.append(mirror)
            guard = self.evidence_guard
        role = Role(
            name=name,
            root=role_root,
            install=install,
            game=install / game_name,
            caster_exe=caster_copy,
            stdout_path=role_root / "caster.stdout.log",
            stderr_path=role_root / "caster.stderr.log",
            host=host,
            mirror=mirror,
            guard=guard,
        )
        self._install_stats[name] = stats
        return role

    def prepare(self) -> None:
        source_game = Path(self.args.game).resolve()
        source_install = source_game.parent
        source_wrapper = Path(self.args.wrapper).resolve()
        required = (
            source_game,
            source_install / "System" / "QOHcnf.key",
            source_wrapper,
        )
        missing = [str(path) for path in required if not path.is_file()]
        if missing:
            raise HarnessFailure(
                "live game install is incomplete; missing " + ", ".join(missing)
            )
        self.prepare_workspace(source_install)
        self.bring_up_hosts()
        self.verify_remote_build_identity()
        source_audio_paths = tuple(
            path
            for path in (
                source_install / "System" / "QOHcnf.key",
                source_install / "LocalConfig" / "FCommand.txt",
                source_install / "FCommand.txt",
            )
            if path.is_file()
        )
        source_audio_hashes = {
            str(path): sha256(path) for path in source_audio_paths
        }

        install_stats: dict[str, object] = {}
        for name in ("host", "client"):
            role = self._create_role(name, name[0], source_game.name)
            install_stats[name] = self._install_stats.pop(name)
            self.roles[name] = role

        self._spectator_started = False
        self._started_spectator_names: set[str] = set()
        self._late_join_at = None
        if self.args.spectator:
            for index in range(self.args.spectator_count):
                name = (
                    "spectator" if index == 0 else f"spectator{index + 1}"
                )
                role = self._create_role(
                    name,
                    "s" if index == 0 else f"v{index + 1}",
                    source_game.name,
                )
                install_stats[name] = self._install_stats.pop(name)
                self.spectators.append(role)
            self.spectator = self.spectators[0]

        if self.args.spectator_capacity_readmit:
            capacity_roles = (
                ("spectator_denied", "d"),
                ("spectator_replacement", "x"),
            )
            for name, directory in capacity_roles:
                role = self._create_role(name, directory, source_game.name)
                install_stats[name] = self._install_stats.pop(name)
                if name == "spectator_denied":
                    self.capacity_denied_role = role
                else:
                    self.capacity_replacement_role = role

        if self.args.spectator_capacity_readmit:
            host_role = self.roles["host"]
            host_role.host.write_text(
                host_role.root / "qoh99caster.ini",
                "[qoh99caster]\n"
                "spectatorAdmission=1\n"
                "spectatorMaxViewers=1\n",
                "ascii",
            )

        wrapper_sha256 = sha256(source_wrapper)
        wrapper_installs: dict[str, object] = {}
        for role in self.prepared_roles():
            installed_wrapper = role.install / "ddraw.dll"
            installed_sha256 = role.host.sha256(installed_wrapper)
            if installed_sha256 != wrapper_sha256:
                raise HarnessFailure(
                    f"{role.name} installed wrapper differs from the "
                    f"requested built artifact: source={wrapper_sha256} "
                    f"installed={installed_sha256}"
                )
            wrapper_installs[role.name] = {
                "path": self._display_path(role, installed_wrapper),
                "sha256": installed_sha256,
            }

        host_role = self.roles["host"]
        client_role = self.roles["client"]
        host_key = host_role.install / "System" / "QOHcnf.key"
        client_key = client_role.install / "System" / "QOHcnf.key"
        if self.args.config_variant == "bak":
            backup = self.source_install_for("client") / "System" / (
                "QOHcnf.key.bak"
            )
            backup_stat = client_role.host.stat(backup)
            if not backup_stat["exists"] or backup_stat["is_dir"]:
                raise HarnessFailure(
                    f"config-isolation variant requires {backup}"
                )
            client_role.host.copy(backup, client_key)

        stress_readback: dict[str, object] | None = None
        if self.args.config_variant == "config-stress":
            stress_readback = {}
            for name, key in (
                ("host", host_key),
                ("client", client_key),
            ):
                spec = QOH_CONFIG_STRESS_SPEC[name]
                profile = int(spec["profile"])
                values = dict(spec["values"])
                stress_readback[name] = self.roles[name].host.config_mutate(
                    key, profile, values
                )

            host_values = dict(QOH_CONFIG_STRESS_SPEC["host"]["values"])
            client_values = dict(
                QOH_CONFIG_STRESS_SPEC["client"]["values"]
            )
            host_match = tuple(host_values[offset] for offset in (36, 40, 44, 48))
            client_match = tuple(
                client_values[offset] for offset in (36, 40, 44, 48)
            )
            valid_match = lambda value: (
                value[0] <= 2
                and value[1] <= 4
                and value[2] == 0
                and value[3] <= 2
            )
            if (
                not valid_match(host_match)
                or not valid_match(client_match)
                or host_match == client_match
            ):
                raise HarnessFailure(
                    "config-stress match settings are not valid divergent "
                    f"QNP5 tuples: host={host_match} client={client_match}"
                )
            for name, values in (
                ("host", host_values),
                ("client", client_values),
            ):
                zero = [
                    offset
                    for offset in QOH_CONFIG_NORMALIZED_OFFSETS
                    if values[offset] == 0
                ]
                if zero:
                    raise HarnessFailure(
                        f"config-stress {name} normalized fields are not "
                        f"all nonzero: {zero}"
                    )

        automatch_config: dict[str, object] | None = None
        if self.args.full_match_rematch:
            automatch_config = {}
            for role in self.runtime_roles():
                key = role.install / "System" / "QOHcnf.key"
                selected = role.host.config_snapshot(key, (48,))
                automatch_config[role.name] = role.host.config_mutate(
                    key,
                    int(selected["profile"]),
                    {48: 1},
                )

        test_audio = {
            role.name: role.host.mute_test_install_audio(role.install)
            for role in self.prepared_roles()
        }
        source_audio_hashes_after = {
            str(path): sha256(path) for path in source_audio_paths
        }
        if source_audio_hashes_after != source_audio_hashes:
            raise HarnessFailure(
                "test audio muting changed the source game install"
            )
        host_hash = host_role.host.sha256(host_key)
        client_hash = client_role.host.sha256(client_key)
        if self.args.config_variant == "identical" and host_hash != client_hash:
            raise HarnessFailure("baseline install keys are unexpectedly different")
        if self.args.config_variant == "bak" and host_hash == client_hash:
            raise HarnessFailure(
                "QOHcnf.key.bak does not differ from QOHcnf.key"
            )
        if (
            self.args.config_variant == "config-stress"
            and host_hash == client_hash
        ):
            raise HarnessFailure(
                "config-stress player QOHcnf.key images are identical"
            )
        config_before: dict[str, object] = {}
        for name, role in self.roles.items():
            key = role.install / "System" / "QOHcnf.key"
            role.config_before = role.host.config_snapshot(
                key, self.config_persistence_offsets
            )
            config_before[name] = role.config_before
        asset_path_preflight = {
            role.name: role.host.validate_asset_paths(role.install)
            for role in self.prepared_roles()
        }
        self.report["install_clone"] = install_stats
        self.report["wrapper_artifact"] = {
            "source_path": str(source_wrapper),
            "source_sha256": wrapper_sha256,
            "installed": wrapper_installs,
        }
        self.report["asset_path_preflight"] = asset_path_preflight
        self.report["test_audio"] = {
            "production_behavior_changed": False,
            "source_install_changed": False,
            "source_file_sha256": source_audio_hashes_after,
            "roles": test_audio,
        }
        self.report["config"] = {
            "host_key_sha256": host_hash,
            "client_key_sha256": client_hash,
            "different": host_hash != client_hash,
        }
        self.report["config_persistence"] = {
            "preserved_offsets": list(self.config_persistence_offsets),
            "before": config_before,
        }
        if stress_readback is not None:
            self.report["config_stress"] = {
                "scenario": QOH_CONFIG_STRESS_SPEC,
                "tracked_offsets": list(QOH_CONFIG_STRESS_OFFSETS),
                "normalized_offsets": list(
                    QOH_CONFIG_NORMALIZED_OFFSETS
                ),
                "authoritative_match": {
                    "timer": 2,
                    "ai": 4,
                    "life": 0,
                    "rounds_minus_one": 2,
                },
                "mutation_readback": stress_readback,
            }
        if automatch_config is not None:
            self.report["automatch_config"] = {
                "rounds_to_win_minus_one": 1,
                "readback": automatch_config,
            }
        self.report["confirmed_hash_symbol_rva"] = (
            f"0x{parse_map_rva(Path(self.args.dll_map)):X}"
        )
        self.event(
            "installs-prepared",
            host_key=host_hash,
            client_key=client_hash,
            stats=install_stats,
            asset_paths=asset_path_preflight,
            selected_profiles={
                name: snapshot["profile"]
                for name, snapshot in config_before.items()
            },
            test_audio_muted={
                name: bool(details["verified"])
                for name, details in test_audio.items()
            },
            wrapper={
                "source_path": str(source_wrapper),
                "sha256": wrapper_sha256,
                "installed_roles": list(wrapper_installs),
            },
        )

    def start_processes(self) -> None:
        relay_port = self.args.relay_port or reserve_relay_port(
            "0.0.0.0" if self.distributed_mode else "127.0.0.1"
        )
        game_port = self.args.game_port or reserve_game_port()
        self.report["relay_port"] = relay_port
        self.report["game_port"] = game_port

        if self.args.spectator:
            self.local_autopunch = LocalAutopunchRelay(
                bind=self.autopunch_bind,
                advertise=self.autopunch_advertise,
            )
            self.local_autopunch.start()
            self.report["local_autopunch"] = {
                "endpoint": (
                    f"{self.autopunch_advertise}:{self.local_autopunch.port}"
                ),
                "protocol": "register=2 lookup=8 mapping=8 punch=1",
            }
        if self.args.spectator_negative in (
            "unavailable",
            "unavailable-stale-artifact",
        ):
            if self.is_remote_role("host"):
                blocked_port = self.host_for("host").reserve_exclusive_udp()
                self.spectator_port_blocker = None
            else:
                blocker = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                    blocker.setsockopt(
                        socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1
                    )
                try:
                    blocker.bind(("0.0.0.0", 0))
                except OSError:
                    blocker.close()
                    raise
                blocked_port = int(blocker.getsockname()[1])
                if blocked_port == 0:
                    blocker.close()
                    raise HarnessFailure(
                        "exclusive unavailable-service fixture received port "
                        "zero"
                    )
                self.spectator_port_blocker = blocker
            self.report["spectator_unavailable_fixture"] = {
                "blocked_udp_port": blocked_port,
                "ownership": (
                    "harness exclusive ephemeral socket held for whole match"
                ),
                "source_selection": "explicit host test seam; not control+1",
            }
        if self.args.spectator_negative == "unavailable-stale-artifact":
            artifact = self.run_root / "h" / "stale-spectator-address.txt"
            artifact.write_text(
                "127.0.0.1:46000\n",
                encoding="ascii",
            )
            self._stale_spectator_artifact = artifact
            self.report["stale_spectator_artifact_fixture"] = {
                "path": str(artifact),
                "synthetic": True,
                "credential_persisted": False,
                "removed_before_current_session": False,
            }
            artifact.unlink()
            if artifact.exists():
                raise HarnessFailure(
                    "could not remove synthetic stale spectator artifact"
                )
            self.report["stale_spectator_artifact_fixture"][
                "removed_before_current_session"
            ] = True

        relay_stdout = self.run_root / "relay.stdout.log"
        relay_stderr = self.run_root / "relay.stderr.log"
        self.relay_stdout_handle = relay_stdout.open("wb")
        self.relay_stderr_handle = relay_stderr.open("wb")
        relay_cmd = relay_service_command(
            str(Path(self.args.relay).resolve()), relay_port, self.relay_bind
        )
        self.relay = subprocess.Popen(
            relay_cmd,
            cwd=self.run_root,
            stdout=self.relay_stdout_handle,
            stderr=self.relay_stderr_handle,
            creationflags=WindowsProcessControl.creation_flags,
        )
        self.report["commands"]["relay"] = relay_cmd
        if self.distributed_mode:
            self._wait_for_relay_bind(relay_stdout, relay_stderr, relay_port)
        else:
            time.sleep(0.35)
        if self.relay.poll() is not None:
            raise HarnessFailure(
                "`qoh99caster relay` exited during startup "
                f"(exit {self.relay.returncode}): "
                + read_text(relay_stderr)
            )

        base_env = os.environ.copy()
        base_env["QOH99_LOG"] = "1"
        base_env["QOH99_WRAPPER_LOG"] = "1"
        for name in (
            "QOH99_NP_TEST_AUTOMATCH",
            "QOH99_NP_TEST_PAUSE",
            "QOH99_NP_TEST_CHARACTERS",
            "QOH99_NP_TEST_FORCE_KO_FRAMES",
            "QOH99_NP_TEST_REMATCH_ANSWER",
            "QOH99_NP_TEST_DELAY_SCRIPT",
            "QOH99_NP_TEST_NET_PROFILE",
            "QOH99_NP_TEST_NET_SEED",
            "QOH99_NP_TEST_SPECTATOR_SOURCE_PORT",
            "QOH99_NP_TEST_SPECTATOR_STARTUP_FAILURE",
        ):
            base_env.pop(name, None)
        base_env["QOH99_RELAY_OVERRIDE"] = (
            f"{self.relay_advertise}:{relay_port}"
        )
        if self.local_autopunch is not None:
            base_env["QOH99_PUNCH_RELAY_OVERRIDE"] = (
                f"{self.autopunch_advertise}:{self.local_autopunch.port}"
            )

        def role_environment(role_index: int) -> dict[str, str]:
            env = base_env.copy()
            env["QOH99_PUBLIC_IP_OVERRIDE"] = self.endpoint_for(
                "host" if role_index == 0 else "client"
            )
            env["QOH99_NP_TEST_PAUSE"] = "live-e2e:frame-barrier"
            if self.args.f1_return_at_frame and role_index == 0:
                env["QOH99_NP_TEST_F1_RETURN_FRAME"] = str(
                    self.args.f1_return_at_frame
                )
            if self.args.full_match_rematch:
                env["QOH99_NP_TEST_AUTOMATCH"] = (
                    "live-e2e:native-rematch"
                )
                env["QOH99_NP_TEST_FORCE_KO_FRAMES"] = str(
                    self.args.force_ko_after_frames
                )
                if (
                    self.args.returned_charselect_idle_ticks
                    or self.args.require_character_switch
                ):
                    env["QOH99_NP_TEST_REMATCH_ANSWER"] = "leave"
                if self.args.p1_character_cursor is not None:
                    env["QOH99_NP_TEST_CHARACTERS"] = (
                        self.forced_character_selection()
                    )
            elif self.args.spectator:
                env["QOH99_NP_TEST_AUTOMATCH"] = (
                    "live-e2e:spectator-confirmed-transition"
                )
            delay_script = (
                self.args.host_delay_script
                if role_index == 0
                else self.args.client_delay_script
            )
            if delay_script:
                env["QOH99_NP_TEST_DELAY_SCRIPT"] = delay_script
            if self.args.network_profile != "none":
                env["QOH99_NP_TEST_NET_PROFILE"] = (
                    "live-e2e:" + self.args.network_profile
                )
                env["QOH99_NP_TEST_NET_SEED"] = str(
                    (self.args.network_seed + role_index)
                    & 0xFFFFFFFF
                )
            return env

        host = self.roles["host"]
        host_cmd = [
            str(host.caster_exe),
            "host",
            str(game_port),
            "--delay",
            str(self.args.host_delay),
            "--rollback",
            str(self.args.host_rollback),
            "--nickname",
            "QOH99Host",
            "--game",
            str(host.game),
            "--dll",
            self.staged_dll_for("host"),
        ]
        if self.args.route == "relay":
            host_cmd.insert(3, "--force-relay")
        if self.args.spectator_postgame_retain_ms:
            host_cmd.extend(
                [
                    "--spectator-retain-ms",
                    str(self.args.spectator_postgame_retain_ms),
                ]
            )
        if not self.args.visible_games:
            host_cmd.append("--hidden-desktop")
        host_env = role_environment(0)
        if self.args.spectator_negative in (
            "unavailable",
            "unavailable-stale-artifact",
        ):
            fixture = self.report["spectator_unavailable_fixture"]
            assert isinstance(fixture, dict)
            host_env["QOH99_NP_TEST_SPECTATOR_SOURCE_PORT"] = str(
                fixture["blocked_udp_port"]
            )
        elif self.args.spectator_negative == "unavailable-partial":
            host_env["QOH99_NP_TEST_SPECTATOR_STARTUP_FAILURE"] = (
                "after-source-acquisition"
            )
        self._start_role(host, host_cmd, host_env)
        if self.distributed_mode:
            self.wait_for_host_listening(self.endpoint_for("host"), game_port)
        else:
            time.sleep(0.35)

        client = self.roles["client"]
        client_cmd = [
            str(client.caster_exe),
            "join",
            f"{self.endpoint_for('host')}:{game_port}",
            "--delay",
            str(self.args.client_delay),
            "--rollback",
            str(self.args.client_rollback),
            "--nickname",
            "QOH99Client",
            "--game",
            str(client.game),
            "--dll",
            self.staged_dll_for("client"),
        ]
        if self.args.route == "relay":
            client_cmd.insert(3, "--force-relay")
        if not self.args.visible_games:
            client_cmd.append("--hidden-desktop")
        client_env = role_environment(1)
        if self.args.layout_command_mutation != "none":
            client_env["QOH99_NP_TEST_LAYOUT_COMMAND_HISTORY"] = (
                "live-e2e:" + self.args.layout_command_mutation
            )
        self._start_role(client, client_cmd, client_env)
        self.report["layout_command_mutation_env"] = {
            "host": None,
            "client": client_env.get(
                "QOH99_NP_TEST_LAYOUT_COMMAND_HISTORY"
            ),
        }
        self.report["test_environment"] = {
            "host": {
                "automatch": host_env.get("QOH99_NP_TEST_AUTOMATCH"),
                "character_cursors": host_env.get(
                    "QOH99_NP_TEST_CHARACTERS"
                ),
                "network_profile": host_env.get(
                    "QOH99_NP_TEST_NET_PROFILE"
                ),
                "network_seed": host_env.get("QOH99_NP_TEST_NET_SEED"),
                "delay_script": host_env.get(
                    "QOH99_NP_TEST_DELAY_SCRIPT"
                ),
                "spectator_source_port": host_env.get(
                    "QOH99_NP_TEST_SPECTATOR_SOURCE_PORT"
                ),
                "spectator_startup_failure": host_env.get(
                    "QOH99_NP_TEST_SPECTATOR_STARTUP_FAILURE"
                ),
            },
            "client": {
                "automatch": client_env.get("QOH99_NP_TEST_AUTOMATCH"),
                "character_cursors": client_env.get(
                    "QOH99_NP_TEST_CHARACTERS"
                ),
                "network_profile": client_env.get(
                    "QOH99_NP_TEST_NET_PROFILE"
                ),
                "network_seed": client_env.get("QOH99_NP_TEST_NET_SEED"),
                "delay_script": client_env.get(
                    "QOH99_NP_TEST_DELAY_SCRIPT"
                ),
                "spectator_source_port": client_env.get(
                    "QOH99_NP_TEST_SPECTATOR_SOURCE_PORT"
                ),
                "spectator_startup_failure": client_env.get(
                    "QOH99_NP_TEST_SPECTATOR_STARTUP_FAILURE"
                ),
            },
        }
        self.event(
            "casters-started",
            route=self.args.route,
            layout_command_mutation=self.args.layout_command_mutation,
            full_match_rematch=self.args.full_match_rematch,
            network_profile=self.args.network_profile,
            peer_settings=self.report["peer_settings"],
            mutation_role=(
                "client"
                if self.args.layout_command_mutation != "none"
                else None
            ),
        )

    def _wait_for_relay_bind(
        self,
        relay_stdout: Path,
        relay_stderr: Path,
        relay_port: int,
        timeout: float = 20.0,
    ) -> None:
        expected = re.compile(
            r"qoh99 relay service listening on\s+"
            + re.escape(self.relay_bind)
            + r":"
            + str(int(relay_port))
            + r"\b",
            re.IGNORECASE,
        )
        deadline = time.monotonic() + self.barrier_deadline(timeout)
        while time.monotonic() < deadline:
            if self.relay is not None and self.relay.poll() is not None:
                return
            text = read_text(relay_stdout) + read_text(relay_stderr)
            if expected.search(text):
                self.event(
                    "relay-bind-observed",
                    bind=self.relay_bind,
                    port=int(relay_port),
                    advertised=self.relay_advertise,
                )
                return
            time.sleep(0.05)
        raise HarnessFailure(
            f"`qoh99caster relay` did not report a bind on {self.relay_bind}:"
            f"{relay_port} within {timeout:.0f}s; expected "
            f"{expected.pattern!r}. stdout: "
            f"{tail(read_text(relay_stdout), 40)} stderr: "
            f"{tail(read_text(relay_stderr), 40)}"
        )

    def wait_for_host_listening(
        self, host_ip: str, game_port: int, timeout: float = 45.0
    ) -> None:
        if self.args.route == "relay":
            self._wait_for_host_relay_room(timeout)
            return
        caster = str(Path(self.args.caster).resolve())
        deadline = time.monotonic() + self.barrier_deadline(timeout)
        last_output = ""
        while time.monotonic() < deadline:
            completed = subprocess.run(
                [caster, "status", f"{host_ip}:{game_port}"],
                cwd=self.run_root,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=15,
                check=False,
                text=True,
                encoding="utf-8",
                errors="replace",
                creationflags=WindowsProcessControl.creation_flags,
            )
            last_output = completed.stdout
            if completed.returncode == 0:
                self.event(
                    "host-listening",
                    endpoint=f"{host_ip}:{game_port}",
                    probe="qoh99caster status",
                )
                return
            self._scan_failures()
            time.sleep(0.25)
        raise HarnessFailure(
            f"host did not accept `qoh99caster status {host_ip}:{game_port}` "
            f"within {timeout:.0f}s; last probe output: {last_output.strip()!r}"
        )

    def _wait_for_host_relay_room(self, timeout: float) -> None:
        host_role = self.roles["host"]
        expected_relay = f"{self.relay_advertise}:{self.args.relay_port}"
        room = re.compile(
            r"\[host-auto\] relay="
            + re.escape(expected_relay)
            + r" room=(?P<room>[0-9A-Fa-f]+)"
        )
        registered = "[host-auto] relay room registered/waiting"
        deadline = time.monotonic() + self.barrier_deadline(timeout)
        while time.monotonic() < deadline:
            text = host_role.client_text()
            match = room.search(text)
            if match and registered in text:
                self.event(
                    "host-relay-room-registered",
                    relay=expected_relay,
                    room=match.group("room"),
                    probe="host caster relay registration",
                )
                return
            self._scan_failures()
            time.sleep(0.25)
        raise HarnessFailure(
            f"host did not register a relay room on {expected_relay} within "
            f"{timeout:.0f}s; `--route relay` disables the direct player "
            "listener, so the room registration is the only readiness signal "
            f"that exists. Last host client.log tail: "
            f"{tail(host_role.client_text(), 30)}"
        )

    def _start_role(
        self,
        role: Role,
        command: list[str],
        env: dict[str, str],
        *,
        report_command: list[str] | None = None,
    ) -> None:
        role.stdout_handle = role.host.open_log_handle(role.stdout_path)
        role.stderr_handle = role.host.open_log_handle(role.stderr_path)
        role.process = role.host.spawn(
            command,
            role.root,
            env,
            role.stdout_handle,
            role.stderr_handle,
        )
        self.report["commands"][role.name] = (
            command if report_command is None else report_command
        )

    def spectator_is_live(self) -> bool:
        return self.spectator is not None and self._spectator_started

    def spectator_route_for_role(self, role: Role) -> str:
        if self.args.spectator_routes and role in self.spectators:
            return self.args.spectator_routes[self.spectators.index(role)]
        return self.args.spectator_route

    def arm_late_join(self) -> None:
        if (
            self.spectator is not None
            and not self._spectator_started
            and self.args.spectator_join_after_seconds > 0.0
            and self._late_join_at is None
        ):
            self._late_join_at = (
                time.monotonic() + self.args.spectator_join_after_seconds
            )

    def maybe_late_join(self) -> None:
        if self._late_join_at is None or self._spectator_started:
            return
        if time.monotonic() < self._late_join_at:
            return
        self._late_join_at = None
        self.event(
            "spectator-late-join-start",
            after_seconds=self.args.spectator_join_after_seconds,
            states=self.current_states(),
        )
        self.start_spectator()

    def start_spectator(
        self,
        role: Role | None = None,
        timeout: float = 45.0,
        expected_failure: int | None = None,
        spectate_address_override: str | None = None,
    ) -> None:
        role = self.spectator if role is None else role
        if role is None:
            return

        access_deadline = time.monotonic() + min(timeout, 20.0)
        access_match: re.Match[str] | None = None
        while time.monotonic() < access_deadline:
            self._pump_observer()
            self._scan_failures()
            host = self.roles["host"]
            evidence = host.stdout_text()
            access_matches = list(
                CASTER_SPECTATOR_ACCESS_READY_RE.finditer(evidence)
            )
            if access_matches:
                access_match = access_matches[-1]
                break
            time.sleep(0.02)
        if access_match is None:
            raise HarnessFailure(
                "host did not emit the spectator access-ready contract on "
                "stdout"
            )

        session = int(access_match.group("session"))
        if session > 0xFFFFFFFF:
            raise HarnessFailure(
                "host emitted an overflowing spectator session id"
            )
        local_player_address = (
            f"{self.endpoint_for('host')}:{int(self.report['game_port'])}"
        )
        if spectate_address_override is None:
            spectate_address = local_player_address
            if self._spectator_player_address is None:
                self._spectator_player_address = spectate_address
            elif spectate_address != self._spectator_player_address:
                raise HarnessFailure(
                    "host replaced its player address during one live "
                    "capacity/readmission session"
                )
            address_source = "the host player ip:port"
        else:
            if (
                self._spectator_player_address is None
                or spectate_address_override
                    != self._spectator_player_address
                or local_player_address
                    != self._spectator_player_address
            ):
                raise HarnessFailure(
                    "capacity/readmission attempt did not reuse the exact "
                    "original host player address"
                )
            spectate_address = spectate_address_override
            address_source = "byte-identical cached host player ip:port"

        route_selector = self.spectator_route_for_role(role)
        command = [
            str(role.caster_exe),
            "spectate",
            spectate_address,
            "--spectator-route",
            route_selector,
            "--nickname",
            "QOH99Viewer" + (
                ""
                if role is self.spectator
                else role.name.removeprefix("spectator")
            ),
            "--game",
            str(role.game),
            "--dll",
            self.staged_dll_for(role.name),
        ]
        if self.args.spectator_topology:
            command.append("--spectator-topology")
        if getattr(self.args, "spectator_latest", False):
            command.append("--latest")
        if not self.args.visible_games:
            command.append("--hidden-desktop")
        spectator_env = os.environ.copy()
        spectator_env["QOH99_LOG"] = "1"
        spectator_env["QOH99_WRAPPER_LOG"] = "1"
        for name in (
            "QOH99_NP_TEST_AUTOMATCH",
            "QOH99_NP_TEST_PAUSE",
            "QOH99_NP_TEST_CHARACTERS",
            "QOH99_NP_TEST_FORCE_KO_FRAMES",
            "QOH99_NP_TEST_REMATCH_ANSWER",
            "QOH99_NP_TEST_DELAY_SCRIPT",
            "QOH99_NP_TEST_NET_PROFILE",
            "QOH99_NP_TEST_NET_SEED",
            "QOH99_NP_INPUT_TEST",
        ):
            spectator_env.pop(name, None)
        if self.local_autopunch is not None:
            spectator_env["QOH99_PUNCH_RELAY_OVERRIDE"] = (
                f"{self.autopunch_advertise}:{self.local_autopunch.port}"
            )
        if self.args.spectator_input_mash:
            spectator_env["QOH99_NP_INPUT_TEST"] = "1"
        if self.args.full_match_rematch:
            spectator_env["QOH99_NP_TEST_AUTOMATCH"] = (
                "live-e2e:native-rematch"
            )
            spectator_env["QOH99_NP_TEST_FORCE_KO_FRAMES"] = str(
                self.args.force_ko_after_frames
            )
            if (
                self.args.returned_charselect_idle_ticks
                or self.args.require_character_switch
            ):
                spectator_env["QOH99_NP_TEST_REMATCH_ANSWER"] = "leave"
            if self.args.p1_character_cursor is not None:
                spectator_env["QOH99_NP_TEST_CHARACTERS"] = (
                    self.forced_character_selection()
                )
        if self.args.network_profile != "none":
            spectator_index = (
                self.spectators.index(role)
                if role in self.spectators
                else len(self._started_spectator_names)
            )
            spectator_env["QOH99_NP_TEST_NET_PROFILE"] = (
                "live-e2e:" + self.args.network_profile
            )
            spectator_env["QOH99_NP_TEST_NET_SEED"] = str(
                (
                    self.args.network_seed
                    + 2
                    + spectator_index
                )
                & 0xFFFFFFFF
            )
        self._start_role(
            role,
            command,
            spectator_env,
            report_command=command,
        )
        self.report.setdefault("test_environment", {})[role.name] = {
            "automatch": spectator_env.get("QOH99_NP_TEST_AUTOMATCH"),
            "character_cursors": spectator_env.get(
                "QOH99_NP_TEST_CHARACTERS"
            ),
            "network_profile": spectator_env.get(
                "QOH99_NP_TEST_NET_PROFILE"
            ),
            "network_seed": spectator_env.get(
                "QOH99_NP_TEST_NET_SEED"
            ),
            "delay_script": spectator_env.get(
                "QOH99_NP_TEST_DELAY_SCRIPT"
            ),
            "input_mash": spectator_env.get("QOH99_NP_INPUT_TEST"),
        }
        access_report = {
            "session": session,
            "address": spectate_address,
            "source": address_source,
            "explicit_loopback_candidate": True,
            "route_selector": route_selector,
            "player_address_reused": spectate_address_override is not None,
        }
        self.report.setdefault("spectator_accesses", {})[role.name] = (
            access_report
        )
        if role is self.spectator:
            self.report["spectator_access"] = access_report
        self.event(
            "spectator-caster-started",
            address=spectate_address,
            session=session,
            role_root=str(role.root),
            source=address_source,
            route_selector=route_selector,
        )

        game_deadline = time.monotonic() + timeout
        while time.monotonic() < game_deadline:
            self._pump_observer()
            if expected_failure is None:
                self._scan_failures()
            if role.process and role.process.poll() is not None:
                if expected_failure is not None:
                    break
                raise HarnessFailure(
                    "spectator caster exited before game creation "
                    f"with code {role.process.returncode}"
                )
            pid = child_game_pid(
                role.process.pid if role.process else 0,
                role.game.name,
            )
            if pid:
                role.game_memory = ProcessMemory(
                    pid,
                    request_set_information=self.args.split_game_affinity,
                )
                self._apply_game_affinity(role)
                role.scene_zero_since = time.monotonic()
                self.event(
                    "game-discovered",
                    role=role.name,
                    pid=pid,
                    game=str(role.game),
                )
                self._started_spectator_names.add(role.name)
                if role is self.spectator:
                    self._spectator_started = True
                if expected_failure is not None:
                    raise HarnessFailure(
                        f"{role.name} launched qoh99 despite expected "
                        f"definitive preflight failure={expected_failure}"
                    )
                return
            time.sleep(0.025)
        if expected_failure is not None:
            if role.process is None or role.process.poll() is None:
                raise HarnessFailure(
                    f"{role.name} did not exit after definitive preflight "
                    f"failure={expected_failure}"
                )
            evidence = role.stdout_text() + "\n" + role.client_text()
            matches = list(SPECTATOR_PREFLIGHT_RE.finditer(evidence))
            if not matches:
                raise HarnessFailure(
                    f"{role.name} denial produced no preflight contract"
                )
            final = matches[-1]
            if (
                final.group("ok") != "0"
                or int(final.group("failure")) != expected_failure
                or final.group("inconclusive") != "0"
            ):
                raise HarnessFailure(
                    f"{role.name} denial was not the required definitive "
                    f"failure={expected_failure}: {final.group(0)}"
                )
            if CASTER_SPECTATOR_LAUNCH_CONTRACT_RE.search(evidence):
                raise HarnessFailure(
                    f"{role.name} emitted a launch contract after denial"
                )
            self.report["spectator_definitive_denial"] = {
                "role": role.name,
                "expected_failure": expected_failure,
                "preflight": final.group(0),
                "caster_exit_code": role.process.returncode,
                "game_process_created": False,
                "admission_policy": "public",
                "fixture": self.args.spectator_negative,
            }
            self.event(
                "spectator-definitive-denial-verified",
                failure=expected_failure,
                line=final.group(0),
                caster_exit_code=role.process.returncode,
            )
            if role in self.spectators:
                self.spectators.remove(role)
            if role is self.spectator:
                self.spectator = None
                self._spectator_started = False
            if role not in self.auxiliary_roles:
                self.auxiliary_roles.append(role)
            return
        raise HarnessFailure(
            f"timed out waiting for {role.name} qoh99 child process"
        )

    def start_additional_spectators(self) -> None:
        if len(self.spectators) <= 1:
            return
        for role in self.spectators[1:]:
            if role.name in self._started_spectator_names:
                continue
            self.start_spectator(role)
        self.event(
            "all-spectator-casters-started",
            roles=[role.name for role in self.spectators],
            count=len(self.spectators),
        )

    def discover_games(self, timeout: float = 45.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for role in self.roles.values():
                if role.game_memory:
                    continue
                if role.process and role.process.poll() is not None:
                    raise HarnessFailure(
                        f"{role.name} caster exited before game creation "
                        f"with code {role.process.returncode}"
                    )
                pid = role.host.child_game_pid(
                    role.process.pid if role.process else 0,
                    role.game.name,
                )
                if pid:
                    role.game_memory = role.host.open_process(
                        pid,
                        request_set_information=self.args.split_game_affinity,
                    )
                    self._apply_game_affinity(role)
                    role.scene_zero_since = time.monotonic()
                    self.event(
                        "game-discovered",
                        role=role.name,
                        pid=pid,
                        game=str(role.game),
                    )
            if all(role.game_memory for role in self.roles.values()):
                return
            self._scan_failures()
            time.sleep(0.025)
        raise HarnessFailure("timed out waiting for both qoh99 child processes")

    def _scan_failures(self) -> None:
        self.refresh_evidence()
        for role in self.runtime_roles():
            if (
                self.players_closed_for_retention
                and role.name in self.roles
            ):
                continue
            if role.process and role.process.poll() is not None:
                role.caster_alive_hint = False
                if not role.game_memory or not role.game_memory.alive():
                    raise HarnessFailure(
                        f"{role.name} exited unexpectedly: "
                        f"caster={role.process.returncode}"
                    )
            elif role.process:
                role.caster_alive_hint = True
            if role.game_memory and not role.game_memory.alive():
                raise HarnessFailure(
                    f"{role.name} qoh99 pid={role.game_memory.pid} exited unexpectedly"
                )
            combined = role.dll_text() + "\n" + role.client_text()
            for marker in FORBIDDEN_MARKERS:
                found = (
                    marker in combined
                    if marker == "[CRASH]"
                    else marker.casefold() in combined.casefold()
                )
                if found:
                    raise HarnessFailure(
                        f"{role.name} log contains forbidden marker {marker!r}"
                    )
            if list(role.evidence_root().glob("logs/session_*/*.dmp")):
                raise HarnessFailure(f"{role.name} produced a crash dump")
            if role.game_memory:
                titles = role.host.window_titles(role.game_memory.pid)
                if any("NETPLAY STOPPED" in title for title in titles):
                    raise HarnessFailure(
                        f"{role.name} reached terminal title: {titles}"
                    )
                if any("エラー" in title for title in titles):
                    raise HarnessFailure(
                        f"{role.name} displayed the game's error dialog: {titles}"
                    )

    def wait_for_charselect(self, timeout: float = 70.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._scan_failures()
            ready = True
            for role in self.roles.values():
                assert role.game_memory
                state = game_state(role.game_memory)
                scene = state.get("scene")
                sub = state.get("scene3_sub")
                if scene == 0:
                    if role.scene_zero_since is None:
                        role.scene_zero_since = time.monotonic()
                    go_seen = "[netplay] GO:" in role.dll_text()
                    if (
                        go_seen
                        and time.monotonic() - role.scene_zero_since > 15.0
                    ):
                        raise HarnessFailure(
                            f"{role.name} semantic black/bootstrap: scene "
                            f"remained 0 after GO; state={state}"
                        )
                else:
                    role.scene_zero_since = None
                if not (
                    scene == 3
                    and sub == 2
                    and "CHARSELECT reached" in role.dll_text()
                ):
                    ready = False
            if ready:
                self.event(
                    "both-at-character-select",
                    states=self.current_states(),
                )
                return
            time.sleep(0.02)
        raise HarnessFailure(
            "timed out waiting for both games at scene=3/substate=2"
        )

    def force_verified_match_transition(self) -> None:
        if self.args.spectator:
            raise HarnessFailure(
                "spectator gates must use the confirmed-input character-select "
                "transition; out-of-band state writes are forbidden"
            )
        suspended_frame_threads: list[SuspendedThread] = []
        try:
            transition_frame = self.suspend_players_at_same_idle_frame(
                suspended_frame_threads
            )

            before_players = self.current_states()
            for role_name, state in before_players.items():
                if state.get("scene") != 3 or state.get("scene3_sub") != 2:
                    raise HarnessFailure(
                        f"{role_name} left character select while freezing: {state}"
                    )
            for role in self.roles.values():
                memory = role.game_memory
                assert memory
                for field, value in CHARSELECT_TRANSITION.items():
                    memory.write_u32(GAME_ADDR[field], value)
            after_players = self.current_states()
            for role_name, state in after_players.items():
                for field, value in CHARSELECT_TRANSITION.items():
                    if state.get(field) != value:
                        raise HarnessFailure(
                            f"{role_name} transition write {field} did not "
                            f"stick: expected={value} state={state}"
                        )

            self.event(
                "verified-charselect-transition-applied",
                canonical_frame=transition_frame,
                players_before=before_players,
                players_after=after_players,
                note=(
                    "only cursor/confirm/match-start/next-scene/mode controls; "
                    "ordinary scene-2 loader remains responsible for match "
                    "data; player-only compatibility gate"
                ),
            )

            for thread in reversed(suspended_frame_threads):
                thread.resume()
            suspended_frame_threads.clear()
            consume_deadline = time.monotonic() + 10.0
            consumed_states: dict[str, object] = {}
            while time.monotonic() < consume_deadline:
                self._scan_failures()
                ready = True
                for name, role in self.roles.items():
                    assert role.game_memory
                    state = game_state(role.game_memory)
                    consumed_states[name] = state
                    if state.get("scene") != 2:
                        ready = False
                if ready:
                    break
                time.sleep(0.002)
            else:
                raise HarnessFailure(
                    "peers did not consume the character-select transition "
                    "while parked at the barrier frame "
                    f"{transition_frame}: states={consumed_states!r}"
                )
            self.event(
                "transition-consumed-while-parked",
                frame=transition_frame,
                states={
                    name: {
                        "scene": state.get("scene"),
                        "match_sub": state.get("match_sub"),
                    }
                    for name, state in consumed_states.items()
                },
            )
        finally:
            for thread in reversed(suspended_frame_threads):
                try:
                    thread.resume()
                except Exception:
                    pass
            if self.test_pause_request_rva is not None:
                for role in self.roles.values():
                    try:
                        self.write_runtime_data_u32(
                            role, self.test_pause_request_rva, 0
                        )
                    except Exception:
                        pass

    def suspend_players_at_same_idle_frame(
        self,
        suspended: list[SuspendedThread],
        timeout: float = 8.0,
    ) -> int:
        roles = tuple(self.roles.values())
        if (
            self.test_pause_request_rva is None
            or self.test_pause_frame_rva is None
            or self.test_pause_held_frame_rva is None
            or self.test_pause_acknowledged_rva is None
        ):
            raise HarnessFailure(
                "cooperative frame barrier requires the DLL pause symbols; "
                "they were not present in the linker map"
            )

        frames = {
            role.name: self.runtime_data_u32(role, self.net_frame_rva)
            for role in roles
        }
        if any(value is None for value in frames.values()):
            raise HarnessFailure(
                "could not read canonical frames for the barrier: "
                + repr(frames)
            )
        target = max(int(v) for v in frames.values()) + 4

        for role in roles:
            self.write_runtime_data_u32(
                role, self.test_pause_frame_rva, target
            )
        for role in roles:
            self.write_runtime_data_u32(
                role, self.test_pause_request_rva, 1
            )

        deadline = time.monotonic() + timeout
        held = {role.name: None for role in roles}
        converged = False
        while time.monotonic() < deadline:
            self._scan_failures()
            ready = True
            for role in roles:
                ack = self.runtime_data_u32(
                    role, self.test_pause_acknowledged_rva
                )
                got = self.runtime_data_u32(
                    role, self.test_pause_held_frame_rva
                )
                held[role.name] = None if got is None else int(got)
                if not ack or held[role.name] != target:
                    ready = False
            if ready:
                converged = True
                break
            time.sleep(0.002)

        if not converged:
            states = self.current_states()
            for role in roles:
                self.write_runtime_data_u32(
                    role, self.test_pause_request_rva, 0
                )
            raise HarnessFailure(
                "peers did not converge on the cooperative barrier frame "
                + str(target)
                + " within "
                + ("%.1f" % timeout)
                + "s: held="
                + repr(held)
                + " startFrames="
                + repr(frames)
                + " states="
                + repr(states)
            )

        for role in roles:
            suspended.append(self.suspend_role_frame_thread(role))

        self.event(
            "players-parked-at-barrier-frame",
            frame=target,
            held=dict(held),
        )
        return target

    def suspend_role_at_idle_tick(
        self,
        role: Role,
        suspended: list[ProcessMemory],
        timeout: float = 3.0,
    ) -> None:
        if not role.game_memory:
            raise HarnessFailure(
                f"{role.name} game is unavailable for idle suspension"
            )
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.runtime_data_u32(role, self.net_frame_rva)
            gate = self.runtime_data_u32(role, self.net_gate_rva)
            if gate != 0:
                time.sleep(0.0005)
                continue
            role.game_memory.suspend()
            frame_after = self.runtime_data_u32(
                role, self.net_frame_rva
            )
            gate_after = self.runtime_data_u32(
                role, self.net_gate_rva
            )
            if frame_after == frame and gate_after == 0:
                suspended.append(role.game_memory)
                self.event(
                    "role-suspended-at-idle-timeline-frame",
                    role=role.name,
                    frame=frame,
                )
                return
            role.game_memory.resume()
            time.sleep(0.0005)
        raise HarnessFailure(
            f"could not suspend {role.name} at an idle canonical frame"
        )

    def wait_for_scene2(self, timeout: float = 35.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._scan_failures()
            states = self.current_states()
            if all(state.get("scene") == 2 for state in states.values()):
                self.event("both-entered-scene2", states=states)
                return
            if any(state.get("scene") == 0 for state in states.values()):
                raise HarnessFailure(
                    f"semantic black after character-select transition: {states}"
                )
            time.sleep(0.01)
        raise HarnessFailure("ordinary scene-2 loader did not start on both peers")

    def attach_hash_observer(self) -> None:
        if self.args.full_match_rematch:
            self.event(
                "confirmed-hash-native-log-observer-enabled",
                note=(
                    "the DLL logs successful authoritative comparisons; "
                    "no debugger breakpoint is attached to either game"
                ),
            )
            return
        processes = {
            role.game_memory.pid: role.game_memory
            for role in self.roles.values()
            if role.game_memory
        }
        rva = parse_map_rva(Path(self.args.dll_map))
        self.observer = ConfirmedHashObserver(
            processes,
            Path(self.args.dll).name,
            rva,
        )
        self.observer.attach()
        self.event(
            "confirmed-hash-observer-attached",
            pids=sorted(processes),
            rva=f"0x{rva:X}",
        )

    def _pump_observer(self) -> None:
        if not self.observer:
            return
        while self.observer.pump(0):
            pass

    def wait_for_layout_and_gameplay(self, timeout: float = 70.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._pump_observer()
            self._scan_failures()
            ready = True
            for role in self.roles.values():
                assert role.game_memory
                state = game_state(role.game_memory)
                layout = (
                    "[netplay] layout rendezvous complete"
                    in role.dll_text()
                )
                if not (
                    layout
                    and state.get("scene") == 2
                    and state.get("match_sub") == 0
                ):
                    ready = False
            if ready:
                for role in self.roles.values():
                    assert role.game_memory
                    role.layout_counter = role.game_memory.read_u32(
                        GAME_ADDR["frame_counter"]
                    )
                    role.layout_net_frames = self.runtime_data_u32(
                        role, self.net_frame_rva
                    )
                self.event(
                    "layout-rendezvous-and-gameplay",
                    states=self.current_states(),
                    counters={
                        name: role.layout_counter
                        for name, role in self.roles.items()
                    },
                    submitted_frames={
                        name: role.layout_net_frames
                        for name, role in self.roles.items()
                    },
                )
                self._require_equal_gameplay_entry_frames()
                return
            time.sleep(0.01)
        raise HarnessFailure(
            "both peers did not complete layout rendezvous in scene2/matchSub0"
        )

    def _require_equal_gameplay_entry_frames(self) -> None:
        entries: dict[str, int] = {}
        for name, role in self.roles.items():
            match = GAMEPLAY_ENTRY_RE.search(role.dll_text())
            if match is not None:
                entries[name] = int(match.group("frame"))
        if len(entries) < len(self.roles):
            self.event("gameplay-entry-frames", entries=entries, skew=None)
            missing = [
                name for name in self.roles if name not in entries
            ]
            raise HarnessFailure(
                "gameplay entry frames are unavailable for "
                f"{missing}; without both peers' `gameplay=0->1` evidence the "
                "canonical-frame skew gate cannot run and this run proves "
                "nothing about rollback or spectator playback"
            )
        skew = max(entries.values()) - min(entries.values())
        self.event("gameplay-entry-frames", entries=entries, skew=skew)
        self.report["gameplay_entry_frames"] = dict(entries)
        self.report["gameplay_entry_skew"] = skew
        if skew != 0:
            raise HarnessFailure(
                "peers entered gameplay on different canonical frames "
                f"({entries}); this run diverged before the first confirmed "
                "gameplay hash and cannot validate rollback or spectator "
                "playback"
            )

    def wait_for_gameplay_ticks(self, timeout: float = 55.0) -> None:
        deadline = time.monotonic() + timeout
        last_progress = time.monotonic()
        last_deltas = {name: 0 for name in self.roles}
        hash_detached = False
        self.arm_late_join()
        while time.monotonic() < deadline:
            self.maybe_late_join()
            self._pump_observer()
            self._scan_failures()
            states = self.current_states()
            deltas: dict[str, int] = {}
            for name, role in self.roles.items():
                state = states[name]
                if state.get("scene") != 2 or state.get("match_sub") != 0:
                    raise HarnessFailure(
                        f"{name} left gameplay before the required tick run: {state}"
                    )
                assert role.layout_net_frames is not None
                current_net_frames = self.runtime_data_u32(
                    role, self.net_frame_rva
                )
                if current_net_frames < role.layout_net_frames:
                    raise HarnessFailure(
                        f"{name} canonical net timeline moved backwards: "
                        f"layout={role.layout_net_frames} "
                        f"current={current_net_frames}"
                    )
                deltas[name] = current_net_frames - role.layout_net_frames
                role.final_net_frames = current_net_frames
                role.final_counter = int(state["frame_counter"])

            if any(deltas[name] > last_deltas[name] for name in deltas):
                last_progress = time.monotonic()
                last_deltas = dict(deltas)
            elif time.monotonic() - last_progress > 10.0:
                raise HarnessFailure(
                    f"semantic black/frozen gameplay: counters stopped; "
                    f"deltas={deltas} states={states}"
                )

            hash_counts = self.hash_counts_by_role()
            if (
                not hash_detached
                and all(count >= 1 for count in hash_counts.values())
            ):
                assert self.observer
                self.observer.detach_all()
                hash_detached = True
                self.event(
                    "gameplay-confirmed-hash-exchanged",
                    successful_receipts=hash_counts,
                )

            if (
                all(
                    delta >= self.args.gameplay_ticks
                    for delta in deltas.values()
                )
                and all(count >= 1 for count in hash_counts.values())
            ):
                self.report["gameplay_tick_deltas"] = deltas
                self.report["cosmetic_counter_deltas"] = {
                    name: (
                        (role.final_counter - role.layout_counter)
                        & 0xFFFFFFFF
                    )
                    for name, role in self.roles.items()
                    if role.final_counter is not None
                    and role.layout_counter is not None
                }
                self.report["confirmed_hash_receipts"] = hash_counts
                self.event(
                    "gameplay-tick-gate-complete",
                    deltas=deltas,
                    confirmed_hash_receipts=hash_counts,
                    states=states,
                )
                return
            time.sleep(0.005)
        raise HarnessFailure(
            "timed out waiting for gameplay ticks and confirmed hashes; "
            f"ticks={last_deltas} hashes={self.hash_counts_by_role()}"
        )

    def wait_for_native_match_and_rematch(
        self, timeout: float = 240.0
    ) -> None:
        metrics: dict[str, dict[str, object]] = {}
        previous: dict[str, dict[str, object]] = self.current_states()
        initial_hash_counts = self.hash_counts_by_role()
        pacing_started = time.monotonic()
        pacing: dict[str, dict[str, object]] = {
            name: {
                "initial_gameplay_start_frame": self.runtime_data_u32(
                    role, self.net_frame_rva
                ),
            }
            for name, role in self.roles.items()
        }
        rematch_hash_baselines: dict[str, int | None] = {
            name: None for name in self.roles
        }
        returned_dwell_complete = (
            self.args.returned_charselect_idle_ticks == 0
        )
        for name, state in previous.items():
            if "read_error" in state:
                raise HarnessFailure(
                    f"{name} state could not be sampled at lifecycle start: "
                    f"{state}"
                )
            if state.get("scene") != 2 or state.get("match_sub") != 0:
                raise HarnessFailure(
                    f"{name} was not in initial gameplay at lifecycle start: "
                    f"{state}"
                )
            if int(state["control_mask"]) != 0:
                raise HarnessFailure(
                    f"{name} did not enter a true two-human VS match: "
                    f"controlMask=0x{int(state['control_mask']):08X} "
                    f"state={state}"
                )
            metrics[name] = {
                "gameplay_entries": 1,
                "gameplay_exits": 0,
                "round_finish_entries": 0,
                "round_completions": 0,
                "charselect_returns": 0,
                "saw_charselect_return": False,
                "rematch_start_frame": None,
                "actions_p1": {int(state["p1_action"])},
                "actions_p2": {int(state["p2_action"])},
                "p1_x_min": int(state["p1_x"]),
                "p1_x_max": int(state["p1_x"]),
                "p2_x_min": int(state["p2_x"]),
                "p2_x_max": int(state["p2_x"]),
                "health_p1_min": int(state["health_p1"]),
                "health_p2_min": int(state["health_p2"]),
                "round_wins_p1_max": int(state["round_wins_p1"]),
                "round_wins_p2_max": int(state["round_wins_p2"]),
                "ko_sequencer_max": int(state["ko_sequencer"]),
                "round_counter_values": {int(state["round_counter"])},
                "winner_values": {int(state["winner"])},
                "match_substates": {int(state["match_sub"])},
            }

        spectator_previous: dict[str, object] | None = None
        spectator_metrics: dict[str, object] | None = None
        if self.spectator_is_live():
            if not self.spectator.game_memory:
                raise HarnessFailure(
                    "spectator game is unavailable at lifecycle start"
                )
            spectator_previous = game_state(self.spectator.game_memory)
            if "read_error" in spectator_previous:
                raise HarnessFailure(
                    "spectator state could not be sampled at lifecycle "
                    f"start: {spectator_previous}"
                )
            if not spectator_previous.get("alive"):
                raise HarnessFailure(
                    "spectator exited before lifecycle monitoring began: "
                    f"{spectator_previous}"
                )
            spectator_initial_gameplay = (
                spectator_previous.get("scene") == 2
                and spectator_previous.get("match_sub") == 0
            )
            spectator_metrics = {
                "gameplay_entries": (
                    1 if spectator_initial_gameplay else 0
                ),
                "gameplay_exits": 0,
                "round_finish_entries": 0,
                "round_completions": 0,
                "charselect_returns": 0,
                "saw_charselect_return": False,
                "rematch_start_frame": None,
                "round_wins_p1_max": int(
                    spectator_previous["round_wins_p1"]
                ),
                "round_wins_p2_max": int(
                    spectator_previous["round_wins_p2"]
                ),
                "ko_sequencer_max": int(
                    spectator_previous["ko_sequencer"]
                ),
                "round_counter_values": {
                    int(spectator_previous["round_counter"])
                },
                "scenes": {int(spectator_previous["scene"])},
                "match_substates": {
                    int(spectator_previous["match_sub"])
                },
            }

        def lifecycle_signature(
            states: dict[str, dict[str, object]]
        ) -> tuple[tuple[object, ...], ...]:
            return tuple(
                (
                    name,
                    state.get("scene"),
                    state.get("match_sub"),
                    state.get("scene3_sub"),
                    state.get("main_menu_sub"),
                    state.get("main_menu_cursor"),
                    state.get("p1_confirm"),
                    state.get("p2_confirm"),
                    state.get("round_wins_p1"),
                    state.get("round_wins_p2"),
                    state.get("round_counter"),
                    state.get("ko_sequencer"),
                    state.get("winner"),
                )
                for name, state in sorted(states.items())
            )

        deadline = time.monotonic() + timeout
        last_lifecycle_progress = time.monotonic()
        last_lifecycle_signature = lifecycle_signature(previous)
        self.arm_late_join()
        while time.monotonic() < deadline:
            self.maybe_late_join()
            self._pump_observer()
            self._scan_failures()
            states = self.current_states()
            read_failures = {
                name: state["read_error"]
                for name, state in states.items()
                if "read_error" in state
            }
            if read_failures:
                time.sleep(0.005)
                continue
            progressed = False
            current_net_frames: dict[str, int] = {}
            for name, role in self.roles.items():
                state = states[name]
                if not state.get("alive"):
                    raise HarnessFailure(
                        f"{name} exited during native lifecycle: {state}"
                    )
                current_net_frames[name] = self.runtime_data_u32(
                    role, self.net_frame_rva
                )

                m = metrics[name]
                m["actions_p1"].add(int(state["p1_action"]))
                m["actions_p2"].add(int(state["p2_action"]))
                m["p1_x_min"] = min(
                    int(m["p1_x_min"]), int(state["p1_x"])
                )
                m["p1_x_max"] = max(
                    int(m["p1_x_max"]), int(state["p1_x"])
                )
                m["p2_x_min"] = min(
                    int(m["p2_x_min"]), int(state["p2_x"])
                )
                m["p2_x_max"] = max(
                    int(m["p2_x_max"]), int(state["p2_x"])
                )
                m["health_p1_min"] = min(
                    int(m["health_p1_min"]), int(state["health_p1"])
                )
                m["health_p2_min"] = min(
                    int(m["health_p2_min"]), int(state["health_p2"])
                )
                m["round_wins_p1_max"] = max(
                    int(m["round_wins_p1_max"]),
                    int(state["round_wins_p1"]),
                )
                m["round_wins_p2_max"] = max(
                    int(m["round_wins_p2_max"]),
                    int(state["round_wins_p2"]),
                )
                m["ko_sequencer_max"] = max(
                    int(m["ko_sequencer_max"]),
                    int(state["ko_sequencer"]),
                )
                m["round_counter_values"].add(
                    int(state["round_counter"])
                )
                m["winner_values"].add(int(state["winner"]))
                m["match_substates"].add(int(state["match_sub"]))

                old = previous[name]
                old_wins = (
                    int(old["round_wins_p1"])
                    + int(old["round_wins_p2"])
                )
                current_wins = (
                    int(state["round_wins_p1"])
                    + int(state["round_wins_p2"])
                )
                if current_wins > old_wins:
                    completed = current_wins - old_wins
                    m["round_completions"] = (
                        int(m["round_completions"]) + completed
                    )
                    progressed = True
                    self.event(
                        "native-round-complete",
                        role=name,
                        completed=m["round_completions"],
                        wins=[
                            int(state["round_wins_p1"]),
                            int(state["round_wins_p2"]),
                        ],
                        state=state,
                    )
                if (
                    int(old["ko_sequencer"]) == 0
                    and int(state["ko_sequencer"]) != 0
                ):
                    m["round_finish_entries"] = (
                        int(m["round_finish_entries"]) + 1
                    )
                    progressed = True
                    self.event(
                        "native-round-finish-entered",
                        role=name,
                        finishes=m["round_finish_entries"],
                        state=state,
                    )
                old_gameplay = (
                    old.get("scene") == 2 and old.get("match_sub") == 0
                )
                gameplay = (
                    state.get("scene") == 2
                    and state.get("match_sub") == 0
                )
                if old_gameplay and not gameplay:
                    m["gameplay_exits"] = int(m["gameplay_exits"]) + 1
                    if (
                        "initial_gameplay_exit_frame"
                        not in pacing[name]
                    ):
                        pacing_now = time.monotonic()
                        start_frame = int(
                            pacing[name][
                                "initial_gameplay_start_frame"
                            ]
                        )
                        elapsed = pacing_now - pacing_started
                        advanced = (
                            current_net_frames[name] - start_frame
                        )
                        pacing[name].update(
                            {
                                "initial_gameplay_exit_frame":
                                    current_net_frames[name],
                                "initial_gameplay_frames": advanced,
                                "initial_gameplay_seconds":
                                    round(elapsed, 3),
                                "initial_gameplay_fps": round(
                                    advanced / elapsed, 3
                                ),
                                "frontend_transition_start_frame":
                                    current_net_frames[name],
                                "_frontend_transition_started":
                                    pacing_now,
                            }
                        )
                    progressed = True
                    self.event(
                        "native-round-gameplay-exit",
                        role=name,
                        exits=m["gameplay_exits"],
                        state=state,
                    )
                if not old_gameplay and gameplay:
                    if (
                        not returned_dwell_complete
                        and int(m["round_completions"]) >= 3
                    ):
                        raise HarnessFailure(
                            f"{name} reached rematch gameplay before the "
                            "required returned-character-select idle dwell"
                        )
                    if int(state["control_mask"]) != 0:
                        raise HarnessFailure(
                            f"{name} gameplay re-entry retained a non-human "
                            f"control mask: 0x{int(state['control_mask']):08X}"
                        )
                    m["gameplay_entries"] = (
                        int(m["gameplay_entries"]) + 1
                    )
                    progressed = True
                    if bool(m["saw_charselect_return"]):
                        m["rematch_start_frame"] = current_net_frames[name]
                        rematch_hash_baselines[name] = (
                            self.hash_counts_by_role()[name]
                        )
                        transition_started = pacing[name].get(
                            "_frontend_transition_started"
                        )
                        transition_start_frame = pacing[name].get(
                            "frontend_transition_start_frame"
                        )
                        if (
                            transition_started is not None
                            and transition_start_frame is not None
                            and "frontend_transition_fps"
                                not in pacing[name]
                        ):
                            elapsed = (
                                time.monotonic()
                                - float(transition_started)
                            )
                            advanced = (
                                current_net_frames[name]
                                - int(transition_start_frame)
                            )
                            pacing[name].update(
                                {
                                    "rematch_entry_frame":
                                        current_net_frames[name],
                                    "frontend_transition_frames":
                                        advanced,
                                    "frontend_transition_seconds":
                                        round(elapsed, 3),
                                    "frontend_transition_fps": round(
                                        advanced / elapsed, 3
                                    ),
                                }
                            )
                    self.event(
                        "native-round-gameplay-entry",
                        role=name,
                        entries=m["gameplay_entries"],
                        after_charselect=bool(
                            m["saw_charselect_return"]
                        ),
                        state=state,
                    )
                in_charselect = (
                    state.get("scene") == 3
                    and state.get("scene3_sub") == 2
                )
                old_in_charselect = (
                    old.get("scene") == 3
                    and old.get("scene3_sub") == 2
                )
                if (
                    in_charselect
                    and not old_in_charselect
                    and int(m["round_completions"]) >= 3
                ):
                    m["charselect_returns"] = (
                        int(m["charselect_returns"]) + 1
                    )
                    m["saw_charselect_return"] = True
                    progressed = True
                    self.event(
                        "native-match-returned-to-charselect",
                        role=name,
                        exits=m["gameplay_exits"],
                        state=state,
                    )
                previous[name] = state

            if (
                not returned_dwell_complete
                and any(
                    bool(metric["saw_charselect_return"])
                    for metric in metrics.values()
                )
            ):
                self.wait_for_returned_charselect_idle_dwell()
                returned_dwell_complete = True
                states = self.current_states()
                current_net_frames = {
                    name: self.runtime_data_u32(
                        role, self.net_frame_rva
                    )
                    for name, role in self.roles.items()
                }
                last_lifecycle_progress = time.monotonic()

            spectator_frame = 0
            if (
                self.spectator is not None
                and self.spectator.game_memory is not None
                and spectator_metrics is not None
                and spectator_previous is not None
            ):
                spectator_state = game_state(
                    self.spectator.game_memory
                )
                if "read_error" in spectator_state:
                    time.sleep(0.005)
                    continue
                if not spectator_state.get("alive"):
                    raise HarnessFailure(
                        "spectator exited during native lifecycle: "
                        f"{spectator_state}"
                    )
                spectator_frame = self.runtime_data_u32(
                    self.spectator, self.net_frame_rva
                )
                spectator_metrics["scenes"].add(
                    int(spectator_state["scene"])
                )
                spectator_metrics["match_substates"].add(
                    int(spectator_state["match_sub"])
                )
                spectator_metrics["round_wins_p1_max"] = max(
                    int(spectator_metrics["round_wins_p1_max"]),
                    int(spectator_state["round_wins_p1"]),
                )
                spectator_metrics["round_wins_p2_max"] = max(
                    int(spectator_metrics["round_wins_p2_max"]),
                    int(spectator_state["round_wins_p2"]),
                )
                spectator_metrics["ko_sequencer_max"] = max(
                    int(spectator_metrics["ko_sequencer_max"]),
                    int(spectator_state["ko_sequencer"]),
                )
                spectator_metrics["round_counter_values"].add(
                    int(spectator_state["round_counter"])
                )
                old_wins = (
                    int(spectator_previous["round_wins_p1"])
                    + int(spectator_previous["round_wins_p2"])
                )
                current_wins = (
                    int(spectator_state["round_wins_p1"])
                    + int(spectator_state["round_wins_p2"])
                )
                if current_wins > old_wins:
                    spectator_metrics["round_completions"] = (
                        int(spectator_metrics["round_completions"])
                        + current_wins
                        - old_wins
                    )
                    progressed = True
                if (
                    int(spectator_previous["ko_sequencer"]) == 0
                    and int(spectator_state["ko_sequencer"]) != 0
                ):
                    spectator_metrics["round_finish_entries"] = (
                        int(spectator_metrics["round_finish_entries"]) + 1
                    )
                    progressed = True
                old_gameplay = (
                    spectator_previous.get("scene") == 2
                    and spectator_previous.get("match_sub") == 0
                )
                gameplay = (
                    spectator_state.get("scene") == 2
                    and spectator_state.get("match_sub") == 0
                )
                if old_gameplay and not gameplay:
                    spectator_metrics["gameplay_exits"] = (
                        int(spectator_metrics["gameplay_exits"]) + 1
                    )
                    progressed = True
                if not old_gameplay and gameplay:
                    spectator_metrics["gameplay_entries"] = (
                        int(spectator_metrics["gameplay_entries"]) + 1
                    )
                    if bool(
                        spectator_metrics["saw_charselect_return"]
                    ):
                        spectator_metrics["rematch_start_frame"] = (
                            spectator_frame
                        )
                    progressed = True
                in_charselect = (
                    spectator_state.get("scene") == 3
                    and spectator_state.get("scene3_sub") == 2
                )
                old_in_charselect = (
                    spectator_previous.get("scene") == 3
                    and spectator_previous.get("scene3_sub") == 2
                )
                if (
                    in_charselect
                    and not old_in_charselect
                    and int(spectator_metrics["round_completions"]) >= 3
                ):
                    spectator_metrics["charselect_returns"] = (
                        int(spectator_metrics["charselect_returns"]) + 1
                    )
                    spectator_metrics["saw_charselect_return"] = True
                    progressed = True
                spectator_previous = spectator_state

            current_lifecycle_signature = lifecycle_signature(states)
            if (
                progressed
                or current_lifecycle_signature != last_lifecycle_signature
            ):
                last_lifecycle_signature = current_lifecycle_signature
                last_lifecycle_progress = time.monotonic()
            if time.monotonic() - last_lifecycle_progress > 30.0:
                raise HarnessFailure(
                    "native match/rematch lifecycle stopped progressing: "
                    f"metrics={self._serializable_lifecycle_metrics(metrics)} "
                    f"states={states}"
                )

            rematch_ready = True
            rematch_deltas: dict[str, int] = {}
            for name, m in metrics.items():
                start = m["rematch_start_frame"]
                if start is None:
                    rematch_ready = False
                    rematch_deltas[name] = 0
                    continue
                delta = current_net_frames[name] - int(start)
                rematch_deltas[name] = delta
                if delta < self.args.rematch_gameplay_ticks:
                    rematch_ready = False

            hash_counts = self.hash_counts_by_role()
            rematch_hashes_observed = all(
                rematch_hash_baselines[name] is not None
                and hash_counts[name]
                > int(rematch_hash_baselines[name])
                for name in self.roles
            )
            spectator_rematch_ready = True
            spectator_rematch_delta = 0
            if spectator_metrics is not None:
                spectator_start = spectator_metrics[
                    "rematch_start_frame"
                ]
                if spectator_start is None:
                    spectator_rematch_ready = False
                else:
                    spectator_rematch_delta = (
                        spectator_frame - int(spectator_start)
                    )
                    spectator_rematch_ready = (
                        spectator_rematch_delta
                        >= self.args.rematch_gameplay_ticks
                    )
            games_required = max(2, int(self.args.game_count))
            lifecycle_counts_ready = all(
                int(m["gameplay_entries"]) >= games_required
                and int(m["charselect_returns"]) >= games_required - 1
                for m in metrics.values()
            )
            if (
                returned_dwell_complete
                and rematch_ready
                and rematch_hashes_observed
                and spectator_rematch_ready
                and lifecycle_counts_ready
            ):
                serializable = self._serializable_lifecycle_metrics(metrics)
                for name, m in serializable.items():
                    if (
                        int(m["round_completions"]) < 2 * games_required - 1
                        or int(m["round_finish_entries"])
                            < 2 * games_required - 1
                        or int(m["gameplay_exits"]) < games_required - 1
                        or int(m["gameplay_entries"]) < games_required
                        or int(m["charselect_returns"]) < games_required - 1
                    ):
                        raise HarnessFailure(
                            f"{name} incomplete native lifecycle: {m}"
                        )
                    movement = (
                        int(m["p1_x_max"]) != int(m["p1_x_min"])
                        or int(m["p2_x_max"]) != int(m["p2_x_min"])
                    )
                    action_variety = (
                        len(m["actions_p1"]) > 1
                        or len(m["actions_p2"]) > 1
                    )
                    if not movement or not action_variety:
                        raise HarnessFailure(
                            f"{name} bots did not exhibit real combat "
                            f"movement/action changes: {m}"
                        )
                    if (
                        int(m["health_p1_min"]) > 300
                        or int(m["health_p2_min"]) > 300
                        or int(m["round_wins_p1_max"]) < 2
                        or int(m["round_wins_p2_max"]) < 1
                        or int(m["ko_sequencer_max"]) < 1
                        or len(m["match_substates"]) < 2
                    ):
                        raise HarnessFailure(
                            f"{name} did not expose real damage, round wins, "
                            f"and native transition substates: {m}"
                        )
                if self.args.require_character_switch:
                    self._require_fresh_characters_per_game(games_required)
                serializable_spectator = None
                if spectator_metrics is not None:
                    serializable_spectator = (
                        self._serializable_lifecycle_metrics(
                            {"spectator": spectator_metrics}
                        )["spectator"]
                    )
                    if (
                        int(
                            serializable_spectator["round_completions"]
                        ) < 3
                        or int(
                            serializable_spectator["gameplay_exits"]
                        ) < 1
                        or int(
                            serializable_spectator["charselect_returns"]
                        ) < 1
                        or int(
                            serializable_spectator["gameplay_entries"]
                        ) < 2
                        or int(
                            serializable_spectator["round_wins_p1_max"]
                        ) < 2
                        or int(
                            serializable_spectator["round_wins_p2_max"]
                        ) < 1
                    ):
                        raise HarnessFailure(
                            "spectator did not replay the complete native "
                            f"lifecycle: {serializable_spectator}"
                        )
                self.report["native_match_rematch"] = {
                    "roles": serializable,
                    "rematch_gameplay_deltas": rematch_deltas,
                    "confirmed_hash_receipts": hash_counts,
                    "initial_confirmed_hash_receipts": initial_hash_counts,
                    "rematch_hash_baselines": rematch_hash_baselines,
                    "post_rematch_hash_observed": (
                        rematch_hashes_observed
                    ),
                    "spectator": serializable_spectator,
                    "spectator_rematch_gameplay_delta": (
                        spectator_rematch_delta
                    ),
                }
                serializable_pacing: dict[
                    str, dict[str, object]
                ] = {}
                for name, sample in pacing.items():
                    public_sample = {
                        key: value
                        for key, value in sample.items()
                        if not key.startswith("_")
                    }
                    if (
                        "initial_gameplay_fps" not in public_sample
                        or "frontend_transition_fps"
                            not in public_sample
                    ):
                        raise HarnessFailure(
                            f"{name} pacing sample is incomplete: "
                            f"{public_sample}"
                        )
                    serializable_pacing[name] = public_sample
                self.report["canonical_pacing"] = {
                    "target_fps": CANONICAL_TARGET_FPS,
                    "minimum_required_fps":
                        self.args.minimum_canonical_fps,
                    "roles": serializable_pacing,
                }
                if self.args.minimum_canonical_fps > 0:
                    phase_floors = {
                        "initial_gameplay_fps":
                            self.args.minimum_canonical_fps,
                        "frontend_transition_fps":
                            self.args.minimum_canonical_fps - 1.0,
                    }
                    wall_clock_pacing = {
                        name: sample
                        for name, sample in serializable_pacing.items()
                        if not self.is_remote_role(name)
                    }
                    slow_samples = {
                        name: {
                            phase: sample[phase]
                            for phase, floor in phase_floors.items()
                            if float(sample[phase]) < floor
                        }
                        for name, sample in wall_clock_pacing.items()
                    }
                    slow_samples = {
                        name: sample
                        for name, sample in slow_samples.items()
                        if sample
                    }
                    self.validate_phase_fps_log_derived(serializable_pacing)
                    for name in list(slow_samples.keys()):
                        entry = slow_samples[name]
                        if "frontend_transition_fps" not in entry:
                            continue
                        sample = serializable_pacing[name]
                        loader_s, loader_spans = (
                            self.native_loader_dispatch_seconds(
                                self.roles[name],
                                int(sample.get(
                                    "frontend_transition_start_frame", 0
                                )),
                                int(sample.get("rematch_entry_frame", 0)),
                            )
                        )
                        loader_s = max(
                            loader_s,
                            self.rematch_layout_barrier_seconds(
                                self.roles[name]
                            ),
                        )
                        barrier_s = loader_s + getattr(
                            self, "_harness_freeze_seconds", 0.0
                        )
                        elapsed = float(
                            sample.get("frontend_transition_seconds", 0.0)
                        )
                        advanced = int(
                            sample.get("frontend_transition_frames", 0)
                        )
                        if (
                            barrier_s > 0.0
                            and elapsed - barrier_s >= 1.0
                        ):
                            adjusted = advanced / (elapsed - barrier_s)
                            sample["frontend_transition_fps_excl_barrier"] = (
                                round(adjusted, 3)
                            )
                            sample["rematch_layout_barrier_seconds"] = (
                                round(barrier_s, 3)
                            )
                            sample["native_loader_dispatches"] = loader_spans
                            self.event(
                                "frontend-transition-barrier-excluded",
                                role=name,
                                barrier_seconds=round(barrier_s, 3),
                                native_loader_dispatches=loader_spans,
                                raw_fps=entry["frontend_transition_fps"],
                                adjusted_fps=round(adjusted, 3),
                            )
                            if adjusted >= phase_floors[
                                "frontend_transition_fps"
                            ]:
                                del entry["frontend_transition_fps"]
                        if not entry:
                            del slow_samples[name]
                    if slow_samples:
                        raise HarnessFailure(
                            "canonical pacing fell below "
                            f"{self.args.minimum_canonical_fps:.1f} fps: "
                            f"{slow_samples}"
                        )
                self.report["gameplay_tick_deltas"] = rematch_deltas
                self.report["confirmed_hash_receipts"] = hash_counts
                self.event(
                    "native-match-rematch-complete",
                    metrics=serializable,
                    rematch_gameplay_deltas=rematch_deltas,
                    confirmed_hash_receipts=hash_counts,
                    rematch_hash_baselines=rematch_hash_baselines,
                    spectator=serializable_spectator,
                    states=states,
                )
                return
            time.sleep(0.005)

        raise HarnessFailure(
            "timed out waiting for native match completion and rematch: "
            f"metrics={self._serializable_lifecycle_metrics(metrics)} "
            f"hashes={self.hash_counts_by_role()} "
            f"states={self.current_states()}"
        )

    @staticmethod
    def _serializable_lifecycle_metrics(
        metrics: dict[str, dict[str, object]]
    ) -> dict[str, dict[str, object]]:
        result: dict[str, dict[str, object]] = {}
        for name, metric in metrics.items():
            converted = {
                key: sorted(value) if isinstance(value, set) else value
                for key, value in metric.items()
            }
            result[name] = converted
        return result

    def wait_for_spectator_attachment(
        self, role: Role, timeout: float = 35.0
    ) -> dict[str, object]:
        if role.game_memory is None:
            raise HarnessFailure(f"{role.name} game was not discovered")
        deadline = time.monotonic() + timeout
        last_state: dict[str, object] = {}
        last_missing = list(SPECTATOR_REQUIRED_DLL_MARKERS)
        last_handoffs = 0
        last_source_count = 0
        while time.monotonic() < deadline:
            self._pump_observer()
            self._scan_failures()
            state = game_state(role.game_memory)
            if "read_error" in state:
                last_state = state
                time.sleep(0.01)
                continue
            if not state.get("alive"):
                raise HarnessFailure(
                    f"{role.name} exited before fallback attachment: {state}"
                )
            viewer_text = role.dll_text()
            missing = [
                marker
                for marker in SPECTATOR_REQUIRED_DLL_MARKERS
                if marker not in viewer_text
            ]
            handoffs = list(
                SPECTATOR_RUNTIME_HANDOFF_RE.finditer(viewer_text)
            )
            source_text = (
                self.roles["host"].stdout_text()
                + "\n"
                + self.roles["host"].client_text()
            )
            source_counts = list(
                SPECTATOR_VIEWER_COUNT_RE.finditer(source_text)
            )
            source_count = max(
                (int(match.group("count")) for match in source_counts),
                default=0,
            )
            if (
                not missing
                and handoffs
                and state.get("scene") == 2
                and state.get("match_sub") == 0
                and source_count >= 1
            ):
                handoff = handoffs[-1]
                attachment = {
                    "handoff_viewer_id": handoff.group("viewer").upper(),
                    "handoff_generation": int(
                        handoff.group("generation")
                    ),
                    "handoff_resume_cursor": int(handoff.group("resume")),
                    "source_viewer_count": source_count,
                    "state": state,
                }
                self.event(
                    "spectator-fallback-primary-attached",
                    role=role.name,
                    **attachment,
                )
                return attachment
            last_state = state
            last_missing = missing
            last_handoffs = len(handoffs)
            last_source_count = source_count
            time.sleep(0.01)
        raise HarnessFailure(
            f"{role.name} did not attach before punch fallback; "
            f"missing={last_missing} handoffs={last_handoffs} "
            f"sourceViewerCount={last_source_count} state={last_state}"
        )

    def wait_for_spectator_playback(
        self, role: Role | None = None, timeout: float = 70.0
    ) -> None:
        role = self.spectator if role is None else role
        if role is None:
            return
        if not role.game_memory:
            raise HarnessFailure(f"{role.name} game was not discovered")

        player_start: dict[str, int] = {}
        for name, player in self.roles.items():
            assert player.game_memory
            player_start[name] = self.runtime_data_u32(
                player, self.net_frame_rva
            )
        deadline = time.monotonic() + timeout
        last_player_progress = {name: 0 for name in self.roles}
        last_spectator_delta = 0
        last_missing = list(SPECTATOR_REQUIRED_DLL_MARKERS)
        last_spectator_state: dict[str, object] = {}
        spectator_timeline_baseline: int | None = None
        spectator_counter_at_baseline: int | None = None
        while time.monotonic() < deadline:
            self._pump_observer()
            self._scan_failures()
            player_states = self.current_states()
            read_failures = {
                name: state["read_error"]
                for name, state in player_states.items()
                if "read_error" in state
            }
            if read_failures:
                time.sleep(0.005)
                continue
            player_progress: dict[str, int] = {}
            for name in self.roles:
                state = player_states[name]
                if state.get("scene") != 2 or state.get("match_sub") != 0:
                    raise HarnessFailure(
                        f"{name} left gameplay during spectator catch-up: "
                        f"{state}"
                    )
                count = self.runtime_data_u32(
                    self.roles[name], self.net_frame_rva
                )
                if count < player_start[name]:
                    raise HarnessFailure(
                        f"{name} canonical net timeline moved backwards "
                        "during spectator catch-up: "
                        f"start={player_start[name]} current={count}"
                    )
                player_progress[name] = count - player_start[name]

            spectator_state = game_state(role.game_memory)
            if "read_error" in spectator_state:
                last_spectator_state = spectator_state
                time.sleep(0.005)
                continue
            if not spectator_state.get("alive"):
                raise HarnessFailure(
                    "spectator exited during catch-up: "
                    f"{spectator_state}"
                )
            text = role.dll_text()
            missing = [
                marker
                for marker in SPECTATOR_REQUIRED_DLL_MARKERS
                if marker not in text
            ]
            spectator_counter = int(spectator_state["frame_counter"])
            spectator_timeline = self.runtime_data_u32(
                role, self.net_frame_rva
            )
            spectator_in_gameplay = (
                spectator_state.get("scene") == 2
                and spectator_state.get("match_sub") == 0
                and "[spectator] gameplay-entry rendezvous complete" in text
            )
            if (
                spectator_in_gameplay
                and spectator_timeline_baseline is None
            ):
                spectator_timeline_baseline = spectator_timeline
                spectator_counter_at_baseline = spectator_counter
                self.event(
                    "spectator-gameplay-timeline-baseline",
                    timeline_frame=spectator_timeline_baseline,
                    cosmetic_counter=spectator_counter_at_baseline,
                    state=spectator_state,
                )
            if spectator_timeline_baseline is None:
                spectator_delta = 0
            else:
                if spectator_timeline < spectator_timeline_baseline:
                    raise HarnessFailure(
                        "spectator canonical timeline moved backwards after "
                        "gameplay entry: "
                        f"baseline={spectator_timeline_baseline} "
                        f"current={spectator_timeline}"
                    )
                spectator_delta = (
                    spectator_timeline - spectator_timeline_baseline
                )
            player_hashes = self.hash_counts_by_role()
            if (
                not missing
                and spectator_state.get("scene") == 2
                and spectator_state.get("match_sub") == 0
                and spectator_delta >= 30
                and all(
                    progress >= 60
                    for progress in player_progress.values()
                )
                and all(count >= 1 for count in player_hashes.values())
            ):
                spectator_report = {
                    "required_markers": list(
                        SPECTATOR_REQUIRED_DLL_MARKERS
                    ),
                    "marker_presence": {
                        marker: marker in text
                        for marker in SPECTATOR_REQUIRED_DLL_MARKERS
                    },
                    "live_state": spectator_state,
                    "viewer_gameplay_timeline_baseline": (
                        spectator_timeline_baseline
                    ),
                    "viewer_timeline_delta": spectator_delta,
                    "viewer_cosmetic_counter_at_baseline": (
                        spectator_counter_at_baseline
                    ),
                    "viewer_cosmetic_counter_current": spectator_counter,
                    "player_overlap_submitted_frames": player_progress,
                    "player_confirmed_hash_receipts": player_hashes,
                    "input_isolation": {
                        "live_evidence": (
                            "viewer authenticated on the host's dedicated "
                            "spectator UDP capability while both authoritative "
                            "players continued submitting indexed inputs and "
                            "receiving confirmed gameplay hashes"
                        ),
                        "runtime_contract": (
                            "spectate launches MODE_SPECTATOR with "
                            "localPlayer=0; the viewer constructs only "
                            "SpectatorViewer and never a player NetplayCore"
                        ),
                        "covered_elsewhere": (
                            "spectator source/viewer unit and paired soak "
                            "tests enforce player-domain isolation and "
                            "spectator-count invariance"
                        ),
                    },
                }
                self.report.setdefault("spectators", {})[role.name] = (
                    spectator_report
                )
                if role is self.spectator:
                    self.report["spectator"] = spectator_report
                self.event(
                    "spectator-live-playback-complete",
                    role=role.name,
                    state=spectator_state,
                    viewer_timeline_delta=spectator_delta,
                    player_overlap_submitted_frames=player_progress,
                    player_confirmed_hash_receipts=player_hashes,
                )
                return
            last_player_progress = player_progress
            last_spectator_delta = spectator_delta
            last_missing = missing
            last_spectator_state = spectator_state
            time.sleep(0.01)
        raise HarnessFailure(
            f"{role.name} did not catch up into live gameplay while players "
            f"continued; missing={last_missing} "
            f"viewerDelta={last_spectator_delta} "
            f"playerProgress={last_player_progress} "
            f"state={last_spectator_state}"
        )

    def wait_for_all_spectator_playback(self) -> None:
        for role in self.spectators:
            self.wait_for_spectator_playback(role)

    def wait_for_spectator_f1_survival(self) -> None:
        if not self.args.f1_return_at_frame or not self.spectators:
            return
        deadline = time.monotonic() + 120.0
        needed_rendezvous = 2
        while True:
            missing: list[str] = []
            host_text = self.roles["host"].dll_text()
            if "[test-automatch] F1 return firing" not in host_text:
                missing.append("host F1 automation firing")
            if (
                "[spectator-source] F1 return published as a chained event"
                not in host_text
            ):
                missing.append("host chained-event publication")
            for role in self.spectators:
                text = role.dll_text()
                if "[spectator] F1 return executed at record" not in text:
                    missing.append(f"{role.name} F1 return execution")
                if (
                    text.count(
                        "[spectator] gameplay-entry rendezvous complete"
                    )
                    < needed_rendezvous
                ):
                    missing.append(
                        f"{role.name} second gameplay entry"
                    )
            if not missing:
                break
            if time.monotonic() > deadline:
                sealed = [
                    role.name
                    for role in self.spectators
                    if "SEALED" in role.dll_text()
                ]
                raise HarnessFailure(
                    "spectator did not survive the F1 return: "
                    f"missing={missing} sealedViewers={sealed}"
                )
            time.sleep(0.5)
        for role in self.spectators:
            self.event(
                "spectator-f1-survival",
                role=role.name,
                detail="viewer followed the F1 return into the next game",
            )

    def exercise_spectator_active_catchup(self) -> None:
        pause_seconds = self.args.spectator_active_catchup_pause_seconds
        if pause_seconds <= 0.0:
            return
        if len(self.spectators) != 2:
            raise HarnessFailure(
                "active spectator catch-up requires two live viewers"
            )
        delayed, smooth = self.spectators
        if delayed.game_memory is None or smooth.game_memory is None:
            raise HarnessFailure(
                "active spectator catch-up viewers have no game process"
            )
        self._require_live_gameplay_players("before viewer catch-up pause")
        delayed_state = game_state(delayed.game_memory)
        smooth_state = game_state(smooth.game_memory)
        for role, state in ((delayed, delayed_state), (smooth, smooth_state)):
            if (
                not state.get("alive")
                or state.get("scene") != 2
                or state.get("match_sub") != 0
            ):
                raise HarnessFailure(
                    f"{role.name} was not live before active catch-up pause: "
                    f"{state}"
                )

        player_before = {
            name: self.runtime_data_u32(role, self.net_frame_rva)
            for name, role in self.roles.items()
        }
        delayed_before = self.runtime_data_u32(delayed, self.net_frame_rva)
        smooth_before = self.runtime_data_u32(smooth, self.net_frame_rva)
        common_before = self._common_player_hashes()
        common_before_max = max(
            (item[2] for item in common_before), default=-1
        )
        delayed_log_offset = len(delayed.dll_text())
        smooth_log_offset = len(smooth.dll_text())
        paused_samples: list[dict[str, object]] = []
        player_max = dict(player_before)
        smooth_max = smooth_before
        pause_started = time.monotonic()
        delayed.game_memory.suspend()
        try:
            pause_deadline = pause_started + pause_seconds
            while time.monotonic() < pause_deadline:
                self._pump_observer()
                self._scan_failures()
                states = self._require_live_gameplay_players(
                    "during viewer catch-up pause"
                )
                current_players = {
                    name: self.runtime_data_u32(role, self.net_frame_rva)
                    for name, role in self.roles.items()
                }
                smooth_current = self.runtime_data_u32(
                    smooth, self.net_frame_rva
                )
                smooth_current_state = game_state(smooth.game_memory)
                if (
                    not smooth_current_state.get("alive")
                    or smooth_current_state.get("scene") != 2
                    or smooth_current_state.get("match_sub") != 0
                ):
                    raise HarnessFailure(
                        "unpaused spectator did not remain live while its "
                        f"peer was paused: {smooth_current_state}"
                    )
                for name, value in current_players.items():
                    player_max[name] = max(player_max[name], value)
                smooth_max = max(smooth_max, smooth_current)
                if len(paused_samples) < 32:
                    paused_samples.append(
                        {
                            "elapsed_seconds": round(
                                time.monotonic() - pause_started, 3
                            ),
                            "players": current_players,
                            "smooth_viewer_timeline": smooth_current,
                            "player_states": states,
                        }
                    )
                time.sleep(0.1)
        finally:
            delayed.game_memory.resume()
        paused_seconds = time.monotonic() - pause_started

        stalled_players = {
            name: {"before": player_before[name], "max": player_max[name]}
            for name in player_before
            if player_max[name] <= player_before[name]
        }
        if stalled_players or smooth_max <= smooth_before:
            raise HarnessFailure(
                "pausing one spectator blocked player or independent viewer "
                "progress: "
                f"players={stalled_players} smooth={smooth_before}/{smooth_max}"
            )

        deadline = time.monotonic() + 30.0
        latest_delayed_state: dict[str, object] = {}
        delayed_after = delayed_before
        common_after: set[tuple[int, int, int, str]] = set()
        catchups: list[re.Match[str]] = []
        converged: list[re.Match[str]] = []
        while time.monotonic() < deadline:
            self._pump_observer()
            self._scan_failures()
            self._require_live_gameplay_players(
                "after viewer catch-up resume"
            )
            latest_delayed_state = game_state(delayed.game_memory)
            if "read_error" in latest_delayed_state:
                time.sleep(0.02)
                continue
            delayed_after = self.runtime_data_u32(delayed, self.net_frame_rva)
            delayed_delta_log = delayed.dll_text()[delayed_log_offset:]
            smooth_delta_log = smooth.dll_text()[smooth_log_offset:]
            catchups = list(SPECTATOR_CATCHUP_RE.finditer(delayed_delta_log))
            converged = list(
                SPECTATOR_CATCHUP_CONVERGED_RE.finditer(delayed_delta_log)
            )
            common_after = self._common_player_hashes()
            max_backlog = max(
                (
                    int(match.group("receive_backlog"))
                    for match in catchups
                ),
                default=0,
            )
            matching_players_advanced = any(
                item[2] > common_before_max for item in common_after
            )
            smooth_new_catchups = list(
                SPECTATOR_CATCHUP_RE.finditer(smooth_delta_log)
            )
            if (
                delayed_after > delayed_before
                and latest_delayed_state.get("alive")
                and latest_delayed_state.get("scene") == 2
                and latest_delayed_state.get("match_sub") == 0
                and max_backlog >= 16
                and max_backlog < 8192
                and converged
                and matching_players_advanced
                and not smooth_new_catchups
            ):
                evidence = {
                    "paused_role": delayed.name,
                    "smooth_role": smooth.name,
                    "requested_pause_seconds": pause_seconds,
                    "actual_pause_seconds": round(paused_seconds, 3),
                    "player_timelines": {
                        "before": player_before,
                        "max_during_pause": player_max,
                    },
                    "delayed_viewer_timeline": {
                        "before": delayed_before,
                        "after": delayed_after,
                    },
                    "smooth_viewer_timeline": {
                        "before": smooth_before,
                        "max_during_pause": smooth_max,
                    },
                    "matching_player_hash_frame_before": common_before_max,
                    "matching_player_hash_frame_after": max(
                        (item[2] for item in common_after), default=-1
                    ),
                    "catchup_receive_backlog_max": max_backlog,
                    "catchup_samples": [
                        match.group(0) for match in catchups[-4:]
                    ],
                    "converged_samples": [
                        match.group(0) for match in converged[-2:]
                    ],
                    "smooth_viewer_new_catchup_lines": 0,
                    "pause_samples": paused_samples,
                }
                self.report["spectator_active_catchup"] = evidence
                self.event("spectator-active-catchup-complete", **evidence)
                return
            time.sleep(0.05)

        raise HarnessFailure(
            "paused spectator did not complete a bounded active catch-up: "
            f"timeline={delayed_before}/{delayed_after} "
            f"state={latest_delayed_state} catchups="
            f"{[match.group(0) for match in catchups[-3:]]} "
            f"converged={len(converged)} commonBefore={common_before_max} "
            f"commonAfter={max((item[2] for item in common_after), default=-1)} "
            f"smoothNewCatchups="
            f"{len(list(SPECTATOR_CATCHUP_RE.finditer(smooth.dll_text()[smooth_log_offset:])))}"
        )

    def exercise_spectator_sibling_disconnect(self) -> None:
        if not self.args.spectator_sibling_disconnect:
            return
        if len(self.spectators) != 2:
            raise HarnessFailure(
                "sibling-disconnect fixture requires two live viewers"
            )
        survivor, closing = self.spectators
        if (
            self.spectator_route_for_role(survivor) != "direct"
            or self.spectator_route_for_role(closing) != "punch"
        ):
            raise HarnessFailure(
                "sibling-disconnect fixture lost its direct/punch ordering"
            )

        for role in (survivor, closing):
            self.validate_spectator_logs(role)
        qsp_by_role = self.report.get("spectator_e0_source_qsp_evidence", {})
        if not isinstance(qsp_by_role, dict):
            raise HarnessFailure(
                "sibling-disconnect fixture has no source-side QSP evidence"
            )
        no_starvation: dict[str, dict[str, int | str]] = {}
        viewer_ids: set[str] = set()
        for role in (survivor, closing):
            evidence = qsp_by_role.get(role.name)
            if not isinstance(evidence, dict):
                raise HarnessFailure(
                    f"{role.name} has no source-side QSP status evidence"
                )
            viewer_id = str(evidence.get("viewer_id", ""))
            generation = int(evidence.get("handoff_generation", 0))
            bootstrap = int(evidence.get("handoff_resume_cursor", 0))
            confirmed = int(evidence.get("source_confirmed_cursor_max", 0))
            acknowledged = int(evidence.get("ack_cursor_max", 0))
            bytes_sent = int(evidence.get("bytes_sent_max", 0))
            if (
                not viewer_id
                or generation <= 0
                or confirmed <= bootstrap
                or acknowledged <= bootstrap
                or bytes_sent <= 0
            ):
                raise HarnessFailure(
                    "two-viewer source scheduling did not prove independent "
                    f"no-starvation progress for {role.name}: {evidence}"
                )
            viewer_ids.add(viewer_id)
            no_starvation[role.name] = {
                "viewer_id": viewer_id,
                "generation": generation,
                "bootstrap_cursor": bootstrap,
                "confirmed_cursor": confirmed,
                "ack_cursor": acknowledged,
                "bytes_sent": bytes_sent,
            }
        if len(viewer_ids) != 2:
            raise HarnessFailure(
                "mixed viewers did not retain two independent viewer IDs: "
                f"{no_starvation}"
            )

        def redacted_viewer_tag(viewer_id: str) -> str:
            value = int(viewer_id, 16) ^ 0xA5A5A5A5DEADBEEF
            value = (value + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
            value = (
                (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9
            ) & 0xFFFFFFFFFFFFFFFF
            value = (
                (value ^ (value >> 27)) * 0x94D049BB133111EB
            ) & 0xFFFFFFFFFFFFFFFF
            value ^= value >> 31
            return f"#{value & 0xFFFF:04x}"

        closing_viewer_id = str(no_starvation[closing.name]["viewer_id"])
        closing_generation = int(no_starvation[closing.name]["generation"])
        closing_release_tag = redacted_viewer_tag(closing_viewer_id)
        survivor_viewer_id = str(no_starvation[survivor.name]["viewer_id"])
        survivor_ack_before = int(no_starvation[survivor.name]["ack_cursor"])
        survivor_bytes_before = int(
            no_starvation[survivor.name]["bytes_sent"]
        )

        autopunch = self.local_autopunch
        if autopunch is None:
            raise HarnessFailure(
                "mixed direct/AutoPunch fixture lost its local helper"
            )
        autopunch_stats = autopunch.stats()
        if (
            int(autopunch_stats["lookup_packets"]) <= 0
            or int(autopunch_stats["mapping_packets"]) < 2
            or len(autopunch_stats["registrations"]) < 2
        ):
            raise HarnessFailure(
                "mixed viewers did not retain independent AutoPunch proof: "
                f"{autopunch_stats}"
            )

        player_before = {
            name: self.runtime_data_u32(role, self.net_frame_rva)
            for name, role in self.roles.items()
        }
        survivor_before = self.runtime_data_u32(survivor, self.net_frame_rva)
        hashes_before = self._common_player_hashes()
        host = self.roles["host"]
        host_log_offset = len(host.client_text())
        close_request = self._post_role_close(closing)

        game_deadline = time.monotonic() + 20.0
        while (
            time.monotonic() < game_deadline
            and closing.game_memory is not None
            and closing.game_memory.alive()
        ):
            self._pump_observer()
            self._require_live_gameplay_players(
                "closing one of two live viewers"
            )
            time.sleep(0.02)
        if closing.game_memory is None or closing.game_memory.alive():
            raise HarnessFailure(
                "selected spectator ignored its isolated WM_CLOSE"
            )
        closing_game_exit = closing.game_memory.exit_code()
        if closing_game_exit not in (0, 1):
            raise HarnessFailure(
                "selected spectator game exited uncleanly: "
                f"{closing_game_exit}"
            )
        if closing.process is None:
            raise HarnessFailure("selected spectator caster was unavailable")
        try:
            closing_caster_exit = closing.process.wait(timeout=10)
        except subprocess.TimeoutExpired as exc:
            raise HarnessFailure(
                "selected spectator caster did not exit after its game"
            ) from exc
        if closing_caster_exit != 0:
            raise HarnessFailure(
                "selected spectator caster exited uncleanly: "
                f"{closing_caster_exit}"
            )

        release_deadline = time.monotonic() + 12.0
        source_count: re.Match[str] | None = None
        host_release: re.Match[str] | None = None
        caster_release: re.Match[str] | None = None
        closed_terminal: re.Match[str] | None = None
        while time.monotonic() < release_deadline:
            self._pump_observer()
            self._require_live_gameplay_players(
                "waiting for one-viewer sibling release"
            )
            host_delta = host.client_text()[host_log_offset:]
            counts = [
                match
                for match in SPECTATOR_VIEWER_COUNT_RE.finditer(host_delta)
                if int(match.group("count")) == 1
            ]
            releases = [
                match
                for match in SPECTATOR_HOST_RELEASE_RE.finditer(host_delta)
                if match.group("viewer").casefold()
                == closing_release_tag.casefold()
            ]
            caster_releases = [
                match
                for match in SPECTATOR_CASTER_RELEASE_RE.finditer(
                    closing.client_text()
                )
                if match.group("viewer").casefold()
                == closing_release_tag.casefold()
            ]
            terminals = [
                match
                for match in SPECTATOR_E0_VIEWER_NOT_ACTIVE_RE.finditer(
                    host_delta
                )
                if (
                    match.group("viewer") == closing_viewer_id
                    and int(match.group("generation"))
                    == closing_generation
                    and match.group("reason") == "detached"
                )
            ]
            if counts:
                source_count = counts[-1]
            if releases:
                host_release = releases[-1]
            if caster_releases:
                caster_release = caster_releases[-1]
            if terminals:
                closed_terminal = terminals[-1]
            if (
                source_count is not None
                and host_release is not None
                and caster_release is not None
                and caster_release.group("acknowledged") == "1"
                and closed_terminal is not None
            ):
                break
            time.sleep(0.02)
        if (
            source_count is None
            or host_release is None
            or caster_release is None
            or caster_release.group("acknowledged") != "1"
            or closed_terminal is None
        ):
            raise HarnessFailure(
                "closed sibling did not retire exactly one source admission "
                "with an attributable detached terminal outcome: "
                f"count={source_count.group(0) if source_count else None} "
                f"hostRelease={host_release.group(0) if host_release else None} "
                f"casterRelease="
                f"{caster_release.group(0) if caster_release else None} "
                f"terminal={closed_terminal.group(0) if closed_terminal else None}"
            )
        closing_dll = closing.dll_text()
        detach_shutdown_marker = (
            "[config] saved QOHcnf.key successfully with local profile "
            "values preserved result=1"
        )
        detach_failures = [
            marker
            for marker in (
                "[spectator-viewer] shutdown QSP Detach relay framing failed",
                "[spectator-viewer-detach] sendto failed",
            )
            if marker in closing_dll
        ]
        if detach_shutdown_marker not in closing_dll or detach_failures:
            raise HarnessFailure(
                "closed sibling did not prove its normal QSP shutdown edge: "
                f"shutdownEdge={detach_shutdown_marker in closing_dll} "
                f"detachFailures={detach_failures}"
            )
        self.validate_spectator_logs(closing, after_shutdown=True)

        self.retired_spectators.append(closing)
        self.spectators = [survivor]
        self.spectator = survivor

        self.wait_for_spectator_playback(survivor)
        self.validate_spectator_logs(survivor)

        postclose_deadline = time.monotonic() + 8.0
        postclose_qsp: dict[str, int | str] | None = None
        while time.monotonic() < postclose_deadline:
            self._pump_observer()
            self._require_live_gameplay_players(
                "proving surviving viewer QSP progress after sibling release"
            )
            host_delta = host.client_text()[host_log_offset:]
            statuses = [
                match
                for match in SPECTATOR_E0_VIEWER_STATUS_RE.finditer(host_delta)
                if (
                    match.group("viewer") == survivor_viewer_id
                    and match.group("state") == "streaming"
                )
            ]
            if statuses:
                latest = statuses[-1]
                acknowledgement = int(latest.group("ack"))
                bytes_sent = int(latest.group("bytes_sent"))
                if (
                    acknowledgement > survivor_ack_before
                    and bytes_sent > survivor_bytes_before
                ):
                    postclose_qsp = {
                        "viewer_id": survivor_viewer_id,
                        "ack_cursor": acknowledgement,
                        "bytes_sent": bytes_sent,
                        "source_status": latest.group(0),
                    }
                    break
            time.sleep(0.02)
        if postclose_qsp is None:
            raise HarnessFailure(
                "surviving viewer had no fresh source-side QSP ACK/upload "
                "progress after its sibling detached: "
                f"viewer={survivor_viewer_id} "
                f"ackBefore={survivor_ack_before} "
                f"bytesBefore={survivor_bytes_before}"
            )
        player_after = {
            name: self.runtime_data_u32(role, self.net_frame_rva)
            for name, role in self.roles.items()
        }
        survivor_after = self.runtime_data_u32(survivor, self.net_frame_rva)
        hashes_after = self._common_player_hashes()
        player_progress = {
            name: player_after[name] - player_before[name]
            for name in player_before
        }
        if (
            survivor_after <= survivor_before
            or any(progress < 60 for progress in player_progress.values())
            or not (hashes_after - hashes_before)
        ):
            raise HarnessFailure(
                "surviving viewer or authoritative players did not advance "
                "after its sibling detached: "
                f"viewer={survivor_before}/{survivor_after} "
                f"players={player_progress} newHashes="
                f"{len(hashes_after - hashes_before)}"
            )
        evidence = {
            "surviving_role": survivor.name,
            "closed_role": closing.name,
            "preclose_no_starvation": no_starvation,
            "autopunch": {
                "lookup_packets": int(autopunch_stats["lookup_packets"]),
                "mapping_packets": int(autopunch_stats["mapping_packets"]),
                "registrations": len(autopunch_stats["registrations"]),
            },
            "closed_viewer": {
                "qsp_viewer_id": closing_viewer_id,
                "generation": closing_generation,
                "release_tag": closing_release_tag,
                "close_requests": len(close_request),
                "game_exit": closing_game_exit,
                "caster_exit": closing_caster_exit,
                "host_viewer_count_after": int(source_count.group("count")),
                "host_release": host_release.group(0),
                "caster_release": caster_release.group(0),
                "source_terminal": closed_terminal.group(0),
            },
            "survivor_timeline": {
                "before": survivor_before,
                "after": survivor_after,
            },
            "survivor_postclose_qsp": postclose_qsp,
            "player_timeline_progress": player_progress,
            "new_common_confirmed_hashes": len(hashes_after - hashes_before),
        }
        self.report["spectator_sibling_disconnect"] = evidence
        self.event("spectator-sibling-disconnect-complete", **evidence)

    def hash_counts_by_role(self) -> dict[str, int]:
        if not self.observer:
            return {
                name: len(
                    list(
                        CONFIRMED_HASH_MATCH_RE.finditer(
                            role.dll_text()
                        )
                    )
                )
                for name, role in self.roles.items()
            }
        return {
            name: self.observer.successes.get(
                role.game_memory.pid if role.game_memory else 0, 0
            )
            for name, role in self.roles.items()
        }

    def current_states(self) -> dict[str, dict[str, object]]:
        states: dict[str, dict[str, object]] = {}
        for name, role in self.roles.items():
            if role.game_memory:
                state = game_state(role.game_memory)
                for _ in range(2):
                    if "read_error" not in state:
                        break
                    time.sleep(0.001)
                    state = game_state(role.game_memory)
                states[name] = state
            else:
                states[name] = {"pid": None, "alive": False}
        return states

    def runtime_states(self) -> dict[str, dict[str, object]]:
        states = self.current_states()
        for role in self.spectators:
            if role.game_memory:
                state = game_state(role.game_memory)
                for _ in range(2):
                    if "read_error" not in state:
                        break
                    time.sleep(0.001)
                    state = game_state(role.game_memory)
                states[role.name] = state
            else:
                states[role.name] = {"pid": None, "alive": False}
        for role in self.auxiliary_roles:
            states[role.name] = {
                "pid": None,
                "alive": False,
                "expected_no_game": True,
                "caster_exit": (
                    role.process.returncode
                    if role.process is not None
                    else None
                ),
            }
        return states

    def validate_logs_and_route(self) -> None:
        for name, role in self.roles.items():
            dll = role.dll_text()
            missing = [
                marker for marker in REQUIRED_DLL_MARKERS if marker not in dll
            ]
            if missing:
                raise HarnessFailure(
                    f"{name} DLL log lacks required markers: {missing}"
                )
            for marker in FORBIDDEN_MARKERS:
                found = (
                    marker in dll
                    if marker == "[CRASH]"
                    else marker.casefold() in dll.casefold()
                )
                if found:
                    raise HarnessFailure(
                        f"{name} DLL log contains forbidden marker {marker!r}"
                    )
        self.validate_route_evidence()
        self.validate_asymmetric_path()

    def validate_asymmetric_path(self) -> None:
        expectation = self.args.expect_asymmetric_path
        if not expectation:
            return
        deadband = 2
        median_bound = deadband + 2
        mirror_bound = 3

        def median(values: list[int]) -> int:
            ordered = sorted(values)
            return ordered[len(ordered) // 2]

        medians: dict[str, int] = {}
        raw_medians: dict[str, int] = {}
        compensations: dict[str, int] = {}
        for name, role in self.roles.items():
            samples = [
                (int(m.group("peer_adv_est")), int(m.group("peer_adv_raw")))
                for m in PACING_WORLD_RE.finditer(role.dll_text())
                if m.group("gameplay") != "0"
                and m.group("peer_fresh") == "1"
            ]
            if len(samples) < 5:
                raise HarnessFailure(
                    f"{name} produced only {len(samples)} fresh gameplay "
                    "pacing-world records; not enough evidence for the "
                    "asymmetric-path check"
                )
            medians[name] = median([est for est, _ in samples])
            raw_medians[name] = median([raw for _, raw in samples])
            compensations[name] = median(
                [raw - est for est, raw in samples]
            )
            if compensations[name] < 1:
                raise HarnessFailure(
                    f"{name} applied no transit compensation on an impaired "
                    f"path: median(peerAdvRaw - peerAdvEst)="
                    f"{compensations[name]} (raw={raw_medians[name]} "
                    f"est={medians[name]})"
                )
            if abs(medians[name]) > median_bound:
                raise HarnessFailure(
                    f"{name} steady frame-advantage estimate escaped the "
                    f"regulator's bound (+/-{median_bound}): "
                    f"median={medians[name]} raw={raw_medians[name]}"
                )
        mirror = sum(medians.values())
        if abs(mirror) > mirror_bound:
            raise HarnessFailure(
                "asymmetric path did not equalize into one consistent phase: "
                f"median frame-advantage estimates {medians} sum to {mirror} "
                f"(mirrored peers must sum within +/-{mirror_bound}; a "
                "same-signed pair means both peers believe they lead or both "
                "believe they trail)"
            )
        self.report["asymmetric_path"] = {
            "profile_direction": expectation,
            "median_frame_advantage_estimate": medians,
            "median_frame_advantage_raw": raw_medians,
            "median_transit_compensation_frames": compensations,
            "mirror_sum": mirror,
            "regulator_median_bound": median_bound,
            "mirror_bound": mirror_bound,
        }
        self.event(
            "asymmetric-path-validated",
            expectation=expectation,
            medians=medians,
            raw_medians=raw_medians,
            compensation=compensations,
            mirror_sum=mirror,
        )

    def rematch_layout_barrier_seconds(self, role: "Role") -> float:
        barriers = [
            int(m.group("elapsed"))
            for m in LIFECYCLE_DISPATCH_RETURN_RE.finditer(role.dll_text())
            if int(m.group("match_before")) == -1
            and int(m.group("match_after")) == 0
        ]
        if not barriers:
            return 0.0
        return max(0.0, (barriers[-1] - 17) / 1000.0)

    NATIVE_LOADER_DISPATCH_MS = 50
    MAX_NATIVE_LOADER_DISPATCHES = 4
    MAX_NATIVE_LOADER_SECONDS = 1.5

    def native_loader_dispatch_seconds(
        self, role: "Role", first_frame: int, last_frame: int
    ) -> tuple[float, list[dict[str, int]]]:
        spans: list[dict[str, int]] = []
        for match in LIFECYCLE_DISPATCH_RETURN_RE.finditer(role.dll_text()):
            frame = int(match.group("frame"))
            elapsed = int(match.group("elapsed"))
            if frame < first_frame or frame > last_frame:
                continue
            if elapsed < self.NATIVE_LOADER_DISPATCH_MS:
                continue
            advanced_lifecycle = any(
                match.group(f"{field}_before") != match.group(f"{field}_after")
                for field in ("scene", "next", "match", "scene3")
            )
            if not advanced_lifecycle:
                continue
            spans.append(
                {
                    "frame": frame,
                    "elapsed_ms": elapsed,
                    "match_sub": int(match.group("match_after")),
                    "scene": int(match.group("scene_after")),
                }
            )
        seconds = sum(max(0, span["elapsed_ms"] - 17) for span in spans) / 1000.0
        if (
            len(spans) > self.MAX_NATIVE_LOADER_DISPATCHES
            or seconds > self.MAX_NATIVE_LOADER_SECONDS
        ):
            return 0.0, spans
        return seconds, spans

    def validate_session_names(self) -> None:
        expected = {
            "host": ("QOH99Host", "QOH99Client", 1),
            "client": ("QOH99Client", "QOH99Host", 2),
        }
        evidence: dict[str, object] = {}
        for name, (local_name, remote_name, local_player) in expected.items():
            text = self.roles[name].dll_text()
            ipc_marker = (
                f"localName='{local_name}' remoteName='{remote_name}'"
            )
            ordered_marker = (
                "QOH99Host(P1) vs QOH99Client(P2)"
            )
            if ipc_marker not in text or ordered_marker not in text:
                raise HarnessFailure(
                    f"{name} did not retain canonical session names: "
                    f"ipc={ipc_marker in text} ordered="
                    f"{ordered_marker in text}"
                )
            evidence[name] = {
                "local": local_name,
                "remote": remote_name,
                "local_player": local_player,
                "ipc_marker": ipc_marker,
                "ordered_marker": ordered_marker,
            }

        for role in self.spectators:
            text = role.dll_text()
            marker = (
                "[spectator] GameStart players='QOH99Host'(P1) "
                "vs 'QOH99Client'(P2)"
            )
            if marker not in text:
                raise HarnessFailure(
                    f"{role.name} did not receive the immutable P1/P2 "
                    "nickname metadata"
                )
            evidence[role.name] = {"game_start_marker": marker}
        self.report["session_name_evidence"] = evidence

    @staticmethod
    def _delay_script_steps(script: str) -> list[tuple[int, int]]:
        return [
            tuple(int(value) for value in item.split(":", 1))
            for item in script.split(",")
            if item
        ]

    def validate_runtime_delay_logs(self) -> None:
        scripts = {
            "host": self.args.host_delay_script,
            "client": self.args.client_delay_script,
        }
        if not any(scripts.values()):
            return

        evidence: dict[str, object] = {}
        for name, script in scripts.items():
            if not script:
                continue
            role = self.roles[name]
            text = role.dll_text()
            expected = self._delay_script_steps(script)
            requests = list(TEST_DELAY_REQUEST_RE.finditer(text))
            applied = list(TEST_DELAY_APPLIED_RE.finditer(text))
            expected_player = 1 if name == "host" else 2
            if len(requests) != len(expected):
                raise HarnessFailure(
                    f"{name} runtime-delay request count differs: "
                    f"expected={expected} observed="
                    f"{[match.group(0) for match in requests]}"
                )
            if len(applied) != len(expected):
                raise HarnessFailure(
                    f"{name} runtime-delay application count differs: "
                    f"expected={expected} observed="
                    f"{[match.group(0) for match in applied]}"
                )
            parsed_requests = [
                (
                    int(match.group("tick")),
                    int(match.group("delay")),
                    int(match.group("player")),
                    int(match.group("frame")),
                )
                for match in requests
            ]
            parsed_applied = [
                (
                    int(match.group("delay")),
                    int(match.group("player")),
                    int(match.group("frame")),
                )
                for match in applied
            ]
            if [
                (tick, delay) for tick, delay, _player, _frame
                in parsed_requests
            ] != expected:
                raise HarnessFailure(
                    f"{name} runtime-delay request order differs: "
                    f"expected={expected} observed={parsed_requests}"
                )
            if [delay for delay, _player, _frame in parsed_applied] != [
                delay for _tick, delay in expected
            ]:
                raise HarnessFailure(
                    f"{name} runtime-delay applied order differs: "
                    f"expected={expected} observed={parsed_applied}"
                )
            for request, application in zip(
                parsed_requests, parsed_applied
            ):
                _tick, _target, request_player, request_frame = request
                _delay, applied_player, applied_frame = application
                if (
                    request_player != expected_player
                    or applied_player != expected_player
                    or applied_frame < request_frame
                ):
                    raise HarnessFailure(
                        f"{name} runtime-delay role/boundary evidence "
                        f"is invalid: request={request} "
                        f"application={application}"
                    )
            evidence[name] = {
                "script": script,
                "requests": [match.group(0) for match in requests],
                "applied": [match.group(0) for match in applied],
            }

        if self.spectator is not None:
            spectator_text = self.spectator.dll_text()
            if "[test-delay]" in spectator_text:
                raise HarnessFailure(
                    "receive-only spectator executed a player runtime-delay "
                    "test hook"
                )
            evidence["spectator"] = {
                "runtime_delay_markers": 0,
            }
        self.report["runtime_delay_evidence"] = evidence

    def validate_route_evidence(self) -> None:
        expected = self.args.route
        host_evidence = (
            self.roles["host"].stdout_text()
            + "\n"
            + self.roles["host"].client_text()
        )
        client_evidence = (
            self.roles["client"].stdout_text()
            + "\n"
            + self.roles["client"].client_text()
        )
        host_contracts = [
            match
            for match in CASTER_ROUTE_CONTRACT_RE.finditer(host_evidence)
            if match.group("role") == "host"
        ]
        client_contracts = [
            match
            for match in CASTER_ROUTE_CONTRACT_RE.finditer(client_evidence)
            if match.group("role") == "client"
        ]
        if not host_contracts:
            raise HarnessFailure(
                "host did not emit a committed route contract"
            )
        if not client_contracts:
            raise HarnessFailure(
                "client did not emit a committed route contract"
            )
        host_contract = host_contracts[-1]
        client_contract = client_contracts[-1]
        host_route = host_contract.group("route")
        client_route = client_contract.group("route")
        host_session = int(host_contract.group("session"))
        client_session = int(client_contract.group("session"))
        if (
            host_route != expected
            or client_route != expected
            or host_session == 0
            or host_session != client_session
        ):
            raise HarnessFailure(
                "caster route contracts disagree with the requested session: "
                f"expected={expected} "
                f"host=({host_route},{host_session}) "
                f"client=({client_route},{client_session})"
            )
        fallback_contracts: list[re.Match[str]] = []
        if expected == "punched-direct":
            fallback_contracts = list(
                CASTER_ROUTE_FALLBACK_CONTRACT_RE.finditer(client_evidence)
            )
        self.report["caster_route_contract"] = {
            "route": expected,
            "session": host_session,
            "host_line": host_contract.group(0),
            "client_line": client_contract.group(0),
            "fallback_line": (
                fallback_contracts[-1].group(0)
                if fallback_contracts
                else None
            ),
        }

        evidence_texts = {"host": host_evidence, "client": client_evidence}
        expected_label = "relay" if expected == "relay" else "lan-direct"
        route_evidence_report: dict[str, object] = {
            "expected_label": expected_label
        }
        recommend_by_role: dict[str, re.Match[str]] = {}
        prelaunch_by_role: dict[str, re.Match[str]] = {}
        for name, text in evidence_texts.items():
            stats = [
                m
                for m in CASTER_ROUTE_STATS_RE.finditer(text)
                if m.group("role") == name
                and m.group("phase") == "prelaunch"
            ]
            if not stats:
                raise HarnessFailure(
                    f"{name} did not emit parseable prelaunch [route-stats] "
                    "evidence"
                )
            prelaunch_by_role[name] = stats[-1]
            labels = [
                m
                for m in CASTER_ROUTE_LABEL_RE.finditer(text)
                if m.group("role") == name
            ]
            if not labels:
                raise HarnessFailure(
                    f"{name} did not emit a parseable [route-label] line"
                )
            label = labels[-1]
            if label.group("label") != expected_label:
                raise HarnessFailure(
                    f"{name} committed route label "
                    f"'{label.group('label')}' but this "
                    f"{expected} session requires '{expected_label}'"
                )
            recommends = [
                m
                for m in CASTER_ROUTE_RECOMMEND_RE.finditer(text)
                if m.group("role") == name
            ]
            if not recommends:
                raise HarnessFailure(
                    f"{name} did not emit a parseable [route-recommend] "
                    "line (or emitted the NO-EVIDENCE form)"
                )
            recommend_by_role[name] = recommends[-1]
            route_evidence_report[name] = {
                "prelaunch": stats[-1].group(0),
                "label": label.group(0),
                "recommend": recommends[-1].group(0),
            }

        if self.args.expect_underbuffered:
            for name, role in self.roles.items():
                runtime_classes = [
                    m
                    for m in DLL_ROUTE_RUNTIME_CLASS_RE.finditer(
                        role.dll_text()
                    )
                ]
                if not runtime_classes:
                    raise HarnessFailure(
                        f"{name} DLL did not emit a parseable "
                        "[route-runtime-class] line for the negative control"
                    )
                final = runtime_classes[-1]
                classes = (
                    final.group("host_class"),
                    final.group("client_class"),
                )
                if "underbuffered" not in classes:
                    raise HarnessFailure(
                        f"{name} runtime reclassification called the "
                        f"deliberately underbuffered settings {classes}; the "
                        "negative control must be diagnosed honestly"
                    )
                entry = route_evidence_report.get(name)
                if isinstance(entry, dict):
                    entry["runtime_class"] = final.group(0)

        agreement_us = int(self.args.route_p50_agreement_us)
        if self.args.network_profile == "none":
            for name, role in self.roles.items():
                samples = [
                    m
                    for m in DLL_ROUTE_SAMPLE_RE.finditer(role.dll_text())
                    if int(m.group("samples")) >= 32
                ]
                if not samples:
                    continue
                runtime = samples[-1]
                pre_p50_us = int(prelaunch_by_role[name].group("p50"))
                run_p50_us = int(runtime.group("p50"))
                if abs(run_p50_us - pre_p50_us) > agreement_us:
                    raise HarnessFailure(
                        f"{name} prelaunch route p50 {pre_p50_us}us and "
                        f"runtime route p50 {run_p50_us}us disagree by more "
                        f"than {agreement_us / 1000.0:.1f} ms on the "
                        "unimpaired route"
                    )
                entry = route_evidence_report.get(name)
                if isinstance(entry, dict):
                    entry["runtime_sample"] = runtime.group(0)
                    entry["prelaunch_p50_us"] = pre_p50_us
                    entry["runtime_p50_us"] = run_p50_us
        route_evidence_report["p50_agreement_us"] = agreement_us
        self.report["route_evidence"] = route_evidence_report

    @staticmethod
    def first_layout_regions(text: str) -> dict[str, dict[str, object]]:
        regions: dict[str, dict[str, object]] = {}
        for match in LAYOUT_REGION_RE.finditer(text):
            name = match.group("name")
            if name in regions:
                continue
            regions[name] = {
                "index": int(match.group("index")),
                "size": int(match.group("size")),
                "present": int(match.group("present")),
                "hash": match.group("hash").upper(),
                "line": match.group(0),
            }
        return regions

    def validate_layout_command_mutation(self) -> None:
        target = self.args.layout_command_mutation
        if target == "none":
            return

        host_text = self.roles["host"].dll_text()
        client_text = self.roles["client"].dll_text()
        host_hooks = list(LAYOUT_COMMAND_MUTATION_RE.finditer(host_text))
        client_hooks = list(LAYOUT_COMMAND_MUTATION_RE.finditer(client_text))
        if host_hooks:
            raise HarnessFailure(
                "layout command-history mutation leaked into the host process"
            )
        if len(client_hooks) != 1:
            raise HarnessFailure(
                "client did not apply exactly one layout command-history "
                f"mutation: observed={len(client_hooks)}"
            )
        hook = client_hooks[0]
        if (
            hook.group("target") != target
            or int(hook.group("generation")) == 0
            or int(hook.group("offset")) != 10
            or hook.group("before").upper() == hook.group("after").upper()
            or hook.group("raw_before").upper()
            == hook.group("raw_after").upper()
        ):
            raise HarnessFailure(
                "client layout command-history mutation evidence is invalid: "
                f"{hook.group(0)}"
            )

        host_regions = self.first_layout_regions(host_text)
        client_regions = self.first_layout_regions(client_text)
        required = ("cmd_p1", "cmd_p2", "cmd_p3", "cmd_p4")
        missing = [
            f"{role}:{name}"
            for role, regions in (
                ("host", host_regions),
                ("client", client_regions),
            )
            for name in required
            if name not in regions
        ]
        if missing:
            raise HarnessFailure(
                "layout command-history region evidence is incomplete: "
                + ", ".join(missing)
            )
        expected_presence = {
            "cmd_p1": 1,
            "cmd_p2": 1,
            "cmd_p3": 0,
            "cmd_p4": 0,
        }
        for name in required:
            for role, regions in (
                ("host", host_regions),
                ("client", client_regions),
            ):
                if (
                    regions[name]["size"] != 512
                    or regions[name]["present"] != expected_presence[name]
                ):
                    raise HarnessFailure(
                        f"{role} {name} layout evidence does not have the "
                        "expected 1v1 activation-gated presence "
                        f"{expected_presence[name]}: {regions[name]}"
                    )

        p1_equal = (
            host_regions["cmd_p1"]["hash"]
            == client_regions["cmd_p1"]["hash"]
        )
        p2_equal = (
            host_regions["cmd_p2"]["hash"]
            == client_regions["cmd_p2"]["hash"]
        )
        p3_equal = (
            host_regions["cmd_p3"]["hash"]
            == client_regions["cmd_p3"]["hash"]
        )
        p4_equal = (
            host_regions["cmd_p4"]["hash"]
            == client_regions["cmd_p4"]["hash"]
        )
        if target == "inactive-p4":
            if not p1_equal or not p2_equal or not p3_equal or not p4_equal:
                raise HarnessFailure(
                    "inactive-P4 mutation was not isolated from the canonical "
                    "layout fingerprint: "
                    f"p1Equal={p1_equal} p2Equal={p2_equal} "
                    f"p3Equal={p3_equal} "
                    f"p4Equal={p4_equal}"
                )
        else:
            if p2_equal:
                raise HarnessFailure(
                    "active-P2 command-history mutation was omitted from the "
                    "canonical layout fingerprint"
                )
            if not p3_equal or not p4_equal:
                raise HarnessFailure(
                    "inactive P3/P4 command regions changed during the "
                    "active-P2 negative control"
                )

        evidence = {
            "target": target,
            "mutation_line": hook.group(0),
            "host_has_mutation": False,
            "command_region_hashes": {
                name: {
                    "host": host_regions[name],
                    "client": client_regions[name],
                    "equal": (
                        host_regions[name]["hash"]
                        == client_regions[name]["hash"]
                    ),
                }
                for name in required
            },
        }
        self.report["layout_command_mutation_evidence"] = evidence
        self.event(
            "layout-command-mutation-verified",
            target=target,
            p1_equal=p1_equal,
            p2_equal=p2_equal,
            p3_equal=p3_equal,
            p4_equal=p4_equal,
            mutation_role="client",
        )

    def wait_for_expected_layout_mismatch(
        self, timeout: float = 45.0
    ) -> None:
        if self.args.layout_command_mutation != "active-p2":
            raise HarnessFailure(
                "expected-layout-mismatch wait requires active-p2 mutation"
            )
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            host = self.roles["host"]
            client = self.roles["client"]
            host_text = host.dll_text()
            client_text = client.dll_text()

            for role in self.roles.values():
                text = role.dll_text() + "\n" + role.client_text()
                if "[CRASH]" in text or " faulted" in text.casefold():
                    raise HarnessFailure(
                        f"{role.name} crashed while waiting for the expected "
                        "active-command layout rejection"
                    )
                if list(role.root.glob("logs/session_*/*.dmp")):
                    raise HarnessFailure(
                        f"{role.name} produced a crash dump while waiting for "
                        "the expected active-command layout rejection"
                    )

            host_terminals = list(TERMINAL_CODE_RE.finditer(host_text))
            client_terminals = list(TERMINAL_CODE_RE.finditer(client_text))
            host_originated = (
                "[netplay] layout rendezvous mismatch" in host_text
                and "post-load gameplay layout/content differs from peer"
                in host_text
            )
            client_originated = (
                "[netplay] layout rendezvous mismatch" in client_text
                and "post-load gameplay layout/content differs from peer"
                in client_text
            )
            ready = (
                "[test-hook] layout command-history mutation" in client_text
                and "target=active-p2" in client_text
                and (host_originated or client_originated)
                and bool(host_terminals)
                and bool(client_terminals)
            )
            if ready:
                host_terminal = host_terminals[-1]
                client_terminal = client_terminals[-1]
                host_code = int(host_terminal.group("code"))
                client_code = int(client_terminal.group("code"))
                host_detail = int(host_terminal.group("detail"))
                client_detail = int(client_terminal.group("detail"))
                if (
                    host_code != 7
                    or client_code != 7
                    or host_detail != client_detail
                    or host_detail == 0
                ):
                    raise HarnessFailure(
                        "active-command mismatch did not produce the exact "
                        "symmetric code-7 terminal: "
                        f"host=({host_code},{host_detail}) "
                        f"client=({client_code},{client_detail})"
                    )
                self.validate_layout_command_mutation()
                self.validate_route_evidence()
                self.report["expected_layout_terminal"] = {
                    "code": 7,
                    "detail": client_detail,
                    "origin": (
                        "both peers"
                        if host_originated and client_originated
                        else "host"
                        if host_originated
                        else "client"
                    ),
                    "cause": "client active-P2 layout fingerprint mutation",
                    "host_line": host_terminal.group(0),
                    "client_line": client_terminal.group(0),
                }
                self.event(
                    "expected-active-layout-mismatch-verified",
                    code=7,
                    detail=client_detail,
                    origin=(
                        "both"
                        if host_originated and client_originated
                        else "host"
                        if host_originated
                        else "client"
                    ),
                    host_frame=int(host_terminal.group("frame")),
                    client_frame=int(client_terminal.group("frame")),
                )
                return

            for role in self.roles.values():
                if (
                    role.process
                    and role.process.poll() is not None
                    and (
                        not role.game_memory
                        or not role.game_memory.alive()
                    )
                ):
                    raise HarnessFailure(
                        f"{role.name} exited before the expected active-command "
                        "layout rejection was fully logged"
                    )
            time.sleep(0.01)
        raise HarnessFailure(
            "active-P2 command-history mutation did not produce the expected "
            "symmetric code-7 layout rejection"
        )

    def validate_config_stress_runtime_logs(self) -> None:
        if self.args.config_variant != "config-stress":
            return

        host_values = dict(QOH_CONFIG_STRESS_SPEC["host"]["values"])
        authoritative_match = tuple(
            host_values[offset] for offset in (36, 40, 44, 48)
        )
        evidence: dict[str, object] = {}
        for name, role in self.roles.items():
            text = role.dll_text()
            spec = QOH_CONFIG_STRESS_SPEC[name]
            profile = int(spec["profile"])
            local_values = dict(spec["values"])

            capture_marker = (
                f"[config] captured local profile {profile} "
                "before runtime overrides"
            )
            capture_lines = [
                line for line in text.splitlines()
                if capture_marker in line
            ]
            if len(capture_lines) != 1:
                raise HarnessFailure(
                    f"{name} config-stress log has "
                    f"{len(capture_lines)} exact profile-capture lines, "
                    "expected one"
                )

            go_lines = [
                line for line in text.splitlines()
                if "[netplay] GO:" in line
            ]
            go_matches = list(NETPLAY_GO_RE.finditer(text))
            if len(go_lines) != 1 or len(go_matches) != 1:
                raise HarnessFailure(
                    f"{name} config-stress GO evidence is not singular "
                    f"and parseable: lines={len(go_lines)} "
                    f"matches={len(go_matches)}"
                )
            go = go_matches[0]
            observed_match = tuple(
                int(go.group(field))
                for field in ("timer", "ai", "life", "rounds")
            )
            expected_local = 1 if name == "host" else 2
            if (
                observed_match != authoritative_match
                or int(go.group("local")) != expected_local
            ):
                raise HarnessFailure(
                    f"{name} did not apply P1-authoritative config at GO: "
                    f"match={observed_match} local=P{go.group('local')} "
                    f"expected={authoritative_match}/P{expected_local}"
                )

            layout_lines = [
                line for line in text.splitlines()
                if "[layout-config] valid=1 " in line
            ]
            layout_matches = list(LAYOUT_CONFIG_RE.finditer(text))
            if (
                not layout_lines
                or len(layout_matches) != len(layout_lines)
            ):
                raise HarnessFailure(
                    f"{name} config-stress layout evidence is incomplete "
                    f"or unparsable: lines={len(layout_lines)} "
                    f"matches={len(layout_matches)}"
                )
            parsed_layouts: list[dict[str, object]] = []
            for layout in layout_matches:
                layout_match = tuple(
                    int(layout.group(field))
                    for field in ("timer", "ai", "life", "rounds")
                )
                normalized = {
                    offset: int(layout.group(f"local{offset}"))
                    for offset in QOH_CONFIG_NORMALIZED_OFFSETS
                }
                retained_local = {
                    68: int(layout.group("local68")),
                    108: int(layout.group("local108")),
                    376: int(layout.group("local376"), 16),
                }
                expected_retained = {
                    offset: local_values[offset]
                    for offset in (68, 108, 376)
                }
                if (
                    int(layout.group("profile")) != profile
                    or layout_match != authoritative_match
                    or any(normalized.values())
                    or retained_local != expected_retained
                ):
                    raise HarnessFailure(
                        f"{name} config-stress runtime policy mismatch: "
                        f"profile={layout.group('profile')} "
                        f"match={layout_match} normalized={normalized} "
                        f"retained={retained_local}; expected "
                        f"profile={profile} match={authoritative_match} "
                        f"retained={expected_retained}"
                    )
                parsed_layouts.append(
                    {
                        "profile": profile,
                        "match": layout_match,
                        "normalized": normalized,
                        "retained_local": retained_local,
                    }
                )
            evidence[name] = {
                "capture_line": capture_lines[0],
                "go_line": go_lines[0],
                "layout_lines": layout_lines,
                "parsed_layouts": parsed_layouts,
            }

        self.report["config_stress_runtime"] = {
            "authoritative_match": authoritative_match,
            "evidence": evidence,
        }
        self.event(
            "config-stress-runtime-verified",
            authoritative_match=authoritative_match,
            profiles={
                name: int(spec["profile"])
                for name, spec in QOH_CONFIG_STRESS_SPEC.items()
            },
            normalized_offsets=list(QOH_CONFIG_NORMALIZED_OFFSETS),
        )

    @staticmethod
    def _log_clock_milliseconds(clock: str) -> int:
        hours, minutes, seconds = clock.split(":")
        whole_seconds, milliseconds = seconds.split(".")
        return (
            int(hours) * 3_600_000
            + int(minutes) * 60_000
            + int(whole_seconds) * 1_000
            + int(milliseconds)
        )

    @staticmethod
    def _gameplay_entry_frames(text: str) -> list[int]:
        return [
            int(match.group("frame"))
            for match in AUTOMATCH_LIFECYCLE_RE.finditer(text)
            if match.group("event") == "ENTER"
        ]

    @staticmethod
    def _annotate_pacing_window_ranges(
        records: list[dict[str, object]]
    ) -> None:
        previous = 0
        for record in records:
            record["window_first_frame"] = previous + 1
            previous = int(record["frame"])

    @staticmethod
    def _pacing_sample_contains_gameplay_entry(
        record: dict[str, object], gameplay_entry_frames: Iterable[int]
    ) -> bool:
        frame = int(record["frame"])
        visible = int(record["visible"])
        first_frame = int(
            record.get("window_first_frame", frame - visible + 1)
        )
        return any(
            first_frame <= entry_frame <= frame
            for entry_frame in gameplay_entry_frames
        )

    def minimum_fps_for(self, role_name: str) -> float:
        if (
            self.is_remote_role(role_name)
            and self.args.remote_minimum_canonical_fps >= 0.0
        ):
            return float(self.args.remote_minimum_canonical_fps)
        return float(self.args.minimum_canonical_fps)

    def validate_phase_fps_log_derived(
        self, pacing: dict[str, dict[str, object]]
    ) -> None:
        if not self.distributed_mode:
            return
        derived: dict[str, object] = {}
        for name, sample in pacing.items():
            if not self.is_remote_role(name):
                continue
            role = self.roles.get(name) or next(
                (item for item in self.runtime_roles() if item.name == name),
                None,
            )
            if role is None:
                continue
            floor = self.minimum_fps_for(name)
            text = role.dll_text()
            windows = [
                {
                    "frame": int(match.group("frame")),
                    "visible": int(match.group("visible")),
                    "elapsed_us": int(match.group("elapsed_us")),
                }
                for match in PACING_PRESENT_RE.finditer(text)
            ]
            if not windows:
                raise HarnessFailure(
                    f"{name} has no [pacing-present] evidence, so its "
                    "log-derived phase cadence cannot be measured; the "
                    "wall-clock floor is refused for remote roles, which "
                    "would leave this role unchecked"
                )
            phases = {
                "initial_gameplay": (
                    int(sample.get("initial_gameplay_start_frame", 0)),
                    int(sample.get("initial_gameplay_exit_frame", 0)),
                ),
                "frontend_transition": (
                    int(sample.get("frontend_transition_start_frame", 0)),
                    int(sample.get("rematch_entry_frame", 0)),
                ),
            }
            role_evidence: dict[str, object] = {"floor": floor}
            slow: dict[str, float] = {}
            for phase, (first, last) in phases.items():
                if last <= first:
                    continue
                selected = [
                    window
                    for window in windows
                    if first <= window["frame"] <= last
                ]
                visible = sum(window["visible"] for window in selected)
                elapsed_us = sum(
                    window["elapsed_us"] for window in selected
                )
                if not selected or elapsed_us <= 0:
                    raise HarnessFailure(
                        f"{name} has no [pacing-present] window inside its "
                        f"{phase} span [{first}, {last}]; the log-derived "
                        "floor cannot be evaluated"
                    )
                rate = visible * 1_000_000.0 / elapsed_us
                role_evidence[phase] = {
                    "windows": len(selected),
                    "visible": visible,
                    "elapsed_us": elapsed_us,
                    "fps": round(rate, 3),
                    "frame_span": [first, last],
                }
                phase_floor = (
                    floor if phase == "initial_gameplay" else floor - 1.0
                )
                if rate < phase_floor:
                    slow[phase] = round(rate, 3)
            if slow:
                raise HarnessFailure(
                    f"{name} log-derived canonical pacing fell below "
                    f"{floor:.1f} fps on its own microsecond clock: {slow} "
                    f"(evidence={role_evidence})"
                )
            derived[name] = role_evidence
        performance = self.report.setdefault("performance", {})
        assert isinstance(performance, dict)
        for name, evidence in derived.items():
            entry = performance.setdefault(name, {})
            assert isinstance(entry, dict)
            entry["log_derived_phase_fps"] = evidence
            entry["floor_variant"] = "log-derived (remote)"
        for name in pacing:
            if self.is_remote_role(name):
                continue
            entry = performance.setdefault(name, {})
            assert isinstance(entry, dict)
            entry["wall_clock_phase_fps"] = {
                "initial_gameplay_fps": pacing[name].get(
                    "initial_gameplay_fps"
                ),
                "frontend_transition_fps": pacing[name].get(
                    "frontend_transition_fps"
                ),
            }
            entry["floor_variant"] = "wall-clock (local)"
        if derived:
            self.event("log-derived-phase-fps-verified", roles=derived)

    def validate_pacing_present_logs(self, after_shutdown: bool) -> None:
        if not self.args.full_match_rematch:
            return

        integer_fields = (
            "frame",
            "elapsed_us",
            "visible",
            "gameplay",
            "interval_min",
            "interval_p95",
            "interval_max",
            "burst",
            "late",
        )
        evidence: dict[str, object] = {}
        for role in self.runtime_roles():
            minimum_fps = self.minimum_fps_for(role.name)
            text = role.dll_text()
            tagged_lines = [
                line
                for line in text.splitlines()
                if "[pacing-present]" in line
            ]
            matches = list(PACING_PRESENT_RE.finditer(text))
            if len(matches) != len(tagged_lines):
                parsed_lines = {match.group(0) for match in matches}
                unmatched = [
                    line for line in tagged_lines
                    if line not in parsed_lines
                ]
                raise HarnessFailure(
                    f"{role.name} has malformed pacing-present telemetry: "
                    f"tagged={len(tagged_lines)} parsed={len(matches)} "
                    f"unmatched={unmatched[:3]}"
                )
            if not matches:
                raise HarnessFailure(
                    f"{role.name} lacks pacing-present cadence evidence "
                    "for the full-match/rematch run"
                )

            hold_window_frames: list[int] = []
            previous_fa_holds: int | None = None
            for world in PACING_WORLD_RE.finditer(text):
                fa_total = int(world.group("frame_advantage_holds"))
                fa_delta = (
                    fa_total - previous_fa_holds
                    if previous_fa_holds is not None
                    else fa_total
                )
                previous_fa_holds = fa_total
                if (
                    int(world.group("stalls")) > 0
                    or int(world.group("exact_waits")) > 0
                    or int(world.group("deadline_rebases")) > 0
                    or fa_delta > 0
                ):
                    hold_window_frames.append(int(world.group("frame")))

            def window_held(present_frame: int) -> bool:
                return any(
                    abs(present_frame - world_frame) <= 2
                    for world_frame in hold_window_frames
                )

            records: list[dict[str, object]] = []
            for match in matches:
                record: dict[str, object] = {
                    "clock": match.group("clock"),
                    "visible_fps": float(match.group("visible_fps")),
                }
                record.update(
                    {
                        field: int(match.group(field))
                        for field in integer_fields
                    }
                )
                records.append(record)

            frames = [int(record["frame"]) for record in records]
            if any(
                current <= previous
                for previous, current in zip(frames, frames[1:])
            ):
                raise HarnessFailure(
                    f"{role.name} pacing-present frames are not strictly "
                    f"increasing: {frames}"
                )
            self._annotate_pacing_window_ranges(records)

            gameplay_entry_frames = self._gameplay_entry_frames(text)
            transition_windows: list[dict[str, object]] = []
            entry_windows: list[dict[str, object]] = []
            steady_gameplay_windows: list[dict[str, object]] = []
            for record in records:
                visible = int(record["visible"])
                gameplay = int(record["gameplay"])
                elapsed_us = int(record["elapsed_us"])
                if elapsed_us <= 0 or visible <= 0:
                    raise HarnessFailure(
                        f"{role.name} pacing-present sample has no positive "
                        f"duration/output: {record}"
                    )
                if not 0 <= gameplay <= visible:
                    raise HarnessFailure(
                        f"{role.name} pacing-present gameplay accounting "
                        f"is invalid: gameplay={gameplay} visible={visible} "
                        f"frame={record['frame']}"
                    )
                interval_min = int(record["interval_min"])
                interval_p95 = int(record["interval_p95"])
                interval_max = int(record["interval_max"])
                if not (
                    0 < interval_min <= interval_p95 <= interval_max
                ):
                    raise HarnessFailure(
                        f"{role.name} pacing-present interval ordering is "
                        f"invalid at frame {record['frame']}: "
                        f"{interval_min}/{interval_p95}/{interval_max}"
                    )
                if int(record["burst"]) != 0:
                    raise HarnessFailure(
                        f"{role.name} exposed a <8 ms presentation burst "
                        f"at frame {record['frame']}: "
                        f"burstIntervals={record['burst']}"
                    )
                if visible < 45 or gameplay != visible:
                    classification = "transition"
                    transition_windows.append(record)
                elif self._pacing_sample_contains_gameplay_entry(
                    record, gameplay_entry_frames
                ):
                    classification = "gameplay-entry"
                    entry_windows.append(record)
                else:
                    classification = "steady-full-gameplay"
                    steady_gameplay_windows.append(record)
                record["classification"] = classification

            if not steady_gameplay_windows:
                raise HarnessFailure(
                    f"{role.name} lacks a steady full-gameplay "
                    "pacing-present sample"
                )
            gameplay_visible_ticks = sum(
                int(record["gameplay"]) for record in records
            )
            frontend_visible_ticks = sum(
                int(record["visible"]) - int(record["gameplay"])
                for record in records
            )
            if role is self.spectator:
                if not transition_windows:
                    raise HarnessFailure(
                        "spectator lacks a frontend/results transition "
                        "pacing-present sample"
                    )
                if gameplay_visible_ticks <= 0:
                    raise HarnessFailure(
                        "spectator pacing-present telemetry proves no "
                        "successful gameplay output"
                    )
                if frontend_visible_ticks <= 0:
                    raise HarnessFailure(
                        "spectator pacing-present telemetry proves no "
                        "successful frontend/results output"
                    )

            maximum_hitch_us = 250_000
            maximum_isolated_hitch_late_intervals = 2
            for record in steady_gameplay_windows:
                frame = int(record["frame"])
                visible_fps = float(record["visible_fps"])
                interval_p95 = int(record["interval_p95"])
                interval_max = int(record["interval_max"])
                if interval_max > maximum_hitch_us:
                    raise HarnessFailure(
                        f"{role.name} steady presentation hitch exceeded "
                        f"{maximum_hitch_us // 1000} ms at frame {frame}: "
                        f"{interval_max} us"
                    )
                if visible_fps > 65.0:
                    raise HarnessFailure(
                        f"{role.name} steady visible-output cadence exceeded "
                        f"65.0 fps at frame {frame}: {visible_fps:.2f}"
                    )
                if interval_p95 > 25_000 and not window_held(frame):
                    raise HarnessFailure(
                        f"{role.name} steady visible-output p95 exceeded "
                        f"25 ms at frame {frame} with no scheduler hold: "
                        f"{interval_p95} us"
                    )

            correction_work_by_frame = {
                int(match.group("frame")): int(
                    match.group("correction_work_us")
                )
                for match in PACING_WORLD_RE.finditer(text)
            }
            slow_steady_windows: list[dict[str, object]] = []
            excused_steady_windows: list[dict[str, object]] = []
            consecutive_slow = 0
            steady_fps_chain: list[float] = []
            for index, record in enumerate(records):
                if record["classification"] != "steady-full-gameplay":
                    consecutive_slow = 0
                    steady_fps_chain.clear()
                    continue

                visible_fps = float(record["visible_fps"])
                steady_fps_chain.append(visible_fps)
                if len(steady_fps_chain) >= 5:
                    window5 = steady_fps_chain[-5:]
                    if all(v < minimum_fps + 0.5 for v in window5):
                        raise HarnessFailure(
                            f"{role.name} sustained five-window steady "
                            f"interval below {minimum_fps + 0.5:.1f} fps "
                            f"ending at frame {record['frame']}: "
                            f"{[round(v, 2) for v in window5]}"
                        )
                slow = visible_fps < minimum_fps
                consecutive_slow = consecutive_slow + 1 if slow else 0
                if consecutive_slow >= 3:
                    raise HarnessFailure(
                        f"{role.name} has three consecutive slow steady "
                        "pacing-present windows ending at frame "
                        f"{record['frame']}"
                    )
                if not slow:
                    continue

                shortfall_ms = (
                    (CANONICAL_TARGET_FPS - visible_fps)
                    / CANONICAL_TARGET_FPS
                    * 1000.0
                )
                correction_ms = (
                    correction_work_by_frame.get(int(record["frame"]), 0)
                    / 1000.0
                )
                record["shortfall_ms"] = round(shortfall_ms, 1)
                record["correction_work_ms"] = round(correction_ms, 1)
                if correction_ms >= shortfall_ms:
                    excused_steady_windows.append(record)
                else:
                    slow_steady_windows.append(record)
                if (
                    int(record["late"])
                    > maximum_isolated_hitch_late_intervals
                    and not window_held(int(record["frame"]))
                ):
                    raise HarnessFailure(
                        f"{role.name} slow steady window at frame "
                        f"{record['frame']} has too many late intervals "
                        f"for one isolated hitch: {record['late']}"
                    )
                if index + 1 >= len(records):
                    if consecutive_slow >= 2:
                        raise HarnessFailure(
                            f"{role.name} run ended during a multi-window "
                            "slow steady stretch with no recovery sample "
                            f"(final frame {record['frame']})"
                        )
                    self.event(
                        "isolated-tail-slow-window",
                        role=role.name,
                        frame=int(record["frame"]),
                        visible_fps=visible_fps,
                    )
                    continue

            allowed_slow_steady_windows = max(
                2, len(steady_gameplay_windows) // 20
            )
            slow_steady_frames = [
                int(record["frame"])
                for record in slow_steady_windows
            ]
            excused_steady_evidence = [
                {
                    "frame": int(record["frame"]),
                    "visible_fps": float(record["visible_fps"]),
                    "shortfall_ms": record["shortfall_ms"],
                    "correction_work_ms": record["correction_work_ms"],
                }
                for record in excused_steady_windows
            ]
            if excused_steady_evidence:
                self.event(
                    "deep-correction-steady-windows-excused",
                    role=role.name,
                    windows=excused_steady_evidence,
                )
            if (
                len(slow_steady_windows)
                > allowed_slow_steady_windows
            ):
                raise HarnessFailure(
                    f"{role.name} has too many slow steady "
                    "pacing-present windows: "
                    f"observed={len(slow_steady_windows)} "
                    f"allowed={allowed_slow_steady_windows} "
                    f"steady={len(steady_gameplay_windows)} "
                    f"frames={slow_steady_frames} "
                    "deepCorrectionExcused="
                    f"{excused_steady_evidence}"
                )

            weighted_steady_visible_fps: float | None = None
            if steady_gameplay_windows:
                steady_visible_ticks = sum(
                    int(record["visible"])
                    for record in steady_gameplay_windows
                )
                steady_elapsed_us = sum(
                    int(record["elapsed_us"])
                    for record in steady_gameplay_windows
                )
                weighted_steady_visible_fps = (
                    steady_visible_ticks * 1_000_000.0
                    / steady_elapsed_us
                )
                if not (
                    minimum_fps
                    <= weighted_steady_visible_fps
                    <= 65.0
                ):
                    raise HarnessFailure(
                        f"{role.name} weighted steady visible-output "
                        f"cadence left [{minimum_fps:.1f}, 65.0] fps: "
                        f"{weighted_steady_visible_fps:.2f} "
                        f"({steady_visible_ticks} ticks/"
                        f"{steady_elapsed_us} us)"
                    )

            evidence[role.name] = {
                "is_spectator": role is self.spectator,
                "minimum_visible_fps": minimum_fps,
                "maximum_visible_fps": 65.0,
                "maximum_steady_hitch_us": maximum_hitch_us,
                "maximum_isolated_hitch_late_intervals": (
                    maximum_isolated_hitch_late_intervals
                ),
                "all_window_count": len(records),
                "transition_window_count": len(transition_windows),
                "transition_frames": [
                    int(record["frame"])
                    for record in transition_windows
                ],
                "gameplay_entry_window_count": len(entry_windows),
                "lifecycle_gameplay_entry_frames": (
                    gameplay_entry_frames
                ),
                "gameplay_entry_frames": [
                    int(record["frame"]) for record in entry_windows
                ],
                "steady_full_gameplay_window_count": len(
                    steady_gameplay_windows
                ),
                "steady_full_gameplay_frames": [
                    int(record["frame"])
                    for record in steady_gameplay_windows
                ],
                "steady_visible_fps_range": (
                    [
                        min(
                            float(record["visible_fps"])
                            for record in steady_gameplay_windows
                        ),
                        max(
                            float(record["visible_fps"])
                            for record in steady_gameplay_windows
                        ),
                    ]
                    if steady_gameplay_windows
                    else None
                ),
                "maximum_steady_interval_p95_us": (
                    max(
                        int(record["interval_p95"])
                        for record in steady_gameplay_windows
                    )
                    if steady_gameplay_windows
                    else None
                ),
                "maximum_steady_interval_us": (
                    max(
                        int(record["interval_max"])
                        for record in steady_gameplay_windows
                    )
                    if steady_gameplay_windows
                    else None
                ),
                "weighted_steady_visible_fps": (
                    weighted_steady_visible_fps
                ),
                "slow_steady_window_count": len(
                    slow_steady_windows
                ),
                "allowed_slow_steady_window_count": (
                    allowed_slow_steady_windows
                ),
                "slow_steady_frames": slow_steady_frames,
                "steady_burst_interval_total": sum(
                    int(record["burst"])
                    for record in steady_gameplay_windows
                ),
                "steady_late_interval_total": sum(
                    int(record["late"])
                    for record in steady_gameplay_windows
                ),
                "gameplay_visible_tick_total": gameplay_visible_ticks,
                "frontend_visible_tick_total": frontend_visible_ticks,
                "all_burst_interval_total": sum(
                    int(record["burst"]) for record in records
                ),
                "windows": records,
            }

        self.report["pacing_present_evidence"] = evidence
        if after_shutdown:
            self.event(
                "pacing-present-logs-verified",
                profile=self.args.network_profile,
                roles=evidence,
            )

    def validate_pacing_ledger_logs(self, after_shutdown: bool) -> None:
        if not self.args.full_match_rematch or not after_shutdown:
            return

        integer_fields = (
            "commit_grants",
            "no_commit_fallback_grants",
            "spectator_batch_fallback_grants",
            "duplicate_commit_callbacks",
            "duplicate_publish_callbacks",
            "hidden_commit_bypasses",
            "unowned_commit_callbacks",
            "total_grants",
            "wait_callbacks",
            "publish_callbacks",
            "publish_failures",
            "clock_faults",
        )
        evidence: dict[str, object] = {}
        for role in self.runtime_roles():
            name = role.name
            text = role.dll_text()
            tagged_lines = [
                line
                for line in text.splitlines()
                if "[pacing-ledger]" in line
            ]
            matches = list(PACING_LEDGER_RE.finditer(text))
            failure_matches = list(
                PACING_PUBLISH_FAILURES_RE.finditer(text)
            )
            if len(matches) != len(tagged_lines):
                parsed_lines = {match.group(0) for match in matches}
                unmatched = [
                    line
                    for line in tagged_lines
                    if line not in parsed_lines
                ]
                raise HarnessFailure(
                    f"{name} has malformed pacing-ledger telemetry: "
                    f"tagged={len(tagged_lines)} parsed={len(matches)} "
                    f"unmatched={unmatched[:3]}"
                )
            if not matches:
                raise HarnessFailure(
                    f"{name} lacks a final pacing-ledger summary"
                )
            if len(failure_matches) != len(matches):
                raise HarnessFailure(
                    f"{name} pacing publish-failure summaries do not match "
                    f"ledger summaries: ledgers={len(matches)} "
                    f"failureSummaries={len(failure_matches)}"
                )

            records: list[dict[str, object]] = []
            for match in matches:
                record: dict[str, object] = {
                    "clock": match.group("clock"),
                }
                record.update(
                    {
                        field: int(match.group(field))
                        for field in integer_fields
                    }
                )
                records.append(record)

            final = records[-1]
            categorized_grants = (
                int(final["commit_grants"])
                + int(final["no_commit_fallback_grants"])
                + int(final["spectator_batch_fallback_grants"])
            )
            if int(final["total_grants"]) != categorized_grants:
                raise HarnessFailure(
                    f"{name} final pacing ledger does not balance: "
                    f"totalGrants={final['total_grants']} "
                    f"categorizedGrants={categorized_grants} "
                    f"ledger={final}"
                )
            if int(final["commit_grants"]) <= 0:
                raise HarnessFailure(
                    f"{name} final pacing ledger has no commit grant: "
                    f"{final}"
                )
            if int(final["total_grants"]) <= 0:
                raise HarnessFailure(
                    f"{name} final pacing ledger has no pacing grant: "
                    f"{final}"
                )
            if int(final["wait_callbacks"]) <= 0:
                raise HarnessFailure(
                    f"{name} final pacing ledger proves no wrapper wait "
                    f"callback: {final}"
                )
            expected_wait_callbacks = (
                int(final["commit_grants"])
                + int(final["duplicate_commit_callbacks"])
                + int(final["hidden_commit_bypasses"])
                + int(final["unowned_commit_callbacks"])
            )
            if int(final["wait_callbacks"]) != expected_wait_callbacks:
                raise HarnessFailure(
                    f"{name} final pacing callback ledger does not "
                    "balance: "
                    f"waitCallbacks={final['wait_callbacks']} "
                    f"categorizedCallbacks={expected_wait_callbacks} "
                    f"ledger={final}"
                )
            if int(final["duplicate_commit_callbacks"]) != 0:
                raise HarnessFailure(
                    f"{name} final pacing ledger reports duplicate "
                    f"canonical commit callbacks: {final}"
                )
            if int(final["unowned_commit_callbacks"]) != 0:
                raise HarnessFailure(
                    f"{name} final pacing ledger reports canonical "
                    f"callbacks outside owned ticks: {final}"
                )
            if int(final["publish_callbacks"]) <= 0:
                raise HarnessFailure(
                    f"{name} final pacing ledger proves no wrapper publish "
                    f"callback: {final}"
                )
            if (
                int(final["publish_callbacks"])
                + int(final["publish_failures"])
                != int(final["commit_grants"])
            ):
                raise HarnessFailure(
                    f"{name} final pacing ledger does not have exactly one "
                    f"renderer outcome per commit grant: {final}"
                )
            if int(final["duplicate_publish_callbacks"]) != 0:
                raise HarnessFailure(
                    f"{name} final pacing ledger reports duplicate publish "
                    f"callbacks: {final}"
                )
            if int(final["clock_faults"]) != 0:
                raise HarnessFailure(
                    f"{name} final pacing ledger reports raw-clock faults: "
                    f"{final}"
                )
            failure_match = failure_matches[-1]
            failure_reasons = {
                field: int(failure_match.group(field))
                for field in (
                    "invalid",
                    "no_target",
                    "no_source",
                    "empty_client",
                    "get_dc",
                    "gdi",
                    "disposition",
                )
            }
            if (
                sum(failure_reasons.values())
                != int(final["publish_failures"])
            ):
                raise HarnessFailure(
                    f"{name} final publish-failure reasons do not balance: "
                    f"ledger={final} reasons={failure_reasons}"
                )
            unexpected_failure_reasons = {
                field: value
                for field, value in failure_reasons.items()
                if field != "empty_client" and value != 0
            }
            if unexpected_failure_reasons:
                raise HarnessFailure(
                    f"{name} final pacing ledger reports a renderer failure "
                    "other than a temporarily empty/minimized client area: "
                    f"{unexpected_failure_reasons}"
                )

            evidence[name] = {
                "summary_count": len(records),
                "final": final,
                "categorized_grants": categorized_grants,
                "publish_failure_reasons": failure_reasons,
            }

        self.report["pacing_ledger_evidence"] = evidence
        self.event(
            "pacing-ledger-logs-verified",
            roles=evidence,
        )

    def validate_pacing_world_logs(self, after_shutdown: bool) -> None:
        if self.args.returned_charselect_idle_ticks == 0:
            return

        integer_fields = (
            "frame",
            "elapsed_us",
            "visible",
            "gameplay",
            "native_advance",
            "native_miss",
            "qoh_fps",
            "qoh_counter",
            "interval_min",
            "interval_p95",
            "interval_max",
            "burst",
            "late",
            "driver_calls",
            "stalls",
            "exact_waits",
            "deadline_waits",
            "spacing_waits",
            "deadline_rebases",
            "pace_debt_us",
            "rollbacks",
            "replay_frames",
            "correction_batches",
            "correction_work_us",
            "scene",
            "match_sub",
            "result",
            "round",
            "finish_timer_p1",
            "finish_timer_p2",
            "p1_x",
            "p1_y",
            "p1_action",
            "p1_anim",
            "p1_anim_frame",
            "p1_left",
            "p1_life",
            "p2_x",
            "p2_y",
            "p2_action",
            "p2_anim",
            "p2_anim_frame",
            "p2_left",
            "p2_life",
            "prediction_depth",
            "max_rollback",
            "cadence_debt",
            "hidden_forward_ticks",
            "catchup_batches",
            "peer_adv_raw",
            "peer_adv_est",
            "peer_fresh",
            "frame_advantage_holds",
        )
        minimum_world_fps = max(
            45.0, float(self.args.minimum_canonical_fps)
        )
        evidence: dict[str, object] = {}
        for name, role in self.roles.items():
            text = role.dll_text()
            tagged_lines = [
                line for line in text.splitlines()
                if "[pacing-world]" in line
            ]
            matches = list(PACING_WORLD_RE.finditer(text))
            if len(matches) != len(tagged_lines):
                parsed_lines = {match.group(0) for match in matches}
                unmatched = [
                    line for line in tagged_lines
                    if line not in parsed_lines
                ]
                raise HarnessFailure(
                    f"{name} has malformed pacing-world telemetry: "
                    f"tagged={len(tagged_lines)} parsed={len(matches)} "
                    f"unmatched={unmatched[:3]}"
                )
            records: list[dict[str, object]] = []
            for match in matches:
                record: dict[str, object] = {
                    "clock": match.group("clock"),
                    "visible_fps": float(match.group("visible_fps")),
                    "native_fps": float(match.group("native_fps")),
                }
                record.update(
                    {
                        field: int(match.group(field))
                        for field in integer_fields
                    }
                )
                records.append(record)
            if len(records) < 5:
                raise HarnessFailure(
                    f"{name} emitted only {len(records)} pacing-world "
                    "windows during the full-match regression"
                )
            frames = [int(record["frame"]) for record in records]
            if any(
                current <= previous
                for previous, current in zip(frames, frames[1:])
            ):
                raise HarnessFailure(
                    f"{name} pacing-world frames are not strictly "
                    f"increasing: {frames}"
                )
            self._annotate_pacing_window_ranges(records)
            for record in records:
                if int(record["prediction_depth"]) > int(record["max_rollback"]):
                    raise HarnessFailure(
                        f"{name} prediction depth "
                        f"{record['prediction_depth']} exceeded the "
                        f"authorized max rollback {record['max_rollback']} "
                        f"at frame {record['frame']}"
                    )
                if int(record["cadence_debt"]) > 8:
                    raise HarnessFailure(
                        f"{name} cadence-debt ledger {record['cadence_debt']} "
                        f"exceeded its bound at frame {record['frame']}"
                    )

            gameplay_entry_frames = self._gameplay_entry_frames(text)
            complete_gameplay = [
                record for record in records
                if int(record["visible"]) >= 45
                and int(record["gameplay"]) == int(record["visible"])
                and int(record["scene"]) == 2
                and int(record["match_sub"]) == 0
                and int(record["result"]) == 0
            ]
            entry_gameplay = []
            full_gameplay = []
            for record in complete_gameplay:
                if self._pacing_sample_contains_gameplay_entry(
                    record, gameplay_entry_frames
                ):
                    entry_gameplay.append(record)
                else:
                    full_gameplay.append(record)
            if not entry_gameplay:
                raise HarnessFailure(
                    f"{name} lacks a complete gameplay-entry pacing window: "
                    f"entries={gameplay_entry_frames} "
                    f"completeFrames="
                    f"{[int(record['frame']) for record in complete_gameplay]}"
                )
            if len(full_gameplay) < 5:
                raise HarnessFailure(
                    f"{name} has only {len(full_gameplay)} complete active-"
                    "gameplay pacing windows; expected at least five"
                )

            for record in entry_gameplay:
                frame = int(record["frame"])
                visible_fps = float(record["visible_fps"])
                native_fps = float(record["native_fps"])
                native_advance = int(record["native_advance"])
                native_miss = int(record["native_miss"])
                gameplay = int(record["gameplay"])
                if not 45.0 <= visible_fps <= 65.0:
                    raise HarnessFailure(
                        f"{name} gameplay-entry pacing was unbounded at "
                        f"frame {frame}: visibleFps={visible_fps:.2f}"
                    )
                if not 45.0 <= native_fps <= 65.0:
                    raise HarnessFailure(
                        f"{name} gameplay-entry native world pacing was "
                        f"unbounded at frame {frame}: "
                        f"nativeFps={native_fps:.2f}"
                    )
                if native_advance + native_miss != gameplay:
                    raise HarnessFailure(
                        f"{name} gameplay-entry native accounting is "
                        f"inconsistent at frame {frame}: "
                        f"advance={native_advance} miss={native_miss} "
                        f"gameplay={gameplay}"
                    )
                if native_miss > 1:
                    raise HarnessFailure(
                        f"{name} gameplay-entry native world failed to "
                        f"advance {native_miss} times at frame {frame}"
                    )
            previous_hidden_forward: int | None = None
            world_slow_chain = 0
            world_sustain_chain: list[float] = []
            for record in full_gameplay:
                frame = int(record["frame"])
                visible = int(record["visible"])
                gameplay = int(record["gameplay"])
                native_advance = int(record["native_advance"])
                native_miss = int(record["native_miss"])
                visible_fps = float(record["visible_fps"])
                native_fps = float(record["native_fps"])
                world_fps_low = min(visible_fps, native_fps)
                if not 45.0 <= visible_fps <= 65.0:
                    raise HarnessFailure(
                        f"{name} dispatcher/world cadence left "
                        f"[45.0, 65] fps at frame "
                        f"{frame}: {visible_fps:.2f}"
                    )
                if not 45.0 <= native_fps <= 65.0:
                    raise HarnessFailure(
                        f"{name} native world pacing left "
                        f"[45.0, 65] fps at frame "
                        f"{frame}: {native_fps:.2f}"
                    )
                world_slow_chain = (
                    world_slow_chain + 1
                    if world_fps_low < minimum_world_fps
                    else 0
                )
                if world_slow_chain >= 3:
                    raise HarnessFailure(
                        f"{name} dispatcher/world cadence stayed below "
                        f"{minimum_world_fps:.1f} fps for three consecutive "
                        f"windows ending at frame {frame}"
                    )
                world_sustain_chain.append(world_fps_low)
                if len(world_sustain_chain) >= 5 and all(
                    v < minimum_world_fps + 0.5
                    for v in world_sustain_chain[-5:]
                ):
                    raise HarnessFailure(
                        f"{name} dispatcher/world cadence sustained below "
                        f"{minimum_world_fps + 0.5:.1f} fps for five "
                        f"windows ending at frame {frame}"
                    )
                if native_advance + native_miss != gameplay:
                    raise HarnessFailure(
                        f"{name} pacing-world accounting is inconsistent "
                        f"at frame {frame}: advance={native_advance} "
                        f"miss={native_miss} gameplay={gameplay}"
                    )
                if native_miss > 1:
                    raise HarnessFailure(
                        f"{name} native world failed to advance "
                        f"{native_miss} times at frame {frame}"
                    )
                interval_min = int(record["interval_min"])
                interval_p95 = int(record["interval_p95"])
                interval_max = int(record["interval_max"])
                if not (
                    0 < interval_min <= interval_p95 <= interval_max
                ):
                    raise HarnessFailure(
                        f"{name} dispatcher-return interval telemetry is "
                        "invalid at "
                        f"frame {frame}: {interval_min}/"
                        f"{interval_p95}/{interval_max}"
                    )
                hidden_forward_total = int(record["hidden_forward_ticks"])
                hidden_forward_delta = (
                    hidden_forward_total - previous_hidden_forward
                    if previous_hidden_forward is not None
                    else hidden_forward_total
                )
                previous_hidden_forward = hidden_forward_total
                if int(record["driver_calls"]) < visible - max(
                    0, hidden_forward_delta
                ):
                    raise HarnessFailure(
                        f"{name} pacing driver accounting regressed at "
                        f"frame {frame}: calls={record['driver_calls']} "
                        f"visible={visible} "
                        f"hiddenForwardDelta={hidden_forward_delta}"
                    )
                qoh_fps = int(record["qoh_fps"])
                if qoh_fps != 0 and not 55 <= qoh_fps <= 65:
                    raise HarnessFailure(
                        f"{name} QOH native FPS sample left [55, 65] at "
                        f"frame {frame}: {qoh_fps}"
                    )

            gameplay_total = sum(
                int(record["gameplay"]) for record in full_gameplay
            )
            native_advance_total = sum(
                int(record["native_advance"])
                for record in full_gameplay
            )
            rollback_total = sum(
                int(record["rollbacks"]) for record in records
            )
            replay_total = sum(
                int(record["replay_frames"]) for record in records
            )
            correction_total = sum(
                int(record["correction_batches"]) for record in records
            )
            if (
                native_advance_total < 225
                or frames[-1] - frames[0] < 240
            ):
                raise HarnessFailure(
                    f"{name} lacks aggregate advancing-world evidence: "
                    f"nativeAdvance={native_advance_total} "
                    f"frameSpan={frames[-1] - frames[0]}"
                )
            if (
                rollback_total == 0
                or replay_total == 0
                or correction_total == 0
            ):
                raise HarnessFailure(
                    f"{name} {self.args.network_profile} d2/r8 run did not "
                    "exercise rollback "
                    f"correction: rollbacks={rollback_total} "
                    f"replayFrames={replay_total} "
                    f"correctionBatches={correction_total}"
                )

            evidence[name] = {
                "all_window_count": len(records),
                "gameplay_entry_window_count": len(entry_gameplay),
                "gameplay_entry_frames": [
                    int(record["frame"]) for record in entry_gameplay
                ],
                "full_gameplay_window_count": len(full_gameplay),
                "minimum_world_fps": minimum_world_fps,
                "maximum_world_fps": 65.0,
                "frame_span": frames[-1] - frames[0],
                "full_gameplay_frames": [
                    int(record["frame"]) for record in full_gameplay
                ],
                "visible_fps_range": [
                    min(
                        float(record["visible_fps"])
                        for record in full_gameplay
                    ),
                    max(
                        float(record["visible_fps"])
                        for record in full_gameplay
                    ),
                ],
                "native_fps_range": [
                    min(
                        float(record["native_fps"])
                        for record in full_gameplay
                    ),
                    max(
                        float(record["native_fps"])
                        for record in full_gameplay
                    ),
                ],
                "maximum_dispatch_return_interval_p95_us": max(
                    int(record["interval_p95"])
                    for record in full_gameplay
                ),
                "dispatcher_return_late_interval_total": sum(
                    int(record["late"]) for record in full_gameplay
                ),
                "dispatcher_return_burst_interval_total": sum(
                    int(record["burst"]) for record in full_gameplay
                ),
                "visible_tick_total": sum(
                    int(record["visible"]) for record in records
                ),
                "full_gameplay_tick_total": gameplay_total,
                "native_advance_total": native_advance_total,
                "rollback_total": rollback_total,
                "replay_frame_total": replay_total,
                "correction_batch_total": correction_total,
                "correction_work_us": sum(
                    int(record["correction_work_us"])
                    for record in records
                ),
                "spacing_wait_total": sum(
                    int(record["spacing_waits"])
                    for record in records
                ),
                "deadline_rebase_total": sum(
                    int(record["deadline_rebases"])
                    for record in records
                ),
                "maximum_pace_debt_us": max(
                    int(record["pace_debt_us"])
                    for record in records
                ),
            }

        self.report["pacing_world_evidence"] = evidence
        if after_shutdown:
            self.event(
                "pacing-world-logs-verified",
                profile=self.args.network_profile,
                roles=evidence,
            )

    @staticmethod
    def _presentation_counts(
        match: re.Match[str],
    ) -> dict[str, int | str]:
        counts: dict[str, int | str] = {
            "clock": match.group("clock"),
            "normal": int(match.group("normal")),
            "suppressed": int(match.group("suppressed")),
            "corrected": int(match.group("corrected")),
        }
        if "queries" in match.groupdict():
            counts["queries"] = int(match.group("queries"))
        return counts

    @staticmethod
    def _presentation_gate_counts(
        match: re.Match[str],
    ) -> dict[str, int | str]:
        return {
            **LiveGameHarness._presentation_counts(match),
            "frame": int(match.group("frame")),
            "query_before": int(match.group("query_before")),
            "query_after": int(match.group("query_after")),
            "verified": int(match.group("verified")),
        }

    @staticmethod
    def _presentation_summary_counts(
        match: re.Match[str],
    ) -> dict[str, int | str]:
        counts: dict[str, int | str] = {
            **LiveGameHarness._presentation_counts(match),
            "hidden": int(match.group("hidden")),
            "corrected_native": int(match.group("corrected_native")),
            "verified": int(match.group("verified")),
            "missing": int(match.group("missing")),
        }
        counts["hidden_forward"] = (
            int(match.group("hidden_forward"))
            if "hidden_forward" in match.re.groupindex
            else 0
        )
        return counts

    def validate_wrapper_presentation_logs(
        self, after_shutdown: bool
    ) -> None:
        if (
            self.args.returned_charselect_idle_ticks == 0
            or not after_shutdown
        ):
            return
        pacing = self.report.get("pacing_world_evidence")
        if not isinstance(pacing, dict):
            raise HarnessFailure(
                "wrapper presentation validation lacks pacing-world evidence"
            )

        evidence: dict[str, object] = {}
        for role in self.runtime_roles():
            text = role.dll_text()
            if not text:
                raise HarnessFailure(
                    f"{role.name} produced no DLL log for wrapper "
                    "presentation validation"
                )

            wrapper_matches = list(PRESENTATION_WRAPPER_RE.finditer(text))
            gate_matches = list(PRESENTATION_GATE_RE.finditer(text))
            gate_nothing_matches = list(
                PRESENTATION_GATE_NOTHING_RE.finditer(text)
            )
            graceful_summary_matches = list(
                PRESENTATION_GRACEFUL_SUMMARY_RE.finditer(text)
            )
            legacy_summary_matches = list(
                PRESENTATION_SESSION_SUMMARY_RE.finditer(text)
            )
            tagged_specs = (
                (
                    "[presentation-wrapper]",
                    wrapper_matches,
                    "wrapper counter",
                ),
                (
                    "[presentation-gate]",
                    gate_matches + gate_nothing_matches,
                    "hidden-tick gate",
                ),
            )
            for tag, matches, description in tagged_specs:
                tagged = [
                    line for line in text.splitlines()
                    if tag in line
                ]
                parsed_lines = {match.group(0) for match in matches}
                unmatched = [
                    line for line in tagged
                    if line not in parsed_lines
                ]
                if len(matches) != len(tagged) or unmatched:
                    raise HarnessFailure(
                        f"{role.name} has malformed {description} DLL "
                        f"telemetry: tagged={len(tagged)} "
                        f"parsed={len(matches)} "
                        f"unmatched={unmatched[:3]}"
                    )

            graceful_summary_tagged = [
                line for line in text.splitlines()
                if "[presentation-summary]" in line
            ]
            parsed_graceful_summary_lines = {
                match.group(0) for match in graceful_summary_matches
            }
            unmatched_graceful_summaries = [
                line for line in graceful_summary_tagged
                if line not in parsed_graceful_summary_lines
            ]
            if (
                len(graceful_summary_matches)
                != len(graceful_summary_tagged)
                or unmatched_graceful_summaries
            ):
                raise HarnessFailure(
                    f"{role.name} has malformed graceful presentation "
                    "summary: "
                    f"tagged={len(graceful_summary_tagged)} "
                    f"parsed={len(graceful_summary_matches)} "
                    f"unmatched={unmatched_graceful_summaries[:3]}"
                )
            if len(graceful_summary_matches) > 1:
                raise HarnessFailure(
                    f"{role.name} has {len(graceful_summary_matches)} "
                    "graceful presentation summaries, expected at most one"
                )

            legacy_summary_tagged = [
                line for line in text.splitlines()
                if (
                    "[hooks] removed" in line
                    and "hiddenReplayNativeTicks=" in line
                )
            ]
            parsed_legacy_summary_lines = {
                match.group(0) for match in legacy_summary_matches
            }
            unmatched_legacy_summaries = [
                line for line in legacy_summary_tagged
                if line not in parsed_legacy_summary_lines
            ]
            if (
                len(legacy_summary_matches)
                != len(legacy_summary_tagged)
                or unmatched_legacy_summaries
            ):
                raise HarnessFailure(
                    f"{role.name} has malformed optional legacy "
                    "presentation shutdown summary: "
                    f"tagged={len(legacy_summary_tagged)} "
                    f"parsed={len(legacy_summary_matches)} "
                    f"unmatched={unmatched_legacy_summaries[:3]}"
                )
            if len(legacy_summary_matches) > 1:
                raise HarnessFailure(
                    f"{role.name} has {len(legacy_summary_matches)} legacy "
                    "presentation shutdown summaries, expected at most one"
                )
            is_player = role.name in self.roles
            if is_player and len(graceful_summary_matches) != 1:
                raise HarnessFailure(
                    f"{role.name} has {len(graceful_summary_matches)} "
                    "graceful presentation summaries after clean shutdown, "
                    "expected exactly one"
                )
            if not wrapper_matches:
                raise HarnessFailure(
                    f"{role.name} built wrapper never reported presentation "
                    "counters through the DLL"
                )

            wrapper_samples = [
                self._presentation_counts(match)
                for match in wrapper_matches
            ]
            for previous, current in zip(
                wrapper_samples, wrapper_samples[1:]
            ):
                for field in (
                    "normal",
                    "suppressed",
                    "corrected",
                    "queries",
                ):
                    if int(current[field]) < int(previous[field]):
                        raise HarnessFailure(
                            f"{role.name} wrapper {field} presentation "
                            f"counter decreased: {previous} -> {current}"
                        )

            gate_samples = [
                self._presentation_gate_counts(match)
                for match in gate_matches
            ]
            for sample in gate_samples:
                if int(sample["query_after"]) <= int(
                    sample["query_before"]
                ):
                    raise HarnessFailure(
                        f"{role.name} hidden presentation tick did not "
                        f"advance the wrapper query: {sample}"
                    )
            for previous, current in zip(
                gate_samples, gate_samples[1:]
            ):
                if int(current["verified"]) <= int(previous["verified"]):
                    raise HarnessFailure(
                        f"{role.name} verified hidden-tick counter did not "
                        f"increase: {previous} -> {current}"
                    )
                if int(current["query_before"]) < int(
                    previous["query_after"]
                ):
                    raise HarnessFailure(
                        f"{role.name} presentation queries moved backwards: "
                        f"{previous} -> {current}"
                    )
                for field in ("normal", "suppressed", "corrected"):
                    if int(current[field]) < int(previous[field]):
                        raise HarnessFailure(
                            f"{role.name} gate snapshot {field} counter "
                            f"decreased: {previous} -> {current}"
                        )

            summary = (
                self._presentation_summary_counts(
                    graceful_summary_matches[0]
                )
                if graceful_summary_matches
                else (
                    self._presentation_summary_counts(
                        legacy_summary_matches[0]
                    )
                    if legacy_summary_matches
                    else None
                )
            )
            summary_source = (
                "graceful"
                if graceful_summary_matches
                else ("legacy-remove-hooks" if legacy_summary_matches else None)
            )
            last_wrapper = wrapper_samples[-1]
            presentation_counts = {
                "clock": last_wrapper["clock"],
                "normal": max(
                    [int(sample["normal"]) for sample in wrapper_samples]
                    + [int(sample["normal"]) for sample in gate_samples]
                ),
                "suppressed": max(
                    [
                        int(sample["suppressed"])
                        for sample in wrapper_samples
                    ]
                    + [
                        int(sample["suppressed"])
                        for sample in gate_samples
                    ]
                ),
                "corrected": max(
                    [
                        int(sample["corrected"])
                        for sample in wrapper_samples
                    ]
                    + [
                        int(sample["corrected"])
                        for sample in gate_samples
                    ]
                ),
                "queries": max(
                    [int(sample["queries"]) for sample in wrapper_samples]
                    + [
                        int(sample["query_after"])
                        for sample in gate_samples
                    ]
                ),
            }
            missing_ticks = (
                int(summary["missing"]) if summary is not None else 0
            )
            summary_lag: dict[str, int] | None = None
            if summary is not None:
                if (
                    int(summary["hidden"]) + int(summary["hidden_forward"])
                    != int(summary["verified"]) + int(summary["missing"])
                ):
                    raise HarnessFailure(
                        f"{role.name} did not account for every hidden native "
                        f"tick: {summary}"
                    )
                allowed_missing = max(
                    8,
                    (int(summary["hidden"]) + int(summary["hidden_forward"]))
                    // 20,
                )
                if missing_ticks > 0 and int(summary["verified"]) == 0:
                    raise HarnessFailure(
                        f"{role.name} never proved the presentation gate yet "
                        f"reported non-rendering hidden ticks: {summary}"
                    )
                if missing_ticks > allowed_missing:
                    raise HarnessFailure(
                        f"{role.name} observed too many missing "
                        "presentation-gate ticks: "
                        f"missing={missing_ticks} allowed={allowed_missing} "
                        f"{summary}"
                    )
                if (
                    int(summary["corrected_native"]) != 0
                    or int(summary["corrected"]) != 0
                ):
                    raise HarnessFailure(
                        f"{role.name} exposed a corrected-final replay "
                        "present instead of suppressing the complete "
                        f"historical correction batch: {summary}"
                    )
                for field in ("normal", "suppressed", "corrected"):
                    if int(summary[field]) < int(
                        presentation_counts[field]
                    ):
                        raise HarnessFailure(
                            f"{role.name} final {field} counter precedes "
                            "an earlier DLL presentation snapshot: "
                            f"periodic={presentation_counts} "
                            f"summary={summary}"
                        )
                if int(summary["queries"]) < int(
                    presentation_counts["queries"]
                ):
                    raise HarnessFailure(
                        f"{role.name} final query counter precedes an "
                        "earlier DLL presentation snapshot: "
                        f"periodic={presentation_counts} summary={summary}"
                    )
                summary_lag = {}
                for field in (
                    "normal",
                    "suppressed",
                    "corrected",
                    "queries",
                ):
                    delta = int(summary[field]) - int(
                        last_wrapper[field]
                    )
                    if delta < 0:
                        raise HarnessFailure(
                            f"{role.name} final {field} counter precedes its "
                            f"last periodic value: periodic={last_wrapper} "
                            f"summary={summary}"
                        )
                    summary_lag[field] = delta
                counter_lag = (
                    int(summary["normal"])
                    + int(summary["suppressed"])
                    + int(summary["corrected"])
                    - int(last_wrapper["normal"])
                    - int(last_wrapper["suppressed"])
                    - int(last_wrapper["corrected"])
                )
                if counter_lag > 299:
                    raise HarnessFailure(
                        f"{role.name} periodic wrapper sample lagged its "
                        f"final total by {counter_lag}, expected at most 299: "
                        f"periodic={last_wrapper} summary={summary}"
                    )
                if gate_samples:
                    verified_lag = int(summary["hidden"]) - int(
                        gate_samples[-1]["verified"]
                    )
                    if not 0 <= verified_lag <= 299:
                        raise HarnessFailure(
                            f"{role.name} periodic hidden-tick sample lagged "
                            f"final by {verified_lag}, expected [0, 299]: "
                            f"gate={gate_samples[-1]} summary={summary}"
                        )
                presentation_counts = dict(summary)

            role_pacing = pacing.get(role.name)
            replay_frames = 0
            correction_batches = 0
            if isinstance(role_pacing, dict):
                replay_frames = int(
                    role_pacing["replay_frame_total"]
                )
                correction_batches = int(
                    role_pacing["correction_batch_total"]
                )
            logged_historical_ticks = replay_frames
            hidden_ticks = (
                int(summary["hidden"])
                if summary is not None
                else (
                    int(gate_samples[-1]["verified"])
                    if gate_samples
                    else 0
                )
            )
            hidden_expected = (
                hidden_ticks > 0 or logged_historical_ticks > 0
            )
            observed_queries = max(
                [int(sample["queries"]) for sample in wrapper_samples]
                + [
                    int(sample["query_after"])
                    for sample in gate_samples
                ]
                + (
                    [int(summary["queries"])]
                    if summary is not None
                    else []
                )
            )
            observed_suppressed = max(
                [int(sample["suppressed"]) for sample in wrapper_samples]
                + [
                    int(sample["suppressed"])
                    for sample in gate_samples
                ]
                + (
                    [int(summary["suppressed"])]
                    if summary is not None
                    else []
                )
            )
            if int(presentation_counts["normal"]) == 0:
                raise HarnessFailure(
                    f"{role.name} wrapper reported no normal visible "
                    "presentation"
                )
            if hidden_expected:
                if not gate_samples:
                    raise HarnessFailure(
                        f"{role.name} exercised historical native replay "
                        "without any verified presentation-gate sample: "
                        f"replayFrames={replay_frames} "
                        f"correctionBatches={correction_batches} "
                        f"summary={summary}"
                    )
                if observed_queries == 0:
                    raise HarnessFailure(
                        f"{role.name} hid replay without resolving/using the "
                        "wrapper presentation disposition"
                    )
                if observed_suppressed == 0:
                    raise HarnessFailure(
                        f"{role.name} verified hidden replay ticks but the "
                        "wrapper suppressed no presents"
                    )
                if (
                    "compatible ddraw presentation gate not active" in text
                ):
                    raise HarnessFailure(
                        f"{role.name} reported an inactive production "
                        "presentation gate"
                    )

            if isinstance(role_pacing, dict):
                visible_ticks = int(
                    role_pacing["visible_tick_total"]
                )
                visible_presents = (
                    int(presentation_counts["normal"])
                    + int(presentation_counts["corrected"])
                )
                periodic_allowance = 0 if summary is not None else 299
                if visible_presents + periodic_allowance < visible_ticks:
                    raise HarnessFailure(
                        f"{role.name} wrapper exposed fewer visible frames "
                        f"than the pacing driver: presents={visible_presents} "
                        f"visibleTicks={visible_ticks} "
                        f"periodicAllowance={periodic_allowance}"
                    )
                replay_presents = (
                    int(presentation_counts["suppressed"])
                    + int(presentation_counts["corrected"])
                )
                if (
                    replay_frames > 0
                    and (
                        observed_suppressed == 0
                        or replay_presents + periodic_allowance
                        + missing_ticks < replay_frames
                    )
                ):
                    raise HarnessFailure(
                        f"{role.name} wrapper did not suppress every "
                        "historical replay presentation: "
                        f"presentation={presentation_counts} "
                        f"loggedReplayFrames={replay_frames} "
                        f"nonRenderingHiddenTicks={missing_ticks} "
                        f"periodicAllowance={periodic_allowance}"
                    )

            evidence[role.name] = {
                "wrapper_sample_count": len(wrapper_samples),
                "gate_sample_count": len(gate_samples),
                "last_wrapper_sample": last_wrapper,
                "last_gate_sample": (
                    gate_samples[-1] if gate_samples else None
                ),
                "shutdown_summary": summary,
                "shutdown_summary_present": summary is not None,
                "shutdown_summary_source": summary_source,
                "periodic_to_final_lag": summary_lag,
                "logged_replay_frames": replay_frames,
                "logged_correction_batches": correction_batches,
                "logged_historical_ticks": logged_historical_ticks,
                "hidden_replay_observed": hidden_expected,
                "observed_disposition_queries": observed_queries,
                "observed_suppressed_presents": observed_suppressed,
            }

        self.report["wrapper_presentation_evidence"] = evidence
        self.event(
            "wrapper-presentation-logs-verified",
            profile=self.args.network_profile,
            roles=evidence,
        )

    def validate_returned_charselect_dwell_logs(
        self, after_shutdown: bool
    ) -> None:
        required_ticks = self.args.returned_charselect_idle_ticks
        if required_ticks == 0:
            return
        live_evidence = self.report.get(
            "returned_charselect_idle_dwell"
        )
        if not isinstance(live_evidence, dict):
            raise HarnessFailure(
                "returned-character-select live dwell evidence is absent"
            )
        live_roles = live_evidence.get("roles")
        if not isinstance(live_roles, dict):
            raise HarnessFailure(
                "returned-character-select role evidence is absent"
            )

        log_evidence: dict[str, object] = {}
        for name, role in self.roles.items():
            role_live = live_roles.get(name)
            if not isinstance(role_live, dict):
                raise HarnessFailure(
                    f"{name} returned-character-select live evidence is absent"
                )
            records = [
                {
                    "clock": match.group("clock"),
                    "clock_ms": self._log_clock_milliseconds(
                        match.group("clock")
                    ),
                    **{
                        field: int(match.group(field))
                        for field in (
                            "frame",
                            "result",
                            "elapsed",
                            "scene_before",
                            "scene_after",
                            "next_before",
                            "next_after",
                            "match_before",
                            "match_after",
                            "scene3_before",
                            "scene3_after",
                        )
                    },
                    "line": match.group(0),
                }
                for match in LIFECYCLE_DISPATCH_RETURN_RE.finditer(
                    role.dll_text()
                )
            ]
            dwell_start = int(role_live["start_canonical_frame"])
            dwell_end = int(role_live["end_canonical_frame"])
            charselect_entries = [
                record
                for record in records
                if record["scene_before"] == 3
                and record["scene_after"] == 3
                and record["scene3_before"] != 2
                and record["scene3_after"] == 2
                and record["frame"] <= dwell_start
            ]
            if not charselect_entries:
                raise HarnessFailure(
                    f"{name} log lacks native returned-character-select entry "
                    f"at or before canonical frame {dwell_start}"
                )
            entry = max(
                charselect_entries, key=lambda record: record["frame"]
            )
            charselect_exits = [
                record
                for record in records
                if record["scene_before"] == 3
                and record["scene_after"] == 3
                and record["next_before"] == 3
                and record["next_after"] == 2
                and record["frame"] >= dwell_end
            ]
            if not charselect_exits:
                raise HarnessFailure(
                    f"{name} log lacks native character-select exit after "
                    f"dwell frame {dwell_end}"
                )
            charselect_exit = min(
                charselect_exits, key=lambda record: record["frame"]
            )
            gameplay_entries = [
                record
                for record in records
                if record["scene_before"] == 2
                and record["scene_after"] == 2
                and record["match_before"] == -1
                and record["match_after"] == 0
                and record["frame"] >= charselect_exit["frame"]
            ]
            if not gameplay_entries:
                raise HarnessFailure(
                    f"{name} log lacks native rematch world-entry return "
                    f"after frame {charselect_exit['frame']}"
                )
            gameplay_entry = min(
                gameplay_entries, key=lambda record: record["frame"]
            )

            charselect_frames = (
                int(charselect_exit["frame"]) - int(entry["frame"])
            )
            if charselect_frames < required_ticks:
                raise HarnessFailure(
                    f"{name} native lifecycle log proves only "
                    f"{charselect_frames} character-select frames, fewer "
                    f"than requested dwell {required_ticks}"
                )
            clock_delta_ms = (
                int(charselect_exit["clock_ms"])
                - int(entry["clock_ms"])
            )
            if clock_delta_ms < 0:
                clock_delta_ms += 24 * 3_600_000
            if clock_delta_ms <= 0:
                raise HarnessFailure(
                    f"{name} native lifecycle log has a nonpositive "
                    "character-select clock interval"
                )
            freeze_ms = int(
                getattr(self, "_harness_freeze_seconds", 0.0) * 1000.0
            )
            effective_ms = max(1, clock_delta_ms - freeze_ms)
            logged_fps = charselect_frames * 1_000.0 / effective_ms
            minimum_fps = float(
                live_evidence["minimum_canonical_fps"]
            )
            if not minimum_fps <= logged_fps <= 70.0:
                raise HarnessFailure(
                    f"{name} log-derived native/presentation pacing was "
                    f"outside [{minimum_fps:.1f}, 70.0] fps after excluding "
                    f"{freeze_ms} ms of harness pause freezes: "
                    f"{logged_fps:.3f}"
                )
            slow_native_returns = [
                record
                for record in (entry, charselect_exit, gameplay_entry)
                if int(record["elapsed"]) >= 2_000
            ]
            if slow_native_returns:
                raise HarnessFailure(
                    f"{name} native lifecycle dispatch blocked for at least "
                    f"2 seconds: {slow_native_returns}"
                )
            log_evidence[name] = {
                "returned_charselect_entry": entry,
                "charselect_exit": charselect_exit,
                "rematch_world_entry": gameplay_entry,
                "charselect_frame_delta": charselect_frames,
                "charselect_clock_milliseconds": clock_delta_ms,
                "native_presentation_fps": round(logged_fps, 3),
                "maximum_native_dispatch_elapsed_ms": max(
                    int(record["elapsed"])
                    for record in (
                        entry,
                        charselect_exit,
                        gameplay_entry,
                    )
                ),
            }

        self.report["returned_charselect_idle_log_evidence"] = (
            log_evidence
        )
        if after_shutdown:
            self.event(
                "returned-charselect-idle-logs-verified",
                profile=self.args.network_profile,
                roles=log_evidence,
            )

    def validate_full_match_logs(self, after_shutdown: bool) -> None:
        if not self.args.full_match_rematch:
            return

        evidence: dict[str, object] = {}
        player_layouts: dict[
            str, dict[int, dict[str, object]]
        ] = {}
        for role in self.runtime_roles():
            text = role.dll_text()
            enabled = [
                line
                for line in text.splitlines()
                if "[test-automatch] ENABLED " in line
            ]
            if len(enabled) != 1:
                raise HarnessFailure(
                    f"{role.name} has {len(enabled)} automatch enable "
                    "markers, expected exactly one"
                )
            expected_force_ko = (
                "forceKoAfter="
                + str(self.args.force_ko_after_frames)
            )
            if expected_force_ko not in enabled[0]:
                raise HarnessFailure(
                    f"{role.name} automatch threshold differs from the "
                    f"requested value: marker={enabled[0]!r} "
                    f"expected={expected_force_ko}"
                )

            lifecycle = list(AUTOMATCH_LIFECYCLE_RE.finditer(text))
            enters = [
                int(match.group("segment"))
                for match in lifecycle
                if match.group("event") == "ENTER"
            ]
            exits = [
                int(match.group("segment"))
                for match in lifecycle
                if match.group("event") == "EXIT"
            ]
            round_begins = [
                (
                    int(match.group("segment")),
                    int(match.group("round")),
                    int(match.group("counter")),
                )
                for match in AUTOMATCH_ROUND_BEGIN_RE.finditer(text)
            ]
            if len(enters) < 2 or len(exits) < 1:
                raise HarnessFailure(
                    f"{role.name} did not execute a complete native set and "
                    f"rematch: segmentEnters={enters} "
                    f"segmentExits={exits}"
                )
            if enters[:2] != [1, 2] or exits[0] != 1:
                raise HarnessFailure(
                    f"{role.name} automatch gameplay segments are not "
                    "contiguous: "
                    f"enters={enters} exits={exits}"
                )
            required_rounds = {(1, 1, 0), (1, 2, 1), (1, 3, 2)}
            if not required_rounds.issubset(set(round_begins)):
                raise HarnessFailure(
                    f"{role.name} did not observe all native in-place "
                    f"rounds: begins={round_begins}"
                )

            input_matches = list(AUTOMATCH_INPUT_RE.finditer(text))
            nonzero_inputs = [
                int(match.group("mask"), 16)
                for match in input_matches
                if int(match.group("mask"), 16) != 0
            ]
            combat_inputs = [
                int(match.group("mask"), 16)
                for match in input_matches
                if " combat=1 " in match.group(0)
                and int(match.group("mask"), 16) != 0
            ]
            if role is self.spectator:
                if input_matches:
                    raise HarnessFailure(
                        "receive-only spectator generated local automatch "
                        f"input: {[match.group(0) for match in input_matches[:4]]}"
                    )
            elif not nonzero_inputs or not combat_inputs:
                raise HarnessFailure(
                    f"{role.name} did not inject nonzero frontend and combat "
                    "inputs through the production indexed-input path"
                )

            exercised_losers = {
                int(value)
                for value in re.findall(
                    r"\[test-automatch\] round=\d+ "
                    r"(?:armed|native combat KO|forced native KO fallback) "
                    r"P([12])\b",
                    text,
                )
            }
            if exercised_losers != {1, 2}:
                raise HarnessFailure(
                    f"{role.name} did not exercise both players as native "
                    "round losers: exercisedLosers="
                    f"{sorted(exercised_losers)}"
                )
            ko_outcomes = [
                {
                    "round": int(match.group("round")),
                    "event": match.group("event"),
                    "player": int(match.group("player")),
                }
                for match in re.finditer(
                    r"\[test-automatch\] round=(?P<round>\d+) "
                    r"(?P<event>native combat KO|"
                    r"forced native KO fallback) "
                    r"P(?P<player>[12])\b",
                    text,
                )
            ]
            games_required = max(2, int(self.args.game_count))
            expected_round_losers = {1: 2, 2: 1, 3: 2}
            for round_number, expected_loser in (
                expected_round_losers.items()
            ):
                outcomes = [
                    outcome for outcome in ko_outcomes
                    if int(outcome["round"]) == round_number
                ]
                if (
                    not 1 <= len(outcomes) <= games_required
                    or any(
                        int(outcome["player"]) != expected_loser
                        for outcome in outcomes
                    )
                ):
                    raise HarnessFailure(
                        f"{role.name} round {round_number} lacks one planned "
                        f"native KO outcome per game for P"
                        f"{expected_loser}: outcomes={outcomes}"
                    )

            epoch_advances = [
                int(value)
                for value in re.findall(
                    r"\[netplay\] rollback layout epoch advanced to (\d+)",
                    text,
                )
            ]
            if not epoch_advances or max(epoch_advances) < 2:
                raise HarnessFailure(
                    f"{role.name} did not rebuild deterministic state across "
                    f"the native set/rematch boundary: "
                    f"epochs={epoch_advances}"
                )

            layouts = [
                {
                    "generation": int(match.group("generation")),
                    "epoch": int(match.group("epoch")),
                    "local_generation": int(
                        match.group("local_generation")
                    ),
                    "digest": match.group("digest").upper(),
                }
                for match in LAYOUT_COMPLETE_RE.finditer(text)
            ]
            if role is not self.spectator:
                layouts_by_epoch: dict[
                    int, list[dict[str, object]]
                ] = {}
                for layout in layouts:
                    layouts_by_epoch.setdefault(
                        int(layout["epoch"]), []
                    ).append(layout)
                unstable_epochs = {
                    epoch: records
                    for epoch, records in layouts_by_epoch.items()
                    if len(
                        {
                            (
                                int(record["generation"]),
                                str(record["digest"]),
                            )
                            for record in records
                        }
                    ) != 1
                }
                if unstable_epochs:
                    raise HarnessFailure(
                        f"{role.name} changed layout identity within an "
                        f"epoch: {unstable_epochs}"
                    )
                missing_epochs = [
                    epoch for epoch in (1, 2)
                    if epoch not in layouts_by_epoch
                ]
                if missing_epochs:
                    raise HarnessFailure(
                        f"{role.name} lacks layout rendezvous for required "
                        f"epochs {missing_epochs}: layouts={layouts}"
                    )
                player_layouts[role.name] = {
                    epoch: layouts_by_epoch[epoch][0]
                    for epoch in (1, 2)
                }
            else:
                rendezvous_count = text.count(
                    "[spectator] gameplay-entry rendezvous complete"
                )
                if rendezvous_count < 2:
                    raise HarnessFailure(
                        "spectator did not independently verify every "
                        "set/rematch gameplay entry: "
                        f"count={rendezvous_count}"
                    )

            evidence[role.name] = {
                "enabled": enabled[0],
                "gameplay_segment_enters": enters,
                "gameplay_segment_exits": exits,
                "round_begins": round_begins,
                "nonzero_input_samples": [
                    f"0x{value:08X}" for value in nonzero_inputs[:12]
                ],
                "combat_input_samples": [
                    f"0x{value:08X}" for value in combat_inputs[:12]
                ],
                "exercised_losers": sorted(exercised_losers),
                "ko_outcomes": ko_outcomes,
                "forced_ko_fallbacks": text.count(
                    "forced native KO fallback"
                ),
                "native_combat_ko_observations": text.count(
                    "native combat KO"
                ),
                "epoch_advances": epoch_advances,
                "layouts": layouts,
            }
            if role is self.spectator:
                evidence[role.name]["gameplay_rendezvous_count"] = (
                    rendezvous_count
                )

        host_layouts = player_layouts.get("host")
        client_layouts = player_layouts.get("client")
        if not host_layouts or not client_layouts:
            raise HarnessFailure(
                "host/client layout rendezvous evidence is incomplete: "
                f"host={host_layouts} client={client_layouts}"
            )
        for epoch in (1, 2):
            host_layout = host_layouts[epoch]
            client_layout = client_layouts[epoch]
            if (
                host_layout["generation"],
                host_layout["epoch"],
                host_layout["digest"],
            ) != (
                client_layout["generation"],
                client_layout["epoch"],
                client_layout["digest"],
            ):
                raise HarnessFailure(
                    f"host/client layout rendezvous proof differs at "
                    f"epoch {epoch}: host={host_layout} "
                    f"client={client_layout}"
                )
        host_layout = host_layouts[2]

        self.report["native_match_log_evidence"] = evidence
        self.validate_pacing_present_logs(after_shutdown)
        self.validate_pacing_ledger_logs(after_shutdown)
        self.validate_pacing_world_logs(after_shutdown)
        self.validate_returned_charselect_dwell_logs(after_shutdown)
        self.validate_wrapper_presentation_logs(after_shutdown)
        if after_shutdown:
            self.event(
                "native-match-rematch-logs-verified",
                rematch_layout=host_layout,
                roles=list(evidence),
            )

    @staticmethod
    def _network_counter_dict(match: re.Match[str]) -> dict[str, int]:
        return {
            field: int(match.group(field))
            for field in (
                "logical",
                "queued",
                "sent",
                "dropped",
                "duplicated",
                "reordered",
                "pending",
                "overflows",
            )
        }

    def validate_network_impairment_logs(
        self, after_shutdown: bool
    ) -> None:
        expected_profile = self.args.network_profile
        role_indexes = {"host": 0, "client": 1, "spectator": 2}
        expected_role_labels = {
            "host": "host",
            "client": "client",
            "spectator": "viewer",
        }
        required_domains = {
            "host": {"player"},
            "client": {"player"},
            "spectator": {"spectator-viewer"},
        }
        for index, spectator in enumerate(self.spectators):
            role_indexes[spectator.name] = 2 + index
            expected_role_labels[spectator.name] = "viewer"
            required_domains[spectator.name] = {"spectator-viewer"}
        evidence: dict[str, object] = {}

        for role in self.runtime_roles():
            text = role.dll_text()
            enabled = list(TEST_NET_ENABLED_RE.finditer(text))
            if expected_profile == "none":
                if enabled:
                    raise HarnessFailure(
                        f"{role.name} unexpectedly enabled packet "
                        f"impairment: {[item.group(0) for item in enabled]}"
                    )
                continue
            if len(enabled) != 1:
                raise HarnessFailure(
                    f"{role.name} has {len(enabled)} parseable packet-profile "
                    "enable markers, expected exactly one"
                )
            marker = enabled[0]
            expected_seed = (
                self.args.network_seed + role_indexes[role.name]
            ) & 0xFFFFFFFF
            if (
                marker.group("profile") != expected_profile
                or int(marker.group("seed"), 16) != expected_seed
                or marker.group("role")
                    != expected_role_labels[role.name]
            ):
                raise HarnessFailure(
                    f"{role.name} packet-profile marker differs from the "
                    f"requested profile/seed/role: {marker.group(0)}"
                )

            role_evidence: dict[str, object] = {
                "enabled": marker.group(0),
                "expected_seed": f"0x{expected_seed:08X}",
            }
            if not after_shutdown:
                evidence[role.name] = role_evidence
                continue

            stats = list(TEST_NET_STATS_RE.finditer(text))
            lanes = list(TEST_NET_LANE_STATS_RE.finditer(text))
            if len(stats) != 1:
                raise HarnessFailure(
                    f"{role.name} has {len(stats)} packet STATS markers "
                    "after shutdown, expected exactly one"
                )
            aggregate = self._network_counter_dict(stats[0])
            aggregate["lanes"] = int(stats[0].group("lanes"))
            if (
                stats[0].group("profile") != expected_profile
                or aggregate["logical"] == 0
                or aggregate["queued"] == 0
                or aggregate["sent"] == 0
                or aggregate["overflows"] != 0
                or aggregate["lanes"] != len(lanes)
            ):
                raise HarnessFailure(
                    f"{role.name} packet impairment was inactive or "
                    "overflowed, or its aggregate lane count disagreed "
                    f"with LANE-STATS: aggregate={stats[0].group(0)} "
                    f"parsedLanes={len(lanes)}"
                )

            lane_evidence: dict[str, object] = {}
            observed_domains = [
                lane.group("domain") for lane in lanes
            ]
            observed_keys = [lane.group("key").upper() for lane in lanes]
            if (
                set(observed_domains) != required_domains[role.name]
                or len(observed_domains) != len(set(observed_domains))
                or len(observed_keys) != len(set(observed_keys))
            ):
                raise HarnessFailure(
                    f"{role.name} packet lane domains/keys are not the "
                    "exact isolated set expected by this harness: "
                    f"expected={sorted(required_domains[role.name])} "
                    f"domains={observed_domains} keys={observed_keys}"
                )
            lane_totals = {
                field: 0
                for field in (
                    "logical",
                    "queued",
                    "sent",
                    "dropped",
                    "duplicated",
                    "reordered",
                    "pending",
                    "overflows",
                )
            }
            for lane in lanes:
                counters = self._network_counter_dict(lane)
                domain = lane.group("domain")
                if (
                    counters["logical"] == 0
                    or counters["queued"] == 0
                    or counters["sent"] == 0
                    or counters["overflows"] != 0
                ):
                    raise HarnessFailure(
                        f"{role.name}/{domain} packet lane was inactive or "
                        f"overflowed: {lane.group(0)}"
                    )
                for field in lane_totals:
                    lane_totals[field] += counters[field]
                lane_evidence[domain] = {
                    "key": lane.group("key").upper(),
                    "lane_seed_final": lane.group("seed").upper(),
                    **counters,
                }
            mismatched_totals = {
                field: {
                    "aggregate": aggregate[field],
                    "lanes": lane_totals[field],
                }
                for field in lane_totals
                if aggregate[field] != lane_totals[field]
            }
            if mismatched_totals:
                raise HarnessFailure(
                    f"{role.name} aggregate impairment counters do not "
                    f"equal the per-lane totals: {mismatched_totals}"
                )

            if expected_profile in (
                "stable-low",
                "stable-120",
                "stable-high",
                "stable-250",
                "stable-300",
                "asym-updown",
                "asym-downup",
            ):
                if any(
                    aggregate[field] != 0
                    for field in ("dropped", "duplicated", "reordered")
                ):
                    raise HarnessFailure(
                        f"{role.name} stable profile unexpectedly changed "
                        f"packet delivery: {aggregate}"
                    )
            elif expected_profile in ("jitter-low", "jitter-high"):
                if (
                    aggregate["duplicated"] == 0
                    or aggregate["reordered"] == 0
                ):
                    raise HarnessFailure(
                        f"{role.name} jitter profile produced no observable "
                        f"duplication/reordering: {aggregate}"
                    )
            elif expected_profile == "packet-loss":
                if (
                    aggregate["dropped"] == 0
                    or aggregate["duplicated"] == 0
                    or aggregate["reordered"] == 0
                ):
                    raise HarnessFailure(
                        f"{role.name} packet-loss profile did not exercise "
                        f"loss/duplication/reordering: {aggregate}"
                    )

            role_evidence["aggregate"] = aggregate
            role_evidence["lanes"] = lane_evidence
            evidence[role.name] = role_evidence

        if expected_profile != "none":
            self.report["network_impairment_evidence"] = evidence
            if after_shutdown:
                self.event(
                    "network-impairment-verified",
                    profile=expected_profile,
                    roles=evidence,
                )

    def validate_spectator_logs(
        self,
        role: Role | None = None,
        *,
        after_shutdown: bool = False,
    ) -> None:
        role = self.spectator if role is None else role
        if role is None:
            if self.distributed_mode and self.args.spectator:
                raise HarnessFailure(
                    "spectator validation was asked to run with no spectator "
                    "role while --spectator was requested; in distributed "
                    "mode this silent skip would hide a viewer that never "
                    "existed"
                )
            return
        spectator_text = role.dll_text()
        missing_viewer = [
            marker
            for marker in SPECTATOR_REQUIRED_DLL_MARKERS
            if marker not in spectator_text
        ]
        if missing_viewer:
            raise HarnessFailure(
                "spectator DLL log lacks required receive-only markers: "
                f"{missing_viewer}"
            )
        launch_evidence = role.stdout_text() + "\n" + role.client_text()
        route_matches = list(
            SPECTATOR_PREFLIGHT_RE.finditer(launch_evidence)
        )
        if not route_matches:
            raise HarnessFailure(
                "spectator caster lacks the production route-preflight "
                "contract"
            )
        route = route_matches[-1]
        route_label = route.group("route")
        requested_route = self.spectator_route_for_role(role)
        expected_route_labels = {
            "auto": {"lan-direct", "loopback"},
            "direct": {"lan-direct", "loopback"},
            "punch": {"punched"},
            "relay": {"relay"},
        }[requested_route]
        if (
            route.group("ok") != "1"
            or route.group("failure") != "0"
            or route.group("inconclusive") != "0"
            or route_label not in expected_route_labels
        ):
            raise HarnessFailure(
                f"{role.name} did not prove the requested "
                f"{requested_route} data route before "
                f"launch: {route.group(0)}"
            )
        forced_route_contract: str | None = None
        if requested_route != "auto":
            force_matches = list(
                SPECTATOR_ROUTE_FORCE_RE.finditer(launch_evidence)
            )
            if not force_matches:
                raise HarnessFailure(
                    f"{role.name} lacks the explicit forced-route contract"
                )
            force = force_matches[-1]
            if force.group("mode") != requested_route:
                raise HarnessFailure(
                    f"{role.name} forced-route contract differs from request: "
                    f"{force.group(0)}"
                )
            direct_race_ms = int(force.group("direct_race_ms"))
            punch_enabled = force.group("punch") == "1"
            relay_enabled = force.group("relay") == "1"
            force_shape_ok = {
                "direct": (
                    direct_race_ms > 0
                    and not punch_enabled
                    and not relay_enabled
                ),
                "punch": (
                    direct_race_ms == 0
                    and punch_enabled
                    and not relay_enabled
                ),
                "relay": (
                    direct_race_ms == 0
                    and not punch_enabled
                    and relay_enabled
                ),
            }[requested_route]
            if not force_shape_ok:
                raise HarnessFailure(
                    f"{role.name} forced-route mode was only a label; its "
                    f"actual route knobs are wrong: {force.group(0)}"
                )
            forced_route_contract = force.group(0)

        socket_matches = list(
            SPECTATOR_PROVED_SOCKET_RE.finditer(launch_evidence)
        )
        if not socket_matches:
            raise HarnessFailure(
                "spectator runtime did not adopt the exact UDP socket proved "
                "by its caster preflight"
            )
        proved_port = int(socket_matches[-1].group("port"))
        if not 1 <= proved_port <= 65535:
            raise HarnessFailure(
                "spectator caster reported an invalid adopted socket port: "
                f"{proved_port}"
            )

        launch_contracts = list(
            CASTER_SPECTATOR_LAUNCH_CONTRACT_RE.finditer(launch_evidence)
        )
        if not launch_contracts:
            raise HarnessFailure(
                "spectator caster did not emit a receive-only launch contract"
            )
        launch_contract = launch_contracts[-1]
        expected_hidden = "0" if self.args.visible_games else "1"
        if (
            int(launch_contract.group("session")) == 0
            or int(launch_contract.group("pid")) <= 0
            or launch_contract.group("hidden_desktop") != expected_hidden
        ):
            raise HarnessFailure(
                "spectator launch contract has invalid live-test fields: "
                f"{launch_contract.group(0)}"
            )
        accesses = self.report.get("spectator_accesses", {})
        access = (
            accesses.get(role.name, {})
            if isinstance(accesses, dict)
            else {}
        )
        expected_session = int(access.get("session", 0))
        expected_pid = role.game_memory.pid if role.game_memory else 0
        if (
            int(launch_contract.group("session")) != expected_session
            or int(launch_contract.group("pid")) != expected_pid
        ):
            raise HarnessFailure(
                "spectator launch contract does not identify the discovered "
                f"viewer: accessSession={expected_session} "
                f"discoveredPid={expected_pid} "
                f"contract={launch_contract.group(0)}"
            )
        client_log = role.client_text()
        preflight_offset = client_log.rfind(route.group(0))
        adoption_offset = client_log.rfind(socket_matches[-1].group(0))
        launch_offset = client_log.rfind(launch_contract.group(0))
        if not (
            0 <= preflight_offset < adoption_offset < launch_offset
        ):
            raise HarnessFailure(
                "spectator route handoff log order is not "
                "preflight -> socket adoption -> game launch: "
                f"offsets={preflight_offset}/{adoption_offset}/"
                f"{launch_offset}"
            )

        logged_secret = re.search(
            r"(?:#k=|[&#]ra=)[0-9A-Fa-f]{32}\b",
            client_log + "\n" + spectator_text,
        )
        if logged_secret:
            raise HarnessFailure(
                "spectator bearer credential leaked into a persistent log"
            )

        launch_report = {
            "session": expected_session,
            "pid": expected_pid,
            "hidden_desktop": expected_hidden == "1",
            "line": launch_contract.group(0),
            "forced_route_contract": forced_route_contract,
        }
        self.report.setdefault(
            "spectator_launch_contracts", {}
        )[role.name] = launch_report
        if role is self.spectator:
            self.report["spectator_launch_contract"] = launch_report
        host = self.roles["host"]
        host_text = host.dll_text()
        missing_source = [
            marker
            for marker in SPECTATOR_SOURCE_REQUIRED_DLL_MARKERS
            if marker not in host_text
        ]
        if missing_source:
            raise HarnessFailure(
                "host DLL log lacks isolated spectator-source markers: "
                f"{missing_source}"
            )
        self.report["spectator_source_markers"] = {
            marker: marker in host_text
            for marker in SPECTATOR_SOURCE_REQUIRED_DLL_MARKERS
        }

        host_client_log = host.client_text()
        host_caster_evidence = host.stdout_text() + "\n" + host_client_log
        if self.args.spectator_topology:
            viewer_client_markers = (
                "[spectator-topology] opt-in bootstrap armed route=",
            )
            viewer_dll_markers = (
                "[spectator-topology] released direct enrollment armed after "
                "QSP InitAck",
                "[spectator-topology] released topology leaf attached; "
                "ordinary E0 viewing continues",
            )
            host_client_markers = (
                "[spectator-topology] released enrollment granted viewer=",
                "[spectator-topology] source authorityReady=1 enrolled=1 "
                "attached=1",
            )
            missing_topology = [
                marker
                for marker in viewer_client_markers
                if marker not in client_log
            ]
            missing_topology.extend(
                marker
                for marker in viewer_dll_markers
                if marker not in spectator_text
            )
            missing_topology.extend(
                marker
                for marker in host_client_markers
                if marker not in host_client_log
            )
            if missing_topology:
                raise HarnessFailure(
                    "released topology direct smoke lacks authenticated "
                    f"attachment evidence: {missing_topology}"
                )
            self.report["released_topology_direct_smoke"] = {
                "viewer_opt_in": True,
                "viewer_parent_commit_attached": True,
                "host_enrollment_granted": True,
                "host_depth_one_attachment": True,
            }
        viewer_counts = [
            {
                "count": int(match.group("count")),
                "capacity": int(match.group("capacity")),
                "line": match.group(0),
            }
            for match in SPECTATOR_VIEWER_COUNT_RE.finditer(
                host_caster_evidence
            )
        ]
        viewer_counts.extend(
            {
                "count": int(match.group("viewers")),
                "capacity": int(match.group("capacity")),
                "line": match.group(0),
            }
            for match in SPECTATOR_RETENTION_LATE_ADMISSION_RE.finditer(
                host_caster_evidence
            )
        )
        expected_viewer_count = len(self.spectators)
        admitted = [
            item
            for item in viewer_counts
            if (
                item["count"] == expected_viewer_count
                and item["capacity"] >= expected_viewer_count
            )
        ]
        if not admitted:
            raise HarnessFailure(
                "host sidecar never reported the required simultaneous "
                f"viewer admission/count transition to {expected_viewer_count}: "
                f"observations={viewer_counts}"
            )

        authenticated_records = [
            {
                "records": int(match.group("records")),
                "line": match.group(0),
            }
            for match in SPECTATOR_AUTHENTICATED_RECORDS_RE.finditer(
                spectator_text
            )
        ]
        if (
            not authenticated_records
            or max(item["records"] for item in authenticated_records) <= 0
        ):
            raise HarnessFailure(
                "spectator accepted no authenticated QSP records despite "
                f"its launch: observations={authenticated_records[-4:]}"
            )
        ingress_report = {
            "maximum_buffered_records": max(
                item["records"] for item in authenticated_records
            ),
            "observations": authenticated_records[-4:],
            "evidence_domain": "QSP receive-only playback archive",
        }
        self.report.setdefault(
            "spectator_authenticated_ingress_by_role", {}
        )[role.name] = ingress_report
        if role is self.spectator:
            self.report["spectator_authenticated_ingress"] = ingress_report

        if (
            not after_shutdown
            and (role.game_memory is None or not role.game_memory.alive())
        ):
            raise HarnessFailure(
                "spectator game exited before the production-caster route "
                "could be validated"
            )
        final_titles = (
            window_titles(role.game_memory.pid)
            if role.game_memory is not None and role.game_memory.alive()
            else []
        )
        if (
            not after_shutdown
            and any("CONNECTING" in title.upper() for title in final_titles)
        ):
            raise HarnessFailure(
                "spectator remained in CONNECTING after successful playback: "
                f"titles={final_titles}"
            )
        reset_count = len(
            re.findall(
                r"\b10054\b",
                launch_evidence + "\n" + spectator_text,
            )
        )
        if reset_count > 1:
            raise HarnessFailure(
                "spectator logged persistent WSAECONNRESET/10054 failures "
                f"after route commit: count={reset_count}"
            )

        player_hashes: dict[
            str, set[tuple[int, int, int, str]]
        ] = {}
        for name, player in self.roles.items():
            matches = list(
                CONFIRMED_HASH_MATCH_RE.finditer(player.dll_text())
            )
            player_hashes[name] = {
                (
                    int(match.group("layout")),
                    int(match.group("epoch")),
                    int(match.group("frame")),
                    match.group("hash").upper(),
                )
                for match in matches
            }
        common_hashes = (
            player_hashes.get("host", set())
            & player_hashes.get("client", set())
        )
        if not common_hashes:
            raise HarnessFailure(
                "players have no identical confirmed hash tuple overlapping "
                f"the spectator session: counts="
                f"{ {name: len(values) for name, values in player_hashes.items()} }"
            )

        if (
            not after_shutdown
            and self.args.spectator_postgame_retain_ms == 0
        ):
            handoffs = list(
                SPECTATOR_RUNTIME_HANDOFF_RE.finditer(spectator_text)
            )
            if not handoffs:
                raise HarnessFailure(
                    f"{role.name} lacks its runtime QSP handoff identity"
                )
            handoff = handoffs[-1]
            viewer_id = handoff.group("viewer").upper()
            source_statuses = list(
                SPECTATOR_E0_SOURCE_STATUS_RE.finditer(host_caster_evidence)
            )
            viewer_statuses = [
                match
                for match in SPECTATOR_E0_VIEWER_STATUS_RE.finditer(
                    host_caster_evidence
                )
                if match.group("viewer") == viewer_id
            ]
            if not source_statuses or not viewer_statuses:
                raise HarnessFailure(
                    "host did not preserve bounded E0 source/viewer cursor "
                    f"evidence for {role.name}: source={len(source_statuses)} "
                    f"viewer={len(viewer_statuses)} id={viewer_id}"
                )
            bootstrap = int(handoff.group("resume"))
            handoff_generation = int(handoff.group("generation"))
            source_confirmed = max(
                int(match.group("confirmed")) for match in source_statuses
            )
            generation_scoped_status = [
                match
                for match in viewer_statuses
                if (
                    int(match.group("generation")) == handoff_generation
                    and int(match.group("bootstrap")) == bootstrap
                )
            ]
            scoped_acknowledgments = [
                int(match.group("ack"))
                for match in generation_scoped_status
            ]
            scoped_bytes_sent = [
                int(match.group("bytes_sent"))
                for match in generation_scoped_status
            ]
            scoped_proof_events = [
                match.group("event") for match in generation_scoped_status
            ]
            if (
                not generation_scoped_status
                or "hello-accepted" not in scoped_proof_events
                or not any(
                    match.group("state") == "streaming"
                    for match in generation_scoped_status
                )
                or source_confirmed <= bootstrap
                or max(scoped_acknowledgments, default=0) <= bootstrap
                or max(scoped_acknowledgments, default=0) > source_confirmed
                or max(scoped_bytes_sent, default=0) <= 0
                or any(
                    match.group("endpoint") == "unavailable"
                    or match.group("provenance") != "source-observed"
                    for match in generation_scoped_status
                )
            ):
                raise HarnessFailure(
                    "E0 source-side QSP evidence did not prove attested "
                    "bootstrap, confirmed progress, acknowledgment progress, "
                    "and per-viewer upload accounting: "
                    f"role={role.name} bootstrap={bootstrap} "
                    f"generation={handoff_generation} "
                    f"confirmed={source_confirmed} "
                    f"scoped_ack={scoped_acknowledgments} "
                    f"scoped_bytes={scoped_bytes_sent} "
                    f"scoped_events={scoped_proof_events}"
                )
            qsp_evidence = {
                "viewer_id": viewer_id,
                "handoff_generation": handoff_generation,
                "handoff_resume_cursor": bootstrap,
                "source_confirmed_cursor_max": source_confirmed,
                "ack_cursor_max": max(scoped_acknowledgments),
                "bytes_sent_max": max(scoped_bytes_sent),
                "qsp_events": scoped_proof_events,
                "latest_status": generation_scoped_status[-1].group(0),
                "latest_source_status": source_statuses[-1].group(0),
                "endpoint_provenance": "source-observed",
            }
            self.report.setdefault(
                "spectator_e0_source_qsp_evidence", {}
            )[role.name] = qsp_evidence

        confirmed_transition_evidence: dict[str, object] | None = None
        if self.args.spectator and not self.args.full_match_rematch:
            mode_marker = (
                "[test-automatch] ENABLED "
                "mode=spectator-confirmed-transition"
            )
            player_input_evidence: dict[str, object] = {}
            for player_name, player in self.roles.items():
                player_text = player.dll_text()
                marker_count = player_text.count(mode_marker)
                if marker_count != 1:
                    raise HarnessFailure(
                        f"{player_name} did not enable the exact spectator "
                        "confirmed-transition driver exactly once: "
                        f"count={marker_count}"
                    )

                parsed_inputs: list[dict[str, object]] = []
                for match in AUTOMATCH_INPUT_RE.finditer(player_text):
                    line_begin = player_text.rfind(
                        "\n", 0, match.start()
                    ) + 1
                    line_end = player_text.find("\n", match.end())
                    if line_end < 0:
                        line_end = len(player_text)
                    line = player_text[line_begin:line_end]
                    parsed_inputs.append(
                        {
                            "frame": int(match.group("frame")),
                            "local_player": int(match.group("player")),
                            "mask": int(match.group("mask"), 16),
                            "scene3": "scene=3" in line,
                            "combat": "combat=1" in line,
                            "line": line,
                        }
                    )
                nonzero_inputs = [
                    item
                    for item in parsed_inputs
                    if int(item["mask"]) != 0
                ]
                expected_player = 1 if player_name == "host" else 2
                invalid_nonzero = [
                    item
                    for item in nonzero_inputs
                    if (
                        item["local_player"] != expected_player
                        or not item["scene3"]
                        or item["combat"]
                        or (int(item["mask"]) & 0x1) == 0
                    )
                ]
                if not nonzero_inputs or invalid_nonzero:
                    raise HarnessFailure(
                        f"{player_name} did not contribute an ordinary "
                        "character-select A edge through the synchronized "
                        "local-input path: "
                        f"nonzero={nonzero_inputs} invalid={invalid_nonzero}"
                    )
                if "[test-automatch] round=" in player_text:
                    raise HarnessFailure(
                        f"{player_name} spectator transition opt-in reached "
                        "the full-rematch life/KO mutation path"
                    )
                player_input_evidence[player_name] = {
                    "driver_enable_count": marker_count,
                    "logged_input_count": len(parsed_inputs),
                    "nonzero_frontend_actions": [
                        {
                            "frame": item["frame"],
                            "local_player": item["local_player"],
                            "mask": f"{int(item['mask']):08X}",
                        }
                        for item in nonzero_inputs
                    ],
                    "full_rematch_round_markers": 0,
                }

            viewer_forbidden = [
                marker
                for marker in (
                    mode_marker,
                    "[test-automatch] input",
                )
                if marker in spectator_text
            ]
            charselect_offset = spectator_text.find("CHARSELECT reached")
            gameplay_offset = spectator_text.find(
                "[spectator] gameplay-entry rendezvous complete"
            )
            if viewer_forbidden:
                raise HarnessFailure(
                    f"{role.name} received player-side transition "
                    f"automation: {viewer_forbidden}"
                )
            wants_latest = getattr(self.args, "spectator_latest", False)
            if wants_latest:
                joined_offset = spectator_text.find("[set-seek] JOINED")
                if not (0 <= joined_offset < gameplay_offset):
                    raise HarnessFailure(
                        f"{role.name} --latest viewer reached gameplay "
                        "without a set-seek join: "
                        f"offsets={joined_offset}/{gameplay_offset}"
                    )
            elif not (
                0 <= charselect_offset < gameplay_offset
            ):
                raise HarnessFailure(
                    f"{role.name} did not reproduce character select before "
                    "gameplay from the confirmed archive: "
                    f"offsets={charselect_offset}/{gameplay_offset}"
                )
            confirmed_transition_evidence = {
                "mode": "spectator-confirmed-transition",
                "players": player_input_evidence,
                "viewer_automation_markers": 0,
                "viewer_charselect_offset": charselect_offset,
                "viewer_gameplay_offset": gameplay_offset,
                "archive_order_proven": True,
                "joined_after_gameplay": (
                    role is not self.spectator
                    or self.args.spectator_join_after_seconds > 0.0
                    or self.args.spectator_postgame_retain_ms > 0
                ),
            }
            self.report.setdefault(
                "spectator_confirmed_transition_by_role", {}
            )[role.name] = confirmed_transition_evidence

        gate_evidence = {
            "address_source": access.get("source"),
            "preflight": route.group(0),
            "route": route_label,
            "route_selector": requested_route,
            "forced_route_contract": forced_route_contract,
            "proved_socket_adopted": True,
            "proved_local_port": proved_port,
            "host_viewer_count": admitted[-1],
            "viewer_authenticated_records_max": max(
                item["records"] for item in authenticated_records
            ),
            "viewer_final_titles": final_titles,
            "wsa_10054_count": reset_count,
            "common_player_confirmed_hashes": len(common_hashes),
            "latest_common_player_hash": {
                "layout": max(common_hashes, key=lambda item: item[2])[0],
                "epoch": max(common_hashes, key=lambda item: item[2])[1],
                "frame": max(common_hashes, key=lambda item: item[2])[2],
                "hash": max(common_hashes, key=lambda item: item[2])[3],
            },
            "required_runtime_markers": {
                "game_start": "[spectator] GameStart" in spectator_text,
                "play": "[spectator] PLAY" in spectator_text,
                "authenticated_records": max(
                    item["records"] for item in authenticated_records
                ) > 0,
            },
            "persistent_connecting": False,
        }
        if confirmed_transition_evidence is not None:
            gate_evidence["confirmed_input_transition"] = (
                confirmed_transition_evidence
            )
        if self.args.spectator_input_mash:
            input_marker = (
                "[input_test] input verification script ENABLED"
            )
            input_hook_marker = (
                "[input_override] method=profile-logical-read enabled"
            )
            neutral_profile = re.search(
                r"\[input-v2\] launch configuration accepted "
                r"mode=\d+ probes=0 spectator=1\b",
                spectator_text,
            )
            forbidden_input_markers = [
                marker
                for marker in (
                    "[input_override] frame=",
                    "[match_drive]",
                    "[netplay] armed:",
                    "[netplay-input] local",
                    "[test-automatch] input",
                )
                if marker in spectator_text
            ]
            if (
                input_marker not in spectator_text
                or input_hook_marker not in spectator_text
                or neutral_profile is None
                or forbidden_input_markers
            ):
                raise HarnessFailure(
                    f"{role.name} did not prove spectator input authority "
                    "isolation under the logical mash source: "
                    f"inputOptIn={input_marker in spectator_text} "
                    f"logicalReadHook={input_hook_marker in spectator_text} "
                    f"neutralProfile={neutral_profile is not None} "
                    f"forbidden={forbidden_input_markers}"
                )
            gate_evidence["input_authority_isolation"] = {
                "logical_mash_source_enabled": True,
                "logical_read_hook_installed": True,
                "neutral_spectator_profile": neutral_profile.group(0),
                "executed_player_input_override_markers": 0,
                "player_netplay_role_armed": False,
                "common_authoritative_player_hashes": len(common_hashes),
            }

        if after_shutdown:
            self.report.setdefault(
                "spectator_post_shutdown_logs_validated", {}
            )[role.name] = True
        else:
            self.report.setdefault(
                "spectator_production_caster_gates", {}
            )[role.name] = gate_evidence
            if role is self.spectator:
                self.report["spectator_production_caster_gate"] = (
                    gate_evidence
                )

    def validate_all_spectator_logs(
        self, *, after_shutdown: bool = False
    ) -> None:
        for role in self.spectators:
            self.validate_spectator_logs(
                role, after_shutdown=after_shutdown
            )
        if self.local_autopunch is None:
            if self.distributed_mode and self.args.spectator:
                raise HarnessFailure(
                    "AutoPunch proof unavailable in distributed mode: no "
                    "local AutoPunch relay was started, so no register/"
                    "lookup/mapping evidence exists for the viewers"
                )
            return
        stats = self.local_autopunch.stats()
        self.report["local_autopunch"]["stats"] = stats
        if int(stats["register_packets"]) <= 0:
            raise HarnessFailure(
                "spectator source/viewers never registered their exact "
                "sockets with the local AutoPunch relay"
            )
        punch_roles = [
            role.name
            for role in self.spectators
            if self.spectator_route_for_role(role) == "punch"
        ]
        if punch_roles:
            expected_mappings = 2 * len(punch_roles)
            if (
                int(stats["lookup_packets"]) < len(punch_roles)
                or int(stats["mapping_packets"]) < expected_mappings
                or len(stats["registrations"]) < len(punch_roles) + 1
            ):
                raise HarnessFailure(
                    "forced-punch viewers did not exercise independent "
                    "AutoPunch register/lookup/mapping exchanges: "
                    f"roles={punch_roles} stats={stats}"
                )
            host_text = (
                self.roles["host"].client_text()
                + "\n"
                + self.roles["host"].stdout_text()
            )
            if "[spectator-autopunch] admitted-viewer" not in host_text:
                raise HarnessFailure(
                    "the spectator source never started traversal toward an "
                    "admitted viewer: no '[spectator-autopunch] "
                    "admitted-viewer' line in the host log, so only the "
                    "viewer was punching"
                )
            if "admitted-viewer" in host_text and "acted=1" not in host_text:
                raise HarnessFailure(
                    "the spectator source saw an admitted viewer but started "
                    "no lookup or punch toward it (every admitted-viewer line "
                    "reports acted=0)"
                )
    def validate_spectator_unavailable(self) -> None:
        negative = self.args.spectator_negative
        if negative not in (
            "unavailable",
            "unavailable-stale-artifact",
            "unavailable-partial",
        ):
            return
        host_evidence = (
            self.roles["host"].stdout_text()
            + "\n"
            + self.roles["host"].client_text()
            + "\n"
            + self.roles["host"].dll_text()
        )
        fixture = self.report.get("spectator_unavailable_fixture")
        expected_port: int | None = None
        if negative in ("unavailable", "unavailable-stale-artifact"):
            if not isinstance(fixture, dict):
                raise HarnessFailure("unavailable-source fixture was not recorded")
            expected_port = int(fixture["blocked_udp_port"])
            bind_attempt = re.search(
                rf"\[host\] spectator source bind attempt mode=test-explicit "
                rf"port={expected_port}",
                host_evidence,
            )
            unavailable = re.search(
                rf"\[host\] spectator source bind failed mode=test-explicit "
                rf"port={expected_port} err=\d+; optional spectator service "
                r"disabled; player session continues WITHOUT spectator serving",
                host_evidence,
            )
        else:
            bind_attempt = re.search(
                r"\[host\] spectator source bind attempt mode=ephemeral port=0",
                host_evidence,
            )
            unavailable = re.search(
                r"\[spectator\.sidecar\] UNAVAILABLE: test forced spectator "
                r"startup failure after source acquisition; optional spectator "
                r"service disabled before game publication; player session continues",
                host_evidence,
            )
        contract = re.search(
            r"\[spectator-contract\] v=1 state=unavailable "
            r"session=\d+ reason='[^']+' no-address-published",
            host_evidence,
        )
        forbidden_markers = (
            "[spectator-access] address=",
            "[spectator-contract] v=1 state=access-ready",
            "[spectator.sidecar] serving on UDP",
            "source spectator relay registration",
        )
        forbidden = [
            marker for marker in forbidden_markers if marker in host_evidence
        ]
        stale_artifact_exists = (
            self._stale_spectator_artifact is not None
            and self._stale_spectator_artifact.exists()
        )
        if (
            bind_attempt is None
            or unavailable is None
            or contract is None
            or forbidden
            or stale_artifact_exists
        ):
            raise HarnessFailure(
                "unavailable spectator source did not fail soft: "
                f"bindAttempt={bind_attempt is not None} "
                f"bindLog={unavailable is not None} "
                f"contract={contract is not None} "
                f"forbidden={forbidden} staleArtifact={stale_artifact_exists}"
            )
        common_hashes = self._common_player_hashes()
        if not common_hashes:
            raise HarnessFailure(
                "players did not continue with matching confirmed hashes "
                "after optional spectator serving failed"
            )
        self.report["spectator_unavailable_gate"] = {
            "fixture": negative,
            "blocked_port": expected_port,
            "bind_attempt": bind_attempt.group(0),
            "bind_failure": unavailable.group(0),
            "unavailable_contract": contract.group(0),
            "spectator_address_emitted": False,
            "route_descriptor_published": False,
            "autopunch_or_source_relay_registration": False,
            "stale_artifact_present": stale_artifact_exists,
            "player_common_hash_lines": len(common_hashes),
        }
        self.event(
            "spectator-unavailable-fail-soft-verified",
            fixture=negative,
            blocked_port=expected_port,
            player_common_hash_lines=len(common_hashes),
        )

    def _common_player_hashes(
        self,
    ) -> set[tuple[int, int, int, str]]:
        hashes: dict[str, set[tuple[int, int, int, str]]] = {}
        for name, role in self.roles.items():
            hashes[name] = {
                (
                    int(match.group("layout")),
                    int(match.group("epoch")),
                    int(match.group("frame")),
                    match.group("hash").upper(),
                )
                for match in CONFIRMED_HASH_MATCH_RE.finditer(
                    role.dll_text()
                )
            }
        return hashes.get("host", set()) & hashes.get("client", set())

    def _require_live_gameplay_players(
        self, checkpoint: str
    ) -> dict[str, dict[str, object]]:
        states = self.current_states()
        invalid = {
            name: state
            for name, state in states.items()
            if (
                not state.get("alive")
                or state.get("scene") != 2
                or state.get("match_sub") != 0
            )
        }
        if invalid:
            raise HarnessFailure(
                "authoritative players stopped gameplay during spectator "
                f"capacity/readmission at {checkpoint}: {invalid}"
            )
        return states

    def run_spectator_capacity_readmit_gate(self) -> None:
        if not self.args.spectator_capacity_readmit:
            return
        first = self.spectator
        denied = self.capacity_denied_role
        replacement = self.capacity_replacement_role
        player_address = self._spectator_player_address
        if (
            first is None
            or denied is None
            or replacement is None
            or player_address is None
        ):
            raise HarnessFailure(
                "capacity/readmission fixture roles or host address are missing"
            )

        self.wait_for_spectator_playback(first)
        self.validate_spectator_logs(first)
        before_states = self._require_live_gameplay_players(
            "initial viewer authenticated"
        )
        before_frames = {
            name: self.runtime_data_u32(role, self.net_frame_rva)
            for name, role in self.roles.items()
        }
        before_hashes = self._common_player_hashes()
        if not before_hashes:
            raise HarnessFailure(
                "players had no matching confirmed hash before the "
                "capacity/readmission transaction"
            )

        host = self.roles["host"]
        initial_host_evidence = host.client_text()
        initial_counts = [
            match
            for match in SPECTATOR_VIEWER_COUNT_RE.finditer(
                initial_host_evidence
            )
            if (
                int(match.group("count")) == 1
                and int(match.group("capacity")) == 1
            )
        ]
        if not initial_counts:
            raise HarnessFailure(
                "the first authenticated viewer did not own the sole "
                "capacity slot (expected count=1 capacity=1)"
            )

        self.start_spectator(
            denied,
            expected_failure=9,
            spectate_address_override=player_address,
        )
        if (
            denied.game_memory is not None
            or denied.process is None
            or denied.process.poll() is None
        ):
            raise HarnessFailure(
                "capacity-denied spectator did not finish without a game "
                "child"
            )
        self._require_live_gameplay_players(
            "concurrent capacity denial"
        )

        host_log_offset = len(host.client_text())
        close_request = self._post_role_close(first)
        game_deadline = time.monotonic() + 20.0
        while (
            time.monotonic() < game_deadline
            and first.game_memory is not None
            and first.game_memory.alive()
        ):
            self._require_live_gameplay_players("closing first viewer")
            time.sleep(0.02)
        if first.game_memory is None or first.game_memory.alive():
            raise HarnessFailure(
                "first spectator ignored its isolated WM_CLOSE"
            )
        first_game_exit = first.game_memory.exit_code()
        if first_game_exit not in (0, 1):
            raise HarnessFailure(
                "first spectator game exited uncleanly: "
                f"{first_game_exit}"
            )
        if first.process is None:
            raise HarnessFailure("first spectator caster was unavailable")
        try:
            first_caster_exit = first.process.wait(timeout=10)
        except subprocess.TimeoutExpired as exc:
            raise HarnessFailure(
                "first spectator caster did not exit after its game"
            ) from exc
        if first_caster_exit != 0:
            raise HarnessFailure(
                "first spectator caster exited uncleanly: "
                f"{first_caster_exit}"
            )

        release_deadline = time.monotonic() + 12.0
        zero_count: re.Match[str] | None = None
        host_release: re.Match[str] | None = None
        caster_release: re.Match[str] | None = None
        while time.monotonic() < release_deadline:
            self._require_live_gameplay_players(
                "waiting for first viewer release"
            )
            host_delta = host.client_text()[host_log_offset:]
            zero_matches = [
                match
                for match in SPECTATOR_VIEWER_COUNT_RE.finditer(host_delta)
                if (
                    int(match.group("count")) == 0
                    and int(match.group("capacity")) == 1
                )
            ]
            host_releases = list(
                SPECTATOR_HOST_RELEASE_RE.finditer(host_delta)
            )
            caster_releases = list(
                SPECTATOR_CASTER_RELEASE_RE.finditer(first.client_text())
            )
            if zero_matches:
                zero_count = zero_matches[-1]
            if host_releases:
                host_release = host_releases[-1]
            if caster_releases:
                caster_release = caster_releases[-1]
            if (
                zero_count is not None
                and host_release is not None
                and caster_release is not None
                and caster_release.group("acknowledged") == "1"
            ):
                break
            time.sleep(0.02)
        first_dll = first.dll_text()
        detach_shutdown_marker = (
            "[config] saved QOHcnf.key successfully with local profile "
            "values preserved result=1"
        )
        detach_failures = [
            marker
            for marker in (
                "[spectator-viewer] shutdown QSP Detach relay framing failed",
                "[spectator-viewer-detach] sendto failed",
            )
            if marker in first_dll
        ]
        if (
            zero_count is None
            or host_release is None
            or caster_release is None
            or caster_release.group("acknowledged") != "1"
            or detach_shutdown_marker not in first_dll
            or detach_failures
        ):
            raise HarnessFailure(
                "first viewer did not prove exact QSP Detach plus "
                "idempotent control release before replacement: "
                f"count0={zero_count.group(0) if zero_count else None} "
                f"hostRelease="
                f"{host_release.group(0) if host_release else None} "
                f"casterRelease="
                f"{caster_release.group(0) if caster_release else None} "
                f"shutdownEdge={detach_shutdown_marker in first_dll} "
                f"detachFailures={detach_failures}"
            )
        self.validate_spectator_logs(first, after_shutdown=True)
        self._require_live_gameplay_players(
            "first viewer detached and capacity reached zero"
        )

        self.retired_spectators.append(first)
        self.spectators = [replacement]
        self.spectator = replacement
        self._spectator_started = False
        replacement_started = time.monotonic()
        replacement_host_offset = len(host.client_text())
        self.start_spectator(
            replacement,
            spectate_address_override=player_address,
        )

        admission_deadline = replacement_started + 20.0
        replacement_count: re.Match[str] | None = None
        while time.monotonic() < admission_deadline:
            self._scan_failures()
            host_delta = host.client_text()[replacement_host_offset:]
            replacement_counts = [
                match
                for match in SPECTATOR_VIEWER_COUNT_RE.finditer(host_delta)
                if (
                    int(match.group("count")) == 1
                    and int(match.group("capacity")) == 1
                )
            ]
            if replacement_counts:
                replacement_count = replacement_counts[-1]
                break
            time.sleep(0.02)
        if replacement_count is None:
            raise HarnessFailure(
                "replacement viewer did not immediately reclaim the released "
                "capacity=1 slot"
            )
        replacement_admission_seconds = (
            time.monotonic() - replacement_started
        )
        module_deadline = time.monotonic() + 15.0
        while time.monotonic() < module_deadline:
            if (
                replacement.process is not None
                and replacement.process.poll() is not None
            ):
                raise HarnessFailure(
                    "replacement spectator caster exited during DLL injection: "
                    f"{replacement.process.returncode}"
                )
            assert replacement.game_memory is not None
            module = module_info(
                replacement.game_memory.pid, Path(self.args.dll).name
            )
            if module is not None:
                replacement.dll_base = module[0]
                break
            time.sleep(0.02)
        if replacement.dll_base is None:
            raise HarnessFailure(
                "replacement spectator did not load qoh99_netplay.dll"
            )
        self.wait_for_spectator_playback(replacement)
        self.validate_spectator_logs(replacement)
        after_states = self._require_live_gameplay_players(
            "replacement authenticated and caught up"
        )
        after_frames = {
            name: self.runtime_data_u32(role, self.net_frame_rva)
            for name, role in self.roles.items()
        }
        frame_progress = {
            name: after_frames[name] - before_frames[name]
            for name in before_frames
        }
        after_hashes = self._common_player_hashes()
        new_matching_hashes = after_hashes - before_hashes
        if (
            any(progress < 60 for progress in frame_progress.values())
            or not new_matching_hashes
        ):
            raise HarnessFailure(
                "players did not continue with new matching confirmed hashes "
                "across spectator detach/readmission: "
                f"frameProgress={frame_progress} "
                f"newCommonHashes={len(new_matching_hashes)}"
            )

        gate = {
            "capacity": 1,
            "route": self.args.spectator_route,
            "verbatim_host_uri_reused": True,
            "first_viewer": {
                "role": first.name,
                "authenticated_playback": True,
                "count_one": initial_counts[-1].group(0),
                "close_request": close_request,
                "game_exit": first_game_exit,
                "caster_exit": first_caster_exit,
            },
            "concurrent_denial": {
                "role": denied.name,
                "preflight_failure": 9,
                "game_process_created": False,
                "caster_exit": denied.process.returncode,
            },
            "detach_release": {
                "count_zero": zero_count.group(0),
                "host_release": host_release.group(0),
                "caster_release": caster_release.group(0),
                "qsp_detach_shutdown_edge": detach_shutdown_marker,
                "qsp_detach_send_failures": detach_failures,
                "qsp_detach_preceded_control_release":
                    host_release.group("present") == "0",
                "control_release_preceded_qsp_detach":
                    host_release.group("present") == "1",
            },
            "replacement_viewer": {
                "role": replacement.name,
                "count_one": replacement_count.group(0),
                "admission_seconds": round(
                    replacement_admission_seconds, 3
                ),
                "authenticated_catch_up": True,
            },
            "players": {
                "before_states": before_states,
                "after_states": after_states,
                "canonical_frame_progress": frame_progress,
                "common_hashes_before": len(before_hashes),
                "common_hashes_after": len(after_hashes),
                "new_matching_hashes": len(new_matching_hashes),
            },
        }
        self.report["spectator_capacity_readmit_gate"] = gate
        self.event(
            "spectator-capacity-readmit-complete",
            route=self.args.spectator_route,
            replacement_admission_seconds=round(
                replacement_admission_seconds, 3
            ),
            frame_progress=frame_progress,
            new_matching_hashes=len(new_matching_hashes),
        )

    def _post_role_close(self, role: Role) -> list[dict[str, object]]:
        if role.game_memory is None or not role.game_memory.alive():
            raise HarnessFailure(
                f"{role.name} game was not alive for WM_CLOSE"
            )
        if not self.args.visible_games:
            return post_close_on_named_desktop(
                role.game_memory.pid, "qoh99np_hidden"
            )
        diagnostics: list[dict[str, object]] = []
        posted = 0
        for hwnd, title in process_windows(role.game_memory.pid):
            if not is_game_window_title(title):
                continue
            ok = bool(user32.PostMessageW(hwnd, WM_CLOSE, 0, 0))
            diagnostics.append(
                {
                    "pid": role.game_memory.pid,
                    "hwnd": f"0x{hwnd:X}",
                    "title": title,
                    "post": "ok" if ok else "failed",
                    "desktop": "interactive",
                }
            )
            posted += int(ok)
        if posted == 0:
            raise HarnessFailure(
                f"{role.name} had no live game window accepting WM_CLOSE: "
                f"{diagnostics}"
            )
        return diagnostics

    def close_players_for_retained_join(
        self, timeout: float = 20.0
    ) -> int:
        if not self.args.spectator_postgame_retain_ms:
            raise HarnessFailure("retained join was not requested")
        if self.observer:
            self.observer.detach_all()

        close_requests: dict[str, list[dict[str, object]]] = {}
        for name in ("client", "host"):
            role = self.roles[name]
            close_requests[name] = self._post_role_close(role)
        self.event(
            "retention-player-close-requested",
            windows=close_requests,
        )

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if all(
                role.game_memory is not None
                and not role.game_memory.alive()
                for role in self.roles.values()
            ):
                break
            time.sleep(0.02)
        alive = [
            role.name
            for role in self.roles.values()
            if role.game_memory is not None and role.game_memory.alive()
        ]
        if alive:
            raise HarnessFailure(
                f"player games did not exit before retained join: {alive}"
            )

        game_exit_codes = {
            name: role.game_memory.exit_code()
            for name, role in self.roles.items()
            if role.game_memory is not None
        }
        bad = {
            name: code
            for name, code in game_exit_codes.items()
            if code not in (0, 1)
        }
        if bad:
            raise HarnessFailure(
                f"player games exited uncleanly before retention: {bad}"
            )

        client = self.roles["client"]
        host = self.roles["host"]
        if client.process is None or host.process is None:
            raise HarnessFailure("player caster process is unavailable")
        try:
            client_code = client.process.wait(timeout=10)
        except subprocess.TimeoutExpired as exc:
            raise HarnessFailure(
                "client caster did not exit after its player game"
            ) from exc
        if client_code != 0:
            raise HarnessFailure(
                f"client caster exited uncleanly: {client_code}"
            )
        if host.process.poll() is not None:
            raise HarnessFailure(
                "host caster exited instead of entering diagnostic retention: "
                f"code={host.process.returncode}"
            )

        self.players_closed_for_retention = True
        retention_deadline = time.monotonic() + min(
            12.0,
            max(
                3.0,
                self.args.spectator_postgame_retain_ms / 1000.0,
            ),
        )
        enter: re.Match[str] | None = None
        while time.monotonic() < retention_deadline:
            if host.process.poll() is not None:
                raise HarnessFailure(
                    "host caster exited before publishing the retained "
                    f"spectator lifecycle: code={host.process.returncode}"
                )
            evidence = host.stdout_text() + "\n" + host.client_text()
            matches = list(
                SPECTATOR_RETENTION_ENTER_RE.finditer(evidence)
            )
            if matches:
                enter = matches[-1]
                break
            time.sleep(0.02)
        if enter is None:
            raise HarnessFailure(
                "host caster remained alive after the game exited but never "
                "entered the bounded spectator-retention phase"
            )
        remaining_ms = int(enter.group("remaining_ms"))
        enter_viewers = int(enter.group("viewers"))
        if (
            remaining_ms <= 0
            or remaining_ms > self.args.spectator_postgame_retain_ms
            or enter_viewers != 0
            or enter.group("phase").lower() != "retained"
        ):
            raise HarnessFailure(
                "host emitted an invalid spectator-retention entry contract: "
                f"{enter.group(0)}"
            )
        if remaining_ms < 8000:
            raise HarnessFailure(
                "spectator-retention entry left less than eight seconds for "
                f"the real caster/game launch: {enter.group(0)}"
            )

        self.report["retention_player_shutdown"] = {
            "close_requests": close_requests,
            "game_exit_codes": game_exit_codes,
            "client_caster_exit": client_code,
            "host_caster_alive": True,
            "requested_retention_ms":
                self.args.spectator_postgame_retain_ms,
            "entry_contract": enter.group(0),
            "entry_remaining_ms": remaining_ms,
        }
        self.event(
            "retention-player-shutdown-complete",
            game_exit_codes=game_exit_codes,
            host_caster_alive=True,
            remaining_ms=remaining_ms,
        )
        return remaining_ms

    def wait_for_retained_spectator_playback(
        self, timeout: float = 45.0
    ) -> None:
        role = self.spectator
        if role is None or role.game_memory is None:
            raise HarnessFailure("retained spectator game was not launched")
        deadline = time.monotonic() + timeout
        last_complete: str | None = None
        while time.monotonic() < deadline:
            self._scan_failures()
            host = self.roles["host"]
            if host.process is None or host.process.poll() is not None:
                raise HarnessFailure(
                    "retained host expired before the late viewer consumed "
                    "the sealed archive"
                )
            text = role.dll_text()
            complete = list(
                SPECTATOR_ARCHIVE_COMPLETE_RE.finditer(text)
            )
            if complete:
                final = complete[-1]
                last_complete = final.group(0)
                host_evidence = (
                    host.stdout_text() + "\n" + host.client_text()
                )
                admissions = list(
                    SPECTATOR_RETENTION_LATE_ADMISSION_RE.finditer(
                        host_evidence
                    )
                )
                if not admissions:
                    raise HarnessFailure(
                        "retained viewer reached archive terminal without "
                        "the host publishing a late-admission transition"
                    )
                valid_admissions = [
                    candidate
                    for candidate in admissions
                    if (
                        int(candidate.group("viewers")) == 1
                        and int(candidate.group("capacity")) >= 1
                        and int(candidate.group("remaining_ms")) > 0
                        and candidate.group("phase").lower() == "retained"
                    )
                ]
                if not valid_admissions:
                    raise HarnessFailure(
                        "host emitted an invalid retained late-admission "
                        "contract: "
                        + " | ".join(
                            candidate.group(0)
                            for candidate in admissions[-4:]
                        )
                    )
                admission = valid_admissions[-1]
                missing = [
                    marker
                    for marker in SPECTATOR_REQUIRED_DLL_MARKERS
                    if marker not in text
                ]
                if missing:
                    raise HarnessFailure(
                        "retained viewer reached archive terminal without "
                        f"the playback contract: missing={missing}"
                    )
                records = int(final.group("records"))
                if records < 30:
                    raise HarnessFailure(
                        "retained viewer received an implausibly short "
                        f"archive: {final.group(0)}"
                    )
                if (
                    int(final.group("reason")) != 2
                    or int(final.group("hash"), 16) == 0
                ):
                    raise HarnessFailure(
                        "active-match source close did not yield the exact "
                        "SourceStopped terminal with a nonzero post-tail hash: "
                        f"{final.group(0)}"
                    )
                source_seals = list(
                    SPECTATOR_PUBLISH_SEALED_RE.finditer(
                        host.dll_text()
                    )
                )
                if not source_seals:
                    raise HarnessFailure(
                        "retained viewer completed but the authoritative "
                        "source has no parseable sealed-prefix proof"
                    )
                source_seal = source_seals[-1]
                if (
                    int(source_seal.group("reason"))
                        != int(final.group("reason"))
                    or int(source_seal.group("cursor")) != records
                    or int(source_seal.group("published")) != records
                    or int(source_seal.group("written")) != records
                    or source_seal.group("hash").upper()
                        != final.group("hash").upper()
                ):
                    raise HarnessFailure(
                        "late viewer's terminal does not exactly match the "
                        "source's sealed confirmed prefix: "
                        f"source={source_seal.group(0)} "
                        f"viewer={final.group(0)}"
                    )
                self.report["retained_spectator_playback"] = {
                    "terminal": final.group(0),
                    "source_seal": source_seal.group(0),
                    "records": records,
                    "reason": int(final.group("reason")),
                    "last_state_hash": final.group("hash").upper(),
                    "joined_after_player_process_exit": True,
                    "late_admission": admission.group(0),
                }
                self.event(
                    "retained-spectator-playback-complete",
                    records=records,
                    reason=int(final.group("reason")),
                    remaining_ms=int(admission.group("remaining_ms")),
                )
                return
            time.sleep(0.01)
        raise HarnessFailure(
            "retained viewer did not consume the terminal archive: "
            f"lastComplete={last_complete}"
        )

    def close_retained_viewer_and_wait_for_host(
        self, timeout: float | None = None
    ) -> None:
        role = self.spectator
        host = self.roles["host"]
        if (
            role is None
            or role.game_memory is None
            or role.process is None
            or host.process is None
        ):
            raise HarnessFailure("retained lifecycle process is unavailable")
        host_wait_timeout = (
            timeout
            if timeout is not None
            else self.args.spectator_postgame_retain_ms / 1000.0 + 12.0
        )
        close_request = self._post_role_close(role)
        viewer_close_deadline = time.monotonic() + 20.0
        while (
            time.monotonic() < viewer_close_deadline
            and role.game_memory.alive()
        ):
            time.sleep(0.02)
        if role.game_memory.alive():
            raise HarnessFailure("retained spectator ignored WM_CLOSE")
        viewer_game_code = role.game_memory.exit_code()
        if viewer_game_code not in (0, 1):
            raise HarnessFailure(
                f"retained spectator game exited uncleanly: {viewer_game_code}"
            )
        try:
            viewer_caster_code = role.process.wait(timeout=10)
        except subprocess.TimeoutExpired as exc:
            raise HarnessFailure(
                "retained spectator caster did not exit"
            ) from exc
        if viewer_caster_code != 0:
            raise HarnessFailure(
                "retained spectator caster exited uncleanly: "
                f"{viewer_caster_code}"
            )
        try:
            host_caster_code = host.process.wait(
                timeout=host_wait_timeout
            )
        except subprocess.TimeoutExpired as exc:
            raise HarnessFailure(
                "host caster outlived its bounded diagnostic retention"
            ) from exc
        if host_caster_code != 0:
            raise HarnessFailure(
                f"retained host caster exited uncleanly: {host_caster_code}"
            )
        host_evidence = host.stdout_text() + "\n" + host.client_text()
        expiries = list(
            SPECTATOR_RETENTION_EXPIRY_RE.finditer(host_evidence)
        )
        stops = list(SPECTATOR_RETENTION_STOP_RE.finditer(host_evidence))
        if not expiries or not stops or stops[-1].group("ok") != "1":
            raise HarnessFailure(
                "retained host exited without the exact expiry/clean-stop "
                "lifecycle contracts: "
                f"expiry={expiries[-1].group(0) if expiries else None} "
                f"stop={stops[-1].group(0) if stops else None}"
            )
        self.report["retention_shutdown"] = {
            "viewer_close_request": close_request,
            "viewer_game_exit": viewer_game_code,
            "viewer_caster_exit": viewer_caster_code,
            "host_caster_exit": host_caster_code,
            "expiry_contract": expiries[-1].group(0),
            "stop_contract": stops[-1].group(0),
        }
        self.event(
            "retained-lifecycle-shutdown-complete",
            viewer_game_exit=viewer_game_code,
            viewer_caster_exit=viewer_caster_code,
            host_caster_exit=host_caster_code,
        )

    def close_games_gracefully(self, timeout: float = 20.0) -> None:
        if self.observer:
            self.observer.detach_all()

        close_requests: dict[str, list[dict[str, object]]] = {}
        close_targets: dict[str, list[tuple[int, str]]] = {}
        for role in self.runtime_roles():
            name = role.name
            memory = role.game_memory
            if not memory or not memory.alive():
                raise HarnessFailure(
                    f"{name} game was not alive for graceful shutdown"
                )
            windows = role.host.process_windows(memory.pid)
            preferred = [
                item
                for item in windows
                if is_game_window_title(item[1])
            ]
            if not preferred:
                raise HarnessFailure(
                    f"{name} has no verified game window for WM_CLOSE; "
                    f"windows={windows}"
                )
            close_targets[name] = preferred
            close_requests[name] = [
                {
                    "pid": memory.pid,
                    "hwnd": f"0x{hwnd:X}",
                    "title": title,
                }
                for hwnd, title in preferred
            ]

        close_order = [
            *reversed(self.spectators),
            self.roles["client"],
            self.roles["host"],
        ]
        for role in close_order:
            name = role.name
            assert role is not None and role.game_memory is not None
            role_hidden = not self.args.visible_games
            if role_hidden:
                candidate_diagnostics = role.host.post_close_on_named_desktop(
                    role.game_memory.pid, "qoh99np_hidden"
                )
                close_requests[name].extend(candidate_diagnostics)
                self.event(
                    "graceful-close-candidates",
                    role=name,
                    candidates=candidate_diagnostics,
                )
                continue
            candidate_diagnostics: list[dict[str, object]] = []
            for hwnd, title in close_targets[name]:
                thread, owner_pid = role.host.window_thread_process_id(hwnd)
                candidate_diagnostics.append(
                    {
                        "hwnd": f"0x{hwnd:X}",
                        "title": title,
                        "is_window": role.host.is_window(hwnd),
                        "owner_pid": owner_pid,
                        "thread_id": thread,
                    }
                )
            self.event(
                "graceful-close-candidates",
                role=name,
                candidates=candidate_diagnostics,
            )
            posted = 0
            stale = 0
            candidates = list(close_targets[name])
            for attempt in range(2):
                for hwnd, title in candidates:
                    posted_ok, error = role.host.post_close_direct(hwnd)
                    if posted_ok:
                        posted += 1
                        close_requests[name].append(
                            {
                                "pid": role.game_memory.pid,
                                "hwnd": f"0x{hwnd:X}",
                                "title": title,
                                "post": "ok",
                                "attempt": attempt + 1,
                            }
                        )
                        continue
                    if error == 1400:
                        stale += 1
                        close_requests[name].append(
                            {
                                "pid": role.game_memory.pid,
                                "hwnd": f"0x{hwnd:X}",
                                "title": title,
                                "post": "stale",
                                "attempt": attempt + 1,
                            }
                        )
                        continue
                    raise ctypes.WinError(
                        error,
                        f"PostMessageW(WM_CLOSE,{name},hwnd=0x{hwnd:X})",
                    )
                if posted or not role.game_memory.alive():
                    break
                time.sleep(0.02)
                refreshed = role.host.process_windows(role.game_memory.pid)
                candidates = [
                    item
                    for item in refreshed
                    if is_game_window_title(item[1])
                ]
            if posted == 0 and role.game_memory.alive():
                raise HarnessFailure(
                    f"{name} had no live game window accepting WM_CLOSE "
                    f"after refresh; stale={stale}"
                )

        self.event("graceful-close-requested", windows=close_requests)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if all(
                role.game_memory and not role.game_memory.alive()
                for role in self.runtime_roles()
            ):
                break
            time.sleep(0.02)
        still_alive = [
            name
            for role in self.runtime_roles()
            for name in (role.name,)
            if role.game_memory and role.game_memory.alive()
        ]
        if still_alive:
            raise HarnessFailure(
                f"games did not exit after WM_CLOSE: {still_alive}"
            )

        game_exit_codes = {
            name: role.game_memory.exit_code()
            for role in self.runtime_roles()
            for name in (role.name,)
            if role.game_memory
        }
        bad_game_codes = {
            name: code
            for name, code in game_exit_codes.items()
            if code not in (0, 1)
        }
        if bad_game_codes:
            raise HarnessFailure(
                f"games exited uncleanly after WM_CLOSE: {bad_game_codes}"
            )

        caster_exit_codes: dict[str, int] = {}
        for role in self.runtime_roles():
            name = role.name
            if not role.process:
                raise HarnessFailure(f"{name} caster process was not started")
            try:
                caster_exit_codes[name] = role.process.wait(timeout=10)
            except subprocess.TimeoutExpired as exc:
                raise HarnessFailure(
                    f"{name} caster did not exit after its game closed"
                ) from exc
        bad_caster_codes = {
            name: code
            for name, code in caster_exit_codes.items()
            if code != 0
        }
        if bad_caster_codes:
            raise HarnessFailure(
                f"casters exited uncleanly: {bad_caster_codes}"
            )

        save_marker = (
            "[config] saved QOHcnf.key successfully with local profile "
            "values preserved result=1"
        )
        missing_save_logs = [
            name
            for name, role in self.roles.items()
            if save_marker not in role.dll_text()
        ]
        self.report["graceful_shutdown"] = {
            "close_requests": close_requests,
            "game_exit_codes": game_exit_codes,
            "caster_exit_codes": caster_exit_codes,
            "save_marker": save_marker,
            "save_marker_seen": {
                name: name not in missing_save_logs for name in self.roles
            },
        }
        if missing_save_logs:
            raise HarnessFailure(
                "normal shutdown did not log preference-preserving config "
                f"save for {missing_save_logs}"
            )
        self.event(
            "graceful-game-exit-complete",
            game_exit_codes=game_exit_codes,
            caster_exit_codes=caster_exit_codes,
            save_marker_seen=True,
        )

    def validate_replay_artifacts(self) -> None:
        if not self.args.full_match_rematch:
            return

        by_role: dict[str, list[dict[str, object]]] = {}
        games_required = max(2, int(self.args.game_count))
        for name, role in self.roles.items():
            replay_dir = role.root / "replays"
            files = role.host.glob(replay_dir, "**/*.q99r")
            partials = role.host.glob(replay_dir, "**/*.q99r.part")
            if partials:
                raise HarnessFailure(
                    f"{name} left torn replay artifacts: {partials}"
                )
            if len(files) != games_required:
                raise HarnessFailure(
                    f"{name} expected {games_required - 1} completed game "
                    "replay(s) plus the active rematch replay, "
                    f"found {len(files)} file(s): {files}"
                )

            inspected: list[dict[str, object]] = []
            for path in files:
                returncode, output = role.host.run(
                    [
                        str(role.caster_exe),
                        "replay",
                        "inspect",
                        self._display_path(role, path),
                    ],
                    role.root,
                    20,
                )
                completed_returncode = returncode
                frames_match = re.search(r"frames:\s*(\d+)", output)
                terminal_match = re.search(
                    r"terminal\s*:\s*(\d+)\s*\(([^)]+)\)", output
                )
                if (
                    completed_returncode != 0
                    or "status      : ok" not in output
                    or "class       : sealed game" not in output
                    or "footer      : present" not in output
                    or frames_match is None
                    or int(frames_match.group(1)) == 0
                    or terminal_match is None
                ):
                    raise HarnessFailure(
                        f"{name} replay inspection failed for {path}: "
                        f"rc={completed_returncode}\n{output}"
                    )
                inspected.append(
                    {
                        "path": str(path),
                        "frames": int(frames_match.group(1)),
                        "terminal": int(terminal_match.group(1)),
                        "terminal_text": terminal_match.group(2),
                    }
                )

            terminal_kinds = [
                int(item["terminal"]) for item in inspected
            ]
            if (
                terminal_kinds.count(1) != games_required - 1
                or terminal_kinds.count(4) != 1
                or len(terminal_kinds) != games_required
            ):
                raise HarnessFailure(
                    f"{name} replay terminals were {terminal_kinds}; "
                    f"expected {games_required - 1} completed game(s) and "
                    "one abnormal active rematch"
                )
            by_role[name] = inspected

        self.report["replay_artifacts"] = by_role
        self.event("replay-artifacts-verified", roles=by_role)

    def validate_config_persistence(self) -> None:
        after_by_role: dict[str, object] = {}
        differences: dict[str, object] = {}
        for name, role in self.roles.items():
            if role.config_before is None:
                raise HarnessFailure(
                    f"{name} lacks its pre-launch config snapshot"
                )
            key = role.install / "System" / "QOHcnf.key"
            after = role.host.config_snapshot(
                key, self.config_persistence_offsets
            )
            after_by_role[name] = after
            before_profile = int(role.config_before["profile"])
            after_profile = int(after["profile"])
            before_mtime = int(role.config_before["mtime_ns"])
            after_mtime = int(after["mtime_ns"])
            before_values = dict(role.config_before["values"])
            after_values = dict(after["values"])
            value_differences = {
                offset: {
                    "before": before_values[offset],
                    "after": after_values[offset],
                }
                for offset in self.config_persistence_offsets
                if before_values[offset] != after_values[offset]
            }
            if (
                before_profile != after_profile
                or value_differences
                or before_mtime == after_mtime
            ):
                differences[name] = {
                    "profile_before": before_profile,
                    "profile_after": after_profile,
                    "mtime_ns_before": before_mtime,
                    "mtime_ns_after": after_mtime,
                    "mtime_changed": before_mtime != after_mtime,
                    "values": value_differences,
                }

        persistence = self.report["config_persistence"]
        assert isinstance(persistence, dict)
        persistence["after"] = after_by_role
        persistence["differences"] = differences
        persistence["passed"] = not differences
        if differences:
            raise HarnessFailure(
                "normal QOHcnf.key save did not execute or changed "
                "selected-profile values: "
                f"{differences}"
            )
        self.event(
            "config-persistence-verified",
            offsets=list(self.config_persistence_offsets),
            profiles={
                name: snapshot["profile"]
                for name, snapshot in after_by_role.items()
            },
        )

    def cleanup(self, force_games: bool) -> list[str]:
        errors: list[str] = []

        def record(label: str, exc: BaseException) -> None:
            errors.append(f"{label}: {type(exc).__name__}: {exc}")

        if self.observer:
            try:
                self.observer.detach_all()
            except BaseException as exc:
                record("confirmed-hash observer detach", exc)
        try:
            self.refresh_evidence(force=True)
        except BaseException as exc:
            record("final evidence mirror flush", exc)
        if force_games:
            for role in self.artifact_roles():
                try:
                    if role.game_memory is None and role.process is not None:
                        pid = role.host.child_game_pid(
                            role.process.pid, role.game.name
                        )
                        if pid:
                            role.game_memory = role.host.open_process(pid)
                    if role.game_memory:
                        try:
                            role.game_memory.resume()
                        except BaseException as exc:
                            record(f"{role.name} game resume", exc)
                        try:
                            role.game_memory.terminate()
                        except BaseException as exc:
                            record(f"{role.name} game terminate", exc)
                except BaseException as exc:
                    record(f"{role.name} game lookup", exc)
        for role in self.artifact_roles():
            if role.process is None or role.process.poll() is not None:
                continue
            try:
                role.process.wait(timeout=6)
            except subprocess.TimeoutExpired:
                try:
                    role.host.stop(role.process)
                except BaseException as exc:
                    record(f"{role.name} caster stop", exc)
            except BaseException as exc:
                record(f"{role.name} caster wait", exc)
        if self.relay:
            try:
                WindowsProcessControl.stop(self.relay)
            except BaseException as exc:
                record("relay stop", exc)
        if self.local_autopunch is not None:
            try:
                self.report["local_autopunch"]["final_stats"] = (
                    self.local_autopunch.stats()
                )
                self.local_autopunch.close()
            except BaseException as exc:
                record("local AutoPunch relay stop", exc)
            finally:
                self.local_autopunch = None
        if self.spectator_port_blocker is not None:
            try:
                self.spectator_port_blocker.close()
            except BaseException as exc:
                record("unavailable source fixture close", exc)
            finally:
                self.spectator_port_blocker = None
        for role in self.artifact_roles():
            for handle_name in ("stdout_handle", "stderr_handle"):
                handle = getattr(role, handle_name)
                if handle:
                    try:
                        handle.close()
                    except BaseException as exc:
                        record(f"{role.name} {handle_name} close", exc)
                    finally:
                        setattr(role, handle_name, None)
            if role.game_memory:
                try:
                    role.game_memory.close()
                except BaseException as exc:
                    record(f"{role.name} game memory close", exc)
        for handle_name in ("relay_stdout_handle", "relay_stderr_handle"):
            handle = getattr(self, handle_name)
            if handle:
                try:
                    handle.close()
                except BaseException as exc:
                    record(f"{handle_name} close", exc)
                finally:
                    setattr(self, handle_name, None)
        for key, host in self.hosts.items():
            if not key:
                continue
            try:
                host.shutdown()
            except BaseException as exc:
                record(f"{key} agent shutdown", exc)
                self._cleanup_escalated = True
                try:
                    host.escalate_kill()
                except BaseException as kill_exc:
                    record(f"{key} escalated remote kill", kill_exc)
        self.report["cleanup"] = {
            "force_games": force_games,
            "errors": errors,
            "escalated": self._cleanup_escalated,
            "task_owned_processes_remain": any(
                role.process is not None and role.process.poll() is None
                for role in self.artifact_roles()
            ) or (self.relay is not None and self.relay.poll() is None),
        }
        return errors

    @staticmethod
    def _diagnostic_section(
        label: str, render: Callable[[], str]
    ) -> str:
        try:
            return render()
        except BaseException as exc:
            return (
                f"<{label} unavailable: {type(exc).__name__}: {exc}>"
            )

    def diagnostics(self) -> str:
        parts = [
            f"run root: {self.run_root}",
            "states:",
            self._diagnostic_section(
                "states",
                lambda: json.dumps(
                    self.runtime_states(), indent=2, default=str
                ),
            ),
        ]
        if self.observer:
            parts.extend(
                [
                    "confirmed-hash observer:",
                    self._diagnostic_section(
                        "confirmed-hash observer",
                        lambda: json.dumps(
                            {
                                "successes": self.hash_counts_by_role(),
                                "events": self.observer.events,
                                "unexpected_exceptions": (
                                    self.observer.unexpected_exceptions[-20:]
                                ),
                            },
                            indent=2,
                        ),
                    ),
                ]
            )
        for role in self.artifact_roles():
            name = role.name
            for title, render in (
                (f"{name} caster stdout", lambda r=role: tail(r.stdout_text())),
                (f"{name} caster stderr", lambda r=role: tail(r.stderr_text())),
                (f"{name} client.log", lambda r=role: tail(r.client_text())),
                (f"{name} dll.log", lambda r=role: tail(r.dll_text(), 180)),
            ):
                parts.append(f"===== {title} =====")
                parts.append(self._diagnostic_section(title, render))
        for stream in ("stdout", "stderr"):
            parts.append(f"===== relay {stream} =====")
            parts.append(
                self._diagnostic_section(
                    f"relay {stream}",
                    lambda s=stream: tail(
                        read_text(self.run_root / f"relay.{s}.log")
                    ),
                )
            )
        return "\n".join(parts)

    def _finalize_evidence_ledger(self) -> None:
        if not self.distributed_mode:
            return
        ledger: dict[str, object] = {}
        starved: list[str] = []
        for role in self.runtime_roles():
            if role.mirror is None:
                continue
            entry = role.mirror.ledger()
            ledger[role.name] = entry
            dll_log = role.latest_log("dll.log")
            entry["dll_log"] = None if dll_log is None else str(dll_log)
            entry["dll_log_bytes"] = (
                0 if dll_log is None else dll_log.stat().st_size
            )
            if not entry["grew"] or not entry["dll_log_bytes"]:
                starved.append(role.name)
                archives = role.host.glob(role.root / "logs", "session_*.zip")
                entry["session_archives"] = [str(item) for item in archives]
        self.report["evidence"] = ledger
        self._distributed_report["evidence"] = ledger
        self.report["distributed"] = self._distributed_report
        if starved:
            raise HarnessFailure(
                f"remote evidence never arrived for {starved}: their dll.log "
                "mirrors are empty or never grew during the run, so every "
                "'forbidden marker present' assertion passed vacuously and "
                f"this run proves nothing ({ledger})"
            )

    def run(self) -> int:
        self.started_mono = time.monotonic()
        error: BaseException | None = None
        force_cleanup_games = False
        try:
            self.prepare()
            self.start_processes()
            self.discover_games()
            if (
                self.args.spectator_join_after_seconds <= 0.0
                and self.args.spectator_postgame_retain_ms == 0
            ):
                self.start_spectator()
            self.wait_for_charselect()
            if self.args.full_match_rematch:
                self.wait_for_scene2(timeout=70.0)
            elif self.args.spectator:
                self.wait_for_scene2(timeout=70.0)
                self.event(
                    "confirmed-input-charselect-transition",
                    states=self.current_states(),
                    mode="spectator-confirmed-transition",
                    spectator_started=self.spectator_is_live(),
                    late_join=(
                        self.args.spectator_join_after_seconds > 0.0
                    ),
                )
            else:
                self.force_verified_match_transition()
            if self.args.layout_command_mutation == "active-p2":
                force_cleanup_games = True
                self.wait_for_expected_layout_mismatch()
                self.report["final_live_states"] = self.runtime_states()
                self.report["confirmed_hash_receipts"] = (
                    self.hash_counts_by_role()
                )
            else:
                self.wait_for_scene2()
                self.attach_hash_observer()
                self.wait_for_layout_and_gameplay()
                if self.args.spectator_count > 1:
                    self.start_additional_spectators()
                if self.args.full_match_rematch:
                    self.wait_for_native_match_and_rematch()
                else:
                    self.wait_for_gameplay_ticks(
                        timeout=max(
                            55.0,
                            self.args.gameplay_ticks / 60.0 * 1.8
                            + self.args.spectator_join_after_seconds
                            + 45.0,
                        )
                    )
                self.run_spectator_capacity_readmit_gate()
                if not self.args.spectator_postgame_retain_ms:
                    self.wait_for_all_spectator_playback()
                    self.wait_for_spectator_f1_survival()
                    self.exercise_spectator_active_catchup()
                    self.exercise_spectator_sibling_disconnect()
                self.refresh_evidence(force=True)
                self.validate_logs_and_route()
                self.validate_runtime_delay_logs()
                self.validate_spectator_unavailable()
                if not self.args.spectator_postgame_retain_ms:
                    self.validate_session_names()
                    self.validate_all_spectator_logs()
                self.validate_full_match_logs(after_shutdown=False)
                self.validate_network_impairment_logs(
                    after_shutdown=False
                )
                self.validate_config_stress_runtime_logs()
                self.validate_layout_command_mutation()
                self.request_spectator_pacing_summary()
                self.report["final_live_states"] = self.runtime_states()
                self.report["confirmed_hash_receipts"] = (
                    self.hash_counts_by_role()
                )
                if self.args.spectator_postgame_retain_ms:
                    retention_remaining_ms = (
                        self.close_players_for_retained_join()
                    )
                    viewer_start_timeout = max(
                        8.0,
                        min(
                            45.0,
                            retention_remaining_ms / 1000.0 - 8.0,
                        ),
                    )
                    self.start_spectator(timeout=viewer_start_timeout)
                    self.wait_for_retained_spectator_playback(
                        timeout=min(
                            45.0,
                            max(
                                8.0,
                                retention_remaining_ms / 1000.0,
                            ),
                        )
                    )
                    self.validate_session_names()
                    self.validate_all_spectator_logs()
                    self.close_retained_viewer_and_wait_for_host()
                    self.refresh_evidence(force=True)
                    self.validate_replay_artifacts()
                    self.validate_config_persistence()
                    self.validate_logs_and_route()
                    self.validate_session_names()
                    self.validate_all_spectator_logs(
                        after_shutdown=True
                    )
                    self.validate_network_impairment_logs(
                        after_shutdown=True
                    )
                else:
                    self.close_games_gracefully()
                    self.refresh_evidence(force=True)
                    self.validate_replay_artifacts()
                    self.validate_config_persistence()
                    self.validate_logs_and_route()
                    self.validate_session_names()
                    self.validate_runtime_delay_logs()
                    self.validate_all_spectator_logs(
                        after_shutdown=True
                    )
                    self.validate_full_match_logs(after_shutdown=True)
                    self.validate_network_impairment_logs(
                        after_shutdown=True
                    )
        except BaseException as exc:
            error = exc
        finally:
            try:
                self.report.setdefault(
                    "final_live_states", self.runtime_states()
                )
                self.report.setdefault(
                    "confirmed_hash_receipts",
                    self.hash_counts_by_role()
                )
            except Exception:
                pass
            try:
                self._finalize_evidence_ledger()
            except BaseException as exc:
                if error is None:
                    error = exc
            cleanup_errors = self.cleanup(
                force_games=error is not None or force_cleanup_games
            )
            if cleanup_errors:
                self.report["cleanup_secondary_errors"] = cleanup_errors
                if error is None:
                    error = HarnessFailure(
                        "task-owned process/resource cleanup reported errors: "
                        + "; ".join(cleanup_errors)
                    )

        self.report["process_exit_information"] = (
            self.process_exit_information()
        )

        def write_compact_report(current_error: BaseException | None) -> str:
            self.report["finished"] = datetime.now().isoformat()
            self.report["elapsed_seconds"] = round(
                time.monotonic() - self.started_mono, 3
            )
            self.report["outcome"] = (
                "passed" if current_error is None else "failed"
            )
            if current_error is not None:
                self.report["error"] = (
                    f"{type(current_error).__name__}: {current_error}"
                )
                self.report["traceback"] = "".join(
                    traceback.format_exception(current_error)
                )
            else:
                self.report.pop("error", None)
                self.report.pop("traceback", None)
            diagnostic_text = self.diagnostics()
            if self.run_root.is_dir():
                (self.run_root / "report.json").write_text(
                    json.dumps(self.report, indent=2, ensure_ascii=False),
                    encoding="utf-8",
                )
                (self.run_root / "diagnostics.txt").write_text(
                    diagnostic_text, encoding="utf-8"
                )
            return diagnostic_text

        diagnostic_text = write_compact_report(error)
        finalization_errors: list[str] = []
        try:
            finalization_errors = self.finalize_disposable_artifacts(
                failed=error is not None
            )
        except BaseException as exc:
            finalization_errors = [
                f"artifact finalizer: {type(exc).__name__}: {exc}"
            ]
        if finalization_errors:
            self.report["artifact_finalization_errors"] = finalization_errors
            if error is None:
                error = HarnessFailure(
                    "disposable artifact finalization failed: "
                    + "; ".join(finalization_errors)
                )
            diagnostic_text = write_compact_report(error)
        elif self.run_root.is_dir():
            diagnostic_text = write_compact_report(error)

        if error is not None:
            artifact_location = (
                str(self.run_root)
                if self.run_root.is_dir()
                else "not created (startup prerequisite failed)"
            )
            print(
                f"\nLIVE GAME E2E FAILED: {type(error).__name__}: {error}\n"
                f"artifacts: {artifact_location}\n{diagnostic_text}",
                file=sys.stderr,
                flush=True,
            )
            return 1
        print(
            f"live {self.args.route}/{self.args.config_variant} game E2E "
            f"passed (layout-command-mutation="
            f"{self.args.layout_command_mutation}); compact artifacts: "
            f"{self.run_root}",
            flush=True,
        )
        return 0

def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--route", choices=("punched-direct", "relay"), required=True
    )
    parser.add_argument(
        "--config-variant",
        choices=("identical", "bak", "config-stress"),
        default="identical",
    )
    parser.add_argument(
        "--prune-on-pass",
        action="store_true",
        help=(
            "legacy compatibility flag; full disposable game clones are now "
            "removed after both passing and ordinary failing runs while "
            "compact diagnostics remain"
        ),
    )
    parser.add_argument(
        "--keep-workdir-on-failure",
        action="store_true",
        help=(
            "retain a complete marker-owned disposable clone only for a "
            "failing run, subject to --keep-workdir-limit"
        ),
    )
    parser.add_argument(
        "--keep-workdir-limit",
        type=int,
        default=1,
        help=(
            "maximum number of complete marker-owned failed run clones to "
            "retain under --output-root (default: 1)"
        ),
    )
    parser.add_argument(
        "--visible-games",
        action="store_true",
        help=(
            "debugging only: run the host/client games on the interactive "
            "desktop instead of the default hidden desktop; automated runs "
            "must stay hidden so they never flash fullscreen windows or "
            "steal focus"
        ),
    )
    parser.add_argument(
        "--layout-command-mutation",
        choices=("none", "inactive-p4", "active-p2"),
        default="none",
        help=(
            "set the client-only pre-fingerprint command-history test hook; "
            "inactive-p4 must remain compatible, active-p2 must be rejected"
        ),
    )
    parser.add_argument("--caster", required=True)
    parser.add_argument(
        "--relay",
        required=True,
        help=(
            "executable launched as `<exe> relay --bind 127.0.0.1 --port N`; "
            "the relay is a caster verb, so this is normally the same "
            "qoh99caster.exe passed to --caster"
        ),
    )
    parser.add_argument("--game", required=True)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--dll-map", required=True)
    parser.add_argument(
        "--wrapper",
        required=True,
        help=(
            "exact built ddraw.dll artifact copied into every isolated game "
            "install"
        ),
    )
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--gameplay-ticks", type=int, default=300)
    parser.add_argument(
        "--spectator-join-after-seconds",
        type=float,
        default=0.0,
        help=(
            "hold the spectator back this many seconds into live gameplay "
            "before launching it, so the viewer joins a match already in "
            "progress. Zero (default) keeps the historical behaviour of "
            "starting the viewer before character select, which only ever "
            "exercises EARLY join"
        ),
    )
    parser.add_argument(
        "--full-match-rematch",
        action="store_true",
        help=(
            "drive ordinary synchronized character-select/combat/KO/result "
            "inputs through a complete first-to-two match, then require "
            "native character-select return and a verified rematch"
        ),
    )
    parser.add_argument(
        "--force-ko-after-frames",
        type=int,
        default=360,
        help=(
            "test-only same-canonical-frame KO fallback after real combat "
            "has had this many gameplay frames; 0 disables both the low-life "
            "schedule and fallback so combat is entirely natural"
        ),
    )
    parser.add_argument(
        "--p1-character-alternate",
        type=int,
        default=0,
        help=(
            "test-only P1 D/Stand modifier (0 or 1). It is the only way to "
            "reach the ten alternate ids: c_serika is Serika's cell (10) with "
            "this set, not a cursor of her own"
        ),
    )
    parser.add_argument(
        "--p2-character-alternate",
        type=int,
        default=0,
        help="test-only P2 D/Stand modifier (0 or 1)",
    )
    parser.add_argument(
        "--strip-character-voices",
        default=None,
        help=(
            "delete every character voice .wav from this role's cloned "
            "install, so the two peers differ exactly the way two real "
            "installations do; the simulation must not notice"
        ),
    )
    parser.add_argument(
        "--p1-character-cursor",
        type=int,
        default=None,
        help=(
            "test-only P1 character cursor slot forced identically in players "
            "and spectator before native synchronized confirmation"
        ),
    )
    parser.add_argument(
        "--p2-character-cursor",
        type=int,
        default=None,
        help=(
            "test-only P2 character cursor slot forced identically in players "
            "and spectator before native synchronized confirmation"
        ),
    )
    parser.add_argument(
        "--rematch-gameplay-ticks",
        type=int,
        default=45,
        help="confirmed native-rematch gameplay frames required",
    )
    parser.add_argument(
        "--require-character-switch",
        action="store_true",
        help=(
            "require a fresh character pair every game; only the dedicated "
            "multi-game regression steers cursors, so tests that hold the "
            "automatch driver disabled across a rematch keep their picks"
        ),
    )
    parser.add_argument(
        "--game-count",
        type=int,
        default=2,
        help=(
            "full native games (matches, not rounds) required in one "
            "session; each rematch character select must land on a fresh "
            "character pair via real cursor-movement taps"
        ),
    )
    parser.add_argument(
        "--returned-charselect-idle-ticks",
        type=int,
        default=0,
        help=(
            "enable the stable-high or stable-120 d2/r8 transition "
            "regression and "
            "hold the first returned, unconfirmed character-select state "
            "for this many canonical ticks before allowing rematch"
        ),
    )
    parser.add_argument(
        "--minimum-canonical-fps",
        type=float,
        default=0.0,
        help=(
            "when positive, require both initial gameplay and the "
            "results/character-select/rematch transition to sustain at least "
            "this canonical-frame rate"
        ),
    )
    parser.add_argument(
        "--network-profile",
        choices=(
            "none",
            "stable-low",
            "stable-120",
            "stable-high",
            "stable-250",
            "stable-300",
            "jitter-low",
            "jitter-high",
            "packet-loss",
            "asym-updown",
            "asym-downup",
        ),
        default="none",
        help="exact-opt-in raw-UDP live impairment profile",
    )
    parser.add_argument(
        "--expect-asymmetric-path",
        choices=("updown", "downup"),
        default=None,
        help=(
            "require an asymmetric one-way profile to equalize into ONE "
            "bounded, mutually consistent phase: fresh frontier samples, real "
            "transit compensation, mirrored frame-advantage estimates, and "
            "neither peer outside the regulator's bound. One-way skew is not "
            "identifiable from round-trip evidence, so the split's direction "
            "is deliberately NOT asserted (updown = host->client slow, downup "
            "= the mirror; both orientations must hold)"
        ),
    )
    parser.add_argument(
        "--expect-underbuffered",
        action="store_true",
        help=(
            "negative control: REQUIRE both casters' measured-route "
            "recommendation to classify the players' explicit settings as "
            "underbuffered, and exempt this run from full-speed floors; the "
            "run must still finish without a desync, cap violation, or "
            "terminal error"
        ),
    )
    parser.add_argument(
        "--network-seed",
        type=lambda text: int(text, 0),
        default=0x51A7E5EE,
        help="base seed for isolated per-role/per-domain packet streams",
    )
    parser.add_argument("--host-delay", type=int, default=1)
    parser.add_argument("--host-rollback", type=int, default=8)
    parser.add_argument("--client-delay", type=int, default=1)
    parser.add_argument("--client-rollback", type=int, default=8)
    parser.add_argument(
        "--host-delay-script",
        default="",
        help=(
            "test-only gameplayTick:delay pairs for P1, e.g. 30:6,120:1"
        ),
    )
    parser.add_argument(
        "--client-delay-script",
        default="",
        help=(
            "test-only gameplayTick:delay pairs for P2, e.g. 45:0,135:4"
        ),
    )
    parser.add_argument(
        "--game-port",
        type=int,
        default=0,
        help="explicit player UDP/TCP port (zero selects a free block)",
    )
    parser.add_argument(
        "--relay-port",
        type=int,
        default=0,
        help="explicit relay UDP/TCP port (zero selects a free port)",
    )
    parser.add_argument(
        "--spectator",
        action="store_true",
        help=(
            "launch a third production caster with the host player ip:port; "
            "require authenticated address discovery, proved-socket handoff, "
            "viewer admission, and live receive-only gameplay"
        ),
    )
    parser.add_argument(
        "--spectator-latest",
        action="store_true",
        help="viewer joins at the newest checkpoint (--latest)",
    )
    parser.add_argument(
        "--f1-return-at-frame",
        type=int,
        default=None,
        help=(
            "host originates one F1 return-to-character-select at this "
            "canonical frame; with a spectator, the viewer must follow the "
            "transition in lockstep and reach the next game's gameplay"
        ),
    )
    parser.add_argument(
        "--spectator-topology",
        action="store_true",
        help=(
            "explicitly opt the one forced-direct spectator into the released "
            "topology proof/QSR depth-one leaf smoke"
        ),
    )
    parser.add_argument(
        "--spectator-count",
        type=int,
        default=1,
        help=(
            "number of physically independent production spectator "
            "casters/games to run simultaneously (1..4)"
        ),
    )
    parser.add_argument(
        "--spectator-route",
        choices=("auto", "direct", "punch", "relay"),
        default="auto",
        help=(
            "spectator-only diagnostic route selector passed to the "
            "production caster; auto remains the shipped direct-first policy"
        ),
    )
    parser.add_argument(
        "--spectator-routes",
        default="",
        help=(
            "test-only comma-separated per-viewer spectator selectors; "
            "requires one selector per simultaneous viewer and leaves the "
            "global --spectator-route at auto"
        ),
    )
    parser.add_argument(
        "--spectator-negative",
        choices=(
            "none",
            "unavailable",
            "unavailable-stale-artifact",
            "unavailable-partial",
        ),
        default="none",
        help=(
            "caster-level negative gate: an occupied independently selected "
            "source endpoint, a stale-address unavailable retry, or an "
            "after-acquisition startup failure must disable spectators "
            "without harming players"
        ),
    )
    parser.add_argument(
        "--spectator-input-mash",
        action="store_true",
        help=(
            "enable the deterministic logical input verification source in "
            "every viewer and prove MODE_SPECTATOR remains receive-only"
        ),
    )
    parser.add_argument(
        "--spectator-capacity-readmit",
        action="store_true",
        help=(
            "set host capacity to one, prove a concurrent second caster is "
            "definitively denied without launching a game, gracefully close "
            "the first viewer, then reuse the exact player address in a fresh "
            "caster and require immediate authenticated readmission"
        ),
    )
    parser.add_argument(
        "--spectator-postgame-retain-ms",
        type=int,
        default=0,
        help=(
            "test-only host CLI retention window: close both player games, "
            "then launch a fresh production spectator against the original "
            "player address and require complete sealed-archive playback"
        ),
    )
    parser.add_argument(
        "--spectator-active-catchup-pause-seconds",
        type=float,
        default=0.0,
        help=(
            "test-only pause duration for one of two live viewers; require "
            "real confirmed-record catch-up while the other viewer and both "
            "players continue"
        ),
    )
    parser.add_argument(
        "--spectator-sibling-disconnect",
        action="store_true",
        help=(
            "test-only two-viewer fixture: prove a mixed direct/AutoPunch "
            "pair has independent no-starvation progress, then normally "
            "close the AutoPunch viewer while the direct viewer and both "
            "players continue"
        ),
    )
    parser.add_argument(
        "--split-game-affinity",
        action="store_true",
        help=(
            "test-only: place each spawned game on a distinct two-logical-CPU "
            "mask when enough processors are available"
        ),
    )
    parser.add_argument(
        "--role-host",
        action="append",
        default=[],
        metavar="ROLE=SSH-TARGET",
        help=(
            "place one role's files and processes on another machine, e.g. "
            "--role-host client=aquatic@192.168.0.131. Repeatable. Without "
            "it every role runs on this controller exactly as before"
        ),
    )
    parser.add_argument(
        "--placement",
        action="append",
        default=[],
        help=(
            "JSON object mapping role names to full placement records "
            "(target, lab, agent_port, drive, display, source_install, "
            "password); --role-host is the short form of the same thing"
        ),
    )
    parser.add_argument(
        "--agent",
        default="",
        help=(
            "built qoh99_e2e_agent.exe staged to every remote machine; "
            "required only when a role is placed remotely"
        ),
    )
    parser.add_argument(
        "--controller-address",
        default="",
        help=(
            "this controller's LAN address as remote peers must reach it; "
            "used for the relay and AutoPunch endpoints advertised to remote "
            "casters (default: 127.0.0.1, i.e. single-machine)"
        ),
    )
    parser.add_argument(
        "--relay-bind",
        default="",
        help=(
            "bind address for `qoh99caster relay` (default: 127.0.0.1 "
            "locally, 0.0.0.0 when any role is remote)"
        ),
    )
    parser.add_argument(
        "--autopunch-bind",
        default="",
        help=(
            "bind address for the harness AutoPunch assist (default: "
            "127.0.0.1 locally, 0.0.0.0 when any role is remote)"
        ),
    )
    parser.add_argument(
        "--remote-minimum-canonical-fps",
        type=float,
        default=-1.0,
        help=(
            "log-derived visible-cadence floor applied to REMOTE roles, whose "
            "wall-clock phase floors are refused because the controller's "
            "clock plus RPC round-trip would bias measured fps downward. "
            "Defaults to --minimum-canonical-fps, so absent means no change"
        ),
    )
    parser.add_argument(
        "--evidence-staleness-budget",
        type=float,
        default=5.0,
        help=(
            "maximum age of a remote role's mirrored evidence while its "
            "caster is alive; older than this and every 'marker in text' "
            "assertion is refused rather than allowed to pass vacuously"
        ),
    )
    parser.add_argument(
        "--route-p50-agreement-us",
        type=int,
        default=3000,
        help=(
            "maximum prelaunch-vs-runtime route p50 disagreement. The default "
            "is the historical 3 ms, unchanged for every existing "
            "registration; distributed lanes set it explicitly rather than "
            "having it silently widened for everyone"
        ),
    )
    parser.add_argument(
        "--remote-barrier-slack-ms",
        type=int,
        default=0,
        help=(
            "extra time allowed to REACH a barrier when roles are on "
            "different machines. It widens deadlines only and never the "
            "values compared at them (heldFrame == target and gameplay-entry "
            "skew == 0 are untouched); the applied value is published in "
            "report['distributed']['barrier_budget']"
        ),
    )
    args = parser.parse_args(list(argv))
    if not 1 <= args.keep_workdir_limit <= 8:
        parser.error("--keep-workdir-limit must be in [1, 8]")
    raw_spectator_routes = str(args.spectator_routes).strip()
    args.spectator_routes = (
        []
        if not raw_spectator_routes
        else [
            route.strip()
            for route in raw_spectator_routes.split(",")
            if route.strip()
        ]
    )
    if raw_spectator_routes and not args.spectator_routes:
        parser.error("--spectator-routes must contain route selectors")
    invalid_spectator_routes = [
        route
        for route in args.spectator_routes
        if route not in ("auto", "direct", "punch", "relay")
    ]
    if invalid_spectator_routes:
        parser.error(
            "--spectator-routes contains unsupported selector(s): "
            + ",".join(invalid_spectator_routes)
        )
    for attribute in (
        "caster",
        "relay",
        "game",
        "dll",
        "dll_map",
        "wrapper",
    ):
        path = Path(getattr(args, attribute))
        if not path.is_file():
            parser.error(f"--{attribute.replace('_', '-')} does not exist: {path}")
    if args.gameplay_ticks < 300:
        parser.error("--gameplay-ticks must be at least 300")
    if args.force_ko_after_frames != 0 and not (
        120 <= args.force_ko_after_frames <= 3600
    ):
        parser.error(
            "--force-ko-after-frames must be 0 (disabled) or in [120, 3600]"
        )
    if args.strip_character_voices is not None:
        if args.strip_character_voices not in ("host", "client"):
            parser.error(
                "--strip-character-voices must name host or client"
            )
        if args.role_host:
            parser.error(
                "--strip-character-voices cannot be combined with "
                "--role-host: it mutates a cloned install in place"
            )
    if (
        (args.p1_character_cursor is None)
        != (args.p2_character_cursor is None)
    ):
        parser.error(
            "--p1-character-cursor and --p2-character-cursor "
            "must be provided together"
        )
    if args.p1_character_cursor is not None:
        if not 0 <= args.p1_character_cursor <= 23:
            parser.error("--p1-character-cursor must be in [0, 23]")
        if not 0 <= args.p2_character_cursor <= 23:
            parser.error("--p2-character-cursor must be in [0, 23]")
        if not args.full_match_rematch:
            parser.error(
                "forced characters require --full-match-rematch"
            )
    for side in ("p1", "p2"):
        alternate = getattr(args, f"{side}_character_alternate")
        if alternate not in (0, 1):
            parser.error(f"--{side}-character-alternate must be 0 or 1")
        if alternate and getattr(args, f"{side}_character_cursor") is None:
            parser.error(
                f"--{side}-character-alternate needs "
                f"--{side}-character-cursor: the modifier selects an "
                "alternate OF a cell, so the cell has to be named"
            )
    if not 30 <= args.rematch_gameplay_ticks <= 90:
        parser.error("--rematch-gameplay-ticks must be in [30, 90]")
    if (
        args.returned_charselect_idle_ticks != 0
        and not 90 <= args.returned_charselect_idle_ticks <= 600
    ):
        parser.error(
            "--returned-charselect-idle-ticks must be zero or in [90, 600]"
        )
    if not 0.0 <= args.minimum_canonical_fps <= 60.6:
        parser.error("--minimum-canonical-fps must be in [0, 60.6]")
    if args.minimum_canonical_fps > 0 and not args.full_match_rematch:
        parser.error(
            "--minimum-canonical-fps requires --full-match-rematch"
        )
    if not 0 <= args.network_seed <= 0xFFFFFFFF:
        parser.error("--network-seed must fit an unsigned 32-bit value")
    for attribute in (
        "host_delay",
        "host_rollback",
        "client_delay",
        "client_rollback",
    ):
        value = getattr(args, attribute)
        if not 0 <= value <= 15:
            parser.error(
                f"--{attribute.replace('_', '-')} must be in [0, 15]"
            )
    delay_script_pattern = re.compile(
        r"[1-9]\d*:(?:[0-9]|1[0-5])"
        r"(?:,[1-9]\d*:(?:[0-9]|1[0-5]))*"
    )
    for role_name in ("host", "client"):
        script = getattr(args, f"{role_name}_delay_script")
        if not script:
            continue
        if delay_script_pattern.fullmatch(script) is None:
            parser.error(
                f"--{role_name}-delay-script must contain strict "
                "gameplayTick:delay pairs with delay in [0, 15]"
            )
        steps = [
            tuple(int(value) for value in item.split(":", 1))
            for item in script.split(",")
        ]
        if any(
            current[0] <= previous[0]
            for previous, current in zip(steps, steps[1:])
        ):
            parser.error(
                f"--{role_name}-delay-script ticks must be strictly "
                "increasing"
            )
        initial = getattr(args, f"{role_name}_delay")
        values = [initial] + [delay for _tick, delay in steps]
        if any(current == previous for previous, current in zip(
            values, values[1:]
        )):
            parser.error(
                f"--{role_name}-delay-script must change the delay at "
                "every step"
            )
    if (
        args.host_delay_script or args.client_delay_script
    ) and not args.full_match_rematch:
        parser.error(
            "runtime delay scripts require --full-match-rematch"
        )
    if args.returned_charselect_idle_ticks:
        if not args.full_match_rematch:
            parser.error(
                "--returned-charselect-idle-ticks requires "
                "--full-match-rematch"
            )
        if args.network_profile not in ("stable-high", "stable-120"):
            parser.error(
                "--returned-charselect-idle-ticks requires "
                "--network-profile stable-high or stable-120"
            )
        if (
            args.host_delay,
            args.host_rollback,
            args.client_delay,
            args.client_rollback,
        ) != (2, 8, 2, 8):
            parser.error(
                "--returned-charselect-idle-ticks requires exact "
                "underbuffered peer settings d2/r8 on both sides"
            )
        if args.host_delay_script or args.client_delay_script:
            parser.error(
                "--returned-charselect-idle-ticks cannot be combined with "
                "runtime delay scripts"
            )
    for attribute in ("game_port", "relay_port"):
        value = getattr(args, attribute)
        if value != 0 and not 1024 <= value <= 65534:
            parser.error(
                f"--{attribute.replace('_', '-')} must be zero or in "
                "[1024, 65534]"
            )
    if args.full_match_rematch and args.config_variant != "identical":
        parser.error(
            "--full-match-rematch requires --config-variant identical; "
            "the test changes the selected first-to-two setting itself"
        )
    if (
        args.full_match_rematch
        and args.layout_command_mutation != "none"
    ):
        parser.error(
            "--full-match-rematch cannot be combined with a layout-command "
            "mutation"
        )
    if (
        args.game_port
        and args.relay_port
        and abs(args.game_port - args.relay_port) <= 1
    ):
        parser.error(
            "--game-port and --relay-port blocks must not overlap"
        )
    if (
        args.layout_command_mutation != "none"
        and args.config_variant != "identical"
    ):
        parser.error(
            "--layout-command-mutation requires --config-variant identical "
            "so command history is the only intentional difference"
        )
    if (
        args.layout_command_mutation == "active-p2"
        and args.spectator
    ):
        parser.error(
            "--layout-command-mutation active-p2 cannot be combined with "
            "--spectator because the player session is expected to stop at "
            "the layout barrier"
        )
    if not 1 <= args.spectator_count <= 4:
        parser.error("--spectator-count must be in [1, 4]")
    if args.spectator_count != 1 and not args.spectator:
        parser.error("--spectator-count > 1 requires --spectator")
    if args.spectator_route != "auto" and not args.spectator:
        parser.error("--spectator-route requires --spectator")
    if args.spectator_topology and (
        not args.spectator
        or args.spectator_count != 1
        or args.spectator_route != "direct"
        or args.spectator_routes
        or args.spectator_negative != "none"
        or args.spectator_capacity_readmit
        or args.spectator_join_after_seconds > 0.0
        or args.spectator_postgame_retain_ms != 0
        or args.full_match_rematch
    ):
        parser.error(
            "--spectator-topology requires one public, forced-direct, "
            "one-match spectator with no other spectator lifecycle fixture"
        )
    if args.spectator_routes:
        if not args.spectator:
            parser.error("--spectator-routes requires --spectator")
        if len(args.spectator_routes) != args.spectator_count:
            parser.error(
                "--spectator-routes requires exactly one selector per "
                "--spectator-count"
            )
        if args.spectator_route != "auto":
            parser.error(
                "--spectator-routes requires global --spectator-route auto"
            )
        if (
            args.spectator_capacity_readmit
            or args.spectator_negative != "none"
            or args.spectator_join_after_seconds > 0.0
            or args.spectator_postgame_retain_ms != 0
            or args.full_match_rematch
        ):
            parser.error(
                "--spectator-routes is limited to the simultaneous Public "
                "one-match E0 route matrix"
            )
    if args.spectator_input_mash and not args.spectator:
        parser.error("--spectator-input-mash requires --spectator")
    if args.spectator_capacity_readmit and not args.spectator:
        parser.error("--spectator-capacity-readmit requires --spectator")
    if args.spectator_capacity_readmit:
        if args.spectator_count != 1:
            parser.error(
                "--spectator-capacity-readmit requires exactly one active "
                "viewer at a time"
            )
        if args.spectator_route not in ("direct", "relay"):
            parser.error(
                "--spectator-capacity-readmit requires forced direct or "
                "forced relay spectator routing"
            )
        if (
            args.spectator_negative != "none"
            or args.spectator_join_after_seconds > 0.0
            or args.spectator_postgame_retain_ms != 0
            or args.full_match_rematch
        ):
            parser.error(
                "--spectator-capacity-readmit is an isolated one-match Public "
                "gate and cannot be combined with other spectator lifecycle "
                "fixtures"
            )
    if (
        args.spectator_negative.startswith("unavailable")
        and args.spectator
    ):
        parser.error(
            "unavailable spectator-negative fixtures must not launch a spectator"
        )
    if (
        args.spectator_negative != "none"
        and args.spectator_route != "auto"
    ):
        parser.error(
            "negative spectator gates cannot force a successful route"
        )
    if args.spectator_count > 1 and args.full_match_rematch:
        parser.error(
            "the simultaneous multi-viewer gate uses the bounded one-match "
            "path; full-match/rematch multi-viewer lifecycle is registered "
            "separately once per-viewer result retention is product-visible"
        )
    if (
        args.spectator_count > 1
        and args.spectator_join_after_seconds > 0.0
    ):
        parser.error(
            "simultaneous multi-viewer and delayed single-viewer join are "
            "separate gates"
        )
    if args.spectator_active_catchup_pause_seconds:
        if not 1.0 <= args.spectator_active_catchup_pause_seconds <= 14.0:
            parser.error(
                "--spectator-active-catchup-pause-seconds must be in [1, 14]"
            )
        if (
            not args.spectator
            or args.spectator_count != 2
            or args.spectator_route != "direct"
            or args.spectator_routes
            or args.spectator_negative != "none"
            or args.spectator_capacity_readmit
            or args.spectator_join_after_seconds > 0.0
            or args.spectator_postgame_retain_ms != 0
            or args.full_match_rematch
        ):
            parser.error(
                "--spectator-active-catchup-pause-seconds requires exactly "
                "two forced-direct public viewers and no other lifecycle fixture"
            )
    if args.spectator_sibling_disconnect:
        if (
            not args.spectator
            or args.spectator_count != 2
            or args.spectator_route != "auto"
            or args.spectator_routes != ["direct", "punch"]
            or args.spectator_negative != "none"
            or args.spectator_active_catchup_pause_seconds
            or args.spectator_capacity_readmit
            or args.spectator_join_after_seconds > 0.0
            or args.spectator_postgame_retain_ms != 0
            or args.full_match_rematch
        ):
            parser.error(
                "--spectator-sibling-disconnect requires exactly two Public "
                "viewers with --spectator-routes direct,punch and no other "
                "lifecycle fixture"
            )
    if args.spectator_postgame_retain_ms:
        if not 15000 <= args.spectator_postgame_retain_ms <= 120000:
            parser.error(
                "--spectator-postgame-retain-ms must be in [15000, 120000] "
                "so a real caster/game launch cannot race the bounded window"
            )
        if not args.spectator:
            parser.error(
                "--spectator-postgame-retain-ms requires --spectator"
            )
        if args.spectator_count != 1:
            parser.error(
                "--spectator-postgame-retain-ms requires exactly one viewer"
            )
        if args.spectator_negative != "none":
            parser.error(
                "--spectator-postgame-retain-ms cannot be combined with a "
                "negative admission gate"
            )
        if args.spectator_join_after_seconds > 0.0:
            parser.error(
                "--spectator-postgame-retain-ms is itself a delayed join "
                "and cannot be combined with --spectator-join-after-seconds"
            )
        if args.full_match_rematch:
            parser.error(
                "--spectator-postgame-retain-ms uses a bounded active-match "
                "source-close path, not the full match/rematch lifecycle"
            )
        if args.layout_command_mutation != "none":
            parser.error(
                "--spectator-postgame-retain-ms cannot be combined with a "
                "layout-command mutation"
            )
    _validate_distributed_arguments(parser, args)
    return args

def _validate_distributed_arguments(
    parser: argparse.ArgumentParser, args: argparse.Namespace
) -> None:
    try:
        placement = _parse_placement(args)
    except HarnessFailure as exc:
        parser.error(str(exc))
        return
    if not placement:
        return

    if not args.agent:
        parser.error(
            "a remote placement requires --agent naming the built "
            "qoh99_e2e_agent.exe"
        )
    if not args.controller_address:
        parser.error(
            "a remote placement requires --controller-address: remote "
            "casters cannot reach a relay or AutoPunch assist advertised as "
            "127.0.0.1"
        )
    if not args.full_match_rematch:
        parser.error(
            "distributed placement requires --full-match-rematch: the "
            "per-frame confirmed-hash debugger observer has no faithful Wine "
            "equivalent, and only the full-match path derives hash receipts "
            "from DLL log evidence instead"
        )
    if not args.visible_games:
        parser.error(
            "distributed placement requires --visible-games: the hidden "
            "desktop is a machine-local kernel object whose WM_CLOSE path is "
            "not reproducible under Wine (that path stays covered by the "
            "local registrations)"
        )
    if args.split_game_affinity:
        parser.error(
            "--split-game-affinity cannot be combined with a remote "
            "placement: the mask is planned from this controller's CPUs and "
            "means nothing on another machine"
        )
    remote_game_roles: dict[str, list[str]] = {}
    for role_name, record in placement.items():
        remote_game_roles.setdefault(str(record["target"]), []).append(
            role_name
        )
    crowded = {
        target: roles
        for target, roles in remote_game_roles.items()
        if len(roles) > 1
    }
    if crowded:
        parser.error(
            "a remote machine may host at most one game role; "
            f"{crowded} would run several concurrently"
        )
    if not args.game_port:
        parser.error(
            "distributed placement requires an explicit --game-port: the "
            "port must be reserved on the machine that binds it"
        )
    if not args.relay_port:
        parser.error(
            "distributed placement requires an explicit --relay-port"
        )
    if not 0.0 < args.evidence_staleness_budget <= 60.0:
        parser.error(
            "--evidence-staleness-budget must be in (0, 60] seconds"
        )
    if args.remote_barrier_slack_ms < 0:
        parser.error("--remote-barrier-slack-ms must not be negative")
    if args.route_p50_agreement_us <= 0:
        parser.error("--route-p50-agreement-us must be positive")
    if args.remote_minimum_canonical_fps >= 0.0 and not (
        0.0 <= args.remote_minimum_canonical_fps <= 60.6
    ):
        parser.error(
            "--remote-minimum-canonical-fps must be in [0, 60.6]"
        )

def main(argv: Iterable[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
        except (AttributeError, ValueError):
            pass
    args = parse_args(sys.argv[1:] if argv is None else argv)
    return LiveGameHarness(args).run()

if __name__ == "__main__":
    raise SystemExit(main())
