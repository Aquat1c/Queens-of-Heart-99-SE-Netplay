
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
from typing import BinaryIO, Iterable

CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_PROCESS_GROUP = 0x00000200

class WindowsProcessControl:

    @staticmethod
    def creation_flags() -> int:
        return (
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
            if os.name == "nt"
            else 0
        )

    @staticmethod
    def kill_tree(process: subprocess.Popen[bytes]) -> None:
        if os.name == "nt":
            subprocess.run(
                [
                    "taskkill",
                    "/PID",
                    str(process.pid),
                    "/T",
                    "/F",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                creationflags=CREATE_NO_WINDOW,
            )
        else:
            process.kill()

@dataclass(frozen=True)
class StressCase:
    name: str
    route: str
    network_profile: str
    host_delay: int
    host_rollback: int
    client_delay: int
    client_rollback: int

INITIAL_CASES: tuple[StressCase, ...] = (
    StressCase(
        "stable_low_direct",
        "punched-direct",
        "stable-low",
        1,
        6,
        2,
        8,
    ),
    StressCase(
        "jitter_high_relay",
        "relay",
        "jitter-high",
        7,
        12,
        9,
        15,
    ),
    StressCase(
        "packet_loss_relay",
        "relay",
        "packet-loss",
        2,
        12,
        4,
        15,
    ),
)

FULL_CASES: tuple[StressCase, ...] = (
    INITIAL_CASES[0],
    StressCase(
        "stable_120_d2r8_relay",
        "relay",
        "stable-120",
        2,
        8,
        2,
        8,
    ),
    StressCase(
        "stable_high_relay",
        "relay",
        "stable-high",
        7,
        12,
        8,
        15,
    ),
    StressCase(
        "jitter_low_direct",
        "punched-direct",
        "jitter-low",
        1,
        8,
        2,
        10,
    ),
    INITIAL_CASES[1],
    INITIAL_CASES[2],
)

@dataclass
class PortLease:
    game_port: int
    relay_port: int
    sockets: list[socket.socket] = field(default_factory=list)

    def close(self) -> None:
        for handle in self.sockets:
            try:
                handle.close()
            except OSError:
                pass
        self.sockets.clear()

@dataclass
class CaseRun:
    ordinal: int
    spec: StressCase
    game_port: int
    relay_port: int
    network_seed: int
    root: Path
    command: list[str]
    lease: PortLease
    process: subprocess.Popen[bytes] | None = None
    stdout_handle: BinaryIO | None = None
    stderr_handle: BinaryIO | None = None
    started_mono: float | None = None
    started_at: str | None = None
    result: dict[str, object] | None = None

    @property
    def case_id(self) -> str:
        return f"s{self.ordinal:02d}_{self.spec.name}"

    def close_logs(self) -> None:
        for attribute in ("stdout_handle", "stderr_handle"):
            handle = getattr(self, attribute)
            if handle is not None:
                handle.close()
                setattr(self, attribute, None)

def now_iso() -> str:
    return datetime.now().astimezone().isoformat()

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()

def write_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    os.replace(temporary, path)

def parse_integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"expected an integer, got {value!r}"
        ) from exc

def add_exclusive_bind(socket_handle: socket.socket) -> None:
    exclusive = getattr(socket, "SO_EXCLUSIVEADDRUSE", None)
    if exclusive is not None:
        socket_handle.setsockopt(socket.SOL_SOCKET, exclusive, 1)

def bind_lease_socket(port: int, kind: int) -> socket.socket:
    handle = socket.socket(socket.AF_INET, kind)
    try:
        add_exclusive_bind(handle)
        handle.bind(("127.0.0.1", port))
        if kind == socket.SOCK_STREAM:
            handle.listen(1)
        return handle
    except BaseException:
        handle.close()
        raise

