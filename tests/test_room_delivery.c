#include <assert.h>
#include "pictochat/room.h"

static pictochat_room_t room;
static host_message_rx_t received[2];
static uint8_t body[1060] = {3,2};
static const uint8_t host[6] = {0,9,0xbf,0xc6,0xc6,0xc6};
static const uint8_t clients[2][6] = {{0,1,2,3,4,5}, {0,1,2,3,4,6}};
static const uint8_t announcement[20] = {0,0,20,0,2,0,0xff,0xff,0x24,4};

int main(void) {
    uint8_t profile[84] = {3};
    pictochat_room_reset(&room, profile);
    for (unsigned i = 0; i < 2; ++i) {
        assert(pictochat_room_join(&room, i, clients[i], i+1, i+1, 1, 2));
        room.peers[i].admitted = true;
        room.peers[i].session.sequence.empty_sent = true;
        room.peers[i].session.sequence.admitted = true;
        room.peers[i].session.identity.phase = HOST_ID_READY;
    }
    // One recipient is busy with an older drawing. It must still receive the
    // room-wide bot reply later; queuing it for the other recipient isn't enough.
    assert(pictochat_session_reply(&room.peers[1].session, announcement,
                                   body, sizeof(body), host, 9));
    pictochat_delivery_t delivery;
    pictochat_room_delivery(&room, &delivery, 8);
    assert(delivery.pending == 3);
    assert(!pictochat_room_reply(&room, &delivery, announcement, body, sizeof(body), host));
    assert(delivery.pending == 2);

    unsigned complete[2] = {0}, count[2] = {0};
    uint16_t next_sequence[2] = {0};
    bool failed = false, missed = false;
    for (unsigned n = 0; n < 250; ++n) {
        pictochat_room_reply(&room, &delivery, announcement, body, sizeof(body), host);
        unsigned dest; pictochat_output_t out;
        assert(pictochat_room_prepare(&room, &dest, &out));
        if (out.application) {
            unsigned port = out.packet.bytes[0] == 2;
            assert(out.sequence == next_sequence[port]); // one room-wide stream
            if (!failed) {
                failed = true;
                assert(pictochat_room_finish(&room, PICTOCHAT_TX_FAILED));
                continue;
            }
            ++next_sequence[port];
            if (!missed && dest == 1) {
                missed = true;
                assert(pictochat_room_finish(&room, PICTOCHAT_TX_NO_REPLY));
                continue;
            }
            ++count[dest];
            assert(out.packet.bytes[4] == 0); // the bot, to both recipients
            if (out.packet.bytes[0] == 1) out.packet.bytes[0] = 0;
            int result = host_message_receive_slot(&received[dest], out.packet.bytes, out.packet.len, 0);
            assert(result >= 0);
            if (result == 1 && received[dest].announcement[16] == 8) {
                ++complete[dest];
                for (unsigned i = 0; i < 6; ++i) assert(received[dest].body[2+i] == host[i^1]);
            }
        }
        assert(pictochat_room_finish(&room, PICTOCHAT_TX_DELIVERED));
    }
    assert(failed && missed && count[0] && count[1]);
    assert(complete[0] == 1 && complete[1] == 1 && delivery.pending == 0);

    pictochat_room_delivery(&room, &delivery, 10);
    room.peers[1].session.sending.cursor.active = true;
    assert(!pictochat_room_reply(&room, &delivery, announcement, body, sizeof(body), host));
    assert(delivery.pending == 2);
    uint16_t old_sequence = room.app_sequence[0];
    assert(pictochat_room_leave(&room, 1));
    assert(pictochat_room_join(&room, 1, clients[1], 2, 99, 3, 4));
    assert(pictochat_room_reply(&room, &delivery, announcement, body, sizeof(body), host));
    assert(!room.peers[1].session.sending.cursor.active);
    assert(room.app_sequence[0] == old_sequence); // reconnect cannot reset stream
    return 0;
}
