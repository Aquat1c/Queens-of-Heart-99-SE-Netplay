"""Real-game replay checkpoint regression (native Windows, Release x86).

Uses the production caster, DLL, wrapper, and existing completed recordings.
Only disposable game copies are modified. Game commands use the production
WM_QOH99_HOTKEY bridge on an owned hidden desktop; physical keyboard capture is
outside this test. No game-memory writes, remote calls, or weakened replay
proof mode are used. Same-process gameplay bytes are compared at equal paused
frames; old recordings may separately pass in supported behaviour-proof mode.

Example, from the repository root:
  python src/netplay/tests/live_replay_checkpoint_e2e.py \
    --game-install "path/to/complete/game" --build BUILD \
    --replay "path/to/completed-one.q99r" --replay "path/to/completed-two.q99r" \
    --output BUILD/checkpoint-live/run-001 --folder

For only the folder checks use --folder-only. Run it once with --folder-pause
and once with --no-folder-pause, using separate output directories. Supply at
least two completed recordings with different rosters; --folder-repeat-first
also exercises a repeated recorded UUID without changing the file bytes.

The output directory must not exist. Failures retain reports and clone logs.
"""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zipfile

import live_game_e2e as live

K, U = live.kernel32, live.user32
WM_QOH99_HOTKEY = 0x8051
KEY_SPACE, KEY_RIGHT, KEY_LEFT, KEY_HOME = 0x39, 0xCD, 0xCB, 0xC7
KEY_F7, KEY_FASTER, KEY_INPUTS = 0x41, 0x0D, 0x0E

class StartupInfo(ctypes.Structure):
    _fields_ = [
        ("cb", w.DWORD), ("lpReserved", w.LPWSTR), ("lpDesktop", w.LPWSTR),
        ("lpTitle", w.LPWSTR), ("dwX", w.DWORD), ("dwY", w.DWORD),
        ("dwXSize", w.DWORD), ("dwYSize", w.DWORD),
        ("dwXCountChars", w.DWORD), ("dwYCountChars", w.DWORD),
        ("dwFillAttribute", w.DWORD), ("dwFlags", w.DWORD),
        ("wShowWindow", w.WORD), ("cbReserved2", w.WORD),
        ("lpReserved2", ctypes.POINTER(w.BYTE)), ("hStdInput", w.HANDLE),
        ("hStdOutput", w.HANDLE), ("hStdError", w.HANDLE),
    ]

class ProcessInfo(ctypes.Structure):
    _fields_ = [("hProcess", w.HANDLE), ("hThread", w.HANDLE),
                ("dwProcessId", w.DWORD), ("dwThreadId", w.DWORD)]

K.CreateProcessW.argtypes = [
    w.LPCWSTR, w.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, w.BOOL, w.DWORD,
    ctypes.c_void_p, w.LPCWSTR, ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInfo),
]
K.CreateProcessW.restype = w.BOOL
U.CreateDesktopW.argtypes = [w.LPCWSTR, w.LPCWSTR, ctypes.c_void_p, w.DWORD,
                            w.DWORD, ctypes.c_void_p]
U.CreateDesktopW.restype = w.HANDLE

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def require(condition, message):
    if not condition:
        raise live.HarnessFailure(str(message))

TRANSPORT_FIELDS = (
    "structBytes", "apiVersion", "generation", "active", "paused", "mayHold",
    "speedPermille", "pendingSteps", "nextRecord", "endpointRecord", "phase",
    "droppedCommands", "takeoverAvailable", "takeoverPhase", "takeoverLane",
    "takeoverDelay", "takeoverCountdown", "takeoverBranchRecord", "takeoverElapsed",
    "takeoverBeyondEndpoint", "takeoverFault",
)

def read_transport_snapshot(memory, address):
    for unused in range(8):
        index = memory.read_u32(address + 176) & 1
        buffer = address + index * 88
        before = memory.read_u32(buffer)
        if before & 1:
            continue
        words = struct.unpack("<21I", memory.read(buffer + 4, 84))
        after = memory.read_u32(buffer)
        if before != after or after & 1 or index != memory.read_u32(address + 176) & 1:
            continue
        if words[:2] != (84, 2):
            return None
        return dict(zip(TRANSPORT_FIELDS, words))
    return None

