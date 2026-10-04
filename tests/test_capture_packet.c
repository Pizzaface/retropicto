#include "../firmware/esp32/capture_packet.h"

#include <assert.h>
#include <stdio.h>

static void test_frame_control(void) {
    assert(WLAN_FC_OFFSET == 0);
    assert(WLAN_ADDR1_OFF == 4);
    assert(WLAN_ADDR2_OFF == 10);
    assert(WLAN_ADDR3_OFF == 16);

    // Cover every bit, including protocol-version and unrelated flag bits.
    for (unsigned first = 0; first <= UINT8_MAX; ++first) {
        for (unsigned second = 0; second <= UINT8_MAX; ++second) {
            const uint8_t frame[] = {(uint8_t)first, (uint8_t)second};
            assert(fc_type(frame) == (first / 4) % 4);
            assert(fc_subtype(frame) == first / 16);
            assert(fc_to_ds(frame) == (second % 2 != 0));
            assert(fc_from_ds(frame) == ((second / 2) % 2 != 0));
        }
    }
}

static void test_header_lengths(void) {
    uint8_t frame[40] = {0};

    // Short input is returned unchanged without inspecting frame bytes.
    for (size_t len = 0; len < 24; ++len) {
        assert(wlan_header_len(NULL, len) == len);
    }

    // Cover all types/subtypes, DS directions, and truncation boundaries.
    for (unsigned type = 0; type < 4; ++type) {
        for (unsigned subtype = 0; subtype < 16; ++subtype) {
            for (unsigned direction = 0; direction < 4; ++direction) {
                frame[0] = (uint8_t)((subtype << 4) | (type << 2));
                frame[1] = (uint8_t)direction;
                size_t expected_header = 24;
                if (type == 2) {
                    if (direction == 3)
                        expected_header += 6;
                    if (subtype >= 8)
                        expected_header += 2;
                }
                for (size_t len = 0; len <= sizeof(frame); ++len) {
                    const size_t expected = len < expected_header ? len : expected_header;
                    assert(wlan_header_len(frame, len) == expected);
                }
                // Other FC flags must not change the existing length model.
                frame[0] |= 0x03;
                frame[1] |= 0xfc;
                assert(wlan_header_len(frame, sizeof(frame)) == expected_header);
            }
        }
    }

    // Preserve the existing capture helper's capped 24-byte model for control
    // frames, rather than introducing a separate ACK/control frame parser.
    frame[0] = 0xd4;
    frame[1] = 0;
    assert(wlan_header_len(frame, 10) == 10);
    assert(wlan_header_len(frame, sizeof(frame)) == 24);
}

static void test_radiotap(void) {
    static const struct {
        uint8_t channel;
        int8_t rssi;
        uint8_t bytes[15];
    } cases[] = {
        {1,
         -42,
         {0x00, 0x00, 0x0f, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x6c, 0x09, 0x80, 0x00,
          0xd6}},
        {7,
         -67,
         {0x00, 0x00, 0x0f, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x8a, 0x09, 0x80, 0x00,
          0xbd}},
        {13,
         INT8_MIN,
         {0x00, 0x00, 0x0f, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x10, 0x00, 0xa8, 0x09, 0x80, 0x00,
          0x80}},
        {14,
         INT8_MAX,
         {0x00, 0x00, 0x0f, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x10, 0x00, 0xb4, 0x09, 0x80, 0x00,
          0x7f}},
        {7,
         0,
         {0x00, 0x00, 0x0f, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x8a, 0x09, 0x80, 0x00,
          0x00}},
    };

    assert(RT_LEN == 15);
    assert(RT_PRESENT == 0x2a);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t storage[32];
        uint8_t expected[15];
        memset(storage, 0xa5, sizeof(storage));
        memcpy(expected, cases[i].bytes, sizeof(expected));
        // The default is FCS present; keep the same fixture valid when the
        // firmware configuration explicitly disables capture FCS accounting.
        if (!FCS_AT_END)
            expected[8] = 0x00;

        // Offset output also verifies that no native-alignment assumption leaks
        // into the byte-oriented wire format.
        assert(build_radiotap(storage + 1, cases[i].channel, cases[i].rssi) == 15);
        assert(memcmp(storage + 1, expected, sizeof(expected)) == 0);
        assert(storage[0] == 0xa5);
        for (size_t tail = 16; tail < sizeof(storage); ++tail) {
            assert(storage[tail] == 0xa5);
        }
    }
}

int main(void) {
    test_frame_control();
    test_header_lengths();
    test_radiotap();
    puts("Capture packet tests passed");
    return 0;
}
