#include <assert.h>
#include "pictochat/host_sequence.h"

int main(void) {
    host_sequence_t seq;

    // No admission: empty once, then heartbeats forever.
    host_sequence_reset(&seq);
    assert(host_sequence_next(&seq, false) == HOST_FRAME_EMPTY);
    for (int i=0; i<1000; ++i)
        assert(host_sequence_next(&seq, false) == HOST_FRAME_HEARTBEAT);

    // Valid admission after empty: 7 roster polls, then application state takes over.
    host_sequence_reset(&seq);
    assert(host_sequence_next(&seq, false) == HOST_FRAME_EMPTY);
    assert(host_sequence_next(&seq, true) == HOST_FRAME_ROSTER);
    for (int i=0; i<6; ++i)
        assert(host_sequence_next(&seq, true) == HOST_FRAME_ROSTER);
    for (int cycle=0; cycle<40; ++cycle)
        assert(host_sequence_next(&seq, true) == HOST_FRAME_SESSION);

    // Duplicate admission does not reset: mid-cycle, admission stays true.
    (void)host_sequence_next(&seq, true);
    (void)host_sequence_next(&seq, true);
    host_frame_t after_mid = host_sequence_next(&seq, true);
    assert(after_mid != HOST_FRAME_EMPTY);
    assert(after_mid != HOST_FRAME_ROSTER);

    // Rejoin clears: reset returns empty, then heartbeats until admitted.
    host_sequence_reset(&seq);
    assert(host_sequence_next(&seq, false) == HOST_FRAME_EMPTY);
    assert(host_sequence_next(&seq, false) == HOST_FRAME_HEARTBEAT);
    assert(host_sequence_next(&seq, true) == HOST_FRAME_ROSTER);

    // An admission already received before the task's first cycle is preserved.
    host_sequence_reset(&seq);
    assert(host_sequence_next(&seq, true) == HOST_FRAME_EMPTY);
    assert(host_sequence_next(&seq, true) == HOST_FRAME_ROSTER);

    return 0;
}
