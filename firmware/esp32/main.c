// RetroPicto ESP32 adapter: radio I/O, capture, and PictoChat room tasks.
//
// platformio.ini selects one of five SNIFFER_MODE values:
//   MODE_DISCOVERY    Channel-hopping Nintendo-frame discovery over serial.
//   MODE_STREAM       Fixed-channel radiotap/PCAP capture over SoftAP UDP.
//   MODE_JOIN         Experimental active client for a real DS room host.
//   MODE_HOST         PictoChat room host, echo bot, or online relay adapter.
//   MODE_SERIAL_MGMT  Fixed-channel USB-only handshake and packet diagnostics.
//
// Compile-time defaults live in firmware_config.h; capture byte helpers live in
// capture_packet.h. Radio state, callbacks, and task ordering stay together here.
// One radio serves one channel: STREAM capture and its SoftAP share that channel.
// See docs/FIRMWARE.md for the source map and hardware-validation workflow.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "pictochat/handshake_filter.h"
#include "pictochat/host_sequence.h"
#include "pictochat/host_identity.h"
#include "pictochat/host_profile.h"
#include "pictochat/session.h"
#include "pictochat/room.h"
#include "online.h"
#include "pictochat/frame.h"
#include "pictochat/host_admission.h"
#include "pictochat/host_poll_fields.h"
#include "pictochat/mp_reply.h"
#include "pictochat/mp_trace.h"
#include "pictochat/ack_gate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us (busy-wait the MP reply slot)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "pictochat/nintendo.h"

#include "firmware_config.h"
#include "capture_packet.h"

// ---- MODE_JOIN: join a real DS host as an ACTIVE 802.11 client ----
// Corrected model (see PROTOCOL.md): association is standard and CLIENT-initiated.
// The client sends Auth-Req(seq1) then Assoc-Req; it does NOT wait for the host.
// The Assoc-Req SSID is derived live from the host beacon's 0xDD vendor IE
// (game_id ‖ stream_code ‖ zeros), so we must latch onto a beacon first.
// We take the joiner's MAC as our STA MAC (run the real joiner DS OFF).
// Our OWN unique client MAC (Nintendo OUI 00:09:BF so the host treats us as a DS
// client, but distinct from any real console). Impersonating a live console's MAC
// collides on-air — diagnosed 2026-08-08: the real DS shares 64:b5:c6:9c:60:a0.
static const uint8_t JOIN_SELF_MAC[6] = {0x00,0x09,0xBF,0xC6,0xC6,0xC6}; // us (client)
static const uint8_t JOIN_HOST_MAC[6] = {0x00,0x22,0xD7,0x39,0xBC,0xA3}; // room host
// Hardcoded MP multicast MACs (silicon-fixed on the DS).
static const uint8_t MP_CMD_MCAST[6]   = {0x03,0x09,0xBF,0x00,0x00,0x00}; // host CMD
static const uint8_t MP_REPLY_MCAST[6] = {0x03,0x09,0xBF,0x00,0x00,0x10}; // client REPLY
static const uint8_t MP_ACK_MCAST[6]   = {0x03,0x09,0xBF,0x00,0x00,0x03}; // host CMD-ACK

// ---- MODE_HOST: we ARE the PictoChat room host; a real DS joins US ----
// Our BSSID / host MAC (Nintendo OUI so the DS accepts us as a console host).
#if PICTOCHAT_ONLINE
static DRAM_ATTR uint8_t HOST_SELF_MAC[6] = {0x00,0x09,0xbf,0xc6,0xc6,0xd0};
#else
static DRAM_ATTR const uint8_t HOST_SELF_MAC[6] = {0x00,0x09,0xBF,0xC6,0xC6,0xC6};
#endif
#if PICTOCHAT_GHOST_DEMO
static const uint8_t GHOST_MAC[6] = {0x00,0x09,0xbf,0xc6,0xc6,0xc7};
#endif

static const uint_least16_t host_profile_bio[] = u"Hi from PICTOBOT!";
_Static_assert(sizeof(host_profile_bio) / sizeof(host_profile_bio[0]) - 1 <=
               HOST_PROFILE_BIO_UNITS, "Bio exceeds 26 UTF-16 code units");

static const char *TAG = "pictochat";

#if SNIFFER_MODE == MODE_HOST
static void host_log_heap(const char *stage) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "HEAP stage=%s internal_free=%u internal_min=%u internal_largest=%u",
             stage, (unsigned)info.total_free_bytes,
             (unsigned)info.minimum_free_bytes, (unsigned)info.largest_free_block);
}
#endif

// ---- Capture queue: keep the WiFi RX callback cheap, do sends in a task ----
#define CAP_MAX_BYTES  600         // radiotap + frame; DS frames are small
#if SNIFFER_MODE == MODE_SERIAL_MGMT
#define CAP_QUEUE_LEN  80          // bounded 64-frame MP burst + handshake headroom
#else
#define CAP_QUEUE_LEN  24
#endif

typedef struct {
    uint16_t len;                  // bytes used in `data`
    uint8_t  data[CAP_MAX_BYTES];  // full frame (radiotap + 802.11), no pcap hdr
    int8_t   rssi;
    uint8_t  channel;
#if SNIFFER_MODE == MODE_SERIAL_MGMT
    uint32_t rx_us;                // radio RX timestamp, not delayed USB log time
    uint8_t rx_rate;               // raw driver PHY-rate code; log with sig_mode
    uint8_t sig_mode;
#endif
} cap_item_t;

static QueueHandle_t s_cap_queue;
#if SNIFFER_MODE == MODE_SERIAL_MGMT
// Diagnostic target only: never changes the C6 host's own identity.
#if defined(SERIAL_TRACE_JORDAN) && SERIAL_TRACE_JORDAN
static const uint8_t SERIAL_TRACE_HOST[6] = {0x00,0x22,0xd7,0x39,0xbc,0xa3};
#else
#define SERIAL_TRACE_HOST HOST_SELF_MAC
#endif
static volatile uint32_t s_capture_drops = 0;
static mp_trace_t s_mp_trace;
static volatile uint32_t s_usb_empty_replies = 0, s_usb_data_replies = 0;
static volatile uint32_t s_usb_reply_bytes = 0, s_usb_host_cmds = 0;
#endif

#if SNIFFER_MODE == MODE_JOIN || SNIFFER_MODE == MODE_HOST
// esp_wifi_80211_tx() rejects auth/assoc management subtypes by default. This
// strong override neuters the raw-frame sanity check so the driver accepts them.
// Requires linking with -Wl,-zmuldefs (set in platformio.ini for the C6 env).
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}
#endif

#if SNIFFER_MODE == MODE_JOIN
// ---- Join state, latched from the host's beacon 0xDD vendor IE ----
typedef enum { JS_SCAN, JS_AUTH_SENT, JS_ASSOC_SENT, JS_JOINED } join_state_t;
static volatile join_state_t s_js = JS_SCAN;
static volatile bool     s_have_beacon = false;
static volatile uint8_t  s_game_id[4]  = {0};   // IE data 0x0C..0x0F
static volatile uint16_t s_stream_code = 0;      // IE data 0x10..0x11 (LE)
static volatile bool     s_got_authresp = false; // host Auth-Resp seq2, status 0
static volatile int      s_assoc_status = -1;    // host Assoc-Resp status (-1 = none)
static volatile uint16_t s_aid_bit = 0;          // our poll bit in the CMD client_bitmask
static volatile uint32_t s_polls = 0, s_replies = 0; // data-phase counters

// The ConsoleId-announce token: a client-minted RANDOM 32-bit transaction handle,
// fresh per announce (decoded from perfect01.pcap — the real DS uses a distinct
// random value every announce; the host merely echoes it in its tid=1 rebroadcast).
// We mint one per join_send_ctrl52() call; replaying a fixed value made the host
// dedupe our transfer as a replay. Seeded with the perfect01 value for the first
// send only; esp_random() overwrites it each call.
static volatile uint32_t s_tok = 0xd7a0885e;   // 5e 88 a0 d7 as LE u32

// The host's live "key" (perfect01: e4 09) that the real DS echoes in its 30B steady
// reply payload `00 00 <key>`. We captured it hardcoded as 0c 00; the real reply
// mirrors the host's current value, seen trailing every host CMD-ACK as `<key> fd 3f`.
static volatile uint8_t s_hostkey[2] = {0xe4, 0x09};

// DIAG (scan visibility): counters for what's actually on air while we're stuck in
// JS_SCAN — distinguishes "host isn't beaconing" from "beacon seen but IE not parsed".
static volatile uint32_t s_bcn_host = 0, s_bcn_other = 0, s_cmd_seen = 0;

static inline void join_tx(const uint8_t *buf, size_t len) {
    esp_wifi_80211_tx(WIFI_IF_STA, buf, len, true /* en_sys_seq */);
}

// Parse a host beacon's Nintendo 0xDD vendor IE to latch game_id + stream_code.
static void join_parse_beacon(const uint8_t *f, int len) {
    int pos = 24 + 12;  // MAC header + (timestamp 8 + interval 2 + capab 2)
    while (pos + 2 <= len) {
        uint8_t tag = f[pos], tlen = f[pos + 1];
        if (pos + 2 + tlen > len) break;
        const uint8_t *d = f + pos + 2;             // IE data
        if (tag == 0xDD && tlen >= 0x12 &&
            d[0] == 0x00 && d[1] == 0x09 && d[2] == 0xBF) {  // Nintendo OUI
            for (int i = 0; i < 4; i++) s_game_id[i] = d[0x0C + i];
            s_stream_code = d[0x10] | (d[0x11] << 8);
            s_have_beacon = true;
            return;
        }
        pos += 2 + tlen;
    }
}

// Build + send an open-system Auth-Req (seq 1) to the host.
static void join_send_auth(void) {
    uint8_t fr[30];
    fr[0] = 0xB0; fr[1] = 0x00;            // FC: mgmt / auth
    fr[2] = 0xA2; fr[3] = 0x00;            // duration
    memcpy(fr + 4,  JOIN_HOST_MAC, 6);     // addr1 RA  = host
    memcpy(fr + 10, JOIN_SELF_MAC, 6);     // addr2 TA  = us
    memcpy(fr + 16, JOIN_HOST_MAC, 6);     // addr3 BSSID = host
    fr[22] = 0; fr[23] = 0;                // seq (HW fills)
    fr[24] = 0x00; fr[25] = 0x00;          // auth algorithm = Open System
    fr[26] = 0x01; fr[27] = 0x00;          // auth transaction seq = 1
    fr[28] = 0x00; fr[29] = 0x00;          // status = 0
    join_tx(fr, sizeof(fr));
}

// Build + send an Assoc-Req whose SSID is derived from the latched beacon IE.
static void join_send_assoc(void) {
    uint8_t fr[64];
    int n = 0;
    fr[n++] = 0x00; fr[n++] = 0x00;        // FC: mgmt / assoc-req
    fr[n++] = 0xA2; fr[n++] = 0x00;        // duration
    memcpy(fr + n, JOIN_HOST_MAC, 6); n += 6;   // addr1 RA  = host
    memcpy(fr + n, JOIN_SELF_MAC, 6); n += 6;   // addr2 TA  = us
    memcpy(fr + n, JOIN_HOST_MAC, 6); n += 6;   // addr3 BSSID = host
    fr[n++] = 0; fr[n++] = 0;              // seq (HW fills)
    fr[n++] = 0x21; fr[n++] = 0x00;        // capability = ESS | Short Preamble
    fr[n++] = 0x01; fr[n++] = 0x00;        // listen interval = 1
    // SSID IE: game_id(4) ‖ stream_code(2 LE) ‖ 26 zeros  (32 bytes total)
    fr[n++] = 0x00; fr[n++] = 0x20;
    for (int i = 0; i < 4; i++) fr[n++] = s_game_id[i];
    fr[n++] = s_stream_code & 0xFF; fr[n++] = (s_stream_code >> 8) & 0xFF;
    for (int i = 0; i < 26; i++) fr[n++] = 0x00;
    // Supported Rates IE: 1M and 2M, both basic
    fr[n++] = 0x01; fr[n++] = 0x02; fr[n++] = 0x82; fr[n++] = 0x84;
    join_tx(fr, n);
}

