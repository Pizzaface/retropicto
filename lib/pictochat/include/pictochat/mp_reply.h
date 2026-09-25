#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// DS Data+CF-Ack / CF-Ack with our host as receiver and the MP reply group in
// addr3. Driver-reported length includes a four-byte capture trailer, which
// must NOT be interpreted as app data (or an alleged session key).
static inline int mp_reply_payload_bytes(const uint8_t *f, size_t len,
                                         const uint8_t host[6]) {
    static const uint8_t reply_group[6] = {3,9,0xbf,0,0,0x10};
    if (len < 28 || (f[0] != 0x18 && f[0] != 0x58) ||
        (f[1] & 3) != 1 || memcmp(f + 4, host, 6) != 0 ||
        memcmp(f + 16, reply_group, 6) != 0) return -1;
    return (int)(len - 28);
}
