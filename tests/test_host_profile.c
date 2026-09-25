#include <assert.h>
#include "pictochat/host_profile.h"
#include "pictochat/host_identity.h"
#include "host_identity_fixture.h"

int main(void) {
    const uint_least16_t host_profile_bio[] = u"Hi from PICTOBOT!";
    uint8_t profile[84], original[84];
    memcpy(original, own_data0 + 12, 84);
    memcpy(profile, original, 84);
    assert(original[28] == 'C' && original[30] == 'a');
    assert(host_profile_set_bio(profile, u"A\u00e9", 2));
    assert(profile[28] == 'A' && profile[29] == 0);
    assert(profile[30] == 0xe9 && profile[31] == 0);
    for (unsigned i = 32; i < 80; ++i) assert(profile[i] == 0);
    assert(memcmp(profile, original, 28) == 0);
    assert(memcmp(profile + 80, original + 80, 4) == 0);
    uint_least16_t maximum[27];
    for (unsigned i = 0; i < 27; ++i) maximum[i] = 'Z';
    assert(host_profile_set_bio(profile, maximum, 26));
    uint8_t saved[84]; memcpy(saved, profile, 84);
    assert(!host_profile_set_bio(profile, maximum, 27));
    assert(memcmp(saved, profile, 84) == 0);
    assert(host_profile_set_bio(profile, host_profile_bio,
           sizeof(host_profile_bio) / sizeof(host_profile_bio[0]) - 1));
    host_identity_t state;
    host_id_packet_t packet;
    host_identity_reset(&state, 1, 2);
    for (unsigned stage = 0; stage < 2; ++stage) {
        assert(host_identity_next(&state, profile, &packet));
        assert(host_identity_next(&state, profile, &packet));
        assert(packet.bytes[13] == stage);
        assert(memcmp(packet.bytes + 40, profile + 28, 52) == 0);
    }
    assert(host_profile_set_bio(profile, u"", 0));
    for (unsigned i = 28; i < 80; ++i) assert(profile[i] == 0);
    return 0;
}
