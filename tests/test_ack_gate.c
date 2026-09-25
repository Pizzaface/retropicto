#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "pictochat/ack_gate.h"

static ack_gate_t gate;
static atomic_uint winners;
static void *contend(void *unused) {
    (void)unused;
    for (int i=0; i<1000; ++i)
        if (ack_gate_claim(&gate)) atomic_fetch_add(&winners,1);
    return NULL;
}

int main(void) {
    // Either callback or fallback can win, but never both in one cycle.
    ack_gate_begin(&gate);
    assert(!ack_gate_replied(&gate));
    assert(ack_gate_claim(&gate));
    assert(!ack_gate_replied(&gate)); // fallback does not establish delivery
    assert(!ack_gate_reply(&gate)); // late reply cannot send a second ACK
    assert(ack_gate_replied(&gate));
    assert(!ack_gate_claim(&gate));
    assert(!ack_gate_claim(&gate)); // duplicate reply cannot send another ACK
    ack_gate_begin(&gate);
    assert(!ack_gate_replied(&gate));
    assert(ack_gate_reply(&gate)); // next cycle permits one ACK again
    assert(ack_gate_replied(&gate));
    for (int cycle=0; cycle<100; ++cycle) {
        pthread_t threads[4];
        ack_gate_begin(&gate);
        atomic_store(&winners,0);
        for (int i=0; i<4; ++i) assert(pthread_create(&threads[i],NULL,contend,NULL)==0);
        for (int i=0; i<4; ++i) assert(pthread_join(threads[i],NULL)==0);
        assert(atomic_load(&winners)==1);
    }
    puts("ACK gate tests passed (100 contested cycles)");
}
