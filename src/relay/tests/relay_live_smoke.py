
from __future__ import annotations

import argparse
import os
import signal
import socket
import struct
import subprocess
import sys
import time
import zlib

QNET_MAGIC = 0x4E393951
QRL_MAGIC = 0x314C5251
QNP_MAGIC = 0x35504E51
QNP_WIRE_VERSION = 14
QNP_COMPATIBILITY_IDENTITY_BYTES = 192
ROOM_LOW = 0x0123456789ABCDEF
ROOM_HIGH = 0xFEDCBA9876543210

def receive_exact(sock: socket.socket, length: int) -> bytes:
    data = b""
    while len(data) < length:
        part = sock.recv(length - len(data))
        if not part:
            raise RuntimeError(
                f"unexpected TCP EOF after {len(data)}/{length} bytes"
            )
        data += part
    return data

def qnet_frame(message_type: int, body: bytes) -> bytes:
    checksum = zlib.crc32(body) & 0xFFFFFFFF if body else 0
    message = (
        struct.pack(
            "<IBBHI",
            QNET_MAGIC,
            message_type,
            0,
            len(body),
            checksum,
        )
        + body
    )
    return struct.pack("<H", len(message)) + message

def receive_qnet(sock: socket.socket) -> tuple[int, bytes, bytes]:
    length = struct.unpack("<H", receive_exact(sock, 2))[0]
    message = receive_exact(sock, length)
    magic, message_type, flags, body_length, checksum = struct.unpack(
        "<IBBHI", message[:12]
    )
    body = message[12:]
    assert magic == QNET_MAGIC
    assert flags == 0
    assert body_length == len(body)
    assert checksum == (zlib.crc32(body) & 0xFFFFFFFF if body else 0)
    return message_type, body, struct.pack("<H", length) + message

def relay_hello(
    room_low: int,
    room_high: int,
    domain: int,
    role: int,
) -> bytes:
    body = struct.pack(
        "<IHBBQQBB2x",
        QRL_MAGIC,
        1,
        1,
        0,
        room_low,
        room_high,
        domain,
        role,
    )
    assert len(body) == 28
    return qnet_frame(0x70, body)

def receive_welcome(
    sock: socket.socket,
    expected_status: int,
    expected_role: int,
) -> list[tuple[int, int]]:
    message_type, body, _ = receive_qnet(sock)
    assert message_type == 0x70
    assert len(body) == 80
    assert struct.unpack("<IHBB", body[:8]) == (QRL_MAGIC, 1, 2, 0)
    low, high = struct.unpack("<QQ", body[8:24])
    domain, role, status = body[24], body[25], body[26]
    assert (low, high, domain, role, status) == (
        ROOM_LOW,
        ROOM_HIGH,
        1,
        expected_role,
        expected_status,
    )
    return [
        struct.unpack("<QQ", body[offset : offset + 16])
        for offset in (28, 44, 60)
    ]

def relay_register(
    role: int,
    token: tuple[int, int],
    domain: int = 1,
) -> bytes:
    record = struct.pack(
        "<IHBBQQBB6xQQ",
        QRL_MAGIC,
        1,
        3,
        0,
        ROOM_LOW,
        ROOM_HIGH,
        domain,
        role,
        token[0],
        token[1],
    )
    assert len(record) == 48
    return record

def receive_ready(
    sock: socket.socket,
    relay: tuple[str, int],
    role: int,
    token: tuple[int, int],
) -> int:
    body, source = sock.recvfrom(256)
    assert source == relay
    assert len(body) == 52
    assert struct.unpack("<IHBB", body[:8]) == (QRL_MAGIC, 1, 4, 0)
    low, high = struct.unpack("<QQ", body[8:24])
    domain, received_role = body[24], body[25]
    received_token = struct.unpack("<QQ", body[28:44])
    peer_ipv4, peer_port = struct.unpack("<IH", body[44:50])
    assert (low, high, domain, received_role, received_token) == (
        ROOM_LOW,
        ROOM_HIGH,
        1,
        role,
        token,
    )
    assert peer_ipv4 != 0
    assert peer_port != 0
    return peer_port

def qnp_hello(
    message_type: int,
    role: int,
    player: int,
    sequence: int,
) -> bytes:
    identity = struct.pack(
        "<II32s32sQIII32s32sIIQQQI",
        1,
        QNP_WIRE_VERSION,
        b"fixture-profile".ljust(32, b"\0"),
        bytes(range(0x20, 0x40)),
        704512,
        0x3A19A8A9,
        1,
        1,
        bytes(range(0x50, 0x70)),
        bytes(range(0x90, 0xB0)),
        16,
        12,
        0x1011121314151617,
        0x2021222324252627,
        0x3031323334353637,
        0,
    )
    assert len(identity) == QNP_COMPATIBILITY_IDENTITY_BYTES
    payload = (
        struct.pack("<QBB2x", 0xABCDEF0123456789, role, player) + identity
    )
    assert len(payload) == 12 + QNP_COMPATIBILITY_IDENTITY_BYTES
    header = struct.pack(
        "<IHBBHHQQQI",
        QNP_MAGIC,
        QNP_WIRE_VERSION,
        message_type,
        0,
        len(payload),
        0,
        0x1122334455667788,
        0x0102030405060708,
        0x1112131415161718,
        sequence,
    )
    packet = header + struct.pack("<I", 0) + payload
    checksum = zlib.crc32(packet) & 0xFFFFFFFF
    return packet[:40] + struct.pack("<I", checksum) + packet[44:]

def reserve_loopback_port() -> int:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]
    finally:
        probe.close()

