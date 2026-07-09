/**
 * @file csi_collector.c
 * @brief CSI data collection and ADR-018 binary frame serialization.
 *
 * Registers the ESP-IDF WiFi CSI callback and serializes incoming CSI data
 * into the ADR-018 binary frame format for UDP transmission.
 *
 * ADR-029 extensions:
 *   - Channel-hop table for multi-band sensing (channels 1/6/11 by default)
 *   - Timer-driven channel hopping at configurable dwell intervals
 *   - NDP frame injection stub for sensing-first TX
 */

#include "csi_collector.h"
#include "nvs_config.h"
#include "stream_sender.h"
#include "edge_processing.h"
#include "c6_timesync.h"  /* ADR-110: 802.15.4 epoch for cross-node alignment */
#include "c6_sync_espnow.h" /* ADR-110 §A0.11: mesh-aligned epoch for sync packet */

#include <string.h>
#include "psa/crypto.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_netif.h"          /* #954: STA gateway lookup for self-ping CSI source */
#include "ping/ping_sock.h"     /* #954: esp_ping gateway traffic generator */
#include "lwip/ip_addr.h"       /* #954: ip_addr_t target for esp_ping */

/* ADR-060: Access the global NVS config for MAC filter and channel override. */
extern nvs_config_t g_nvs_config;

/* Defensive fix (#232, #375, #385, #386, #390): capture NVS config fields into
 * module-local statics BEFORE wifi_init_sta() runs, because WiFi driver init
 * can corrupt g_nvs_config (confirmed on device 80:b5:4e:c1:be:b8).
 * main.c calls csi_collector_set_node_id() immediately after nvs_config_load(),
 * and all runtime paths use the local copies exclusively. */
static uint8_t s_node_id = 1;
static bool s_node_id_early_set = false;

/* Defensive copy of MAC filter config — the CSI callback fires at 100-500 Hz
 * and reads filter_mac_set + filter_mac on every invocation. If wifi_init_sta()
 * corrupts g_nvs_config, the callback would read garbage, potentially causing
 * LoadProhibited panics (observed: Core 0 panic after ~2400 callbacks). */
static uint8_t s_filter_mac[6] = {0};
static bool    s_filter_mac_set = false;

/* ADR-032 §2.8: Derived SipHash-2-4 key for CSI frame integrity. */
static uint8_t s_siphash_key[16] = {0};
static bool    s_siphash_key_valid = false;

/* ADR-057: Build-time guard — fail early if CSI is not enabled in sdkconfig.
 * Without this, the firmware compiles but crashes at runtime with:
 *   "E (xxxx) wifi:CSI not enabled in menuconfig!"
 * which is confusing for users flashing pre-built binaries. */
#ifndef CONFIG_ESP_WIFI_CSI_ENABLED
#error "CONFIG_ESP_WIFI_CSI_ENABLED must be set in sdkconfig. " \
       "Run: idf.py menuconfig -> Component config -> Wi-Fi -> Enable WiFi CSI, " \
       "or copy sdkconfig.defaults.template to sdkconfig.defaults before building."
#endif

static const char *TAG = "csi_collector";

static uint32_t s_sequence = 0;
static uint32_t s_cb_count = 0;
static uint32_t s_send_ok = 0;
static uint32_t s_send_fail = 0;
static uint32_t s_rate_skip = 0;

/**
 * Minimum interval between UDP sends in microseconds.
 * CSI callbacks can fire hundreds of times per second in promiscuous mode.
 * We cap the send rate to avoid exhausting lwIP packet buffers (ENOMEM).
 * Default: 20 ms = 50 Hz max send rate.
 */
#define CSI_MIN_SEND_INTERVAL_US  (20 * 1000)
static int64_t s_last_send_us = 0;

/**
 * Minimum interval between processing ANY CSI callback in microseconds.
 * Promiscuous MGMT+DATA can fire 100-500+ times/sec. At rates above ~50 Hz,
 * the WiFi FIQ handler (wDev_ProcessFiq) races with SPI flash cache operations,
 * causing Core 0 LoadProhibited panics in cache_ll_l1_resume_icache.
 *
 * This early gate drops excess callbacks BEFORE any processing (serialization,
 * UDP, edge enqueue), keeping the effective callback rate at ~50 Hz while
 * preserving the full MGMT+DATA promiscuous filter and HT-LTF/STBC CSI quality.
 *
 * The WiFi hardware still captures all frames and the CSI data is generated,
 * but we simply discard the excess in software. This reduces the time spent
 * in callback context per second, giving the WiFi ISR more headroom.
 */
#define CSI_MIN_PROCESS_INTERVAL_US  (20 * 1000)  /* 50 Hz */
static int64_t s_last_process_us = 0;
static uint32_t s_early_drop = 0;

/* ---- ADR-029: Channel-hop state ---- */

/** Channel hop table (populated from NVS at boot or via set_hop_table). */
static uint8_t  s_hop_channels[CSI_HOP_CHANNELS_MAX] = {1, 6, 11, 36, 40, 44};

/** Number of active channels in the hop table. 1 = single-channel (no hop). */
static uint8_t  s_hop_count   = 1;

/** Dwell time per channel in milliseconds. */
static uint32_t s_dwell_ms    = 50;

/** Current index into s_hop_channels. */
static uint8_t  s_hop_index   = 0;

/** Handle for the periodic hop timer. NULL when timer is not running. */
static esp_timer_handle_t s_hop_timer = NULL;

