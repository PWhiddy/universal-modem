#ifndef UM_LIVE_WIRE_H
#define UM_LIVE_WIRE_H

#include "um.h"

#include <stddef.h>
#include <stdint.h>

#define UM_LIVE_WIRE_HEADER_SIZE 12u
#define UM_LIVE_MAX_BODY 512u
#define UM_LIVE_MAX_WIRE (UM_LIVE_WIRE_HEADER_SIZE + UM_LIVE_MAX_BODY)

typedef enum {
    UM_WIRE_DISCOVER = 1,
    UM_WIRE_OFFER = 2,
    UM_WIRE_CONFIRM = 3,
    UM_WIRE_CONNECTED = 4,
    UM_WIRE_CALIB_BEGIN = 5,
    UM_WIRE_CALIB_READY = 6,
    UM_WIRE_CALIB_PROBE = 7,
    UM_WIRE_CALIB_REPORT = 8,
    UM_WIRE_CALIB_VERIFY = 9,
    UM_WIRE_CALIB_VERIFY_RESULT = 10,
    UM_WIRE_TEST_BEGIN = 11,
    UM_WIRE_DATA = 12,
    UM_WIRE_ACK = 13,
    UM_WIRE_TURN = 14,
    UM_WIRE_COMPLETE = 15,
    UM_WIRE_CALIB_CACHE = 16,
    UM_WIRE_PROXY_BEGIN = 17,
    UM_WIRE_IP_FRAGMENT = 18,
    UM_WIRE_IP_ACK = 19,
    UM_WIRE_PROXY_TURN = 20,
    UM_WIRE_PROXY_TURN_ACK = 21,
    UM_WIRE_PROXY_TURN_COMMIT = 22,
    UM_WIRE_PROXY_COMPLETE = 23
} um_live_wire_type;

typedef struct {
    um_live_wire_type type;
    uint32_t session_id;
    uint16_t sequence;
    size_t body_length;
    uint8_t body[UM_LIVE_MAX_BODY];
} um_live_wire_message;

int um_live_wire_encode(um_live_wire_type type, uint32_t session_id,
                        uint16_t sequence, const uint8_t *body,
                        size_t body_length, uint8_t *wire,
                        size_t wire_capacity, size_t *wire_length);
int um_live_wire_decode(const uint8_t *wire, size_t wire_length,
                        um_live_wire_message *message);

#endif
