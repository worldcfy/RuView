/**
 * @file c6_sync_espnow.c
 * @brief ESP-NOW cross-node time-sync — ADR-110 D1 workaround.
 *
 * Same protocol as c6_timesync.c (TS_BEACON every 100 ms with leader epoch),
 * but over ESP-NOW instead of 802.15.4 because the IDF v5.4 ieee802154 RX
 * path doesn't deliver frames to user-space (see WITNESS-LOG-110 §D1).
 *
 * Frame layout (16 bytes payload, broadcast MAC FF:FF:FF:FF:FF:FF):
 *   [0..3]   Magic         0x53454E50  ('SENP' — Sync via ESP-NOW)
 *   [4]      Protocol ver  0x01
 *   [5]      Leader flag   1 if sender claims leader
 *   [6..7]   Reserved
 *   [8..15]  Leader epoch µs (LE u64)
 */

#include "sdkconfig.h"
#include "c6_sync_espnow.h"
#include "nvs_config.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "mesh_frame.h"
#include <string.h>

static const char *TAG = "c6_espnow";

#define BEACON_MAGIC      0x53454E50u   /* 'SENP' little-endian */
#define BEACON_PROTO_VER  0x01
#define BEACON_PERIOD_MS  100
#define VALID_WINDOW_MS   3000

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  proto_ver;
    uint8_t  leader_flag;
    uint16_t _reserved;
    uint64_t leader_epoch_us;
} espnow_beacon_t;

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t s_destination_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool s_directed_peer = false;
static uint32_t s_probe_period_ms = BEACON_PERIOD_MS;
static bool s_peer_configuration_valid = true;
static uint8_t s_radio_mode = RADIO_MODE_AP;
static uint8_t s_peer_count = 0;
static uint8_t s_peers[CFG_LIMIT_MESH_PEERS_MAX][6];
static uint8_t s_gateway_mac[6];
static bool s_gateway_local = false;
static mesh_frame_state_t s_reassembly[CFG_LIMIT_MESH_PEERS_MAX];
typedef struct {
    uint16_t length;
    uint8_t data[MESH_FRAME_MAX];
} mesh_telemetry_t;
static QueueHandle_t s_relay_queue = NULL;
static QueueHandle_t s_relay_free = NULL;
static mesh_telemetry_t s_relay_pool[8];

bool c6_sync_espnow_is_apless(void) { return s_radio_mode == RADIO_MODE_APLESS; }

bool c6_sync_espnow_source_allowed(const uint8_t *mac)
{
    if (mac == NULL) return false;
    if (!s_directed_peer) return true;
    for (uint8_t index = 0; index < s_peer_count; index++) {
        if (memcmp(mac, s_peers[index], 6) == 0) return true;
    }
    return false;
}

int c6_sync_espnow_relay_telemetry(const uint8_t *data, size_t len)
{
    if (s_relay_queue == NULL || data == NULL || len < 4 || len > MESH_FRAME_MAX) return -1;
    mesh_telemetry_t *frame = NULL;
    if (xQueueReceive(s_relay_free, &frame, 0) != pdTRUE) return -1;
    frame->length = (uint16_t)len;
    memcpy(frame->data, data, len);
    if (xQueueSend(s_relay_queue, &frame, 0) == pdTRUE) return (int)len;
    xQueueSend(s_relay_free, &frame, 0);
    return -1;
}