/**
 * Serialize CSI data into ADR-018 binary frame format.
 *
 * Layout:
 *   [0..3]   Magic: 0xC5110001 (LE)
 *   [4]      Node ID
 *   [5]      Number of antennas (rx_ctrl.rx_ant + 1 if available, else 1)
 *   [6..7]   Number of subcarriers (LE u16) = len / (2 * n_antennas)
 *   [8..11]  Frequency MHz (LE u32) — derived from channel
 *   [12..15] Sequence number (LE u32)
 *   [16]     RSSI (i8)
 *   [17]     Noise floor (i8)
 *   [18..19] Reserved
 *   [20..]   I/Q data (raw bytes from ESP-IDF callback)
 */
size_t csi_serialize_frame(const wifi_csi_info_t *info, uint8_t *buf, size_t buf_len)
{
    if (info == NULL || buf == NULL || info->buf == NULL) {
        return 0;
    }

    uint8_t n_antennas = 1;  /* ESP32-S3 typically reports 1 antenna for CSI */
    uint16_t iq_len = (uint16_t)info->len;
    uint16_t n_subcarriers = iq_len / (2 * n_antennas);

    uint8_t header_len = CSI_HEADER_SIZE;
    uint32_t magic = CSI_MAGIC;

    if (s_siphash_key_valid && g_nvs_config.sec_level >= 1) {
        header_len = CSI_AUTHENTICATED_HEADER_SIZE;
        magic = CSI_AUTHENTICATED_MAGIC;
    }

    size_t frame_size = header_len + iq_len;
    if (frame_size > buf_len) {
        ESP_LOGW(TAG, "Buffer too small: need %u, have %u", (unsigned)frame_size, (unsigned)buf_len);
        return 0;
    }

    /* Derive frequency from channel number */
    uint8_t channel = info->rx_ctrl.channel;
    uint32_t freq_mhz;
    if (channel >= 1 && channel <= 13) {
        freq_mhz = 2412 + (channel - 1) * 5;
    } else if (channel == 14) {
        freq_mhz = 2484;
    } else if (channel >= 36 && channel <= 177) {
        freq_mhz = 5000 + channel * 5;
    } else {
        freq_mhz = 0;
    }

    /* Magic (LE) */
    memcpy(&buf[0], &magic, 4);

    /* Node ID (captured at init into s_node_id to survive memory corruption
     * that could clobber g_nvs_config.node_id - see #232/#375/#385/#390). */
    buf[4] = s_node_id;

    /* Number of antennas */
    buf[5] = n_antennas;

    /* Number of subcarriers (LE u16) */
    memcpy(&buf[6], &n_subcarriers, 2);

    /* Frequency MHz (LE u32) */
    memcpy(&buf[8], &freq_mhz, 4);

    /* Sequence number (LE u32) */
    uint32_t seq = s_sequence++;
    memcpy(&buf[12], &seq, 4);

    /* RSSI (i8) */
    buf[16] = (uint8_t)(int8_t)info->rx_ctrl.rssi;

    /* Noise floor (i8) */
    buf[17] = (uint8_t)(int8_t)info->rx_ctrl.noise_floor;

    /* PPDU type (byte 18) + bandwidth/flags (byte 19).
     * Previously reserved-zero, now optionally populated when CONFIG_CSI_FRAME_HE_TAGGING.
     * Readers that don't know about the extension see zeros — backward compatible.
     *
     * The struct that backs info->rx_ctrl is target-conditional in IDF v5.4
     * (esp_wifi/include/local/esp_wifi_types_native.h):
     *
     *   CONFIG_SOC_WIFI_HE_SUPPORT=y  (C6/C5)  →  esp_wifi_rxctrl_t with cur_bb_format, second
     *   otherwise                     (S3 etc) →  legacy struct with sig_mode, cwb, stbc
     *
     * Byte-18 PPDU type encoding stays the same across targets:
     *   0=HT/legacy bucket, 1=HE-SU, 2=HE-MU, 3=HE-TB, 0xFF=unknown
     */
#ifdef CONFIG_CSI_FRAME_HE_TAGGING
    uint8_t ppdu_type = 0xFF;
    uint8_t flags     = 0;
#if CONFIG_SOC_WIFI_HE_SUPPORT
    /* HE-capable chips: read cur_bb_format (0=11b, 1=11g, 2=HT, 3=VHT, 4=HE-SU,
     * 5=HE-MU, 6=HE-ERSU, 7=HE-TB) and 'second' (40 MHz secondary chan offset). */
    switch (info->rx_ctrl.cur_bb_format) {
        case 0:
        case 1:
        case 2:  ppdu_type = 0; break;  /* 11b/g/a/HT bucket */
        case 3:  ppdu_type = 0; break;  /* VHT — rare on 2.4 GHz, HT bucket */
        case 4:  ppdu_type = 1; break;  /* HE-SU */
        case 5:  ppdu_type = 2; break;  /* HE-MU */
        case 6:  ppdu_type = 1; break;  /* HE-ER-SU collapses to HE-SU */
        case 7:  ppdu_type = 3; break;  /* HE-TB */
        default: ppdu_type = 0xFF; break;
    }
    if (info->rx_ctrl.second != 0) flags |= 0x1;  /* bw 40 MHz */
#else
    /* Pre-HE chips (S3 etc): use legacy sig_mode + cwb + stbc fields. */
    switch (info->rx_ctrl.sig_mode) {
        case 0: ppdu_type = 0; break;  /* non-HT (11b/g) */
        case 1: ppdu_type = 0; break;  /* HT (11n) */
        case 3: ppdu_type = 0; break;  /* VHT — bucket as HT for storage */
        default: ppdu_type = 0xFF; break;
    }
    if (info->rx_ctrl.cwb) flags |= 0x1;            /* bw 40 MHz */
    if (info->rx_ctrl.stbc) flags |= (1 << 2);      /* STBC */
#endif  /* CONFIG_SOC_WIFI_HE_SUPPORT */
    /* ADR-018 byte 19 bit 4 = "cross-node sync valid". Two transports can
     * set it: the original 802.15.4 c6_timesync (broken in IDF v5.4 — D1)
     * and the ESP-NOW workaround c6_sync_espnow (measured working in §A0.7-
     * §A0.10). OR them together so frames signal sync from whichever
     * transport is alive on this node. Host can pair against the sync
     * packet (§A0.12) once it sees this bit. */
#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_C6_TIMESYNC_ENABLE)
    if (c6_timesync_is_valid()) flags |= (1 << 4);  /* 15.4 sync valid */
#endif
    if (c6_sync_espnow_is_valid()) flags |= (1 << 4);  /* ESP-NOW sync valid (D1 workaround) */
    buf[18] = ppdu_type;
    buf[19] = flags;
#else
    buf[18] = 0;
    buf[19] = 0;
#endif

    /* I/Q data */
    memcpy(&buf[header_len], info->buf, iq_len);

    /* Compute and append the SipHash-2-4 tag if using authenticated format */
    if (header_len == CSI_AUTHENTICATED_HEADER_SIZE) {
        uint64_t tag = siphash24_two_parts(buf, 20, info->buf, iq_len, s_siphash_key);
        memcpy(&buf[20], &tag, 8);
    }

    return frame_size;
}

