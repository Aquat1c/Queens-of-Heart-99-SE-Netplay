
from __future__ import annotations

import os
import socket
import subprocess
import sys
import time

def reserve_relay_port() -> int:
    for _ in range(100):
        tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp.bind(("127.0.0.1", 0))
        port = tcp.getsockname()[1]
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            udp.bind(("127.0.0.1", port))
        except OSError:
            tcp.close()
            udp.close()
            continue
        tcp.close()
        udp.close()
        return port
    raise RuntimeError("could not reserve a TCP+UDP relay test port")

def reserve_game_port() -> int:
    for port in range(46000, 46998):
        first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        second = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            first.bind(("127.0.0.1", port))
            second.bind(("127.0.0.1", port + 1))
        except OSError:
            first.close()
            second.close()
            continue
        first.close()
        second.close()
        return port
    raise RuntimeError("could not reserve adjacent UDP game test ports")

def run_pair(
    caster: str,
    relay_port: int,
    game: str,
    netplay_dll: str,
    force_client: bool,
) -> None:
    game_port = reserve_game_port()
    env = os.environ.copy()
    env["QOH99_PUBLIC_IP_OVERRIDE"] = "127.0.0.1"
    env["QOH99_RELAY_OVERRIDE"] = f"127.0.0.1:{relay_port}"
    host_args = [
        caster,
        "host",
        str(game_port),
        "--force-relay",
        "--delay",
        "1",
        "--rollback",
        "4",
        "--rendezvous-only",
        "--game",
        game,
        "--dll",
        netplay_dll,
    ]
    client_args = [
        caster,
        "join",
        f"127.0.0.1:{game_port}",
        "--delay",
        "2",
        "--rollback",
        "5",
        "--rendezvous-only",
        "--game",
        game,
        "--dll",
        netplay_dll,
    ]
    if force_client:
        client_args.insert(3, "--force-relay")

    host = subprocess.Popen(
        host_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, env=env
    )
    try:
        time.sleep(0.35)
        client = subprocess.Popen(
            client_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env=env
        )
        try:
            client_out, client_err = client.communicate(timeout=75)
            host_out, host_err = host.communicate(timeout=75)
        finally:
            if client.poll() is None:
                client.kill()
                client.communicate()
    finally:
        if host.poll() is None:
            host.kill()
            host.communicate()

    if host.returncode != 0 or client.returncode != 0:
        raise AssertionError(
            f"unexpected exits host={host.returncode} client={client.returncode}\n"
            f"host stdout:\n{host_out}\nhost stderr:\n{host_err}\n"
            f"client stdout:\n{client_out}\nclient stderr:\n{client_err}"
        )
    expected = "relay" if force_client else "punched-direct"
    if f"UDP path validated ({expected})" not in host_out:
        raise AssertionError(f"host did not commit {expected}:\n{host_out}")
    if f"Session route={expected}" not in client_out:
        raise AssertionError(f"client did not commit {expected}:\n{client_out}")
    if not force_client:
        required = (
            "Direct connection failed; switching to "
            "AutoPunch/relay automatically"
        )
        if required not in client_out:
            raise AssertionError(
                f"automatic TCP fallback was not observed:\n{client_out}"
            )

def relay_service_command(relay_caster: str, port: int) -> list[str]:
    return [relay_caster, "relay", "--bind", "127.0.0.1", "--port", str(port)]

def main() -> int:
    if len(sys.argv) != 5:
        print(
            "usage: auto_route_smoke.py <qoh99caster> "
            "<qoh99caster-for-relay> <qoh99.exe> <qoh99_netplay.dll>\n"
            "       the second path is launched as `<exe> relay --bind ...`; "
            "it is normally the same caster",
              file=sys.stderr)
        return 2
    caster = os.path.abspath(sys.argv[1])
    relay_caster = os.path.abspath(sys.argv[2])
    game = os.path.abspath(sys.argv[3])
    netplay_dll = os.path.abspath(sys.argv[4])
    if not os.path.isfile(game):
        raise AssertionError(
            "required complete November qoh99.exe fixture is unavailable: "
            "configure QOH99_AUTO_ROUTE_GAME_EXE to a full, content-attested "
            "installation; the executable-only evidence root is insufficient"
        )
    if not os.path.isfile(netplay_dll):
        raise AssertionError(f"required netplay DLL is unavailable: {netplay_dll}")
    relay_port = reserve_relay_port()
    relay = subprocess.Popen(
        relay_service_command(relay_caster, relay_port),
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
    )
    try:
        time.sleep(0.35)
        if relay.poll() is not None:
            raise AssertionError(
                "`qoh99caster relay` exited early "
                f"(exit {relay.returncode}): {relay.stderr.read()}"
            )
        run_pair(caster, relay_port, game, netplay_dll,
                 force_client=False)
        run_pair(caster, relay_port, game, netplay_dll,
                 force_client=True)
    finally:
        if relay.poll() is None:
            relay.terminate()
            try:
                relay.wait(timeout=3)
            except subprocess.TimeoutExpired:
                relay.kill()
                relay.wait(timeout=3)
    print("automatic direct-failure, punched-direct, and forced-relay smoke passed")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
