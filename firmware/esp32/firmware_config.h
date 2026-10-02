#ifndef RETROPICTO_FIRMWARE_CONFIG_H
#define RETROPICTO_FIRMWARE_CONFIG_H

// Compile-time mode and board defaults. platformio.ini overrides selected values.
// Pick exactly one mode. SNIFFER_MODE is normally set per-board by a build flag
// in platformio.ini; the default here keeps a bare `pio run` doing STREAM.
#define MODE_DISCOVERY 0
#define MODE_STREAM    1
#define MODE_JOIN      2
#define MODE_HOST      3
#define MODE_SERIAL_MGMT 4 // fixed-channel, USB-only handshake diagnostics
#ifndef SNIFFER_MODE
#define SNIFFER_MODE   MODE_STREAM
#endif

#ifndef PICTOCHAT_ONLINE
#define PICTOCHAT_ONLINE 0
#endif

#ifndef PICTOCHAT_GHOST_DEMO
#define PICTOCHAT_GHOST_DEMO 0
#endif
#if PICTOCHAT_ONLINE
#ifndef ONLINE_LOCAL_SLOTS
#define ONLINE_LOCAL_SLOTS 1u
#endif
#define HOST_RADIO_CLIENTS ONLINE_LOCAL_SLOTS
#else
#define HOST_RADIO_CLIENTS (PICTOCHAT_ROOM_CLIENTS - PICTOCHAT_GHOST_DEMO)
#endif
#if PICTOCHAT_GHOST_DEMO
#define GHOST_SLOT (PICTOCHAT_ROOM_CLIENTS - 1u)
#define GHOST_AID 15u
#define GHOST_GENERATION 1u
#endif
#if PICTOCHAT_ONLINE
#define HOST_CHATROOM (online_node()-1)

#else
#ifndef HOST_CHATROOM
#define HOST_CHATROOM  1        // 0..3 = rooms A..D (perfect01 host used B=1)
#endif
_Static_assert(HOST_CHATROOM >= 0 && HOST_CHATROOM <= 3, "Room must be A..D (0..3)");
#endif
#define JOIN_RETRY_MS   700        // resend Auth-Req if no progress within this
#define ASSOC_RESEND_MAX 3         // resend Assoc-Req this many times before re-auth
#define RX_STALL_SECS   6          // watchdog: no Nintendo frame for this long => wedge
#define SEND_PROFILE    1          // 1 = send identity card in reply slots;
                                   // 0 = empty-reply keepalive (isolation test)

// SoftAP the PC joins in STREAM mode.
#define AP_SSID        "pictochat-sniffer"
#define AP_PASS        "dspackets"       // >= 8 chars, or "" for an open AP
#define AP_MAX_CONN    2

// The channel to capture + stream on. Set this to whatever MODE_DISCOVERY told
// you your DSs are using. SoftAP and capture both use it.
#ifndef CAPTURE_CHANNEL
#define CAPTURE_CHANNEL 7
#endif

// UDP: frames are broadcast to <softap-subnet>.255 : UDP_PORT.
#define UDP_PORT       5555

// The ESP32 promiscuous payload usually carries the 4-byte FCS at the end.
// Setting this reflects that in the radiotap FLAGS field so Wireshark accounts
// for it. If your captures show a bogus 4-byte trailer, set this to 0.
#define FCS_AT_END     1

// libpcap link-layer type. 127 = LINKTYPE_IEEE802_11_RADIOTAP.
#define PCAP_LINKTYPE  127

#endif /* RETROPICTO_FIRMWARE_CONFIG_H */
