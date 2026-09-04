#include "../src/network_trace.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int tests_run;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                               \
            ++failures;                                                        \
            return;                                                            \
        }                                                                      \
    } while (0)

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

static size_t write_example_question(uint8_t *dns)
{
    size_t offset = 12u;
    dns[offset++] = 7u;
    memcpy(&dns[offset], "example", 7u);
    offset += 7u;
    dns[offset++] = 3u;
    memcpy(&dns[offset], "com", 3u);
    offset += 3u;
    dns[offset++] = 0u;
    write_u16(&dns[offset], 1u);
    offset += 2u;
    write_u16(&dns[offset], 1u);
    return offset + 2u;
}

static size_t make_dns_response(uint8_t packet[96])
{
    uint8_t *dns = &packet[28];
    size_t offset;
    size_t dns_length;
    size_t packet_length;
    memset(packet, 0, 96u);
    write_u16(dns, UINT16_C(0x4621));
    write_u16(&dns[2], UINT16_C(0x8180));
    write_u16(&dns[4], 1u);
    write_u16(&dns[6], 1u);
    offset = write_example_question(dns);
    dns[offset++] = UINT8_C(0xc0);
    dns[offset++] = 12u;
    write_u16(&dns[offset], 1u);
    offset += 2u;
    write_u16(&dns[offset], 1u);
    offset += 2u;
    write_u32(&dns[offset], 300u);
    offset += 4u;
    write_u16(&dns[offset], 4u);
    offset += 2u;
    dns[offset++] = 93u;
    dns[offset++] = 184u;
    dns[offset++] = 216u;
    dns[offset++] = 34u;
    dns_length = offset;
    packet_length = 28u + dns_length;
    packet[0] = UINT8_C(0x45);
    write_u16(&packet[2], (uint16_t)packet_length);
    packet[8] = 64u;
    packet[9] = 17u;
    packet[12] = 1u;
    packet[13] = 1u;
    packet[14] = 1u;
    packet[15] = 1u;
    packet[16] = 10u;
    packet[17] = 77u;
    packet[18] = 0u;
    packet[19] = 2u;
    write_u16(&packet[20], 53u);
    write_u16(&packet[22], 53000u);
    write_u16(&packet[24], (uint16_t)(dns_length + 8u));
    return packet_length;
}

static size_t make_tcp_packet(uint8_t packet[64], unsigned flags,
                              size_t payload_length)
{
    size_t packet_length = 40u + payload_length;
    memset(packet, 0, 64u);
    packet[0] = UINT8_C(0x45);
    write_u16(&packet[2], (uint16_t)packet_length);
    packet[8] = 64u;
    packet[9] = 6u;
    packet[12] = 10u;
    packet[13] = 77u;
    packet[14] = 0u;
    packet[15] = 2u;
    packet[16] = 93u;
    packet[17] = 184u;
    packet[18] = 216u;
    packet[19] = 34u;
    write_u16(&packet[20], 51000u);
    write_u16(&packet[22], 443u);
    packet[32] = UINT8_C(0x50);
    packet[33] = (uint8_t)flags;
    memset(&packet[40], UINT8_C(0xa5), payload_length);
    return packet_length;
}

static void test_dns_attribution_and_activity_filter(void)
{
    um_network_trace trace;
    uint8_t response[96];
    uint8_t tcp[64];
    uint8_t invalid[1] = {0u};
    char description[512];
    size_t response_length = make_dns_response(response);
    size_t tcp_length;

    ++tests_run;

    um_network_trace_init(&trace);
    CHECK(um_network_trace_describe(&trace, response, response_length,
                                    description,
                                    sizeof(description)) != 0);
    CHECK(strstr(description, "DNS response example.com A") != NULL);
    um_network_trace_observe(&trace, response, response_length);

    tcp_length = make_tcp_packet(tcp, UINT8_C(0x02), 0u);
    CHECK(um_network_trace_describe(&trace, tcp, tcp_length, description,
                                    sizeof(description)) != 0);
    CHECK(strstr(description, "93.184.216.34(example.com):443") != NULL);
    CHECK(strstr(description, "payload=0") != NULL);

    tcp_length = make_tcp_packet(tcp, UINT8_C(0x10), 0u);
    CHECK(um_network_trace_describe(&trace, tcp, tcp_length, description,
                                    sizeof(description)) == 0);
    tcp_length = make_tcp_packet(tcp, UINT8_C(0x18), 5u);
    CHECK(um_network_trace_describe(&trace, tcp, tcp_length, description,
                                    sizeof(description)) != 0);
    CHECK(strstr(description, "payload=5") != NULL);

    CHECK(um_network_trace_describe(&trace, invalid, sizeof(invalid),
                                    description,
                                    sizeof(description)) == 0);
}

static void test_malformed_packets_fail_safely(void)
{
    um_network_trace trace;
    uint8_t packet[256];
    char description[64];
    uint32_t random = UINT32_C(0x6e657477);
    size_t trial;
    ++tests_run;
    um_network_trace_init(&trace);
    for (trial = 0u; trial < 5000u; ++trial) {
        size_t length;
        size_t index;
        random ^= random << 13u;
        random ^= random >> 17u;
        random ^= random << 5u;
        length = random % (sizeof(packet) + 1u);
        for (index = 0u; index < length; ++index) {
            random ^= random << 13u;
            random ^= random >> 17u;
            random ^= random << 5u;
            packet[index] = (uint8_t)random;
        }
        um_network_trace_observe(&trace, packet, length);
        (void)um_network_trace_describe(
            &trace, packet, length, description, sizeof(description));
    }
}

int main(void)
{
    test_dns_attribution_and_activity_filter();
    test_malformed_packets_fail_safely();
    if (failures != 0) {
        fprintf(stderr, "%d of %d network trace tests failed\n", failures,
                tests_run);
        return 1;
    }
    printf("all %d network trace tests passed\n", tests_run);
    return 0;
}