// Send the steady-state REPLY to the host's CMD poll, in our slot. This is the
// byte-exact 30-byte frame a REAL DS client sends (perfect01.pcap: 155k of them),
// NOT an empty CF-Ack — a small fast frame that fits even the narrow 214us slot.
static void join_send_reply(void) {
    uint8_t fr[30];
    fr[0] = 0x18; fr[1] = 0x11;            // FC: Data+CF-Ack, ToDS|PwrMgmt (real DS)
    fr[2] = 0xfe; fr[3] = 0x03;            // duration 0x03fe (captured)
    memcpy(fr + 4,  JOIN_HOST_MAC,  6);    // addr1 = host BSSID
    memcpy(fr + 10, JOIN_SELF_MAC,  6);    // addr2 = us (client)
    memcpy(fr + 16, MP_REPLY_MCAST, 6);    // addr3 = REPLY multicast
    fr[22] = 0; fr[23] = 0;                // seq (HW fills)
    fr[24] = 0x00; fr[25] = 0x80;          // wmHeader (port0 | VSync)
    fr[26] = 0x00; fr[27] = 0x00;          // payload `00 00 <host key>` — the real DS
    fr[28] = s_hostkey[0]; fr[29] = s_hostkey[1];  // echoes the host's live key (e4 09)
    join_tx(fr, sizeof(fr));
    s_replies++;
}

// Stage B: the 136-byte ConsoleId header/data frame (type 6, size 0x68=104). Byte-exact
// from perfect01.pcap [idx 471294] — the FIRST identity frame the real DS sends after
// assoc, carrying its 84-byte ConsoleId body (all zeros on that console). Its own
// random transfer token sits after the `06 00 68 00` descriptor.
static void join_send_hdr136(void) {
    uint8_t fr[132];
    int n = 0;
    fr[n++] = 0x18; fr[n++] = 0x11;        // FC (captured)
    fr[n++] = 0x56; fr[n++] = 0x02;        // duration (captured)
    memcpy(fr + n, JOIN_HOST_MAC,  6); n += 6;
    memcpy(fr + n, JOIN_SELF_MAC,  6); n += 6;
    memcpy(fr + n, MP_REPLY_MCAST, 6); n += 6;
    fr[n++] = 0; fr[n++] = 0;              // seq (HW fills)
    fr[n++] = 0x34; fr[n++] = 0x8c;        // wmHeader (len_half 0x34=104, port12)
    fr[n++] = 0x06; fr[n++] = 0x00; fr[n++] = 0x68; fr[n++] = 0x00;  // type6, size 104
    uint32_t tok = esp_random();           // ConsoleId transfer token (random)
    fr[n++] = tok & 0xff;         fr[n++] = (tok >> 8)  & 0xff;
    fr[n++] = (tok >> 16) & 0xff; fr[n++] = (tok >> 24) & 0xff;
    while (n < 130) fr[n++] = 0x00;        // 96-byte ConsoleId body (zeros)
    fr[n++] = 0x02; fr[n++] = 0x00;        // trailer (captured 02 00)
    join_tx(fr, n);                        // n == 132; HW appends FCS -> 136 on air
    s_replies++;
}

// Stage B: the 52-byte transfer-progress CONTROL frame (port13). Byte-exact from
// perfect01.pcap [idx 471340] — the real DS sends this between the stage-0 data and
// the stage-1 announce, and again after stage-1, with an incrementing counter
// (0x0009 then 0x000b). The 9d0233 02 / 6f0233 02 fields are fixed in the capture.
static void join_send_ctrl_progress(uint16_t ctr) {
    uint8_t fr[48];
    int n = 0;
    fr[n++] = 0x18; fr[n++] = 0x11;        // FC (captured)
    fr[n++] = 0xa6; fr[n++] = 0x03;        // duration (captured)
    memcpy(fr + n, JOIN_HOST_MAC,  6); n += 6;
    memcpy(fr + n, JOIN_SELF_MAC,  6); n += 6;
    memcpy(fr + n, MP_REPLY_MCAST, 6); n += 6;
    fr[n++] = 0; fr[n++] = 0;              // seq (HW fills)
    fr[n++] = 0x0a; fr[n++] = 0x8d;        // wmHeader (port13)
    static const uint8_t body[20] = {
        0x03,0x00,0x14,0x00,0x01,0x04,0xff,0xff,
        0x9d,0x02,0x33,0x02,0x01,0x00,0x00,0x00,0x6f,0x02,0x33,0x02};
    memcpy(fr + n, body, sizeof(body)); n += sizeof(body);
    fr[n++] = ctr & 0xff; fr[n++] = (ctr >> 8) & 0xff;   // progress counter
    join_tx(fr, n);                        // n == 48; HW appends FCS -> 52 on air
    s_replies++;
}

// How many profile-card frames we've pushed since the last (re)join. We spam the
// identity for the first N reply slots, then fall back to empty keepalive REPLYs.
static volatile int s_profile_sends = 0;

// Stage B: send our identity/profile card as a DATA REPLY in our poll slot.
// Byte-exact reconstruction of a REAL client profile reply captured in
// perfect01.pcap (frame #471329, the "Ash" console). Layout (no Nintendo CRC —
// only the 802.11 FCS, per ref §3.4 — so MAC/name may be swapped freely):
//   24B MAC | wmHeader(30 8e = len_half 96B, port14+VSync)
//   payload[96] = 02 00 | 60 00 | 01 00 | 54 01 00 00 | 00 00 | 03 00
//                 | <our MAC halfword-swapped> | <name UTF-16LE> | pad | 0f 00 04 14
//   | seqNo footer (04 00)   [port>=8 carries a 2-byte seq footer]
// The real DS sends the identity as TWO 128B frames — stage 0 (tag 03 00) and
// stage 1 (tag 03 01), identical but for that tag byte and the seqNo footer
// (04/05). `stage` selects which.
static void join_send_profile(int stage) {
    uint8_t fr[124];
    int n = 0;
    fr[n++] = 0x18; fr[n++] = 0x11;        // FC: Data+CF-Ack, ToDS|PwrMgmt (captured)
    fr[n++] = 0x76; fr[n++] = 0x02;        // duration (captured)
    memcpy(fr + n, JOIN_HOST_MAC,  6); n += 6;  // addr1 = host BSSID
    memcpy(fr + n, JOIN_SELF_MAC,  6); n += 6;  // addr2 = us (client)
    memcpy(fr + n, MP_REPLY_MCAST, 6); n += 6;  // addr3 = REPLY multicast
    fr[n++] = 0; fr[n++] = 0;              // seq (HW fills)
    fr[n++] = 0x30; fr[n++] = 0x8e;        // wmHeader: len_half=48 (96B), port14|VSync
    int app0 = n;                          // 96-byte payload starts here
    fr[n++] = 0x02; fr[n++] = 0x00;        // payload[0..1] (captured)
    fr[n++] = 0x60; fr[n++] = 0x00;        // payload len = 96
    fr[n++] = 0x01; fr[n++] = 0x00;        // type = 1 (identity), flags = 0
    fr[n++] = 0x54; fr[n++] = 0x01;        // total transfer size = 0x0154 = 340
    fr[n++] = 0x00; fr[n++] = 0x00;
    fr[n++] = 0x00; fr[n++] = 0x00;
    fr[n++] = 0x03; fr[n++] = (uint8_t)stage;  // identity magic, stage (0 or 1)
    fr[n++] = 0x09; fr[n++] = 0x00;        // our MAC, byte-pairs swapped:
    fr[n++] = 0xC6; fr[n++] = 0xBF;        //   00:09:BF:C6:C6:C6 -> 09 00 C6 BF C6 C6
    fr[n++] = 0xC6; fr[n++] = 0xC6;
    static const char *nm = "name";        // name, UTF-16LE
    for (const char *p = nm; *p; p++) { fr[n++] = (uint8_t)*p; fr[n++] = 0x00; }
    while (n < app0 + 92) fr[n++] = 0x00;  // zero-pad to payload offset 92
    fr[n++] = 0x0f; fr[n++] = 0x00;        // payload[92..95] trailer (captured)
    fr[n++] = 0x04; fr[n++] = 0x14;
    fr[n++] = (uint8_t)(0x04 + stage); fr[n++] = 0x00;  // seqNo footer (04/05)
    join_tx(fr, n);                        // n == 124; HW appends FCS
    s_replies++;
}

// Stage B: the 52-byte ConsoleId ANNOUNCE control frame (port 13). Byte-exact from
// perfect01.pcap (the real DS's "54 00 00 00 29 00 69 00" = announce an 84-byte,
// type-0x69 ConsoleId transfer). Per the reference, render (appearing on screen) is
// gated on the host seeing this announce then the ConsoleId completing. The 4
// trailing bytes are a transaction token minted fresh (esp_random) on every call —
// perfect01 shows the real DS never reuses one; the host echoes it in its tid=1.
static void join_send_ctrl52(void) {
    uint8_t fr[48];
    int n = 0;
    fr[n++] = 0x18; fr[n++] = 0x11;        // FC (captured)
    fr[n++] = 0xa6; fr[n++] = 0x03;        // duration (captured)
    memcpy(fr + n, JOIN_HOST_MAC,  6); n += 6;
    memcpy(fr + n, JOIN_SELF_MAC,  6); n += 6;
    memcpy(fr + n, MP_REPLY_MCAST, 6); n += 6;
    fr[n++] = 0; fr[n++] = 0;              // seq (HW fills)
    fr[n++] = 0x0a; fr[n++] = 0x8d;        // wmHeader: len_half=10 (20B), port13|VSync
    fr[n++] = 0x00; fr[n++] = 0x00;        // 20-byte payload (captured announce):
    fr[n++] = 0x14; fr[n++] = 0x00;
    fr[n++] = 0x01; fr[n++] = 0x00;
    fr[n++] = 0xff; fr[n++] = 0xff;
    fr[n++] = 0x54; fr[n++] = 0x00; fr[n++] = 0x00; fr[n++] = 0x00;  // size 84
    fr[n++] = 0x29; fr[n++] = 0x00; fr[n++] = 0x69; fr[n++] = 0x00;  // type 0x69
    // token: a fresh random 32-bit transaction handle, minted per announce (the real
    // DS never repeats one; a reused token reads to the host as a replayed transfer).
    s_tok = esp_random();
    fr[n++] = s_tok & 0xff;         fr[n++] = (s_tok >> 8)  & 0xff;
    fr[n++] = (s_tok >> 16) & 0xff; fr[n++] = (s_tok >> 24) & 0xff;
    fr[n++] = 0x08; fr[n++] = 0x00;        // seqNo footer (port>=8)
    join_tx(fr, n);                        // n == 48; HW appends FCS
    s_replies++;
}

// Deauth (reason 3) to the host — real DS clients send this on leave. We use it to
// clear a stale association ghost (e.g. after reflashing while joined) so the host
// will accept a fresh auth/assoc instead of rejecting the already-known MAC.
static void join_send_deauth(void) {
    uint8_t fr[26];
    fr[0] = 0xC0; fr[1] = 0x00;            // FC: mgmt / deauth
    fr[2] = 0x00; fr[3] = 0x00;            // duration
    memcpy(fr + 4,  JOIN_HOST_MAC, 6);     // addr1 RA  = host
    memcpy(fr + 10, JOIN_SELF_MAC, 6);     // addr2 TA  = us
    memcpy(fr + 16, JOIN_HOST_MAC, 6);     // addr3 BSSID = host
    fr[22] = 0; fr[23] = 0;                // seq (HW fills)
    fr[24] = 0x03; fr[25] = 0x00;          // reason 3
    join_tx(fr, sizeof(fr));
}
#endif

#if SNIFFER_MODE == MODE_HOST
// ============================ MODE_HOST ============================
// Diagnostic only: --wrap redirects the undefined lmac.o reference to this
// active PPDU setup function. Sample header Duration without modifying it.
// Do not log from the Wi-Fi TX path; host_task reports bounded totals instead.
static atomic_uint s_ppdu_calls;
static atomic_uint s_ppdu_cmds;
static atomic_uint s_ppdu_before_zero;
static atomic_uint s_ppdu_after_zero;
static atomic_uint s_ppdu_changed;
static atomic_uint s_edca_calls;
static atomic_uint s_edca_cmds;
static atomic_uint s_edca_zero;

#if CONFIG_IDF_TARGET_ESP32C6
static const uint8_t *IRAM_ATTR host_probe_frame(void *tx_slot) {
    // Verified against linked lmacSetTxFrame -> hal_mac_tx_set_ppdu and
    // PPDU prologue: *(tx_slot) is ebuf; ebuf+4 -> header owner; owner+4 -> frame.
    // All are driver-owned live objects for the duration of this synchronous call.
    const uint8_t *frame = NULL;
    void *ebuf = tx_slot ? *(void **)tx_slot : NULL;
    void *owner = ebuf ? *(void **)((uint8_t *)ebuf + 4) : NULL;
    if (owner) {
        frame = *(const uint8_t **)((uint8_t *)owner + 4);
        if (frame && (*(const uint16_t *)((uint8_t *)ebuf + 36) & 0x2000)) frame += 8;
    }
    return frame;
}
#endif // C6 private TX-slot layout.

