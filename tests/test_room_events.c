#include <assert.h>
#include <stdio.h>
#include "pictochat/room.h"
#include "host_identity_fixture.h"

static pictochat_room_t room;
static const uint8_t macs[2][6] = {{0,1,2,3,4,5}, {0,1,2,3,4,6}};
static uint8_t received_body[HOST_MESSAGE_MAX];
static unsigned events[5], sent[2];
static void on_event(void *context, const pictochat_event_t *event) {
    assert(context == &events);
    assert(!room.outstanding && room.dispatching);
    assert(event->peer_slot < 2 && event->generation == event->peer_slot + 10);
    assert(event->aid == (event->peer_slot ? 7 : 1));
    assert(!memcmp(event->mac, macs[event->peer_slot], 6));
    ++events[event->type];
    unsigned slot; pictochat_output_t out;
    assert(!pictochat_room_prepare(&room, &slot, &out));
    assert(!pictochat_room_leave(&room, event->peer_slot));
    assert(!pictochat_room_set_handler(&room, NULL, NULL));
    pictochat_delivery_t snapshot;
    pictochat_room_delivery(&room, &snapshot, 3); // read-only calls are safe
    if (event->type == PICTOCHAT_MESSAGE_RECEIVED) {
        assert(event->length == 2084 && event->sender_slot == 1);
        assert(room.peers[0].session.received.complete);
        memcpy(received_body, event->body, event->length);
    } else if (event->type == PICTOCHAT_MESSAGE_SENT) {
        assert(event->length == 2084 && event->sender_slot == 0);
        assert(event->token == 0xaabbccdd && event->announcement[0] == 1);
        assert(!room.peers[event->peer_slot].session.sending.cursor.active);
        assert(!memcmp(event->body + 8, received_body + 8, event->length - 8));
        ++sent[event->peer_slot];
    } else {
        assert(!event->body && !event->announcement && !event->length);
        if (event->type == PICTOCHAT_PEER_READY)
            assert(room.peers[event->peer_slot].session.identity.phase == HOST_ID_READY);
        if (event->type == PICTOCHAT_PEER_LEFT)
            assert(!room.peers[event->peer_slot].connected);
    }
}
static void prepare(void) {
    unsigned slot; pictochat_output_t out;
    assert(pictochat_room_prepare(&room, &slot, &out));
}
static void deliver(void) { assert(pictochat_room_finish(&room, PICTOCHAT_TX_DELIVERED)); }
int main(void) {
    uint8_t profile[84] = {3};
    pictochat_room_reset(&room, profile);
    assert(pictochat_room_set_handler(&room, on_event, &events));
    assert(pictochat_room_join(&room, 0, macs[0], 1, 10, 1, 2));
    assert(events[PICTOCHAT_PEER_JOINED] == 1);
    assert(pictochat_room_join(&room, 0, macs[0], 1, 10, 1, 2));
    assert(events[PICTOCHAT_PEER_JOINED] == 1);
    pictochat_peer_t *peer = &room.peers[0];
    peer->admitted = true;
    peer->session.sequence.empty_sent = peer->session.sequence.admitted = true;
    peer->session.identity.phase = HOST_ID_WAIT1;
    peer->session.identity.relayed[0] = true;
    assert(pictochat_room_receive(&room, 0, 10, client_announce1, sizeof(client_announce1)) == 0);
    prepare(); deliver();
    assert(pictochat_room_receive(&room, 0, 10, client_data1, sizeof(client_data1)) == 0);
    prepare();
    assert(!events[PICTOCHAT_PEER_READY]); // prepare is tentative
    assert(!pictochat_room_set_handler(&room, NULL, NULL));
    assert(pictochat_room_finish(&room, PICTOCHAT_TX_FAILED));
    prepare(); assert(pictochat_room_finish(&room, PICTOCHAT_TX_NO_REPLY));
    assert(!events[PICTOCHAT_PEER_READY]);
    prepare(); deliver();
    assert(events[PICTOCHAT_PEER_READY] == 1);
    prepare(); deliver(); assert(events[PICTOCHAT_PEER_READY] == 1);

    FILE *f = fopen("tests/fixtures/send-client-apps.bin", "rb"); assert(f);
    for (;;) {
        int lo = fgetc(f); if (lo == EOF) break;
        int hi = fgetc(f); assert(hi != EOF);
        uint8_t app[268]; size_t len = (unsigned)lo | ((unsigned)hi << 8);
        assert(len <= sizeof(app) && fread(app, 1, len, f) == len);
        assert(pictochat_room_receive(&room, 0, 10, app, len) >= 0);
        prepare(); deliver();
    }
    fclose(f);
    assert(events[PICTOCHAT_MESSAGE_RECEIVED] == 1);
    assert(!events[PICTOCHAT_MESSAGE_SENT]); // receiving/relaying is not a new outbound transfer
    assert(pictochat_room_join(&room, 1, macs[1], 7, 11, 3, 4));
    room.peers[1].admitted = true;
    room.peers[1].session.sequence.empty_sent = room.peers[1].session.sequence.admitted = true;
    room.peers[1].session.identity.phase = HOST_ID_READY;
    pictochat_delivery_t ticket;
    pictochat_room_delivery(&room, &ticket, 0xaabbccdd);
    assert(pictochat_room_reply(&room, &ticket, peer->session.received.announcement,
                               received_body, 2084, macs[0]));
    assert(!events[PICTOCHAT_MESSAGE_SENT]); // queued != sent
    unsigned retries[2] = {0};
    for (unsigned n = 0; n < 200 && (!sent[0] || !sent[1]); ++n) {
        unsigned slot; pictochat_output_t out;
        assert(pictochat_room_prepare(&room, &slot, &out));
        if (out.drawing && !room.peers[slot].session.sending.cursor.active && retries[slot] < 2) {
            assert(!sent[slot]);
            assert(pictochat_room_finish(&room, retries[slot]++ == 0 ?
                PICTOCHAT_TX_FAILED : PICTOCHAT_TX_NO_REPLY));
            assert(!sent[slot]);
        } else deliver();
    }
    assert(retries[0] == 2 && retries[1] == 2);
    assert(sent[0] == 1 && sent[1] == 1 && events[PICTOCHAT_MESSAGE_SENT] == 2);
    assert(pictochat_session_reply(&room.peers[1].session, peer->session.received.announcement,
                                   received_body, 2084, macs[0], 0xaabbccdd));
    assert(pictochat_room_leave(&room, 1)); // queued delivery canceled, no SENT
    assert(events[PICTOCHAT_MESSAGE_SENT] == 2);
    assert(pictochat_room_leave(&room, 1));
    assert(events[PICTOCHAT_PEER_LEFT] == 1);
    assert(pictochat_room_set_handler(&room, NULL, NULL));
    assert(pictochat_room_leave(&room, 0));
    assert(events[PICTOCHAT_PEER_LEFT] == 1);
    pictochat_room_reset(&room, room.profile);
    assert(!room.handler && !room.handler_context && !room.dispatching);
    return 0;
}
