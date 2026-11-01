
from __future__ import annotations

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import traceback
from typing import Callable, Iterable

import live_game_e2e as live

if os.name != "nt":
    raise SystemExit("live_training_e2e.py requires Windows")

HIDDEN_DESKTOP = "qoh99np_hidden"
TRAINING_ACTIVE = 4
TRAINING_ADMITTED = 1
TRAINING_DISABLE_NONE = 0
COMMAND_INGRESS_ACCEPTED = 0
COMMAND_ACK_APPLIED = 1
FIGHTER_SIZE = 0x5FC

COMMAND = {
    "reset_position": 1,
    "save_state": 2,
    "load_state": 3,
    "toggle_record": 6,
    "toggle_playback": 7,
    "stop_macro": 8,
    "set_guard_mode": 11,
    "set_playback_mode": 13,
    "set_hitbox_display": 15,
    "return_to_character_select": 16,
}
POSITION_CENTER = 3
GUARD_OFF = 0
GUARD_ALWAYS_STAND = 2
GUARD_ALWAYS_CROUCH = 3
PLAYBACK_LOOP = 1
MACRO_IDLE = 0
MACRO_RECORDING = 2
MACRO_PLAYING = 5
LOGICAL_DOWN = 0x20000
LOGICAL_LEFT = 0x40000
LOGICAL_RIGHT = 0x80000
LOGICAL_A = 0x00001
LOGICAL_B = 0x00002
LOGICAL_C = 0x00004
LOGICAL_DIRECTION_MASK = 0xF0000

SELECTOR_INPUT_HISTORY = (
    (0x8A65CC, 0x8A654C, 0x8A658C),
    (0x8A6780, 0x8A6700, 0x8A6740),
)

ADDR = {
    **live.GAME_ADDR,
    "p1_input_source": 0x4A37C4,
    "p2_input_source": 0x4A37C8,
    "p2_route": 0x613F14,
    "stage_cursor": 0x6236E8,
    "logical_stage": 0x4ECB1C,
    "local_network_mode": 0x4AAA50,
    "vs_launch": 0x6236B0,
    "active_config_ptr": 0x613DE0,
    "copied_config": 0x623520,
    "pause": 0x62D580,
    "freeze": 0x6267D0,
    "native_fatal_disable": 0x6275A0,
    "result_state": 0x88098C,
    "outcome_p1": 0x625620,
    "outcome_p2": 0x625624,
    "result_progress_p1": 0x625640,
    "result_progress_p2": 0x625644,
    "hitbox": 0x4AA424,
    "camera_x": 0x626E80,
    "camera_y": 0x626E84,
    "camera_previous_x": 0x62D630,
    "fighter_active_table": 0x4A37B4,
    "fighter_mirror_table": 0x623500,
}

ANCHOR_REANCHOR_SLACK = 32

FIGHTER_OFFSET = {
    "x": 136,
    "y": 140,
    "facing": 170,
    "slot": 172,
    "character": 174,
    "previous_x": 1312,
    "round_end": 1364,
    "round_lost": 1368,
    "fatal_cause": 1376,
    "fatal_attacker": 1380,
    "com": 1400,
    "facing_snapshot": 1404,
}

MAILBOX_OFFSET = {
    "command_kind": 0,
    "slot_or_setting": 4,
    "value": 8,
    "flags": 12,
    "gameplay_p1_mask": 16,
    "request_id": 20,
    "ingress_request_id": 24,
    "ingress_result": 28,
    "acknowledged_command_id": 32,
    "acknowledged_result": 36,
    "training_generation": 40,
    "lifecycle_state": 44,
    "final_p1_mask": 48,
    "final_p2_mask": 52,
    "macro_state": 56,
    "macro_cursor": 60,
    "selected_macro_frame_count": 64,
    "hitbox_effective": 68,
    "step_serial_low": 72,
    "step_serial_high": 76,
    "ingress_generation": 80,
    "lethal_damage_request_id": 84,
    "lethal_damage_ack_id": 88,
    "lethal_damage_ack_result": 92,
    "lethal_damage_amount": 96,
    "lethal_damage_detour_count": 100,
    "lethal_damage_generation": 104,
    "lethal_damage_victim_slot": 108,
}
MAILBOX_SIZE = 112

MAP_SYMBOLS = {
    "step_serial": "?g_trainingStepSerial@",
    "last_admission": "?g_trainingLastAdmission@",
    "lifecycle": "?g_trainingLifecycle@",
    "disable_reason": "?g_trainingDisableReason@",
    "mutation_failed": "?g_trainingMutationFailed@",
    "round_character_state": "?g_trainingRoundCharacterState@",
    "sticky_errors": "?g_trainingStickyErrors@",
    "frontend_input_armed": "?g_trainingFrontendInputArmed@",
    "stage_input_armed": "?g_trainingStageInputArmed@",
    "live_input_mask": "?g_trainingLiveE2ELogicalMask@",
    "live_mailbox": "?g_trainingLiveE2EMailbox@",
}

REQUIRED_DLL_MARKERS = (
    "[training] explicit offline training armed",
    "[training] native hooks qualified and installed",
    "[training-ui] provider registered",
    "[boot] explicit TRAINING direct-boot armed",
    "[training-select] held P2 inactive after native same-call handoff",
    "[training-select] native row-1 handoff complete",
    "[training] lifecycle ACTIVE scene=2 sub=0",
)

FORBIDDEN_MARKERS = (
    "[training] FAIL-CLOSED",
    "[training] post-step invariant FAILED",
    "[training-select] FAILED:",
    "[training] native seam qualification FAILED",
    "[training] native hook transaction FAILED",
    "DLL did not signal ready",
    "Launch failed:",
)

