#pragma once
#include <stdbool.h>
#include <stdatomic.h>

// The host task opens each cycle before submitting its CMD. Both the RX callback
// and fallback must claim ACK ownership BEFORE enqueueing a transmission.
// This prevents duplicate submission, not late-reply attribution or TX failure.
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "ACK claims must be lock-free in WiFi callback");
typedef struct { atomic_uint claimed, replied; } ack_gate_t;

static inline void ack_gate_begin(ack_gate_t *gate) {
    atomic_store_explicit(&gate->replied, 0, memory_order_release);
    atomic_store_explicit(&gate->claimed, 0, memory_order_release);
}

static inline bool ack_gate_claim(ack_gate_t *gate) {
    return atomic_exchange_explicit(&gate->claimed, 1, memory_order_acq_rel) == 0;
}

static inline bool ack_gate_reply(ack_gate_t *gate) {
    atomic_store_explicit(&gate->replied, 1, memory_order_release);
    return ack_gate_claim(gate);
}

static inline bool ack_gate_replied(const ack_gate_t *gate) {
    return atomic_load_explicit(&gate->replied, memory_order_acquire) != 0;
}
