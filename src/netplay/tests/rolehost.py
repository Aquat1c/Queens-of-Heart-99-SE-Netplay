
from __future__ import annotations

import base64
import json
import os
from pathlib import Path, PureWindowsPath
import queue
import shlex
import socket
import struct
import subprocess
import threading
import time
from typing import Callable, Iterable, Sequence

AGENT_PROTOCOL_VERSION = 1

DEFAULT_AGENT_DEBUG_PORT = 47311

DEFAULT_RPC_TIMEOUT_SECONDS = 10.0
BULK_RPC_TIMEOUT_SECONDS = 900.0
DEFAULT_LEASE_MS = 30000
HEARTBEAT_INTERVAL_SECONDS = 5.0

class RoleHostFailure(RuntimeError):
    pass

class LocalBackend:

    def __init__(self, **members: object) -> None:
        required = (
            "failure",
            "process_memory",
            "process_control",
            "child_game_pid",
            "module_info",
            "process_module_map",
            "process_windows",
            "window_titles",
            "window_thread_process_id",
            "is_window",
            "post_close_direct",
            "post_close_on_named_desktop",
            "open_thread_suspended",
            "sha256",
            "clone_game_install",
            "clone_payload_bytes",
            "validate_runtime_asset_paths",
            "mute_test_install_audio",
            "qoh_selected_profile_snapshot",
            "mutate_qoh_selected_profile",
            "game_state_from_memory",
        )
        missing = [name for name in required if name not in members]
        if missing:
            raise RoleHostFailure(
                f"LocalBackend is missing harness members: {missing}"
            )
        for name, value in members.items():
            setattr(self, name, value)

class RoleHost:

    remote = False
    label = "local"

    def handshake(self) -> dict[str, object]:
        return {"placement": self.label, "remote": self.remote}

    def heartbeat(self) -> None:
        return None

    def shutdown(self) -> None:
        return None

    def path(self, value: str | os.PathLike[str]) -> Path:
        return Path(value)

