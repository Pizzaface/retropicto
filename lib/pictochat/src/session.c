#include "pictochat/session.h"

void pictochat_session_reset(pictochat_session_t *s, const uint8_t profile[84], uint32_t token0,
                             uint32_t token1) {
    /* Allow reusing s->profile on a new association. */
    uint8_t copy[84];
    memcpy(copy, profile, sizeof(copy));
    memset(s, 0, sizeof(*s));
    memcpy(s->profile, copy, sizeof(copy));
    host_identity_reset(&s->identity, token0, token1);
}

int pictochat_session_receive(pictochat_session_t *s, const uint8_t *app, size_t len) {
    if (s->outstanding || s->identity.pending)
        return -2;
    if (!app || (len & 1) || !host_identity_receive(&s->identity, app, len))
        return -1;
    return host_message_receive_slot(&s->received, app, len, s->identity.client_slot);
}

bool pictochat_session_reply(pictochat_session_t *s, const uint8_t announcement[20],
                             const uint8_t *body, size_t len, const uint8_t mac[6],
                             uint32_t token) {
    if (s->outstanding || s->sending.cursor.active || s->identity.phase != HOST_ID_READY ||
        !announcement || !body || !mac || len <= HOST_MESSAGE_HEADER || len > HOST_MESSAGE_MAX ||
        (len - HOST_MESSAGE_HEADER) % 1024 || body[0] != 3 || body[1] != 2 ||
        host_message_u16(announcement) != 0 || host_message_u16(announcement + 2) != 20 ||
        announcement[4] < 1 || announcement[4] > 15 || host_message_u16(announcement + 8) != len ||
        announcement[10] || announcement[11])
        return false;
    host_message_reply(&s->sending, announcement, body, (uint16_t)len, mac, token);
    return true;
}

bool pictochat_session_prepare(pictochat_session_t *s, bool admitted, pictochat_output_t *out) {
    if (s->outstanding || !out)
        return false;
    s->saved_sequence = s->sequence;
    s->saved_identity = s->identity;
    s->saved_cursor = s->sending.cursor;
    memcpy(s->saved_app_sequence, s->app_sequence, sizeof(s->app_sequence));
    s->output = (pictochat_output_t){.kind = host_sequence_next(&s->sequence, admitted)};
    if (s->output.kind == HOST_FRAME_SESSION) {
        s->output.application = host_identity_next(&s->identity, s->profile, &s->output.packet);
        if (!s->output.application && s->identity.phase == HOST_ID_READY) {
            s->output.drawing = host_message_next(&s->sending, &s->output.packet);
            s->output.application = s->output.drawing;
        }
        if (s->output.application) {
            unsigned port = s->output.packet.bytes[0] == 2;
            s->output.sequence = s->app_sequence[port]++;
        } else
            s->output.kind = HOST_FRAME_HEARTBEAT;
    }
    s->outstanding = true;
    *out = s->output;
    return true;
}

bool pictochat_session_finish(pictochat_session_t *s, pictochat_tx_result_t result) {
    if (!s->outstanding || result < PICTOCHAT_TX_FAILED || result > PICTOCHAT_TX_DELIVERED)
        return false;
    if (result == PICTOCHAT_TX_FAILED) {
        s->sequence = s->saved_sequence;
        memcpy(s->app_sequence, s->saved_app_sequence, sizeof(s->app_sequence));
    }
    if (result == PICTOCHAT_TX_FAILED ||
        (result == PICTOCHAT_TX_NO_REPLY && s->output.application)) {
        s->identity = s->saved_identity;
        s->sending.cursor = s->saved_cursor;
    }
    s->outstanding = false;
    return true;
}