def reserve_case_ports(game_port: int, relay_port: int) -> PortLease:
    lease = PortLease(game_port=game_port, relay_port=relay_port)
    endpoints = (
        (game_port, socket.SOCK_STREAM, "game TCP"),
        (game_port, socket.SOCK_DGRAM, "game UDP"),
        (game_port + 1, socket.SOCK_DGRAM, "spectator UDP"),
        (relay_port, socket.SOCK_STREAM, "relay TCP"),
        (relay_port, socket.SOCK_DGRAM, "relay UDP"),
    )
    try:
        for port, kind, label in endpoints:
            try:
                lease.sockets.append(bind_lease_socket(port, kind))
            except OSError as exc:
                raise RuntimeError(
                    f"{label} port 127.0.0.1:{port} is unavailable: {exc}"
                ) from exc
        return lease
    except BaseException:
        lease.close()
        raise

def selected_cases(args: argparse.Namespace) -> tuple[StressCase, ...]:
    if args.case_set == "full":
        return FULL_CASES
    return INITIAL_CASES[: args.initial_case_count]

def validate_ports(
    cases: tuple[StressCase, ...], args: argparse.Namespace
) -> list[tuple[int, int, tuple[int, int]]]:
    if args.game_port_stride < 2:
        raise ValueError("--game-port-stride must be at least 2")

    assignments: list[tuple[int, int, tuple[int, int]]] = []
    game_blocks: list[set[int]] = []
    relay_ports: set[int] = set()
    for ordinal, _ in enumerate(cases):
        game_port = args.game_port_base + ordinal * args.game_port_stride
        block = (game_port, game_port + args.game_port_stride - 1)
        relay_port = args.relay_port_base + ordinal
        if not (1 <= game_port <= 65534):
            raise ValueError(
                f"case {ordinal} game port {game_port} cannot provide "
                "the adjacent spectator port"
            )
        if block[1] > 65535:
            raise ValueError(
                f"case {ordinal} game block {block[0]}..{block[1]} "
                "exceeds 65535"
            )
        if not (1 <= relay_port <= 65535):
            raise ValueError(
                f"case {ordinal} relay port {relay_port} is invalid"
            )
        numeric_block = set(range(block[0], block[1] + 1))
        if any(numeric_block & prior for prior in game_blocks):
            raise ValueError(f"overlapping game port block {block}")
        if relay_port in relay_ports:
            raise ValueError(f"duplicate relay port {relay_port}")
        game_blocks.append(numeric_block)
        relay_ports.add(relay_port)
        assignments.append((game_port, relay_port, block))

    all_game_ports = set().union(*game_blocks) if game_blocks else set()
    overlap = all_game_ports & relay_ports
    if overlap:
        raise ValueError(
            "relay ports overlap reserved game blocks: "
            + ", ".join(str(port) for port in sorted(overlap))
        )
    return assignments

def derived_seed(master_seed: int, ordinal: int) -> int:
    seed = (
        (master_seed & 0xFFFFFFFF)
        + ordinal * 0x9E3779B9
    ) & 0xFFFFFFFF
    return seed or (ordinal + 1)

def worker_command(
    args: argparse.Namespace,
    spec: StressCase,
    case_root: Path,
    game_port: int,
    relay_port: int,
    network_seed: int,
) -> list[str]:
    command = [
        str(Path(args.python).resolve()),
        str(Path(args.live_harness).resolve()),
        "--route",
        spec.route,
        "--config-variant",
        "identical",
        "--caster",
        str(Path(args.caster).resolve()),
        "--relay",
        str(Path(args.relay).resolve()),
        "--game",
        str(Path(args.game).resolve()),
        "--dll",
        str(Path(args.dll).resolve()),
        "--wrapper",
        str(Path(args.wrapper).resolve()),
        "--dll-map",
        str(Path(args.dll_map).resolve()),
        "--output-root",
        str(case_root.resolve()),
        "--gameplay-ticks",
        str(args.gameplay_ticks),
        "--spectator",
        "--full-match-rematch",
        "--force-ko-after-frames",
        str(args.force_ko_after_frames),
        "--rematch-gameplay-ticks",
        str(args.rematch_gameplay_ticks),
        "--network-profile",
        spec.network_profile,
        "--network-seed",
        str(network_seed),
        "--game-port",
        str(game_port),
        "--relay-port",
        str(relay_port),
        "--host-delay",
        str(spec.host_delay),
        "--host-rollback",
        str(spec.host_rollback),
        "--client-delay",
        str(spec.client_delay),
        "--client-rollback",
        str(spec.client_rollback),
    ]
    if spec.network_profile in ("stable-low", "stable-120", "stable-high"):
        command.extend(["--minimum-canonical-fps", "59"])
    return command

