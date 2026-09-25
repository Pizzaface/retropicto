#include <assert.h>
#include <string.h>
#include "pictochat/host_admission.h"

int main(void) {
    const uint8_t host[6] = {0,9,0xbf,0xc6,0xc6,0xc6};
    const uint8_t client[6] = {0,0x22,0xd7,0x39,0xbc,0xa3};
    uint8_t f[140] = {0x18,0x11};
    memcpy(f+4,host,6); memcpy(f+10,client,6);
    memcpy(f+16,"\x03\x09\xbf\0\0\x10",6);
    f[24]=0x34; f[25]=0x8c; f[26]=6; f[28]=0x68;

    // Valid type-6 admission.
    assert(host_admission_reply(f,136,host,client));

    // Truncated lengths rejected.
    for (size_t n=0; n<136; ++n) assert(!host_admission_reply(f,n,host,client));
    assert(!host_admission_reply(f,140,host,client));

    // Bit-flip robustness: corrupting any critical byte invalidates admission.
    const unsigned offsets[] = {0,1,4,10,16,26,27,28,29};
    for (unsigned i=0; i<sizeof offsets/sizeof offsets[0]; ++i) {
        f[offsets[i]] ^= 1;
        assert(!host_admission_reply(f,136,host,client));
        f[offsets[i]] ^= 1;
    }

    // Type-2 identity frame rejected.
    f[26]=2; assert(!host_admission_reply(f,136,host,client));

    // Trailer-masquerading short frame rejected.
    f[0]=0x58; f[26]=6;
    assert(!host_admission_reply(f,28,host,client));

    return 0;
}