static bool IRAM_ATTR host_probe_cmd(const uint8_t *frame) {
    // The configured host MAC lives in DRAM so this hook stays safe while flash is unavailable.
    return frame && frame[0] == 0x28 && frame[1] == 0x02 &&
               frame[4] == 0x03 && frame[5] == 0x09 && frame[6] == 0xbf &&
               frame[7] == 0x00 && frame[8] == 0x00 && frame[9] == 0x00 &&
               frame[10] == HOST_SELF_MAC[0] && frame[11] == HOST_SELF_MAC[1] &&
               frame[12] == HOST_SELF_MAC[2] && frame[13] == HOST_SELF_MAC[3] &&
               frame[14] == HOST_SELF_MAC[4] && frame[15] == HOST_SELF_MAC[5];
}

#if CONFIG_IDF_TARGET_ESP32C6
extern int __real_hal_mac_tx_set_ppdu(void *tx_slot, void *txrx);
int IRAM_ATTR __wrap_hal_mac_tx_set_ppdu(void *tx_slot, void *txrx) {
    const uint8_t *frame = host_probe_frame(tx_slot);
    atomic_fetch_add_explicit(&s_ppdu_calls, 1, memory_order_relaxed);
    bool cmd = host_probe_cmd(frame);
    uint16_t before = cmd ? (uint16_t)(frame[2] | (frame[3] << 8)) : 0;
    if (cmd) {
        atomic_fetch_add_explicit(&s_ppdu_cmds, 1, memory_order_relaxed);
        if (!before) atomic_fetch_add_explicit(&s_ppdu_before_zero, 1, memory_order_relaxed);
    }
    int result = __real_hal_mac_tx_set_ppdu(tx_slot, txrx);
    if (cmd) {
        uint16_t after = (uint16_t)(frame[2] | (frame[3] << 8));
        if (!after) atomic_fetch_add_explicit(&s_ppdu_after_zero, 1, memory_order_relaxed);
        if (before != after) atomic_fetch_add_explicit(&s_ppdu_changed, 1, memory_order_relaxed);
    }
    return result;
}

// lmacTxFrame calls EDCA setup after PPDU setup and before hal_mac_txq_enable.
// Re-resolve the current header after EDCA; never retain driver pointers across calls.
extern int __real_hal_mac_tx_config_edca(void *tx_slot);
int IRAM_ATTR __wrap_hal_mac_tx_config_edca(void *tx_slot) {
    int result = __real_hal_mac_tx_config_edca(tx_slot);
    const uint8_t *frame = host_probe_frame(tx_slot);
    atomic_fetch_add_explicit(&s_edca_calls, 1, memory_order_relaxed);
    if (host_probe_cmd(frame)) {
        atomic_fetch_add_explicit(&s_edca_cmds, 1, memory_order_relaxed);
        if (!frame[2] && !frame[3])
            atomic_fetch_add_explicit(&s_edca_zero, 1, memory_order_relaxed);
    }
    return result;
}
#endif // C6-only private driver diagnostics; never dereference these layouts on Xtensa.
// We host a PictoChat room: beacon the 0xDD IE so the room appears in a real DS's
// room list, accept its Auth/Assoc, then run the CMD/REPLY TDMA poll loop with a
// mandatory CMD-ACK and a type-5 member heartbeat so the DS stays joined. All frame
// layouts are byte-derived from perfect01.pcap (scratchpad host_frames.py /
// beacon_offsets.py). Getting the DS to select+enter our room and stay is the first
// milestone; rendering a bot member + enabling Send is a further layer.
typedef struct {
    bool connected, admitted;
    uint8_t mac[6], aid;
    uint32_t generation;
} host_station_t;
static host_station_t s_stations[PICTOCHAT_ROOM_CLIENTS]; // admission lock
static uint32_t s_generation;
static unsigned s_cycle_slot;
static uint32_t s_cycle_generation;
static bool s_cycle_open;
static int64_t s_cycle_started_us; // admission lock; software submission time
static atomic_uint s_reply_closed, s_reply_other, s_reply_invalid;
static atomic_int s_reply_rssi, s_reply_noise, s_reply_delay_us;
static volatile bool     s_host_beaconing = false; // arm the beacon SSID-delete surgery
static volatile uint32_t s_rx_replies = 0;         // client REPLYs we've seen
static volatile uint32_t s_cmds_tx = 0;
static portMUX_TYPE s_admission_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    uint32_t generation;
    host_id_packet_t app;
} host_app_rx_t;
static QueueHandle_t s_host_app_queue[PICTOCHAT_ROOM_CLIENTS];
static atomic_uint s_host_app_drops;
#if CONFIG_IDF_TARGET_ESP32
// Original ESP32 has a tighter static DRAM window than its total internal heap.
// Reserve the room before starting Wi-Fi/BLE, outside the radio callback path.
static pictochat_room_t *s_room;
#else
static pictochat_room_t s_room_storage;
static pictochat_room_t *const s_room = &s_room_storage;
#endif
typedef struct {
    uint32_t id;
    pictochat_delivery_t delivery;
    uint16_t len;
    uint8_t announcement[20];
    uint8_t body[];
} host_drawing_t;
static QueueHandle_t s_drawing_ready, s_drawing_dump;
static uint32_t s_drawings_received, s_drawings_sent, s_drawing_drops;

// Log complete received drawings outside the radio/host task. The log carries
// offsets and a checksum so a host-side tool can reject missing serial lines.
static void host_drawing_dump_task(void *arg) {
    host_drawing_t *drawing;
    char hex[129];
    static const char digits[] = "0123456789abcdef";
    for (;;) {
        if (xQueueReceive(s_drawing_dump, &drawing, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI(TAG, "DRAW BEGIN id=%lu len=%u hash=%08lx", (unsigned long)drawing->id,
                 drawing->len, (unsigned long)host_message_hash(drawing->body, drawing->len));
        for (unsigned offset = 0; offset < drawing->len; offset += 64) {
            unsigned n = drawing->len - offset;
            if (n > 64) n = 64;
            for (unsigned i = 0; i < n; ++i) {
                uint8_t b = drawing->body[offset + i];
                hex[2*i] = digits[b >> 4]; hex[2*i+1] = digits[b & 15];
            }
            hex[2*n] = 0;
            ESP_LOGI(TAG, "DRAW DATA id=%lu offset=%u hex=%s",
                     (unsigned long)drawing->id, offset, hex);
        }
        ESP_LOGI(TAG, "DRAW END id=%lu", (unsigned long)drawing->id);
        free(drawing);
    }
}
static volatile uint16_t s_cmd_seq = 0x2b00;       // CMD trailer counter (real: 16b, +~1/frame)
static host_poll_fields_t s_poll_fields[PICTOCHAT_ROOM_CLIENTS];
static volatile uint32_t s_cmd_magic = 0;          // member magic (real holds ~3 frames)
static ack_gate_t        s_ack_gate;              // one ACK owner per CMD cycle
static volatile uint32_t s_reply_acks = 0;         // DIAG: reply-timed ACKs sent
// The MP cycle is CMD -> client reply slot -> ACK (client_time=998us).
// Start the fallback delay after the driver's CMD completion callback, not TX
// submission: software traces showed the old fallback submitting ACK before that
// callback. These event times are not calibrated RF timestamps. The RX path can
// claim the cycle's ACK first when a reply arrives.
#define HOST_ACK_DELAY_US  1300     // client_time 998us + SIFS/preamble slack (ref value)
#define HOST_CMD_INTERVAL_MS 13     // ref-proven cadence (~77/s); 4ms exhausts TX buffers
#define HOST_BEACON_US     102400   // 100 TU — regular beacon, decoupled from CMD cadence

// Software event trace, not RF timestamps. It measures when the driver reports
// completion relative to ACK submission; no printing occurs in Wi-Fi callbacks.
typedef struct {
    int64_t us;
    uint16_t len, token;
    uint8_t event, fc, rate, status;
} host_tx_trace_t;
static QueueHandle_t s_host_trace_queue;
static atomic_uint s_host_trace_left;
static atomic_uint s_host_trace_drops;
static atomic_uint s_done_cmds, s_done_acks, s_done_fail, s_submit_fail;
static SemaphoreHandle_t s_cmd_completed;
static bool s_cmd_submit_ok;  // host task alone submits CMDs
static bool s_cmd_granted;
static uint32_t s_reply_timeouts;
static bool s_cmd_completed_ok; // published by completion semaphore

static void host_trace(uint8_t event, const uint8_t *frame, size_t len,
                       uint8_t rate, uint8_t status) {
    unsigned left = atomic_load_explicit(&s_host_trace_left, memory_order_relaxed);
    do {
        if (!left) return;
    } while (!atomic_compare_exchange_weak_explicit(&s_host_trace_left, &left, left - 1,
                                                    memory_order_relaxed, memory_order_relaxed));
    host_tx_trace_t record = {
        .us = esp_timer_get_time(), .len = len, .event = event,
        .fc = frame[0], .rate = rate, .status = status,
        .token = frame[0] == 0x28 && len >= 34 ?
                 (uint16_t)(frame[len - 4] | (frame[len - 3] << 8)) : 0
    };
    if (xQueueSend(s_host_trace_queue, &record, 0) != pdTRUE)
        atomic_fetch_add_explicit(&s_host_trace_drops, 1, memory_order_relaxed);
}

static void host_tx_done(const esp_80211_tx_info_t *info) {
    if (!info || !info->data || info->ifidx != WIFI_IF_AP) return;
    const uint8_t *frame = info->data;
    bool cmd = host_probe_cmd(frame);
    bool ack = frame[0] == 0x18 && frame[1] == 0x02 &&
               memcmp(frame + 4, MP_ACK_MCAST, 6) == 0;
    if (!cmd && !ack) return;
    atomic_fetch_add_explicit(cmd ? &s_done_cmds : &s_done_acks, 1, memory_order_relaxed);
    if (info->tx_status != WIFI_SEND_SUCCESS)
        atomic_fetch_add_explicit(&s_done_fail, 1, memory_order_relaxed);
    host_trace('D', frame, 24u + info->data_len, info->rate, info->tx_status);
    if (cmd) {
        s_cmd_completed_ok = info->tx_status == WIFI_SEND_SUCCESS;
        xSemaphoreGive(s_cmd_completed);
    }
}

static void host_trace_task(void *arg) {
    host_tx_trace_t event;
    for (;;) {
        if (xQueueReceive(s_host_trace_queue, &event, portMAX_DELAY) == pdTRUE)
            ESP_LOGI(TAG, "TXTRACE %c us=%lld fc=%02x len=%u token=%04x rate=%u status=%u",
                     event.event, (long long)event.us, event.fc, event.len,
                     event.token, event.rate, event.status);
    }
}

static inline void host_tx(const uint8_t *b, size_t n) {
    if (b[0] == 0x28) {
        s_cmd_granted = n >= 28 && (b[26] || b[27]);
        portENTER_CRITICAL(&s_admission_lock);
        s_cycle_open = s_cmd_granted;
        s_cycle_started_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_admission_lock);
    }
    host_trace('S', b, n, 255, 0);
    esp_err_t result = esp_wifi_80211_tx(WIFI_IF_AP, b, n, true /* en_sys_seq */);
    if (b[0] == 0x28) s_cmd_submit_ok = result == ESP_OK;
    if (result != ESP_OK)
        atomic_fetch_add_explicit(&s_submit_fail, 1, memory_order_relaxed);
}

// The 0xDD PictoChat room-advert IE in esp_wifi_set_vendor_ie() (vendor_ie_data_t)
// layout: element_id, length, OUI(3), oui_type, then 28B payload. Byte-derived from our
// own perfect01.pcap beacon. The SoftAP HW engine emits this in every beacon (with a
// real HW-maintained TSF), which is the point of the pivot — a regular, HW-timed beacon
// the DS can slave its MP clock to, unlike our old jittery manual injection.
// Mutable: the users field (index 31) is bumped 1->2 on join and re-registered so the
// beacon advertises the DS as present — an 802.11 assoc alone leaves the PictoChat UI
// treating the room as empty; the advertised user count is what populates it.
#define HOST_VIE_USERS_OFF 31
static uint8_t k_pictochat_vendor_ie[34] = {
    0xDD, 0x20,
    0x00,0x09,0xBF, 0x00,          // OUI 00:09:BF, oui_type 0
    0x0a,0x00, 0x69,0x18,          // stepping, lcdsync
    0x01,0x00,0x00,0x00,           // fixed id = 1
    0x00,0x00,0x00,0x00,           // game id = 0
    0x00,0x03,                     // stream_code
    0x08, 0x01,                    // len-from-0x18, beacon_type = 1
    0xc0,0x00, 0xc0,0x00,          // CMD size / REPLY size = 192
    0x8a,0xbd,                     // magic 0x18
    0xa7,0xc0,                     // 0x1a field
#if PICTOCHAT_ONLINE
    0, 0x01,                      // room assigned from board identity before Wi-Fi starts
#else
    HOST_CHATROOM, 0x01,           // chatroom, users (host only until a DS joins)
#endif
    0x04,0x00,                     // fixed 0x0004
};

// Re-register the beacon vendor IE with an updated user count (unregister first so the
// re-register can't fail). Called on STA connect/disconnect.
static void host_set_beacon_users(uint8_t n) {
    k_pictochat_vendor_ie[HOST_VIE_USERS_OFF] = n;
    if (!s_host_beaconing) return; // room closed: keep the IE off the air
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0,
                           k_pictochat_vendor_ie);
    esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0,
                           k_pictochat_vendor_ie);
}

