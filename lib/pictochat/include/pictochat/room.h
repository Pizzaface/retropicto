#pragma once
#include "session.h"
#include "events.h"

/* Single owner, no allocation or I/O. One outstanding radio cycle per room.
 * Storage slots are independent of Wi-Fi AIDs (1..15). Keep off MCU stacks. */
#define PICTOCHAT_ROOM_CLIENTS 4u
typedef struct {
    bool connected, admitted, ghost;
    uint8_t mac[6], aid;
    uint32_t generation;
    pictochat_session_t session;
    uint8_t announcement[20];
    host_id_packet_t identities[2];
    uint8_t identity_announcements[2][20];
    uint32_t versions[2], seen[PICTOCHAT_ROOM_CLIENTS][2];
    uint16_t drawing_targets;
    uint8_t drawing_next;
    bool replay_active, replay_data;
    uint8_t replay_source, replay_stage;
    uint32_t replay_version;
    host_id_packet_t replay[2];
} pictochat_peer_t;
typedef struct {
    pictochat_peer_t peers[PICTOCHAT_ROOM_CLIENTS];
    uint8_t profile[84], next, selected;
    bool outstanding, replay_output;
    uint16_t app_sequence[2], saved_app_sequence;
    uint8_t output_port;
    pictochat_event_handler_t handler;
    void *handler_context;
    bool dispatching;
} pictochat_room_t;

/* Caller retains the drawing until pending is zero. Generations prevent a
 * replacement client from receiving messages queued for a departed member. */
typedef struct {
    uint16_t pending;
    uint32_t generation[PICTOCHAT_ROOM_CLIENTS], token;
} pictochat_delivery_t;
void pictochat_room_delivery(const pictochat_room_t *r, pictochat_delivery_t *delivery, uint32_t token);
/* Queue a host-originated reply to each snapshot recipient when its sender is
 * free. Pass a validated completed drawing; false means retain and retry. */
bool pictochat_room_reply(pictochat_room_t *r, pictochat_delivery_t *delivery,
    const uint8_t announcement[20], const uint8_t *body, size_t len, const uint8_t mac[6]);

/* reset clears handlers too; register once after reset, not on every join. */
void pictochat_room_reset(pictochat_room_t *r, const uint8_t profile[84]);
/* NULL disables notifications. Refused during a cycle or another handler. */
bool pictochat_room_set_handler(pictochat_room_t *r, pictochat_event_handler_t handler,
                                void *context);
/* Membership changes are refused while output is outstanding. Duplicate joins
 * with identical MAC/AID/generation preserve state. Generation must be nonzero. */
bool pictochat_room_join(pictochat_room_t *r, unsigned slot, const uint8_t mac[6],
                         unsigned aid, uint32_t generation, uint32_t token0, uint32_t token1);
/* Virtual member: occupies a storage/roster slot but is never a radio recipient.
 * Profile is an 84-byte ConsoleId, including its halfword-swapped MAC.
 * Change generation to replace a profile; duplicate joins preserve it. */
bool pictochat_room_ghost_join(pictochat_room_t *r, unsigned slot, unsigned aid,
    uint32_t generation, const uint8_t profile[84], uint32_t token0, uint32_t token1);
/* Copy a complete drawing and author it as this ghost. Returns 0 on acceptance,
 * -2 for backpressure (retain/retry), -1 for invalid input/stale generation.
 * No receive event is emitted. SENT events still identify each real recipient. */
int pictochat_room_ghost_send(pictochat_room_t *r, unsigned slot, uint32_t generation,
    const uint8_t announcement[20], const uint8_t *body, size_t len, uint32_t token);
bool pictochat_room_leave(pictochat_room_t *r, unsigned slot);
int pictochat_room_receive(pictochat_room_t *r, unsigned slot, uint32_t generation,
                           const uint8_t *app, size_t len);
bool pictochat_room_prepare(pictochat_room_t *r, unsigned *slot, pictochat_output_t *out);
bool pictochat_room_finish(pictochat_room_t *r, pictochat_tx_result_t result);
/* MACs indexed by AID; index zero is supplied by the frame encoder. */
void pictochat_room_members(const pictochat_room_t *r, uint8_t members[16][6]);
