#include <assert.h>
#include <stdio.h>
#include "pictochat/session.h"
#include "host_identity_fixture.h"

static pictochat_session_t session;
static pictochat_output_t out, retry;
static uint8_t profile[84] = {3};
static const uint8_t mac[6] = {0, 9, 0xbf, 0xc6, 0xc6, 0xc6};

static void prepare(void) {
    assert(pictochat_session_prepare(&session, true, &out));
}

static void deliver(void) {
    assert(pictochat_session_finish(&session, PICTOCHAT_TX_DELIVERED));
}

int main(void) {
    pictochat_session_reset(&session, profile, 0x12345678, 2);
    assert(!pictochat_session_finish(&session, PICTOCHAT_TX_DELIVERED));
    assert(pictochat_session_prepare(&session, false, &out));
    assert(out.kind == HOST_FRAME_EMPTY);
    assert(!pictochat_session_prepare(&session, true, &retry));
    assert(pictochat_session_receive(&session, own_data0, sizeof(own_data0)) == -2);
    assert(pictochat_session_finish(&session, PICTOCHAT_TX_FAILED));
    prepare();
    assert(out.kind == HOST_FRAME_EMPTY);
    deliver();
    assert(pictochat_session_prepare(&session, false, &out));
    assert(out.kind == HOST_FRAME_HEARTBEAT);
    deliver();
    for (unsigned i = 0; i < 7; ++i) {
        prepare();
        assert(out.kind == HOST_FRAME_ROSTER);
        deliver();
    }
    prepare();
    assert(out.application && out.packet.bytes[0] == 1 && out.sequence == 0);
    retry = out;
    assert(pictochat_session_finish(&session, PICTOCHAT_TX_FAILED));
    prepare();
    assert(out.sequence == retry.sequence && out.packet.len == retry.packet.len);
    assert(!memcmp(out.packet.bytes, retry.packet.bytes, out.packet.len));
    assert(pictochat_session_finish(&session, PICTOCHAT_TX_NO_REPLY));
    prepare();
    assert(out.sequence == 1 && !memcmp(out.packet.bytes, retry.packet.bytes, out.packet.len));
    deliver();
    prepare();
    assert(out.packet.bytes[0] == 2 && out.packet.bytes[13] == 0);
    deliver();
    prepare();
    assert(out.packet.bytes[0] == 1);
    deliver();
    prepare();
    assert(out.packet.bytes[0] == 2 && out.packet.bytes[13] == 1);
    deliver();
    prepare();
    assert(out.packet.bytes[0] == 1 && out.packet.bytes[4] == 1);
    deliver();
    prepare();
    assert(out.kind == HOST_FRAME_HEARTBEAT);
    deliver();

    /* Replay captured client drawing through the actual public engine, checking
       relay bytes, backpressure and one-time completion despite retransmission. */
    FILE *f = fopen("tests/fixtures/send-client-apps.bin", "rb");
    assert(f);
    unsigned completed = 0;
    const uint8_t odd[13] = {2, 0, 13, 0, 1, 0, 1};
    assert(pictochat_session_receive(&session, odd, sizeof(odd)) == -1);
    assert(!session.identity.pending);
    for (;;) {
        int lo = fgetc(f);
        if (lo == EOF)
            break;
        int hi = fgetc(f);
        assert(hi != EOF);
        uint8_t app[268];
        size_t len = (unsigned)lo | ((unsigned)hi << 8);
        assert(len <= sizeof(app) && fread(app, 1, len, f) == len);
        int result = pictochat_session_receive(&session, app, len);
        assert(result >= 0);
        completed += result == 1;
        assert(pictochat_session_receive(&session, app, len) == -2);
        prepare();
        assert(out.application && out.packet.len == len);
        if (app[0] == 0)
            app[0] = 1;
        assert(!memcmp(out.packet.bytes, app, len));
        deliver();
    }
    fclose(f);
    assert(completed == 1 && session.received.total == 2084);
    /* Identity-ready gating is tested by the identity suite; isolate sender here. */
    assert(!pictochat_session_reply(&session, session.received.announcement, session.received.body,
                                    session.received.total, mac, 3));
    session.identity.phase = HOST_ID_READY;
    assert(!pictochat_session_reply(&session, session.received.announcement, session.received.body,
                                    HOST_MESSAGE_MAX + 1, mac, 3));
    assert(pictochat_session_reply(&session, session.received.announcement, session.received.body,
                                   session.received.total, mac, 3));
    assert(!pictochat_session_reply(&session, session.received.announcement, session.received.body,
                                    session.received.total, mac, 3));
    unsigned sent = 0;
    while (session.sending.cursor.active) {
        prepare();
        assert(out.drawing);
        retry = out;
        assert(pictochat_session_finish(&session, PICTOCHAT_TX_FAILED));
        prepare();
        assert(out.sequence == retry.sequence);
        assert(!memcmp(out.packet.bytes, retry.packet.bytes, out.packet.len));
        assert(pictochat_session_finish(&session, PICTOCHAT_TX_NO_REPLY));
        prepare();
        assert(out.sequence == (uint16_t)(retry.sequence + 1));
        assert(!memcmp(out.packet.bytes, retry.packet.bytes, out.packet.len));
        deliver();
        ++sent;
    }
    assert(sent == 45); /* three announcements + fourteen chunks x three */
    pictochat_session_reset(&session, session.profile, 4, 5);
    assert(!session.received.active && !session.sending.cursor.active);
    assert(!session.outstanding && !session.identity.pending && session.profile[0] == 3);
    return 0;
}
