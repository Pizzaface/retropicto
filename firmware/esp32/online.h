#pragma once
#include <stdbool.h>
#include "pictochat/room.h"

/* Configure after esp_wifi_init, before start; run after radio setup. */
void online_wifi_configure(void);
void online_start(void);
/* Room-owner calls only, outside an outstanding radio cycle. */
bool online_tick(pictochat_room_t *room);
void online_message(const pictochat_event_t *event);
unsigned online_ghost_count(void);
unsigned online_channel(void);
unsigned online_node(void);
