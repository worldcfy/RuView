import importlib.util
from pathlib import Path
import unittest

PATH = Path(__file__).resolve().parents[1] / "scripts" / "mesh-topology.py"
SPEC = importlib.util.spec_from_file_location("mesh_topology", PATH)
TOPOLOGY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOPOLOGY)


class MeshTopologyTests(unittest.TestCase):
    def nodes(self, count):
        return [{"node_id": index + 1, "mac": f"02:00:00:00:00:{index + 1:02x}"} for index in range(count)]

    def test_ap_mode_accepts_one_through_six(self):
        for count in range(1, 7):
            plan = TOPOLOGY.build_plan(self.nodes(count), "ap")
            self.assertEqual(len(plan["nodes"]), count)
            for node in plan["nodes"]:
                peers = node["settings"]["peer_macs"].split(",") if count > 1 else []
                self.assertEqual(len(peers), count - 1)
                self.assertNotIn(node["mac"], peers)
                self.assertEqual(node["settings"]["radio_mode"], 1)
                self.assertEqual(node["settings"]["tdm_node_count"], count)
                self.assertEqual(node["settings"]["node_id"], node["node_id"])

    def test_apless_mode_accepts_two_through_six_with_one_gateway(self):
        for count in range(2, 7):
            plan = TOPOLOGY.build_plan(self.nodes(count), "apless", gateway_id=count)
            self.assertEqual(plan["gateway_id"], count)
            gateways = {node["settings"]["mesh_gateway_mac"] for node in plan["nodes"]}
            self.assertEqual(len(gateways), 1)
            self.assertTrue(all(node["settings"]["radio_mode"] == 2 for node in plan["nodes"]))

    def test_rejects_invalid_sizes_and_duplicate_identities(self):
        for count in (0, 7):
            with self.assertRaises(ValueError):
                TOPOLOGY.build_plan(self.nodes(count), "ap")
        with self.assertRaises(ValueError):
            TOPOLOGY.build_plan(self.nodes(1), "apless")
        with self.assertRaises(ValueError):
            TOPOLOGY.build_plan([self.nodes(2)[0], self.nodes(2)[0]], "ap")

    def test_preserves_only_measured_finite_geometry(self):
        nodes = self.nodes(2)
        nodes[0]["position"] = [0, -0.5, 1.2]
        self.assertEqual(TOPOLOGY.build_plan(nodes, "ap")["host_node_positions"], "1:0,-0.5,1.2")
        nodes[0]["position"] = [0, float("nan"), 1.2]
        with self.assertRaises(ValueError):
            TOPOLOGY.build_plan(nodes, "ap")


if __name__ == "__main__":
    unittest.main()