/**
 * WiFi CSI callback — invoked by ESP-IDF when CSI data is available.
 */
static void wifi_csi_callback(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;

    /* Early rate gate: drop excess callbacks to ~50 Hz to prevent
     * SPI flash cache crash in WiFi ISR (wDev_ProcessFiq). */
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_process_us) < CSI_MIN_PROCESS_INTERVAL_US) {
        s_early_drop++;
        return;
    }
    s_last_process_us = now_us;

    /* ADR-060: MAC address filtering — drop frames from non-matching sources.
     * Uses defensively-copied s_filter_mac instead of g_nvs_config (which can
     * be corrupted by wifi_init_sta — same root cause as the node_id clobber). */
    if (s_filter_mac_set) {
        if (memcmp(info->mac, s_filter_mac, 6) != 0) {
            return;  /* Source MAC doesn't match filter — skip frame. */
        }
    }

    s_cb_count++;

    if (s_cb_count <= 3 || (s_cb_count % 100) == 0) {
        ESP_LOGI(TAG, "CSI cb #%lu: len=%d rssi=%d ch=%d",
                 (unsigned long)s_cb_count, info->len,
                 info->rx_ctrl.rssi, info->rx_ctrl.channel);
    }

    uint8_t frame_buf[CSI_MAX_FRAME_SIZE];
    size_t frame_len = csi_serialize_frame(info, frame_buf, sizeof(frame_buf));

    if (frame_len > 0) {
        /* Rate-limit UDP sends to avoid ENOMEM from lwIP pbuf exhaustion.
         * In promiscuous mode, CSI callbacks can fire 100-500+ times/sec.
         * We only need 20-50 Hz for the sensing pipeline. */
        int64_t now = esp_timer_get_time();
        if ((now - s_last_send_us) >= CSI_MIN_SEND_INTERVAL_US) {
            int ret = stream_sender_send(frame_buf, frame_len);
            if (ret > 0) {
                s_send_ok++;
                s_last_send_us = now;
            } else {
                s_send_fail++;
                if (s_send_fail <= 5) {
                    ESP_LOGW(TAG, "sendto failed (fail #%lu)", (unsigned long)s_send_fail);
                }
            }
        } else {
            s_rate_skip++;
        }
    }

    /* ADR-039: Enqueue raw I/Q into edge processing ring buffer. */
    if (info->buf && info->len > 0) {
        edge_enqueue_csi((const uint8_t *)info->buf, (uint16_t)info->len,
                         (int8_t)info->rx_ctrl.rssi, info->rx_ctrl.channel);
    }

    /* ADR-110 §A0.11/§A0.12 — Emit a sync-packet every N CSI frames so the
     * host aggregator can pair node-local sequence numbers with the mesh-aligned
     * epoch coming out of c6_sync_espnow_get_epoch_us(). Backwards-compatible
     * with the ADR-018 frame format: new packet uses a distinct magic so the
     * existing CSI parser can dispatch by first 4 bytes.
     *
     * Cadence is operator-tunable via CONFIG_C6_SYNC_EVERY_N_FRAMES (default 20).
     * At 10 Hz observed CSI rate that's ~2 s between sync packets; raise to 50
     * for ~5 s (less overhead, slower convergence), lower to 5 for ~0.5 s
     * (heavier wire, tighter ADR-029/030 multistatic alignment window). */
    {
#ifndef CONFIG_C6_SYNC_EVERY_N_FRAMES
#define CONFIG_C6_SYNC_EVERY_N_FRAMES 20
#endif
        if ((s_cb_count % CONFIG_C6_SYNC_EVERY_N_FRAMES) == 0) {
            uint8_t sync[32];
            uint32_t sync_magic = 0xC511A110u;    /* CSI-ADR-110 sync packet */
            uint64_t local_us = (uint64_t)esp_timer_get_time();
            uint64_t epoch_us = c6_sync_espnow_get_epoch_us();
            int64_t  off_smooth = c6_sync_espnow_get_offset_us_smoothed();
            uint8_t  flags = 0;
            if (c6_sync_espnow_is_leader()) flags |= 0x01;
            if (c6_sync_espnow_is_valid())  flags |= 0x02;
            if (off_smooth != 0)            flags |= 0x04;

            memcpy(&sync[0],  &sync_magic, 4);
            sync[4] = s_node_id;
            sync[5] = 0x01;                       /* protocol version */
            sync[6] = flags;
            sync[7] = 0;                          /* reserved */
            memcpy(&sync[8],  &local_us, 8);
            memcpy(&sync[16], &epoch_us, 8);
            memcpy(&sync[24], &s_sequence, 4);    /* high-water seq for pairing */
            uint32_t zero32 = 0;
            memcpy(&sync[28], &zero32, 4);        /* reserved (room for leader_id low32) */
            /* Sync packets are 32 B at ~0.5 Hz — priority path so the CSI
             * ENOMEM backoff can't starve cross-node time alignment (#1183). */
            int sr = stream_sender_send_priority(sync, sizeof(sync));
            static uint32_t s_sync_count = 0;
            s_sync_count++;
            if (s_sync_count <= 3 || (s_sync_count % 60) == 0) {
                ESP_LOGI(TAG, "sync-pkt #%lu (sr=%d) node=%u flags=0x%02x "
                              "local_us=%llu epoch_us=%llu seq=%lu",
                         (unsigned long)s_sync_count, sr,
                         (unsigned)s_node_id, (unsigned)flags,
                         (unsigned long long)local_us,
                         (unsigned long long)epoch_us,
                         (unsigned long)s_sequence);
            }
        }
    }
}