class LocalRoleHost(RoleHost):

    remote = False
    label = "local"

    def __init__(self, backend: LocalBackend) -> None:
        self.backend = backend

    def spawn(
        self,
        argv: Sequence[str],
        cwd: Path,
        env: dict[str, str] | None,
        stdout_handle: object,
        stderr_handle: object,
    ) -> subprocess.Popen[bytes]:
        return subprocess.Popen(
            list(argv),
            cwd=cwd,
            stdout=stdout_handle,
            stderr=stderr_handle,
            env=env,
            creationflags=self.backend.process_control.creation_flags,
        )

    def open_log_handle(self, path: Path):
        return path.open("wb")

    def stop(
        self,
        process: subprocess.Popen[bytes],
        terminate_timeout: float = 3.0,
        kill_timeout: float = 2.0,
    ) -> None:
        self.backend.process_control.stop(
            process, terminate_timeout, kill_timeout
        )

    def child_game_pid(self, parent_pid: int, image_name: str) -> int | None:
        return self.backend.child_game_pid(parent_pid, image_name)

    def open_process(
        self, pid: int, request_set_information: bool = False
    ) -> object:
        return self.backend.process_memory(
            pid, request_set_information=request_set_information
        )

    def module_base(
        self, pid: int, module_name: str
    ) -> tuple[int, int, str] | None:
        return self.backend.module_info(pid, module_name)

    def process_module_map(self, pid: int) -> list[dict[str, object]]:
        return self.backend.process_module_map(pid)

    def game_state(self, memory: object) -> dict[str, object]:
        return self.backend.game_state_from_memory(memory)

    def open_thread_suspended(
        self, pid: int, tid: int, request_context: bool
    ) -> object:
        return self.backend.open_thread_suspended(pid, tid, request_context)

    def process_windows(self, pid: int) -> list[tuple[int, str]]:
        return self.backend.process_windows(pid)

    def window_titles(self, pid: int) -> list[str]:
        return self.backend.window_titles(pid)

    def window_thread_process_id(self, hwnd: int) -> tuple[int, int]:
        return self.backend.window_thread_process_id(hwnd)

    def is_window(self, hwnd: int) -> bool:
        return self.backend.is_window(hwnd)

    def post_close_direct(self, hwnd: int) -> tuple[bool, int]:
        return self.backend.post_close_direct(hwnd)

    def post_close_on_named_desktop(
        self, pid: int, desktop_name: str
    ) -> list[dict[str, object]]:
        return self.backend.post_close_on_named_desktop(pid, desktop_name)

    def disk_usage(self, path: Path) -> tuple[int, int, int]:
        usage = __import__("shutil").disk_usage(path)
        return int(usage.total), int(usage.used), int(usage.free)

    def sha256(self, path: Path) -> str:
        return self.backend.sha256(path)

    def stat(self, path: Path) -> dict[str, object]:
        try:
            info = path.stat()
        except OSError:
            return {"exists": False, "size": 0, "mtime_ns": 0, "is_dir": False}
        return {
            "exists": True,
            "size": int(info.st_size),
            "mtime_ns": int(info.st_mtime_ns),
            "is_dir": path.is_dir(),
        }

    def glob(self, root: Path, pattern: str) -> list[Path]:
        return sorted(root.glob(pattern))

    def read_bytes(self, path: Path) -> bytes:
        return path.read_bytes()

    def write_bytes(self, path: Path, data: bytes) -> None:
        path.write_bytes(data)

    def write_text(self, path: Path, text: str, encoding: str) -> None:
        path.write_text(text, encoding=encoding)

    def mkdir(self, path: Path, parents: bool = False, exist_ok: bool = False):
        path.mkdir(parents=parents, exist_ok=exist_ok)

    def copy(self, source: Path, destination: Path) -> None:
        __import__("shutil").copy2(source, destination)

    def clone_game_install(
        self, source: Path, destination: Path
    ) -> dict[str, int]:
        return self.backend.clone_game_install(source, destination)

    def clone_payload_bytes(self, source: Path) -> tuple[int, int]:
        return self.backend.clone_payload_bytes(source)

    def validate_asset_paths(self, install: Path) -> dict[str, object]:
        return self.backend.validate_runtime_asset_paths(install)

    def mute_test_install_audio(self, install: Path) -> dict[str, object]:
        return self.backend.mute_test_install_audio(install)

    def config_snapshot(
        self, path: Path, offsets: tuple[int, ...]
    ) -> dict[str, object]:
        return self.backend.qoh_selected_profile_snapshot(path, offsets)

    def config_mutate(
        self, path: Path, profile: int, values: dict[int, int]
    ) -> dict[str, object]:
        return self.backend.mutate_qoh_selected_profile(path, profile, values)

    def run(
        self,
        argv: Sequence[str],
        cwd: Path,
        timeout: float,
    ) -> tuple[int, str]:
        completed = subprocess.run(
            list(argv),
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return completed.returncode, completed.stdout

    def reserve_relay_port(self, bind: str) -> int:
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
        raise self.backend.failure("could not reserve a TCP+UDP relay port")

    def reserve_game_port(self, bind: str) -> int:
        for port in range(46000, 46998):
            first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                first.bind((bind, port))
            except OSError:
                first.close()
                continue
            first.close()
            return port
        raise self.backend.failure(
            "could not reserve a UDP game/control port"
        )

class RemoteProcHandle:

    def __init__(self, client: "AgentClient", token: str, pid: int) -> None:
        self._client = client
        self._token = token
        self.pid = int(pid)
        self.returncode: int | None = None

    def poll(self) -> int | None:
        if self.returncode is not None:
            return self.returncode
        reply = self._client.call("proc_poll", token=self._token)
        if reply.get("running"):
            return None
        self.returncode = int(reply["returncode"])
        return self.returncode

    def wait(self, timeout: float | None = None) -> int:
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            code = self.poll()
            if code is not None:
                return code
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(self._token, timeout or 0.0)
            time.sleep(0.02)

    def terminate(self) -> None:
        self._client.call("proc_stop", token=self._token, force=False)

    def kill(self) -> None:
        self._client.call("proc_stop", token=self._token, force=True)

class RemoteMemory:

    def __init__(self, client: "AgentClient", handle: str, pid: int) -> None:
        self._client = client
        self._handle: str | None = handle
        self.pid = int(pid)
        self.suspended = False

    def _call(self, verb: str, **payload: object) -> dict[str, object]:
        if self._handle is None:
            raise self._client.failure(
                f"cannot {verb} on closed remote process {self.pid}"
            )
        return self._client.call(verb, handle=self._handle, **payload)

    def close(self) -> None:
        if self._handle is not None:
            self._client.call("close_process", handle=self._handle)
            self._handle = None

    def alive(self) -> bool:
        if self._handle is None:
            return False
        return bool(self._call("proc_alive")["alive"])

    def exit_code(self) -> int:
        return int(self._call("proc_exit_code")["code"])

    def set_affinity(self, mask: int) -> None:
        self._call("proc_set_affinity", mask=int(mask))

    def read(self, address: int, size: int) -> bytes:
        reply = self._call("mem_read", address=int(address), size=int(size))
        return base64.b64decode(str(reply["data"]))

    def write(self, address: int, data: bytes) -> None:
        self._call(
            "mem_write",
            address=int(address),
            data=base64.b64encode(data).decode("ascii"),
        )

    def read_u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]

    def read_i32(self, address: int) -> int:
        return struct.unpack("<i", self.read(address, 4))[0]

    def write_u32(self, address: int, value: int) -> None:
        self.write(address, struct.pack("<I", value & 0xFFFFFFFF))

    def suspend(self) -> None:
        if self.suspended:
            return
        self._call("proc_suspend")
        self.suspended = True

    def resume(self) -> None:
        if not self.suspended:
            return
        self._call("proc_resume")
        self.suspended = False

    def terminate(self) -> None:
        if self.alive():
            self._call("proc_terminate")

    def read_many(
        self, specs: Sequence[tuple[str, int, str]]
    ) -> dict[str, int]:
        reply = self._call(
            "mem_read_many",
            specs=[
                {"name": name, "address": int(address), "type": kind}
                for name, address, kind in specs
            ],
        )
        return {
            str(name): int(value)
            for name, value in dict(reply.get("values") or {}).items()
        }

    def sample_game_state(
        self, specs: Sequence[tuple[str, int, str]]
    ) -> dict[str, object]:
        reply = self._call(
            "sample_game_state",
            specs=[
                {"name": name, "address": int(address), "type": kind}
                for name, address, kind in specs
            ],
        )
        return dict(reply)