// Show or hide the PictoChat room: the 0xDD IE is what a DS looks for, so dropping it makes
// the room vanish from the DS list; stations already inside are deauthed so they fall out too.
static void host_set_visible(bool visible) {
    if (visible == s_host_beaconing) return;
    s_host_beaconing = visible;
    if (visible) {
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, k_pictochat_vendor_ie);
        esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, k_pictochat_vendor_ie);
    } else {
        esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, k_pictochat_vendor_ie);
        esp_wifi_deauth_sta(0);
    }
    ESP_LOGI(TAG, "room %s", visible ? "open" : "closed");
}

// Strong-symbol override of the IDF beacon builder's DS-Parameter-Set writer (linker
// needs -Wl,-zmuldefs, already in the esp32c6host build flags). A real DS refuses a room
// whose beacon still carries an SSID element, so while hosting we DELETE the (hidden,
// zero-length) SSID IE by sliding the 6-byte Supported Rates element back 2 bytes over
// it, then write the DS Param Set into the freed space. The fixed data-8/data-6 offsets
// are only valid because the SoftAP is configured ssid_hidden=1 (2-byte SSID IE) and
// WIFI_PROTOCOL_11B (exactly-4-rate, 6-byte Supported Rates IE). When not yet hosting,
// fall back to the stock behavior (write a normal 3-byte DS Param Set).
#if PICTOCHAT_ONLINE
uint8_t *ieee80211_add_dsparams(uint8_t *data) {
    // Only the hidden PictoChat beacon has an empty SSID followed by four rates.
    // Let the SDK build ordinary STA probe/association parameters unchanged.
    if (s_host_beaconing && data[-8]==0 && data[-7]==0 && data[-6]==1 && data[-5]==4) {
#else
uint8_t *ieee80211_add_dsparams(uint8_t *data) {
    if (s_host_beaconing) {
#endif
        uint8_t rates[6];
        memcpy(rates, data - 6, 6);   // save Supported Rates element
        memcpy(data - 8, rates, 6);   // slide it back over the 2B zero-length SSID IE
        data[-2] = 0x03;              // DS Parameter Set tag
        data[-1] = 0x01;              // length
#if PICTOCHAT_ONLINE
        data[0] = (uint8_t)online_channel();
#else
        data[0]  = CAPTURE_CHANNEL;   // channel
#endif
        return data + 1;              // 2 bytes reclaimed from the deleted SSID
    }
#if PICTOCHAT_ONLINE
    data[0] = 0x03; data[1] = 0x01; data[2] = (uint8_t)online_channel();
    return data + 3;
#else
    data[0] = 0x03; data[1] = 0x01; data[2] = CAPTURE_CHANNEL;
    return data + 3;
#endif
}

#if PICTOCHAT_ONLINE
// Keep both entry points: --wrap only redirects undefined references, whereas
// the strong symbol also covers calls resolved inside the SDK archive.
uint8_t *__wrap_ieee80211_add_dsparams(uint8_t *data) {
    return ieee80211_add_dsparams(data);
}
#endif

// MP CMD builders request dur=0x04e0, but C6 multicast CMDs have on-air Duration 0.
// Earlier builder/register/buffer overrides did not change that observation. A later
// diagnostic wrapped the *linked* hal_mac_tx_set_ppdu: CMD Duration remained nonzero
// before and after this active setup, while an independent WROOM still captured zero.
// The exact later overwrite/transmission stage, and whether Duration causes type-6
// admission, remain unknown. The old mac_tx_set_duration override was reverted.

static void host_send_cmd_empty(void) {
    uint8_t frame[30];
    size_t len = pictochat_frame_empty(frame, sizeof(frame), HOST_SELF_MAC);
    host_tx(frame, len);
    s_cmds_tx++;
}

static void host_send_cmd(uint16_t tid, unsigned slot, bool admitted) {
    uint8_t frame[138];
    uint8_t members[16][6];
    pictochat_room_members(s_room, members);
    host_poll_fields_result_t fields = host_poll_fields_next(&s_poll_fields[slot], true);
    if (fields.bitmask) fields.bitmask = (uint16_t)(1u << s_room->peers[slot].aid);
    if ((s_cmd_seq & 3) == 0) s_cmd_magic = esp_random();
    size_t len = pictochat_frame_room_members(frame, sizeof(frame), HOST_SELF_MAC,
        members, tid, fields, s_cmd_magic, s_cmd_seq++, admitted);
    host_tx(frame, len);
    s_cmds_tx++;
}

// Default example identity; protocol callers supply their own 84-byte profile.
static const uint8_t host_profile[84] = {
    0x03,0x00,0x09,0x00,0xc6,0xbf,0xc6,0xc6,0x50,0x00,0x49,0x00,
    0x43,0x00,0x54,0x00,0x4f,0x00,0x42,0x00,0x4f,0x00,0x54,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0b,0x00,0x07,0x10,
};
// Encode an application packet with a fresh sequence on its WM port. All
// identity/relay packets grant only the selected client a reply slot.
static void host_send_app(const host_id_packet_t *app, uint16_t sequence, unsigned aid) {
    uint8_t frame[PICTOCHAT_FRAME_MAX];
    size_t len = pictochat_frame_target_app(frame, sizeof(frame), HOST_SELF_MAC,
                                          app, sequence, (uint16_t)(1u << aid));
    configASSERT(len);
    host_tx(frame, len);
    s_cmds_tx++;
}

static void host_send_ack(void) {
    uint8_t frame[28];
    size_t len = pictochat_frame_ack(frame, sizeof(frame), HOST_SELF_MAC);
    host_tx(frame, len);
}

// SoftAP MLME events: the DS's open-system Auth/Assoc is handled natively by the AP, so
// we learn the joined client's MAC here (rather than raw-TXing our own Auth/Assoc-Resp)
// and start/stop the CMD poll loop accordingly.
static void host_wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base != WIFI_EVENT) return;
    if (id != WIFI_EVENT_AP_STACONNECTED && id != WIFI_EVENT_AP_STADISCONNECTED) return;
    bool joining = id == WIFI_EVENT_AP_STACONNECTED;
    wifi_event_ap_staconnected_t *join = data;
    wifi_event_ap_stadisconnected_t *leave = data;
    const uint8_t *mac = joining ? join->mac : leave->mac;
    unsigned slot = PICTOCHAT_ROOM_CLIENTS, count = 0;
    bool accepted = false;
    portENTER_CRITICAL(&s_admission_lock);
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i)
        if (s_stations[i].connected && !memcmp(s_stations[i].mac, mac, 6)) { slot = i; break; }
    if (joining && slot == PICTOCHAT_ROOM_CLIENTS)
        for (unsigned i = 0; i < HOST_RADIO_CLIENTS; ++i)
            if (!s_stations[i].connected) { slot = i; break; }
    if (slot < PICTOCHAT_ROOM_CLIENTS) {
        host_station_t *station = &s_stations[slot];
        if (joining && join->aid >= 1 && join->aid <= 15
#if PICTOCHAT_GHOST_DEMO
            && join->aid != GHOST_AID && memcmp(mac, GHOST_MAC, 6)
#endif
        ) {
            if (!station->connected || station->aid != join->aid) {
                if (++s_generation == 0) ++s_generation;
                *station = (host_station_t){.connected = true, .aid = join->aid,
                                           .generation = s_generation};
                memcpy(station->mac, mac, 6);
            }
            accepted = true;
        } else if (!joining) station->connected = false;
    }
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) count += s_stations[i].connected;
    portEXIT_CRITICAL(&s_admission_lock);
    if (joining && !accepted) esp_wifi_deauth_sta(join->aid);
    host_set_beacon_users(1 + count + PICTOCHAT_GHOST_DEMO
#if PICTOCHAT_ONLINE
        + online_ghost_count()
#endif
    );
    atomic_store_explicit(&s_host_trace_left, 64, memory_order_relaxed);
    ESP_LOGI(TAG, "HOST: %s clients=%u", joining ? (accepted ? "joined" : "rejected") : "left", count);
    host_log_heap(joining ? "association" : "leave");
}
#endif

// ---- Promiscuous RX callback: filter to Nintendo, enqueue for streaming ----
static volatile uint32_t s_seen = 0;

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA &&
        type != WIFI_PKT_CTRL) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 10) return;   // shortest 802.11 frame (ACK/CTS) — allow ctrl frames

    // Keep it only if a Nintendo OUI appears in an address that actually exists
    // for this frame length (control frames may carry only addr1, or addr1+2).
    bool nin = mac_is_nintendo(f + WLAN_ADDR1_OFF);            // addr1 @4  (len>=10)
    if (!nin && len >= 16) nin = mac_is_nintendo(f + WLAN_ADDR2_OFF); // addr2 @10
    if (!nin && len >= 24) nin = mac_is_nintendo(f + WLAN_ADDR3_OFF); // addr3 @16
    if (!nin) return;
    s_seen++;
#if PICTOCHAT_ONLINE
    // Capture the other relay's on-air beacon, not our local builder buffer.
    if (len >= 40 && f[0] == 0x80 && f[10] == 0 && f[11] == 9 &&
        f[12] == 0xbf && f[13] == 0xc6 && f[14] == 0xc6 &&
        (f[15] == 0xd1 || f[15] == 0xd2)) {
        static unsigned beacon_samples;
        if (beacon_samples++ < 3) {
            ESP_LOGI(TAG,"RELAY BEACON channel=%u bytes=%d",pkt->rx_ctrl.channel,len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG,f,len,ESP_LOG_INFO);
        }
    }
#endif

#if SNIFFER_MODE == MODE_SERIAL_MGMT
    int reply_bytes = mp_reply_payload_bytes(f, len, SERIAL_TRACE_HOST);
    if (reply_bytes == 0) s_usb_empty_replies++;
    else if (reply_bytes > 0) {
        s_usb_data_replies++;
        s_usb_reply_bytes += (uint32_t)reply_bytes;
    }
    if (len >= 24 && fc_type(f) == 2 &&
        memcmp(f + 10, SERIAL_TRACE_HOST, 6) == 0 &&
        memcmp(f + 4, MP_CMD_MCAST, 6) == 0) s_usb_host_cmds++;
    bool trace_mp = mp_trace_select(&s_mp_trace, f, len, SERIAL_TRACE_HOST);
    if (!is_handshake_frame(f, len) && !trace_mp) return;
#endif

