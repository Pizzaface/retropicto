#include "pictochat/frame.h"

static void u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void header(uint8_t *out, size_t size, const uint8_t host[6], bool ack) {
    static const uint8_t group[6] = {3, 9, 0xbf, 0, 0, 0};
    memset(out, 0, size);
    out[0] = ack ? 0x18 : 0x28;
    out[1] = 2;
    memcpy(out + 4, group, 6);
    if (ack)
        out[9] = 3;
    memcpy(out + 10, host, 6);
    memcpy(out + 16, host, 6);
    if (!ack) {
        u16(out + 2, 0x04e0);
        u16(out + 24, 998);
    }
}

size_t pictochat_frame_empty(uint8_t *out, size_t capacity, const uint8_t host[6]) {
    if (!out || !host || capacity < 30)
        return 0;
    header(out, 30, host, false);
    u16(out + 2, 0x00f0);
    return 30;
}

size_t pictochat_frame_members(uint8_t *out, size_t capacity, const uint8_t host[6],
                               const uint8_t *client, uint16_t kind,
                               host_poll_fields_result_t fields, uint32_t magic, uint16_t sequence,
                               bool admitted) {
    if (!out || !host || capacity < 138 || (kind != 4 && kind != 5))
        return 0;
    header(out, 138, host, false);
    u16(out + 26, fields.bitmask);
    u16(out + 28, fields.wm);
    u16(out + 30, kind);
    u16(out + 32, 104);
    for (unsigned i = 0; i < 4; ++i)
        out[34 + i] = (uint8_t)(magic >> (8 * i));
    for (unsigned i = 0; i < 6; ++i) {
        out[38 + i] = host[i ^ 1];
        if (client)
            out[44 + i] = client[i ^ 1];
    }
    host_poll_footer(out + 134, sequence, fields.bitmask, admitted);
    return 138;
}

size_t pictochat_frame_app(uint8_t *out, size_t capacity, const uint8_t host[6],
                           const host_id_packet_t *app, uint16_t sequence) {
    if (!out || !host || !app || app->len < 4 || app->len > sizeof(app->bytes) || (app->len & 1) ||
        capacity < 34u + app->len || (app->bytes[2] | ((unsigned)app->bytes[3] << 8)) != app->len)
        return 0;
    size_t size = 34u + app->len;
    header(out, size, host, false);
    u16(out + 26, 2);
    uint16_t wm = (app->len / 2) | (app->bytes[0] == 2 ? 0x1e00 : 0x1d00);
    if (sequence & 1)
        wm |= 0x8000;
    u16(out + 28, wm);
    memcpy(out + 30, app->bytes, app->len);
    host_poll_footer(out + 30 + app->len, sequence, 2, true);
    return size;
}

size_t pictochat_frame_ack(uint8_t *out, size_t capacity, const uint8_t host[6]) {
    if (!out || !host || capacity < 28)
        return 0;
    header(out, 28, host, true);
    u16(out + 24, 0x82);
    return 28;
}

size_t pictochat_frame_room_members(uint8_t *out, size_t capacity, const uint8_t host[6],
                                    const uint8_t members[16][6], uint16_t kind,
                                    host_poll_fields_result_t fields, uint32_t magic,
                                    uint16_t sequence, bool admitted) {
    if (!members || (fields.bitmask & 1) ||
        (fields.bitmask && (fields.bitmask & (fields.bitmask - 1))))
        return 0;
    size_t len =
        pictochat_frame_members(out, capacity, host, NULL, kind, fields, magic, sequence, admitted);
    if (len)
        for (unsigned aid = 1; aid < 16; ++aid)
            for (unsigned j = 0; j < 6; ++j)
                out[38 + 6 * aid + j] = members[aid][j ^ 1];
    return len;
}

size_t pictochat_frame_target_app(uint8_t *out, size_t capacity, const uint8_t host[6],
                                  const host_id_packet_t *app, uint16_t sequence, uint16_t target) {
    if (!target || (target & 1) || (target & (target - 1)))
        return 0;
    size_t len = pictochat_frame_app(out, capacity, host, app, sequence);
    if (len) {
        u16(out + 26, target);
        u16(out + len - 2, target);
    }
    return len;
}
