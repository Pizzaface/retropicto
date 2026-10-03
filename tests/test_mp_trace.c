#include <assert.h>
#include <stdio.h>
#include <string.h>
// The delivery witness must retain ACKs/replies past the default 64 frames.
#define MP_TRACE_FRAMES 128u
#include "pictochat/mp_trace.h"

static void check_pair(const uint8_t host[6], const uint8_t client[6]) {
    uint8_t assoc[34] = {0x10}, cmd[142] = {0x28,2};
    uint8_t ack[32] = {0x18,2}, reply[28] = {0x58,1};
    memcpy(assoc+4,client,6); memcpy(assoc+10,host,6);
    memcpy(cmd+4,"\x03\x09\xbf\0\0\0",6); memcpy(cmd+10,host,6);
    memcpy(ack+4,"\x03\x09\xbf\0\0\x03",6); memcpy(ack+10,host,6);
    memcpy(reply+4,host,6); memcpy(reply+10,client,6);
    memcpy(reply+16,"\x03\x09\xbf\0\0\x10",6);
    mp_trace_t s = {0};
    assert(!mp_trace_select(&s,cmd,sizeof cmd,host)); // not armed
    assoc[26]=1; // failed association must not arm
    assert(!mp_trace_select(&s,assoc,sizeof assoc,host));
    assert(s.remaining==0);
    assoc[26]=0;
    assert(!mp_trace_select(&s,assoc,29,host)); // no complete fixed body + trailer
    assert(!mp_trace_select(&s,assoc,sizeof assoc,host));
    assert(s.remaining==MP_TRACE_FRAMES);
    assert(mp_trace_select(&s,cmd,sizeof cmd,host));
    assert(mp_trace_select(&s,ack,sizeof ack,host));
    assert(mp_trace_select(&s,reply,sizeof reply,host));
    uint8_t data_reply[136] = {0};
    memcpy(data_reply,reply,sizeof reply);
    assert(mp_trace_select(&s,data_reply,sizeof data_reply,host));
    assert(mp_reply_payload_bytes(data_reply,sizeof data_reply,host)==108);
    unsigned left=s.remaining;
    assoc[1]=8; // retry of same response must not extend capture
    assert(!mp_trace_select(&s,assoc,sizeof assoc,host));
    assert(s.remaining==left);
    cmd[10]^=1; assert(!mp_trace_select(&s,cmd,sizeof cmd,host)); cmd[10]^=1;
    reply[10]^=1; assert(!mp_trace_select(&s,reply,sizeof reply,host)); reply[10]^=1;
    assert(!mp_trace_select(&s,cmd,23,host));
    while(s.remaining) assert(mp_trace_select(&s,cmd,sizeof cmd,host));
    assert(!mp_trace_select(&s,ack,sizeof ack,host));
    data_reply[26] = 2; // Client drawing traffic must survive the handshake budget.
    assert(mp_trace_select(&s,data_reply,sizeof data_reply,host));
    unsigned client_left = s.app_remaining;
    data_reply[10] ^= 1;
    assert(!mp_trace_select(&s,data_reply,sizeof data_reply,host));
    assert(s.app_remaining == client_left);
    data_reply[10] ^= 1;
    cmd[30] = 2;
    unsigned app_left = s.app_remaining;
    assert(app_left > 0);
    while (s.app_remaining) assert(mp_trace_select(&s,cmd,sizeof cmd,host));
    assert(!mp_trace_select(&s,cmd,sizeof cmd,host));
    cmd[30] = 0;
    assoc[22]=0x10; // fresh association re-arms
    assert(!mp_trace_select(&s,assoc,sizeof assoc,host));
    assert(s.remaining==MP_TRACE_FRAMES);
}

int main(void) {
    assert(MP_TRACE_FRAMES == 128u);
    const uint8_t c6[6] = {0,9,0xbf,0xc6,0xc6,0xc6};
    const uint8_t jordan[6] = {0,0x22,0xd7,0x39,0xbc,0xa3};
    const uint8_t ash[6] = {0x64,0xb5,0xc6,0x9c,0x60,0xa0};
    check_pair(c6,jordan);
    check_pair(jordan,ash);
    puts("MP trace selection tests passed (C6/Jordan and Jordan/Ash)");
}
