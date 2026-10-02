#ifndef RETROPICTO_CAPTURE_PACKET_H
#define RETROPICTO_CAPTURE_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "firmware_config.h"

// ---- 802.11 header field offsets (little bits of layout we rely on) ----
#define WLAN_FC_OFFSET 0 // frame control (2 bytes)
#define WLAN_ADDR1_OFF 4 // receiver / destination
#define WLAN_ADDR2_OFF 10 // transmitter / source
#define WLAN_ADDR3_OFF 16 // BSSID (usually)

// Frame Control decode helpers.
static inline uint8_t fc_type(const uint8_t *f) { return (f[0] >> 2) & 0x3; }
static inline uint8_t fc_subtype(const uint8_t *f) { return (f[0] >> 4) & 0xF; }
static inline bool fc_to_ds(const uint8_t *f) { return f[1] & 0x01; }
static inline bool fc_from_ds(const uint8_t *f) { return f[1] & 0x02; }

// Length of the 802.11 MAC header for a given frame (handles addr4 + QoS).
static size_t wlan_header_len(const uint8_t *f, size_t len) {
    if (len < 24) return len;
    size_t hdr = 24;
    uint8_t type = fc_type(f);
    if (type == 2 /* data */ && fc_to_ds(f) && fc_from_ds(f)) {
        hdr += 6; // addr4 present
    }
    // QoS data subtypes have the QoS control field (2 bytes).
    if (type == 2 && (fc_subtype(f) & 0x08)) {
        hdr += 2;
    }
    return hdr > len ? len : hdr;
}

// ---- Minimal radiotap header (version 0) ----
// Present fields, in bit order: FLAGS(1), CHANNEL(3), DBM_ANTSIGNAL(5).
// Layout: [ver][pad][len:2][present:4][flags:1][pad:1][chanfreq:2][chanflags:2][antsignal:1]
#define RT_PRESENT ((1u << 1) | (1u << 3) | (1u << 5))
#define RT_LEN 15

static size_t build_radiotap(uint8_t *out, uint8_t channel, int8_t rssi) {
    memset(out, 0, RT_LEN);
    out[0] = 0; // it_version
    out[1] = 0; // it_pad
    out[2] = RT_LEN & 0xFF; // it_len (LE)
    out[3] = (RT_LEN >> 8) & 0xFF;
    out[4] = RT_PRESENT & 0xFF; // it_present (LE)
    out[5] = (RT_PRESENT >> 8) & 0xFF;
    out[6] = (RT_PRESENT >> 16) & 0xFF;
    out[7] = (RT_PRESENT >> 24) & 0xFF;

    // FLAGS (offset 8): bit4 (0x10) = FCS present at end of frame.
    out[8] = FCS_AT_END ? 0x10 : 0x00;
    // out[9] is alignment padding for the u16 channel field.

    // CHANNEL (offset 10): frequency in MHz, then channel flags.
    uint16_t freq = (channel == 14) ? 2484 : (2407 + channel * 5);
    out[10] = freq & 0xFF;
    out[11] = (freq >> 8) & 0xFF;
    uint16_t chflags = 0x0080; // 2 GHz spectrum
    out[12] = chflags & 0xFF;
    out[13] = (chflags >> 8) & 0xFF;

    // DBM_ANTSIGNAL (offset 14): signed dBm.
    out[14] = (uint8_t)rssi;
    return RT_LEN;
}

#endif // RETROPICTO_CAPTURE_PACKET_H
