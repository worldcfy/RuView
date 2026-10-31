#!/usr/bin/env python3
"""Generate validated per-board mesh settings without contacting hardware."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re


def normalize_mac(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value):
        raise ValueError("Each node needs a six-byte station MAC")
    octets = bytes.fromhex(value.replace(":", ""))
    if not any(octets) or octets[0] & 1:
        raise ValueError("Station MACs must be nonzero unicast addresses")
    return value.lower()


def build_plan(nodes: list[dict], mode: str, channel: int = 6,
               probe_hz: int = 10, gateway_id: int | None = None) -> dict:
    """Return reciprocal peer settings for a complete configured mesh."""
    if mode not in {"ap", "apless"} or not isinstance(nodes, list):
        raise ValueError("Choose ap or apless with a JSON array of nodes")
    minimum = 1 if mode == "ap" else 2
    if not minimum <= len(nodes) <= 6:
        raise ValueError(f"{mode} mode needs {minimum}-6 nodes")
    if type(channel) is not int or not 1 <= channel <= 11:
        raise ValueError("Mesh channel must be in 1..11")
    if type(probe_hz) is not int or not 1 <= probe_hz <= 50:
        raise ValueError("Probe cadence must be in 1..50 Hz")
    normalized = []
    for node in nodes:
        if not isinstance(node, dict) or type(node.get("node_id")) is not int or not 1 <= node["node_id"] <= 254:
            raise ValueError("Every node needs an integer node_id in 1..254")
        item = {"node_id": node["node_id"], "mac": normalize_mac(node.get("mac"))}
        position = node.get("position")
        if position is not None:
            if not isinstance(position, list) or len(position) != 3 or any(
                type(value) not in (int, float) or not math.isfinite(value) for value in position
            ):
                raise ValueError("Positions must contain three finite measured coordinates")
            item["position"] = position
        normalized.append(item)
    if len({node["node_id"] for node in normalized}) != len(nodes) or len({node["mac"] for node in normalized}) != len(nodes):
        raise ValueError("Node IDs and station MACs must be distinct")
    normalized.sort(key=lambda node: node["node_id"])
    gateway = None
    if mode == "apless":
        gateway_id = normalized[0]["node_id"] if gateway_id is None else gateway_id
        gateway = next((node for node in normalized if node["node_id"] == gateway_id), None)
        if gateway is None:
            raise ValueError("The USB gateway must be one of the configured nodes")
    result = []
    for index, node in enumerate(normalized):
        peers = [other["mac"] for other in normalized if other["node_id"] != node["node_id"]]
        result.append({**node, "settings": {
            "radio_mode": 1 if mode == "ap" else 2,
            "node_id": node["node_id"],
            "tdm_node_count": len(normalized),
            "tdm_slot_index": index,
            "peer_macs": ",".join(peers),
            "peer_probe_hz": probe_hz,
            "mesh_channel": channel,
            "mesh_gateway_mac": gateway["mac"] if gateway else "",
        }})
    geometry = ";".join(
        str(node["node_id"]) + ":" + ",".join(str(value) for value in node["position"])
        for node in normalized if "position" in node
    )
    return {"schema": "ruview.mesh-plan.v1", "mode": mode,
            "gateway_id": gateway["node_id"] if gateway else None,
            "host_node_positions": geometry or None, "nodes": result}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nodes-file", type=Path, required=True)
    parser.add_argument("--mode", choices=("ap", "apless"), required=True)
    parser.add_argument("--channel", type=int, default=6)
    parser.add_argument("--probe-hz", type=int, default=10)
    parser.add_argument("--gateway-id", type=int)
    arguments = parser.parse_args()
    try:
        nodes = json.loads(arguments.nodes_file.read_text())
        plan = build_plan(nodes, arguments.mode, arguments.channel, arguments.probe_hz, arguments.gateway_id)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(plan, indent=2))


if __name__ == "__main__":
    main()