#if SNIFFER_MODE == MODE_HOST
    {
        const uint8_t *a1 = f + WLAN_ADDR1_OFF;  // dst / RA
        const uint8_t *a2 = f + WLAN_ADDR2_OFF;  // src / TA
        // DIAG: log distinct (type/subtype, src, dst) so we see what the DS is doing
        // (beaconing its own room? scanning? auth-ing to a different BSSID?).
        {
            uint8_t ty = fc_type(f), st = fc_subtype(f);
            uint32_t sig = ((uint32_t)ty << 28) ^ ((uint32_t)st << 24)
                         ^ ((uint32_t)a2[4] << 16) ^ ((uint32_t)a2[5] << 8) ^ a1[5];
            static uint32_t seen[24]; static int sn;
            bool known = false;
            for (int i = 0; i < sn; i++) if (seen[i] == sig) { known = true; break; }
            if (!known && sn < 24) {
                seen[sn++] = sig;
                ESP_LOGI(TAG, "RX ty%d st%2d src=%02x:%02x:%02x:%02x:%02x:%02x "
                         "dst=%02x:%02x:%02x:%02x:%02x:%02x len=%d",
                         ty, st, a2[0],a2[1],a2[2],a2[3],a2[4],a2[5],
                         a1[0],a1[1],a1[2],a1[3],a1[4],a1[5], len);
            }
        }
        // NOTE: the DS's open-system Auth/Assoc to our BSSID is handled by the SoftAP
        // MLME natively (join/leave surface via host_wifi_evt STA CONNECTED/DISCONNECTED);
        // we no longer raw-TX our own Auth/Assoc-Resp. Mgmt frames just fall through.

        // Client REPLY to our CMD poll (data to REPLY multicast).
        if (fc_type(f) == 2 && len >= 24 &&
            memcmp(f + WLAN_ADDR3_OFF, MP_REPLY_MCAST, 6) == 0) {
            if (mp_reply_payload_bytes(f, (size_t)len, HOST_SELF_MAC) < 0) {
                atomic_fetch_add_explicit(&s_reply_invalid, 1, memory_order_relaxed);
                return;
            }
            // Only the selected MAC and association generation may close this
            // cycle. A different room member's reply cannot acknowledge delivery.
            portENTER_CRITICAL(&s_admission_lock);
            unsigned slot = s_cycle_slot;
            host_station_t *station = &s_stations[slot];
            bool selected_client = station->connected &&
                station->generation == s_cycle_generation && !memcmp(f + 10, station->mac, 6);
            bool our_client = s_cycle_open && selected_client;
            if (selected_client) {
                atomic_store_explicit(&s_reply_rssi, pkt->rx_ctrl.rssi, memory_order_relaxed);
                atomic_store_explicit(&s_reply_noise, pkt->rx_ctrl.noise_floor, memory_order_relaxed);
                atomic_store_explicit(&s_reply_delay_us,
                    (int)(esp_timer_get_time() - s_cycle_started_us), memory_order_relaxed);
            }
            uint32_t generation = station->generation;
            bool send_ack = false;
            if (our_client) {
                if (host_admission_reply(f, (size_t)len, HOST_SELF_MAC, station->mac))
                    station->admitted = true;
                send_ack = ack_gate_reply(&s_ack_gate);
                s_cycle_open = false;
            }
            portEXIT_CRITICAL(&s_admission_lock);
            if (!our_client) {
                atomic_fetch_add_explicit(selected_client ? &s_reply_closed : &s_reply_other,
                                          1, memory_order_relaxed);
                return;
            }
            s_rx_replies++;
            host_trace('R', f, (size_t)len - 4, 255, 0);
            if (send_ack) { host_send_ack(); s_reply_acks++; }
            if (len >= 36 && len <= 32 + sizeof(((host_id_packet_t *)0)->bytes)) {
                uint16_t kind = f[26] | ((uint16_t)f[27] << 8);
                uint8_t port = f[25] & 15;
                if ((kind == 0 && port == 13) || (kind == 2 && port == 14)) {
                    host_app_rx_t input = {.generation = generation};
                    input.app.len = (uint16_t)(len - 32);
                    memcpy(input.app.bytes, f + 26, input.app.len);
                    if (xQueueSend(s_host_app_queue[slot], &input, 0) != pdTRUE)
                        atomic_fetch_add_explicit(&s_host_app_drops, 1, memory_order_relaxed);
                }
            }
            // DIAG: log distinct REPLY shapes (wmHeader + app type + size) so we can
            // see the client's identity-send sequence and what it waits for.
            if (len >= 30) {
                uint16_t wm  = f[24] | (f[25] << 8);
                uint16_t typ = f[26] | (f[27] << 8);
                // Unmissable admission markers: ap=0006 = the DS's type-6 room-admission
                // (it accepted our poll slot at the APP layer); ap=0002 = its identity/name
                // reply to our tid=2 profile. Either means the DS's MP app ENGAGED our host.
                static bool saw6, saw2;
                if (typ == 0x0006 && !saw6) {
                    saw6 = true;
                    ESP_LOGW(TAG, "*** DS APP ENGAGED: type-6 ADMISSION (ap=0006 len=%d) ***", len);
                }
                if (typ == 0x0002 && !saw2) {
                    saw2 = true;
                    ESP_LOGW(TAG, "*** DS SENT IDENTITY (ap=0002 len=%d) -> mutual profile ***", len);
                }
                uint32_t sig = ((uint32_t)wm << 16) ^ ((uint32_t)typ << 4) ^ (len & 0xF);
                static uint32_t seen[32]; static int sn;
                bool known = false;
                for (int k = 0; k < sn; k++) if (seen[k] == sig) { known = true; break; }
                if (!known && sn < 32) {
                    seen[sn++] = sig;
                    int bl = len - 24; if (bl > 22) bl = 22;
                    char hex[48]; int p = 0;
                    for (int k = 0; k < bl; k++) p += sprintf(hex + p, "%02x", f[24 + k]);
                    ESP_LOGI(TAG, "RPLY wm=%04x len=%d app=%s", wm, len, hex);
                }
            }
        }
        return;   // HOST never streams
    }
#endif

#if SNIFFER_MODE == MODE_JOIN
    const uint8_t *a1 = f + WLAN_ADDR1_OFF;  // dst / RA
    const uint8_t *a2 = f + WLAN_ADDR2_OFF;  // src / TA

    // Latch the host's live "key": it trails every host CMD-ACK as `<key> fd 3f`.
    // The real DS echoes it in its 30B steady reply payload `00 00 <key>`.
    if (memcmp(a1, MP_ACK_MCAST, 6) == 0 && len >= 32 &&
        f[30] == 0xfd && f[31] == 0x3f) {
        s_hostkey[0] = f[28]; s_hostkey[1] = f[29];
    }

    // Data phase: the host CMD poll is a data frame to the CMD multicast. It is
    // BOTH the poll (client_bitmask) AND the carrier for member-list (type 4/5)
    // and identity-announce (type 1) app frames. If our AID bit is polled, fire
    // an empty REPLY immediately (in-slot keepalive), then decode the app layer.
    if (fc_type(f) == 2 && memcmp(a1, MP_CMD_MCAST, 6) == 0) {
        s_cmd_seen++;   // DIAG
        if (s_js == JS_JOINED && len >= 28) {
            s_polls++;
            uint16_t bitmask = f[26] | (f[27] << 8);  // CMD body 0x1A client_bitmask
            if (bitmask & s_aid_bit) {
#if SEND_PROFILE
                // Real DS sends its 128B identity only a handful of times, and it
                // needs a WIDE reply slot (client_time ~998us); a 128B frame @2Mbps
                // is ~512us and overflows the narrow 214us slot. So send identity a
                // few times, only when the granted slot is wide enough.
                uint16_t ctime = f[24] | (f[25] << 8);   // reply-slot width (us)
                if (ctime >= 500 && s_profile_sends < 7) {
                    // Byte-exact real-DS identity sequence (perfect01.pcap map_join),
                    // one frame per wide reply slot:
                    //   hdr136 -> announce0 -> data0 -> ctrl -> announce1 -> data1 -> ctrl
                    switch (s_profile_sends) {
                        case 0: join_send_hdr136();          break;  // ConsoleId header
                        case 1: join_send_ctrl52();          break;  // announce st0 (tok)
                        case 2: join_send_profile(0);        break;  // data st0 (name)
                        case 3: join_send_ctrl_progress(0x0009); break;  // progress
                        case 4: join_send_ctrl52();          break;  // announce st1 (tok)
                        case 5: join_send_profile(1);        break;  // data st1
                        case 6: join_send_ctrl_progress(0x000b); break;  // progress
                    }
                    s_profile_sends++;
                } else {
                    join_send_reply();
                }
#else
                join_send_reply();
#endif
            }

            // DIAG (Stage B): decode the CMD app layer. Header map (after 24B MAC):
            //   f[24..25] client_time  f[26..27] bitmask  f[28..29] wmHeader
            //   f[30..31] app type_id  f[32..33] size  f[34..37] magic
            //   f[38..]   member table (16 x 6B, MAC halfword-swapped, host first)
            if (len >= 44) {
                uint16_t wm  = f[28] | (f[29] << 8);
                uint16_t tid = f[30] | (f[31] << 8);
                // our MAC as stored in the member table (each 2-byte pair swapped)
                static const uint8_t self_sw[6] =
                    {0x09, 0x00, 0xC6, 0xBF, 0xC6, 0xC6};
                bool in_roster = false;
                int  members   = 0;
                if ((tid == 4 || tid == 5) && len >= 38 + 96) {
                    const uint8_t *mt = f + 38;
                    for (int e = 0; e < 16; e++) {
                        const uint8_t *m = mt + e * 6;
                        bool zero = true;
                        for (int i = 0; i < 6; i++) if (m[i]) { zero = false; break; }
                        if (!zero) members++;
                        if (memcmp(m, self_sw, 6) == 0) in_roster = true;
                    }
                }
                // Log only when the picture changes (type_id, roster, or count).
                static int last_tid = -2, last_roster = -2, last_members = -2;
                if (tid != last_tid || in_roster != last_roster ||
                    members != last_members) {
                    last_tid = tid; last_roster = in_roster; last_members = members;
                    int n = len - 30; if (n > 20) n = 20;
                    char hex[48]; int p = 0;
                    for (int i = 0; i < n; i++) p += sprintf(hex + p, "%02x", f[30 + i]);
                    uint16_t ct = f[24] | (f[25] << 8);  // reply-slot width (us)
                    ESP_LOGI(TAG, "CMD ct=%u wm=%04x tid=%u members=%d us_in_roster=%d app=%s",
                             ct, wm, tid, members, in_roster, hex);
                }
            }
        }
        return;
    }
    // DIAG: any non-CMD data frame the host directs at us (identity unicast, etc.)
    if (s_js == JS_JOINED && fc_type(f) == 2 && len >= 26) {
        const uint8_t *a3 = f + 16;
        uint32_t sig = ((uint32_t)a3[5] << 24) ^ ((uint32_t)a1[5] << 20)
                     ^ ((uint32_t)f[24] << 12) ^ ((uint32_t)f[25] << 4)
                     ^ (uint32_t)(len & 0xF);
        static uint32_t seen_sig[48];
        static int seen_n;
        bool known = false;
        for (int i = 0; i < seen_n; i++) if (seen_sig[i] == sig) { known = true; break; }
        if (!known && seen_n < 48) {
            seen_sig[seen_n++] = sig;
            int bl = len - 24; if (bl > 24) bl = 24;
            char hex[64]; int p = 0;
            for (int i = 0; i < bl; i++) p += sprintf(hex + p, "%02x", f[24 + i]);
            const char *dst = (memcmp(a1, JOIN_SELF_MAC, 6) == 0) ? "US" : "mc";
            ESP_LOGI(TAG, "DIAG->%s a3=%02x:%02x fc=%02x%02x len=%d body=%s",
                     dst, a3[4], a3[5], f[0], f[1], len, hex);
        }
    }
    if (fc_type(f) != 0) return;             // otherwise management only
    uint8_t st = fc_subtype(f);

    if (st == 8) {                            // beacon
        if (memcmp(a2, JOIN_HOST_MAC, 6) == 0) {   // our host -> latch IE
            s_bcn_host++;                     // DIAG
            bool had = s_have_beacon;
            join_parse_beacon(f, len);
            if (!had && s_have_beacon)        // DIAG: confirm the IE actually parsed
                ESP_LOGI(TAG, "BEACON latched host IE game=%02x%02x%02x%02x stream=0x%04x",
                         s_game_id[0], s_game_id[1], s_game_id[2], s_game_id[3],
                         s_stream_code);
            return;
        }
        // DIAG: a Nintendo beacon from someone other than our hardcoded host — log
        // each distinct source once so we can spot a room hosted by the other DS.
        s_bcn_other++;
        static uint8_t seen_bcn[8][6];
        static int seen_bcn_n;
        bool known = false;
        for (int i = 0; i < seen_bcn_n; i++)
            if (memcmp(seen_bcn[i], a2, 6) == 0) { known = true; break; }
        if (!known && seen_bcn_n < 8) {
            memcpy(seen_bcn[seen_bcn_n++], a2, 6);
            ESP_LOGI(TAG, "BEACON from non-host %02x:%02x:%02x:%02x:%02x:%02x",
                     a2[0], a2[1], a2[2], a2[3], a2[4], a2[5]);
        }
        return;
    }
    // Below here we only care about the host talking directly to us.
    if (memcmp(a1, JOIN_SELF_MAC, 6) != 0 || memcmp(a2, JOIN_HOST_MAC, 6) != 0)
        return;
    if (st == 11) {                          // Auth-Resp
        uint16_t aseq = (len >= 28) ? (f[26] | (f[27] << 8)) : 0;
        uint16_t stat = (len >= 30) ? (f[28] | (f[29] << 8)) : 0xFFFF;
        if (aseq == 2 && stat == 0) s_got_authresp = true;
    } else if (st == 1) {                    // Assoc-Resp: capture status + AID
        s_assoc_status = (len >= 28) ? (f[26] | (f[27] << 8)) : 0xFFFF;
        if (len >= 30) {
            uint16_t aid = (f[28] | (f[29] << 8)) & 0x3FFF;  // strip the top 2 bits
            s_aid_bit = (uint16_t)(1u << (aid & 0x0F));
        }
    } else if (st == 12) {                   // Deauth -> restart
        s_js = JS_SCAN; s_got_authresp = false; s_assoc_status = -1;
    }
    return;
