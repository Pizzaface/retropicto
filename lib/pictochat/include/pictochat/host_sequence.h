#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HOST_FRAME_EMPTY, HOST_FRAME_ROSTER, HOST_FRAME_HEARTBEAT, HOST_FRAME_SESSION
} host_frame_t;

typedef struct {
    bool empty_sent;
    bool admitted;
    uint8_t roster_left;
} host_sequence_t;

static inline void host_sequence_reset(host_sequence_t *seq) {
    *seq = (host_sequence_t){0};
}

// Exactly one CMD per reply/ACK cycle. No identity transfer before admission.
// Seven roster polls match the fresh real-DS sample, not a proven protocol rule.
static inline host_frame_t host_sequence_next(host_sequence_t *seq, bool admitted) {
    if (!seq->empty_sent) {
        seq->empty_sent = true;
        return HOST_FRAME_EMPTY;
    }
    if (!seq->admitted) {
        if (!admitted) return HOST_FRAME_HEARTBEAT;
        seq->admitted = true;
        seq->roster_left = 7;
    }
    if (seq->roster_left) {
        --seq->roster_left;
        return HOST_FRAME_ROSTER;
    }
    return HOST_FRAME_SESSION; // application state decides what to send next
}
