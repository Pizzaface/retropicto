#include <assert.h>
#include <stdio.h>
#include "pictochat/host_message.h"

static host_message_rx_t rx;
static host_message_tx_t tx;
static host_id_packet_t packets[128];
static uint8_t expected[HOST_MESSAGE_MAX];

int main(void) {
    FILE *f = fopen("tests/fixtures/send-client-apps.bin", "rb");
    assert(f);
    unsigned n = 0;
    for (;;) {
        int lo = fgetc(f), hi;
        if (lo == EOF)
            break;
        hi = fgetc(f);
        assert(hi != EOF && n < 128);
        packets[n].len = (uint16_t)(lo | (hi << 8));
        assert(packets[n].len <= sizeof(packets[n].bytes));
        assert(fread(packets[n].bytes, 1, packets[n].len, f) == packets[n].len);
        ++n;
    }
    fclose(f);
    f = fopen("tests/fixtures/send-message.bin", "rb");
    assert(f);
    size_t total = fread(expected, 1, sizeof(expected), f);
    fclose(f);
    assert(total == 2084 && n > 13);
    unsigned first_data = 0;
    while (first_data < n && packets[first_data].bytes[0] != 2)
        ++first_data;
    assert(first_data < n);

    host_identity_t identity;
    host_identity_reset(&identity, 1, 2);
    identity.phase = HOST_ID_READY;
    unsigned complete = 0, short_final = 0;
    host_id_packet_t out;
    uint8_t profile[84] = {3};
    // Actual captured announcement, all 160-byte chunks, duplicate fragments,
    // and four-byte final chunks must be accepted and relayed without truncation.
    for (unsigned i = 0; i < n; ++i) {
        host_id_packet_t *p = &packets[i];
        assert(host_identity_receive(&identity, p->bytes, p->len));
        int result = host_message_receive(&rx, p->bytes, p->len);
        assert(result >= 0);
        complete += result == 1;
        assert(host_identity_next(&identity, profile, &out));
        assert(out.len == p->len);
        assert(out.bytes[0] == (p->bytes[0] == 0 ? 1 : 2));
        assert(memcmp(out.bytes + 1, p->bytes + 1, p->len - 1) == 0);
        if (p->len == 16 && p->bytes[0] == 2 && p->bytes[7] == 1)
            ++short_final;
    }
    assert(complete == 1 && short_final && rx.total == total);
    assert(memcmp(rx.body, expected, total) == 0);

    // Sender output must preserve the exact received bitmap and metadata except
    // for the host sender MAC. Validate offsets/bytes independently of RX parsing.
    uint8_t mac[6] = {0, 9, 0xbf, 0xc6, 0xc6, 0xc6};
    host_message_reply(&tx, rx.announcement, rx.body, rx.total, mac, 0x12345678);
    for (unsigned copy = 0; copy < HOST_MESSAGE_COPIES; ++copy)
        assert(host_message_next(&tx, &out) && out.len == 20 && out.bytes[0] == 1 &&
               out.bytes[4] == 0);
    assert(host_message_u16(out.bytes + 8) == total);
    unsigned offset = 0;
    unsigned copies = 0;
    while (host_message_next(&tx, &out)) {
        assert(out.bytes[0] == 2 && out.bytes[4] == 0);
        assert(host_message_u16(out.bytes + 8) == offset);
        assert(out.len == 12u + out.bytes[6]);
        for (unsigned j = 0; j < out.bytes[6]; ++j) {
            unsigned at = offset + j;
            uint8_t want = at >= 2 && at < 8 ? mac[(at - 2) ^ 1] : expected[at];
            assert(out.bytes[12 + j] == want);
        }
        assert((out.bytes[7] & 1) == (offset + out.bytes[6] == total));
        if (++copies == HOST_MESSAGE_COPIES) {
            offset += out.bytes[6];
            copies = 0;
        }
    }
    assert(offset == total && !tx.cursor.active);

    // A final fragment arriving early does not complete a drawing with holes.
    host_message_reset(&rx);
    assert(host_message_receive(&rx, packets[0].bytes, packets[0].len) == 0);
    assert(host_message_receive(&rx, packets[n - 1].bytes, packets[n - 1].len) == 0);
    assert(!rx.complete);
    // Conflicting overlap invalidates the transfer, rather than mixing pixels.
    assert(host_message_receive(&rx, packets[first_data].bytes, packets[first_data].len) == 0);
    out = packets[first_data];
    out.bytes[12] ^= 1;
    assert(host_message_receive(&rx, out.bytes, out.len) == -1 && rx.invalid);
    assert(host_message_receive(&rx, packets[first_data].bytes, packets[first_data].len) == 0);
    // A new announcement must discard old coverage.
    out = packets[0];
    out.bytes[16] ^= 1;
    assert(host_message_receive(&rx, out.bytes, out.len) == 0);
    assert(rx.covered == 0 && !rx.invalid && !rx.complete);
    assert(host_message_receive(&rx, packets[first_data].bytes, packets[first_data].len - 1) == -1);
    return 0;
}