#endif

#if SNIFFER_MODE == MODE_DISCOVERY
    // Serial-only: summarize the frame and the channel it arrived on.
    const uint8_t *src = f + WLAN_ADDR2_OFF;
    const char *kind = (fc_type(f) == 0 && fc_subtype(f) == 8) ? "BEACON"
                     : (fc_type(f) == 2) ? "DATA " : "MGMT ";
    ESP_LOGI(TAG, "ch%2d rssi%4d %s src=%02X:%02X:%02X:%02X:%02X:%02X len=%d",
             pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi, kind,
             src[0], src[1], src[2], src[3], src[4], src[5], len);
#else
    // Stream: build radiotap + frame into a queue item.
    if (RT_LEN + len > CAP_MAX_BYTES) len = CAP_MAX_BYTES - RT_LEN;
    cap_item_t item;
    size_t rt = build_radiotap(item.data, pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi);
    memcpy(item.data + rt, f, len);
    item.len = (uint16_t)(rt + len);
    item.rssi = pkt->rx_ctrl.rssi;
    item.channel = pkt->rx_ctrl.channel;
    // Drop if the queue is full rather than block the WiFi task.
#if SNIFFER_MODE == MODE_SERIAL_MGMT
    item.rx_us = pkt->rx_ctrl.timestamp;
    item.rx_rate = pkt->rx_ctrl.rate;
    item.sig_mode = pkt->rx_ctrl.sig_mode;
    if (xQueueSend(s_cap_queue, &item, 0) != pdTRUE) s_capture_drops++;
#else
    xQueueSend(s_cap_queue, &item, 0);
#endif
#endif
}

#if SNIFFER_MODE == MODE_SERIAL_MGMT
// Print outside the WiFi callback; frames include FCS, matching the UDP sniffer.
static void serial_mgmt_task(void *arg) {
    cap_item_t item;
    char hex[2 * CAP_MAX_BYTES + 1];
    const char digits[] = "0123456789abcdef";
    for (;;) {
        if (xQueueReceive(s_cap_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        size_t n = item.len - RT_LEN;
        for (size_t i = 0; i < n; ++i) {
            uint8_t b = item.data[RT_LEN + i];
            hex[2 * i] = digits[b >> 4];
            hex[2 * i + 1] = digits[b & 15];
        }
        hex[2 * n] = '\0';
        ESP_LOGI(TAG, "%s rx_us=%lu ch=%u rssi=%d len=%u drops=%lu rate=%u sig=%u frame=%s",
                 fc_type(item.data + RT_LEN) == 2 ? "MP" : "MGMT",
                 (unsigned long)item.rx_us, item.channel, item.rssi, (unsigned)n,
                 (unsigned long)s_capture_drops, item.rx_rate, item.sig_mode, hex);
    }
}
#endif

#if SNIFFER_MODE == MODE_STREAM
// ---- Sender task: wrap queue items in a libpcap record, UDP-broadcast ----
static void sender_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(UDP_PORT);
    dst.sin_addr.s_addr = inet_addr("192.168.4.255"); // SoftAP subnet broadcast

    static uint8_t out[16 + CAP_MAX_BYTES];
    cap_item_t item;
    for (;;) {
        if (xQueueReceive(s_cap_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        // libpcap per-record header (LE): ts_sec, ts_usec, incl_len, orig_len.
        int64_t us = esp_timer_get_time();
        uint32_t ts_sec = (uint32_t)(us / 1000000);
        uint32_t ts_usec = (uint32_t)(us % 1000000);
        uint32_t inc = item.len;
        memcpy(out + 0,  &ts_sec,  4);
        memcpy(out + 4,  &ts_usec, 4);
        memcpy(out + 8,  &inc,     4);
        memcpy(out + 12, &inc,     4);
        memcpy(out + 16, item.data, item.len);

        sendto(sock, out, 16 + item.len, 0,
               (struct sockaddr *)&dst, sizeof(dst));
    }
}
#endif

// ---- Periodic heartbeat so you know it's alive even when quiet ----
static void heartbeat_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive — %lu Nintendo frames seen so far",
                 (unsigned long)s_seen);
#if SNIFFER_MODE == MODE_HOST
        host_log_heap("periodic");
#endif
#if SNIFFER_MODE == MODE_SERIAL_MGMT
        ESP_LOGI(TAG, "USB MP totals: host_cmd=%lu reply_empty=%lu reply_data=%lu payload_bytes=%lu",
                 (unsigned long)s_usb_host_cmds, (unsigned long)s_usb_empty_replies,
                 (unsigned long)s_usb_data_replies, (unsigned long)s_usb_reply_bytes);
#endif
    }
}

#if SNIFFER_MODE == MODE_DISCOVERY
// ---- Channel hopper: sweep 1..13 so we can find the DS channel ----
static void hopper_task(void *arg) {
    uint8_t ch = 1;
    for (;;) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(300)); // dwell long enough to catch beacons
        ch = (ch >= 13) ? 1 : ch + 1;
    }
}
#endif

#if SNIFFER_MODE == MODE_JOIN
// Active client: latch a beacon, then drive Auth-Req -> Assoc-Req, retrying until
// the host accepts (Assoc-Resp status 0). Deauth or timeout resets to SCAN.
static void join_task(void *arg) {
    static int assoc_fails = 0;    // consecutive full auth+assoc rounds that failed
    static int assoc_resends = 0;  // Assoc-Req resends within the current round
    // Clear any stale association from a prior boot (e.g. reflash while joined) so
    // the host will accept a fresh handshake instead of rejecting a known MAC.
    join_send_deauth();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60));
        switch (s_js) {
        case JS_SCAN:
            if (!s_have_beacon) {             // wait for the host's 0xDD IE
                static int scan_ticks = 0;    // DIAG: ~1s throttle (60ms/tick)
                if (++scan_ticks >= 16) {
                    scan_ticks = 0;
                    ESP_LOGW(TAG, "SCAN: no host beacon — host_bcn=%lu other_bcn=%lu "
                             "cmd=%lu seen=%lu",
                             (unsigned long)s_bcn_host, (unsigned long)s_bcn_other,
                             (unsigned long)s_cmd_seen, (unsigned long)s_seen);
                }
                continue;
            }
            s_got_authresp = false; s_assoc_status = -1;
            join_send_auth();
            ESP_LOGI(TAG, "JOIN: tx Auth-Req (stream=0x%04x)", s_stream_code);
            s_js = JS_AUTH_SENT;
            break;
        case JS_AUTH_SENT: {
            for (int i = 0; i < JOIN_RETRY_MS / 20 && !s_got_authresp; i++)
                vTaskDelay(pdMS_TO_TICKS(20));
            if (!s_got_authresp) { s_js = JS_SCAN; break; }   // retry from scratch
            assoc_resends = 0;                // fresh round: reset resend budget
            join_send_assoc();
            ESP_LOGI(TAG, "JOIN: rx Auth-Resp -> tx Assoc-Req");
            s_js = JS_ASSOC_SENT;
            break;
        }
        case JS_ASSOC_SENT: {
            for (int i = 0; i < JOIN_RETRY_MS / 20 && s_assoc_status < 0; i++)
                vTaskDelay(pdMS_TO_TICKS(20));
            if (s_assoc_status == 0) {
                ESP_LOGI(TAG, "JOIN: Assoc-Resp OK  *** JOINED — sending identity ***");
                assoc_fails = 0; assoc_resends = 0;
                s_profile_sends = 0;          // re-announce our profile this session
                s_js = JS_JOINED;
            } else if (++assoc_resends < ASSOC_RESEND_MAX) {
                // Auth is reliably answered but the Assoc-Resp is often missed. The
                // host keeps us authenticated, so resend the Assoc-Req directly
                // instead of paying for a full re-auth round-trip. join_send_assoc()
                // reads the live s_game_id/s_stream_code, which the beacon RX cb
                // refreshes on every beacon — so each resend uses the freshest
                // stream_code (a stale code is a common rejection cause).
                s_assoc_status = -1;
                join_send_assoc();
                ESP_LOGW(TAG, "JOIN: assoc miss — resend Assoc-Req (%d/%d, stream=0x%04x)",
                         assoc_resends, ASSOC_RESEND_MAX, s_stream_code);
                // stay in JS_ASSOC_SENT to await the resend's response
            } else {
                ESP_LOGW(TAG, "JOIN: assoc status=%d after %d resends — full re-auth",
                         s_assoc_status, assoc_resends);
                assoc_resends = 0;
                // A run of failed rounds usually means a stale ghost or stale beacon.
                // Deauth to clear the host's entry and force a fresh beacon latch.
                if (++assoc_fails >= 3) {
                    assoc_fails = 0;
                    join_send_deauth();
                    s_have_beacon = false;    // re-latch the freshest beacon
                }
                s_js = JS_SCAN;               // rejection is normal; retry
            }
            break;
        }
        case JS_JOINED: {
            // Associated + replying to CMD polls. Report progress; if the poll
            // stream dries up (host dropped us without a deauth we caught),
            // fall back to SCAN and rejoin.
            uint32_t p0 = s_polls;
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "JOIN: in room — polls=%lu replies=%lu (+%lu polls/s)",
                     (unsigned long)s_polls, (unsigned long)s_replies,
                     (unsigned long)(s_polls - p0));
            if (s_polls == p0) {             // no polls for a full second
                ESP_LOGW(TAG, "JOIN: poll stream stopped — rejoining");
                s_js = JS_SCAN; s_got_authresp = false; s_assoc_status = -1;
            }
            break;
        }
        }
    }
}

// Bring the STA radio + promiscuous capture back up without a reboot. Used by the
// watchdog as the first, cheap attempt to clear an RX wedge. Errors are ignored
// deliberately: if the soft path can't recover, the watchdog reboots next tick.
static void radio_soft_reinit(void) {
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_mac(WIFI_IF_STA, JOIN_SELF_MAC);
    esp_wifi_start();
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
                       WIFI_PROMIS_FILTER_MASK_CTRL,
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(CAPTURE_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
}

// RX-wedge watchdog. The C6 radio wedges (RX stops, s_seen frozen) after a long
// run of raw-TX auth grinding, and used to need a manual esptool hard reset. An
// active room always yields Nintendo frames (host beacons ~10/s at minimum), so
// s_seen not advancing for RX_STALL_SECS means the radio is wedged, not idle.
// Recovery: soft radio re-init first; if the next window is still frozen, reboot.
static void watchdog_task(void *arg) {
    uint32_t last = s_seen;
    int  stall = 0;
    bool tried_soft = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_seen != last) {                 // radio is alive
            last = s_seen; stall = 0; tried_soft = false;
            continue;
        }
        if (++stall < RX_STALL_SECS) continue; // not yet long enough to call it wedged
        if (!tried_soft) {
            ESP_LOGW(TAG, "WATCHDOG: RX wedged (%ds no frames) — soft radio re-init",
                     stall);
            radio_soft_reinit();
            tried_soft = true;
            // Drop back to a clean scan so we re-latch a fresh beacon and rejoin.
            s_js = JS_SCAN; s_have_beacon = false;
            s_got_authresp = false; s_assoc_status = -1;
            stall = 0; last = s_seen;
        } else {
            ESP_LOGE(TAG, "WATCHDOG: still wedged after soft re-init — rebooting");
            esp_restart();
        }
    }
}
#endif

#if SNIFFER_MODE == MODE_HOST
// Host loop: beacon ~every 100ms so the room appears; once a DS associates, drive the
// CMD poll + mandatory CMD-ACK (~100/s) with the type-5 member heartbeat to keep it.
// Application policy: react to complete messages without inspecting protocol cursors.
// Expensive work goes to queues; message bytes are borrowed only for this call.
static void host_room_event(void *context, const pictochat_event_t *event) {
    const pictochat_room_t *room = context;
    if (event->type == PICTOCHAT_MESSAGE_RECEIVED) {
#if PICTOCHAT_ONLINE
        online_message(event);
        return;
#endif
        ++s_drawings_received;
        host_drawing_t *drawing = malloc(sizeof(*drawing) + event->length);
        if (drawing) {
            drawing->id = s_drawings_received;
            pictochat_room_delivery(room, &drawing->delivery, esp_random());
            drawing->len = (uint16_t)event->length;
            memcpy(drawing->announcement, event->announcement, 20);
            memcpy(drawing->body, event->body, event->length);
            if (xQueueSend(s_drawing_ready, &drawing, 0) != pdTRUE) {
                free(drawing); ++s_drawing_drops;
            }
        } else {
            ++s_drawing_drops;
            ESP_LOGE(TAG, "DRAW allocation failed bytes=%u",
                     (unsigned)(sizeof(*drawing) + event->length));
        }
        host_log_heap("drawing_received");
        ESP_LOGI(TAG, "DRAW received=%lu aid=%u len=%u hash=%08lx drops=%lu",
            (unsigned long)s_drawings_received, event->aid, (unsigned)event->length,
            (unsigned long)host_message_hash(event->body, event->length),
            (unsigned long)s_drawing_drops);
    } else if (event->type == PICTOCHAT_MESSAGE_SENT) {
        host_log_heap("drawing_sent");
        ++s_drawings_sent;
        ESP_LOGI(TAG, "DRAW outbound TX complete aid=%u sender=%u count=%lu bytes=%u hash=%08lx",
            event->aid, event->sender_slot, (unsigned long)s_drawings_sent,
            (unsigned)event->length, (unsigned long)host_message_hash(event->body, event->length));
    } else if (event->type == PICTOCHAT_PEER_READY) {
        ESP_LOGI(TAG, "ROOM ready aid=%u generation=%lu", event->aid,
                 (unsigned long)event->generation);
    }
}

