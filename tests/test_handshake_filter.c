#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "pictochat/handshake_filter.h"

int main(void) {
    uint8_t frame[34] = {0};
    const uint8_t allowed[] = {0x00, 0x10, 0x20, 0x30, 0xa0, 0xb0, 0xc0};
    for (size_t i = 0; i < sizeof(allowed); ++i) {
        frame[0] = allowed[i];
        assert(is_handshake_frame(frame, sizeof(frame)));
        for (size_t len = 0; len < 24; ++len)
            assert(!is_handshake_frame(frame, len));
    }
    frame[0] = 0x80;
    assert(!is_handshake_frame(frame, sizeof(frame))); // beacon
    frame[0] = 0x40;
    assert(!is_handshake_frame(frame, sizeof(frame))); // probe
    frame[0] = 0x28;
    assert(!is_handshake_frame(frame, sizeof(frame))); // MP poll
    frame[0] = 0xd4;
    assert(!is_handshake_frame(frame, sizeof(frame))); // ACK
    return 0;
}
