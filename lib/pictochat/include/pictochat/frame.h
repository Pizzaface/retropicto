#pragma once
#include "host_identity.h"
#include "host_poll_fields.h"

/* Raw 802.11 frames, excluding radiotap and FCS. All wire integers are LE.
 * Return encoded length or zero for insufficient capacity/invalid arguments.
 * Input buffers must not overlap output. client may be NULL for an empty slot. */
#define PICTOCHAT_FRAME_MAX 302u
size_t pictochat_frame_empty(uint8_t *out, size_t capacity, const uint8_t host[6]);
size_t pictochat_frame_members(uint8_t *out, size_t capacity, const uint8_t host[6],
                               const uint8_t *client, uint16_t kind,
                               host_poll_fields_result_t fields, uint32_t magic, uint16_t sequence,
                               bool admitted);
size_t pictochat_frame_app(uint8_t *out, size_t capacity, const uint8_t host[6],
                           const host_id_packet_t *app, uint16_t sequence);
size_t pictochat_frame_ack(uint8_t *out, size_t capacity, const uint8_t host[6]);
/* Multi-user encoders. Members are indexed by AID (1..15); zero means vacant.
 * Poll exactly one AID at a time; application footer targets the same AID.
 * The legacy helpers above retain their AID-1 wire format. */
size_t pictochat_frame_room_members(uint8_t *out, size_t capacity, const uint8_t host[6],
                                    const uint8_t members[16][6], uint16_t kind,
                                    host_poll_fields_result_t fields, uint32_t magic,
                                    uint16_t sequence, bool admitted);
size_t pictochat_frame_target_app(uint8_t *out, size_t capacity, const uint8_t host[6],
                                  const host_id_packet_t *app, uint16_t sequence, uint16_t target);