static void host_build_profile(uint8_t profile[84]) {
    memcpy(profile, host_profile, sizeof(host_profile));
    bool profile_ok = host_profile_set_bio(profile, host_profile_bio,
                      sizeof(host_profile_bio) / sizeof(host_profile_bio[0]) - 1);
    configASSERT(profile_ok);
    (void)profile_ok;
    for (unsigned i=0; i<6; ++i) profile[2+i]=HOST_SELF_MAC[i^1];
#if PICTOCHAT_ONLINE
    memset(profile+8,0,20);
    char name[8]="RELAY ?"; name[6]=(char)('A'+online_node()-1);
    for (unsigned i=0; name[i]; ++i) profile[8+2*i]=(uint8_t)name[i];
    static const uint_least16_t relay_bio[]=u"Wi-Fi PictoChat bridge";
    host_profile_set_bio(profile,relay_bio,sizeof(relay_bio)/sizeof(relay_bio[0])-1);
#endif
}

static void host_log_status(unsigned clients,
                            const uint32_t rx_rejected[PICTOCHAT_ROOM_CLIENTS],
                            uint32_t last_cmds) {
    ESP_LOGI(TAG, "HOST: clients=%u cmds_tx=%lu rx_replies=%lu reply_acks=%lu timeouts=%lu "
             "(+%lu cmd/s)", clients, (unsigned long)s_cmds_tx,
             (unsigned long)s_rx_replies, (unsigned long)s_reply_acks,
             (unsigned long)s_reply_timeouts,
             (unsigned long)(s_cmds_tx - last_cmds));
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) {
        const pictochat_peer_t *p = &s_room->peers[i];
        if (p->connected)
            ESP_LOGI(TAG, "ROOM aid=%u admitted=%u phase=%u identity=%lu/%lu replay=%u",
                p->aid, (unsigned)p->admitted, p->session.identity.phase,
                (unsigned long)p->versions[0], (unsigned long)p->versions[1],
                (unsigned)p->replay_active);
        if (p->connected && !p->ghost) {
            const host_message_rx_t *rx = &p->session.received;
            ESP_LOGI(TAG, "DRAW RX aid=%u active=%u covered=%u/%u final=%u invalid=%u complete=%u rejected=%lu pending=%u transfer=%u",
                p->aid, rx->active, rx->covered, rx->total, rx->final_seen,
                rx->invalid, rx->complete, (unsigned long)rx_rejected[i],
                p->session.identity.pending, p->session.identity.transfer_size);
        }
    }
    ESP_LOGI(TAG, "TX duration probe: ppdu=%u cmd=%u before_zero=%u after_zero=%u changed=%u",
             atomic_load_explicit(&s_ppdu_calls, memory_order_relaxed),
             atomic_load_explicit(&s_ppdu_cmds, memory_order_relaxed),
             atomic_load_explicit(&s_ppdu_before_zero, memory_order_relaxed),
             atomic_load_explicit(&s_ppdu_after_zero, memory_order_relaxed),
             atomic_load_explicit(&s_ppdu_changed, memory_order_relaxed));
    ESP_LOGI(TAG, "TX late probe: edca=%u cmd=%u zero=%u",
             atomic_load_explicit(&s_edca_calls, memory_order_relaxed),
             atomic_load_explicit(&s_edca_cmds, memory_order_relaxed),
             atomic_load_explicit(&s_edca_zero, memory_order_relaxed));
    ESP_LOGI(TAG, "TX completion: cmd=%u ack=%u failed=%u rejected=%u trace_drops=%u",
             atomic_load_explicit(&s_done_cmds, memory_order_relaxed),
             atomic_load_explicit(&s_done_acks, memory_order_relaxed),
             atomic_load_explicit(&s_done_fail, memory_order_relaxed),
             atomic_load_explicit(&s_submit_fail, memory_order_relaxed),
             atomic_load_explicit(&s_host_trace_drops, memory_order_relaxed));
    // Diagnostic software timing, not calibrated over-the-air latency.
    ESP_LOGI(TAG, "RADIO DIAG: closed=%u other=%u invalid=%u last_rssi=%d noise=%d reply_after_submit_us=%d",
             atomic_load_explicit(&s_reply_closed, memory_order_relaxed),
             atomic_load_explicit(&s_reply_other, memory_order_relaxed),
             atomic_load_explicit(&s_reply_invalid, memory_order_relaxed),
             atomic_load_explicit(&s_reply_rssi, memory_order_relaxed),
             atomic_load_explicit(&s_reply_noise, memory_order_relaxed),
             atomic_load_explicit(&s_reply_delay_us, memory_order_relaxed));
}

static void host_task(void *arg) {
    uint32_t last_cmds = 0;
    uint32_t rx_rejected[PICTOCHAT_ROOM_CLIENTS] = {0};
    int64_t next_stat = 0;
    uint8_t profile[84];
    host_build_profile(profile);
    pictochat_room_reset(s_room, profile);
#if PICTOCHAT_GHOST_DEMO
    uint8_t ghost_profile[84];
    memcpy(ghost_profile, profile, sizeof(ghost_profile));
    for (unsigned i = 0; i < 6; ++i) ghost_profile[2+i] = GHOST_MAC[i^1];
    memset(ghost_profile + 8, 0, 20);
    static const char ghost_name[] = "GHOST";
    for (unsigned i = 0; i < sizeof(ghost_name)-1; ++i) ghost_profile[8+2*i] = ghost_name[i];
    static const uint_least16_t ghost_bio[] = u"Local ghost; online next";
    // Bio is limited to 26 UTF-16 units.
    bool ghost_bio_ok = host_profile_set_bio(ghost_profile, ghost_bio,
        sizeof(ghost_bio) / sizeof(ghost_bio[0]) - 1);
    configASSERT(ghost_bio_ok);
    (void)ghost_bio_ok;
    bool ghost_joined = pictochat_room_ghost_join(s_room, GHOST_SLOT, GHOST_AID,
        GHOST_GENERATION, ghost_profile, esp_random(), esp_random());
    configASSERT(ghost_joined);
    (void)ghost_joined;
    ESP_LOGI(TAG, "GHOST demo: virtual member aid=%u; drawing echoes use GHOST", GHOST_AID);
#endif
    bool registered = pictochat_room_set_handler(s_room, host_room_event, s_room);
    configASSERT(registered);
    (void)registered;
    for (;;) {
        int64_t now = esp_timer_get_time();
        host_station_t stations[PICTOCHAT_ROOM_CLIENTS];
        portENTER_CRITICAL(&s_admission_lock);
        memcpy(stations, s_stations, sizeof(stations));
        portEXIT_CRITICAL(&s_admission_lock);
        unsigned clients = 0;
        // Remove old generations first, so AID reuse cannot collide with a
        // departed peer later in the table. Only this task touches the engine.
        for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) {
            pictochat_peer_t *peer = &s_room->peers[i];
            if (peer->connected && !peer->ghost && (!stations[i].connected ||
                peer->generation != stations[i].generation)) pictochat_room_leave(s_room, i);
        }
        for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) {
            if (!stations[i].connected) continue;
            ++clients;
            pictochat_peer_t *peer = &s_room->peers[i];
            if (!peer->connected) {
                bool joined = pictochat_room_join(s_room, i, stations[i].mac, stations[i].aid,
                    stations[i].generation, esp_random(), esp_random());
                configASSERT(joined);
                (void)joined;
                host_poll_fields_reset(&s_poll_fields[i]);
            }
            peer->admitted = stations[i].admitted;
            host_app_rx_t input;
            while (xQueuePeek(s_host_app_queue[i], &input, 0) == pdTRUE) {
                int result = pictochat_room_receive(s_room, i, input.generation,
                                                    input.app.bytes, input.app.len);
                if (result == -2) break; // retain input until relay/fanout has capacity
                if (result < 0) ++rx_rejected[i];
                xQueueReceive(s_host_app_queue[i], &input, 0);
            }
        }
#if PICTOCHAT_ONLINE
        host_set_visible(online_room_wanted(now));
        if (online_tick(s_room) && s_host_beaconing) host_set_beacon_users(1 + clients + online_ghost_count());
#endif
        // PICTOBOT is a room participant: every member receives the same reply.
        // Keep the queued body until each recipient has copied it or disconnected.
        host_drawing_t *drawing;
        if (xQueuePeek(s_drawing_ready, &drawing, 0) == pdTRUE) {
#if PICTOCHAT_GHOST_DEMO
            int result = pictochat_room_ghost_send(s_room, GHOST_SLOT, GHOST_GENERATION,
                drawing->announcement, drawing->body, drawing->len, drawing->delivery.token);
            bool queued = result == 0;
            if (result == -1) {
                ESP_LOGE(TAG, "GHOST rejected drawing id=%lu", (unsigned long)drawing->id);
                ++s_drawing_drops;
                queued = true; // export the original rather than wedge the queue
            }
#else
            bool queued = pictochat_room_reply(s_room, &drawing->delivery, drawing->announcement,
                                               drawing->body, drawing->len, HOST_SELF_MAC);
#endif
            if (queued) {
                xQueueReceive(s_drawing_ready, &drawing, 0);
                if (xQueueSend(s_drawing_dump, &drawing, 0) != pdTRUE) {
                    free(drawing); ++s_drawing_drops;
                }
            }
        }
        unsigned slot;
        pictochat_output_t output;
        if (pictochat_room_prepare(s_room, &slot, &output)) {
            pictochat_peer_t *peer = &s_room->peers[slot];
            bool admitted = peer->admitted;
            // Every CMD, including roster/identity, gets its own reply slot + ACK.
            // Previously identity frames were appended after the heartbeat's ACK,
            // and independent modulo schedules sent orphaned/out-of-order fragments.
            portENTER_CRITICAL(&s_admission_lock);
            s_cycle_open = false;
            s_cycle_slot = slot;
            s_cycle_generation = peer->generation;
            ack_gate_begin(&s_ack_gate);
            portEXIT_CRITICAL(&s_admission_lock);
            host_id_packet_t outgoing = output.packet;
            bool app_sent = output.application;
            pictochat_tx_result_t tx_result = PICTOCHAT_TX_FAILED;
            switch (output.kind) {
                case HOST_FRAME_EMPTY:     host_send_cmd_empty(); break;
                case HOST_FRAME_ROSTER:    host_send_cmd(4, slot, admitted); break;
                case HOST_FRAME_HEARTBEAT: host_send_cmd(5, slot, admitted); break;
                case HOST_FRAME_SESSION:  host_send_app(&outgoing, output.sequence, peer->aid); break;
            }
            if (!s_cmd_submit_ok) {
                ack_gate_claim(&s_ack_gate); // rejected poll has no reply window
            } else {
                // Exactly one CMD is outstanding. Never reuse its completion
                // signal for a later cycle after a timeout: wait for that CMD.
                // A late/missing callback is a driver stall, not permission to
                // submit another poll or send an ACK before transmission ends.
                bool warned = false;
                while (xSemaphoreTake(s_cmd_completed, pdMS_TO_TICKS(100)) != pdTRUE) {
                    if (!warned) {
                        ESP_LOGW(TAG, "HOST: waiting for CMD TX completion; no new poll submitted");
                        warned = true;
                    }
                }
                if (s_cmd_completed_ok) {
                    tx_result = PICTOCHAT_TX_DELIVERED;
                    if (admitted && s_cmd_granted) {
                        // A completed radio TX does not prove the DS received it.
                        // Like the reference runner, wait for the polled reply;
                        // do not acknowledge missing client data on a timeout.
                        int64_t deadline = esp_timer_get_time() + 5000;
                        while (!ack_gate_replied(&s_ack_gate) && esp_timer_get_time() < deadline)
                            esp_rom_delay_us(50);
                        if (!ack_gate_replied(&s_ack_gate)) {
                            ++s_reply_timeouts;
                            if (app_sent) {
                                tx_result = PICTOCHAT_TX_NO_REPLY;
                                // Keep advancing WM sequence numbers on retries,
                                // as the reference does for each radio poll.
                                app_sent = false;
                            }
                            ack_gate_claim(&s_ack_gate);
                        }
                    } else {
                        esp_rom_delay_us(HOST_ACK_DELAY_US);
                        if (ack_gate_claim(&s_ack_gate)) host_send_ack();
                    }
                } else {
                    ack_gate_claim(&s_ack_gate);
                }
            }
            portENTER_CRITICAL(&s_admission_lock);
            s_cycle_open = false;
            portEXIT_CRITICAL(&s_admission_lock);
            bool finished = pictochat_room_finish(s_room, tx_result);
            configASSERT(finished);
            (void)finished;
            if (app_sent && s_cmd_submit_ok && s_cmd_completed_ok)
                ESP_LOGI(TAG, "HOST APP: aid=%u type=%u slot=%u bytes=%u identity_phase=%u rx_drops=%u",
                         peer->aid, outgoing.bytes[0], outgoing.bytes[4], outgoing.len, peer->session.identity.phase,
                         atomic_load_explicit(&s_host_app_drops, memory_order_relaxed));
            vTaskDelay(pdMS_TO_TICKS(HOST_CMD_INTERVAL_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));            // idle: SoftAP keeps beaconing
        }

        if (now >= next_stat) {                        // ~1s status
            host_log_status(clients, rx_rejected, last_cmds);
            last_cmds = s_cmds_tx;
            next_stat = now + 1000000;
        }
    }
}
#endif

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

