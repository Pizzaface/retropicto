#include <assert.h>
#include "pictochat/host_relay_repeat.h"

int main(void) {
    uint8_t app[20] = {0, 0, 20, 0, 1};
    host_relay_repeat_t s = {0};
    assert(!host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 1000));
    s = (host_relay_repeat_t){.valid = true,
                              .generation = 1,
                              .sequence = 7,
                              .delivered_us = 1000,
                              .packet = {.len = sizeof(app)}};
    memcpy(s.packet.bytes, app, sizeof(app));
    s.packet.bytes[0] = 1; // A committed announcement relay changes only type.
    assert(host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 1001));
    assert(host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 100999));
    assert(!host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 101000));
    assert(!host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 999));
    assert(!host_relay_repeat_skip(&s, 2, 7, app, sizeof(app), 1001));
    assert(!host_relay_repeat_skip(&s, 1, 8, app, sizeof(app), 1001));
    assert(!host_relay_repeat_skip(&s, 1, 7, app, sizeof(app) - 1, 1001));
    app[16] = 1; // New transfer token despite reused WM sequence.
    assert(!host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 1001));
    app[16] = 0;
    app[0] = s.packet.bytes[0] = 2;
    assert(host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 1001));
    app[8] = 1; // New fragment offset must never be suppressed.
    assert(!host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 1001));
    app[8] = 0;
    app[12] = 1;
    assert(!host_relay_repeat_skip(&s, 1, 7, app, sizeof(app), 1001));
    assert(!host_relay_repeat_skip(&s, 1, 7, NULL, 0, 1001));
    return 0;
}
