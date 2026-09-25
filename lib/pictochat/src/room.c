#include "pictochat/room.h"

static pictochat_event_t peer_event(const pictochat_room_t *r, unsigned slot,
                                   pictochat_event_type_t type) {
    const pictochat_peer_t *p = &r->peers[slot];
    pictochat_event_t event = {.type = type, .peer_slot = slot,
        .aid = p->aid, .generation = p->generation};
    memcpy(event.mac, p->mac, sizeof(event.mac));
    return event;
}
static void dispatch(pictochat_room_t *r, const pictochat_event_t *event) {
    if (!r->handler) return;
    r->dispatching = true;
    r->handler(r->handler_context, event);
    r->dispatching = false;
}
static void message_event(pictochat_room_t *r, unsigned slot, pictochat_event_type_t type,
                           const uint8_t announcement[20], const uint8_t *body, size_t len) {
    pictochat_event_t event = peer_event(r, slot, type);
    event.announcement = announcement; event.body = body; event.length = len;
    event.sender_slot = announcement[4];
    for (unsigned i = 0; i < 4; ++i) event.token |= (uint32_t)announcement[16 + i] << (8 * i);
    dispatch(r, &event);
}
bool pictochat_room_set_handler(pictochat_room_t *r, pictochat_event_handler_t handler,
                                void *context) {
    if (r->outstanding || r->dispatching) return false;
    r->handler = handler; r->handler_context = context;
    return true;
}

