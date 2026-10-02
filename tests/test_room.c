#include <assert.h>
#include <stdio.h>
#include "pictochat/room.h"
#include "pictochat/frame.h"
#include "host_identity_fixture.h"

static pictochat_room_t room;
static uint8_t profile[84] = {3};
static const uint8_t macs[4][6] = {
    {0, 1, 2, 3, 4, 5}, {0, 1, 2, 3, 4, 6}, {0, 1, 2, 3, 4, 7}, {0, 1, 2, 3, 4, 8}};
static host_message_rx_t delivered[4];
static unsigned completions[4];

static void cycle(bool check_drawing) {
    unsigned slot;
    pictochat_output_t out, retry;
    assert(pictochat_room_prepare(&room, &slot, &out));
    if (room.replay_output) {
        unsigned source = room.peers[slot].replay_source;
        assert(out.packet.bytes[4] == room.peers[source].aid);
    }
    if (out.application && room.peers[slot].session.saved_identity.phase == HOST_ID_REQUEST &&
        !room.peers[slot].session.saved_identity.pending)
        assert(out.packet.bytes[4] == room.peers[slot].aid);
    assert(!pictochat_room_prepare(&room, &slot, &retry));
    assert(!pictochat_room_leave(&room, slot));
    if (check_drawing && out.drawing) {
        /* Only the chosen recipient's state advances; timeout leaves its
         * payload cursor untouched while other clients can still be polled. */
        host_message_cursor_t before = room.peers[slot].session.saved_cursor;
        assert(pictochat_room_finish(&room, PICTOCHAT_TX_NO_REPLY));
        assert(!memcmp(&before, &room.peers[slot].session.sending.cursor, sizeof(before)));
        room.next = slot;
        unsigned retry_slot;
        assert(pictochat_room_prepare(&room, &retry_slot, &retry));
        assert(retry_slot == slot && retry.sequence == (uint16_t)(out.sequence + 1));
        assert(retry.packet.len == out.packet.len &&
               !memcmp(retry.packet.bytes, out.packet.bytes, out.packet.len));
    }
    if (out.drawing) {
        assert(out.packet.bytes[4] == room.peers[1 - slot].aid);
        uint8_t bytes[268];
        memcpy(bytes, out.packet.bytes, out.packet.len);
        if (bytes[0] == 1)
            bytes[0] = 0;
        bytes[4] = 1; // normalize host outbound slot for the receive assembler
        int result = host_message_receive(&delivered[slot], bytes, out.packet.len);
        assert(result >= 0);
        completions[slot] += result == 1;
    }
    assert(pictochat_room_finish(&room, PICTOCHAT_TX_DELIVERED));
}

static void pump(unsigned n, bool drawings) {
    while (n--)
        cycle(drawings);
}

static void feed(unsigned slot, const uint8_t *app, size_t len) {
    uint8_t addressed[268];
    assert(len <= sizeof(addressed));
    memcpy(addressed, app, len);
    addressed[4] = room.peers[slot].aid;
    if (len == 96 && addressed[0] == 2 && addressed[12] == 3 && addressed[13] <= 1)
        for (unsigned j = 0; j < 6; ++j)
            addressed[14 + j] = room.peers[slot].mac[j ^ 1];
    int result;
    unsigned tries = 0;
    while ((result = pictochat_room_receive(&room, slot, room.peers[slot].generation, addressed,
                                            len)) == -2) {
        assert(++tries < 1000);
        cycle(false);
    }
    assert(result >= 0);
    while (room.peers[slot].session.identity.pending)
        cycle(false);
}

static void identify(unsigned slot) {
    feed(slot, client_announce0, sizeof(client_announce0));
    feed(slot, client_data0, sizeof(client_data0));
    feed(slot, client_announce1, sizeof(client_announce1));
    feed(slot, client_data1, sizeof(client_data1));
}

