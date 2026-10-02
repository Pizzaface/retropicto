#include <assert.h>
#include "pictochat/host_identity.h"
#include "host_identity_fixture.h"
static const uint8_t request_identity[] = {1, 0, 20, 0, 1, 0, 0xff, 0xff, 84,   0,
                                           0, 0, 0,  0, 0, 0, 0xb7, 0x78, 0xd5, 0x29};

static void expect_next(host_identity_t *s, const uint8_t *bytes, size_t len) {
    host_id_packet_t out;
    assert(host_identity_next(s, own_data0 + 12, &out));
    assert(out.len == len);
    assert(memcmp(out.bytes, bytes, len) == 0);
}

int main(void) {
    host_identity_t state;
    host_id_packet_t out;
    host_identity_reset(&state, 0x51b35858, 0x013d747a);
    expect_next(&state, own_announce0, sizeof(own_announce0));
    expect_next(&state, own_data0, sizeof(own_data0));
    assert(state.phase == HOST_ID_ANNOUNCE1);

    // Replay the two-sided identity exchange, checking exact captured payloads.
    assert(host_identity_receive(&state, client_announce0, sizeof(client_announce0)));
    expect_next(&state, relay_announce0, sizeof(relay_announce0));
    assert(host_identity_receive(&state, client_data0, sizeof(client_data0)));
    expect_next(&state, relay_data0, sizeof(relay_data0));
    expect_next(&state, own_announce1, sizeof(own_announce1));
    // The client's second announcement can arrive before our second data packet.
    assert(host_identity_receive(&state, client_announce1, sizeof(client_announce1)));
    expect_next(&state, relay_announce1, sizeof(relay_announce1));
    expect_next(&state, own_data1, sizeof(own_data1));
    expect_next(&state, request_identity, sizeof(request_identity));
    assert(host_identity_receive(&state, client_data1, sizeof(client_data1)));
    expect_next(&state, relay_data1, sizeof(relay_data1));
    assert(state.phase == HOST_ID_READY);
    for (int i = 0; i < 100; ++i)
        assert(!host_identity_next(&state, own_data0 + 12, &out));

    // A new join cannot inherit pending relay data or progress from the old one.
    assert(host_identity_receive(&state, client_announce0, sizeof(client_announce0)));
    host_identity_reset(&state, 0x51b35858, 0x013d747a);
    assert(!state.pending && !state.announced && !state.relayed[0] && !state.relayed[1]);
    assert(!host_identity_receive(&state, client_data0, sizeof(client_data0)));
    expect_next(&state, own_announce0, sizeof(own_announce0));

    // Failed submission/completion restores the snapshot for byte-identical retry.
    host_identity_t before = state;
    expect_next(&state, own_data0, sizeof(own_data0));
    state = before;
    expect_next(&state, own_data0, sizeof(own_data0));

    // Malformed/foreign transfers cannot start a relay or advance identity state.
    assert(!host_identity_receive(&state, client_announce0, sizeof(client_announce0) - 1));
    uint8_t bad[96];
    memcpy(bad, client_announce0, sizeof(client_announce0));
    bad[4] = 2;
    assert(!host_identity_receive(&state, bad, sizeof(client_announce0)));
    assert(host_identity_receive(&state, client_announce0, sizeof(client_announce0)));
    // A second receive cannot overwrite a queued announcement.
    assert(!host_identity_receive(&state, client_data0, sizeof(client_data0)));
    expect_next(&state, relay_announce0, sizeof(relay_announce0));
    memcpy(bad, client_data0, sizeof(client_data0));
    bad[6]--;
    assert(!host_identity_receive(&state, bad, sizeof(bad)));
    memcpy(bad, client_data0, sizeof(client_data0));
    bad[8] = 1;
    assert(!host_identity_receive(&state, bad, sizeof(bad)));
    assert(state.phase == HOST_ID_ANNOUNCE1);
    // With no client application replies, still publish both host stages.
    host_identity_reset(&state, 0x51b35858, 0x013d747a);
    expect_next(&state, own_announce0, sizeof(own_announce0));
    expect_next(&state, own_data0, sizeof(own_data0));
    expect_next(&state, own_announce1, sizeof(own_announce1));
    expect_next(&state, own_data1, sizeof(own_data1));
    expect_next(&state, request_identity, sizeof(request_identity));
    assert(!host_identity_next(&state, own_data0 + 12, &out));
    assert(state.phase == HOST_ID_WAIT1);
    // Announcement captured from Jordan at AID 2 during the two-console trial.
    static const uint8_t announce2[20] = {0, 0, 20,   0,    2,    0, 0xff, 0xff, 84,   0,
                                          0, 0, 0xe4, 0x51, 0x3a, 2, 0x6d, 0xb0, 0x6e, 0xe9};
    host_identity_reset(&state, 1, 2);
    state.client_slot = 2;
    assert(!host_identity_receive(&state, client_announce0, sizeof(client_announce0)));
    assert(host_identity_receive(&state, announce2, sizeof(announce2)));
    assert(host_identity_next(&state, own_data0 + 12, &out));
    assert(out.bytes[0] == 1 && out.bytes[4] == 2);
    assert(!memcmp(out.bytes + 1, announce2 + 1, 19));
    memcpy(bad, client_data0, sizeof(bad));
    bad[4] = 2;
    assert(host_identity_receive(&state, bad, sizeof(bad)));
    assert(host_identity_next(&state, own_data0 + 12, &out));
    assert(!memcmp(out.bytes, bad, sizeof(bad)));
    state.phase = HOST_ID_REQUEST;
    assert(host_identity_next(&state, own_data0 + 12, &out));
    assert(out.bytes[4] == 2);
    return 0;
}