#if SNIFFER_MODE == MODE_STREAM
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = CAPTURE_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    if (strlen(AP_PASS) >= 8) {
        strlcpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
#elif SNIFFER_MODE == MODE_JOIN
    // Join: STA interface for injection. Take the joiner's MAC so the hardware
    // auto-ACKs the host's unicast auth/assoc-req at SIFS.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, JOIN_SELF_MAC));
#elif SNIFFER_MODE == MODE_HOST
    // Host: real SoftAP so the HARDWARE beacon engine radiates a regular 100 TU beacon
    // with a genuine HW-maintained TSF (the whole point of the pivot). We take our BSSID
    // as the AP MAC, hide the SSID (ssid_hidden=1 -> 2-byte zero-length SSID element that
    // the ieee80211_add_dsparams override then deletes), and run OPEN auth so the MLME
    // accepts the DS's open-system Auth/Assoc natively. Only the MP CMD/ACK layer is
    // raw-injected on top (host_task).
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
#if PICTOCHAT_ONLINE
    online_wifi_configure();
    HOST_SELF_MAC[5]=(uint8_t)(0xd0+online_node());
    k_pictochat_vendor_ie[HOST_VIE_USERS_OFF-1]=(uint8_t)(online_node()-1);
#endif
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_AP, HOST_SELF_MAC));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, host_wifi_evt, NULL, NULL));
    wifi_config_t ap = {0};
    // CRITICAL: the DS's PictoChat Assoc-Req carries a 32-byte "SSID" =
    // game_id(4)=0 ‖ stream_code(2 LE) ‖ 26 zeros. The SoftAP MLME only accepts an
    // Assoc-Req whose SSID matches ours, so our AP SSID must BE that exact 32-byte value
    // (else the MLME logs "removing station after unsuccessful auth/assoc" and never
    // fires STACONNECTED). ssid_hidden=1 still makes the BEACON's SSID element
    // zero-length (so the ieee80211_add_dsparams surgery math holds and the DS sees no
    // SSID), while ssid_len=32 governs assoc-req matching. stream_code must equal the
    // value our beacon vendor IE advertises (0x0300 -> bytes 00 03).
    memset(ap.ap.ssid, 0, sizeof(ap.ap.ssid));
    ap.ap.ssid[4]        = 0x00;              // stream_code low  (matches vendor IE)
    ap.ap.ssid[5]        = 0x03;              // stream_code high
    ap.ap.ssid_len       = 32;
    ap.ap.ssid_hidden    = 1;                 // beacon SSID element stays zero-length
#if PICTOCHAT_USB || PICTOCHAT_BLE
    ap.ap.channel        = online_channel();
#else
    ap.ap.channel        = CAPTURE_CHANNEL;
#endif
    ap.ap.max_connection = HOST_RADIO_CLIENTS;
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.beacon_interval = 100;              // 100 TU — HW beacon timer + real TSF
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
#else
    // Discovery: no link needed. NULL mode + promiscuous lets us hop freely.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

#if SNIFFER_MODE == MODE_HOST
    ESP_ERROR_CHECK(esp_wifi_register_80211_tx_cb(host_tx_done));
#endif

#if SNIFFER_MODE == MODE_SERIAL_MGMT
    // RX timestamps are precise only without modem/light sleep (ESP-IDF contract).
    // CONFIG_PM_ENABLE is off, but modem sleep defaults to WIFI_PS_MIN_MODEM
    // independently. Disable it only for this passive USB timing diagnostic.
    wifi_ps_type_t ps_before, ps_after;
    ESP_ERROR_CHECK(esp_wifi_get_ps(&ps_before));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_get_ps(&ps_after));
    ESP_ERROR_CHECK(ps_after == WIFI_PS_NONE ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "SERIAL_MGMT RX clock: power_save=%d -> %d (WIFI_PS_NONE=%d); verify on-air trace",
             (int)ps_before, (int)ps_after, (int)WIFI_PS_NONE);
#endif

    // Enable promiscuous capture, management + data frames only.
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
                       WIFI_PROMIS_FILTER_MASK_CTRL,  // also see CF-Poll/PS-Poll/ACK
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

#if SNIFFER_MODE == MODE_STREAM || SNIFFER_MODE == MODE_JOIN || SNIFFER_MODE == MODE_HOST || SNIFFER_MODE == MODE_SERIAL_MGMT
#if PICTOCHAT_USB || PICTOCHAT_BLE
    ESP_ERROR_CHECK(esp_wifi_set_channel(online_channel(), WIFI_SECOND_CHAN_NONE));
#elif !PICTOCHAT_ONLINE
    ESP_ERROR_CHECK(esp_wifi_set_channel(CAPTURE_CHANNEL, WIFI_SECOND_CHAN_NONE));
#endif
#endif
#if SNIFFER_MODE == MODE_JOIN
    // The DS speaks 802.11b at 1-2 Mbps; send our injected frames at 1 Mbps.
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
#endif
#if SNIFFER_MODE == MODE_HOST
    // DS radios are 802.11b DSSS only — beacon/frames must be 11b or the DS can't
    // demodulate us. Keep long preambles in the raw-TX rate experiment; an earlier
    // short-preamble experiment lost room visibility. Check visibility again.
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B));
    // This C6 driver rejects both raw-rate APIs before start (ESP_FAIL).
    // Its linked implementation requires an allocated AP interface. Configure
    // after start and verify the effective rate with TX completion and RX logs.
    wifi_tx_rate_config_t raw_rate = {
        .phymode = WIFI_PHY_MODE_11B, .rate = WIFI_PHY_RATE_2M_L,
    };
    ESP_ERROR_CHECK(esp_wifi_config_80211_tx(WIFI_IF_AP, &raw_rate));
    ESP_LOGI(TAG, "HOST raw TX configured: 2 Mbps long preamble; verify completion/RX rate");
    // Inject the 0xDD PictoChat room-advert IE into the SoftAP beacon (unregister first
    // so a re-register can't fail), then arm the SSID-delete beacon surgery.
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0,
                           k_pictochat_vendor_ie);
#if !PICTOCHAT_USB
    ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0,
                                           k_pictochat_vendor_ie));
    s_host_beaconing = true;
#endif
#if PICTOCHAT_GHOST_DEMO
    host_set_beacon_users(2);
#endif
#endif
}

void app_main(void) {
#if SNIFFER_MODE == MODE_HOST && CONFIG_IDF_TARGET_ESP32
    s_room = heap_caps_calloc(1, sizeof(*s_room), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_room ? ESP_OK : ESP_ERR_NO_MEM);
#endif
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

#if SNIFFER_MODE == MODE_HOST
    s_cmd_completed = xSemaphoreCreateBinary();
    configASSERT(s_cmd_completed);
    s_host_trace_queue = xQueueCreate(64, sizeof(host_tx_trace_t));
    configASSERT(s_host_trace_queue);
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) {
        s_host_app_queue[i] = xQueueCreate(16, sizeof(host_app_rx_t));
        configASSERT(s_host_app_queue[i]);
    }
    s_drawing_ready = xQueueCreate(PICTOCHAT_ROOM_CLIENTS, sizeof(host_drawing_t *));
    s_drawing_dump = xQueueCreate(2, sizeof(host_drawing_t *));
    configASSERT(s_drawing_ready && s_drawing_dump);
    xTaskCreate(host_drawing_dump_task, "draw_dump", 3072, NULL, 2, NULL);
    xTaskCreate(host_trace_task, "tx_trace", 3072, NULL, 2, NULL);
#endif

#if SNIFFER_MODE == MODE_STREAM || SNIFFER_MODE == MODE_SERIAL_MGMT
    s_cap_queue = xQueueCreate(CAP_QUEUE_LEN, sizeof(cap_item_t));
    configASSERT(s_cap_queue);
#endif

    wifi_init();

#if SNIFFER_MODE == MODE_STREAM
    ESP_LOGI(TAG, "STREAM mode: SoftAP '%s' on channel %d. Join it, then run "
                  "tools/udp_to_wireshark.py on port %d.",
             AP_SSID, CAPTURE_CHANNEL, UDP_PORT);
    xTaskCreate(sender_task, "sender", 4096, NULL, 5, NULL);
#elif SNIFFER_MODE == MODE_SERIAL_MGMT
    ESP_LOGI(TAG, "SERIAL_MGMT mode: channel %d, handshake frames over USB; no WiFi link needed.",
             CAPTURE_CHANNEL);
    ESP_LOGI(TAG, "SERIAL_MGMT trace host=%02x:%02x:%02x:%02x:%02x:%02x (first %u MP frames per association)",
             SERIAL_TRACE_HOST[0], SERIAL_TRACE_HOST[1], SERIAL_TRACE_HOST[2],
             SERIAL_TRACE_HOST[3], SERIAL_TRACE_HOST[4], SERIAL_TRACE_HOST[5],
             (unsigned)MP_TRACE_FRAMES);
    xTaskCreate(serial_mgmt_task, "serial_mgmt", 4096, NULL, 5, NULL);
#elif SNIFFER_MODE == MODE_JOIN
    ESP_LOGI(TAG, "JOIN mode: active client %02X:%02X:%02X:%02X:%02X:%02X on "
                  "channel %d. Turn the real joiner DS OFF. Latching a beacon, "
                  "then sending Auth-Req -> Assoc-Req to the host.",
             JOIN_SELF_MAC[0], JOIN_SELF_MAC[1], JOIN_SELF_MAC[2],
             JOIN_SELF_MAC[3], JOIN_SELF_MAC[4], JOIN_SELF_MAC[5], CAPTURE_CHANNEL);
    xTaskCreate(join_task, "join", 4096, NULL, 5, NULL);
    xTaskCreate(watchdog_task, "watchdog", 3072, NULL, 6, NULL);
#elif SNIFFER_MODE == MODE_HOST
    ESP_LOGI(TAG, "HOST mode: hosting a PictoChat room (BSSID "
                  "%02X:%02X:%02X:%02X:%02X:%02X, room %c) on channel %d. Beaconing; "
                  "put a real DS into PictoChat and look for the room.",
             HOST_SELF_MAC[0], HOST_SELF_MAC[1], HOST_SELF_MAC[2], HOST_SELF_MAC[3],
             HOST_SELF_MAC[4], HOST_SELF_MAC[5], 'A' + HOST_CHATROOM,
#if PICTOCHAT_ONLINE
             online_channel());
#else
             CAPTURE_CHANNEL);
#endif
#if PICTOCHAT_ONLINE
    online_start();
#endif
    xTaskCreate(host_task, "host", 6144, NULL, 5, NULL);
#else
    ESP_LOGI(TAG, "DISCOVERY mode: hopping channels 1..13, printing Nintendo "
                  "frames over serial. Note the channel your DSs use.");
    xTaskCreate(hopper_task, "hopper", 2048, NULL, 4, NULL);
#endif

    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 3, NULL);
#if SNIFFER_MODE == MODE_HOST
    host_log_heap("startup");
#endif
}