/**
 * Promiscuous mode callback — required for CSI to fire on all received frames.
 * We don't need the packet content, just the CSI triggered by reception.
 */
static void wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    /* No-op: CSI callback is registered separately and fires in parallel. */
    (void)buf;
    (void)type;
}

/* ---- EchoPose#521/#954: connected-STA CSI traffic source (additive) ----
 *
 * The ESP32 CSI engine only produces CSI for received OFDM frames (L-LTF/HT-LTF).
 * On a quiet network — or on a display-enabled build where the #893 MGMT->MGMT+DATA
 * promiscuous upgrade is skipped (has_display=true) — the only CSI-eligible frames
 * are sparse beacons (often non-OFDM DSSS), so wifi_csi_callback can starve to
 * yield=0pps -> DEGRADED -> motion/presence=0 (#521, #954).
 *
 * This guarantees a ~50 Hz OFDM unicast floor by pinging the STA's own gateway:
 * the router's ICMP echo replies are OFDM frames destined to this station, which
 * drive the CSI engine regardless of promiscuous filter state or ambient traffic.
 * It is ADDITIVE — promiscuous capture (#396/#893) is left fully intact so
 * multistatic/multi-node sensing still hears other stations' frames. Mirrors
 * Espressif's esp-csi csi_recv_router reference.
 */
static esp_ping_handle_t s_self_ping = NULL;
static void csi_ping_cb_noop(esp_ping_handle_t hdl, void *args) { (void)hdl; (void)args; }

static void csi_start_self_ping(void)
{
    if (s_self_ping != NULL) {
        return;  /* already running */
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta == NULL || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.gw.addr == 0) {
        ESP_LOGW(TAG, "self-ping: no gateway IP yet; CSI relies on ambient frames (#954)");
        return;
    }

    char gw_str[16];
    esp_ip4addr_ntoa(&ip.gw, gw_str, sizeof(gw_str));

    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    ipaddr_aton(gw_str, &target);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr     = target;
    cfg.count           = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms     = 20;     /* 50 Hz -> ~50 received OFDM replies/sec */
    cfg.data_size       = 1;
    cfg.task_stack_size = 4096;

    esp_ping_callbacks_t cbs = {
        .cb_args         = NULL,
        .on_ping_success = csi_ping_cb_noop,
        .on_ping_timeout = csi_ping_cb_noop,
        .on_ping_end     = csi_ping_cb_noop,
    };

    if (esp_ping_new_session(&cfg, &cbs, &s_self_ping) == ESP_OK && s_self_ping != NULL) {
        esp_ping_start(s_self_ping);
        ESP_LOGI(TAG, "self-ping started -> %s @50Hz (CSI OFDM source, fix #521/#954)", gw_str);
    } else {
        ESP_LOGW(TAG, "self-ping: esp_ping_new_session failed");
        s_self_ping = NULL;
    }
}

void csi_collector_set_node_id(uint8_t node_id)
{
    s_node_id = node_id;
    s_node_id_early_set = true;
    ESP_LOGI(TAG, "Early capture node_id=%u (before WiFi init, #232/#390)",
             (unsigned)node_id);

    /* Also capture MAC filter config now — same struct, same corruption risk.
     * The CSI callback reads filter_mac_set on every invocation (100-500 Hz),
     * so a corrupted value could cause erratic filtering or crash. */
    s_filter_mac_set = (g_nvs_config.filter_mac_set != 0);
    if (s_filter_mac_set) {
        memcpy(s_filter_mac, g_nvs_config.filter_mac, 6);
        ESP_LOGI(TAG, "Early capture filter_mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 s_filter_mac[0], s_filter_mac[1], s_filter_mac[2],
                 s_filter_mac[3], s_filter_mac[4], s_filter_mac[5]);
    }
}

/* ---- ADR-032 §2.2 & §2.8: SipHash-2-4 and key derivation ---- */

