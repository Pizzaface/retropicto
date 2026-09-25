#include <assert.h>
#include "pictochat/frame.h"
static const uint8_t host[6] = {0,9,0xbf,0xc6,0xc6,0xc6};
static const uint8_t client[6] = {0,0x22,0xd7,0x39,0xbc,0xa3};
int main(void) {
    uint8_t frame[PICTOCHAT_FRAME_MAX];
    memset(frame, 0xa5, sizeof(frame));
    assert(!pictochat_frame_empty(frame, 29, host) && frame[0] == 0xa5);
    assert(pictochat_frame_empty(frame, sizeof(frame), host) == 30);
    static const uint8_t empty[30] = {
        0x28,2,0xf0,0,3,9,0xbf,0,0,0,0,9,0xbf,0xc6,0xc6,0xc6,
        0,9,0xbf,0xc6,0xc6,0xc6,0,0,0xe6,3,0,0,0,0};
    assert(!memcmp(frame, empty, sizeof(empty)));
    host_poll_fields_result_t fields = {.bitmask=2, .wm=0x9c34};
    assert(pictochat_frame_members(frame,sizeof(frame),host,client,4,fields,0x12345678,0xfffe,true)==138);
    assert(frame[2] == 0xe0 && frame[3] == 4 && frame[28] == 0x34 && frame[29] == 0x9c);
    assert(frame[30] == 4 && frame[32] == 104 && frame[34] == 0x78 && frame[37] == 0x12);
    for (unsigned i=0;i<6;++i) { assert(frame[38+i]==host[i^1]); assert(frame[44+i]==client[i^1]); }
    for (unsigned i=50;i<134;++i) assert(frame[i]==0);
    assert(frame[134]==0xfe && frame[135]==0xff && frame[136]==2 && frame[137]==0);
    assert(pictochat_frame_members(frame,sizeof(frame),host,NULL,5,fields,0,0,false)==138);
    assert(frame[44]==0 && frame[136]==0);
    assert(!pictochat_frame_members(frame,sizeof(frame),host,client,6,fields,0,0,true));
    host_id_packet_t app = {.len=20, .bytes={1,0,20,0}};
    assert(pictochat_frame_app(frame,sizeof(frame),host,&app,0xffff)==54);
    assert(frame[28]==10 && frame[29]==0x9d);
    assert(!memcmp(frame+30,app.bytes,20));
    assert(frame[50]==0xff && frame[51]==0xff && frame[52]==2 && frame[53]==0);
    app.bytes[0]=2;
    assert(pictochat_frame_app(frame,sizeof(frame),host,&app,0)==54);
    assert(frame[29]==0x1e);
    assert(!pictochat_frame_app(frame,53,host,&app,0));
    app.len=269; assert(!pictochat_frame_app(frame,sizeof(frame),host,&app,0));
    app.len=3; assert(!pictochat_frame_app(frame,sizeof(frame),host,&app,0));
    assert(pictochat_frame_ack(frame,sizeof(frame),host)==28);
    assert(frame[0]==0x18 && frame[2]==0 && frame[9]==3 && frame[24]==0x82);
    return 0;
}
