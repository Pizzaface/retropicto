// nintendo.h — Nintendo OUI (MAC prefix) table and DS local-wireless notes.
//
// The Nintendo DS family transmits PictoChat / local multiplayer over raw
// IEEE 802.11 data + beacon frames. Every DS radio's MAC starts with a
// Nintendo-assigned OUI. Filtering on these prefixes is how we cheaply pull DS
// traffic out of the noise of a normal 2.4 GHz environment.
//
// The DS ALSO stamps its OUI (00:09:BF) inside the vendor-specific information
// element (tag 221) of its beacons — see NDS_VENDOR_OUI below.

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// OUIs seen on DS / DS Lite / DSi wireless radios. Not exhaustive — Nintendo
// holds dozens of blocks — but covers the common PictoChat-era hardware.
// Order doesn't matter; lookup is a linear scan (the list is short and this
// runs on a filtered fast path).
static const uint8_t NINTENDO_OUIS[][3] = {
    {0x00, 0x09, 0xBF}, // original DS (also used in the beacon vendor IE)
    {0x00, 0x16, 0x56},
    {0x00, 0x17, 0xAB},
    {0x00, 0x19, 0x1D},
    {0x00, 0x1A, 0xE9},
    {0x00, 0x1B, 0x7A},
    {0x00, 0x1B, 0xEA},
    {0x00, 0x1C, 0xBE},
    {0x00, 0x1D, 0xBC},
    {0x00, 0x1E, 0x35},
    {0x00, 0x1F, 0x32},
    {0x00, 0x21, 0x47},
    {0x00, 0x21, 0xBD},
    {0x00, 0x22, 0x4C},
    {0x00, 0x22, 0xAA},
    {0x00, 0x22, 0xD7},
    {0x00, 0x23, 0x31},
    {0x00, 0x23, 0xCC},
    {0x00, 0x24, 0x1E},
    {0x00, 0x24, 0x44},
    {0x00, 0x24, 0xF3},
    {0x00, 0x25, 0xA0},
    {0x00, 0x26, 0x59},
    {0x00, 0x27, 0x09},
    {0x04, 0x03, 0xD6},
    {0xE8, 0x4E, 0xCE}, // later Nintendo hardware; harmless to include
};

static const size_t NINTENDO_OUI_COUNT =
    sizeof(NINTENDO_OUIS) / sizeof(NINTENDO_OUIS[0]);

// The OUI Nintendo places inside the beacon vendor-specific IE (tag 221).
static const uint8_t NDS_VENDOR_OUI[3] = {0x00, 0x09, 0xBF};

// Returns true if the 6-byte MAC at `mac` starts with a known Nintendo OUI.
static inline bool mac_is_nintendo(const uint8_t *mac) {
    for (size_t i = 0; i < NINTENDO_OUI_COUNT; i++) {
        if (mac[0] == NINTENDO_OUIS[i][0] &&
            mac[1] == NINTENDO_OUIS[i][1] &&
            mac[2] == NINTENDO_OUIS[i][2]) {
            return true;
        }
    }
    return false;
}