static void relay_worker(void *context)
{
    (void)context;
    mesh_telemetry_t *frame;
    uint32_t sequence = 0;
    static char line[MESH_FRAME_MAX * 2 + 16];
    const char digits[] = "0123456789abcdef";
    for (;;) {
        if (xQueueReceive(s_relay_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        if (s_gateway_local) {
            memcpy(line, "RUVIEW_MESH ", 12);
            for (size_t index = 0; index < frame->length; index++) {
                line[12 + index * 2] = digits[frame->data[index] >> 4];
                line[13 + index * 2] = digits[frame->data[index] & 15];
            }
            line[12 + frame->length * 2] = '\n';
            usb_serial_jtag_write_bytes(line, 13 + frame->length * 2, pdMS_TO_TICKS(100));
        } else {
            uint8_t packet[MESH_RADIO_MTU];
            size_t offset = 0;
            sequence++;
            while (offset < frame->length) {
                size_t length = mesh_frame_encode(frame->data, frame->length, sequence, offset, packet, sizeof(packet));
                if (length == 0 || esp_now_send(s_gateway_mac, packet, length) != ESP_OK) break;
                offset += length - MESH_FRAGMENT_HEADER_SIZE;
            }
        }
        xQueueSend(s_relay_free, &frame, 0);
    }
}

static uint64_t s_local_id     = 0;   /* 6-byte MAC packed into u64 */
static uint64_t s_leader_id    = 0;
static int64_t  s_offset_us    = 0;
static uint64_t s_last_seen_us = 0;
static bool     s_is_leader    = false;
static TimerHandle_t s_beacon_timer = NULL;

static uint32_t s_tx_count = 0;
static uint32_t s_tx_fail  = 0;
static uint32_t s_rx_count = 0;
static uint32_t s_rx_magic_match = 0;

/* ADR-110 P10 — EMA-smoothed offset (host-side trajectory in firmware).
 *
 * The §A0.8 four-minute soak measured 540 µs sample-stdev around a true
 * offset that drifts at ≈1.4 ppm between two C6 crystals. An exponential
 * moving average with α=0.125 (Q3.3 fixed-point shift = 3) yields an
 * effective ~8-sample window, fast enough to track the drift (~7 µs/sec
 * worst-case) while suppressing the per-beacon WiFi-MAC jitter.
 *
 * Two consumers: get_offset_us() (raw, unchanged — for diagnostics) and
 * get_offset_us_smoothed() (filtered — what CSI frames should stamp).
 * Both expose `int64_t` so call sites stay identical. */
#define OFFSET_EMA_SHIFT 3           /* α = 1/8 = 0.125 */
static int64_t s_offset_us_smoothed = 0;
static bool    s_smoothed_seeded    = false;

static uint64_t mac6_to_u64(const uint8_t mac[6])
{
    return ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
           ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
           ((uint64_t)mac[4] <<  8) |  (uint64_t)mac[5];
}

static void send_beacon(void)
{
    espnow_beacon_t b = {
        .magic           = BEACON_MAGIC,
        .proto_ver       = BEACON_PROTO_VER,
        .leader_flag     = s_is_leader ? 1 : 0,
        ._reserved       = 0,
        .leader_epoch_us = (uint64_t)esp_timer_get_time(),
    };
    uint8_t destinations = s_directed_peer ? s_peer_count : 1;
    for (uint8_t index = 0; index < destinations; index++) {
        const uint8_t *destination = s_directed_peer ? s_peers[index] : s_broadcast_mac;
        esp_err_t result = esp_now_send(destination, (uint8_t *)&b, sizeof(b));
        s_tx_count++;
        if (result != ESP_OK) s_tx_fail++;
    }
    /* Diag log every 50 beacons. */
    if ((s_tx_count % 50) == 1) {
        ESP_LOGI(TAG, "tx#%lu (fail=%lu) rx#%lu (match=%lu) leader=%d offset_us=%lld smoothed=%lld",
                 (unsigned long)s_tx_count, (unsigned long)s_tx_fail,
                 (unsigned long)s_rx_count, (unsigned long)s_rx_magic_match,
                 (int)s_is_leader, (long long)s_offset_us,
                 (long long)s_offset_us_smoothed);
    }
}

/* IDF v5.4 ESP-NOW recv callback signature uses esp_now_recv_info_t.
 * Falls back to the older signature on older IDF via ifdef. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_recv(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len)
{
    const uint8_t *src_mac = info ? info->src_addr : NULL;
#else
static void on_recv(const uint8_t *src_mac, const uint8_t *data, int len)
{
#endif
    s_rx_count++;
    if (!c6_sync_espnow_source_allowed(src_mac)) return;
    if (s_radio_mode == RADIO_MODE_APLESS && s_gateway_local && data != NULL && len > 0) {
        for (uint8_t index = 0; index < s_peer_count; index++) {
            if (memcmp(src_mac, s_peers[index], 6) == 0
                && mesh_frame_feed(&s_reassembly[index], data, (size_t)len) == 1) {
                c6_sync_espnow_relay_telemetry(s_reassembly[index].data, s_reassembly[index].total);
                return;
            }
        }
    }
    if (data == NULL || len < (int)sizeof(espnow_beacon_t)) return;
    const espnow_beacon_t *b = (const espnow_beacon_t *)data;
    if (b->magic != BEACON_MAGIC || b->proto_ver != BEACON_PROTO_VER) return;
    s_rx_magic_match++;
    uint64_t sender_id = src_mac ? mac6_to_u64(src_mac) : 0;
    uint64_t now_us    = (uint64_t)esp_timer_get_time();

    /* Adopt sender as leader if it's claiming leadership AND its ID is
     * lower than our current leader (or we have no leader). Lowest MAC
     * wins — deterministic. */
    if (b->leader_flag && (s_leader_id == 0 || sender_id < s_leader_id)) {
        if (s_is_leader && sender_id < s_local_id) {
            ESP_LOGI(TAG, "stepping down: heard lower-id leader %012llx (we are %012llx)",
                     (unsigned long long)sender_id, (unsigned long long)s_local_id);
            s_is_leader = false;
        }
        s_leader_id = sender_id;
    }

    /* If accepted leader, compute offset from their epoch (only for non-leader). */
    if (b->leader_flag && !s_is_leader && sender_id == s_leader_id) {
        int64_t raw = (int64_t)b->leader_epoch_us - (int64_t)now_us;
        s_offset_us    = raw;
        s_last_seen_us = now_us;
        /* EMA: y[n] = y[n-1] + (raw - y[n-1]) >> SHIFT */
        if (!s_smoothed_seeded) {
            s_offset_us_smoothed = raw;
            s_smoothed_seeded    = true;
        } else {
            s_offset_us_smoothed += (raw - s_offset_us_smoothed) >> OFFSET_EMA_SHIFT;
        }
    }
}

/* Issue #944: ESP-IDF v6.0 changed `esp_now_send_cb_t` from
 *   void (*)(const uint8_t *mac, esp_now_send_status_t status)
 * to
 *   void (*)(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
 * Both signatures ignore the address-side argument here — we only inspect
 * `status` to bump the TX-fail counter — so the body is identical; only the
 * function-pointer type differs.
 *
 * Issue #1005: Espressif backported the new signature to v5.5
 * (`esp_now_send_info_t` = typedef of `wifi_tx_info_t` there), so the guard
 * must be the full version triple, not ESP_IDF_VERSION_MAJOR.
 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) s_tx_fail++;
}
#else
static void on_send(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    if (status != ESP_NOW_SEND_SUCCESS) s_tx_fail++;
}
#endif

static void beacon_timer_cb(TimerHandle_t t)
{
    (void)t;
    uint64_t now = (uint64_t)esp_timer_get_time();
    /* Promote self if no leader beacon for VALID_WINDOW_MS and we have lowest known id. */
    if (!s_is_leader && (now - s_last_seen_us) > (VALID_WINDOW_MS * 1000ULL)) {
        if (s_leader_id == 0 || s_local_id < s_leader_id) {
            s_is_leader  = true;
            s_leader_id  = s_local_id;
            s_offset_us  = 0;
            ESP_LOGI(TAG, "promoting self to leader (no beacons for %u ms; local_id=%012llx)",
                     (unsigned)VALID_WINDOW_MS, (unsigned long long)s_local_id);
        }
    }
    send_beacon();
}

