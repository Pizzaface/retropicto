#pragma once
#include "host_message.h"
#include "host_sequence.h"

/* Single-client, single-owner protocol engine: no clocks, allocation or I/O.
 * Feed application bytes without WM framing. Resolve each prepared output with
 * finish() before receiving or queuing more data. Keep state off MCU stacks. */
typedef enum {
    PICTOCHAT_TX_FAILED, /* submission or driver completion failed */
    PICTOCHAT_TX_NO_REPLY, /* transmitted; granted client did not reply */
    PICTOCHAT_TX_DELIVERED /* completed and any required reply arrived */
} pictochat_tx_result_t;

typedef struct {
    host_frame_t kind;
    bool application, drawing;
    uint16_t sequence;
    host_id_packet_t packet;
} pictochat_output_t;

typedef struct {
    host_sequence_t sequence;
    host_identity_t identity;
    host_message_rx_t received;
    host_message_tx_t sending;
    uint8_t profile[84];
    uint16_t app_sequence[2];
    bool outstanding;
    pictochat_output_t output;
    host_sequence_t saved_sequence;
    host_identity_t saved_identity;
    host_message_cursor_t saved_cursor;
    uint16_t saved_app_sequence[2];
} pictochat_session_t;

void pictochat_session_reset(pictochat_session_t *s, const uint8_t profile[84], uint32_t token0,
                             uint32_t token1);
/* reset defaults to member slot 1. Before receiving/preparing any traffic, a
 * multi-client owner sets identity.client_slot to the client's roster/AID slot
 * (1..15). The room wrapper does this on join. Slot 0 always denotes the host. */
/* -2: busy (retry later), -1: rejected, 0: accepted, 1: complete drawing.
 * Odd-sized applications are rejected: this host uses word-sized WM packets.
 * Completed bytes remain in received until the next accepted announcement. */
int pictochat_session_receive(pictochat_session_t *s, const uint8_t *app, size_t len);
bool pictochat_session_reply(pictochat_session_t *s, const uint8_t announcement[20],
                             const uint8_t *body, size_t len, const uint8_t mac[6], uint32_t token);
bool pictochat_session_prepare(pictochat_session_t *s, bool admitted, pictochat_output_t *out);
bool pictochat_session_finish(pictochat_session_t *s, pictochat_tx_result_t result);
