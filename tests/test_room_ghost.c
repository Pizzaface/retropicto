#include <assert.h>
#include <string.h>
#include "pictochat/room.h"
#include "host_identity_fixture.h"

static pictochat_room_t room;
static host_message_rx_t received[2];
static unsigned sent, incoming, completed[2];
static uint8_t body[1060] = {3,2};
static const uint8_t announcement[20] = {0,0,20,0,1,0,255,255,0x24,4};
static const uint8_t macs[2][6] = {{0,1,2,3,4,5}, {0,1,2,3,4,6}};
static uint8_t profile[84];

static void event(void *context, const pictochat_event_t *e) {
    (void)context;
    if (e->type == PICTOCHAT_MESSAGE_RECEIVED) ++incoming;
    if (e->type == PICTOCHAT_MESSAGE_SENT) {
        assert(e->sender_slot == 15 && e->peer_slot < 2);
        ++sent;
    }
}
static void real_join(unsigned slot, unsigned generation) {
    assert(pictochat_room_join(&room, slot, macs[slot], slot+1, generation, 10, 11));
    room.peers[slot].admitted = true;
    // Physical handshake is covered by test_room; start at its READY boundary.
    room.peers[slot].session.sequence.empty_sent = true;
    room.peers[slot].session.sequence.admitted = true;
    room.peers[slot].session.identity.phase = HOST_ID_READY;
}
static void cycle(pictochat_tx_result_t result) {
    unsigned slot;
    pictochat_output_t out;
    assert(pictochat_room_prepare(&room, &slot, &out));
    assert(slot < 2); // never grant a reply slot to a ghost
    if (out.application) {
        assert(out.packet.bytes[4] == 15);
        if (room.replay_output && out.packet.bytes[0] == 2) {
            unsigned stage = out.packet.bytes[13];
            assert(stage <= 1 && out.packet.len == 96);
            assert(!memcmp(out.packet.bytes + 14, profile + 2, 82));
        }
        if (out.drawing) {
            assert(room.peers[slot].seen[3][0] && room.peers[slot].seen[3][1]);
            if (result == PICTOCHAT_TX_DELIVERED) {
                if (out.packet.bytes[0] == 1) out.packet.bytes[0] = 0;
                int rc = host_message_receive_slot(&received[slot], out.packet.bytes, out.packet.len, 15);
                assert(rc >= 0);
                completed[slot] += rc == 1;
            }
        }
    }
    assert(pictochat_room_ghost_send(&room, 3, 1, announcement, body, sizeof(body), 99) == -2);
    assert(!pictochat_room_leave(&room, 3));
    assert(pictochat_room_finish(&room, result));
}
int main(void) {
    memcpy(profile, client_data0 + 12, 84);
    pictochat_room_reset(&room, own_data0 + 12);
    assert(pictochat_room_set_handler(&room, event, NULL));
    assert(!pictochat_room_ghost_join(&room, 3, 15, 0, profile, 1, 2));
    assert(!pictochat_room_ghost_join(&room, 3, 0, 1, profile, 1, 2));
    assert(!pictochat_room_ghost_join(&room, 3, 15, 1, NULL, 1, 2));
    assert(pictochat_room_ghost_join(&room, 3, 15, 1, profile, 1, 2));
    unsigned slot; pictochat_output_t out;
    assert(!pictochat_room_prepare(&room, &slot, &out)); // ghosts alone need no polls
    uint8_t members[16][6];
    pictochat_room_members(&room, members);
    for (unsigned i = 0; i < 6; ++i) assert(members[15][i] == profile[2+(i^1)]);
    assert(!pictochat_room_join(&room, 0, macs[0], 15, 1, 1, 2));
    assert(pictochat_room_receive(&room, 3, 1, client_announce0, 20) == -1);
    real_join(0, 1);
    pictochat_delivery_t delivery;
    pictochat_room_delivery(&room, &delivery, 1);
    assert(delivery.pending == 1); // host replies never wait on ghosts
    assert(pictochat_room_ghost_send(&room, 3, 2, announcement, body, sizeof(body), 12) == -1);
    assert(pictochat_room_ghost_send(&room, 3, 1, announcement, body, sizeof(body)-1, 12) == -1);
    body[40] = 0x5a;
    assert(pictochat_room_ghost_send(&room, 3, 1, announcement, body, sizeof(body), 12) == 0);
    assert(pictochat_room_ghost_send(&room, 3, 1, announcement, body, sizeof(body), 13) == -2);
    real_join(1, 2); // late join gets identity, not the previously queued drawing
    assert(room.peers[3].drawing_targets == 1);
    for (unsigned n = 0; n < 250; ++n)
        cycle(n % 9 == 0 ? PICTOCHAT_TX_FAILED : n % 7 == 0 ? PICTOCHAT_TX_NO_REPLY : PICTOCHAT_TX_DELIVERED);
    assert(completed[0] == 1 && completed[1] == 0 && sent == 1 && incoming == 0);
    assert(!memcmp(received[0].body + 8, body + 8, sizeof(body)-8));
    assert(!memcmp(received[0].body + 2, profile + 2, 6));
    assert(received[0].announcement[16] == 12);
    assert(room.peers[1].seen[3][0] && room.peers[1].seen[3][1]);
    assert(pictochat_room_ghost_join(&room, 3, 15, 1, profile, 100, 101));
    assert(room.peers[0].seen[3][0]); // duplicate join does not replay/reset
    assert(pictochat_room_ghost_send(&room, 3, 1, announcement, body, sizeof(body), 13) == 0);
    assert(pictochat_room_leave(&room, 1));
    real_join(1, 3);
    for (unsigned n = 0; n < 200; ++n) cycle(PICTOCHAT_TX_DELIVERED);
    assert(completed[0] == 2 && completed[1] == 0 && sent == 2);
    assert(pictochat_room_ghost_send(&room, 3, 1, announcement, body, sizeof(body), 14) == 0);
    for (unsigned n = 0; n < 200; ++n) cycle(PICTOCHAT_TX_DELIVERED);
    assert(completed[0] == 3 && completed[1] == 1 && sent == 4);
    assert(pictochat_room_leave(&room, 3));
    pictochat_room_members(&room, members);
    static const uint8_t zero[6] = {0};
    assert(!memcmp(members[15], zero, 6));
    assert(!room.peers[0].seen[3][0] && !room.peers[1].seen[3][1]);
    assert(pictochat_room_ghost_send(&room, 3, 1, announcement, body, sizeof(body), 15) == -1);
    assert(pictochat_room_ghost_join(&room, 3, 15, 2, profile, 3, 4));
    assert(pictochat_room_ghost_send(&room, 3, 1, announcement, body, sizeof(body), 15) == -1);
    return 0;
}