class RemoteSuspendedThread:

    def __init__(
        self, client: "AgentClient", token: str, pid: int, tid: int
    ) -> None:
        self._client = client
        self._token = token
        self.pid = int(pid)
        self.tid = int(tid)
        self.handle = 0
        self.active = True

    def resume(self) -> None:
        if not self.active:
            return
        self.active = False
        self._client.call("thread_resume", token=self._token)

class AgentClient:

    def __init__(
        self,
        writer,
        reader,
        failure: Callable[[str], BaseException],
        label: str,
    ) -> None:
        self._writer = writer
        self._reader = reader
        self._lock = threading.Lock()
        self._next_id = 1
        self.failure = failure
        self.label = label
        self._replies: "queue.Queue[bytes | None]" = queue.Queue()
        self._pump = threading.Thread(
            target=self._pump_replies,
            name=f"qoh99-agent-reader-{label}",
            daemon=True,
        )
        self._pump.start()

    def _pump_replies(self) -> None:
        try:
            for raw in iter(self._reader.readline, b""):
                self._replies.put(raw)
        except (OSError, ValueError):
            pass
        finally:
            self._replies.put(None)

    def call(
        self,
        verb: str,
        timeout: float = DEFAULT_RPC_TIMEOUT_SECONDS,
        **payload: object,
    ) -> dict[str, object]:
        request = {"id": self._next_id, "verb": verb}
        request.update(payload)
        line = json.dumps(request, separators=(",", ":")) + "\n"
        with self._lock:
            self._next_id += 1
            try:
                self._writer.write(line.encode("utf-8"))
                self._writer.flush()
            except OSError as exc:
                raise self.failure(
                    f"{self.label} agent RPC {verb!r} failed on the wire: "
                    f"{type(exc).__name__}: {exc}"
                ) from exc
            try:
                raw = self._replies.get(timeout=timeout)
            except queue.Empty:
                raise self.failure(
                    f"{self.label} agent did not answer {verb!r} within "
                    f"{timeout:.1f}s; treating the remote box as lost rather "
                    "than waiting on it"
                ) from None
        if not raw:
            raise self.failure(
                f"{self.label} agent closed the control channel during "
                f"{verb!r}; the remote box is no longer usable"
            )
        try:
            reply = json.loads(raw.decode("utf-8"))
        except ValueError as exc:
            raise self.failure(
                f"{self.label} agent returned unparseable JSON for {verb!r}: "
                f"{raw[:200]!r}"
            ) from exc
        if reply.get("id") != request["id"]:
            raise self.failure(
                f"{self.label} agent reply id {reply.get('id')} does not "
                f"match request {request['id']} ({verb!r}); the serial "
                "contract is broken"
            )
        if reply.get("error"):
            raise self.failure(
                f"{self.label} agent {verb!r} failed: {reply['error']}"
            )
        return dict(reply.get("result") or {})

    def close(self) -> None:
        for stream in (self._writer, self._reader):
            try:
                stream.close()
            except OSError:
                pass