def _inside(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False

def _build_ancestor(path: Path) -> Path | None:
    resolved = path.resolve()
    for candidate in (resolved, *resolved.parents):
        if candidate.name.casefold() == "build":
            return candidate
    return None

def _release_artifact(path: Path) -> bool:
    return any(part.casefold() == "release" for part in path.resolve().parts)

def _file_record(path: Path) -> dict[str, object]:
    stat = path.stat()
    return {
        "path": str(path.resolve()),
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "sha256": live.sha256(path),
    }

def _pe_link_metadata(path: Path) -> dict[str, int]:
    image = path.read_bytes()
    if len(image) < 0x100 or image[:2] != b"MZ":
        raise live.HarnessFailure(f"{path} is not a PE image")
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    if (
        pe_offset + 4 + 20 + 60 > len(image)
        or image[pe_offset:pe_offset + 4] != b"PE\0\0"
    ):
        raise live.HarnessFailure(f"{path} has an invalid PE header")
    coff = pe_offset + 4
    timestamp = struct.unpack_from("<I", image, coff + 4)[0]
    optional = coff + 20
    magic = struct.unpack_from("<H", image, optional)[0]
    if magic != 0x10B:
        raise live.HarnessFailure(
            f"{path} is not the required 32-bit PE32 image"
        )
    size_of_image = struct.unpack_from("<I", image, optional + 56)[0]
    if size_of_image == 0:
        raise live.HarnessFailure(f"{path} has zero SizeOfImage")
    return {
        "coff_timestamp": timestamp,
        "size_of_image": size_of_image,
    }

def _parse_unique_map_data_rva(
    map_path: Path,
    symbol_fragment: str,
    expected_timestamp: int,
    size_of_image: int,
) -> int:
    text = live.read_text(map_path)
    timestamp_match = re.search(
        r"^\s*Timestamp is ([0-9A-Fa-f]+)\b", text, re.MULTILINE
    )
    preferred_match = re.search(
        r"Preferred load address is\s+([0-9A-Fa-f]+)", text
    )
    symbol_matches = re.findall(
        r"^\s*[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+\S*"
        + re.escape(symbol_fragment)
        + r"\S*\s+([0-9A-Fa-f]+)\s",
        text,
        re.MULTILINE,
    )
    if timestamp_match is None or preferred_match is None:
        raise live.HarnessFailure(
            f"{map_path} lacks its linker timestamp/load address"
        )
    map_timestamp = int(timestamp_match.group(1), 16)
    if map_timestamp != expected_timestamp:
        raise live.HarnessFailure(
            f"linker map/DLL timestamp mismatch: "
            f"map=0x{map_timestamp:08X} dll=0x{expected_timestamp:08X}"
        )
    if len(symbol_matches) != 1:
        raise live.HarnessFailure(
            f"{map_path} contains {len(symbol_matches)} matches for "
            f"{symbol_fragment!r}; exactly one is required"
        )
    preferred = int(preferred_match.group(1), 16)
    symbol = int(symbol_matches[0], 16)
    rva = symbol - preferred
    if rva <= 0 or rva >= size_of_image:
        raise live.HarnessFailure(
            f"{symbol_fragment} RVA 0x{rva:X} is outside DLL "
            f"SizeOfImage 0x{size_of_image:X}"
        )
    return rva

def _source_guard_paths(source_game: Path) -> tuple[Path, ...]:
    install = source_game.parent
    candidates = (
        source_game,
        install / "System" / "QOHcnf.key",
        install / "LocalConfig" / "FCommand.txt",
        install / "FCommand.txt",
    )
    return tuple(path for path in candidates if path.is_file())

def _guard_snapshot(paths: Iterable[Path]) -> dict[str, dict[str, object]]:
    return {str(path.resolve()): _file_record(path) for path in paths}

def _latest_log(root: Path, filename: str) -> Path | None:
    paths = sorted(
        (root / "logs").glob(f"session_*/{filename}"),
        key=lambda path: path.stat().st_mtime_ns,
    )
    return paths[-1] if paths else None

def _read_u8(memory: live.ProcessMemory, address: int) -> int:
    return memory.read(address, 1)[0]

def _read_u64(memory: live.ProcessMemory, address: int) -> int:
    return struct.unpack("<Q", memory.read(address, 8))[0]

def _write_i32(
    memory: live.ProcessMemory, address: int, value: int
) -> None:
    if not -(1 << 31) <= value < (1 << 31):
        raise live.HarnessFailure(
            f"signed DWORD value is out of range: {value}"
        )
    memory.write(address, struct.pack("<i", value))
    observed = memory.read_i32(address)
    if observed != value:
        raise live.HarnessFailure(
            f"write at 0x{address:X} did not stick: "
            f"expected={value} actual={observed}"
        )

class TrainingHarness:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.started_mono = time.monotonic()
        self.events: list[dict[str, object]] = []
        self.source_game = Path(args.game).resolve()
        self.source_install = self.source_game.parent
        self.source_artifacts = {
            "caster": Path(args.caster).resolve(),
            "dll": Path(args.dll).resolve(),
            "wrapper": Path(args.wrapper).resolve(),
            "dll_map": Path(args.dll_map).resolve(),
        }

        roots = {
            _build_ancestor(path)
            for path in self.source_artifacts.values()
        }
        if None in roots or len(roots) != 1:
            raise live.HarnessFailure(
                "caster, DLL, wrapper, and linker map must all come from "
                "one canonical BUILD tree"
            )
        self.build_root = next(iter(roots))
        assert self.build_root is not None
        non_release = [
            str(path)
            for path in self.source_artifacts.values()
            if not _release_artifact(path)
        ]
        if non_release:
            raise live.HarnessFailure(
                "live training requires exact Release artifacts; non-Release: "
                + ", ".join(non_release)
            )

        output_root = Path(args.output_root).resolve()
        if output_root == self.build_root or not _inside(
            output_root, self.build_root
        ):
            raise live.HarnessFailure(
                f"output root must be a child of canonical BUILD "
                f"{self.build_root}, got {output_root}"
            )
        output_root.mkdir(parents=True, exist_ok=True)
        nonce = int(time.time() * 1000) & 0xFFFFFF
        self.run_root = (
            output_root / f"t_{os.getpid():x}_{nonce:x}"
        ).resolve()
        if self.run_root.exists():
            raise live.HarnessFailure(
                f"refusing to overwrite live-training run {self.run_root}"
            )
        self.run_root.mkdir()
        self.role_root = self.run_root / "r"
        self.role_root.mkdir()
        self.install = self.role_root / "g"
        self.game = self.install / self.source_game.name
        self.caster = self.role_root / "qoh99caster.exe"
        self.dll = self.install / "qoh99_netplay.dll"
        self.wrapper = self.install / "ddraw.dll"
        self.dll_map = self.role_root / "qoh99_netplay.map"
        self.stdout_path = self.role_root / "caster.stdout.log"
        self.stderr_path = self.role_root / "caster.stderr.log"
        self.report_path = self.run_root / "report.json"
        self.stdout_handle: object | None = None
        self.stderr_handle: object | None = None
        self.caster_process: subprocess.Popen[bytes] | None = None
        self.game_memory: live.ProcessMemory | None = None
        self.game_pid: int | None = None
        self.run_deadline: float | None = None
        self.dll_base: int | None = None
        self.symbol_rvas: dict[str, int] = {}
        self.next_command_id = 0x40000000
        self.preexisting_pids = {
            pid for pid, _ppid, _image in live.process_entries()
        }
        self.source_guard_paths = _source_guard_paths(self.source_game)
        self.source_guard_before = _guard_snapshot(
            self.source_guard_paths
        )
        self.clone_config_before: dict[str, object] | None = None
        self.passed = False
        self.report: dict[str, object] = {
            "schema": 1,
            "started": datetime.now().isoformat(),
            "canonical_build_root": str(self.build_root),
            "run_root": str(self.run_root),
            "arguments": {
                "admitted_steps": args.admitted_steps,
                "minimum_admitted_hz": args.minimum_admitted_hz,
                "maximum_admitted_hz": args.maximum_admitted_hz,
                "timeout_seconds": args.timeout,
                "p1_character": args.p1_character,
                "p2_character": args.p2_character,
                "prune_game_clone_on_pass":
                    args.prune_game_clone_on_pass,
            },
            "source_guard": {
                "before": self.source_guard_before,
            },
            "events": self.events,
        }

    def event(self, name: str, **details: object) -> None:
        item = {
            "time": datetime.now().isoformat(),
            "elapsed_seconds": round(
                time.monotonic() - self.started_mono, 3
            ),
            "name": name,
            **details,
        }
        self.events.append(item)
        print(f"[training-e2e] {name}: {details}", flush=True)

    def dll_text(self) -> str:
        path = _latest_log(self.role_root, "dll.log")
        return live.read_text(path) if path else ""

    def combined_text(self) -> str:
        return "\n".join(
            (
                self.dll_text(),
                live.read_text(self.stdout_path),
                live.read_text(self.stderr_path),
            )
        )

    def prepare(self) -> None:
        required = (
            self.source_game,
            self.source_install / "System" / "QOHcnf.key",
            self.source_install / "LocalConfig" / "FCommand.txt",
            *self.source_artifacts.values(),
        )
        missing = [str(path) for path in required if not path.is_file()]
        if missing:
            raise live.HarnessFailure(
                "live training inputs are incomplete: "
                + ", ".join(missing)
            )
        if _inside(self.source_install, self.run_root) or _inside(
            self.run_root, self.source_install
        ):
            raise live.HarnessFailure(
                "source install and owned clone root must not overlap"
            )

        clone_stats = live.clone_game_install(
            self.source_install, self.install
        )
        shutil.copy2(self.source_artifacts["caster"], self.caster)
        shutil.copy2(self.source_artifacts["dll"], self.dll)
        shutil.copy2(self.source_artifacts["wrapper"], self.wrapper)
        shutil.copy2(self.source_artifacts["dll_map"], self.dll_map)
        audio = live.mute_test_install_audio(self.install)
        asset_paths = live.validate_runtime_asset_paths(self.install)

        installed = {
            "caster": self.caster,
            "dll": self.dll,
            "wrapper": self.wrapper,
            "dll_map": self.dll_map,
        }
        artifact_report: dict[str, object] = {}
        for name, source in self.source_artifacts.items():
            source_record = _file_record(source)
            installed_record = _file_record(installed[name])
            if source_record["sha256"] != installed_record["sha256"]:
                raise live.HarnessFailure(
                    f"copied {name} differs from requested Release artifact"
                )
            artifact_report[name] = {
                "source": source_record,
                "installed": installed_record,
                "exact_copy": True,
            }

        pe_metadata = _pe_link_metadata(self.dll)
        self.symbol_rvas = {
            name: _parse_unique_map_data_rva(
                self.dll_map,
                fragment,
                pe_metadata["coff_timestamp"],
                pe_metadata["size_of_image"],
            )
            for name, fragment in MAP_SYMBOLS.items()
        }
        self.clone_config_before = live.qoh_selected_profile_snapshot(
            self.install / "System" / "QOHcnf.key",
            (36, 80, 84, 88, live.QOH_CONFIG_NOSOUND_OFFSET),
        )
        self.report.update(
            {
                "clone": {
                    "path": str(self.install),
                    "stats": clone_stats,
                    "asset_path_preflight": asset_paths,
                    "audio": audio,
                },
                "artifacts": artifact_report,
                "training_symbol_rvas": {
                    name: f"0x{rva:X}"
                    for name, rva in self.symbol_rvas.items()
                },
                "dll_pe_metadata": {
                    "coff_timestamp":
                        f"0x{pe_metadata['coff_timestamp']:08X}",
                    "size_of_image":
                        f"0x{pe_metadata['size_of_image']:X}",
                    "map_timestamp_matched": True,
                    "symbols_unique_and_in_image": True,
                },
                "clone_config_before": self.clone_config_before,
            }
        )
        self.event(
            "prepared",
            clone=str(self.install),
            copied=clone_stats["copied"],
            skipped=clone_stats["skipped"],
            audio_muted=audio["verified"],
            symbols=self.report["training_symbol_rvas"],
        )

    def launch(self) -> None:
        environment = os.environ.copy()
        for name in tuple(environment):
            if name.startswith("QOH99_NP_TEST_"):
                environment.pop(name, None)
        environment["QOH99_TRAINING_LIVE_E2E"] = "1"
        environment["QOH99_LOG"] = "1"
        command = [
            str(self.caster),
            "offline",
            "--mode",
            "training",
            "--hidden-desktop",
            "--game",
            str(self.game),
            "--dll",
            str(self.dll),
        ]
        self.stdout_handle = self.stdout_path.open("wb")
        self.stderr_handle = self.stderr_path.open("wb")
        self.caster_process = subprocess.Popen(
            command,
            cwd=self.role_root,
            stdout=self.stdout_handle,
            stderr=self.stderr_handle,
            env=environment,
            creationflags=live.CREATE_NO_WINDOW,
        )
        self.run_deadline = time.monotonic() + self.args.timeout
        self.report["command"] = command
        self.report["caster_pid"] = self.caster_process.pid
        self.event(
            "caster-started",
            pid=self.caster_process.pid,
            command=command,
        )

    def scan_failures(self) -> None:
        if self.caster_process is not None:
            code = self.caster_process.poll()
            if code is not None and (
                self.game_memory is None
                or not self.game_memory.alive()
            ):
                raise live.HarnessFailure(
                    f"caster/game exited unexpectedly with caster code {code}"
                )
        if self.game_memory is not None and not self.game_memory.alive():
            raise live.HarnessFailure(
                f"owned qoh99 pid={self.game_memory.pid} exited unexpectedly"
            )
        evidence = self.combined_text()
        for marker in FORBIDDEN_MARKERS:
            if marker in evidence:
                raise live.HarnessFailure(
                    f"training failure marker observed: {marker}"
                )

    def wait_until(
        self,
        label: str,
        predicate: Callable[[], object | None],
        timeout: float | None = None,
        diagnostic: Callable[[], object] | None = None,
    ) -> object:
        stage_deadline = time.monotonic() + (
            self.args.timeout if timeout is None else timeout
        )
        deadline = (
            min(stage_deadline, self.run_deadline)
            if self.run_deadline is not None
            else stage_deadline
        )
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            self.scan_failures()
            try:
                result = predicate()
                if result is not None and result is not False:
                    return result
            except OSError as error:
                last_error = error
            time.sleep(0.01)
        details: list[str] = []
        if last_error is not None:
            details.append(f"last read error={last_error}")
        if diagnostic is not None:
            try:
                details.append(
                    "last observed="
                    + json.dumps(diagnostic(), sort_keys=True)
                )
            except Exception as error:
                details.append(f"diagnostic read failed={error}")
        suffix = "; " + "; ".join(details) if details else ""
        raise live.HarnessFailure(f"timed out waiting for {label}{suffix}")

    def discover_game(self) -> None:
        assert self.caster_process is not None

        def find_child() -> int | None:
            pid = live.child_game_pid(
                self.caster_process.pid, self.game.name
            )
            if pid is None:
                return None
            if pid in self.preexisting_pids:
                raise live.HarnessFailure(
                    f"refusing pre-existing qoh99 pid={pid}"
                )
            return pid

        pid = int(self.wait_until("owned qoh99 child", find_child))
        self.game_pid = pid
        self.game_memory = live.ProcessMemory(pid)
        self.report["game_pid"] = pid

        def verify_image() -> bool:
            module = live.module_info(pid, self.game.name)
            if module is None:
                return False
            actual = Path(module[2]).resolve()
            if os.path.normcase(str(actual)) != os.path.normcase(
                str(self.game.resolve())
            ):
                raise live.HarnessFailure(
                    f"child pid={pid} image is not owned clone: "
                    f"expected={self.game} actual={actual}"
                )
            return True

        self.wait_until("owned qoh99 image verification", verify_image)
        self.event("game-discovered", pid=pid, image=str(self.game))

        def injected() -> bool:
            assert self.game_memory is not None
            dll_module = live.module_info(pid, self.dll.name)
            wrapper_module = live.module_info(pid, self.wrapper.name)
            if dll_module is None or wrapper_module is None:
                return False
            actual_dll = Path(dll_module[2]).resolve()
            actual_wrapper = Path(wrapper_module[2]).resolve()
            for expected, actual, label in (
                (self.dll, actual_dll, "DLL"),
                (self.wrapper, actual_wrapper, "wrapper"),
            ):
                if os.path.normcase(str(expected.resolve())) != os.path.normcase(
                    str(actual)
                ):
                    raise live.HarnessFailure(
                        f"loaded {label} is not owned Release copy: "
                        f"expected={expected} actual={actual}"
                    )
            self.dll_base = int(dll_module[0])
            return True

        self.wait_until("owned DLL and wrapper modules", injected)
        self.report["dll_base"] = f"0x{self.dll_base:X}"
        self.event(
            "runtime-modules-verified",
            dll_base=self.report["dll_base"],
            dll=str(self.dll),
            wrapper=str(self.wrapper),
        )

    def runtime_address(self, symbol: str) -> int:
        if self.dll_base is None:
            raise live.HarnessFailure("training DLL base is unavailable")
        return self.dll_base + self.symbol_rvas[symbol]

    def runtime_u32(self, symbol: str) -> int:
        assert self.game_memory is not None
        return self.game_memory.read_u32(self.runtime_address(symbol))

    def runtime_u64(self, symbol: str) -> int:
        assert self.game_memory is not None
        return _read_u64(self.game_memory, self.runtime_address(symbol))

    def runtime_bool(self, symbol: str) -> bool:
        assert self.game_memory is not None
        return _read_u8(
            self.game_memory, self.runtime_address(symbol)
        ) != 0

    def set_live_input_mask(self, value: int) -> None:
        if value & ~0xF000F:
            raise live.HarnessFailure(
                f"live training input mask is non-canonical: 0x{value:X}"
            )
        if value != 0 and self.runtime_u32("frontend_input_armed") != 1:
            raise live.HarnessFailure(
                "refusing to pulse live input before the managed frontend "
                "gate is armed"
            )
        assert self.game_memory is not None
        address = self.runtime_address("live_input_mask")
        self.game_memory.write_u32(address, value)
        observed = self.game_memory.read_u32(address)
        if observed != value:
            raise live.HarnessFailure(
                f"live input write did not stick: expected={value} "
                f"actual={observed}"
            )

    def live_mailbox(self) -> dict[str, int]:
        assert self.game_memory is not None
        base = self.runtime_address("live_mailbox")
        raw = self.game_memory.read(base, MAILBOX_SIZE)
        return {
            field: struct.unpack_from("<I", raw, offset)[0]
            for field, offset in MAILBOX_OFFSET.items()
        }

    def set_live_gameplay_p1_mask(self, value: int) -> None:
        if value & ~0xF000F:
            raise live.HarnessFailure(
                f"non-canonical live gameplay mask 0x{value:X}"
            )
        assert self.game_memory is not None
        address = (
            self.runtime_address("live_mailbox")
            + MAILBOX_OFFSET["gameplay_p1_mask"]
        )
        self.game_memory.write_u32(address, value)
        observed = self.game_memory.read_u32(address)
        if observed != value:
            raise live.HarnessFailure(
                f"live gameplay mask write did not stick: "
                f"expected=0x{value:X} actual=0x{observed:X}"
            )

    def send_training_command(
        self,
        label: str,
        command_kind: int,
        *,
        slot_or_setting: int = 0,
        value: int = 0,
        flags: int = 0,
        timeout: float = 5.0,
    ) -> dict[str, object]:
        assert self.game_memory is not None
        memory = self.game_memory
        command_id = self.next_command_id
        self.next_command_id += 1
        if self.next_command_id > 0xFFFFFFFF:
            raise live.HarnessFailure("live command ID wrapped")

        base = self.runtime_address("live_mailbox")
        memory.suspend()
        try:
            state = self.invariant_snapshot()
            if (
                state["scene"] != 2
                or state["match_sub"] != 0
                or self.runtime_u32("lifecycle") != TRAINING_ACTIVE
            ):
                raise live.HarnessFailure(
                    f"refusing {label} outside active Training gameplay: "
                    + json.dumps(state, sort_keys=True)
                )
            memory.write_u32(
                base + MAILBOX_OFFSET["request_id"], 0
            )
            payload = {
                "command_kind": command_kind,
                "slot_or_setting": slot_or_setting,
                "value": value,
                "flags": flags,
            }
            for field, field_value in payload.items():
                if not 0 <= field_value <= 0xFFFFFFFF:
                    raise live.HarnessFailure(
                        f"{label} {field} is outside u32: {field_value}"
                    )
                memory.write_u32(
                    base + MAILBOX_OFFSET[field], field_value
                )
            for field, expected in payload.items():
                actual = memory.read_u32(
                    base + MAILBOX_OFFSET[field]
                )
                if actual != expected:
                    raise live.HarnessFailure(
                        f"{label} payload readback failed for {field}: "
                        f"expected={expected} actual={actual}"
                    )
            memory.write_u32(
                base + MAILBOX_OFFSET["request_id"], command_id
            )
            if (
                memory.read_u32(
                    base + MAILBOX_OFFSET["request_id"]
                )
                != command_id
            ):
                raise live.HarnessFailure(
                    f"{label} request publication did not stick"
                )
        finally:
            memory.resume()

        def ingress() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            if mailbox["ingress_request_id"] != command_id:
                return None
            if mailbox["ingress_result"] != COMMAND_INGRESS_ACCEPTED:
                raise live.HarnessFailure(
                    f"{label} queue ingress rejected request {command_id}: "
                    + json.dumps(mailbox, sort_keys=True)
                )
            return mailbox

        ingress_state = self.wait_until(
            f"{label} command queue ingress",
            ingress,
            timeout=timeout,
            diagnostic=self.live_mailbox,
        )

        def acknowledged() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            if mailbox["acknowledged_command_id"] != command_id:
                return None
            if mailbox["acknowledged_result"] != COMMAND_ACK_APPLIED:
                raise live.HarnessFailure(
                    f"{label} command {command_id} was not applied: "
                    + json.dumps(mailbox, sort_keys=True)
                )
            return mailbox

        ack_state = self.wait_until(
            f"{label} command acknowledgement",
            acknowledged,
            timeout=timeout,
            diagnostic=self.live_mailbox,
        )
        record: dict[str, object] = {
            "label": label,
            "command_id": command_id,
            "command_kind": command_kind,
            "slot_or_setting": slot_or_setting,
            "value": value,
            "flags": flags,
            "published_last_while_suspended": True,
            "ingress": ingress_state,
            "acknowledgement": ack_state,
        }
        self.event(
            "training-command-applied",
            label=label,
            command_id=command_id,
            command_kind=command_kind,
        )
        return record

    def wait_for_active_training(
        self, label: str, timeout: float = 15.0
    ) -> dict[str, int]:
        def active() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            if (
                self.runtime_u32("lifecycle") == TRAINING_ACTIVE
                and self.runtime_u32("disable_reason")
                    == TRAINING_DISABLE_NONE
                and not self.runtime_bool("mutation_failed")
                and self.runtime_bool("round_character_state")
                and self.runtime_u64("step_serial") > 0
                and mailbox["lifecycle_state"] == TRAINING_ACTIVE
            ):
                return mailbox
            return None

        result = self.wait_until(
            label,
            active,
            timeout=timeout,
            diagnostic=self.live_mailbox,
        )
        assert isinstance(result, dict)
        return result

    def run_lethal_damage_detour_probe(self) -> dict[str, object]:
        assert self.game_memory is not None
        memory = self.game_memory
        request_id = self.next_command_id
        self.next_command_id += 1
        before_mailbox = self.live_mailbox()
        before_invariants = self.invariant_snapshot()
        before_fighter = self.fighter_snapshot()[1]
        before_life = dict(before_invariants["life"]["p2"])
        if int(before_life["effective"]) < 0:
            raise live.HarnessFailure(
                "lethal detour probe began with negative P2 life"
            )
        fatal_keys = (
            "active_fatal_cause",
            "active_fatal_attacker",
            "mirror_fatal_cause",
            "mirror_fatal_attacker",
        )
        before_fatal = {
            key: int(before_fighter[key]) for key in fatal_keys
        }

        base = self.runtime_address("live_mailbox")
        request_address = (
            base + MAILBOX_OFFSET["lethal_damage_request_id"]
        )
        memory.suspend()
        try:
            state = self.invariant_snapshot()
            if (
                state["scene"] != 2
                or state["match_sub"] != 0
                or self.runtime_u32("lifecycle") != TRAINING_ACTIVE
            ):
                raise live.HarnessFailure(
                    "refusing lethal detour probe outside active Training"
                )
            memory.write_u32(request_address, 0)
            memory.write_u32(request_address, request_id)
            if memory.read_u32(request_address) != request_id:
                raise live.HarnessFailure(
                    "lethal detour request publication did not stick"
                )
        finally:
            memory.resume()

        def acknowledged() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            if mailbox["lethal_damage_ack_id"] != request_id:
                return None
            if mailbox["lethal_damage_ack_result"] != 1:
                raise live.HarnessFailure(
                    "lethal detour probe rejected: "
                    + json.dumps(mailbox, sort_keys=True)
                )
            return mailbox

        ack = self.wait_until(
            "qualified 0x42D760 lethal-damage detour probe",
            acknowledged,
            timeout=5.0,
            diagnostic=self.live_mailbox,
        )
        assert isinstance(ack, dict)
        after_invariants = self.invariant_snapshot()
        after_fighter = self.fighter_snapshot()[1]
        after_fatal = {
            key: int(after_fighter[key]) for key in fatal_keys
        }
        after_life = dict(after_invariants["life"]["p2"])
        damage = int(ack["lethal_damage_amount"])
        if not 1 <= damage <= 1_000_000:
            raise live.HarnessFailure(
                f"lethal detour used out-of-bound damage {damage}"
            )
        if int(after_life["effective"]) != 0:
            raise live.HarnessFailure(
                "lethal detour did not floor P2 effective life to zero: "
                + json.dumps(after_life, sort_keys=True)
            )
        if after_fatal != before_fatal:
            raise live.HarnessFailure(
                "lethal detour did not restore fatal metadata: "
                + json.dumps(
                    {"before": before_fatal, "after": after_fatal},
                    sort_keys=True,
                )
            )
        if (
            int(ack["lethal_damage_detour_count"])
            != int(before_mailbox["lethal_damage_detour_count"]) + 1
            or int(ack["lethal_damage_victim_slot"]) != 1
        ):
            raise live.HarnessFailure(
                "lethal probe did not traverse exactly one P2 detour: "
                + json.dumps(ack, sort_keys=True)
            )

        record: dict[str, object] = {
            "request_id": request_id,
            "published_while_process_suspended": True,
            "entry_address": "0x42D760",
            "victim_slot": 1,
            "before_life": before_life,
            "after_life": after_life,
            "damage": damage,
            "before_fatal": before_fatal,
            "after_fatal": after_fatal,
            "fatal_metadata_restored": after_fatal == before_fatal,
            "detour_count_before":
                before_mailbox["lethal_damage_detour_count"],
            "detour_count_after": ack["lethal_damage_detour_count"],
            "ack": ack,
        }
        self.event(
            "lethal-damage-detour-proven",
            request_id=request_id,
            damage=damage,
            effective_life=after_life["effective"],
        )
        return record

    def run_native_combat_hit_probe(self) -> dict[str, object]:
        before_mailbox = self.live_mailbox()
        before_invariants = self.invariant_snapshot()
        before_life = dict(before_invariants["life"]["p2"])
        before_serial = self.runtime_u64("step_serial")
        before_count = int(
            before_mailbox["lethal_damage_detour_count"]
        )
        attempts: list[dict[str, object]] = []

        self.set_live_gameplay_p1_mask(LOGICAL_RIGHT)
        approach_started = time.monotonic()
        try:
            while time.monotonic() - approach_started < 1.5:
                positions = self.position_snapshot()
                fighters = positions["fighters"]
                assert isinstance(fighters, list)
                by_player = {
                    int(item["player"]): item
                    for item in fighters
                    if isinstance(item, dict)
                }
                if set(by_player) == {0, 1}:
                    separation = (
                        int(by_player[1]["active_x"])
                        - int(by_player[0]["active_x"])
                    )
                    if separation <= 72:
                        break
                if self.runtime_bool("mutation_failed"):
                    raise live.HarnessFailure(
                        "Training failed while approaching for native hit"
                    )
                time.sleep(0.025)
        finally:
            self.set_live_gameplay_p1_mask(0)
        approached_positions = self.position_snapshot()
        time.sleep(0.08)

        buttons = (LOGICAL_A, LOGICAL_B, LOGICAL_C)
        try:
            for attempt in range(18):
                button = buttons[attempt % len(buttons)]
                self.set_live_gameplay_p1_mask(
                    LOGICAL_RIGHT | button
                )
                time.sleep(0.11)
                self.set_live_gameplay_p1_mask(LOGICAL_RIGHT)
                time.sleep(0.10)
                mailbox = self.live_mailbox()
                snapshot = self.invariant_snapshot()
                attempts.append({
                    "attempt": attempt + 1,
                    "button": button,
                    "detour_count":
                        mailbox["lethal_damage_detour_count"],
                    "p2_effective": snapshot["life"]["p2"]["effective"],
                    "step_serial": self.runtime_u64("step_serial"),
                    "mutation_failed":
                        self.runtime_bool("mutation_failed"),
                })
                if self.runtime_bool("mutation_failed"):
                    raise live.HarnessFailure(
                        "Training fail-closed during genuine native hit: "
                        + json.dumps(attempts[-1], sort_keys=True)
                    )
                if (
                    int(mailbox["lethal_damage_detour_count"])
                        >= before_count + 1
                    and int(snapshot["life"]["p2"]["effective"])
                        < int(before_life["effective"])
                ):
                    break
        finally:
            self.set_live_gameplay_p1_mask(0)

        after_mailbox = self.live_mailbox()
        after_invariants = self.invariant_snapshot()
        after_count = int(
            after_mailbox["lethal_damage_detour_count"]
        )
        after_life = dict(after_invariants["life"]["p2"])
        if (
            after_count < before_count + 1
            or int(after_life["effective"])
                >= int(before_life["effective"])
        ):
            raise live.HarnessFailure(
                "stock collision did not produce a native P2 damage "
                "entries: "
                + json.dumps({
                    "before_count": before_count,
                    "after_count": after_count,
                    "before_life": before_life,
                    "after_life": after_life,
                    "approached_positions": approached_positions,
                    "attempts": attempts,
                }, sort_keys=True)
            )

        serial_after_hit = self.runtime_u64("step_serial")
        settle = {
            "count": after_count,
            "last_change_serial": serial_after_hit,
        }

        def continued() -> dict[str, object] | None:
            serial = self.runtime_u64("step_serial")
            failed = self.runtime_bool("mutation_failed")
            count = int(
                self.live_mailbox()["lethal_damage_detour_count"]
            )
            if count != settle["count"]:
                settle["count"] = count
                settle["last_change_serial"] = serial
            if (
                not failed
                and serial >= serial_after_hit + 12
                and serial >= settle["last_change_serial"] + 20
            ):
                return {
                    "step_serial": serial,
                    "mutation_failed": failed,
                    "settled_detour_count": count,
                    "mailbox": self.live_mailbox(),
                }
            return None

        continuation = self.wait_until(
            "continued Training admission after genuine native hits",
            continued,
            timeout=3.0,
            diagnostic=lambda: {
                "step_serial": self.runtime_u64("step_serial"),
                "mutation_failed": self.runtime_bool("mutation_failed"),
                "mailbox": self.live_mailbox(),
            },
        )
        assert isinstance(continuation, dict)
        after_mailbox = self.live_mailbox()
        after_invariants = self.invariant_snapshot()
        after_count = int(
            after_mailbox["lethal_damage_detour_count"]
        )
        after_life = dict(after_invariants["life"]["p2"])
        record: dict[str, object] = {
            "native_collision_path": True,
            "input_source": "exact-env logical P1 snapshots",
            "before_step_serial": before_serial,
            "after_hit_step_serial": serial_after_hit,
            "before_detour_count": before_count,
            "after_detour_count": after_count,
            "detour_entries": after_count - before_count,
            "before_life": before_life,
            "after_life": after_life,
            "approached_positions": approached_positions,
            "attempts": attempts,
            "continued_admission": continuation,
        }
        self.event(
            "native-combat-hit-proven",
            detour_entries=after_count - before_count,
            p2_life_before=before_life["effective"],
            p2_life_after=after_life["effective"],
            continued_step_serial=continuation["step_serial"],
        )
        return record

    def selection_state(self) -> dict[str, int]:
        assert self.game_memory is not None
        memory = self.game_memory
        fields = (
            "scene",
            "scene_next",
            "scene3_sub",
            "main_menu_sub",
            "p1_char",
            "p2_char",
            "p1_confirm",
            "p2_confirm",
            "match_start",
            "frame_counter",
            "game_mode",
            "control_mask",
            "p1_input_source",
            "p2_input_source",
            "p2_route",
            "stage_cursor",
            "logical_stage",
        )
        state = {
            field: memory.read_u32(ADDR[field]) for field in fields
        }
        state["frontend_input_armed"] = self.runtime_u32(
            "frontend_input_armed"
        )
        state["stage_input_armed"] = self.runtime_u32(
            "stage_input_armed"
        )
        state["live_input_mask"] = self.runtime_u32("live_input_mask")
        return state

    def selector_input_history(self, player: int) -> dict[str, int]:
        if player not in (0, 1):
            raise live.HarnessFailure(
                f"selector input-history player is invalid: {player}"
            )
        assert self.game_memory is not None
        cursor_address, value_base, duration_base = (
            SELECTOR_INPUT_HISTORY[player]
        )
        cursor = self.game_memory.read_u32(cursor_address) & 0xFF
        if cursor >= 16:
            raise live.HarnessFailure(
                f"P{player + 1} selector input-history cursor is invalid: "
                f"{cursor}"
            )
        return {
            "cursor": cursor,
            "value": self.game_memory.read_u32(
                value_base + cursor * 4
            ) & 0x0F,
            "duration": self.game_memory.read_u32(
                duration_base + cursor * 4
            ),
        }

    def drive_character_select(self, cycle: int = 1) -> None:
        assert self.game_memory is not None
        memory = self.game_memory
        selection_timeout = 15.0
        transitions: list[dict[str, object]] = []
        selection_cycles = self.report.setdefault(
            "selection_cycles", []
        )
        assert isinstance(selection_cycles, list)
        last_transition: tuple[int, ...] | None = None
        last_observed: dict[str, int] = {}
        reached_character_select = False

        def observe(stage: str, state: dict[str, int]) -> None:
            nonlocal last_transition, last_observed
            last_observed = dict(state)
            signature = tuple(
                int(state[field])
                for field in (
                    "scene",
                    "scene_next",
                    "main_menu_sub",
                    "scene3_sub",
                    "p1_confirm",
                    "p2_confirm",
                    "match_start",
                    "frame_counter",
                    "game_mode",
                    "control_mask",
                    "p1_input_source",
                    "p2_input_source",
                    "p2_route",
                    "stage_cursor",
                    "logical_stage",
                    "frontend_input_armed",
                    "stage_input_armed",
                    "live_input_mask",
                )
            )
            if signature == last_transition:
                return
            last_transition = signature
            item: dict[str, object] = {
                "elapsed_seconds": round(
                    time.monotonic() - self.started_mono, 3
                ),
                "stage": stage,
                "state": dict(state),
            }
            transitions.append(item)
            self.event(
                "selection-transition", stage=stage, state=state
            )

        def ready() -> dict[str, int] | None:
            nonlocal reached_character_select
            state = self.selection_state()
            observe("await-p1", state)
            if state["scene"] == 3 and state["scene3_sub"] == 2:
                reached_character_select = True
            if state["scene"] == 2 and not reached_character_select:
                raise live.HarnessFailure(
                    "game entered scene 2 before training character select "
                    "(native attract/demo path): "
                    + json.dumps(state, sort_keys=True)
                )
            expected = (
                state["scene"] == 3
                and state["scene_next"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 1
                and state["p2_confirm"] == 0
                and state["game_mode"] == 1
                and (state["control_mask"] & 3) == 0
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 0
                and state["p2_route"] == 1
                and state["frontend_input_armed"] == 1
                and state["live_input_mask"] == 0
            )
            return state if expected else None

        ready_state = self.wait_until(
            "training P1 character-selection ownership",
            ready,
            timeout=selection_timeout,
            diagnostic=lambda: last_observed,
        )
        assert isinstance(ready_state, dict)
        self.set_live_input_mask(0)
        memory.suspend()
        try:
            locked_ready = ready()
            if not isinstance(locked_ready, dict):
                raise live.HarnessFailure(
                    "P1 character-select ownership changed before the "
                    "atomic cursor/confirm transaction"
                )
            memory.write_u32(
                ADDR["p1_char"], self.args.p1_character
            )
            if (
                memory.read_u32(ADDR["p1_char"])
                    != self.args.p1_character
                or memory.read_u32(ADDR["p1_confirm"]) != 1
            ):
                raise live.HarnessFailure(
                    "P1 cursor transaction changed native confirm ownership"
                )
        finally:
            memory.resume()
        self.event(
            "p1-cursor-set",
            state=locked_ready,
            character=self.args.p1_character,
            atomic_while_owned_process_suspended=True,
        )

        def p1_pressed() -> dict[str, int] | None:
            state = self.selection_state()
            observe("native-p1-first-press", state)
            valid = (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 2
                and state["p2_confirm"] == 0
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 0
                and state["p2_route"] == 1
                and state["frontend_input_armed"] == 1
                and state["live_input_mask"] == 1
            )
            return state if valid else None

        self.set_live_input_mask(1)
        try:
            p1_press_state = self.wait_until(
                "native P1 first confirm edge state 1->2",
                p1_pressed,
                timeout=3.0,
                diagnostic=lambda: last_observed,
            )
            assert isinstance(p1_press_state, dict)
            p1_press_edge = self.selector_input_history(0)
            if p1_press_edge["value"] != 1:
                raise live.HarnessFailure(
                    "P1 state 1->2 did not leave confirm in the native "
                    "input history: "
                    + json.dumps(p1_press_edge, sort_keys=True)
                )
        finally:
            self.set_live_input_mask(0)
        assert isinstance(p1_press_state, dict)

        def p1_neutral_rearmed() -> dict[str, int] | None:
            state = self.selection_state()
            edge = self.selector_input_history(0)
            state["selector_history_cursor"] = edge["cursor"]
            state["selector_history_value"] = edge["value"]
            state["selector_history_duration"] = edge["duration"]
            observe("native-p1-neutral-rearm", state)
            valid = (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 2
                and state["p2_confirm"] == 0
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 0
                and state["p2_route"] == 1
                and state["frontend_input_armed"] == 1
                and state["live_input_mask"] == 0
                and edge["value"] == 0
                and edge["cursor"] != p1_press_edge["cursor"]
            )
            return state if valid else None

        p1_neutral_state = self.wait_until(
            "native P1 neutral history edge while selector remains state 2",
            p1_neutral_rearmed,
            timeout=3.0,
            diagnostic=lambda: last_observed,
        )
        assert isinstance(p1_neutral_state, dict)

        def p1_confirmed() -> dict[str, int] | None:
            state = self.selection_state()
            edge = self.selector_input_history(0)
            state["selector_history_cursor"] = edge["cursor"]
            state["selector_history_value"] = edge["value"]
            state["selector_history_duration"] = edge["duration"]
            observe("native-p1-second-press", state)
            valid = (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 4
                and state["p2_confirm"] == 0
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 0
                and state["p2_route"] == 1
                and state["frontend_input_armed"] == 1
                and state["live_input_mask"] == 1
                and edge["value"] == 1
                and edge["cursor"]
                    != p1_neutral_state["selector_history_cursor"]
            )
            return state if valid else None

        self.set_live_input_mask(1)
        try:
            p1_confirm_state = self.wait_until(
                "native P1 second confirm edge state 2->4 and "
                "same-call P2 hold",
                p1_confirmed,
                timeout=3.0,
                diagnostic=lambda: last_observed,
            )
        finally:
            self.set_live_input_mask(0)
        assert isinstance(p1_press_state, dict)
        assert isinstance(p1_confirm_state, dict)
        self.event(
            "p1-confirmed-natively",
            first_press_state=p1_press_state,
            first_press_history=p1_press_edge,
            neutral_rearm_state=p1_neutral_state,
            second_press_state=p1_confirm_state,
            state=p1_confirm_state,
            character=self.args.p1_character,
        )

        def p2_handoff() -> dict[str, int] | None:
            state = self.selection_state()
            observe("await-p2-handoff", state)
            valid = (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 4
                and state["p2_confirm"] == 1
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 0
                and state["p2_route"] == 1
                and (state["control_mask"] & 3) == 0
                and state["frontend_input_armed"] == 1
                and state["live_input_mask"] == 0
            )
            return state if valid else None

        handoff_state = self.wait_until(
            "native two-neutral-step P2 handoff",
            p2_handoff,
            timeout=selection_timeout,
            diagnostic=lambda: last_observed,
        )
        assert isinstance(handoff_state, dict)
        memory.suspend()
        try:
            locked_handoff = p2_handoff()
            if not isinstance(locked_handoff, dict):
                raise live.HarnessFailure(
                    "P2 handoff changed before the atomic cursor/confirm "
                    "transaction"
                )
            memory.write_u32(
                ADDR["p2_char"], self.args.p2_character
            )
            if (
                memory.read_u32(ADDR["p2_char"])
                    != self.args.p2_character
                or memory.read_u32(ADDR["p2_confirm"]) != 1
            ):
                raise live.HarnessFailure(
                    "P2 cursor transaction changed native confirm ownership"
                )
        finally:
            memory.resume()
        self.event(
            "p2-cursor-set",
            state=locked_handoff,
            character=self.args.p2_character,
            atomic_while_owned_process_suspended=True,
        )

        def p2_pressed() -> dict[str, int] | None:
            state = self.selection_state()
            observe("native-p2-first-press", state)
            valid = (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 4
                and state["p2_confirm"] == 2
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 0
                and state["p2_route"] == 1
                and state["frontend_input_armed"] == 1
                and state["live_input_mask"] == 1
            )
            return state if valid else None

        self.set_live_input_mask(1)
        try:
            p2_press_state = self.wait_until(
                "native P2 first confirm edge state 1->2",
                p2_pressed,
                timeout=3.0,
                diagnostic=lambda: last_observed,
            )
            assert isinstance(p2_press_state, dict)
            p2_press_edge = self.selector_input_history(1)
            if p2_press_edge["value"] != 1:
                raise live.HarnessFailure(
                    "P2 state 1->2 did not leave confirm in the native "
                    "input history: "
                    + json.dumps(p2_press_edge, sort_keys=True)
                )
        finally:
            self.set_live_input_mask(0)
        assert isinstance(p2_press_state, dict)

        def p2_neutral_rearmed() -> dict[str, int] | None:
            state = self.selection_state()
            edge = self.selector_input_history(1)
            state["selector_history_cursor"] = edge["cursor"]
            state["selector_history_value"] = edge["value"]
            state["selector_history_duration"] = edge["duration"]
            observe("native-p2-neutral-rearm", state)
            valid = (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 4
                and state["p2_confirm"] == 2
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 0
                and state["p2_route"] == 1
                and state["frontend_input_armed"] == 1
                and state["live_input_mask"] == 0
                and edge["value"] == 0
                and edge["cursor"] != p2_press_edge["cursor"]
            )
            return state if valid else None

        p2_neutral_state = self.wait_until(
            "native P2 neutral history edge while selector remains state 2",
            p2_neutral_rearmed,
            timeout=3.0,
            diagnostic=lambda: last_observed,
        )
        assert isinstance(p2_neutral_state, dict)

        def p2_confirmed() -> dict[str, int] | None:
            state = self.selection_state()
            edge = self.selector_input_history(1)
            state["selector_history_cursor"] = edge["cursor"]
            state["selector_history_value"] = edge["value"]
            state["selector_history_duration"] = edge["duration"]
            observe("native-p2-second-press", state)
            valid = (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 4
                and state["p2_confirm"] == 4
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 0
                and state["p2_route"] == 1
                and state["frontend_input_armed"] == 1
                and state["live_input_mask"] == 1
                and edge["value"] == 1
                and edge["cursor"]
                    != p2_neutral_state["selector_history_cursor"]
            )
            return state if valid else None

        self.set_live_input_mask(1)
        try:
            p2_confirm_state = self.wait_until(
                "native P2 second confirm edge state 2->4",
                p2_confirmed,
                timeout=3.0,
                diagnostic=lambda: last_observed,
            )
        finally:
            self.set_live_input_mask(0)
        assert isinstance(p2_press_state, dict)
        assert isinstance(p2_confirm_state, dict)
        self.event(
            "p2-confirmed-natively",
            first_press_state=p2_press_state,
            first_press_history=p2_press_edge,
            neutral_rearm_state=p2_neutral_state,
            second_press_state=p2_confirm_state,
            state=p2_confirm_state,
            character=self.args.p2_character,
        )

        def match_start_ready() -> dict[str, int] | None:
            state = self.selection_state()
            observe("await-native-stage-confirm", state)
            valid = (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and state["p1_confirm"] == 4
                and state["p2_confirm"] == 4
                and state["match_start"] == 1
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 1
                and state["p2_route"] == 1
                and state["frontend_input_armed"] == 1
                and state["stage_input_armed"] == 1
                and state["live_input_mask"] == 0
            )
            return state if valid else None

        stage_ready_state = self.wait_until(
            "native match-start stage confirmation prompt",
            match_start_ready,
            timeout=selection_timeout,
            diagnostic=lambda: last_observed,
        )
        assert isinstance(stage_ready_state, dict)

        initial_stage_cursor = stage_ready_state["stage_cursor"]
        if initial_stage_cursor != 0:
            raise live.HarnessFailure(
                "training stage selector did not begin at stage 1: "
                + json.dumps(stage_ready_state, sort_keys=True)
            )

        def second_stage_selected() -> dict[str, int] | None:
            state = self.selection_state()
            observe("native-stage-right", state)
            if state["stage_cursor"] == initial_stage_cursor:
                return None
            return state

        self.set_live_input_mask(LOGICAL_RIGHT)
        try:
            stage_right_state = self.wait_until(
                "one native Right edge selecting stage 2",
                second_stage_selected,
                timeout=3.0,
                diagnostic=lambda: last_observed,
            )
        finally:
            self.set_live_input_mask(0)
        assert isinstance(stage_right_state, dict)
        if stage_right_state["stage_cursor"] != 1:
            raise live.HarnessFailure(
                "one native Right edge did not advance exactly one stage: "
                + json.dumps(stage_right_state, sort_keys=True)
            )

        def gameplay() -> dict[str, int] | None:
            state = self.selection_state()
            observe("native-stage-release", state)
            valid = (
                state["scene"] == 2
                and state["scene_next"] == 2
                and memory.read_i32(ADDR["match_sub"]) == 0
                and state["p1_input_source"] == 0
                and state["p2_input_source"] == 1
                and state["p2_route"] == 1
                and (state["control_mask"] & 3) == 0
            )
            if not valid:
                return None
            state["match_sub"] = memory.read_i32(ADDR["match_sub"])
            return state

        stage_press_start = self.selection_state()
        stage_press_start_frame = stage_press_start["frame_counter"]
        stage_press_end = stage_press_start
        stage_press_reason = "wall-clock-bound"
        stage_press_wall_bound = 0.25
        stage_press_deadline = (
            time.monotonic() + stage_press_wall_bound
        )
        self.set_live_input_mask(1)
        try:
            while True:
                self.scan_failures()
                stage_press_end = self.selection_state()
                observe("native-stage-press", stage_press_end)
                frame_delta = (
                    stage_press_end["frame_counter"]
                    - stage_press_start_frame
                ) & 0xFFFFFFFF
                visible_transition = (
                    stage_press_end["scene"] != 3
                    or stage_press_end["scene_next"] != 3
                    or stage_press_end["scene3_sub"] != 2
                    or stage_press_end["match_start"] != 1
                )
                if visible_transition:
                    stage_press_reason = "visible-native-transition"
                    break
                if frame_delta >= 2:
                    stage_press_reason = "two-dispatch-frame-target"
                    break
                if time.monotonic() >= stage_press_deadline:
                    break
                time.sleep(0.005)
        finally:
            self.set_live_input_mask(0)
        stage_press_frames = (
            stage_press_end["frame_counter"] - stage_press_start_frame
        ) & 0xFFFFFFFF
        gameplay_state = self.wait_until(
            "released stage-confirm edge, scene 2, and P2 normalization",
            gameplay,
            timeout=selection_timeout,
            diagnostic=lambda: last_observed,
        )
        selection_record = {
            "cycle": cycle,
            "transitions": transitions,
            "ready": locked_ready,
            "p1_press_state": p1_press_state,
            "p1_neutral_rearm": p1_neutral_state,
            "p1_confirmed_natively": p1_confirm_state,
            "p2_handoff": locked_handoff,
            "p2_press_state": p2_press_state,
            "p2_neutral_rearm": p2_neutral_state,
            "p2_confirmed_natively": p2_confirm_state,
            "native_stage_confirm_ready": stage_ready_state,
            "native_stage_right_once": stage_right_state,
            "stage_confirm_pulse": {
                "press_start": stage_press_start,
                "press_end": stage_press_end,
                "observed_dispatch_frames": stage_press_frames,
                "target_hold_dispatch_frames": 2,
                "wall_clock_bound_seconds": stage_press_wall_bound,
                "release_reason": stage_press_reason,
                "released_before_terminal_wait": True,
            },
            "gameplay": gameplay_state,
            "direct_confirm_state_writes": False,
            "native_confirm_pulses": 5,
            "native_press_release_edges": 5,
            "native_stage_direction_edges": 1,
        }
        selection_cycles.append(selection_record)
        if cycle == 1:
            self.report["selection"] = selection_record
            self.report["selection_transitions"] = transitions
        self.event(
            "native-selection-complete",
            cycle=cycle,
            state=gameplay_state,
        )

    def fighter_snapshot(self) -> list[dict[str, int]]:
        assert self.game_memory is not None
        memory = self.game_memory
        result: list[dict[str, int]] = []
        for player in range(2):
            item: dict[str, int] = {"player": player}
            for table_name, label in (
                ("fighter_active_table", "active"),
                ("fighter_mirror_table", "mirror"),
            ):
                pointer = memory.read_u32(
                    ADDR[table_name] + player * 4
                )
                if pointer < 0x10000:
                    raise live.HarnessFailure(
                        f"P{player + 1} {label} fighter pointer is invalid: "
                        f"0x{pointer:X}"
                    )
                memory.read(pointer, FIGHTER_SIZE)
                slot = _read_u8(
                    memory, pointer + FIGHTER_OFFSET["slot"]
                )
                item[f"{label}_pointer"] = pointer
                item[f"{label}_slot"] = slot
                item[f"{label}_character"] = _read_u8(
                    memory, pointer + FIGHTER_OFFSET["character"]
                )
                item[f"{label}_x"] = memory.read_i32(
                    pointer + FIGHTER_OFFSET["x"]
                )
                item[f"{label}_y"] = memory.read_i32(
                    pointer + FIGHTER_OFFSET["y"]
                )
                item[f"{label}_previous_x"] = memory.read_i32(
                    pointer + FIGHTER_OFFSET["previous_x"]
                )
                item[f"{label}_facing"] = _read_u8(
                    memory, pointer + FIGHTER_OFFSET["facing"]
                )
                item[f"{label}_facing_snapshot"] = memory.read_u32(
                    pointer + FIGHTER_OFFSET["facing_snapshot"]
                )
                for field in (
                    "round_end", "round_lost", "fatal_cause",
                    "fatal_attacker", "com",
                ):
                    item[f"{label}_{field}"] = memory.read_u32(
                        pointer + FIGHTER_OFFSET[field]
                    )
            result.append(item)
        return result

    def position_snapshot(self) -> dict[str, object]:
        assert self.game_memory is not None
        fighters = self.fighter_snapshot()
        return {
            "fighters": [
                {
                    key: value
                    for key, value in fighter.items()
                    if key == "player"
                    or key.endswith((
                        "_x", "_y", "_previous_x",
                        "_facing", "_facing_snapshot",
                        "_character",
                    ))
                }
                for fighter in fighters
            ],
            "camera_x": self.game_memory.read_i32(ADDR["camera_x"]),
            "camera_y": self.game_memory.read_i32(ADDR["camera_y"]),
            "camera_previous_x": self.game_memory.read_i32(
                ADDR["camera_previous_x"]
            ),
        }

    def assert_native_position_commit(
        self, snapshot: dict[str, object]
    ) -> None:
        fighters = snapshot.get("fighters")
        if not isinstance(fighters, list) or len(fighters) != 2:
            raise live.HarnessFailure(
                "native position snapshot lacks both fighters"
            )
        problems: dict[str, object] = {}
        for fighter in fighters:
            assert isinstance(fighter, dict)
            player = int(fighter["player"]) + 1
            copies = ["active"]
            if int(fighter["mirror_character"]) != 0xFF:
                copies.append("mirror")
            for copy in copies:
                x = int(fighter[f"{copy}_x"])
                previous = int(fighter[f"{copy}_previous_x"])
                y = int(fighter[f"{copy}_y"])
                facing = int(fighter[f"{copy}_facing"])
                facing_snapshot = int(
                    fighter[f"{copy}_facing_snapshot"]
                )
                if not 0 <= x <= 511:
                    problems[f"p{player}_{copy}_x"] = x
                if y != 0:
                    problems[f"p{player}_{copy}_y"] = y
                if not 0 <= previous <= 511:
                    problems[f"p{player}_{copy}_previous_x"] = {
                        "x": x,
                        "previous_x": previous,
                    }
                if facing not in (0, 1) or facing_snapshot != facing:
                    problems[f"p{player}_{copy}_facing"] = {
                        "facing": facing,
                        "snapshot": facing_snapshot,
                    }
            if "mirror" in copies:
                for field in (
                    "x", "y", "previous_x", "facing", "facing_snapshot"
                ):
                    if (
                        fighter[f"active_{field}"]
                        != fighter[f"mirror_{field}"]
                    ):
                        problems[f"p{player}_{field}_mirror"] = {
                            "active": fighter[f"active_{field}"],
                            "mirror": fighter[f"mirror_{field}"],
                        }
        by_player = {
            int(fighter["player"]): fighter
            for fighter in fighters
            if isinstance(fighter, dict)
        }
        if set(by_player) == {0, 1}:
            p1 = by_player[0]
            p2 = by_player[1]
            p1_x = int(p1["active_x"])
            p2_x = int(p2["active_x"])
            separation = p2_x - p1_x
            if separation <= 0:
                problems["center_anchor_separation"] = {
                    "expected": "P1 remains left of P2",
                    "actual": separation,
                }
            if (
                int(p1["active_facing"]) != 0
                or int(p2["active_facing"]) != 1
            ):
                problems["center_facing_orientation"] = {
                    "expected": {"p1": 0, "p2": 1},
                    "actual": {
                        "p1": p1["active_facing"],
                        "p2": p2["active_facing"],
                    },
                }
            expected_camera = max(
                0, min(192, (p1_x + p2_x) // 2 - 32)
            )
            actual_camera = snapshot.get("camera_x")
            if (
                not isinstance(actual_camera, int)
                or abs(actual_camera - expected_camera)
                > ANCHOR_REANCHOR_SLACK
            ):
                problems["center_camera_x"] = {
                    "expected": expected_camera,
                    "slack": ANCHOR_REANCHOR_SLACK,
                    "actual": actual_camera,
                }
        else:
            problems["fighter_player_slots"] = {
                "expected": [0, 1],
                "actual": sorted(by_player),
            }
        if snapshot.get("camera_y") != 0:
            problems["camera_y"] = snapshot.get("camera_y")
        if snapshot.get("camera_previous_x") != snapshot.get("camera_x"):
            problems["camera_previous_x"] = {
                "camera_x": snapshot.get("camera_x"),
                "previous": snapshot.get("camera_previous_x"),
            }
        if problems:
            raise live.HarnessFailure(
                "native reset postvalidation was not externally visible: "
                + json.dumps(problems, sort_keys=True)
            )

    def invariant_snapshot(self) -> dict[str, object]:
        assert self.game_memory is not None
        memory = self.game_memory
        active_config = memory.read_u32(ADDR["active_config_ptr"])
        if active_config < 0x10000:
            raise live.HarnessFailure(
                f"invalid active QOH config pointer 0x{active_config:X}"
            )
        memory.read(active_config, live.QOH_CONFIG_PROFILE_STRIDE)
        life: dict[str, dict[str, int]] = {}
        for player in (1, 2):
            current = memory.read_i32(ADDR[f"life_current_p{player}"])
            reserve = memory.read_i32(ADDR[f"life_reserve_p{player}"])
            damage = memory.read_i32(ADDR[f"life_damage_p{player}"])
            life[f"p{player}"] = {
                "current": current,
                "reserve": reserve,
                "damage": damage,
                "effective": current + reserve - damage,
            }
        return {
            "scene": memory.read_u32(ADDR["scene"]),
            "scene_next": memory.read_u32(ADDR["scene_next"]),
            "match_sub": memory.read_i32(ADDR["match_sub"]),
            "result_state": memory.read_u32(ADDR["result_state"]),
            "outcome_p1": memory.read_u32(ADDR["outcome_p1"]),
            "outcome_p2": memory.read_u32(ADDR["outcome_p2"]),
            "round_counter": memory.read_u32(ADDR["round_counter"]),
            "round_wins_p1": memory.read_u32(ADDR["round_wins_p1"]),
            "round_wins_p2": memory.read_u32(ADDR["round_wins_p2"]),
            "result_progress_p1":
                memory.read_u32(ADDR["result_progress_p1"]),
            "result_progress_p2":
                memory.read_u32(ADDR["result_progress_p2"]),
            "control_mask": memory.read_u32(ADDR["control_mask"]),
            "p1_input_source":
                memory.read_u32(ADDR["p1_input_source"]),
            "p2_input_source":
                memory.read_u32(ADDR["p2_input_source"]),
            "p2_route": memory.read_u32(ADDR["p2_route"]),
            "local_network_mode":
                memory.read_u32(ADDR["local_network_mode"]),
            "vs_launch": memory.read_u32(ADDR["vs_launch"]),
            "pause": _read_u8(memory, ADDR["pause"]),
            "freeze": _read_u8(memory, ADDR["freeze"]),
            "native_fatal_disable":
                memory.read_u32(ADDR["native_fatal_disable"]),
            "frame_counter": memory.read_u32(ADDR["frame_counter"]),
            "active_config": active_config,
            "config_timer_mode": memory.read_u32(active_config + 36),
            "config_extra_function":
                memory.read_u32(active_config + 80),
            "config_wait_count": memory.read_u32(active_config + 84),
            "config_wait_counter": memory.read_u32(active_config + 88),
            "copied_timer_mode":
                memory.read_u32(ADDR["copied_config"] + 36),
            "copied_extra_function":
                memory.read_u32(ADDR["copied_config"] + 80),
            "copied_wait_count":
                memory.read_u32(ADDR["copied_config"] + 84),
            "copied_wait_counter":
                memory.read_u32(ADDR["copied_config"] + 88),
            "life": life,
            "fighters": self.fighter_snapshot(),
        }

    def assert_invariants(
        self,
        snapshot: dict[str, object],
        stable: dict[str, int],
        require_zero_life: bool,
    ) -> None:
        exact = {
            "scene": 2,
            "scene_next": 2,
            "match_sub": 0,
            "result_state": 0,
            "control_mask": 0,
            "p1_input_source": 0,
            "p2_input_source": 1,
            "p2_route": 1,
            "local_network_mode": 0,
            "pause": 0,
            "freeze": 0,
            "native_fatal_disable": 0,
            "config_timer_mode": 2,
            "config_extra_function": 0,
            "config_wait_count": 0,
            "config_wait_counter": 0,
            "copied_timer_mode": 2,
            "copied_extra_function": 0,
            "copied_wait_count": 0,
            "copied_wait_counter": 0,
        }
        differences = {
            field: {
                "expected": expected,
                "actual": snapshot.get(field),
            }
            for field, expected in exact.items()
            if snapshot.get(field) != expected
        }
        if int(snapshot.get("vs_launch", 1)) == 1:
            differences["vs_launch"] = {
                "expected": "not 1",
                "actual": snapshot.get("vs_launch"),
            }
        if int(snapshot.get("control_mask", 0)) & 3:
            differences["control_mask_low_two_bits"] = {
                "expected": 0,
                "actual": int(snapshot["control_mask"]) & 3,
            }
        for field, expected in stable.items():
            if snapshot.get(field) != expected:
                differences[field] = {
                    "expected": expected,
                    "actual": snapshot.get(field),
                }
        fighters = snapshot.get("fighters")
        if not isinstance(fighters, list) or len(fighters) != 2:
            differences["fighters"] = {
                "expected": "two validated fighter pairs",
                "actual": fighters,
            }
        else:
            for item in fighters:
                player = int(item["player"]) + 1
                expected_character = (
                    self.args.p1_character
                    if player == 1
                    else self.args.p2_character
                )
                if int(item["active_character"]) != expected_character:
                    differences[f"p{player}_active_character"] = {
                        "expected": expected_character,
                        "actual": item["active_character"],
                    }
                for copy in ("active", "mirror"):
                    for field in ("round_end", "round_lost", "com"):
                        key = f"{copy}_{field}"
                        if int(item[key]) != 0:
                            differences[f"p{player}_{key}"] = {
                                "expected": 0,
                                "actual": item[key],
                            }
        if require_zero_life:
            life = snapshot.get("life")
            if not isinstance(life, dict):
                differences["life"] = {
                    "expected": "P1/P2 effective life records",
                    "actual": life,
                }
            else:
                for player in ("p1", "p2"):
                    effective = int(life[player]["effective"])
                    if effective != 0:
                        differences[f"{player}_effective_life"] = {
                            "expected": 0,
                            "actual": effective,
                        }
        if differences:
            raise live.HarnessFailure(
                "training invariant changed: "
                + json.dumps(differences, sort_keys=True)
            )

    def exercise_recording_slots_after_reset(self) -> None:
        self.wait_for_active_training("recording slot regressions")
        self.set_live_gameplay_p1_mask(0)
        self.send_training_command("save-auto-record-state", COMMAND["save_state"])
        self.send_training_command("loop-auto-play", COMMAND["set_playback_mode"], value=PLAYBACK_LOOP)
        rows = []

        def wait_macro(state: int, minimum_frames: int = 0) -> dict[str, int]:
            def ready():
                box = self.live_mailbox()
                return box if box["macro_state"] == state and box["selected_macro_frame_count"] >= minimum_frames else None
            return self.wait_until("automatic macro state", ready, timeout=3.0, diagnostic=self.live_mailbox)

        for slot in range(8):
            self.send_training_command(f"select-recording-{slot}", 9, slot_or_setting=slot)
            self.send_training_command("record-after-reset", 45, value=2)
            self.send_training_command("reset-starts-recording", COMMAND["reset_position"], value=POSITION_CENTER)
            reset_recording = wait_macro(MACRO_RECORDING, 3)
            self.send_training_command("load-restarts-recording", COMMAND["load_state"])
            load_recording = wait_macro(MACRO_RECORDING, 3)
            self.send_training_command("play-after-reset", 45, value=1)
            self.send_training_command("reset-starts-playback", COMMAND["reset_position"], value=POSITION_CENTER)
            reset_playing = wait_macro(MACRO_PLAYING)
            self.send_training_command("load-restarts-playback", COMMAND["load_state"])
            load_playing = wait_macro(MACRO_PLAYING)
            self.send_training_command("nothing-after-reset", 45, value=0)
            self.send_training_command("reset-stops-macro", COMMAND["reset_position"], value=POSITION_CENTER)
            idle = wait_macro(MACRO_IDLE, 3)
            rows.append({"slot": slot + 1, "reset_recording": reset_recording,
                         "load_recording": load_recording, "reset_playing": reset_playing,
                         "load_playing": load_playing, "idle": idle})

        for slot in range(8):
            self.send_training_command(f"clear-recording-{slot}", 5, slot_or_setting=slot)
            if self.live_mailbox()["selected_macro_frame_count"] != 0:
                raise live.HarnessFailure(f"recording slot {slot + 1} was not cleared")
            for other in range(slot + 1, 8):
                self.send_training_command(f"check-preserved-{other}", 9, slot_or_setting=other)
                if self.live_mailbox()["selected_macro_frame_count"] < 3:
                    raise live.HarnessFailure(f"clearing slot {slot + 1} damaged slot {other + 1}")
        self.send_training_command("restore-recording-slot", 9, slot_or_setting=0)
        self.send_training_command("restore-playback-mode", COMMAND["set_playback_mode"], value=0)
        self.report["recording_slot_regressions"] = rows
        self.event("all-recording-slots-verified", slots=len(rows))

    def exercise_training_commands_and_return(self) -> None:
        assert self.game_memory is not None
        memory = self.game_memory
        command_records: list[dict[str, object]] = []
        active_mailbox = self.wait_for_active_training(
            "first qualified Training gameplay"
        )
        first_generation = active_mailbox["training_generation"]
        initial = self.invariant_snapshot()
        stable_fields = {
            field: int(initial[field])
            for field in (
                "outcome_p1",
                "outcome_p2",
                "round_counter",
                "round_wins_p1",
                "round_wins_p2",
                "result_progress_p1",
                "result_progress_p2",
            )
        }
        self.assert_invariants(
            initial, stable_fields, require_zero_life=False
        )

        saved_positions = self.position_snapshot()
        command_records.append(self.send_training_command(
            "save-slot-0", COMMAND["save_state"], slot_or_setting=0
        ))
        command_records.append(self.send_training_command(
            "reset-center",
            COMMAND["reset_position"],
            value=POSITION_CENTER,
        ))

        def centered() -> dict[str, object] | None:
            snapshot = self.position_snapshot()
            self.assert_native_position_commit(snapshot)
            return snapshot if snapshot != saved_positions else None

        centered_positions = self.wait_until(
            "native-validated center reset",
            centered,
            timeout=5.0,
            diagnostic=self.position_snapshot,
        )
        command_records.append(self.send_training_command(
            "load-slot-0", COMMAND["load_state"], slot_or_setting=0
        ))

        def restored() -> dict[str, object] | None:
            snapshot = self.position_snapshot()
            return snapshot if snapshot == saved_positions else None

        restored_positions = self.wait_until(
            "savestate position/state restoration",
            restored,
            timeout=5.0,
            diagnostic=self.position_snapshot,
        )
        after_load = self.invariant_snapshot()
        self.assert_invariants(
            after_load, stable_fields, require_zero_life=False
        )

        command_records.append(self.send_training_command(
            "hitbox-on", COMMAND["set_hitbox_display"], value=2
        ))

        def hitbox_on() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            native = _read_u8(memory, ADDR["hitbox"])
            return (
                {"native": native, "mirror": mailbox["hitbox_effective"]}
                if native == 1 and mailbox["hitbox_effective"] == 1
                else None
            )

        hitbox_on_state = self.wait_until(
            "native hitbox flag enabled",
            hitbox_on,
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )
        command_records.append(self.send_training_command(
            "hitbox-off", COMMAND["set_hitbox_display"], value=0
        ))

        def hitbox_off() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            native = _read_u8(memory, ADDR["hitbox"])
            return (
                {"native": native, "mirror": mailbox["hitbox_effective"]}
                if native == 0 and mailbox["hitbox_effective"] == 0
                else None
            )

        hitbox_off_state = self.wait_until(
            "native hitbox flag disabled",
            hitbox_off,
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )
        command_records.append(self.send_training_command(
            "reset-center-for-native-hit",
            COMMAND["reset_position"],
            value=POSITION_CENTER,
        ))

        def native_hit_centered() -> dict[str, object] | None:
            snapshot = self.position_snapshot()
            self.assert_native_position_commit(snapshot)
            return snapshot

        native_hit_center = self.wait_until(
            "center reset before genuine native combat hit",
            native_hit_centered,
            timeout=3.0,
            diagnostic=self.position_snapshot,
        )
        native_combat_hit = self.run_native_combat_hit_probe()
        lethal_damage_probe = self.run_lethal_damage_detour_probe()
        after_lethal_probe = self.invariant_snapshot()
        self.assert_invariants(
            after_lethal_probe, stable_fields, require_zero_life=False
        )

        p2 = self.fighter_snapshot()[1]
        p2_facing = int(p2["active_facing"])
        away = LOGICAL_LEFT if p2_facing == 0 else LOGICAL_RIGHT
        command_records.append(self.send_training_command(
            "guard-always-stand",
            COMMAND["set_guard_mode"],
            value=GUARD_ALWAYS_STAND,
        ))

        def standing_guard() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            return (
                mailbox
                if mailbox["final_p2_mask"] == away
                else None
            )

        standing_guard_state = self.wait_until(
            "Always Stand logical P2 guard",
            standing_guard,
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )
        command_records.append(self.send_training_command(
            "guard-always-crouch",
            COMMAND["set_guard_mode"],
            value=GUARD_ALWAYS_CROUCH,
        ))
        crouch_mask = away | LOGICAL_DOWN

        def crouching_guard() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            return (
                mailbox
                if mailbox["final_p2_mask"] == crouch_mask
                else None
            )

        crouching_guard_state = self.wait_until(
            "Always Crouch logical P2 guard",
            crouching_guard,
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )

        self.set_live_gameplay_p1_mask(0)
        command_records.append(self.send_training_command(
            "macro-record-placement", COMMAND["toggle_record"]
        ))
        command_records.append(self.send_training_command(
            "macro-record-start", COMMAND["toggle_record"]
        ))
        self.wait_until(
            "macro recording state",
            lambda: (
                self.live_mailbox()
                if self.live_mailbox()["macro_state"] == MACRO_RECORDING
                else None
            ),
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )
        self.set_live_gameplay_p1_mask(LOGICAL_A)

        def recorded_frames() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            valid = (
                mailbox["macro_state"] == MACRO_RECORDING
                and mailbox["selected_macro_frame_count"] >= 4
                and mailbox["final_p1_mask"] == (
                    LOGICAL_DOWN | (
                        LOGICAL_LEFT if self.fighter_snapshot()[0]["active_facing"] == 0
                        else LOGICAL_RIGHT
                    )
                )
                and mailbox["final_p2_mask"] == LOGICAL_A
                and (
                    mailbox["final_p2_mask"]
                    & LOGICAL_DIRECTION_MASK
                ) == 0
            )
            return mailbox if valid else None

        recording_state = self.wait_until(
            "guard-clean macro recording frames",
            recorded_frames,
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )
        self.set_live_gameplay_p1_mask(0)
        command_records.append(self.send_training_command(
            "macro-record-stop", COMMAND["toggle_record"]
        ))

        def recording_stopped() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            return (
                mailbox
                if mailbox["macro_state"] == MACRO_IDLE
                and mailbox["selected_macro_frame_count"] >= 4
                else None
            )

        recording_stopped_state = self.wait_until(
            "macro record stop boundary",
            recording_stopped,
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )
        command_records.append(self.send_training_command(
            "macro-loop-mode",
            COMMAND["set_playback_mode"],
            value=PLAYBACK_LOOP,
        ))
        command_records.append(self.send_training_command(
            "macro-play-start", COMMAND["toggle_playback"]
        ))

        def clean_playback() -> dict[str, int] | None:
            mailbox = self.live_mailbox()
            valid = (
                mailbox["macro_state"] == MACRO_PLAYING
                and mailbox["final_p2_mask"] == LOGICAL_A
                and (
                    mailbox["final_p2_mask"]
                    & LOGICAL_DIRECTION_MASK
                ) == 0
            )
            return mailbox if valid else None

        playback_state = self.wait_until(
            "guard-clean logical macro playback",
            clean_playback,
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )
        command_records.append(self.send_training_command(
            "macro-stop", COMMAND["stop_macro"]
        ))
        self.wait_until(
            "macro idle after stop",
            lambda: (
                self.live_mailbox()
                if self.live_mailbox()["macro_state"] == MACRO_IDLE
                else None
            ),
            timeout=3.0,
            diagnostic=self.live_mailbox,
        )
        command_records.append(self.send_training_command(
            "guard-off", COMMAND["set_guard_mode"], value=GUARD_OFF
        ))
        self.set_live_gameplay_p1_mask(0)

        command_records.append(self.send_training_command(
            "return-to-character-select",
            COMMAND["return_to_character_select"],
        ))

        def returned() -> dict[str, object] | None:
            state = self.selection_state()
            mailbox = self.live_mailbox()
            if (
                state["scene"] == 3
                and state["scene3_sub"] == 2
                and mailbox["training_generation"] > first_generation
                and state["live_input_mask"] == 0
                and mailbox["gameplay_p1_mask"] == 0
                and mailbox["request_id"] == 0
                and mailbox["lethal_damage_request_id"] == 0
                and mailbox["command_kind"] == 0
                and mailbox["slot_or_setting"] == 0
                and mailbox["value"] == 0
                and mailbox["flags"] == 0
            ):
                return {"selection": state, "mailbox": mailbox}
            return None

        returned_state = self.wait_until(
            "native return to character select and new generation",
            returned,
            timeout=15.0,
            diagnostic=lambda: {
                "selection": self.selection_state(),
                "mailbox": self.live_mailbox(),
            },
        )
        self.drive_character_select(cycle=2)
        second_active = self.wait_for_active_training(
            "second qualified Training gameplay"
        )
        if second_active["training_generation"] <= first_generation:
            raise live.HarnessFailure(
                "second Training match did not use a new generation"
            )
        final_invariants = self.invariant_snapshot()
        self.assert_invariants(
            final_invariants,
            {
                field: int(final_invariants[field])
                for field in (
                    "outcome_p1",
                    "outcome_p2",
                    "round_counter",
                    "round_wins_p1",
                    "round_wins_p2",
                    "result_progress_p1",
                    "result_progress_p2",
                )
            },
            require_zero_life=False,
        )
        self.report["training_commands"] = {
            "first_generation": first_generation,
            "second_generation": second_active["training_generation"],
            "commands": command_records,
            "saved_positions": saved_positions,
            "centered_positions": centered_positions,
            "restored_positions": restored_positions,
            "load_restored_exact_position_state":
                restored_positions == saved_positions,
            "hitbox_on": hitbox_on_state,
            "hitbox_off": hitbox_off_state,
            "native_hit_center": native_hit_center,
            "native_combat_hit": native_combat_hit,
            "lethal_damage_detour": lethal_damage_probe,
            "post_lethal_invariants": after_lethal_probe,
            "always_stand": standing_guard_state,
            "always_crouch": crouching_guard_state,
            "recording": recording_state,
            "recording_stopped": recording_stopped_state,
            "playback": playback_state,
            "return": returned_state,
            "second_active": second_active,
            "final_invariants": final_invariants,
        }
        self.event(
            "training-command-return-cycle-complete",
            commands=len(command_records),
            first_generation=first_generation,
            second_generation=second_active["training_generation"],
        )

    def wait_for_admitted_runtime(self) -> None:
        assert self.game_memory is not None

        def active() -> dict[str, int] | None:
            lifecycle = self.runtime_u32("lifecycle")
            disable_reason = self.runtime_u32("disable_reason")
            mutation_failed = self.runtime_bool("mutation_failed")
            serial = self.runtime_u64("step_serial")
            if (
                lifecycle == TRAINING_ACTIVE
                and disable_reason == TRAINING_DISABLE_NONE
                and not mutation_failed
                and serial > 0
            ):
                return {
                    "lifecycle": lifecycle,
                    "disable_reason": disable_reason,
                    "last_admission":
                        self.runtime_u32("last_admission"),
                    "step_serial": serial,
                    "sticky_errors":
                        self.runtime_u32("sticky_errors"),
                }
            return None

        runtime_start = self.wait_until(
            "qualified training admission", active
        )
        assert isinstance(runtime_start, dict)
        initial_snapshot = self.invariant_snapshot()
        stable_fields = {
            field: int(initial_snapshot[field])
            for field in (
                "round_counter",
                "outcome_p1",
                "outcome_p2",
                "round_wins_p1",
                "round_wins_p2",
                "result_progress_p1",
                "result_progress_p2",
            )
        }
        non_neutral = {
            field: value
            for field, value in stable_fields.items()
            if value != 0
        }
        if non_neutral:
            raise live.HarnessFailure(
                "fresh training match entered with non-neutral round/result "
                "state: " + json.dumps(non_neutral, sort_keys=True)
            )
        self.assert_invariants(
            initial_snapshot, stable_fields, require_zero_life=False
        )

        memory = self.game_memory
        memory.suspend()
        try:
            before_zero = self.invariant_snapshot()
            self.assert_invariants(
                before_zero, stable_fields, require_zero_life=False
            )
            life_write: dict[str, dict[str, int]] = {}
            for player in (1, 2):
                current = memory.read_i32(
                    ADDR[f"life_current_p{player}"]
                )
                reserve = memory.read_i32(
                    ADDR[f"life_reserve_p{player}"]
                )
                total = current + reserve
                if not 0 <= total < (1 << 31):
                    raise live.HarnessFailure(
                        f"P{player} current+reserve is not a safe signed "
                        f"zero-life damage value: {current}+{reserve}"
                    )
                _write_i32(
                    memory, ADDR[f"life_damage_p{player}"], total
                )
                life_write[f"p{player}"] = {
                    "current": current,
                    "reserve": reserve,
                    "damage_written": total,
                }
            after_zero = self.invariant_snapshot()
            self.assert_invariants(
                after_zero, stable_fields, require_zero_life=True
            )
        finally:
            memory.resume()

        start_serial = self.runtime_u64("step_serial")
        target_serial = start_serial + self.args.admitted_steps + 1
        start_frame = int(after_zero["frame_counter"])
        start_sticky = self.runtime_u32("sticky_errors")
        watch_started = time.monotonic()
        next_sample = start_serial
        sample_stride = max(1, self.args.admitted_steps // 40)
        samples: list[dict[str, object]] = []
        admitted_seen = False

        while True:
            self.scan_failures()
            lifecycle = self.runtime_u32("lifecycle")
            disable_reason = self.runtime_u32("disable_reason")
            mutation_failed = self.runtime_bool("mutation_failed")
            admission = self.runtime_u32("last_admission")
            serial = self.runtime_u64("step_serial")
            sticky = self.runtime_u32("sticky_errors")
            admitted_seen = admitted_seen or admission == TRAINING_ADMITTED
            if (
                lifecycle != TRAINING_ACTIVE
                or disable_reason != TRAINING_DISABLE_NONE
                or mutation_failed
                or sticky != start_sticky
            ):
                raise live.HarnessFailure(
                    "training runtime left qualified state: "
                    f"lifecycle={lifecycle} disable={disable_reason} "
                    f"mutationFailed={mutation_failed} "
                    f"sticky={start_sticky}->{sticky} serial={serial}"
                )
            if serial >= next_sample or serial >= target_serial:
                snapshot = self.invariant_snapshot()
                self.assert_invariants(
                    snapshot, stable_fields, require_zero_life=True
                )
                samples.append(
                    {
                        "step_serial": serial,
                        "delta": serial - start_serial,
                        "last_admission": admission,
                        "frame_counter": snapshot["frame_counter"],
                        "effective_life": {
                            player: details["effective"]
                            for player, details in snapshot["life"].items()
                        },
                    }
                )
                next_sample = serial + sample_stride
            if serial >= target_serial:
                break
            if (
                self.run_deadline is not None
                and time.monotonic() >= self.run_deadline
            ):
                raise live.HarnessFailure(
                    f"total launched-process timeout at admitted "
                    f"delta={serial - start_serial}; "
                    f"required > {self.args.admitted_steps}"
                )
            time.sleep(0.005)

        elapsed = time.monotonic() - watch_started
        final_serial = self.runtime_u64("step_serial")
        final_snapshot = self.invariant_snapshot()
        self.assert_invariants(
            final_snapshot, stable_fields, require_zero_life=True
        )
        delta = final_serial - start_serial
        admitted_hz = delta / elapsed if elapsed > 0 else 0.0
        if delta <= self.args.admitted_steps:
            raise live.HarnessFailure(
                f"observed only {delta} admitted steps; "
                f"required > {self.args.admitted_steps}"
            )
        if not admitted_seen:
            raise live.HarnessFailure(
                "map-visible last-admission state never reported Admitted"
            )
        if admitted_hz < self.args.minimum_admitted_hz:
            raise live.HarnessFailure(
                f"training admitted only {admitted_hz:.3f} steps/s; "
                f"minimum is {self.args.minimum_admitted_hz:.3f}"
            )
        if admitted_hz > self.args.maximum_admitted_hz:
            raise live.HarnessFailure(
                f"training admitted {admitted_hz:.3f} steps/s; maximum "
                f"full-speed bound is {self.args.maximum_admitted_hz:.3f}"
            )

        evidence = self.combined_text()
        missing_markers = [
            marker for marker in REQUIRED_DLL_MARKERS
            if marker not in evidence
        ]
        if missing_markers:
            raise live.HarnessFailure(
                "missing training runtime log evidence: "
                + ", ".join(missing_markers)
            )
        self.report["training_runtime"] = {
            "qualified_start": runtime_start,
            "initial_snapshot": initial_snapshot,
            "zero_life_write": life_write,
            "zero_life_snapshot": after_zero,
            "stable_fields": stable_fields,
            "start_step_serial": start_serial,
            "final_step_serial": final_serial,
            "admitted_step_delta": delta,
            "strictly_more_than_requested": delta > self.args.admitted_steps,
            "elapsed_seconds": elapsed,
            "admitted_steps_per_second": admitted_hz,
            "start_frame_counter": start_frame,
            "final_frame_counter": final_snapshot["frame_counter"],
            "samples": samples,
            "final_snapshot": final_snapshot,
            "required_log_markers": list(REQUIRED_DLL_MARKERS),
        }
        self.event(
            "zero-life-soak-complete",
            admitted_steps=delta,
            admitted_hz=round(admitted_hz, 3),
            frame_counter_delta=(
                int(final_snapshot["frame_counter"]) - start_frame
            ) & 0xFFFFFFFF,
            samples=len(samples),
        )

    def preserve_game_logs(self) -> list[str]:
        destination = self.role_root / "preserved_game_logs"
        copied: list[str] = []
        candidates = (
            self.install / "ddraw_wrapper.log",
            self.install / "qoherror.txt",
        )
        for source in candidates:
            if not source.is_file():
                continue
            destination.mkdir(exist_ok=True)
            target = destination / source.name
            shutil.copy2(source, target)
            copied.append(str(target.relative_to(self.run_root)))
        game_logs = self.install / "logs"
        if game_logs.is_dir():
            for source in game_logs.rglob("*"):
                if not source.is_file():
                    continue
                relative = source.relative_to(game_logs)
                target = destination / "logs" / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
                copied.append(str(target.relative_to(self.run_root)))
        return copied

    def cleanup(self) -> list[str]:
        errors: list[str] = []
        close_diagnostics: list[dict[str, object]] = []
        memory = self.game_memory
        if self.game_pid is None and self.caster_process is not None:
            candidate = live.child_game_pid(
                self.caster_process.pid, self.game.name
            )
            if (
                candidate is not None
                and candidate not in self.preexisting_pids
            ):
                self.game_pid = candidate
        if memory is None and self.game_pid is not None:
            caster_pid = (
                self.caster_process.pid
                if self.caster_process is not None else -1
            )
            still_owned = any(
                pid == self.game_pid
                and ppid == caster_pid
                and image.casefold() == self.game.name.casefold()
                for pid, ppid, image in live.process_entries()
            )
            if still_owned:
                try:
                    memory = live.ProcessMemory(self.game_pid)
                    self.game_memory = memory
                except Exception as error:
                    errors.append(
                        f"owned game handle reacquire: {error}"
                    )
        if memory is not None and memory.alive():
            try:
                close_diagnostics = live.post_close_on_named_desktop(
                    memory.pid, HIDDEN_DESKTOP
                )
                deadline = time.monotonic() + 8.0
                while memory.alive() and time.monotonic() < deadline:
                    time.sleep(0.02)
                if memory.alive():
                    raise live.HarnessFailure(
                        "owned qoh99 ignored WM_CLOSE for 8 seconds"
                    )
            except Exception as error:
                errors.append(f"game graceful close: {error}")
                try:
                    memory.terminate()
                    deadline = time.monotonic() + 5.0
                    while memory.alive() and time.monotonic() < deadline:
                        time.sleep(0.02)
                    if memory.alive():
                        errors.append(
                            "owned qoh99 remained active after termination"
                        )
                except Exception as terminate_error:
                    errors.append(
                        f"game forced cleanup: {terminate_error}"
                    )
        self.report["hidden_desktop_close"] = close_diagnostics

        process = self.caster_process
        if process is not None and process.poll() is None:
            try:
                process.wait(timeout=8.0)
            except subprocess.TimeoutExpired:
                errors.append("owned caster did not exit after game close")
                process.terminate()
                try:
                    process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3.0)
        if process is not None:
            self.report["caster_exit_code"] = process.poll()
        if memory is not None:
            try:
                self.report["game_exit_code"] = memory.exit_code()
            except Exception:
                pass
            memory.close()
        for handle in (self.stdout_handle, self.stderr_handle):
            if handle is not None:
                try:
                    handle.close()
                except Exception as error:
                    errors.append(f"log handle close: {error}")
        return errors

    def finalize_evidence(self) -> list[str]:
        errors: list[str] = []
        source_after = _guard_snapshot(self.source_guard_paths)
        source_unchanged = source_after == self.source_guard_before
        self.report["source_guard"] = {
            "before": self.source_guard_before,
            "after": source_after,
            "unchanged": source_unchanged,
        }
        if not source_unchanged:
            errors.append("source game install guard hashes changed")

        clone_key = self.install / "System" / "QOHcnf.key"
        if clone_key.is_file() and self.clone_config_before is not None:
            try:
                after = live.qoh_selected_profile_snapshot(
                    clone_key,
                    (36, 80, 84, 88, live.QOH_CONFIG_NOSOUND_OFFSET),
                )
                before_values = {
                    int(key): int(value)
                    for key, value in dict(
                        self.clone_config_before["values"]
                    ).items()
                }
                after_values = {
                    int(key): int(value)
                    for key, value in dict(after["values"]).items()
                }
                restored = (
                    int(after["profile"])
                    == int(self.clone_config_before["profile"])
                    and after_values == before_values
                )
                self.report["clone_config_after"] = after
                self.report["training_config_overlays_not_persisted"] = restored
                if self.passed and not restored:
                    errors.append(
                        "training runtime values persisted into cloned "
                        "QOHcnf.key"
                    )
            except Exception as error:
                errors.append(f"clone config readback: {error}")

        self.report["preserved_game_logs"] = self.preserve_game_logs()
        dll_log = _latest_log(self.role_root, "dll.log")
        client_log = _latest_log(self.role_root, "client.log")
        self.report["logs"] = {
            "caster_stdout": str(self.stdout_path),
            "caster_stderr": str(self.stderr_path),
            "dll": str(dll_log) if dll_log else None,
            "client": str(client_log) if client_log else None,
            "dll_tail": live.tail(live.read_text(dll_log), 160)
                if dll_log else "",
            "client_tail": live.tail(live.read_text(client_log), 80)
                if client_log else "",
            "stdout_tail": live.tail(
                live.read_text(self.stdout_path), 80
            ),
            "stderr_tail": live.tail(
                live.read_text(self.stderr_path), 80
            ),
        }
        return errors

    def prune_clone(self) -> None:
        target = self.install.resolve()
        if (
            target.name != "g"
            or not _inside(target, self.run_root.resolve())
            or target == self.run_root.resolve()
        ):
            raise live.HarnessFailure(
                f"refusing unsafe clone prune target {target}"
            )
        shutil.rmtree(target)
        self.report["clone_pruned_on_pass"] = {
            "path": str(target),
            "removed": True,
            "logs_and_report_preserved": True,
        }

    def write_report(self) -> None:
        self.report["finished"] = datetime.now().isoformat()
        self.report["elapsed_seconds"] = (
            time.monotonic() - self.started_mono
        )
        self.report["passed"] = self.passed
        self.report_path.write_text(
            json.dumps(self.report, indent=2, sort_keys=True),
            encoding="utf-8",
        )

    def run(self) -> int:
        active_error: BaseException | None = None
        try:
            self.prepare()
            self.launch()
            self.discover_game()
            self.drive_character_select(cycle=1)
            self.exercise_training_commands_and_return()
            self.exercise_recording_slots_after_reset()
            self.wait_for_admitted_runtime()
            self.passed = True
        except BaseException as error:
            active_error = error
            self.passed = False
            self.report["failure"] = {
                "type": type(error).__name__,
                "message": str(error),
                "traceback": traceback.format_exc(),
            }
            self.event(
                "failed",
                error_type=type(error).__name__,
                message=str(error),
            )
        finally:
            cleanup_errors = self.cleanup()
            evidence_errors = self.finalize_evidence()
            final_errors = cleanup_errors + evidence_errors
            if final_errors:
                self.passed = False
                self.report["finalization_errors"] = final_errors
                if active_error is None:
                    active_error = live.HarnessFailure(
                        "; ".join(final_errors)
                    )
            if (
                self.passed
                and self.args.prune_game_clone_on_pass
            ):
                try:
                    self.prune_clone()
                except Exception as error:
                    self.passed = False
                    self.report.setdefault(
                        "finalization_errors", []
                    ).append(f"clone prune: {error}")
                    if active_error is None:
                        active_error = error
            self.write_report()

        if active_error is not None:
            print(
                f"[training-e2e] FAILED: {active_error}\n"
                f"[training-e2e] report: {self.report_path}",
                file=sys.stderr,
                flush=True,
            )
            return 1
        self.event("passed", report=str(self.report_path))
        self.write_report()
        print(
            f"[training-e2e] PASS report={self.report_path}",
            flush=True,
        )
        return 0

def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Launch one hidden-desktop Release training session, perform the "
            "native two-stage character selection, exercise the real Training "
            "command queue and return/reselection lifecycle, force effective "
            "life to zero, and verify more than 480 admitted training steps."
        )
    )
    parser.add_argument("--caster", required=True)
    parser.add_argument("--game", required=True)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--wrapper", required=True)
    parser.add_argument("--dll-map", required=True)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--admitted-steps", type=int, default=480)
    parser.add_argument("--minimum-admitted-hz", type=float, default=58.0)
    parser.add_argument("--maximum-admitted-hz", type=float, default=62.5)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--p1-character", type=int, default=0)
    parser.add_argument("--p2-character", type=int, default=0)
    parser.add_argument(
        "--prune-game-clone-on-pass",
        action="store_true",
        help=(
            "remove only the owned physical game clone after success; report, "
            "caster/DLL logs, stdout/stderr, hashes, and copied map remain"
        ),
    )
    args = parser.parse_args(list(argv))
    for attribute in ("caster", "game", "dll", "wrapper", "dll_map"):
        path = Path(getattr(args, attribute))
        if not path.is_file():
            parser.error(
                f"--{attribute.replace('_', '-')} does not exist: {path}"
            )
    if args.admitted_steps < 480:
        parser.error("--admitted-steps must be at least 480")
    if not 0.0 <= args.minimum_admitted_hz <= 60.6:
        parser.error("--minimum-admitted-hz must be in [0, 60.6]")
    if not 60.0 <= args.maximum_admitted_hz <= 120.0:
        parser.error("--maximum-admitted-hz must be in [60, 120]")
    if args.maximum_admitted_hz <= args.minimum_admitted_hz:
        parser.error(
            "--maximum-admitted-hz must exceed --minimum-admitted-hz"
        )
    if not 20.0 <= args.timeout <= 600.0:
        parser.error("--timeout must be in [20, 600] seconds")
    for attribute in ("p1_character", "p2_character"):
        value = getattr(args, attribute)
        if not 0 <= value <= 23:
            parser.error(
                f"--{attribute.replace('_', '-')} must be in [0, 23]"
            )
    return args

def main(argv: Iterable[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(
                encoding="utf-8", errors="backslashreplace"
            )
        except (AttributeError, ValueError):
            pass
    args = parse_args(sys.argv[1:] if argv is None else argv)
    return TrainingHarness(args).run()

if __name__ == "__main__":
    raise SystemExit(main())
