#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Keep only management handshakes; full-rate MP traffic exceeds 115200-baud USB UART.
static inline bool is_handshake_frame(const uint8_t *frame, size_t len) {
    if (len < 24 || (frame[0] & 0x0f) != 0)
        return false;
    switch (frame[0] >> 4) {
    case 0:
    case 1: // association request/response
    case 2:
    case 3: // reassociation request/response
    case 10:
    case 11:
    case 12: // disassociation, authentication, deauthentication
        return true;
    default:
        return false;
    }
}