static uint64_t u8to64_le(const uint8_t *p)
{
    return ((uint64_t)p[0]) |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND \
  do { \
    v0 += v1; v2 += v3; \
    v1 = ROTL(v1, 13); v3 = ROTL(v3, 16); \
    v1 ^= v0; v3 ^= v2; \
    v0 = ROTL(v0, 32); \
    v2 += v1; v0 += v3; \
    v1 = ROTL(v1, 17); v3 = ROTL(v3, 21); \
    v1 ^= v2; v3 ^= v0; \
    v2 = ROTL(v2, 32); \
  } while (0)

uint64_t siphash24(const uint8_t *in, size_t inlen, const uint8_t *key)
{
    uint64_t k0 = u8to64_le(key);
    uint64_t k1 = u8to64_le(key + 8);

    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f6f676e6f7465ULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;

    const uint8_t *end = in + (inlen & ~7);
    const int left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;

    for (const uint8_t *ni = in; ni < end; ni += 8) {
        uint64_t m = u8to64_le(ni);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }

    switch (left) {
        case 7: b |= ((uint64_t)end[6]) << 48; // fallthrough
        case 6: b |= ((uint64_t)end[5]) << 40; // fallthrough
        case 5: b |= ((uint64_t)end[4]) << 32; // fallthrough
        case 4: b |= ((uint64_t)end[3]) << 24; // fallthrough
        case 3: b |= ((uint64_t)end[2]) << 16; // fallthrough
        case 2: b |= ((uint64_t)end[1]) << 8;  // fallthrough
        case 1: b |= ((uint64_t)end[0]);
        case 0: break;
    }

    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

uint64_t siphash24_two_parts(const uint8_t *p1, size_t len1,
                             const uint8_t *p2, size_t len2,
                             const uint8_t *key)
{
    uint64_t k0 = u8to64_le(key);
    uint64_t k1 = u8to64_le(key + 8);

    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f6f676e6f7465ULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;

    size_t total_len = len1 + len2;
    uint64_t b = ((uint64_t)total_len) << 56;

    size_t processed = 0;
    uint64_t m = 0;
    int m_bytes = 0;

    for (size_t i = 0; i < len1; i++) {
        m |= ((uint64_t)p1[i]) << (m_bytes * 8);
        m_bytes++;
        if (m_bytes == 8) {
            v3 ^= m;
            SIPROUND;
            SIPROUND;
            v0 ^= m;
            m = 0;
            m_bytes = 0;
        }
        processed++;
    }
    for (size_t i = 0; i < len2; i++) {
        m |= ((uint64_t)p2[i]) << (m_bytes * 8);
        m_bytes++;
        if (m_bytes == 8) {
            v3 ^= m;
            SIPROUND;
            SIPROUND;
            v0 ^= m;
            m = 0;
            m_bytes = 0;
        }
        processed++;
    }

    uint64_t last_block = 0;
    int remaining = total_len - processed;
    for (int i = 0; i < remaining; i++) {
        size_t idx = processed + i;
        uint8_t val = (idx < len1) ? p1[idx] : p2[idx - len1];
        last_block |= ((uint64_t)val) << (i * 8);
    }

    b |= last_block;
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

/* Manual HMAC-SHA256 using PSA hash functions to avoid key import overhead. */
static esp_err_t hmac_sha256_psa(const uint8_t *key, size_t key_len,
                                 const uint8_t *msg, size_t msg_len,
                                 uint8_t *out_mac)
{
    uint8_t ipad[64];
    uint8_t opad[64];
    memset(ipad, 0x36, 64);
    memset(opad, 0x5c, 64);

    uint8_t key_block[64];
    memset(key_block, 0, 64);
    if (key_len > 64) {
        psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
        size_t out_len = 0;
        if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS ||
            psa_hash_update(&op, key, key_len) != PSA_SUCCESS ||
            psa_hash_finish(&op, key_block, 32, &out_len) != PSA_SUCCESS) {
            return ESP_FAIL;
        }
    } else {
        memcpy(key_block, key, key_len);
    }

    for (int i = 0; i < 64; i++) {
        ipad[i] ^= key_block[i];
        opad[i] ^= key_block[i];
    }

    uint8_t inner_hash[32];
    psa_hash_operation_t op_inner = PSA_HASH_OPERATION_INIT;
    size_t out_len_inner = 0;
    if (psa_hash_setup(&op_inner, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&op_inner, ipad, 64) != PSA_SUCCESS ||
        psa_hash_update(&op_inner, msg, msg_len) != PSA_SUCCESS ||
        psa_hash_finish(&op_inner, inner_hash, 32, &out_len_inner) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    psa_hash_operation_t op_outer = PSA_HASH_OPERATION_INIT;
    size_t out_len_outer = 0;
    if (psa_hash_setup(&op_outer, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&op_outer, opad, 64) != PSA_SUCCESS ||
        psa_hash_update(&op_outer, inner_hash, 32) != PSA_SUCCESS ||
        psa_hash_finish(&op_outer, out_mac, 32, &out_len_outer) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void csi_collector_init_security(void)
{
    // Initialize PSA Crypto library
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize PSA Cryptography: %d", (int)st);
        return;
    }

    if (g_nvs_config.mesh_key_valid) {
        uint8_t hmac_out[32];
        esp_err_t err = hmac_sha256_psa(g_nvs_config.mesh_key, 16,
                                        (const uint8_t *)"csi-frame-siphash", 17,
                                        hmac_out);
        if (err == ESP_OK) {
            memcpy(s_siphash_key, hmac_out, 16);
            s_siphash_key_valid = true;
            ESP_LOGI(TAG, "Mesh security: derived SipHash key successfully");
        } else {
            ESP_LOGE(TAG, "Mesh security: failed to derive SipHash key");
        }
    } else {
        ESP_LOGW(TAG, "Mesh security: no valid mesh key provisioned, running unauthenticated");
    }
}

void csi_collector_init(void)
{
    if (!s_node_id_early_set) {
        /* Fallback: no early capture — use current g_nvs_config (may be clobbered). */
        s_node_id = g_nvs_config.node_id;
        ESP_LOGW(TAG, "Late capture node_id=%u (no early set_node_id call)",
                 (unsigned)s_node_id);
    } else if (g_nvs_config.node_id != s_node_id) {
        /* Canary: early capture disagrees with current g_nvs_config — corruption
         * happened between nvs_config_load() and here (likely wifi_init_sta). */
        ESP_LOGW(TAG, "node_id clobber CONFIRMED: early=%u g_nvs_config=%u "
                 "(WiFi init likely corrupted struct, using early value)",
                 (unsigned)s_node_id, (unsigned)g_nvs_config.node_id);
    } else {
        ESP_LOGI(TAG, "node_id=%u verified (early capture matches g_nvs_config)",
                 (unsigned)s_node_id);
    }

    /* Canary for filter_mac: check if WiFi init corrupted the filter fields. */
    if (s_node_id_early_set) {
        bool mac_set_now = (g_nvs_config.filter_mac_set != 0);
        if (mac_set_now != s_filter_mac_set) {
            ESP_LOGW(TAG, "filter_mac_set clobber CONFIRMED: early=%d g_nvs_config=%d",
                     (int)s_filter_mac_set, (int)mac_set_now);
        } else if (s_filter_mac_set &&
                   memcmp(s_filter_mac, g_nvs_config.filter_mac, 6) != 0) {
            ESP_LOGW(TAG, "filter_mac clobber CONFIRMED: bytes differ after WiFi init");
        }
    } else {
        /* No early capture — grab filter config now (may already be corrupted). */
        s_filter_mac_set = (g_nvs_config.filter_mac_set != 0);
        if (s_filter_mac_set) {
            memcpy(s_filter_mac, g_nvs_config.filter_mac, 6);
        }
    }

    /* ADR-060: Determine the CSI channel.
     * Priority: 1) NVS override (--channel), 2) connected AP channel, 3) Kconfig default. */
    uint8_t csi_channel = (uint8_t)CONFIG_CSI_WIFI_CHANNEL;

    if (g_nvs_config.csi_channel > 0) {
        /* Explicit NVS override via provision.py --channel */
        csi_channel = g_nvs_config.csi_channel;
        ESP_LOGI(TAG, "Using NVS channel override: %u", (unsigned)csi_channel);
    } else {
        /* Auto-detect from connected AP */
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && ap_info.primary > 0) {
            csi_channel = ap_info.primary;
            ESP_LOGI(TAG, "Auto-detected AP channel: %u", (unsigned)csi_channel);
        } else {
            ESP_LOGW(TAG, "Could not detect AP channel, using Kconfig default: %u",
                     (unsigned)csi_channel);
        }
    }

    /* Update the hop table's first channel to match. */
    s_hop_channels[0] = csi_channel;

    /* Disable WiFi modem sleep — reliable CSI capture needs the radio awake.
     * The ESP-IDF STA default is WIFI_PS_MIN_MODEM, which lets the modem
     * sleep between DTIM beacons; with the MGMT-only promiscuous filter
     * (EchoPose#396) that starves the CSI callback and the per-second yield
     * collapses toward 0 pps (EchoPose#521). Operators who want battery
     * duty-cycling opt back in via power_mgmt_init() (provision.py
     * --duty-cycle <N>), which runs after this and re-enables modem sleep. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: %s — CSI yield may be low",
                 esp_err_to_name(ps_err));
    } else {
        ESP_LOGI(TAG, "WiFi modem sleep disabled (WIFI_PS_NONE) for CSI capture");
    }

    /* Enable promiscuous mode — required for reliable CSI callbacks.
     * Without this, CSI only fires on frames destined to this station,
     * which may be very infrequent on a quiet network. */
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb));

    /* MGMT-only promiscuous filter + active probe injection (EchoPose#396).
     *
     * DATA frames cause 100-500+ WiFi HW interrupts/sec which crashes Core 0
     * in wDev_ProcessFiq (SPI flash cache race in ESP-IDF WiFi blob).
     * MGMT-only gives ~10 Hz (beacons). Probe request injection at 10 Hz
     * adds ~10 Hz probe responses from APs → ~20 Hz total, matching the
     * edge processing designed sample rate of 20 Hz. */
    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));

    ESP_LOGI(TAG, "Promiscuous mode enabled (MGMT-only, EchoPose#396)");

