# Two-node sensing with an AP

The existing host accepts nodes 1 and 3. Configure their actual positions with
`--node-positions '1:0,-0.5,1.2;3:2,-3,0.1'` only if those coordinates match the
installation. Coordinates are metres in the host's x/y floor plane with z as
height; the observatory maps them to Three.js coordinates. The drawn baseline
represents configured geometry, not proof of a successful direct radio link.

## Radio modes

Both modes retain the existing 2.4 GHz AP association and UDP host uplink.

| Setting | Default | Purpose |
| --- | --- | --- |
| `radio_mode` | `0` | `0`: current AP CSI/broadcast sync; `1`: directed peer CSI probes |
| `peer_mac` | empty | Other board's Wi-Fi station MAC, not its USB identity |
| `peer_probe_hz` | `10` | Directed probe cadence, bounded to 1-50 Hz |

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

This change does not implement an AP-less host uplink. ESP-NOW probes and
timestamp synchronization are not proof of coherent carrier-phase alignment,
and their payloads are not authenticated sensing authority. Keep the host's
calibration and evidence gates enabled.

## Live observatory

Open `/ui/observatory.html` on the running sensing server. The diagnostics band
shows each node's CSI freshness, RSSI, measured rate, configured position,
calibration state, phase-fusion mode, and recent programmed WASM events.

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
```

Run `run_config_recovery` with `CJSON_DIR` pointing to the installed cJSON source
if that managed-component directory is absent. Build the S3 target with the
installed ESP-IDF environment and an absolute build directory. A successful
build is not a physical-radio test. Firmware flashing, peer provisioning, and
real two-board validation require separate approval; this source change does
not claim improved sensing accuracy or medically valid vital measurements.