void pictochat_room_reset(pictochat_room_t *r, const uint8_t profile[84]) {
    uint8_t copy[84];
    memcpy(copy, profile, sizeof(copy));
    memset(r, 0, sizeof(*r));
    memcpy(r->profile, copy, sizeof(copy));
}
bool pictochat_room_leave(pictochat_room_t *r, unsigned slot) {
    if (r->outstanding || r->dispatching || slot >= PICTOCHAT_ROOM_CLIENTS) return false;
    bool connected = r->peers[slot].connected;
    pictochat_event_t event = peer_event(r, slot, PICTOCHAT_PEER_LEFT);
    memset(&r->peers[slot], 0, sizeof(r->peers[slot]));
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) {
        pictochat_peer_t *p = &r->peers[i];
        p->drawing_targets &= ~(1u << slot);
        memset(p->seen[slot], 0, sizeof(p->seen[slot]));
        if (p->replay_active && p->replay_source == slot) p->replay_active = false;
        if (p->connected) p->session.sequence.roster_left = 7;
    }
    if (connected) dispatch(r, &event);
    return true;
}
static bool join_member(pictochat_room_t *r, unsigned slot, const uint8_t mac[6],
    unsigned aid, uint32_t generation, uint32_t token0, uint32_t token1,
    const uint8_t *ghost_profile) {
    if (r->outstanding || r->dispatching || slot >= PICTOCHAT_ROOM_CLIENTS || !mac ||
        aid < 1 || aid > 15 || !generation) return false;
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i)
        if (i != slot && r->peers[i].connected &&
            (r->peers[i].aid == aid || !memcmp(r->peers[i].mac, mac, 6))) return false;
    pictochat_peer_t *p = &r->peers[slot];
    if (p->connected && p->generation == generation && p->aid == aid &&
        !memcmp(p->mac, mac, 6)) return p->ghost == (ghost_profile != NULL);
    pictochat_room_leave(r, slot);
    p->connected = true; p->aid = (uint8_t)aid; p->generation = generation;
    memcpy(p->mac, mac, 6);
    pictochat_session_reset(&p->session, r->profile, token0, token1);
    p->session.identity.client_slot = p->aid;
    if (ghost_profile) {
        p->ghost = p->admitted = true;
        p->session.identity.phase = HOST_ID_READY;
        for (unsigned stage = 0; stage < 2; ++stage) {
            uint8_t *a = p->identity_announcements[stage];
            a[0] = 0; a[2] = 20; a[4] = p->aid;
            a[6] = a[7] = 0xff; a[8] = 84;
            // Captured client descriptor; virtual-member behavior needs RF validation.
            a[12] = 0x29; a[14] = 0x69;
            uint32_t token = stage ? token1 : token0;
            for (unsigned j = 0; j < 4; ++j) a[16+j] = (uint8_t)(token >> (8*j));
            host_id_packet_t *packet = &p->identities[stage];
            packet->len = 96;
            packet->bytes[0] = 2; packet->bytes[2] = 96;
            packet->bytes[4] = p->aid; packet->bytes[6] = 84; packet->bytes[7] = 1;
            memcpy(packet->bytes + 12, ghost_profile, 84);
            packet->bytes[13] = (uint8_t)stage;
            p->versions[stage] = 1;
        }
    }
    pictochat_event_t event = peer_event(r, slot, PICTOCHAT_PEER_JOINED);
    dispatch(r, &event);
    return true;
}
bool pictochat_room_join(pictochat_room_t *r, unsigned slot, const uint8_t mac[6],
    unsigned aid, uint32_t generation, uint32_t token0, uint32_t token1) {
    return join_member(r, slot, mac, aid, generation, token0, token1, NULL);
}
bool pictochat_room_ghost_join(pictochat_room_t *r, unsigned slot, unsigned aid,
    uint32_t generation, const uint8_t profile[84], uint32_t token0, uint32_t token1) {
    if (!profile || profile[0] != 3 || profile[1] > 1) return false;
    uint8_t copy[84], mac[6];
    memcpy(copy, profile, 84);
    for (unsigned i = 0; i < 6; ++i) mac[i] = copy[2 + (i ^ 1)];
    return join_member(r, slot, mac, aid, generation, token0, token1, copy);
}
int pictochat_room_ghost_send(pictochat_room_t *r, unsigned slot, uint32_t generation,
    const uint8_t announcement[20], const uint8_t *body, size_t len, uint32_t token) {
    if (slot >= PICTOCHAT_ROOM_CLIENTS) return -1;
    pictochat_peer_t *p = &r->peers[slot];
    if (!p->connected || !p->ghost || p->generation != generation) return -1;
    if (r->outstanding || r->dispatching || p->drawing_targets) return -2;
    // Reuse checked body/announcement validation, then retain a source copy
    // for independent per-recipient forwarding. Never schedule the ghost itself.
    if (!pictochat_session_reply(&p->session, announcement, body, len, p->mac, token)) return -1;
    host_message_tx_t *tx = &p->session.sending;
    host_message_rx_t *rx = &p->session.received;
    memcpy(rx->announcement, tx->announcement, 20);
    rx->announcement[4] = p->aid;
    memcpy(rx->body, tx->body, len);
    rx->total = (uint16_t)len;
    tx->cursor.active = false;
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i)
        if (r->peers[i].connected && r->peers[i].admitted && !r->peers[i].ghost)
            p->drawing_targets |= 1u << i;
    return 0;
}
int pictochat_room_receive(pictochat_room_t *r, unsigned slot, uint32_t generation,
                           const uint8_t *app, size_t len) {
    if (slot >= PICTOCHAT_ROOM_CLIENTS || !app || len < 4) return -1;
    pictochat_peer_t *p = &r->peers[slot];
    if (!p->connected || !p->admitted || p->ghost || p->generation != generation) return -1;
    if (r->outstanding || r->dispatching || p->drawing_targets) return -2;
    int result = pictochat_session_receive(&p->session, app, len);
    if (result < 0) return result;
    if (app[0] == 0 && len == 20) memcpy(p->announcement, app, 20);
    if (app[0] == 2 && len == 96 && p->session.identity.relay_identity) {
        unsigned stage = app[13];
        if (p->identities[stage].len != len || memcmp(p->identities[stage].bytes, app, len)) {
            p->identities[stage] = p->session.identity.relay;
            memcpy(p->identity_announcements[stage], p->announcement, 20);
            if (++p->versions[stage] == 0) ++p->versions[stage];
        }
    }
    if (result == 1) {
        for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i)
            if (i != slot && r->peers[i].connected && r->peers[i].admitted && !r->peers[i].ghost)
                p->drawing_targets |= 1u << i;
        message_event(r, slot, PICTOCHAT_MESSAGE_RECEIVED, p->session.received.announcement,
                      p->session.received.body, p->session.received.total);
    }
    return result;
}
static bool identities_current(const pictochat_peer_t *dest, const pictochat_peer_t *src,
                               unsigned source) {
    return src->versions[0] && src->versions[1] &&
           dest->seen[source][0] == src->versions[0] && dest->seen[source][1] == src->versions[1];
}
static void forward_drawing(pictochat_room_t *r, unsigned dest) {
    pictochat_peer_t *p = &r->peers[dest];
    if (p->session.sending.cursor.active || p->replay_active ||
        p->session.identity.phase != HOST_ID_READY) return;
    for (unsigned n = 0; n < PICTOCHAT_ROOM_CLIENTS; ++n) {
        unsigned i = (p->drawing_next + n) % PICTOCHAT_ROOM_CLIENTS;
        pictochat_peer_t *src = &r->peers[i];
        if (!(src->drawing_targets & (1u << dest)) || !identities_current(p, src, i)) continue;
        host_message_rx_t *rx = &src->session.received;
        /* Keep the source's member slot as well as its body and token.
         * Slot zero identifies the host, not an arbitrary outbound channel. */
        host_message_tx_t *tx = &p->session.sending;
        tx->total = rx->total;
        memcpy(tx->announcement, rx->announcement, 20);
        tx->announcement[0] = 1;
        memcpy(tx->body, rx->body, rx->total);
        tx->cursor = (host_message_cursor_t){.active = true};
        src->drawing_targets &= ~(1u << dest);
        p->drawing_next = (i + 1) % PICTOCHAT_ROOM_CLIENTS;
        break;
    }
}
static void start_replay(pictochat_room_t *r, unsigned dest) {
    pictochat_peer_t *p = &r->peers[dest];
    if (p->replay_active || p->session.sending.cursor.active ||
        p->session.identity.phase != HOST_ID_READY) return;
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) {
        pictochat_peer_t *src = &r->peers[i];
        if (i == dest || !src->connected) continue;
        for (unsigned stage = 0; stage < 2; ++stage) {
            if (!src->versions[stage] || p->seen[i][stage] == src->versions[stage]) continue;
            p->replay_active = true; p->replay_data = false;
            p->replay_source = i; p->replay_stage = stage;
            p->replay_version = src->versions[stage];
            p->replay[0].len = 20;
            memcpy(p->replay[0].bytes, src->identity_announcements[stage], 20);
            p->replay[0].bytes[0] = 1;
            p->replay[1] = src->identities[stage];
            return;
        }
    }
}
bool pictochat_room_prepare(pictochat_room_t *r, unsigned *slot, pictochat_output_t *out) {
    if (r->outstanding || r->dispatching || !slot || !out) return false;
    for (unsigned n = 0; n < PICTOCHAT_ROOM_CLIENTS; ++n) {
        unsigned i = (r->next + n) % PICTOCHAT_ROOM_CLIENTS;
        pictochat_peer_t *p = &r->peers[i];
        if (!p->connected || p->ghost) continue;
        start_replay(r, i);
        forward_drawing(r, i);
        if (!pictochat_session_prepare(&p->session, p->admitted, out)) return false;
        r->replay_output = p->replay_active && out->kind == HOST_FRAME_HEARTBEAT &&
                           p->session.identity.phase == HOST_ID_READY;
        if (r->replay_output) {
            out->kind = HOST_FRAME_SESSION; out->application = true;
            out->packet = p->replay[p->replay_data];
            out->sequence = p->session.app_sequence[p->replay_data]++;
            p->session.output = *out;
        }
        if (out->application) {
            r->output_port = out->packet.bytes[0] == 2;
            r->saved_app_sequence = r->app_sequence[r->output_port];
            out->sequence = r->app_sequence[r->output_port]++;
            p->session.output = *out;
        }
        r->selected = i; r->next = (i + 1) % PICTOCHAT_ROOM_CLIENTS;
        r->outstanding = true; *slot = i;
        return true;
    }
    return false;
}
bool pictochat_room_finish(pictochat_room_t *r, pictochat_tx_result_t result) {
    if (!r->outstanding || r->dispatching) return false;
    pictochat_peer_t *p = &r->peers[r->selected];
    if (!pictochat_session_finish(&p->session, result)) return false;
    if (p->session.output.application && result == PICTOCHAT_TX_FAILED)
        r->app_sequence[r->output_port] = r->saved_app_sequence;
    if (r->replay_output && result == PICTOCHAT_TX_DELIVERED) {
        if (p->replay_data) {
            p->seen[p->replay_source][p->replay_stage] = p->replay_version;
            p->replay_active = false;
        } else p->replay_data = true;
    }
    r->outstanding = false;
    if (result == PICTOCHAT_TX_DELIVERED) {
        if (p->session.saved_identity.phase != HOST_ID_READY &&
            p->session.identity.phase == HOST_ID_READY) {
            pictochat_event_t event = peer_event(r, r->selected, PICTOCHAT_PEER_READY);
            dispatch(r, &event);
        }
        if (p->session.output.drawing && p->session.saved_cursor.active &&
            !p->session.sending.cursor.active)
            message_event(r, r->selected, PICTOCHAT_MESSAGE_SENT,
                p->session.sending.announcement, p->session.sending.body, p->session.sending.total);
    }
    return true;
}
void pictochat_room_members(const pictochat_room_t *r, uint8_t members[16][6]) {
    memset(members, 0, 16 * 6);
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i)
        if (r->peers[i].connected) memcpy(members[r->peers[i].aid], r->peers[i].mac, 6);
}

