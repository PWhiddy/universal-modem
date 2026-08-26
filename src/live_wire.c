#include "live_wire.h"

#include <string.h>

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | bytes[3];
}

int um_live_wire_encode(um_live_wire_type type, uint32_t session_id,
                        uint16_t sequence, const uint8_t *body,
                        size_t body_length, uint8_t *wire,
                        size_t wire_capacity, size_t *wire_length)
{
    size_t length;
    if (type < UM_WIRE_DISCOVER || type > UM_WIRE_CALIB_BODY_RESULT ||
        (body_length != 0u && body == NULL) || body_length > UM_LIVE_MAX_BODY ||
        wire == NULL || wire_length == NULL) {
        return UM_ERR_ARGUMENT;
    }
    length = UM_LIVE_WIRE_HEADER_SIZE + body_length;
    if (wire_capacity < length) {
        return UM_ERR_CAPACITY;
    }
    wire[0] = UINT8_C(0x55);
    wire[1] = UINT8_C(0x41);
    wire[2] = 1u;
    wire[3] = (uint8_t)type;
    write_u32(&wire[4], session_id);
    write_u16(&wire[8], sequence);
    write_u16(&wire[10], (uint16_t)body_length);
    if (body_length != 0u) {
        memcpy(wire + UM_LIVE_WIRE_HEADER_SIZE, body, body_length);
    }
    *wire_length = length;
    return UM_OK;
}

int um_live_wire_decode(const uint8_t *wire, size_t wire_length,
                        um_live_wire_message *message)
{
    size_t body_length;
    if (wire == NULL || message == NULL ||
        wire_length < UM_LIVE_WIRE_HEADER_SIZE ||
        wire[0] != UINT8_C(0x55) || wire[1] != UINT8_C(0x41) ||
        wire[2] != 1u || wire[3] < UM_WIRE_DISCOVER ||
        wire[3] > UM_WIRE_CALIB_BODY_RESULT) {
        return UM_ERR_HEADER;
    }
    body_length = read_u16(&wire[10]);
    if (body_length > UM_LIVE_MAX_BODY ||
        wire_length != UM_LIVE_WIRE_HEADER_SIZE + body_length) {
        return UM_ERR_HEADER;
    }
    memset(message, 0, sizeof(*message));
    message->type = (um_live_wire_type)wire[3];
    message->session_id = read_u32(&wire[4]);
    message->sequence = read_u16(&wire[8]);
    message->body_length = body_length;
    if (body_length != 0u) {
        memcpy(message->body, wire + UM_LIVE_WIRE_HEADER_SIZE, body_length);
    }
    return UM_OK;
}