class WindowsRelayProcess:

    def __init__(self, caster: str, bind: str, port: int) -> None:
        self.command = [caster, "relay", "--bind", bind, "--port", str(port)]
        creation_flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        self.process = subprocess.Popen(
            self.command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=creation_flags,
        )

    def running(self) -> bool:
        return self.process.poll() is None

    def request_stop(self) -> None:
        try:
            self.process.send_signal(signal.CTRL_BREAK_EVENT)
            self.process.wait(timeout=5)
        except (AttributeError, OSError, subprocess.TimeoutExpired):
            self.force_stop()

    def force_stop(self) -> None:
        self.process.terminate()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)

def run(caster: str) -> None:
    port = reserve_loopback_port()
    relay = ("127.0.0.1", port)
    service = WindowsRelayProcess(caster, relay[0], relay[1])
    process = service.process
    opened: list[socket.socket] = []
    try:
        deadline = time.monotonic() + 5.0
        while True:
            try:
                host = socket.create_connection(relay, timeout=0.1)
                opened.append(host)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        "`qoh99caster relay` did not start within five seconds"
                    )
                time.sleep(0.02)

        host.sendall(relay_hello(ROOM_LOW, ROOM_HIGH, 1, 1))
        assert receive_welcome(host, 2, 1) == [(0, 0)] * 3

        client = socket.create_connection(relay, timeout=2)
        opened.append(client)
        client.sendall(relay_hello(ROOM_LOW, ROOM_HIGH, 1, 2))
        client_tokens = receive_welcome(client, 1, 2)
        host_tokens = receive_welcome(host, 1, 1)
        assert client_tokens[0] != (0, 0)
        assert client_tokens[1:] == [(0, 0), (0, 0)]
        assert all(token != (0, 0) for token in host_tokens)
        assert len(set(host_tokens + [client_tokens[0]])) == 4

        host_control = qnet_frame(1, struct.pack("<I", 4))
        client_control = qnet_frame(2, b"client-config")
        host.sendall(host_control)
        client.sendall(client_control)
        assert receive_qnet(client)[2] == host_control
        assert receive_qnet(host)[2] == client_control

        duplicate = socket.create_connection(relay, timeout=2)
        opened.append(duplicate)
        duplicate.sendall(relay_hello(ROOM_LOW, ROOM_HIGH, 1, 2))
        assert receive_welcome(duplicate, 4, 2) == [(0, 0)] * 3

        unknown = socket.create_connection(relay, timeout=2)
        opened.append(unknown)
        unknown.sendall(relay_hello(ROOM_LOW ^ 1, ROOM_HIGH, 1, 2))
        assert receive_qnet(unknown)[1][26] == 3

        spectator = socket.create_connection(relay, timeout=2)
        opened.append(spectator)
        spectator.sendall(relay_hello(ROOM_LOW ^ 2, ROOM_HIGH, 2, 1))
        assert receive_qnet(spectator)[1][26] == 5

        host_udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client_udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        attacker = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        opened.extend((host_udp, client_udp, attacker))
        for udp in (host_udp, client_udp, attacker):
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(2)

        wrong_token = (host_tokens[0][0] ^ 1, host_tokens[0][1])
        attacker.settimeout(0.15)
        attacker.sendto(relay_register(1, wrong_token), relay)
        try:
            attacker.recvfrom(256)
            raise AssertionError("wrong registration token received a response")
        except socket.timeout:
            pass

        host_udp.sendto(relay_register(1, host_tokens[0]), relay)
        client_udp.sendto(relay_register(2, client_tokens[0]), relay)
        assert (
            receive_ready(host_udp, relay, 1, host_tokens[0])
            == client_udp.getsockname()[1]
        )
        assert (
            receive_ready(client_udp, relay, 2, client_tokens[0])
            == host_udp.getsockname()[1]
        )

        client_hello = qnp_hello(1, 2, 1, 7)
        client_udp.sendto(client_hello, relay)
        relayed, source = host_udp.recvfrom(1200)
        assert relayed == client_hello
        assert source == relay

        host_ack = qnp_hello(2, 1, 0, 8)
        host_udp.sendto(host_ack, relay)
        relayed, source = client_udp.recvfrom(1200)
        assert relayed == host_ack
        assert source == relay

        corrupted = bytearray(client_hello)
        corrupted[-1] ^= 1
        client_udp.sendto(corrupted, relay)
        client_udp.sendto(qnp_hello(1, 1, 1, 9), relay)
        host_udp.settimeout(0.2)
        try:
            host_udp.recvfrom(1200)
            raise AssertionError("invalid QNP player datagram was forwarded")
        except socket.timeout:
            pass
    finally:
        for opened_socket in opened:
            try:
                opened_socket.close()
            except OSError:
                pass

        if service.running():
            service.request_stop()

        stdout, stderr = process.communicate(timeout=2)
        if process.returncode != 0:
            sys.stderr.write(stdout.decode(errors="replace"))
            sys.stderr.write(stderr.decode(errors="replace"))
            raise RuntimeError(
                "`qoh99caster relay` did not shut down cleanly "
                f"(exit {process.returncode})"
            )

def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Live loopback acceptance test for the caster's relay service "
            "mode (`qoh99caster relay`)."
        )
    )
    parser.add_argument(
        "caster",
        help="path to qoh99caster.exe (launched as `qoh99caster relay`)",
    )
    arguments = parser.parse_args()
    caster = os.path.abspath(arguments.caster)
    if not os.path.isfile(caster):
        parser.error(f"caster executable not found: {caster}")
    run(caster)
    print("qoh99 relay live smoke: PASS")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