esp_err_t c6_sync_espnow_configure_peer(uint8_t mode, const uint8_t *peer_mac, uint8_t probe_hz)
{
    uint8_t peers[1][6];
    if (peer_mac != NULL) memcpy(peers[0], peer_mac, 6);
    return c6_sync_espnow_configure_mesh(mode, peers, peer_mac != NULL ? 1 : 0, probe_hz, NULL);
}

esp_err_t c6_sync_espnow_configure_mesh(uint8_t mode, const uint8_t peers[][6],
                                      uint8_t count, uint8_t probe_hz, const uint8_t *gateway)
{
    if (s_beacon_timer != NULL) return ESP_ERR_INVALID_STATE;
    s_peer_configuration_valid = false;
    if (count > CFG_LIMIT_MESH_PEERS_MAX || (count != 0 && peers == NULL)) return ESP_ERR_INVALID_ARG;
    if (mode > RADIO_MODE_APLESS || probe_hz < CFG_LIMIT_PEER_PROBE_HZ_MIN
        || probe_hz > CFG_LIMIT_PEER_PROBE_HZ_MAX) return ESP_ERR_INVALID_ARG;
    if (mode == RADIO_MODE_APLESS && (count == 0 || gateway == NULL
        || (gateway[0] & 1u) || mac6_to_u64(gateway) == 0)) return ESP_ERR_INVALID_ARG;
    for (uint8_t index = 0; index < count; index++) {
        if ((peers[index][0] & 1u) || mac6_to_u64(peers[index]) == 0) return ESP_ERR_INVALID_ARG;
        for (uint8_t previous = 0; previous < index; previous++) {
            if (memcmp(peers[index], peers[previous], 6) == 0) return ESP_ERR_INVALID_ARG;
        }
    }
    s_radio_mode = mode;
    s_peer_count = count;
    if (count != 0) memcpy(s_peers, peers, count * 6u);
    if (gateway != NULL) memcpy(s_gateway_mac, gateway, 6);
    if (mode == RADIO_MODE_AP) {
        s_directed_peer = false;
        memcpy(s_destination_mac, s_broadcast_mac, 6);
        s_probe_period_ms = BEACON_PERIOD_MS;
    } else {
        s_directed_peer = count > 0;
        if (count > 0) memcpy(s_destination_mac, peers[0], 6);
        s_probe_period_ms = 1000u / probe_hz;
    }
    s_peer_configuration_valid = true;
    return ESP_OK;
}