def discover_worker_report(case_root: Path) -> tuple[Path | None, object | None]:
    reports = sorted(
        case_root.glob("*/report.json"),
        key=lambda path: path.stat().st_mtime_ns,
    )
    if len(reports) != 1:
        return None, {
            "error": (
                f"expected exactly one worker report below {case_root}, "
                f"found {len(reports)}"
            ),
            "candidates": [str(path) for path in reports],
        }
    report_path = reports[0]
    try:
        return report_path, json.loads(
            report_path.read_text(encoding="utf-8")
        )
    except (OSError, json.JSONDecodeError) as exc:
        return report_path, {
            "error": f"could not read worker report: {exc}"
        }

def start_case(case: CaseRun, project_root: Path) -> None:
    case.root.mkdir(parents=True)
    stdout_path = case.root / "harness.stdout.log"
    stderr_path = case.root / "harness.stderr.log"
    write_json(
        case.root / "command.json",
        {
            "case_id": case.case_id,
            "command": case.command,
            "game_port": case.game_port,
            "spectator_port": case.game_port + 1,
            "relay_port": case.relay_port,
            "network_seed": case.network_seed,
        },
    )

    case.stdout_handle = stdout_path.open("wb")
    case.stderr_handle = stderr_path.open("wb")
    case.lease.close()
    environment = os.environ.copy()
    environment["PYTHONUNBUFFERED"] = "1"
    creationflags = WindowsProcessControl.creation_flags()
    try:
        case.process = subprocess.Popen(
            case.command,
            cwd=project_root,
            stdout=case.stdout_handle,
            stderr=case.stderr_handle,
            env=environment,
            creationflags=creationflags,
        )
    except BaseException:
        case.close_logs()
        raise
    case.started_mono = time.monotonic()
    case.started_at = now_iso()

def terminate_owned_tree(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    WindowsProcessControl.kill_tree(process)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)

def finalize_case(
    case: CaseRun,
    exit_code: int,
    timed_out: bool = False,
) -> dict[str, object]:
    case.close_logs()
    report_path, worker_report = discover_worker_report(case.root)
    elapsed = (
        round(time.monotonic() - case.started_mono, 3)
        if case.started_mono is not None
        else 0.0
    )
    worker_outcome = (
        worker_report.get("outcome")
        if isinstance(worker_report, dict)
        else None
    )
    passed = (
        not timed_out
        and exit_code == 0
        and worker_outcome == "passed"
    )
    result: dict[str, object] = {
        "case_id": case.case_id,
        "name": case.spec.name,
        "route": case.spec.route,
        "network_profile": case.spec.network_profile,
        "network_seed": case.network_seed,
        "peer_settings": {
            "host": {
                "delay": case.spec.host_delay,
                "rollback": case.spec.host_rollback,
            },
            "client": {
                "delay": case.spec.client_delay,
                "rollback": case.spec.client_rollback,
            },
        },
        "ports": {
            "game": case.game_port,
            "spectator": case.game_port + 1,
            "relay": case.relay_port,
        },
        "started": case.started_at,
        "finished": now_iso(),
        "elapsed_seconds": elapsed,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "outcome": "passed" if passed else "failed",
        "case_root": str(case.root),
        "stdout": str(case.root / "harness.stdout.log"),
        "stderr": str(case.root / "harness.stderr.log"),
        "worker_report": str(report_path) if report_path else None,
        "worker_outcome": worker_outcome,
    }
    if not passed:
        if timed_out:
            result["error"] = "worker exceeded the per-case hard timeout"
        elif exit_code != 0:
            result["error"] = f"worker exited with code {exit_code}"
        elif isinstance(worker_report, dict):
            result["error"] = worker_report.get(
                "error", "worker report did not declare outcome=passed"
            )
        else:
            result["error"] = "worker report is missing or invalid"
        result["worker_report_detail"] = worker_report
    case.result = result
    write_json(case.root / "supervisor_result.json", result)
    return result

