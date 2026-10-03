#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HOST_PROFILE_BIO_OFFSET 28u
#define HOST_PROFILE_BIO_UNITS 26u

// Preserve the rest of the captured 84-byte identity and clear unused space.
static inline bool host_profile_set_bio(uint8_t profile[84], const uint_least16_t *bio,
                                        size_t units) {
    if (units > HOST_PROFILE_BIO_UNITS)
        return false;
    memset(profile + HOST_PROFILE_BIO_OFFSET, 0, HOST_PROFILE_BIO_UNITS * 2);
    for (size_t i = 0; i < units; ++i) {
        profile[HOST_PROFILE_BIO_OFFSET + 2 * i] = (uint8_t)bio[i];
        profile[HOST_PROFILE_BIO_OFFSET + 2 * i + 1] = (uint8_t)(bio[i] >> 8);
    }
    return true;
}