#if CONFIG_SOC_WIFI_HE_SUPPORT
    /* Wi-Fi 6 targets (e.g. ESP32-C6): wifi_csi_config_t is wifi_csi_acquire_config_t
     * (bitfields), not the legacy 802.11n bool layout used on ESP32-S3. */
    wifi_csi_config_t csi_config;
    memset(&csi_config, 0, sizeof(csi_config));
    csi_config.enable = 1U;
    csi_config.acquire_csi_legacy = 1U;
    csi_config.acquire_csi_ht20 = 1U;
    csi_config.acquire_csi_ht40 = 1U;
    csi_config.acquire_csi_su = 1U;
    csi_config.acquire_csi_mu = 1U;
    csi_config.acquire_csi_dcm = 1U;
    csi_config.acquire_csi_beamformed = 1U;
#if CONFIG_SOC_WIFI_MAC_VERSION_NUM >= 3
    csi_config.acquire_csi_force_lltf = 1U;
    csi_config.acquire_csi_vht = 1U;
    csi_config.acquire_csi_he_stbc_mode = ESP_CSI_ACQUIRE_STBC_SAMPLE_HELTFS;
    csi_config.val_scale_cfg = 0U;
#else
    csi_config.acquire_csi_he_stbc = ESP_CSI_ACQUIRE_STBC_SAMPLE_HELTFS;
    csi_config.val_scale_cfg = 0U;
#endif
    csi_config.dump_ack_en = 0U;
#else
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = false,
    };
#endif

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_callback, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    if (g_nvs_config.filter_mac_set) {
        ESP_LOGI(TAG, "MAC filter active: %02x:%02x:%02x:%02x:%02x:%02x",
                 g_nvs_config.filter_mac[0], g_nvs_config.filter_mac[1],
                 g_nvs_config.filter_mac[2], g_nvs_config.filter_mac[3],
                 g_nvs_config.filter_mac[4], g_nvs_config.filter_mac[5]);
    }

    ESP_LOGI(TAG, "CSI collection initialized (node_id=%u, channel=%u)",
             (unsigned)s_node_id, (unsigned)csi_channel);

    /* EchoPose#521/#954: start the connected-STA traffic source so the CSI engine
     * receives a guaranteed OFDM unicast floor even when promiscuous capture is
     * starved (display builds / quiet networks). Additive to #396/#893. */
    csi_start_self_ping();
}