esp_err_t c6_sync_espnow_init(void)
{
    uint8_t mac[6];
    esp_err_t mac_result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (mac_result != ESP_OK) return mac_result;
    s_local_id = mac6_to_u64(mac);
    if (!s_peer_configuration_valid) {
        ESP_LOGE(TAG, "Directed sensing needs a distinct unicast peer and a 1..50 Hz probe rate");
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t index = 0; index < s_peer_count; index++) {
        if (memcmp(mac, s_peers[index], 6) == 0) return ESP_ERR_INVALID_ARG;
    }
    s_gateway_local = s_radio_mode == RADIO_MODE_APLESS && memcmp(mac, s_gateway_mac, 6) == 0;
    if (s_radio_mode == RADIO_MODE_APLESS && !s_gateway_local
        && !c6_sync_espnow_source_allowed(s_gateway_mac)) return ESP_ERR_INVALID_ARG;
    esp_err_t r = esp_now_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(r));
        return r;
    }
    r = esp_now_register_recv_cb(on_recv);
    if (r != ESP_OK) return r;
    r = esp_now_register_send_cb(on_send);
    if (r != ESP_OK) return r;

    /* Add broadcast peer so esp_now_send to FF:FF:FF:FF:FF:FF works. */
    uint8_t destinations = s_directed_peer ? s_peer_count : 1;
    for (uint8_t index = 0; index < destinations; index++) {
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_directed_peer ? s_peers[index] : s_broadcast_mac, 6);
    peer.channel = 0;        /* current STA channel */
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    r = esp_now_add_peer(&peer);
    if (r != ESP_OK && r != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(r));
        return r;
    }
    if (s_directed_peer) {
        esp_now_rate_config_t rate_config = {
            .phymode = WIFI_PHY_MODE_HT20,
            .rate = WIFI_PHY_RATE_MCS0_LGI,
            .ersu = false,
            .dcm = false,
        };
        r = esp_now_set_peer_rate_config(peer.peer_addr, &rate_config);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "Peer CSI probe rate configuration failed: %s", esp_err_to_name(r));
            return r;
        }
    }
    }
    if (s_radio_mode == RADIO_MODE_APLESS) {
        s_relay_queue = xQueueCreate(8, sizeof(mesh_telemetry_t *));
        s_relay_free = xQueueCreate(8, sizeof(mesh_telemetry_t *));
        if (s_relay_queue == NULL || s_relay_free == NULL) return ESP_ERR_NO_MEM;
        for (size_t index = 0; index < 8; index++) {
            mesh_telemetry_t *frame = &s_relay_pool[index];
            if (xQueueSend(s_relay_free, &frame, 0) != pdTRUE) return ESP_ERR_NO_MEM;
        }
        if (!usb_serial_jtag_is_driver_installed() && s_gateway_local) {
            usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
            r = usb_serial_jtag_driver_install(&usb_config);
            if (r != ESP_OK) return r;
        }
        if (xTaskCreate(relay_worker, "mesh_relay", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    }

    /* Start as candidate leader — will step down on receiving lower-id beacon. */
    s_is_leader    = true;
    s_leader_id    = s_local_id;
    s_last_seen_us = (uint64_t)esp_timer_get_time();

    s_beacon_timer = xTimerCreate("c6_espnow_beacon",
                                  pdMS_TO_TICKS(s_probe_period_ms),
                                  pdTRUE, NULL, beacon_timer_cb);
    if (s_beacon_timer == NULL) {
        ESP_LOGE(TAG, "xTimerCreate failed");
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(s_beacon_timer, 0) != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "init done: local_id=%012llx directed_peer=%d period=%lums",
             (unsigned long long)s_local_id, (int)s_directed_peer,
             (unsigned long)s_probe_period_ms);
    return ESP_OK;
}

uint64_t c6_sync_espnow_get_epoch_us(void)
{
    /* Prefer the smoothed offset once we've heard a leader beacon; falls
     * back to raw=0 on the leader board and during the first second after
     * follower boot. The smoothed value is what CSI frames should stamp
     * for cross-board multistatic alignment (§A0.8 measured 540 µs raw
     * stdev → expected <100 µs smoothed with α=1/8 over ~8 samples). */
    int64_t off = s_smoothed_seeded ? s_offset_us_smoothed : s_offset_us;
    return (uint64_t)((int64_t)esp_timer_get_time() + off);
}

bool c6_sync_espnow_is_leader(void) { return s_is_leader; }
int64_t c6_sync_espnow_get_offset_us(void) { return s_offset_us; }
int64_t c6_sync_espnow_get_offset_us_smoothed(void) { return s_offset_us_smoothed; }

bool c6_sync_espnow_is_valid(void)
{
    if (s_is_leader) return true;
    uint64_t now = (uint64_t)esp_timer_get_time();
    return (now - s_last_seen_us) < (VALID_WINDOW_MS * 1000ULL);
}

uint32_t c6_sync_espnow_tx_count(void)       { return s_tx_count; }
uint32_t c6_sync_espnow_tx_fail(void)        { return s_tx_fail; }
uint32_t c6_sync_espnow_rx_count(void)       { return s_rx_count; }
uint32_t c6_sync_espnow_rx_magic_match(void) { return s_rx_magic_match; }
