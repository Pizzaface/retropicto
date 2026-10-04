#include <assert.h>
#include <string.h>
#include "pictochat/mp_reply.h"

int main(void) {
    const uint8_t host[6] = {0, 9, 0xbf, 0xc6, 0xc6, 0xc6};
    const uint8_t group[6] = {3, 9, 0xbf, 0, 0, 0x10};
    uint8_t f[136] = {0x58, 0x11};
    memcpy(f + 4, host, 6);
    memcpy(f + 16, group, 6);
    assert(mp_reply_payload_bytes(f, 28, host) == 0);
    f[0] = 0x18;
    assert(mp_reply_payload_bytes(f, 30, host) == 2);
    assert(mp_reply_payload_bytes(f, 136, host) == 108);
    for (size_t n = 0; n < 28; ++n)
        assert(mp_reply_payload_bytes(f, n, host) == -1);
    f[4] ^= 1;
    assert(mp_reply_payload_bytes(f, sizeof(f), host) == -1);
    f[4] ^= 1;
    f[16] ^= 1;
    assert(mp_reply_payload_bytes(f, sizeof(f), host) == -1);
    f[16] ^= 1;
    f[0] = 0xb0; // authentication is not an MP reply
    assert(mp_reply_payload_bytes(f, sizeof(f), host) == -1);
    f[0] = 0x98; // QoS would have a different header size; not this protocol
    assert(mp_reply_payload_bytes(f, sizeof(f), host) == -1);
    return 0;
}