def manifest_for(
    args: argparse.Namespace,
    run_root: Path,
    cases: list[CaseRun],
) -> dict[str, object]:
    return {
        "schema": 1,
        "started": now_iso(),
        "case_set": args.case_set,
        "concurrency": args.concurrency,
        "timing_class": (
            "representative" if args.concurrency == 1 else "overload"
        ),
        "presentation_cadence_is_representative": args.concurrency == 1,
        "case_timeout_seconds": args.case_timeout_seconds,
        "run_root": str(run_root),
        "live_harness": str(Path(args.live_harness).resolve()),
        "wrapper": {
            "path": str(Path(args.wrapper).resolve()),
            "bytes": Path(args.wrapper).resolve().stat().st_size,
            "sha256": sha256(Path(args.wrapper).resolve()),
        },
        "cases": [
            {
                "case_id": case.case_id,
                "name": case.spec.name,
                "route": case.spec.route,
                "network_profile": case.spec.network_profile,
                "network_seed": case.network_seed,
                "game_port_block": [
                    case.game_port,
                    case.game_port + args.game_port_stride - 1,
                ],
                "ports": {
                    "game_tcp_udp": case.game_port,
                    "spectator_udp": case.game_port + 1,
                    "relay_tcp_udp": case.relay_port,
                },
                "peer_settings": {
                    "host": [
                        case.spec.host_delay,
                        case.spec.host_rollback,
                    ],
                    "client": [
                        case.spec.client_delay,
                        case.spec.client_rollback,
                    ],
                },
                "root": str(case.root),
                "command": case.command,
            }
            for case in cases
        ],
    }

def supervise(
    args: argparse.Namespace,
    run_root: Path,
    cases: list[CaseRun],
) -> int:
    project_root = Path(__file__).resolve().parents[3]
    pending = list(cases)
    active: list[CaseRun] = []
    completed: list[dict[str, object]] = []
    launch_stopped = False
    interrupted = False

    try:
        while pending or active:
            while (
                pending
                and not launch_stopped
                and len(active) < args.concurrency
            ):
                case = pending.pop(0)
                print(
                    f"[parallel] START {case.case_id} "
                    f"route={case.spec.route} "
                    f"profile={case.spec.network_profile} "
                    f"ports=game:{case.game_port}/"
                    f"spectator:{case.game_port + 1}/"
                    f"relay:{case.relay_port}",
                    flush=True,
                )
                try:
                    start_case(case, project_root)
                except BaseException as exc:
                    case.result = {
                        "case_id": case.case_id,
                        "name": case.spec.name,
                        "outcome": "failed",
                        "error": f"worker launch failed: {exc}",
                        "case_root": str(case.root),
                    }
                    write_json(
                        case.root / "supervisor_result.json",
                        case.result,
                    )
                    completed.append(case.result)
                    launch_stopped = True
                    print(
                        f"[parallel] FAIL {case.case_id}: {exc}",
                        file=sys.stderr,
                        flush=True,
                    )
                    break
                active.append(case)

            if not active:
                break

            now = time.monotonic()
            for case in list(active):
                assert case.process is not None
                exit_code = case.process.poll()
                timed_out = (
                    exit_code is None
                    and case.started_mono is not None
                    and now - case.started_mono
                        > args.case_timeout_seconds
                )
                if timed_out:
                    terminate_owned_tree(case.process)
                    exit_code = (
                        case.process.returncode
                        if case.process.returncode is not None
                        else 124
                    )
                if exit_code is None:
                    continue

                result = finalize_case(
                    case, exit_code, timed_out=timed_out
                )
                completed.append(result)
                active.remove(case)
                print(
                    f"[parallel] {result['outcome'].upper()} "
                    f"{case.case_id} elapsed="
                    f"{result['elapsed_seconds']}s artifacts={case.root}",
                    file=(
                        sys.stdout
                        if result["outcome"] == "passed"
                        else sys.stderr
                    ),
                    flush=True,
                )
                if result["outcome"] != "passed":
                    launch_stopped = True

            if active:
                time.sleep(0.1)
    except KeyboardInterrupt:
        interrupted = True
        launch_stopped = True
        print(
            "[parallel] interrupted; terminating exact test-owned trees",
            file=sys.stderr,
            flush=True,
        )
        for case in active:
            assert case.process is not None
            terminate_owned_tree(case.process)
            completed.append(
                finalize_case(
                    case,
                    case.process.returncode
                    if case.process.returncode is not None
                    else 130,
                    timed_out=False,
                )
            )
        active.clear()
    finally:
        for case in cases:
            case.lease.close()
            case.close_logs()

    completed_ids = {
        str(result.get("case_id")) for result in completed
    }
    skipped = [
        {
            "case_id": case.case_id,
            "name": case.spec.name,
            "outcome": "skipped",
            "reason": (
                "not launched after an earlier failure"
                if launch_stopped
                else "not launched"
            ),
            "case_root": str(case.root),
        }
        for case in cases
        if case.case_id not in completed_ids
    ]
    passed = (
        not interrupted
        and not skipped
        and len(completed) == len(cases)
        and all(result.get("outcome") == "passed" for result in completed)
    )
    summary = {
        "schema": 1,
        "finished": now_iso(),
        "outcome": "passed" if passed else "failed",
        "concurrency": args.concurrency,
        "timing_class": (
            "representative" if args.concurrency == 1 else "overload"
        ),
        "interrupted": interrupted,
        "fail_fast_triggered": launch_stopped,
        "run_root": str(run_root),
        "completed": completed,
        "skipped": skipped,
    }
    write_json(run_root / "summary.json", summary)
    print(
        f"[parallel] OVERALL {summary['outcome'].upper()} "
        f"completed={len(completed)}/{len(cases)} "
        f"skipped={len(skipped)} artifacts={run_root}",
        file=sys.stdout if passed else sys.stderr,
        flush=True,
    )
    return 0 if passed else 1

