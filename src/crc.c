#include "um_internal.h"

uint32_t um_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;

    for (i = 0; i < length; ++i) {
        unsigned bit;
        crc ^= data[i];
        for (bit = 0; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1u) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

uint16_t um_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = UINT16_C(0xffff);
    size_t i;

    for (i = 0; i < length; ++i) {
        unsigned bit;
        crc ^= (uint16_t)data[i] << 8u;
        for (bit = 0; bit < 8u; ++bit) {
            crc = (crc & UINT16_C(0x8000)) != 0u
                      ? (uint16_t)((crc << 1u) ^ UINT16_C(0x1021))
                      : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}

void um_bytes_to_bits(const uint8_t *bytes, size_t byte_count, uint8_t *bits)
{
    size_t i;
    for (i = 0; i < byte_count; ++i) {
        unsigned bit;
        for (bit = 0; bit < 8u; ++bit) {
            bits[i * 8u + bit] =
                (uint8_t)((bytes[i] >> (7u - bit)) & 1u);
        }
    }
}

void um_bits_to_bytes(const uint8_t *bits, size_t bit_count, uint8_t *bytes)
{
    size_t i;
    size_t byte_count = (bit_count + 7u) / 8u;
    for (i = 0; i < byte_count; ++i) {
        bytes[i] = 0u;
    }
    for (i = 0; i < bit_count; ++i) {
        bytes[i / 8u] |= (uint8_t)((bits[i] & 1u) << (7u - (i % 8u)));
    }
}

size_t um_gcd_size(size_t a, size_t b)
{
    while (b != 0u) {
        size_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

const char *um_status_string(int status)
{
    switch (status) {
    case UM_OK:
        return "ok";
    case UM_ERR_ARGUMENT:
        return "invalid argument";
    case UM_ERR_MEMORY:
        return "out of memory";
    case UM_ERR_CONFIG:
        return "invalid modem configuration";
    case UM_ERR_SYNC:
        return "frame synchronization not found";
    case UM_ERR_TRUNCATED:
        return "truncated frame";
    case UM_ERR_HEADER:
        return "invalid frame header";
    case UM_ERR_CRC:
        return "payload checksum mismatch";
    case UM_ERR_CAPACITY:
        return "output capacity too small";
    case UM_ERR_TIMEOUT:
        return "operation timed out";
    case UM_ERR_AUDIO:
        return "audio device error";
    case UM_ERR_UNSUPPORTED:
        return "unsupported on this system";
    case UM_ERR_INTERRUPTED:
        return "interrupted";
    default:
        return "unknown error";
    }
}
