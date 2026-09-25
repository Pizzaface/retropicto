#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* LAN PoC framing. Integers little endian; checksum detects corruption, not
 * authentication. Never expose this unauthenticated service to the internet. */
#define RELAY_HEADER 16u
#define RELAY_MAX_PAYLOAD (28u + 10276u)
enum { RELAY_STATE = 1, RELAY_DRAWING = 2, RELAY_ACK = 3 };
static inline uint32_t relay_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void relay_put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i));
}
static inline uint32_t relay_hash(const uint8_t *p, size_t n) {
    uint32_t h=2166136261u;
    for (size_t i=0; i<n; ++i) h=(h^p[i])*16777619u;
    return h;
}
static inline bool relay_length(unsigned kind, size_t n) {
    return (kind==RELAY_STATE && n==92) || (kind==RELAY_ACK && n==8) ||
        (kind==RELAY_DRAWING && n>=28+1060 && n<=RELAY_MAX_PAYLOAD && (n-28-36)%1024==0);
}
static inline bool relay_header(uint8_t out[RELAY_HEADER], unsigned kind,
                                uint32_t seq, const uint8_t *payload, size_t n) {
    if (!out || !payload || !relay_length(kind,n)) return false;
    memcpy(out,"PCTR",4); out[4]=1; out[5]=(uint8_t)kind;
    out[6]=(uint8_t)n; out[7]=(uint8_t)(n>>8);
    relay_put32(out+8,seq); relay_put32(out+12,relay_hash(payload,n));
    return true;
}
static inline bool relay_parse_header(const uint8_t h[RELAY_HEADER],
                                      unsigned *kind, uint32_t *seq, size_t *n) {
    if (!h || !kind || !seq || !n || memcmp(h,"PCTR",4) || h[4]!=1) return false;
    unsigned k=h[5]; size_t len=h[6] | ((size_t)h[7]<<8);
    if (!relay_length(k,len)) return false;
    *kind=k; *seq=relay_get32(h+8); *n=len;
    return true;
}
