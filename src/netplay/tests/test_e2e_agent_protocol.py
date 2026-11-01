
from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

AGENT = Path(os.environ.get("QOH99_E2E_AGENT", ""))
CASTER = Path(os.environ.get("QOH99_CASTER", ""))
PROTOCOL_VERSION = 1
LEASE_MS = 6000

def free_port() -> int:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = int(probe.getsockname()[1])
    probe.close()
    return port

class AgentSession:

    def __init__(self, port: int) -> None:
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.stream = self.socket.makefile("rb")
        self.next_id = 1

    def call(self, verb: str, **payload: object) -> dict:
        request = {"id": self.next_id, "verb": verb}
        request.update(payload)
        self.next_id += 1
        self.socket.sendall((json.dumps(request) + "\n").encode("utf-8"))
        reply = json.loads(self.stream.readline())
        assert reply["id"] == request["id"], reply
        if reply.get("error"):
            raise RuntimeError(f"{verb}: {reply['error']}")
        return reply.get("result") or {}

    def close(self) -> None:
        self.stream.close()
        self.socket.close()

@unittest.skipUnless(
    AGENT.is_file() and CASTER.is_file(),
    "requires QOH99_E2E_AGENT and QOH99_CASTER to name built artifacts",
)
class AgentProtocolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp(prefix="qoh99-agent-"))
        self.lock = self.temp / "agent.lock"
        self.port = free_port()
        self.agent = subprocess.Popen(
            [
                str(AGENT),
                "--port", str(self.port),
                "--lease-ms", str(LEASE_MS),
                "--lock", str(self.lock),
            ],
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.monotonic() + 15.0
        self.session: AgentSession | None = None
        while time.monotonic() < deadline:
            try:
                self.session = AgentSession(self.port)
                break
            except OSError:
                time.sleep(0.1)
        self.assertIsNotNone(self.session, "agent never accepted a connection")
        self.children: list[subprocess.Popen] = []

    def tearDown(self) -> None:
        for child in self.children:
            if child.poll() is None:
                child.kill()
        if self.session is not None:
            try:
                self.session.call("shutdown")
            except (RuntimeError, OSError):
                pass
            self.session.close()
        if self.agent.poll() is None:
            self.agent.kill()
        self.agent.wait(timeout=10)

    def start_target(self) -> subprocess.Popen:
        child = subprocess.Popen(
            [str(CASTER), "relay", "--bind", "127.0.0.1",
             "--port", str(free_port())],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.children.append(child)
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            try:
                self.session.call("module_map", pid=child.pid)
                return child
            except RuntimeError:
                time.sleep(0.1)
        self.fail("the 32-bit target never became enumerable")

    def test_handshake_reports_a_verifiable_identity(self) -> None:
        result = self.session.call(
            "handshake", protocol=PROTOCOL_VERSION, lease_ms=LEASE_MS
        )
        self.assertEqual(result["protocol"], PROTOCOL_VERSION)
        self.assertEqual(
            result["agent_sha256"],
            hashlib.sha256(AGENT.read_bytes()).hexdigest(),
        )
        self.assertTrue(result["agent_build"])

    def test_protocol_mismatch_is_refused(self) -> None:
        with self.assertRaises(RuntimeError) as caught:
            self.session.call("handshake", protocol=PROTOCOL_VERSION + 999)
        self.assertIn("protocol mismatch", str(caught.exception))

    def test_second_agent_is_refused(self) -> None:
        second = subprocess.run(
            [str(AGENT), "--port", str(free_port()), "--lock", str(self.lock)],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(second.returncode, 2, second.stderr)
        self.assertIn("refusing to start", second.stderr)

    def test_expired_lease_kills_the_agent_and_releases_the_lock(self) -> None:
        self.session.call("handshake", protocol=PROTOCOL_VERSION,
                          lease_ms=LEASE_MS)
        deadline = time.monotonic() + LEASE_MS / 1000.0 + 20.0
        while time.monotonic() < deadline and self.agent.poll() is None:
            time.sleep(0.25)
        self.assertIsNotNone(self.agent.poll(), "agent survived an expired lease")
        self.assertEqual(self.agent.returncode, 4)
        self.assertFalse(
            self.lock.exists(),
            "a stale lock file makes `naslab_agent.sh doctor` report a run "
            "in flight when there is none",
        )
        self.session = None

    def test_unknown_verb_is_a_structured_error(self) -> None:
        with self.assertRaises(RuntimeError) as caught:
            self.session.call("no_such_verb")
        self.assertIn("unknown verb", str(caught.exception))

    def test_filesystem_verbs_agree_with_local_truth(self) -> None:
        root = self.temp / "fs"
        self.session.call("fs_mkdir", path=str(root), parents=True,
                          exist_ok=False)
        payload = b"hello "
        self.session.call(
            "fs_write",
            path=str(root / "a.log"),
            data=base64.b64encode(payload).decode("ascii"),
        )
        stat = self.session.call("fs_stat", path=str(root / "a.log"))
        self.assertTrue(stat["exists"])
        self.assertEqual(stat["size"], len(payload))
        self.assertFalse(stat["is_dir"])
        self.assertEqual(
            self.session.call("fs_sha256", path=str(root / "a.log"))["sha256"],
            hashlib.sha256(payload).hexdigest(),
        )
        globbed = self.session.call("fs_glob", root=str(root),
                                    pattern="*.log")["paths"]
        self.assertEqual(len(globbed), 1)
        self.assertTrue(globbed[0].endswith("a.log"))
        usage = self.session.call("fs_disk_usage", path=str(root))
        self.assertGreater(usage["total"], 0)
        self.assertGreater(usage["free"], 0)

    def test_glob_matches_a_wildcard_in_a_middle_component(self) -> None:
        root = self.temp / "mirror"
        session = root / "logs" / "session_20260816_010203_004_p99"
        self.session.call("fs_mkdir", path=str(session), parents=True,
                          exist_ok=False)
        for name in ("dll.log", "client.log", "crash.dmp"):
            self.session.call(
                "fs_write",
                path=str(session / name),
                data=base64.b64encode(name.encode()).decode("ascii"),
            )
        other = root / "logs" / "archive"
        self.session.call("fs_mkdir", path=str(other), parents=True,
                          exist_ok=False)
        self.session.call(
            "fs_write",
            path=str(other / "dll.log"),
            data=base64.b64encode(b"decoy").decode("ascii"),
        )
        for pattern, expected in (
            ("logs/session_*/dll.log", ["dll.log"]),
            ("logs/session_*/client.log", ["client.log"]),
            ("logs/session_*/*.dmp", ["crash.dmp"]),
            ("logs/*/dll.log", ["dll.log", "dll.log"]),
            ("logs/session_*/*.q99r", []),
        ):
            paths = self.session.call(
                "fs_glob", root=str(root), pattern=pattern
            )["paths"]
            self.assertEqual(
                len(paths), len(expected),
                f"{pattern!r} matched {paths}",
            )
            for path, leaf in zip(paths, expected):
                self.assertTrue(
                    path.endswith(leaf), f"{pattern!r} -> {path}"
                )
            for path in paths:
                self.assertNotIn("*", path)

    def test_tail_is_incremental_and_exposes_truncation(self) -> None:
        log = self.temp / "role.log"
        log.write_bytes(b"hello ")
        first = self.session.call("fs_tail", path=str(log), offset=0)
        self.assertEqual(base64.b64decode(first["data"]), b"hello ")
        self.assertEqual(first["offset"], 6)

        with log.open("ab") as stream:
            stream.write(b"world")
        second = self.session.call("fs_tail", path=str(log),
                                   offset=first["offset"])
        self.assertEqual(base64.b64decode(second["data"]), b"world")
        self.assertEqual(second["size"], 11)

        log.write_bytes(b"X")
        third = self.session.call("fs_tail", path=str(log),
                                  offset=second["offset"])
        self.assertLess(third["size"], second["offset"])
        self.assertTrue(third["exists"])

        missing = self.session.call("fs_tail", path=str(self.temp / "nope"),
                                    offset=0)
        self.assertEqual(missing["size"], 0)
        self.assertEqual(missing["data"], "")
        self.assertFalse(missing["exists"])

        empty = self.temp / "empty.log"
        empty.write_bytes(b"")
        emptied = self.session.call("fs_tail", path=str(empty), offset=0)
        self.assertEqual(emptied["size"], 0)
        self.assertTrue(emptied["exists"])

        deleted = self.temp / "vanishes.log"
        deleted.write_bytes(b"body")
        seen = self.session.call("fs_tail", path=str(deleted), offset=0)
        self.assertEqual(base64.b64decode(seen["data"]), b"body")
        self.assertTrue(seen["exists"])
        deleted.unlink()
        gone = self.session.call("fs_tail", path=str(deleted),
                                 offset=seen["offset"])
        self.assertFalse(gone["exists"])
        self.assertEqual(gone["size"], 0)

    def test_process_memory_and_module_verbs(self) -> None:
        target = self.start_target()
        opened = self.session.call("open_process", pid=target.pid,
                                   set_information=False)
        handle = opened["handle"]
        self.assertTrue(self.session.call("proc_alive", handle=handle)["alive"])

        modules = self.session.call("module_map", pid=target.pid)["modules"]
        self.assertGreater(len(modules), 3)
        bases = [int(module["base"]) for module in modules]
        self.assertEqual(bases, sorted(bases), "module map must be base-sorted")

        located = self.session.call("module_base", pid=target.pid,
                                    name="qoh99caster.exe")
        self.assertTrue(located["found"])
        self.assertGreater(located["base"], 0)

        header = base64.b64decode(
            self.session.call("mem_read", handle=handle,
                              address=located["base"], size=2)["data"]
        )
        self.assertEqual(header, b"MZ")

        self.session.call("proc_suspend", handle=handle)
        self.session.call("proc_resume", handle=handle)

        self.session.call("proc_terminate", handle=handle)
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if not self.session.call("proc_alive", handle=handle)["alive"]:
                break
            time.sleep(0.05)
        self.assertFalse(self.session.call("proc_alive", handle=handle)["alive"])
        self.session.call("close_process", handle=handle)
        with self.assertRaises(RuntimeError):
            self.session.call("mem_read", handle=handle, address=0x1000, size=4)

    def test_mem_read_many_batches_scalars_in_one_round_trip(self) -> None:
        target = self.start_target()
        handle = self.session.call("open_process", pid=target.pid)["handle"]
        base = self.session.call(
            "module_base", pid=target.pid, name="qoh99caster.exe"
        )["base"]
        addresses = [base, base + 4, base + 8]
        specs = [
            {"name": f"v{index}", "address": address, "type": "u32"}
            for index, address in enumerate(addresses)
        ]
        values = self.session.call(
            "mem_read_many", handle=handle, specs=specs
        )["values"]
        self.assertEqual(len(values), len(specs))
        self.assertEqual(values["v0"] & 0xFFFF, 0x5A4D)
        for index, address in enumerate(addresses):
            single = self.session.call(
                "mem_read", handle=handle, address=address, size=4
            )
            expected = int.from_bytes(
                base64.b64decode(single["data"]), "little"
            )
            self.assertEqual(values[f"v{index}"], expected)
        with self.assertRaises(Exception) as caught:
            self.session.call(
                "mem_read_many",
                handle=handle,
                specs=specs + [{"name": "bad", "address": 1, "type": "u32"}],
            )
        self.assertIn("ReadProcessMemory", str(caught.exception))

    def test_sample_game_state_batches_one_round_trip(self) -> None:
        target = self.start_target()
        opened = self.session.call("open_process", pid=target.pid)
        located = self.session.call("module_base", pid=target.pid,
                                    name="qoh99caster.exe")
        sample = self.session.call(
            "sample_game_state",
            handle=opened["handle"],
            specs=[
                {"name": "unsigned", "address": located["base"], "type": "u32"},
                {"name": "signed", "address": located["base"], "type": "i32"},
            ],
        )
        self.assertTrue(sample["alive"])
        self.assertIn("titles", sample)
        self.assertIn("values", sample)
        self.assertEqual(sample["values"]["unsigned"] & 0xFFFF, 0x5A4D)
        self.assertEqual(sample["values"]["unsigned"],
                         sample["values"]["signed"] & 0xFFFFFFFF)

        broken = self.session.call(
            "sample_game_state",
            handle=opened["handle"],
            specs=[{"name": "bad", "address": 1, "type": "u32"}],
        )
        self.assertIn("read_error", broken)
        self.assertNotIn("values", broken)

    def test_spawn_redirects_stdio_and_reports_exit(self) -> None:
        out = self.temp / "child.out"
        spawned = self.session.call(
            "spawn",
            argv=[sys.executable, "-c", "print('agent-spawn-ok')"],
            cwd=str(self.temp),
            env={"PATH": os.environ.get("PATH", "")},
            stdout=str(out),
            stderr=str(self.temp / "child.err"),
        )
        self.assertGreater(spawned["pid"], 0)
        deadline = time.monotonic() + 30.0
        poll = {"running": True}
        while time.monotonic() < deadline:
            poll = self.session.call("proc_poll", token=spawned["token"])
            if not poll["running"]:
                break
            time.sleep(0.05)
        self.assertFalse(poll["running"])
        self.assertEqual(poll["returncode"], 0)
        self.assertEqual(out.read_text().strip(), "agent-spawn-ok")

    def test_run_returns_combined_output_verbatim(self) -> None:
        result = self.session.call(
            "run",
            argv=[sys.executable, "-c",
                  "import sys; print('out'); print('err', file=sys.stderr)"],
            cwd=str(self.temp),
            run_timeout=20,
        )
        self.assertEqual(result["returncode"], 0)
        self.assertIn("out", result["output"])
        self.assertIn("err", result["output"])

    def test_port_reservation_semantics(self) -> None:
        game = self.session.call("reserve_port", kind="game",
                                 bind="127.0.0.1")["port"]
        self.assertTrue(46000 <= game < 46998)
        relay = self.session.call("reserve_port", kind="relay",
                                  bind="127.0.0.1")["port"]
        self.assertGreater(relay, 1024)

        blocked = self.session.call("reserve_exclusive_udp",
                                    bind="0.0.0.0")["port"]
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        with self.assertRaises(OSError):
            probe.bind(("0.0.0.0", blocked))
        probe.close()

@unittest.skipUnless(AGENT.is_file(), "requires QOH99_E2E_AGENT")
class AgentStdioTransportTests(unittest.TestCase):

    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp(prefix="qoh99-agent-stdio-"))
        self.agent = subprocess.Popen(
            [
                str(AGENT), "--stdio", "--lease-ms", "60000",
                "--lock", str(self.temp / "agent.lock"),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        self.next_id = 1

    def tearDown(self) -> None:
        if self.agent.poll() is None:
            self.agent.kill()
        self.agent.wait(timeout=10)

    def call(self, verb: str, **payload: object) -> dict:
        request = {"id": self.next_id, "verb": verb}
        request.update(payload)
        self.next_id += 1
        self.agent.stdin.write((json.dumps(request) + "\n").encode("utf-8"))
        self.agent.stdin.flush()
        reply = json.loads(self.agent.stdout.readline())
        self.assertEqual(reply["id"], request["id"])
        if reply.get("error"):
            raise RuntimeError(f"{verb}: {reply['error']}")
        return reply.get("result") or {}

    def test_stdio_answers_a_single_short_request(self) -> None:
        result = self.call("handshake", protocol=PROTOCOL_VERSION)
        self.assertEqual(result["protocol"], PROTOCOL_VERSION)
        self.assertEqual(result["pointer_bits"], 32)

    def test_stdio_framing_survives_a_large_binary_payload(self) -> None:
        blob = b"\r\n" * 40000
        path = self.temp / "blob.bin"
        self.call("fs_write", path=str(path),
                  data=base64.b64encode(blob).decode("ascii"))
        self.assertEqual(path.read_bytes(), blob)
        tail = self.call("fs_tail", path=str(path), offset=0)
        self.assertEqual(base64.b64decode(tail["data"]), blob)
        self.assertEqual(
            self.call("handshake", protocol=PROTOCOL_VERSION)["protocol"],
            PROTOCOL_VERSION,
        )

    def test_stdio_socket_verbs_work(self) -> None:
        self.call("handshake", protocol=PROTOCOL_VERSION)
        port = self.call("reserve_port", kind="game", bind="127.0.0.1")["port"]
        self.assertTrue(46000 <= port < 46998)

if __name__ == "__main__":
    unittest.main()
