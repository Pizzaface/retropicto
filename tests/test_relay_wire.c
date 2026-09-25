#include <assert.h>
#include "pictochat/relay_wire.h"

static uint8_t payload[RELAY_MAX_PAYLOAD];
int main(void) {
    uint8_t h[RELAY_HEADER]; unsigned kind=99; uint32_t seq=0; size_t len=0;
    for (size_t i=0;i<sizeof(payload);++i) payload[i]=(uint8_t)(i*17);
    assert(relay_hash((const uint8_t *)"hello",5)==0x4f9f2cabu);
    assert(relay_header(h,RELAY_STATE,0x12345678,payload,92));
    assert(!memcmp(h,"PCTR\1\1\134\0\170\126\064\022",12));
    assert(relay_parse_header(h,&kind,&seq,&len));
    assert(kind==RELAY_STATE && seq==0x12345678 && len==92);
    assert(relay_get32(h+12)==relay_hash(payload,len));
    payload[8]^=1;
    assert(relay_get32(h+12)!=relay_hash(payload,len));
    assert(!relay_header(h,RELAY_STATE,0,payload,91));
    assert(!relay_header(h,RELAY_ACK,0,payload,9));
    assert(!relay_header(h,99,0,payload,92));
    assert(!relay_header(h,RELAY_DRAWING,1,payload,28+36));
    assert(!relay_header(h,RELAY_DRAWING,1,payload,RELAY_MAX_PAYLOAD+1));
    for (unsigned rows=1; rows<=10; ++rows) {
        size_t n=28+36+rows*1024;
        assert(relay_header(h,RELAY_DRAWING,rows,payload,n));
        assert(relay_parse_header(h,&kind,&seq,&len));
        assert(kind==RELAY_DRAWING && seq==rows && len==n);
        assert(!relay_header(h,RELAY_DRAWING,rows,payload,n-1));
    }
    assert(relay_header(h,RELAY_ACK,7,payload,8));
    h[4]=2; assert(!relay_parse_header(h,&kind,&seq,&len));
    h[4]=1; h[0]='x'; assert(!relay_parse_header(h,&kind,&seq,&len));
    h[0]='P'; h[6]=255; h[7]=255; assert(!relay_parse_header(h,&kind,&seq,&len));
    assert(!relay_parse_header(NULL,&kind,&seq,&len));
    assert(!relay_header(h,RELAY_STATE,0,NULL,92));
    return 0;
}
