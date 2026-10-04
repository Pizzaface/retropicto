#pragma once
#include <stddef.h>
#include <stdint.h>

/* Room events describe committed state, never tentative prepare() output. */
typedef enum {
    PICTOCHAT_PEER_JOINED,
    PICTOCHAT_PEER_LEFT,
    PICTOCHAT_PEER_READY,
    PICTOCHAT_MESSAGE_RECEIVED,
    PICTOCHAT_MESSAGE_SENT
} pictochat_event_type_t;

typedef struct {
    pictochat_event_type_t type;
    unsigned peer_slot; /* storage slot, distinct from Wi-Fi AID */
    uint8_t aid, mac[6]; /* source for RECEIVED; recipient for SENT */
    uint32_t generation;
    uint8_t sender_slot; /* original author: host=0, client=AID */
    uint32_t token; /* little-endian announcement token */
    const uint8_t *announcement; /* 20 bytes for message events, otherwise NULL */
    const uint8_t *body; /* borrowed complete message, otherwise NULL */
    size_t length;
} pictochat_event_t;

/* Synchronous, on the room owner's thread after state is committed. Keep work
 * bounded; copy message bytes before returning if a worker needs them later.
 * Do not reset/mutate the room from a handler. Read-only recipient snapshots
 * are allowed. SENT fires per recipient after the final copy is delivered,
 * not when queued, and does not prove the console displayed the message. */
typedef void (*pictochat_event_handler_t)(void *context, const pictochat_event_t *event);
