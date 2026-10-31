import importlib.util
from pathlib import Path
import struct
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "mesh-usb-bridge.py"
SPEC = importlib.util.spec_from_file_location("mesh_usb_bridge", SCRIPT)
BRIDGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BRIDGE)


class MeshUsbBridgeTests(unittest.TestCase):
    def frame(self, node_id):
        frame = bytearray(22)
        struct.pack_into("<I", frame, 0, 0xC5110001)
        frame[4] = node_id
        frame[5] = 1
        struct.pack_into("<H", frame, 6, 1)
        return bytes(frame)

    def test_preserves_distinct_node_frames(self):
        for node_id in (1, 6):
            frame = self.frame(node_id)
            line = b"RUVIEW_MESH " + frame.hex().encode() + b"\n"
            self.assertEqual(BRIDGE.decode_mesh_line(line, {1, 6}), frame)

    def test_rejects_unlisted_nodes_and_non_mesh_lines(self):
        line = b"RUVIEW_MESH " + self.frame(3).hex().encode()
        self.assertIsNone(BRIDGE.decode_mesh_line(line, {1, 6}))
        self.assertIsNone(BRIDGE.decode_mesh_line(b"boot log", {1}))

    def test_rejects_malformed_and_oversized_payloads(self):
        self.assertIsNone(BRIDGE.decode_mesh_line(b"RUVIEW_MESH zz", {1}))
        self.assertIsNone(BRIDGE.decode_mesh_line(b"RUVIEW_MESH " + b"00" * 2069, {1}))
        self.assertIsNone(BRIDGE.decode_mesh_line(b"RUVIEW_MESH 01020304", {1}))


if __name__ == "__main__":
    unittest.main()