class HiddenSession:
    def __init__(self, install: Path, output: Path, record):
        self.install, self.output, self.record = install, output, record
        self.name = f"qoh_checkpoint_{os.getpid()}_{time.monotonic_ns()}"
        self.desktop = U.CreateDesktopW(self.name, None, None, 0, 0x1FF, None)
        require(self.desktop, "CreateDesktopW failed")
        self.caster = self.game = None
        self.streams = []
        self.symbols = {}

    def __enter__(self):
        return self

    def launch(self, arguments: list[str], console=False):
        import msvcrt
        info = StartupInfo()
        info.cb, info.lpDesktop = ctypes.sizeof(info), self.name
        info.dwFlags, info.wShowWindow = 1, 0
        if not console:
            out = self.output.open("wb")
            inp = open(os.devnull, "rb")
            self.streams = [out, inp]
            for stream in self.streams:
                os.set_handle_inheritable(msvcrt.get_osfhandle(stream.fileno()), True)
            info.dwFlags |= 0x100
            info.hStdInput = msvcrt.get_osfhandle(inp.fileno())
            info.hStdOutput = info.hStdError = msvcrt.get_osfhandle(out.fileno())
        command = [str(self.install / "qoh99caster.exe"), *arguments]
        pi = ProcessInfo()
        require(K.CreateProcessW(
            command[0], ctypes.create_unicode_buffer(subprocess.list2cmdline(command)),
            None, None, not console, 0x10 if console else live.CREATE_NO_WINDOW,
            None, str(self.install), ctypes.byref(info), ctypes.byref(pi)),
            "CreateProcessW failed")
        self.caster = live.ProcessMemory(pi.dwProcessId)
        K.CloseHandle(pi.hThread)
        K.CloseHandle(pi.hProcess)
        self.record("launch", pid=self.caster.pid, desktop=self.name, command=command)

    def wait(self, label, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            require(self.caster.alive(), f"caster exited waiting for {label}")
            time.sleep(0.005)
        raise live.HarnessFailure(f"timed out waiting for {label}")

    def attach_game(self, excluded_pid=None):
        if self.game:
            require(not self.game.alive(), "previous game is still running")
            self.game.close()
        def child():
            pid = live.child_game_pid(self.caster.pid, "qoh99.exe")
            return pid if pid and pid != excluded_pid else None
        pid = self.wait("game child", child)
        self.game = live.ProcessMemory(pid)
        module = self.wait("netplay module", lambda: live.module_info(pid, "qoh99_netplay.dll"))
        require(Path(module[2]).resolve() == self.install / "qoh99_netplay.dll",
                "wrong netplay DLL loaded")
        require(Path(live.module_info(pid, "qoh99.exe")[2]).resolve()
                == self.install / "qoh99.exe", "wrong game loaded")
        names = ["g_netFrame@", "g_replayBootstrapPhase@", "g_replayHalted@",
                 "g_replayHitboxesEnabled@", "g_replayHitboxSnapshotValid@", "g_rb@",
                 "g_replayGameStartV2@"]
        self.symbols = {
            name: int(module[0]) + live.parse_map_data_rva(
                self.install / "qoh99_netplay.map", "?" + name) for name in names
        }
        self.symbols["transport"] = int(module[0]) + live.parse_map_data_rva(
            self.install / "qoh99_netplay.map", "?publisher@?1??transportPublisher@")
        self.wait("initial replay frame", lambda: self.frame() > 0
                  and self.byte("g_replayBootstrapPhase@") == 3, 60)
        self.record("game_attached", pid=pid, frame=self.frame(),
                    modules=live.process_module_map(pid))

    def frame(self):
        return self.game.read_u32(self.symbols["g_netFrame@"])

    def byte(self, name):
        return self.game.read(self.symbols[name], 1)[0]

    def transport(self):
        return read_transport_snapshot(self.game, self.symbols["transport"])

    def identity(self):
        low, high, generation = struct.unpack("<QQI", self.game.read(
            self.symbols["g_replayGameStartV2@"], 20))
        return f"{high:016X}{low:016X}", generation

    def roster(self):
        address = self.symbols["g_replayGameStartV2@"] + 442
        return tuple(self.game.read(address + slot * 8 + 2, 1)[0] for slot in range(4))

    def window(self):
        matches, errors = [], []
        def worker():
            try:
                require(U.SetThreadDesktop(self.desktop), "SetThreadDesktop failed")
                @live.WNDENUMPROC
                def visit(hwnd, unused):
                    owner = w.DWORD()
                    U.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                    if owner.value == self.game.pid:
                        title = ctypes.create_unicode_buffer(256)
                        U.GetWindowTextW(hwnd, title, 256)
                        if "QOH" in title.value.upper():
                            matches.append(int(hwnd))
                    return True
                U.EnumDesktopWindows(self.desktop, visit, 0)
            except Exception as exc:
                errors.append(repr(exc))
        thread = threading.Thread(target=worker)
        thread.start()
        thread.join(3)
        require(not thread.is_alive() and not errors and len(matches) == 1,
                f"game window enumeration failed: {errors}, {matches}")
        return matches[0]

    def hotkey(self, scan):
        errors = []
        def worker():
            try:
                require(U.SetThreadDesktop(self.desktop), "SetThreadDesktop failed")
                matches = []
                @live.WNDENUMPROC
                def visit(hwnd, unused):
                    owner = w.DWORD()
                    U.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                    if owner.value == self.game.pid:
                        title = ctypes.create_unicode_buffer(256)
                        U.GetWindowTextW(hwnd, title, 256)
                        if "QOH" in title.value.upper():
                            matches.append(int(hwnd))
                    return True
                U.EnumDesktopWindows(self.desktop, visit, 0)
                require(matches, "game window missing on owned desktop")
                require(U.PostMessageW(matches[0], WM_QOH99_HOTKEY, scan, 1),
                        "PostMessageW hotkey failed")
            except Exception as exc:
                errors.append(repr(exc))
        thread = threading.Thread(target=worker)
        thread.start()
        thread.join(3)
        require(not thread.is_alive() and not errors, errors)

    def stable(self, expected=None):
        value = self.frame()
        time.sleep(0.06)
        require(self.frame() == value, f"pause drift: {value} -> {self.frame()}")
        if expected is not None:
            require(value == expected, f"expected frame {expected}, observed {value}")
        require(self.byte("g_replayHalted@") == 0, "replay halted")
        return value

    def step(self, direction=1):
        target = self.frame() + direction
        started = time.monotonic()
        self.hotkey(KEY_RIGHT if direction > 0 else KEY_LEFT)
        self.wait(f"step to {target}", lambda: self.frame() == target, 10)
        self.stable(target)
        if direction < 0:
            self.record("backward_step", frame=target,
                        elapsed_ms=round((time.monotonic() - started) * 1000, 2))

    def approach(self, target):
        if target - self.frame() > 160:
            self.hotkey(KEY_SPACE)
            self.wait("fast forward", lambda: self.frame() >= target - 128, 100)
            self.hotkey(KEY_SPACE)
            time.sleep(0.08)
            self.stable()
        require(self.frame() <= target, f"overshot target {target}: {self.frame()}")
        while self.frame() < target:
            self.step()

    def gameplay_state(self):
        memory, rb = self.game, self.symbols["g_rb@"]
        def vector(offset, stride):
            begin, end, capacity = struct.unpack("<III", memory.read(rb + offset, 12))
            require(0 < begin <= end <= capacity and (end - begin) % stride == 0,
                    f"invalid RollbackManager vector at +{offset}")
            count = (end - begin) // stride
            require(0 < count < 1024, f"unexpected region count {count}")
            return memory.read(begin, end - begin)
        def name(pointer):
            return memory.read(pointer, 96).split(b"\0", 1)[0].decode("ascii", "replace")
        values = {}
        total_size = memory.read_u32(rb)
        require(0 < total_size < 32 * 1024 * 1024, "invalid rollback size")
        for address, size, label, role in struct.iter_unpack("<IIII", vector(32, 16)):
            if role > 1:
                continue
            require(0 < size <= total_size, "invalid fixed region size")
            data = bytearray(memory.read(address, size))
            if address == 0x62EB80 and size == 0x40024:
                depth = struct.unpack_from("<I", data)[0]
                require(depth < 65536, "invalid AI stack depth")
                start = 0x20 + (depth + 1) * 4
                data[start:0x40020] = b"\0" * (0x40020 - start)
            values["fixed:" + name(label)] = bytes(data)
        for owner, offset, size, label, role, captured, activation in struct.iter_unpack(
                "<IIIIIII", vector(56, 28)):
            if role > 1:
                continue
            require(0 < size <= total_size, "invalid child region size")
            active = not activation or memory.read_u32(activation) != 0
            address = memory.read_u32(owner + offset) if active else 0
            values["child:" + name(label)] = (b"\1" + memory.read(address, size)
                                              if active else b"\0")
        require(len(values) >= 10, "gameplay region coverage unexpectedly small")
        return values

    def state_summary(self, state):
        return {name: hashlib.sha256(data).hexdigest() for name, data in state.items()}

    def same_state(self, expected, label):
        actual = self.gameplay_state()
        differences = []
        for name in sorted(set(expected) | set(actual)):
            before, after = expected.get(name, b""), actual.get(name, b"")
            if before != after:
                offsets = [i for i, (a, b) in enumerate(zip(before, after)) if a != b]
                differences.append({"region": name, "different_bytes": len(offsets),
                                    "first_offsets": offsets[:16],
                                    "before_size": len(before), "after_size": len(after)})
        self.record("state_equality", label=label, frame=self.frame(),
                    region_count=len(actual), equal=not differences, differences=differences,
                    digests=self.state_summary(actual))
        require(not differences, f"gameplay state differs at {label}: {differences}")

    def __exit__(self, exc_type, exc, tb):
        if self.game:
            if self.game.alive():
                try:
                    live.post_close_on_named_desktop(self.game.pid, self.name)
                    until = time.monotonic() + 3
                    while self.game.alive() and time.monotonic() < until:
                        time.sleep(0.03)
                finally:
                    self.game.terminate()
            self.game.close()
        if self.caster:
            child = live.child_game_pid(self.caster.pid, "qoh99.exe")
            if child:
                memory = live.ProcessMemory(child)
                memory.terminate()
                memory.close()
            until = time.monotonic() + 3
            while self.caster.alive() and time.monotonic() < until:
                time.sleep(0.03)
            self.caster.terminate()
            self.caster.close()
        for stream in self.streams:
            stream.close()
        U.CloseDesktop(self.desktop)

def logs_for(install, pid, member="dll.log"):
    paths = list((install / "logs").glob(f"*_p{pid}.zip"))
    require(paths, f"missing session archive for caster {pid}")
    with zipfile.ZipFile(paths[-1]) as archive:
        return archive.read(member).decode("utf-8", "replace")

def assert_ledger(log, record):
    lines = [line for line in log.splitlines() if "[audio-ledger]" in line]
    require(lines, "audio ledger missing")
    for line in lines:
        fields = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
        for key in ("failedClosed", "journalOverflow", "bgmOverflow"):
            require(fields.get(key) == 0, f"audio {key}: {line}")
    record("audio_ledger", text=lines[-1])

def boundary(session, base):
    session.approach(base - 1)
    states = {}
    for frame in range(base - 1, base + 3):
        if session.frame() != frame:
            session.step()
        states[frame] = session.gameplay_state()
    for repetition in range(2):
        for frame in (base + 1, base, base - 1):
            session.step(-1)
            session.same_state(states[frame], f"boundary-{base}-back-{repetition}")
        for frame in (base, base + 1, base + 2):
            session.step()
            session.same_state(states[frame], f"boundary-{base}-forward-{repetition}")

def verify_fixture(install, fixture, frames, output, record):
    with HiddenSession(install, output / (fixture.stem + "-viewer.txt"), record) as session:
        session.launch(["replay", "play", str(fixture), "--game", str(install / "qoh99.exe"),
                        "--dll", str(install / "qoh99_netplay.dll")])
        session.attach_game()
        require(session.stable(1) == 1, "first-frame pause missing")
        first = session.gameplay_state()
        session.same_state(first, "initial-pause-stable")
        require(session.byte("g_replayHitboxesEnabled@") == 0, "hitboxes default enabled")
        session.hotkey(KEY_F7)
        session.wait("custom hitboxes", lambda: session.byte("g_replayHitboxesEnabled@") == 1)
        require(session.game.read(0x4AA424, 1) == b"\0", "native hitboxes enabled")
        boundary(session, 16)
        session.hotkey(KEY_HOME)
        session.wait("Home", lambda: session.frame() == 1)
        session.stable(1)
        session.same_state(first, "Home-after-early-rewind")
        for unused in range(4):
            session.hotkey(KEY_FASTER)
            time.sleep(0.03)
        for base in sorted({4096, ((frames - 100) // 32) * 32}):
            boundary(session, base)
        session.hotkey(KEY_HOME)
        session.wait("late Home", lambda: session.frame() == 1)
        session.stable(1)
        session.same_state(first, "Home-after-late-rewind")
        require(session.byte("g_replayHitboxesEnabled@") == 1, "Home lost display preference")
        session.hotkey(KEY_SPACE)
        session.wait("complete viewer", lambda: not session.caster.alive(), 140)
        require(session.caster.exit_code() == 0, "viewer failed")
        pid = session.caster.pid
    log = logs_for(install, pid)
    (output / (fixture.stem + "-dll.log")).write_text(log, encoding="utf-8")
    seeks = []
    pattern = (r"\[replay.seek\] restored=(\d+) checkpoint=(\d+) target=(\d+) "
               r"hiddenTicks=(\d+) stride=(\d+) bytes=(\d+)")
    for match in re.finditer(pattern, log):
        values = [int(value) for value in match.groups()]
        row = dict(zip(("restored", "checkpoint", "target", "hiddenTicks", "stride", "bytes"), values))
        require(row["restored"] == 1, row)
        require(row["hiddenTicks"] == row["target"] - row["checkpoint"], row)
        require(0 <= row["hiddenTicks"] < row["stride"], row)
        if row["target"] >= 4000:
            require(row["checkpoint"] > 0, row)
        seeks.append(row)
    require(len(seeks) >= 18 and any(row["target"] >= 4000 for row in seeks),
            f"missing checkpoint seek evidence: {len(seeks)} rows")
    assert_ledger(log, record)
    record("checkpoint_seeks", fixture=fixture.name, rows=seeks)
    with HiddenSession(install, output / (fixture.stem + "-verify.txt"), record) as verify:
        verify.launch(["replay", "verify", str(fixture), "--simulate", "--game",
                       str(install / "qoh99.exe"), "--dll", str(install / "qoh99_netplay.dll")])
        verify.wait("headless verification", lambda: not verify.caster.alive(), 140)
        require(verify.caster.exit_code() == 0, "headless verification failed")
        pid = verify.caster.pid
    text = (output / (fixture.stem + "-verify.txt")).read_text(errors="replace")
    require("0 live device call(s)" in text and f"records consumed: {frames}" in text, text[-3000:])
    assert_ledger(logs_for(install, pid), record)
    record("fixture_verified", fixture=fixture.name, output=text)

def console_helper(pid, key, output):
    class Coord(ctypes.Structure):
        _fields_ = [("X", w.SHORT), ("Y", w.SHORT)]
    class Rect(ctypes.Structure):
        _fields_ = [("Left", w.SHORT), ("Top", w.SHORT), ("Right", w.SHORT), ("Bottom", w.SHORT)]
    class Info(ctypes.Structure):
        _fields_ = [("size", Coord), ("cursor", Coord), ("attributes", w.WORD),
                    ("window", Rect), ("maxsize", Coord)]
    class Key(ctypes.Structure):
        _fields_ = [("down", w.BOOL), ("repeat", w.WORD), ("vk", w.WORD),
                    ("scan", w.WORD), ("char", w.WCHAR), ("state", w.DWORD)]
    class Event(ctypes.Union):
        _fields_ = [("key", Key), ("bytes", ctypes.c_byte * 16)]
    class Input(ctypes.Structure):
        _fields_ = [("kind", w.WORD), ("event", Event)]
    K.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                             w.DWORD, w.DWORD, w.HANDLE]
    K.CreateFileW.restype = w.HANDLE
    K.GetConsoleScreenBufferInfo.argtypes = [w.HANDLE, ctypes.POINTER(Info)]
    K.ReadConsoleOutputCharacterW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD, Coord, ctypes.POINTER(w.DWORD)]
    K.WriteConsoleInputW.argtypes = [w.HANDLE, ctypes.POINTER(Input), w.DWORD, ctypes.POINTER(w.DWORD)]
    result, handles = {}, []
    try:
        K.FreeConsole()
        require(K.AttachConsole(pid), "AttachConsole failed")
        if key:
            handle = K.CreateFileW("CONIN$", 0xC0000000, 3, None, 3, 0, None)
            handles.append(handle)
            vk, char = (27, "\x1b") if key == "ESC" else (ord(key.upper()), key.lower())
            events = (Input * 2)()
            for i in range(2):
                events[i].kind = 1
                events[i].event.key = Key(i == 0, 1, vk, 0, char, 0)
            written = w.DWORD()
            require(K.WriteConsoleInputW(handle, events, 2, ctypes.byref(written)), "console write failed")
        handle = K.CreateFileW("CONOUT$", 0x80000000, 3, None, 3, 0, None)
        handles.append(handle)
        info = Info()
        require(K.GetConsoleScreenBufferInfo(handle, ctypes.byref(info)), "console info failed")
        size = min(info.size.X * info.size.Y, 200000)
        buffer, read = ctypes.create_unicode_buffer(size + 1), w.DWORD()
        require(K.ReadConsoleOutputCharacterW(handle, buffer, size, Coord(0, 0), ctypes.byref(read)),
                "console read failed")
        result["text"] = "\n".join(buffer.value[i:i + info.size.X].rstrip()
                                    for i in range(0, len(buffer.value), info.size.X)).rstrip()
    except Exception as exc:
        result["error"] = repr(exc)
    finally:
        for handle in handles:
            K.CloseHandle(handle)
        K.FreeConsole()
        output.write_text(json.dumps(result, indent=2), encoding="utf-8")

def verify_folder(install, fixtures, output, record, pause=True, repeat_first=False):
    require(len(fixtures) >= 2, "folder regression needs two different-roster fixtures")
    queue = output / "queue"
    queue.mkdir()
    selected = fixtures + ([fixtures[0]] if repeat_first else [])
    expected, frame_counts = [], {}
    for i, fixture in enumerate(selected, 1):
        shutil.copy2(fixture, queue / f"game-{i}.q99r")
        inspected = (output / (fixture.stem + "-inspect.txt")).read_text(encoding="utf-8")
        uuid = re.search(r"game uuid\s*:\s*([0-9A-Fa-f]{32})", inspected)
        frames = re.search(r"frames:\s*(\d+)", inspected)
        require(uuid and frames, "fixture inspection is missing identity/frame count")
        expected.append(uuid[1].upper())
        frame_counts[uuid[1].upper()] = int(frames[1])
    count = len(selected)
    (install / "qoh99caster.ini").write_text(
        f"[qoh99caster]\ngamePath={install / 'qoh99.exe'}\ndllPath={install / 'qoh99_netplay.dll'}\n"
        f"replayRoot={queue}\nreplayPauseAtStart={int(pause)}\n", encoding="utf-8")
    observed, rosters = [], set()
    with HiddenSession(install, output / "folder-console.json", record) as session:
        session.launch(["replays"], console=True)
        def console(key=None):
            command = [sys.executable, str(Path(__file__).resolve()), "--console-helper",
                       str(session.caster.pid), "--console-output", str(session.output)]
            if key:
                command.extend(["--console-key", key])
            subprocess.run(command, check=True, timeout=8, creationflags=live.CREATE_NO_WINDOW)
            value = json.loads(session.output.read_text(encoding="utf-8"))
            require("error" not in value, value)
            return value["text"]
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            screen = console()
            if f"{count} replays" in screen and screen.count("Ready") >= count:
                break
            time.sleep(0.25)
        else:
            raise live.HarnessFailure("browser never classified the folder fixtures Ready")
        record("folder_ready", screen=screen, pause_at_start=pause, expected_uuids=expected)
        console("P")
        session.attach_game()
        pid, hwnd = session.game.pid, session.window()
        status = session.wait("published replay transport", session.transport)
        if pause:
            session.stable(1)
            session.wait("first replay pause status",
                         lambda: (session.transport() or {}).get("paused") == 1)
        else:
            require(status["paused"] == 0, "first replay paused with preference disabled")
            start = status["nextRecord"]
            session.wait("first replay autoplay", lambda: session.frame() > start + 3)
            session.hotkey(KEY_SPACE)
            session.wait("first replay setup pause", lambda: (session.transport() or {}).get("paused") == 1)
            session.stable()
        require(session.byte("g_replayHitboxesEnabled@") == 0, "folder hitbox default wrong")
        session.hotkey(KEY_INPUTS)
        session.hotkey(KEY_F7)
        session.wait("first replay hitboxes", lambda: session.byte("g_replayHitboxesEnabled@") == 1)
        for unused in range(3):
            session.hotkey(KEY_FASTER)
            time.sleep(0.03)
        session.wait("8x replay speed", lambda: (session.transport() or {}).get("speedPermille") == 8000)
        session.hotkey(KEY_SPACE)
        for ordinal in range(1, count + 1):
            require(session.game.alive() and session.window() == hwnd, "folder replaced its game window")
            status = session.wait("playing transport", lambda: session.transport())
            uuid, generation = session.identity()
            require(uuid in frame_counts, f"unexpected folder source {uuid}")
            require(status["endpointRecord"] == frame_counts[uuid], "wrong per-file endpoint")
            observed.append(uuid)
            roster = session.roster()
            require(all(character <= 33 or character == 255 for character in roster), "invalid roster layout")
            rosters.add(roster)
            record("folder_game", game=ordinal, pid=pid, hwnd=hwnd, uuid=uuid,
                   recorded_generation=generation, roster=roster, transport=status)
            if ordinal > 1:
                require(status["paused"] == 0, "later replay did not autostart")
                require(status["speedPermille"] == 8000, "folder lost replay speed")
                start = session.frame()
                session.wait("later replay advancing", lambda: session.frame() > start + 3)
                require(session.byte("g_replayHitboxesEnabled@") == 1, "folder lost hitbox preference")
                require(session.game.read(0x4AA424, 1) == b"\0", "native hitboxes enabled")
                record("folder_autostart", game=ordinal, pid=pid, hwnd=hwnd,
                       before=start, after=session.frame(), speed=status["speedPermille"])
            if ordinal == 2:
                session.hotkey(KEY_SPACE)
                session.wait("second replay pause", lambda: (session.transport() or {}).get("paused") == 1)
                frame = session.stable()
                before_home = session.gameplay_state()
                session.hotkey(KEY_HOME)
                session.wait("second replay Home", lambda: session.frame() == 1
                             and (session.transport() or {}).get("paused") == 1)
                session.stable(1)
                session.approach(frame)
                session.same_state(before_home, "second-file-Home-reproduces-original-frame")
                session.step()
                next_state = session.gameplay_state()
                session.step(-1)
                session.same_state(before_home, "second-file-backstep")
                session.step()
                session.same_state(next_state, "second-file-forward-after-backstep")
                require(session.identity()[0] == uuid, "seek restored another replay's identity")
                require(session.transport()["speedPermille"] == 8000, "seek lost carried speed")
                session.hotkey(KEY_SPACE)
            status = session.wait("resumed transport", lambda: (value if (value := session.transport())
                                  and not value["paused"] else None))
            old_generation = status["generation"]
            settled_at, advanced = None, False
            deadline = time.monotonic() + 100
            while session.game.alive() and time.monotonic() < deadline:
                try:
                    status = session.transport()
                    if (ordinal < count and status and status["generation"] > old_generation
                            and status["nextRecord"] > 0 and session.byte("g_replayBootstrapPhase@") == 3):
                        advanced = True
                        break
                    if (status and status["generation"] == old_generation
                            and status["endpointRecord"] > 0
                            and status["nextRecord"] == status["endpointRecord"]
                            and session.game.read_u32(0x6234EC) == 2):
                        sub = session.game.read_u32(0x8719A4)
                        counter = session.game.read_u32(0x6236C8)
                        settled = all(session.game.read_u32(0x8A5628 + 20 * i) == 0 for i in range(9))
                        if sub == 5 or (sub == 2 and counter >= 61):
                            require(session.byte("g_replayHitboxesEnabled@") == 0,
                                    "winscreen retained custom hitboxes")
                        if (sub == 5 or (sub == 2 and counter >= 61 and settled)) and settled_at is None:
                            settled_at = time.monotonic()
                            record("folder_settled", game=ordinal, pid=pid, hwnd=hwnd,
                                   frame=session.frame(), sub=sub, counter=counter)
                except OSError:
                    if session.game.alive():
                        raise
                time.sleep(0.005)
            if ordinal < count:
                require(advanced and session.game.alive(), "folder relaunched/exited instead of advancing in process")
                require(session.window() == hwnd, "folder replaced HWND during native teardown")
                record("folder_handoff", game=ordinal, pid=pid, hwnd=hwnd,
                       previous_generation=old_generation, next_generation=status["generation"],
                       native_settled_observed=settled_at is not None)
            else:
                require(not session.game.alive(), "final folder game did not exit")
                require(settled_at is not None, "final settled winscreen was not observed")
                elapsed = time.monotonic() - settled_at
                require(elapsed >= 1, f"final winscreen held only {elapsed}s")
                record("folder_game_closed", game=ordinal, pid=pid, hwnd=hwnd, settled_seconds=elapsed)
        require(Counter(observed) == Counter(expected), "folder skipped, duplicated, or substituted a file")
        require(len(rosters) >= 2, "folder regression requires at least two different rosters")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            screen = console()
            if f"Folder playback finished: {count} game(s)." in screen:
                break
            time.sleep(0.1)
        else:
            raise live.HarnessFailure(screen)
        require(live.child_game_pid(session.caster.pid, "qoh99.exe") is None,
                "folder left a child running")
        record("folder_finished", screen=screen, pid=pid, hwnd=hwnd, games=count)
        console("ESC")
        session.wait("browser exit", lambda: not session.caster.alive(), 10)
        require(session.caster.exit_code() == 0, "browser exit failed")
        caster_pid = session.caster.pid
    dll_log = logs_for(install, caster_pid)
    client_log = logs_for(install, caster_pid, "client.log")
    (output / "folder-dll.log").write_text(dll_log, encoding="utf-8")
    (output / "folder-client.log").write_text(client_log, encoding="utf-8")
    switches = [int(value) for value in re.findall(r"next game accepted switch=(\d+)", dll_log)]
    require(switches == list(range(1, count)), f"wrong handoff sequence: {switches}")
    require(dll_log.count("[replay.v3] bootstrap verified") == count, "missing fresh per-file bootstrap")
    require(dll_log.count("[replay.presentation] win screen finished") == count, "unfinished native winscreen")
    results = re.findall(r"result accepted reason=(\d+) consumed=(\d+)/(\d+)", client_log)
    require(len(results) == count, f"expected {count} independently bound results, got {results}")
    for uuid, (reason, consumed, endpoint) in zip(observed, results):
        require(int(reason) in (1, 15, 16) and int(consumed) == int(endpoint) == frame_counts[uuid],
                f"invalid per-file completion: {reason}, {consumed}, {endpoint}")
    launches = re.findall(r"\[replay.play.v2\] launch ok=1 pid=(\d+)", client_log)
    require(launches == [str(pid)], f"folder launched multiple games: {launches}")
    flags = re.findall(r"\[replay.folder\] next game=(\d+)/(\d+) frames=\d+ flags=(\d+)", client_log)
    require(len(flags) == count - 1, "missing carried display preference evidence")
    for ordinal, total, bits in flags:
        bits = int(bits)
        require(bits & 29 == 29, "next game lost autoplay/input/transport/hitbox settings")
        require(bool(bits & 2) == (int(ordinal) == int(total)), "final hold set on wrong replay")
    assert_ledger(dll_log, record)
    record("folder_proof", pid=pid, hwnd=hwnd, switches=switches, results=results, flags=flags)

def main(args):
    source, build, output = args.game_install.resolve(), args.build.resolve(), args.output.resolve()
    require(source != output and source not in output.parents,
            "output must be outside the source install")
    require(os.environ.get("QOH99_REPLAY_PROOF_OBSERVE") != "diagnose", "proof-observe mode is forbidden")
    output.mkdir(parents=True, exist_ok=False)
    report = {"events": [], "source": str(source), "build": str(build), "output": str(output)}
    def record(event, **fields):
        row = {"event": event, "time": time.time(), **fields}
        report["events"].append(row)
        print(json.dumps({key: value for key, value in row.items()
                          if key not in {"digests", "modules", "screen", "output"}}), flush=True)
    config = source / "System/QOHcnf.key"
    report["source_config_before"] = sha(config)
    install = output / "game"
    try:
        report["clone"] = live.clone_game_install(source, install)
        report["audio_muting"] = live.mute_test_install_audio(install)
        for name in (".qoh99_run_in_progress", "qoh99caster-controls.json"):
            (install / name).unlink(missing_ok=True)
        (install / "ddraw_wrapper.ini").write_text(
            "[Display]\nFpsLimit=60\nWindowScale=1\n[Window]\nPresentationMode=0\n"
            "ClientWidth=640\nClientHeight=480\n[Renderer]\nBackend=0\n")
        (install / "qoh99caster.ini").write_text(
            "[qoh99caster]\nreplayPauseAtStart=1\n", encoding="utf-8")
        artifacts = {name: build / "Release" / name for name in
                     ("qoh99caster.exe", "qoh99_netplay.dll", "qoh99_netplay.map")}
        artifacts["ddraw.dll"] = build / "ddraw_wrapper/Release/ddraw.dll"
        for name, path in artifacts.items():
            shutil.copy2(path, install / name)
        report["artifact_hashes"] = {name: sha(install / name) for name in artifacts}
        replay_dir = install / "replays"
        replay_dir.mkdir()
        fixtures = []
        for index, original in enumerate(args.replay):
            fixture = replay_dir / f"{index:02d}_{original.name}"
            shutil.copy2(original.resolve(), fixture)
            inspected = subprocess.run(
                [str(install / "qoh99caster.exe"), "replay", "inspect", str(fixture)],
                cwd=install, capture_output=True, text=True, timeout=20,
                creationflags=live.CREATE_NO_WINDOW)
            text = inspected.stdout + inspected.stderr
            match = re.search(r"frames:\s*(\d+)", text)
            require(inspected.returncode == 0 and match and re.search(r"terminal\s*:\s*1\s", text),
                    f"fixture must be a completed recording: {text}")
            frames = int(match[1])
            (output / (fixture.stem + "-inspect.txt")).write_text(text, encoding="utf-8")
            require(frames > (160 if args.folder_only else 4300),
                    "fixture too short for the selected replay checks")
            record("fixture", path=str(original.resolve()), sha256=sha(fixture), frames=frames)
            fixtures.append(fixture)
            if not args.folder_only:
                verify_fixture(install, fixture, frames, output, record)
        if args.folder or args.folder_only:
            verify_folder(install, fixtures, output, record,
                          pause=args.folder_pause, repeat_first=args.folder_repeat_first)
        report["passed"] = True
    except Exception as exc:
        report["error"] = repr(exc)
        record("failed", error=repr(exc))
        raise
    finally:
        report["source_config_after"] = sha(config)
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        require(report["source_config_after"] == report["source_config_before"],
                "source config unexpectedly changed")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-install", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--replay", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--folder", action="store_true")
    parser.add_argument("--folder-only", action="store_true",
                        help="run the same-process folder checks without the long checkpoint runs")
    parser.add_argument("--folder-pause", action=argparse.BooleanOptionalAction, default=True,
                        help="first-replay pause setting for the folder (--no-folder-pause tests autoplay)")
    parser.add_argument("--folder-repeat-first", action="store_true",
                        help="include another unchanged copy of the first fixture in the folder")
    parser.add_argument("--console-helper", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--console-output", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--console-key", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.console_helper:
        console_helper(args.console_helper, args.console_key, args.console_output)
    else:
        if not all((args.game_install, args.build, args.replay, args.output)):
            parser.error("--game-install, --build, --replay and --output are required")
        main(args)