/* Accessor for other modules that need the authoritative runtime node_id. */
uint8_t csi_collector_get_node_id(void)
{
    return s_node_id;
}

/* ---- ADR-081: packet yield accessor for the radio abstraction layer ---- */

uint16_t csi_collector_get_pkt_yield_per_sec(void)
{
    /* Simple sliding window: record the callback count at ~1 s ago, return
     * the delta. Called from adaptive_controller's fast loop (200 ms), so
     * we update the snapshot every ~5 calls. */
    static int64_t  s_yield_window_start_us = 0;
    static uint32_t s_yield_window_start_cb = 0;
    static uint16_t s_last_yield            = 0;

    int64_t now = esp_timer_get_time();
    if (s_yield_window_start_us == 0) {
        s_yield_window_start_us = now;
        s_yield_window_start_cb = s_cb_count;
        return 0;
    }
    int64_t elapsed = now - s_yield_window_start_us;
    if (elapsed < 1000000LL) {
        return s_last_yield;
    }
    uint32_t delta = s_cb_count - s_yield_window_start_cb;
    /* Scale back to per-second if the window ran long (shouldn't, but be safe). */
    uint64_t per_sec = ((uint64_t)delta * 1000000ULL) / (uint64_t)elapsed;
    if (per_sec > 0xFFFFu) per_sec = 0xFFFFu;
    s_last_yield            = (uint16_t)per_sec;
    s_yield_window_start_us = now;
    s_yield_window_start_cb = s_cb_count;
    return s_last_yield;
}

uint16_t csi_collector_get_send_fail_count(void)
{
    uint32_t f = s_send_fail;
    return (f > 0xFFFFu) ? 0xFFFFu : (uint16_t)f;
}

/* ---- ADR-029: Channel hopping ---- */

void csi_collector_set_hop_table(const uint8_t *channels, uint8_t hop_count, uint32_t dwell_ms)
{
    if (channels == NULL) {
        ESP_LOGW(TAG, "csi_collector_set_hop_table: channels is NULL");
        return;
    }
    if (hop_count == 0 || hop_count > CSI_HOP_CHANNELS_MAX) {
        ESP_LOGW(TAG, "csi_collector_set_hop_table: invalid hop_count=%u (max=%u)",
                 (unsigned)hop_count, (unsigned)CSI_HOP_CHANNELS_MAX);
        return;
    }
    if (dwell_ms < 10) {
        ESP_LOGW(TAG, "csi_collector_set_hop_table: dwell_ms=%lu too small, clamping to 10",
                 (unsigned long)dwell_ms);
        dwell_ms = 10;
    }

    memcpy(s_hop_channels, channels, hop_count);
    s_hop_count = hop_count;
    s_dwell_ms  = dwell_ms;
    s_hop_index = 0;

    ESP_LOGI(TAG, "Hop table set: %u channels, dwell=%lu ms", (unsigned)hop_count,
             (unsigned long)dwell_ms);
    for (uint8_t i = 0; i < hop_count; i++) {
        ESP_LOGI(TAG, "  hop[%u] = channel %u", (unsigned)i, (unsigned)channels[i]);
    }
}

void csi_hop_next_channel(void)
{
    if (s_hop_count <= 1) {
        /* Single-channel mode: no-op for backward compatibility. */
        return;
    }

    s_hop_index = (s_hop_index + 1) % s_hop_count;
    uint8_t channel = s_hop_channels[s_hop_index];

    /*
     * esp_wifi_set_channel() changes the primary channel.
     * The second parameter is the secondary channel offset for HT40;
     * we use HT20 (no secondary) for sensing.
     */
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Channel hop to %u failed: %s", (unsigned)channel, esp_err_to_name(err));
    } else if ((s_cb_count % 200) == 0) {
        /* Periodic log to confirm hopping is working (not every hop). */
        ESP_LOGI(TAG, "Hopped to channel %u (index %u/%u)",
                 (unsigned)channel, (unsigned)s_hop_index, (unsigned)s_hop_count);
    }
}

/**
 * Timer callback for channel hopping.
 * Called every s_dwell_ms milliseconds from the esp_timer context.
 */
static void hop_timer_cb(void *arg)
{
    (void)arg;
    csi_hop_next_channel();
}

void csi_collector_enable_data_capture(void)
{
    /* MGMT-only (EchoPose#396) starves the CSI callback on display-less boards
     * (EchoPose#521/#893): beacons alone are sparse, yield collapses to 0 pps.
     * Without a display there is no QSPI/SPI-flash cache contention with the
     * DATA-frame interrupt load, so capture DATA frames too. */
    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filt);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CSI filter upgraded to MGMT+DATA (no display, EchoPose#893)");
    } else {
        ESP_LOGW(TAG, "Failed to enable DATA-frame CSI capture: %s", esp_err_to_name(err));
    }
}

void csi_collector_start_hop_timer(void)
{
    if (s_hop_count <= 1) {
        ESP_LOGI(TAG, "Single-channel mode: hop timer not started");
        return;
    }

    if (s_hop_timer != NULL) {
        ESP_LOGW(TAG, "Hop timer already running");
        return;
    }

    esp_timer_create_args_t timer_args = {
        .callback = hop_timer_cb,
        .arg      = NULL,
        .name     = "csi_hop",
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_hop_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create hop timer: %s", esp_err_to_name(err));
        return;
    }

    uint64_t period_us = (uint64_t)s_dwell_ms * 1000;
    err = esp_timer_start_periodic(s_hop_timer, period_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start hop timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_hop_timer);
        s_hop_timer = NULL;
        return;
    }

    ESP_LOGI(TAG, "Hop timer started: period=%lu ms, channels=%u",
             (unsigned long)s_dwell_ms, (unsigned)s_hop_count);
}

