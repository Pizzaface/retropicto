#pragma once
#include <stdbool.h>
#include "mp_reply.h"

// Recognize the complete type-6 shape observed in real-DS admission captures.
// len includes the driver's four-byte trailer; it is never application data.
// This deliberately accepts only the observed 108-byte payload / size 0x0068.
static inline bool host_admission_reply(const uint8_t *f, size_t len, const uint8_t host[6],
                                        const uint8_t client[6]) {
    if (mp_reply_payload_bytes(f, len, host) != 108)
        return false;
    return memcmp(f + 10, client, 6) == 0 && f[26] == 6 && f[27] == 0 && f[28] == 0x68 &&
           f[29] == 0;
}