int main(void) {
    pictochat_room_reset(&room, profile);
    unsigned slot;
    pictochat_output_t out;
    assert(!pictochat_room_prepare(&room, &slot, &out));
    assert(!pictochat_room_join(&room, 4, macs[0], 1, 1, 1, 2));
    assert(!pictochat_room_join(&room, 0, macs[0], 0, 1, 1, 2));
    assert(!pictochat_room_join(&room, 0, macs[0], 16, 1, 1, 2));
    assert(pictochat_room_join(&room, 0, macs[0], 1, 1, 1, 2));
    assert(!pictochat_room_join(&room, 1, macs[1], 1, 2, 3, 4));
    assert(!pictochat_room_join(&room, 1, macs[0], 9, 2, 3, 4));
    assert(pictochat_room_join(&room, 1, macs[1], 9, 2, 3, 4));
    room.peers[0].admitted = room.peers[1].admitted = true;
    for (unsigned i = 0; i < 4; ++i) {
        assert(pictochat_room_prepare(&room, &slot, &out));
        assert(slot == i % 2);
        assert(pictochat_room_finish(&room, PICTOCHAT_TX_DELIVERED));
    }
    pump(40, false);
    identify(0);
    identify(1);
    pump(40, false);
    for (unsigned i = 0; i < 2; ++i) {
        assert(room.peers[i].session.identity.phase == HOST_ID_READY);
        assert(room.peers[i].seen[1 - i][0] == 1 && room.peers[i].seen[1 - i][1] == 1);
    }
    /* Identity replay commits only after the intended recipient replies. */
    room.peers[0].seen[1][0] = 0;
    room.next = 0;
    assert(pictochat_room_prepare(&room, &slot, &out) && room.replay_output);
    assert(pictochat_room_receive(&room, 1, 2, client_announce0, 20) == -2);
    assert(pictochat_room_finish(&room, PICTOCHAT_TX_FAILED));
    room.next = 0;
    pictochat_output_t retry;
    assert(pictochat_room_prepare(&room, &slot, &retry));
    assert(retry.sequence == out.sequence && !memcmp(retry.packet.bytes, out.packet.bytes, 20));
    assert(pictochat_room_finish(&room, PICTOCHAT_TX_NO_REPLY));
    assert(!room.peers[0].seen[1][0] && !room.peers[0].replay_data);
    pump(8, false);
    assert(room.peers[0].seen[1][0] == 1);
    /* Duplicate joins and identity packets do not reset established peers. */
    assert(pictochat_room_join(&room, 0, macs[0], 1, 1, 99, 100));
    assert(room.peers[0].session.identity.phase == HOST_ID_READY);
    identify(0);
    assert(room.peers[0].versions[0] == 1);

    uint8_t members[16][6], frame[302];
    pictochat_room_members(&room, members);
    host_poll_fields_result_t fields = {.bitmask = 1u << 9, .wm = 0x1c34};
    assert(pictochat_frame_room_members(frame, sizeof(frame), macs[2], members, 5, fields, 0, 3,
                                        true) == 138);
    for (unsigned j = 0; j < 6; ++j) {
        assert(frame[44 + j] == macs[0][j ^ 1]);
        assert(frame[92 + j] == macs[1][j ^ 1]);
    }
    assert(frame[26] == 0 && frame[27] == 2 && frame[136] == 0 && frame[137] == 2);
    host_id_packet_t app = {.len = 20, .bytes = {1, 0, 20, 0}};
    assert(pictochat_frame_target_app(frame, sizeof(frame), macs[2], &app, 7, 1u << 15) == 54);
    assert(frame[27] == 128 && frame[53] == 128);
    assert(!pictochat_frame_target_app(frame, sizeof(frame), macs[2], &app, 7, 6));
    assert(!pictochat_frame_target_app(frame, sizeof(frame), macs[2], &app, 7, 1));

    /* Late join replays existing identities, and announces the newcomer to
     * both established clients without resetting their handshake state. */
    assert(pictochat_room_join(&room, 2, macs[2], 15, 3, 5, 6));
    room.peers[2].admitted = true;
    pump(60, false);
    identify(2);
    pump(100, false);
    assert(room.peers[2].seen[0][1] == 1 && room.peers[2].seen[1][1] == 1);
    assert(room.peers[0].seen[2][1] == 1 && room.peers[1].seen[2][1] == 1);
    assert(pictochat_room_leave(&room, 2));
    pump(30, false);

    /* Interleaved drawings from two clients use independent reassembly and
     * reach the other recipient, preserving the entire captured body. */
    FILE *f = fopen("tests/fixtures/send-client-apps.bin", "rb");
    assert(f);
    for (;;) {
        int lo = fgetc(f);
        if (lo == EOF)
            break;
        int hi = fgetc(f);
        assert(hi != EOF);
        uint8_t bytes[268];
        size_t len = (unsigned)lo | ((unsigned)hi << 8);
        assert(len <= sizeof(bytes) && fread(bytes, 1, len, f) == len);
        feed(0, bytes, len);
        if (bytes[0] == 0)
            bytes[16] ^= 0x40;
        if (bytes[0] == 2 && bytes[8] == 0 && bytes[9] == 0)
            bytes[14] ^= 0x40;
        feed(1, bytes, len);
    }
    fclose(f);
    pump(250, true);
    assert(memcmp(delivered[0].body, delivered[1].body, 2084));
    for (unsigned i = 0; i < 2; ++i) {
        assert(completions[i] == 1 && delivered[i].total == 2084);
        assert(!memcmp(delivered[i].body, room.peers[1 - i].session.received.body, 2084));
        assert(!memcmp(delivered[i].announcement + 16,
                       room.peers[1 - i].session.received.announcement + 16, 4));
    }
    // The bot may still echo a drawing received from a non-1 client slot.
    pictochat_session_t *second = &room.peers[1].session;
    assert(pictochat_session_reply(second, second->received.announcement, second->received.body,
                                   second->received.total, macs[2], 77));
    assert(second->sending.announcement[4] == 0);
    /* Disconnect/reuse removes pending recipients and cached identities;
     * stale queued input cannot enter the new association. */
    room.peers[0].drawing_targets = 2;
    assert(pictochat_room_receive(&room, 0, 1, client_announce0, 20) == -2);
    assert(pictochat_room_leave(&room, 1));
    assert(!room.peers[0].drawing_targets && !room.peers[0].seen[1][0]);
    assert(room.peers[0].session.identity.phase == HOST_ID_READY);
    assert(pictochat_room_join(&room, 1, macs[3], 9, 4, 7, 8));
    room.peers[1].admitted = true;
    assert(pictochat_room_receive(&room, 1, 2, client_announce0, 20) == -1);
    assert(!room.peers[1].session.received.active);
    assert(pictochat_room_join(&room, 2, macs[1], 5, 5, 9, 10));
    assert(pictochat_room_join(&room, 3, macs[2], 15, 6, 11, 12));
    unsigned selected[4] = {0};
    for (unsigned i = 0; i < 16; ++i) {
        assert(pictochat_room_prepare(&room, &slot, &out));
        ++selected[slot];
        assert(pictochat_room_finish(&room, PICTOCHAT_TX_DELIVERED));
    }
    for (unsigned i = 0; i < 4; ++i)
        assert(selected[i] == 4);
    return 0;
}