class SshRoleHost(RoleHost):

    remote = True

    def __init__(
        self,
        *,
        name: str,
        target: str,
        lab: str,
        agent_port: int,
        drive: str,
        display: str,
        password: str | None,
        scratch: Path,
        failure: Callable[[str], BaseException],
        lease_ms: int = DEFAULT_LEASE_MS,
        connect_timeout: float = 60.0,
    ) -> None:
        self.label = f"ssh:{target}"
        self.name = name
        self.target = target
        self.lab = lab.rstrip("/")
        self.agent_port = int(agent_port)
        self.drive = drive.rstrip("\\").rstrip(":") + ":"
        self.display = display
        self.scratch = scratch
        self.failure = failure
        self.lease_ms = int(lease_ms)
        self._password = password
        self._askpass: Path | None = None
        self._forward: subprocess.Popen[bytes] | None = None
        self._forward_log: Path | None = None
        self._forward_handle = None
        self._agent_launch: subprocess.Popen[bytes] | None = None
        self.client: AgentClient | None = None
        self._heartbeat_stop = threading.Event()
        self._heartbeat_thread: threading.Thread | None = None
        self.handshake_result: dict[str, object] = {}
        self._connect_timeout = connect_timeout
        self._local_port = 0
        self._drive_root: str | None = None

    def _ssh_environment(self) -> dict[str, str]:
        env = os.environ.copy()
        if self._password:
            if self._askpass is None:
                helper = self.scratch / f"askpass-{self.name}.sh"
                helper.write_text(
                    '#!/bin/sh\nprintf "%s\\n" "$QOH_PW"\n',
                    encoding="ascii",
                )
                try:
                    helper.chmod(0o700)
                except OSError:
                    pass
                self._askpass = helper
            env["QOH_PW"] = self._password
            env["SSH_ASKPASS"] = str(self._askpass)
            env["SSH_ASKPASS_REQUIRE"] = "force"
            env.pop("DISPLAY", None)
        return env

    def _ssh_base(self) -> list[str]:
        return [
            "ssh",
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-o", "BatchMode=no",
            "-o", "ConnectTimeout=15",
            "-o", "ServerAliveInterval=5",
            "-o", "ServerAliveCountMax=3",
        ]

    def ssh_run(
        self, command: str, timeout: float = 60.0
    ) -> tuple[int, str, str]:
        completed = subprocess.run(
            [*self._ssh_base(), self.target, command],
            env=self._ssh_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return completed.returncode, completed.stdout, completed.stderr

    def deploy_wrapper(self) -> str:
        source = Path(__file__).resolve().parent / "remote" / "naslab_agent.sh"
        if not source.is_file():
            raise self.failure(
                f"{self.name}: the remote wrapper is missing from this "
                f"checkout: {source}"
            )
        rc, _out, err = self.ssh_run(
            f"mkdir -p {shlex.quote(self.lab)}", timeout=30.0
        )
        if rc != 0:
            raise self.failure(
                f"{self.name}: could not create lab directory {self.lab}: "
                f"{err.strip()!r}"
            )
        self._scp(source, f"{self.lab}/naslab_agent.sh")
        rc, _out, err = self.ssh_run(
            f"sed -i 's/\\r$//' {shlex.quote(self.lab + '/naslab_agent.sh')}",
            timeout=30.0,
        )
        if rc != 0:
            raise self.failure(
                f"{self.name}: could not normalise the deployed wrapper: "
                f"{err.strip()!r}"
            )
        return _sha256_file(source)

    def drive_root(self) -> str:
        if self._drive_root is not None:
            return self._drive_root
        rc, out, err = self.ssh_run(
            f"sh {shlex.quote(self.lab + '/naslab_agent.sh')} drive-root",
            timeout=60.0,
        )
        resolved = out.strip().splitlines()[-1].strip() if out.strip() else ""
        if rc != 0 or not resolved.startswith("/"):
            raise self.failure(
                f"{self.name}: could not resolve the {self.drive} drive root "
                f"on the remote box (rc={rc} stdout={out.strip()!r} "
                f"stderr={err.strip()!r})"
            )
        self._drive_root = resolved
        return resolved

    def _scp(self, source: Path, remote_target: str) -> None:
        completed = subprocess.run(
            [
                "scp",
                "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=/dev/null",
                "-o", "ConnectTimeout=15",
                str(source),
                f"{self.target}:{remote_target}",
            ],
            env=self._ssh_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=600.0,
            check=False,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if completed.returncode != 0:
            raise self.failure(
                f"{self.name}: scp of {source.name} failed: "
                f"{completed.stderr.strip()!r}"
            )

    def stage(self, files: dict[str, Path], remote_dir: str) -> dict[str, str]:
        rc, _out, err = self.ssh_run(
            f"mkdir -p {shlex.quote(remote_dir)}", timeout=30.0
        )
        if rc != 0:
            raise self.failure(
                f"{self.name}: could not create remote staging directory "
                f"{remote_dir}: {err.strip()!r}"
            )
        digests: dict[str, str] = {}
        for name, path in files.items():
            if not path.is_file():
                raise self.failure(
                    f"{self.name}: staging source {name} is missing: {path}"
                )
            self._scp(path, f"{remote_dir}/{path.name}")
            rc, out, err = self.ssh_run(
                f"sha256sum {shlex.quote(remote_dir + '/' + path.name)}",
                timeout=120.0,
            )
            if rc != 0:
                raise self.failure(
                    f"{self.name}: could not hash staged {path.name}: "
                    f"{err.strip()!r}"
                )
            remote_digest = out.split()[0] if out.split() else ""
            local_digest = _sha256_file(path)
            if remote_digest != local_digest:
                raise self.failure(
                    f"{self.name}: staged {path.name} hashes "
                    f"{remote_digest} on the remote box but {local_digest} "
                    "here; the copy is corrupt"
                )
            digests[name] = local_digest
        return digests

    def start(self, agent_source: Path, staged: dict[str, Path]) -> None:
        self._forward_log = self.scratch / f"ssh-agent-{self.name}.log"
        self._forward_handle = self._forward_log.open("wb")
        remote_command = (
            f"sh {shlex.quote(self.lab + '/naslab_agent.sh')} "
            f"stdio {self.lease_ms} "
            f"2>> {shlex.quote(self.lab + '/agent.boot.log')}"
        )
        self._forward = subprocess.Popen(
            [*self._ssh_base(), self.target, remote_command],
            env=self._ssh_environment(),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self._forward_handle,
        )

        candidate = AgentClient(
            self._forward.stdin, self._forward.stdout, self.failure, self.name
        )
        last_error: BaseException | None = None
        try:
            self.handshake_result = candidate.call(
                "handshake",
                protocol=AGENT_PROTOCOL_VERSION,
                lease_ms=self.lease_ms,
                timeout=self._connect_timeout,
            )
            self.client = candidate
        except BaseException as exc:
            if "protocol mismatch" in str(exc):
                raise
            last_error = exc
        if self.client is None:
            transcript = ""
            try:
                self._forward_handle.flush()
                lines = self._forward_log.read_text(
                    encoding="utf-8", errors="replace"
                ).splitlines()
                transcript = "\n".join(lines[-12:])
            except OSError:
                pass
            rc, boot, _err = self.ssh_run(
                f"tail -20 {shlex.quote(self.lab + '/agent.boot.log')}",
                timeout=30.0,
            )
            raise self.failure(
                f"{self.name}: the remote agent never completed a handshake "
                f"within {self._connect_timeout:.0f}s "
                f"(last error: {last_error}; ssh exit="
                f"{self._forward.poll()}).\n"
                f"ssh transcript tail:\n{transcript}\n"
                f"remote agent.boot.log tail:\n{boot if rc == 0 else '(unavailable)'}"
            )
        remote_protocol = int(self.handshake_result.get("protocol", -1))
        if remote_protocol != AGENT_PROTOCOL_VERSION:
            raise self.failure(
                f"{self.name}: remote agent speaks protocol "
                f"{remote_protocol}, controller speaks "
                f"{AGENT_PROTOCOL_VERSION}; rebuild and re-stage "
                "qoh99_e2e_agent.exe on both sides"
            )
        local_agent_sha = _sha256_file(agent_source)
        if str(self.handshake_result.get("agent_sha256")) != local_agent_sha:
            raise self.failure(
                f"{self.name}: staged agent hash "
                f"{self.handshake_result.get('agent_sha256')} does not match "
                f"the controller's build {local_agent_sha}"
            )
        self.handshake_result["staged"] = {
            name: str(path) for name, path in staged.items()
        }

        self._heartbeat_thread = threading.Thread(
            target=self._heartbeat_loop,
            name=f"qoh99-agent-heartbeat-{self.name}",
            daemon=True,
        )
        self._heartbeat_thread.start()

    def _heartbeat_loop(self) -> None:
        while not self._heartbeat_stop.wait(HEARTBEAT_INTERVAL_SECONDS):
            client = self.client
            if client is None:
                return
            try:
                client.call("heartbeat", timeout=DEFAULT_RPC_TIMEOUT_SECONDS)
            except BaseException:
                return

    def heartbeat(self) -> None:
        self._require_client().call("heartbeat")

    def handshake(self) -> dict[str, object]:
        return dict(self.handshake_result)

    def shutdown(self) -> None:
        self._heartbeat_stop.set()
        if self._heartbeat_thread is not None:
            self._heartbeat_thread.join(timeout=3.0)
            self._heartbeat_thread = None
        if self.client is not None:
            try:
                self.client.call("shutdown", timeout=5.0)
            except BaseException:
                pass
            self.client.close()
            self.client = None
        if self._forward is not None:
            if self._forward.poll() is None:
                self._forward.terminate()
                try:
                    self._forward.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    self._forward.kill()
            self._forward = None

    def escalate_kill(self) -> bool:
        rc, _out, _err = self.ssh_run(
            "pkill -f qoh99caster; pkill -f -i qoh99.exe; "
            "pkill -f qoh99_e2e_agent; true",
            timeout=30.0,
        )
        return rc == 0

    def _require_client(self) -> AgentClient:
        if self.client is None:
            raise self.failure(
                f"{self.name}: the remote agent connection is not open"
            )
        return self.client

    def path(self, value: str | os.PathLike[str]) -> Path:
        return Path(PureWindowsPath(str(value)))

    def spawn(
        self,
        argv: Sequence[str],
        cwd: Path,
        env: dict[str, str] | None,
        stdout_handle: object,
        stderr_handle: object,
    ) -> RemoteProcHandle:
        reply = self._require_client().call(
            "spawn",
            argv=list(argv),
            cwd=str(cwd),
            env=dict(env or {}),
            stdout=str(stdout_handle),
            stderr=str(stderr_handle),
            timeout=60.0,
        )
        return RemoteProcHandle(
            self._require_client(), str(reply["token"]), int(reply["pid"])
        )

    def open_log_handle(self, path: Path) -> Path:
        return _RemoteLogHandle(path)

    def stop(
        self,
        process: RemoteProcHandle,
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

    def child_game_pid(self, parent_pid: int, image_name: str) -> int | None:
        reply = self._require_client().call(
            "child_game_pid", parent_pid=int(parent_pid), image=image_name
        )
        pid = int(reply.get("pid", 0))
        return pid or None

    def open_process(
        self, pid: int, request_set_information: bool = False
    ) -> RemoteMemory:
        reply = self._require_client().call(
            "open_process",
            pid=int(pid),
            set_information=bool(request_set_information),
        )
        return RemoteMemory(
            self._require_client(), str(reply["handle"]), int(pid)
        )

    def module_base(
        self, pid: int, module_name: str
    ) -> tuple[int, int, str] | None:
        reply = self._require_client().call(
            "module_base", pid=int(pid), name=module_name
        )
        if not reply.get("found"):
            return None
        return (
            int(reply["base"]),
            int(reply["size"]),
            str(reply["path"]),
        )

    def process_module_map(self, pid: int) -> list[dict[str, object]]:
        reply = self._require_client().call("module_map", pid=int(pid))
        return list(reply.get("modules") or [])

    def game_state(self, memory: RemoteMemory) -> dict[str, object]:
        raise self.failure(
            "SshRoleHost.game_state requires the harness's GAME_ADDR specs; "
            "call sample_game_state through the harness seam"
        )

    def open_thread_suspended(
        self, pid: int, tid: int, request_context: bool
    ) -> RemoteSuspendedThread:
        reply = self._require_client().call(
            "thread_suspend",
            pid=int(pid),
            tid=int(tid),
            want_context=bool(request_context),
        )
        return RemoteSuspendedThread(
            self._require_client(), str(reply["token"]), int(pid), int(tid)
        )

    def process_windows(self, pid: int) -> list[tuple[int, str]]:
        reply = self._require_client().call("windows", pid=int(pid))
        return [
            (int(item["hwnd"]), str(item["title"]))
            for item in reply.get("windows") or []
        ]

    def window_titles(self, pid: int) -> list[str]:
        return sorted({title for _hwnd, title in self.process_windows(pid)})

    def window_thread_process_id(self, hwnd: int) -> tuple[int, int]:
        reply = self._require_client().call("window_info", hwnd=int(hwnd))
        return int(reply["thread_id"]), int(reply["owner_pid"])

    def is_window(self, hwnd: int) -> bool:
        reply = self._require_client().call("window_info", hwnd=int(hwnd))
        return bool(reply["is_window"])

    def post_close_direct(self, hwnd: int) -> tuple[bool, int]:
        reply = self._require_client().call("post_close", hwnd=int(hwnd))
        return bool(reply["posted"]), int(reply["error"])

    def post_close_on_named_desktop(
        self, pid: int, desktop_name: str
    ) -> list[dict[str, object]]:
        raise self.failure(
            "hidden-desktop WM_CLOSE is not reproducible under Wine; "
            "distributed runs require --visible-games (see the distributed "
            "coverage table)"
        )

    def disk_usage(self, path: Path) -> tuple[int, int, int]:
        reply = self._require_client().call("fs_disk_usage", path=str(path))
        return int(reply["total"]), int(reply["used"]), int(reply["free"])

    def sha256(self, path: Path) -> str:
        return str(
            self._require_client().call(
                "fs_sha256", path=str(path), timeout=BULK_RPC_TIMEOUT_SECONDS
            )["sha256"]
        )

    def stat(self, path: Path) -> dict[str, object]:
        return dict(
            self._require_client().call("fs_stat", path=str(path))
        )

    def glob(self, root: Path, pattern: str) -> list[Path]:
        reply = self._require_client().call(
            "fs_glob", root=str(root), pattern=pattern, timeout=60.0
        )
        return [self.path(item) for item in reply.get("paths") or []]

    def read_bytes(self, path: Path) -> bytes:
        reply = self._require_client().call(
            "fs_read", path=str(path), timeout=120.0
        )
        return base64.b64decode(str(reply["data"]))

    def write_bytes(self, path: Path, data: bytes) -> None:
        self._require_client().call(
            "fs_write",
            path=str(path),
            data=base64.b64encode(data).decode("ascii"),
            timeout=120.0,
        )

    def write_text(self, path: Path, text: str, encoding: str) -> None:
        self.write_bytes(path, text.encode(encoding))

    def mkdir(self, path: Path, parents: bool = False, exist_ok: bool = False):
        self._require_client().call(
            "fs_mkdir",
            path=str(path),
            parents=bool(parents),
            exist_ok=bool(exist_ok),
        )

    def copy(self, source: Path, destination: Path) -> None:
        self._require_client().call(
            "fs_copy",
            source=str(source),
            destination=str(destination),
            timeout=300.0,
        )

    def clone_game_install(
        self, source: Path, destination: Path
    ) -> dict[str, int]:
        reply = self._require_client().call(
            "clone_install",
            source=str(source),
            destination=str(destination),
            timeout=BULK_RPC_TIMEOUT_SECONDS,
        )
        return {
            "copied": int(reply["copied"]),
            "skipped": int(reply["skipped"]),
        }

    def clone_payload_bytes(self, source: Path) -> tuple[int, int]:
        reply = self._require_client().call(
            "clone_payload",
            source=str(source),
            timeout=BULK_RPC_TIMEOUT_SECONDS,
        )
        return int(reply["bytes"]), int(reply["files"])

    def validate_asset_paths(self, install: Path) -> dict[str, object]:
        reply = self._require_client().call(
            "validate_asset_paths",
            root=str(install),
            timeout=BULK_RPC_TIMEOUT_SECONDS,
        )
        violations = list(reply.get("too_long") or [])
        if violations:
            raise self.failure(
                f"{len(violations)} copied runtime asset paths exceed "
                f"{reply.get('limit')} characters on {self.label} "
                f"({violations[:8]})"
            )
        if int(reply.get("checked", 0)) == 0:
            raise self.failure(
                f"copied game install contains no files: {install}"
            )
        return {
            "checked": int(reply["checked"]),
            "limit": int(reply["limit"]),
            "longest_length": int(reply["longest_length"]),
            "longest_path": str(reply["longest_path"]),
        }

    def mute_test_install_audio(self, install: Path) -> dict[str, object]:
        return dict(
            self._require_client().call(
                "mute_audio", install=str(install), timeout=60.0
            )
        )

    def config_snapshot(
        self, path: Path, offsets: tuple[int, ...]
    ) -> dict[str, object]:
        reply = self._require_client().call(
            "config_snapshot", path=str(path), offsets=list(offsets)
        )
        reply["values"] = {
            int(key): int(value)
            for key, value in dict(reply.get("values") or {}).items()
        }
        return dict(reply)

    def config_mutate(
        self, path: Path, profile: int, values: dict[int, int]
    ) -> dict[str, object]:
        reply = self._require_client().call(
            "config_mutate",
            path=str(path),
            profile=int(profile),
            values={str(k): int(v) for k, v in values.items()},
        )
        reply["values"] = {
            int(key): int(value)
            for key, value in dict(reply.get("values") or {}).items()
        }
        return dict(reply)

    def run(
        self, argv: Sequence[str], cwd: Path, timeout: float
    ) -> tuple[int, str]:
        reply = self._require_client().call(
            "run",
            argv=list(argv),
            cwd=str(cwd),
            run_timeout=float(timeout),
            timeout=timeout + 30.0,
        )
        return int(reply["returncode"]), str(reply["output"])

    def reserve_relay_port(self, bind: str) -> int:
        reply = self._require_client().call(
            "reserve_port", kind="relay", bind=bind
        )
        return int(reply["port"])

    def reserve_game_port(self, bind: str) -> int:
        reply = self._require_client().call(
            "reserve_port", kind="game", bind=bind
        )
        return int(reply["port"])

    def reserve_exclusive_udp(self, bind: str = "0.0.0.0") -> int:
        reply = self._require_client().call(
            "reserve_exclusive_udp", bind=bind
        )
        return int(reply["port"])

    def tail(self, path: Path, offset: int) -> tuple[int, bytes, int, bool]:
        reply = self._require_client().call(
            "fs_tail", path=str(path), offset=int(offset), timeout=60.0
        )
        return (
            int(reply["offset"]),
            base64.b64decode(str(reply.get("data") or "")),
            int(reply["size"]),
            bool(reply.get("exists", True)),
        )

class _RemoteLogHandle:

    def __init__(self, path: Path) -> None:
        self.path = path

    def __str__(self) -> str:
        return str(self.path)

    def close(self) -> None:
        return None

def _sha256_file(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

class LogMirror:

    def __init__(
        self,
        role_name: str,
        remote_root: Path,
        local_root: Path,
        host: SshRoleHost,
        failure: Callable[[str], BaseException],
    ) -> None:
        self.role_name = role_name
        self.remote_root = remote_root
        self.local_root = local_root
        self.host = host
        self.failure = failure
        self.local_root.mkdir(parents=True, exist_ok=True)
        self._offsets: dict[str, int] = {}
        self.refreshes = 0
        self.bytes_mirrored = 0
        self.last_refresh_mono: float | None = None
        self.grew = False
        self._tracked: list[str] = []
        self._sealed: list[str] = []

    def _relative(self, remote: Path) -> str:
        try:
            return str(PureWindowsPath(str(remote)).relative_to(
                PureWindowsPath(str(self.remote_root))
            ))
        except ValueError as exc:
            raise self.failure(
                f"{self.role_name}: {remote} is outside the mirrored role "
                f"root {self.remote_root}"
            ) from exc

    def local_path(self, remote: Path) -> Path:
        return self.local_root / self._relative(remote)

    def refresh(self) -> None:
        discovered = set(self._tracked)
        for pattern in (
            "caster.stdout.log",
            "caster.stderr.log",
            "logs/session_*/dll.log",
            "logs/session_*/client.log",
            "logs/session_*/*.dmp",
            "replays/*.q99r",
            "replays/*.q99r.part",
        ):
            for remote in self.host.glob(self.remote_root, pattern):
                discovered.add(self._relative(remote))
        self._tracked = sorted(discovered)

        grew = False
        for relative in self._tracked:
            remote = self.host.path(str(self.remote_root) + "\\" + relative)
            offset = self._offsets.get(relative, 0)
            new_offset, data, size, exists = self.host.tail(remote, offset)
            local = self.local_root / relative
            local.parent.mkdir(parents=True, exist_ok=True)
            if not exists:
                if relative not in self._sealed:
                    self._sealed.append(relative)
                continue
            if size < offset:
                self._offsets[relative] = 0
                new_offset, data, size, exists = self.host.tail(remote, 0)
                if exists:
                    local.write_bytes(data)
                    self._offsets[relative] = new_offset
                    grew = grew or bool(data)
                continue
            if data:
                with local.open("ab") as stream:
                    stream.write(data)
                grew = True
                self.bytes_mirrored += len(data)
            self._offsets[relative] = new_offset
        self.refreshes += 1
        self.last_refresh_mono = time.monotonic()
        self.grew = self.grew or grew

    def ledger(self) -> dict[str, object]:
        return {
            "bytes": self.bytes_mirrored,
            "refreshes": self.refreshes,
            "files": list(self._tracked),
            "sealed_after_remote_delete": list(self._sealed),
            "last_age_s": (
                None
                if self.last_refresh_mono is None
                else round(time.monotonic() - self.last_refresh_mono, 3)
            ),
            "grew": self.grew,
        }

class EvidenceGuard:

    def __init__(
        self,
        enabled: bool,
        staleness_budget_s: float,
        failure: Callable[[str], BaseException],
    ) -> None:
        self.enabled = enabled
        self.staleness_budget_s = staleness_budget_s
        self.failure = failure

    def check(
        self, role_name: str, mirror: LogMirror | None, caster_alive: bool
    ) -> None:
        if not self.enabled or mirror is None:
            return
        if mirror.refreshes == 0:
            raise self.failure(
                f"{role_name}: evidence mirror has never been refreshed; "
                "every 'marker in text' assertion would pass vacuously"
            )
        if not caster_alive or mirror.last_refresh_mono is None:
            return
        age = time.monotonic() - mirror.last_refresh_mono
        if age > self.staleness_budget_s:
            raise self.failure(
                f"{role_name}: evidence mirror is {age:.1f}s stale while its "
                f"caster is still running (budget "
                f"{self.staleness_budget_s:.1f}s); assertions on this text "
                "cannot be trusted"
            )
