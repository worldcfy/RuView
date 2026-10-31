#!/usr/bin/env python3
"""Forward complete AP-less gateway telemetry to the local RuView UDP receiver."""

from __future__ import annotations

import argparse
import os
import select
import socket
import struct
import termios
import time
import tty

PREFIX = b"RUVIEW_MESH "
MAX_FRAME = 2068
MAX_LINE = len(PREFIX) + MAX_FRAME * 2 + 2


def decode_mesh_line(line: bytes, allowed_nodes: set[int]) -> bytes | None:
    """Validate a bounded gateway record without modifying its telemetry bytes."""
    if len(line) > MAX_LINE or not line.startswith(PREFIX):
        return None
    encoded = line[len(PREFIX):].rstrip(b"\r\n")
    if not encoded or len(encoded) % 2 or any(value not in b"0123456789abcdefABCDEF" for value in encoded):
        return None
    frame = bytes.fromhex(encoded.decode("ascii"))
    if len(frame) < 4 or len(frame) > MAX_FRAME:
        return None
    magic = struct.unpack_from("<I", frame)[0]
    if magic >> 16 != 0xC511:
        return None
    if magic == 0xC5110001:
        if len(frame) < 20 or frame[4] not in allowed_nodes:
            return None
        antennas = frame[5]
        subcarriers = struct.unpack_from("<H", frame, 6)[0]
        if not 1 <= antennas <= 4 or not 1 <= subcarriers <= 1024:
            return None
        if len(frame) != 20 + antennas * subcarriers * 2:
            return None
    return frame


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="native USB serial/JTAG device path")
    parser.add_argument("--nodes", required=True, help="comma-separated allowed node IDs, 1-6 nodes")
    parser.add_argument("--udp-port", type=int, default=5005)
    arguments = parser.parse_args()
    try:
        nodes = [int(value) for value in arguments.nodes.split(",")]
    except ValueError:
        parser.error("--nodes must contain integer node IDs")
    if not 1 <= len(nodes) <= 6 or len(set(nodes)) != len(nodes) or any(not 1 <= value <= 254 for value in nodes):
        parser.error("--nodes requires 1-6 distinct node IDs in 1..254")
    if not 1 <= arguments.udp_port <= 65535:
        parser.error("--udp-port must be in 1..65535")

    descriptor = os.open(arguments.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    previous = termios.tcgetattr(descriptor)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    pending = bytearray()
    forwarded = rejected = 0
    reported_at = time.monotonic()
    try:
        tty.setraw(descriptor)
        print(f"USB mesh bridge -> 127.0.0.1:{arguments.udp_port}; nodes={nodes}", flush=True)
        while True:
            ready, _, _ = select.select([descriptor], [], [], 1)
            if ready:
                chunk = os.read(descriptor, 8192)
                if not chunk:
                    raise RuntimeError("USB gateway disconnected")
                pending.extend(chunk)
                while b"\n" in pending:
                    line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    if not line.startswith(PREFIX):
                        continue
                    frame = decode_mesh_line(bytes(line), set(nodes))
                    if frame is None:
                        rejected += 1
                    else:
                        udp.sendto(frame, ("127.0.0.1", arguments.udp_port))
                        forwarded += 1
                if len(pending) > MAX_LINE:
                    pending.clear()
                    rejected += 1
            if time.monotonic() - reported_at >= 10:
                print(f"forwarded={forwarded} rejected={rejected}", flush=True)
                reported_at = time.monotonic()
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(descriptor, termios.TCSANOW, previous)
        os.close(descriptor)
        udp.close()


if __name__ == "__main__":
    main()