/* ---- ADR-032 §2.3: NDP injection rate limiter (token bucket) ---- */

/** Token bucket state for NDP injection rate limiting. */
typedef struct {
    uint32_t tokens;           /**< current token count */
    uint32_t max_tokens;       /**< bucket capacity */
    uint32_t refill_interval_us; /**< microseconds per refilled token */
    int64_t  last_refill_us;   /**< absolute time of last refill (esp_timer) */
} ndp_rate_limiter_t;

/* Defaults — configurable via NVS keys ndp_max_tokens / ndp_refill_hz */
#define NDP_RATE_MAX_TOKENS_DEFAULT   20
#define NDP_RATE_REFILL_HZ_DEFAULT    20

static ndp_rate_limiter_t s_ndp_limiter = {
    .tokens              = NDP_RATE_MAX_TOKENS_DEFAULT,
    .max_tokens          = NDP_RATE_MAX_TOKENS_DEFAULT,
    .refill_interval_us  = 1000000 / NDP_RATE_REFILL_HZ_DEFAULT,
    .last_refill_us      = 0,
};

/** Apply NVS overrides to the NDP rate limiter (called once at boot). */
void csi_collector_configure_ndp_rate_limiter(void)
{
    /* NVS keys: ndp_max_tokens (u8), ndp_refill_hz (u8) */
    if (g_nvs_config.ndp_max_tokens > 0 && g_nvs_config.ndp_max_tokens <= 100) {
        s_ndp_limiter.max_tokens = g_nvs_config.ndp_max_tokens;
        s_ndp_limiter.tokens      = g_nvs_config.ndp_max_tokens;
        ESP_LOGI(TAG, "NDP rate limiter: max_tokens=%u (NVS override)",
                 (unsigned)g_nvs_config.ndp_max_tokens);
    }
    uint8_t refill_hz = NDP_RATE_REFILL_HZ_DEFAULT;
    if (g_nvs_config.ndp_refill_hz > 0 && g_nvs_config.ndp_refill_hz <= 100) {
        refill_hz = g_nvs_config.ndp_refill_hz;
        ESP_LOGI(TAG, "NDP rate limiter: refill_hz=%u (NVS override)",
                 (unsigned)refill_hz);
    }
    s_ndp_limiter.refill_interval_us = 1000000 / refill_hz;
    s_ndp_limiter.last_refill_us = esp_timer_get_time();
}

/** Refill the token bucket based on elapsed time. Must be called before
 *  every token check to enable the burst-and-refill semantics. */
static void ndp_rate_refill(void)
{
    int64_t now = esp_timer_get_time();
    if (s_ndp_limiter.last_refill_us == 0) {
        s_ndp_limiter.last_refill_us = now;
        return;
    }
    int64_t elapsed = now - s_ndp_limiter.last_refill_us;
    if (elapsed < (int64_t)s_ndp_limiter.refill_interval_us) {
        return;  /* not yet due for a refill */
    }
    uint32_t new_tokens = (uint32_t)(elapsed / (int64_t)s_ndp_limiter.refill_interval_us);
    s_ndp_limiter.last_refill_us += (int64_t)new_tokens * (int64_t)s_ndp_limiter.refill_interval_us;
    uint32_t sum = s_ndp_limiter.tokens + new_tokens;
    if (sum > s_ndp_limiter.max_tokens) {
        sum = s_ndp_limiter.max_tokens;
    }
    s_ndp_limiter.tokens = sum;
}

/** Try to consume one token from the bucket. Returns true if allowed. */
static bool ndp_rate_try_consume(void)
{
    ndp_rate_refill();
    if (s_ndp_limiter.tokens > 0) {
        s_ndp_limiter.tokens--;
        return true;
    }
    return false;
}

/* ---- ADR-029/ADR-032 §2.3: NDP frame injection with rate limiting ---- */

esp_err_t csi_inject_ndp_frame(void)
{
    /* Rate limiter gate (ADR-032 §2.3) — reject if bucket is empty. */
    if (!ndp_rate_try_consume()) {
        static uint32_t s_ndp_drop_log = 0;
        s_ndp_drop_log++;
        if ((s_ndp_drop_log % 100) == 1) {
            ESP_LOGW(TAG, "NDP rate-limited: %lu drops so far",
                     (unsigned long)s_ndp_drop_log);
        }
        return ESP_ERR_NOT_ALLOWED;
    }

    /*
     * Construct a minimal 802.11 Null Data Packet frame.
     *
     * A real NDP is preamble-only (~24 us airtime, no payload) and is the
     * sensing-first TX mechanism described in ADR-029.
     *
     * Frame structure (IEEE 802.11 Null Data):
     *   FC (2) | Duration (2) | Addr1 (6) | Addr2 (6) | Addr3 (6) | SeqCtl (2)
     *   = 24 bytes total, no body, no FCS (hardware appends FCS).
     */
    uint8_t ndp_frame[24];
    memset(ndp_frame, 0, sizeof(ndp_frame));

    /* Frame Control: Type=Data (0x02), Subtype=Null (0x04) -> 0x0048 */
    ndp_frame[0] = 0x48;
    ndp_frame[1] = 0x00;

    /* Addr1 (destination): broadcast */
    memset(&ndp_frame[4], 0xFF, 6);

    /* Addr3 (BSSID): broadcast */
    memset(&ndp_frame[16], 0xFF, 6);

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, ndp_frame, sizeof(ndp_frame), false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NDP inject failed: %s", esp_err_to_name(err));
    }

    return err;
}
