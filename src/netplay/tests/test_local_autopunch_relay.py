
from __future__ import annotations

import socket
import struct

from live_game_e2e import LocalAutopunchRelay

def decode_mapping(packet: bytes) -> tuple[int, int, str]:
    if len(packet) != 8:
        raise AssertionError(f"unexpected mapping length: {len(packet)}")
    internal, observed = struct.unpack("!HH", packet[:4])
    return internal, observed, socket.inet_ntoa(packet[4:8])

def main() -> int:
    relay = LocalAutopunchRelay()
    try:
        requester_observed = ("127.0.0.1", 61001)
        target_observed = ("127.0.0.1", 48127)
        with relay._lock:
            assert relay._store_registration_locked(
                requester_observed, 54000, 100.0
            )
            assert relay._store_registration_locked(target_observed, 7501, 100.0)
            resolved = relay._resolve_lookup_locked(
                requester_observed, 54000, "127.0.0.1", 7501, 101.0
            )
            assert resolved is not None
            requester_packet, requester_destination, target_packet, target_destination = resolved

        assert requester_destination == requester_observed
        assert target_destination == target_observed
        assert decode_mapping(requester_packet) == (7501, 48127, "127.0.0.1")
        assert decode_mapping(target_packet) == (54000, 61001, "127.0.0.1")

        with relay._lock:
            assert (
                relay._resolve_lookup_locked(
                    requester_observed, 54001, "127.0.0.1", 7501, 101.0
                )
                is None
            )
        print("LocalAutopunchRelay unequal-port semantics: PASS", flush=True)
        return 0
    finally:
        relay.socket.close()

if __name__ == "__main__":
    raise SystemExit(main())