void pictochat_room_delivery(const pictochat_room_t *r, pictochat_delivery_t *delivery, uint32_t token) {
    *delivery = (pictochat_delivery_t){.token = token};
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) {
        if (r->peers[i].connected && r->peers[i].admitted && !r->peers[i].ghost) {
            delivery->pending |= 1u << i;
            delivery->generation[i] = r->peers[i].generation;
        }
    }
}
bool pictochat_room_reply(pictochat_room_t *r, pictochat_delivery_t *delivery,
    const uint8_t announcement[20], const uint8_t *body, size_t len, const uint8_t mac[6]) {
    if (r->outstanding || r->dispatching) return false;
    for (unsigned i = 0; i < PICTOCHAT_ROOM_CLIENTS; ++i) {
        if (!(delivery->pending & (1u << i))) continue;
        pictochat_peer_t *p = &r->peers[i];
        if (!p->connected || p->ghost || p->generation != delivery->generation[i]) {
            delivery->pending &= ~(1u << i);
            continue;
        }
        bool forwarding = false;
        for (unsigned j = 0; j < PICTOCHAT_ROOM_CLIENTS; ++j)
            forwarding |= (r->peers[j].drawing_targets & (1u << i)) != 0;
        if (forwarding || p->replay_active || !p->admitted) continue;
        if (pictochat_session_reply(&p->session, announcement, body, len, mac, delivery->token))
            delivery->pending &= ~(1u << i);
    }
    return delivery->pending == 0;
}
