# ESP32 mesh sensing: AP connected and AP less

The existing host accepts nodes 1 and 3. Configure their actual positions with
`--node-positions '1:0,-0.5,1.2;3:2,-3,0.1'` only if those coordinates match the
installation. Coordinates are metres in the host's x/y floor plane with z as
height; the observatory maps them to Three.js coordinates. The drawn baseline
represents configured geometry, not proof of a successful direct radio link.

## Radio modes

AP-connected configurations support 1-6 ESP32s and retain the existing 2.4 GHz
AP association and UDP host uplink. AP-less configurations support 2-6 ESP32s
on one fixed Wi-Fi channel, using a native USB serial/JTAG gateway to the host.

| Setting | Default | Purpose |
| --- | --- | --- |
| `radio_mode` | `0` | `0`: AP CSI/broadcast sync; `1`: AP-connected peers; `2`: AP-less mesh |
| `peer_mac` | empty | Other board's Wi-Fi station MAC, not its USB identity |
| `peer_macs` | empty | Comma-separated distinct station MACs of up to five other nodes; replaces the legacy single peer when present |
| `peer_probe_hz` | `10` | Directed probe cadence, bounded to 1-50 Hz |
| `mesh_channel` | `6` | AP-less fixed channel, 1-11; ignored for AP-associated channel selection |
| `mesh_gateway_mac` | empty | Native-USB gateway's station MAC; required on every AP-less node, including the gateway itself |

Directed mode keeps the AP channel, disables channel hopping, filters CSI to the
configured peer, and requests an HT20/MCS0 ESP-NOW rate. Set reciprocal station
MACs on the two boards. Use the authenticated firmware configuration tool after
an explicitly approved firmware update:

```bash
python firmware/esp32-csi-node/config_push.py --node BOARD_IP \
  --psk-file EXISTING_PSK_FILE --set radio_mode=1 \
  peer_mac=OTHER_BOARD_STATION_MAC peer_probe_hz=20
```

Substitute the node's actual private IP, existing credential file, and verified
peer station MAC. The command can reboot the node; it is not run automatically.
Read settings back with the same tool's `--get` option. To return to AP-only CSI,
set `radio_mode=0`. Settings are validated before NVS writes. Empty peer MACs
are allowed as unconfigured state, but directed operation requires a valid,
distinct, nonzero unicast peer.

## Generate reciprocal mesh settings

Prepare an operator inventory containing the actual station MAC and unique ID
for each node, plus optional measured positions. The input is a JSON array of
objects with `node_id`, `mac`, and optionally `position: [x, y, z]`.

```bash
python scripts/mesh-topology.py --nodes-file VERIFIED_NODE_INVENTORY.json --mode ap
python scripts/mesh-topology.py --nodes-file VERIFIED_NODE_INVENTORY.json \
  --mode apless --gateway-id 1 --channel 6 --probe-hz 10
```

The planner validates sizes, IDs, unicast MACs, duplicate identities, channels,
cadence, and finite positions. It emits each board's existing config-API keys,
reciprocal peer list, common gateway, and consistent TDM indices. It does not
contact a board or invent physical coordinates. `host_node_positions` can be
passed to the server's existing `--node-positions` option when present.

Apply only the generated `settings` object for the matching physical board
through the authenticated configuration workflow after approved flashing.
Every node's peer list excludes itself. AP-connected mode with one node has no
direct peers; it retains AP CSI. All nodes in AP-less mode must use the same
channel and gateway MAC. Changing mode requires a reboot, and HTTP
configuration becomes unavailable after leaving the AP; retain a USB recovery
path and do not rely on Wi-Fi-only provisioning for later mode changes.

## AP-less USB gateway

Connect exactly the designated gateway's native USB serial/JTAG port to the
host. Its firmware reassembles bounded ESP-NOW telemetry fragments from the
configured peers and exports complete datagrams as versioned mesh records.
Local gateway CSI follows the same export path. No independent AP is created,
and no PC ESP-NOW-capable Wi-Fi driver is assumed.

```bash
python scripts/mesh-usb-bridge.py --port /dev/ttyACM0 --nodes 1,3 --udp-port 5005
```

Use the actual gateway port and full configured node-ID set. The bridge uses
POSIX serial I/O, does not reset the board, restores terminal settings on exit,
rejects malformed/oversized records and unlisted raw CSI node IDs, and forwards
unchanged datagrams to `127.0.0.1:5005`. The normal sensing server must be
running. ESP-NOW, USB backpressure, and the bounded eight-frame pool can drop
frames; this is not a lossless transport or a verified six-node throughput
claim. Capture rate and freshness must be measured after deployment.

ESP-NOW probes and timestamp synchronization are not proof of coherent
carrier-phase alignment. MAC allow-lists and CRC/FCS are not authentication.
Keep host calibration and evidence gates enabled; this remains experimental
sensing rather than medical or security authority.

## Live observatory

Open `/ui/observatory.html` on the running sensing server. The diagnostics band
shows each node's CSI freshness, RSSI, measured rate, configured position,
calibration state, phase-fusion mode, and recent programmed WASM events.
Configured geometry renders 1-6 sensors and up to 15 peer edges. In Settings,
select the topology reference to hide the external AP for an AP-less layout.
That selection affects visualization only; it neither configures radios nor
proves that the drawn peer links exchanged CSI.

Zero-confidence pose placeholders do not produce a live skeleton. A complete
supplied pose with confident keypoints is shown as an estimate rather than
regenerated through demo animation. Vital readings are displayed only when the
server publishes qualified values; abstention is shown explicitly. Gesture
events require a suitable running module/model. No numeric vital or gesture
output is invented to fill an unavailable measurement.

## Verification and deployment limits

```bash
node --test ui/observatory/js/main.test.mjs ui/services/ws-ticket.test.mjs
make -C firmware/esp32-csi-node/test run_config_rules run_self_ping run_csi_sanitize
make -C firmware/esp32-csi-node/test run_mesh_frame
python -m unittest discover -s tests -p 'test_mesh_*.py'
```

Run `run_config_recovery` with `CJSON_DIR` pointing to the installed cJSON source
if that managed-component directory is absent. Build the S3 target with the
installed ESP-IDF environment and an absolute build directory. A successful
build is not a physical-radio test. Firmware flashing, peer provisioning, and
real two-board validation require separate approval; this source change does
not claim improved sensing accuracy or medically valid vital measurements.