def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    script = Path(__file__).resolve()
    parser = argparse.ArgumentParser(
        description=(
            "Run independent production caster/game/spectator sessions "
            "concurrently with deterministic network profiles."
        )
    )
    parser.add_argument("--caster", required=True)
    parser.add_argument(
        "--relay",
        required=True,
        help=(
            "executable each worker launches as `<exe> relay --bind ... "
            "--port ...`; the relay is a caster verb, so this is normally "
            "the same qoh99caster.exe passed to --caster"
        ),
    )
    parser.add_argument("--game", required=True)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--wrapper", required=True)
    parser.add_argument("--dll-map", required=True)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument(
        "--live-harness",
        default=str(script.with_name("live_game_e2e.py")),
    )
    parser.add_argument(
        "--case-set",
        choices=("initial", "full"),
        default="initial",
        help=(
            "initial runs 2-3 representative cases; full runs all five "
            "stable/jitter/loss profiles"
        ),
    )
    parser.add_argument(
        "--initial-case-count",
        type=int,
        choices=(2, 3),
        default=3,
        help="number of cases when --case-set initial is selected",
    )
    parser.add_argument(
        "--concurrency",
        type=int,
        default=1,
        help=(
            "number of simultaneously rendered sessions; the default is one "
            "so presentation cadence remains representative. Values above "
            "one are overload/isolation stress, not visual-speed evidence"
        ),
    )
    parser.add_argument("--case-timeout-seconds", type=int, default=600)
    parser.add_argument("--game-port-base", type=int, default=46000)
    parser.add_argument("--game-port-stride", type=int, default=4)
    parser.add_argument("--relay-port-base", type=int, default=47000)
    parser.add_argument("--network-seed", type=parse_integer, default=0x514F4839)
    parser.add_argument("--gameplay-ticks", type=int, default=300)
    parser.add_argument("--rematch-gameplay-ticks", type=int, default=45)
    parser.add_argument("--force-ko-after-frames", type=int, default=360)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help=(
            "reserve/release the complete port plan and write manifests "
            "without starting workers"
        ),
    )
    args = parser.parse_args(list(argv))

    for attribute in (
        "caster",
        "relay",
        "game",
        "dll",
        "wrapper",
        "dll_map",
        "python",
        "live_harness",
    ):
        path = Path(getattr(args, attribute))
        if not path.is_file():
            parser.error(
                f"--{attribute.replace('_', '-')} does not exist: {path}"
            )
    if not 1 <= args.concurrency <= 8:
        parser.error("--concurrency must be between 1 and 8")
    if args.case_timeout_seconds < 60:
        parser.error("--case-timeout-seconds must be at least 60")
    if args.gameplay_ticks < 300:
        parser.error("--gameplay-ticks must be at least 300")
    if not 30 <= args.rematch_gameplay_ticks <= 90:
        parser.error("--rematch-gameplay-ticks must be in [30, 90]")
    if not 120 <= args.force_ko_after_frames <= 3600:
        parser.error("--force-ko-after-frames must be between 120 and 3600")
    return args

