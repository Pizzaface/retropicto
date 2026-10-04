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

#define HOST_PROFILE_NAME_OFFSET 8u
#define HOST_PROFILE_NAME_UNITS 10u
#define HOST_PROFILE_COLOUR_OFFSET 80u

// UTF-16LE nickname; clears the unused part of the 20-byte field.
static inline bool host_profile_set_name(uint8_t profile[84], const uint_least16_t *name,
                                         size_t units) {
    if (units > HOST_PROFILE_NAME_UNITS)
        return false;
    memset(profile + HOST_PROFILE_NAME_OFFSET, 0, HOST_PROFILE_NAME_UNITS * 2);
    for (size_t i = 0; i < units; ++i) {
        profile[HOST_PROFILE_NAME_OFFSET + 2 * i] = (uint8_t)name[i];
        profile[HOST_PROFILE_NAME_OFFSET + 2 * i + 1] = (uint8_t)(name[i] >> 8);
    }
    return true;
}

// DS favourite colour index 0..15 (captured profiles: 11 blue, 15 magenta).
static inline bool host_profile_set_colour(uint8_t profile[84], unsigned colour) {
    if (colour > 15)
        return false;
    profile[HOST_PROFILE_COLOUR_OFFSET] = (uint8_t)colour;
    return true;
}

// Complete identity from scratch: type 3, stage 0, halfword-swapped MAC, empty
// name/bio, colour, birthday month/day. Callers then set name/bio.
static inline bool host_profile_init(uint8_t profile[84], const uint8_t mac[6], unsigned colour,
                                     unsigned month, unsigned day) {
    if (colour > 15 || month < 1 || month > 12 || day < 1 || day > 31)
        return false;
    memset(profile, 0, 84);
    profile[0] = 3;
    for (unsigned i = 0; i < 6; ++i)
        profile[2 + i] = mac[i ^ 1];
    profile[HOST_PROFILE_COLOUR_OFFSET] = (uint8_t)colour;
    profile[82] = (uint8_t)month;
    profile[83] = (uint8_t)day;
    return true;
}
