#include <assert.h>
#include <string.h>
#include "pictochat/host_poll_fields.h"

int main(void) {
    // Literal footers from perfect01.pcap, parse_pcap indexes 471293 / 471296.
    // Before admission, the granted heartbeat footer is 84160000. After the
    // type-6 reply, the granted roster footer is 89160200, not 89160000.
    uint8_t footer[4];
    const uint8_t startup[] = {0x84,0x16,0,0};
    const uint8_t roster[] = {0x89,0x16,2,0};
    host_poll_footer(footer, 0x1684, 2, false);
    assert(memcmp(footer, startup, 4) == 0);
    host_poll_footer(footer, 0x1689, 2, true);
    assert(memcmp(footer, roster, 4) == 0);
    host_poll_footer(footer, 0x1689, 0, true);
    assert(footer[2] == 0 && footer[3] == 0);
    host_poll_fields_t state;
    host_poll_fields_reset(&state);
    // First type-5 grant is low-WM; subsequent grants alternate low/high WM.
    for (unsigned i=0; i<1200; ++i) {
        host_poll_fields_result_t f = host_poll_fields_next(&state, true);
        assert(f.bitmask == (i % 3 == 0 ? 2 : 0));
        assert(f.wm == (i % 2 == 0 ? 0x1c34 : 0x9c34));
    }
    (void)host_poll_fields_next(&state, true);
    host_poll_fields_reset(&state); // new join starts identically, regardless of counter
    host_poll_fields_result_t f = host_poll_fields_next(&state, true);
    assert(f.bitmask == 2 && f.wm == 0x1c34);
    host_poll_fields_reset(&state);
    for (unsigned i=0; i<12; ++i) {
        f = host_poll_fields_next(&state, false);
        assert(f.bitmask == 0); // never grant without an active client
        assert(f.wm == (i % 2 == 0 ? 0x1c34 : 0x9c34));
    }
}