def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if os.name != "nt":
        print(
            "live_game_parallel_stress.py requires Windows",
            file=sys.stderr,
        )
        return 2

    specs = selected_cases(args)
    try:
        assignments = validate_ports(specs, args)
    except ValueError as exc:
        print(f"invalid port plan: {exc}", file=sys.stderr)
        return 2

    output_root = Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    nonce = time.time_ns() & 0xFFFFFF
    run_root: Path | None = None
    for attempt in range(256):
        candidate = output_root / f"p{(nonce + attempt) & 0xFFFFFF:06x}"
        try:
            candidate.mkdir()
        except FileExistsError:
            continue
        run_root = candidate
        break
    if run_root is None:
        print(
            "could not allocate a unique compact artifact directory",
            file=sys.stderr,
        )
        return 2

    cases: list[CaseRun] = []
    try:
        for ordinal, (spec, assignment) in enumerate(
            zip(specs, assignments)
        ):
            game_port, relay_port, _ = assignment
            lease = reserve_case_ports(game_port, relay_port)
            case_root = run_root / f"s{ordinal}"
            seed = derived_seed(args.network_seed, ordinal)
            cases.append(
                CaseRun(
                    ordinal=ordinal,
                    spec=spec,
                    game_port=game_port,
                    relay_port=relay_port,
                    network_seed=seed,
                    root=case_root,
                    command=worker_command(
                        args,
                        spec,
                        case_root,
                        game_port,
                        relay_port,
                        seed,
                    ),
                    lease=lease,
                )
            )
    except BaseException as exc:
        for case in cases:
            case.lease.close()
        write_json(
            run_root / "summary.json",
            {
                "schema": 1,
                "finished": now_iso(),
                "outcome": "failed",
                "error": f"port preflight failed: {exc}",
                "run_root": str(run_root),
            },
        )
        print(
            f"parallel stress port preflight failed: {exc}\n"
            f"artifacts: {run_root}",
            file=sys.stderr,
        )
        return 2

    manifest = manifest_for(args, run_root, cases)
    write_json(run_root / "manifest.json", manifest)
    if args.concurrency > 1:
        print(
            "[parallel] NOTE: concurrency > 1 is renderer/CPU overload "
            "stress; use --concurrency 1 for representative visible pacing",
            flush=True,
        )
    if args.dry_run:
        for case in cases:
            case.lease.close()
        write_json(
            run_root / "summary.json",
            {
                "schema": 1,
                "finished": now_iso(),
                "outcome": "dry-run",
                "concurrency": args.concurrency,
                "timing_class": (
                    "representative"
                    if args.concurrency == 1
                    else "overload"
                ),
                "run_root": str(run_root),
                "cases": [case.case_id for case in cases],
            },
        )
        print(
            f"[parallel] DRY-RUN cases={len(cases)} artifacts={run_root}",
            flush=True,
        )
        return 0
    return supervise(args, run_root, cases)

if __name__ == "__main__":
    raise SystemExit(main())
