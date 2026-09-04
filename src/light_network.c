#include "um_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIGHT_NETWORK_RECORD_BYTES 2u
#define LIGHT_NETWORK_MIN_MTU 68u
#define LIGHT_NETWORK_MAX_MTU 1500u
#define LIGHT_NETWORK_MAX_PACKETS 4096u
#define LIGHT_NETWORK_DATA_BYTES (UM_LIGHT_MAX_PAYLOAD - 8u)

typedef struct {
    uint8_t *bytes;
    size_t length;
    size_t ip_bytes;
    size_t udp_packets;
    size_t tcp_packets;
    size_t spanning_packets;
} light_network_stream;

typedef struct {
    size_t packets;
    size_t ip_bytes;
    size_t udp_packets;
    size_t tcp_packets;
} light_network_validation;

_Static_assert(LIGHT_NETWORK_DATA_BYTES == 83u,
               "unexpected optical session data capacity");

static void light_network_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void light_network_write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static uint16_t light_network_read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t light_network_checksum_add(uint32_t sum,
                                           const uint8_t *bytes,
                                           size_t length)
{
    while (length >= 2u) {
        sum += ((uint32_t)bytes[0] << 8u) | bytes[1];
        bytes += 2u;
        length -= 2u;
    }
    if (length != 0u) {
        sum += (uint32_t)bytes[0] << 8u;
    }
    return sum;
}

static uint16_t light_network_checksum_finish(uint32_t sum)
{
    while ((sum >> 16u) != 0u) {
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16u);
    }
    return (uint16_t)~sum;
}

static uint8_t light_network_payload_byte(uint32_t seed, unsigned direction,
                                          size_t packet_index,
                                          size_t byte_index)
{
    uint32_t value = seed ^
                     ((uint32_t)direction + UINT32_C(1)) *
                         UINT32_C(0x6d2b79f5) ^
                     (uint32_t)packet_index * UINT32_C(0x9e3779b9) ^
                     (uint32_t)byte_index * UINT32_C(0x85ebca6b);
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16u;
    return (uint8_t)value;
}

static size_t light_network_packet_length(unsigned mtu, size_t packet_index,
                                          uint8_t protocol)
{
    size_t minimum = protocol == 17u ? 28u : 40u;
    size_t candidate;
    switch (packet_index % 11u) {
    case 0u:
        candidate = minimum;
        break;
    case 1u:
        candidate = minimum + 1u;
        break;
    case 2u:
        candidate = 52u;
        break;
    case 3u:
        candidate = 57u;
        break;
    case 4u:
        candidate = 81u;
        break;
    case 5u:
        candidate = 83u;
        break;
    case 6u:
        candidate = 127u;
        break;
    case 7u:
        candidate = (size_t)mtu / 3u;
        break;
    case 8u:
        candidate = (size_t)mtu / 2u;
        break;
    case 9u:
        candidate = (size_t)mtu - 1u;
        break;
    default:
        candidate = mtu;
        break;
    }
    if (candidate < minimum) {
        candidate = minimum;
    }
    if (candidate > mtu) {
        candidate = mtu;
    }
    return candidate;
}

static void light_network_addresses(uint8_t *packet, unsigned direction,
                                    size_t packet_index)
{
    uint8_t remote_last = (uint8_t)(34u + packet_index % 17u);
    if (direction == 0u) {
        packet[12] = 10u;
        packet[13] = 77u;
        packet[14] = 0u;
        packet[15] = 2u;
        packet[16] = 93u;
        packet[17] = 184u;
        packet[18] = 216u;
        packet[19] = remote_last;
    } else {
        packet[12] = 93u;
        packet[13] = 184u;
        packet[14] = 216u;
        packet[15] = remote_last;
        packet[16] = 10u;
        packet[17] = 77u;
        packet[18] = 0u;
        packet[19] = 2u;
    }
}

static uint32_t light_network_transport_sum(const uint8_t *packet,
                                            size_t transport_length)
{
    uint32_t sum = light_network_checksum_add(0u, &packet[12], 8u);
    sum += packet[9];
    sum += (uint32_t)transport_length;
    return light_network_checksum_add(sum, &packet[20], transport_length);
}

static void light_network_make_packet(uint8_t *packet, size_t length,
                                      uint8_t protocol, unsigned direction,
                                      size_t packet_index, uint32_t seed)
{
    size_t transport_length = length - 20u;
    size_t transport_header = protocol == 17u ? 8u : 20u;
    size_t payload_offset = 20u + transport_header;
    size_t i;
    uint16_t checksum;
    uint16_t source_port = (uint16_t)(12000u + packet_index % 40000u);
    uint16_t destination_port = protocol == 17u ? UINT16_C(53)
                                                 : UINT16_C(443);

    memset(packet, 0, length);
    packet[0] = 0x45u;
    light_network_write_u16(&packet[2], (uint16_t)length);
    light_network_write_u16(&packet[4],
                            (uint16_t)(packet_index +
                                       (size_t)direction * 0x4000u));
    light_network_write_u16(&packet[6], UINT16_C(0x4000));
    packet[8] = 64u;
    packet[9] = protocol;
    light_network_addresses(packet, direction, packet_index);

    if (direction != 0u) {
        uint16_t swap = source_port;
        source_port = destination_port;
        destination_port = swap;
    }
    light_network_write_u16(&packet[20], source_port);
    light_network_write_u16(&packet[22], destination_port);
    if (protocol == 17u) {
        light_network_write_u16(&packet[24], (uint16_t)transport_length);
    } else {
        light_network_write_u32(&packet[24],
                                seed ^ (uint32_t)packet_index *
                                           UINT32_C(0x01010101));
        light_network_write_u32(&packet[28],
                                seed + (uint32_t)packet_index * 257u);
        packet[32] = 0x50u;
        packet[33] = 0x18u;
        light_network_write_u16(&packet[34], UINT16_C(32768));
    }
    for (i = payload_offset; i < length; ++i) {
        packet[i] = light_network_payload_byte(
            seed, direction, packet_index, i - payload_offset);
    }

    checksum = light_network_checksum_finish(
        light_network_checksum_add(0u, packet, 20u));
    light_network_write_u16(&packet[10], checksum);
    checksum = light_network_checksum_finish(
        light_network_transport_sum(packet, transport_length));
    if (protocol == 17u) {
        light_network_write_u16(&packet[26],
                                checksum == 0u ? UINT16_C(0xffff)
                                               : checksum);
    } else {
        light_network_write_u16(&packet[36], checksum);
    }
}

static void light_network_stream_destroy(light_network_stream *stream)
{
    free(stream->bytes);
    memset(stream, 0, sizeof(*stream));
}

static int light_network_build_stream(unsigned mtu, size_t packet_count,
                                      unsigned direction, uint32_t seed,
                                      light_network_stream *stream)
{
    size_t capacity;
    size_t packet_index;
    size_t offset = 0u;
    memset(stream, 0, sizeof(*stream));
    if (packet_count == 0u) {
        return UM_OK;
    }
    if ((size_t)mtu + LIGHT_NETWORK_RECORD_BYTES >
        SIZE_MAX / packet_count) {
        return UM_ERR_ARGUMENT;
    }
    capacity = packet_count *
               ((size_t)mtu + LIGHT_NETWORK_RECORD_BYTES);
    stream->bytes = (uint8_t *)malloc(capacity);
    if (stream->bytes == NULL) {
        return UM_ERR_MEMORY;
    }
    for (packet_index = 0u; packet_index < packet_count; ++packet_index) {
        uint8_t protocol = ((packet_index + direction) & 1u) == 0u
                               ? 17u
                               : 6u;
        size_t packet_length = light_network_packet_length(
            mtu, packet_index, protocol);
        size_t record_length = LIGHT_NETWORK_RECORD_BYTES + packet_length;
        size_t record_last = offset + record_length - 1u;
        if (offset / LIGHT_NETWORK_DATA_BYTES !=
            record_last / LIGHT_NETWORK_DATA_BYTES) {
            ++stream->spanning_packets;
        }
        light_network_write_u16(&stream->bytes[offset],
                                (uint16_t)packet_length);
        light_network_make_packet(
            &stream->bytes[offset + LIGHT_NETWORK_RECORD_BYTES],
            packet_length, protocol, direction, packet_index, seed);
        if (protocol == 17u) {
            ++stream->udp_packets;
        } else {
            ++stream->tcp_packets;
        }
        stream->ip_bytes += packet_length;
        offset += record_length;
    }
    stream->length = offset;
    return UM_OK;
}

static int light_network_validate_packet(const uint8_t *packet,
                                         size_t packet_length,
                                         unsigned direction,
                                         light_network_validation *validated)
{
    size_t header_length;
    size_t transport_length;
    uint16_t recorded_length;
    if (packet_length < 20u || (packet[0] >> 4u) != 4u) {
        return UM_ERR_HEADER;
    }
    header_length = (size_t)(packet[0] & 0x0fu) * 4u;
    recorded_length = light_network_read_u16(&packet[2]);
    if (header_length != 20u || recorded_length != packet_length) {
        return UM_ERR_HEADER;
    }
    if (light_network_checksum_finish(light_network_checksum_add(
            0u, packet, header_length)) != 0u) {
        return UM_ERR_CRC;
    }
    if ((direction == 0u &&
         (packet[12] != 10u || packet[13] != 77u ||
          packet[14] != 0u || packet[15] != 2u)) ||
        (direction != 0u &&
         (packet[16] != 10u || packet[17] != 77u ||
          packet[18] != 0u || packet[19] != 2u))) {
        return UM_ERR_HEADER;
    }

    transport_length = packet_length - header_length;
    if (packet[9] == 17u) {
        if (transport_length < 8u ||
            light_network_read_u16(&packet[24]) != transport_length ||
            light_network_read_u16(&packet[26]) == 0u) {
            return UM_ERR_HEADER;
        }
        if (light_network_checksum_finish(light_network_transport_sum(
                packet, transport_length)) != 0u) {
            return UM_ERR_CRC;
        }
        ++validated->udp_packets;
    } else if (packet[9] == 6u) {
        size_t tcp_header_length;
        if (transport_length < 20u) {
            return UM_ERR_HEADER;
        }
        tcp_header_length = (size_t)(packet[32] >> 4u) * 4u;
        if (tcp_header_length < 20u ||
            tcp_header_length > transport_length) {
            return UM_ERR_HEADER;
        }
        if (light_network_checksum_finish(light_network_transport_sum(
                packet, transport_length)) != 0u) {
            return UM_ERR_CRC;
        }
        ++validated->tcp_packets;
    } else {
        return UM_ERR_HEADER;
    }
    ++validated->packets;
    validated->ip_bytes += packet_length;
    return UM_OK;
}

static int light_network_validate_stream(
    const uint8_t *bytes, size_t length, unsigned direction,
    light_network_validation *validated)
{
    size_t offset = 0u;
    memset(validated, 0, sizeof(*validated));
    while (offset < length) {
        size_t packet_length;
        int status;
        if (length - offset < LIGHT_NETWORK_RECORD_BYTES) {
            return UM_ERR_HEADER;
        }
        packet_length = light_network_read_u16(&bytes[offset]);
        offset += LIGHT_NETWORK_RECORD_BYTES;
        if (packet_length == 0u || packet_length > length - offset) {
            return UM_ERR_HEADER;
        }
        status = light_network_validate_packet(
            &bytes[offset], packet_length, direction, validated);
        if (status != UM_OK) {
            return status;
        }
        offset += packet_length;
    }
    return offset == length ? UM_OK : UM_ERR_HEADER;
}

static void light_network_log(um_log_callback logger, void *context,
                              const char *format, size_t first,
                              size_t second)
{
    char message[192];
    if (logger == NULL) {
        return;
    }
    (void)snprintf(message, sizeof(message), format, first, second);
    logger(context, message);
}

um_light_network_simulation_config
um_light_network_simulation_default_config(void)
{
    um_light_network_simulation_config config;
    memset(&config, 0, sizeof(config));
    config.session = um_light_session_simulation_default_config();
    config.mtu = 576u;
    config.client_packet_count = 20u;
    config.gateway_packet_count = 24u;
    config.random_seed = UINT32_C(0x4e455449);
    return config;
}

int um_simulate_light_network(
    const um_light_network_simulation_config *config,
    um_light_network_simulation_result *result, um_log_callback logger,
    void *logger_context)
{
    light_network_stream client_stream;
    light_network_stream gateway_stream;
    light_network_validation at_gateway;
    light_network_validation at_client;
    um_light_session_simulation_config session_config;
    uint8_t *gateway_received = NULL;
    uint8_t *client_received = NULL;
    int status;

    if (config == NULL || result == NULL) {
        return UM_ERR_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    memset(&client_stream, 0, sizeof(client_stream));
    memset(&gateway_stream, 0, sizeof(gateway_stream));
    if (config->mtu < LIGHT_NETWORK_MIN_MTU ||
        config->mtu > LIGHT_NETWORK_MAX_MTU ||
        config->client_packet_count > LIGHT_NETWORK_MAX_PACKETS ||
        config->gateway_packet_count > LIGHT_NETWORK_MAX_PACKETS) {
        return UM_ERR_ARGUMENT;
    }

    status = light_network_build_stream(
        config->mtu, config->client_packet_count, 0u,
        config->random_seed ^ UINT32_C(0xc11e4701), &client_stream);
    if (status != UM_OK) {
        goto done;
    }
    status = light_network_build_stream(
        config->mtu, config->gateway_packet_count, 1u,
        config->random_seed ^ UINT32_C(0x6a7e5a91), &gateway_stream);
    if (status != UM_OK) {
        goto done;
    }
    if (client_stream.length != 0u) {
        gateway_received = (uint8_t *)malloc(client_stream.length);
        if (gateway_received == NULL) {
            status = UM_ERR_MEMORY;
            goto done;
        }
    }
    if (gateway_stream.length != 0u) {
        client_received = (uint8_t *)malloc(gateway_stream.length);
        if (client_received == NULL) {
            status = UM_ERR_MEMORY;
            goto done;
        }
    }

    result->client_packets_sent = config->client_packet_count;
    result->gateway_packets_sent = config->gateway_packet_count;
    result->client_ip_bytes_sent = client_stream.ip_bytes;
    result->gateway_ip_bytes_sent = gateway_stream.ip_bytes;
    result->udp_packets = client_stream.udp_packets +
                          gateway_stream.udp_packets;
    result->tcp_packets = client_stream.tcp_packets +
                          gateway_stream.tcp_packets;
    result->packets_spanning_optical_frames =
        client_stream.spanning_packets + gateway_stream.spanning_packets;
    result->framing_overhead_bytes =
        LIGHT_NETWORK_RECORD_BYTES *
        (config->client_packet_count + config->gateway_packet_count);
    result->client_stream_crc32 =
        um_crc32(client_stream.bytes, client_stream.length);
    result->gateway_stream_crc32 =
        um_crc32(gateway_stream.bytes, gateway_stream.length);
    light_network_log(logger, logger_context,
                      "optical IPv4 queues ready upload=%zu packets "
                      "download=%zu packets",
                      config->client_packet_count,
                      config->gateway_packet_count);

    session_config = config->session;
    session_config.client_payload_bytes = client_stream.length;
    session_config.gateway_payload_bytes = gateway_stream.length;
    status = um_light_simulate_payloads(
        &session_config, &result->session, client_stream.bytes,
        gateway_received, gateway_stream.bytes, client_received, logger,
        logger_context);
    if (status != UM_OK) {
        goto done;
    }
    if ((client_stream.length != 0u &&
         memcmp(client_stream.bytes, gateway_received,
                client_stream.length) != 0) ||
        (gateway_stream.length != 0u &&
         memcmp(gateway_stream.bytes, client_received,
                gateway_stream.length) != 0)) {
        ++result->checksum_errors;
        result->session.final_connected = 0;
        status = UM_ERR_CRC;
        goto done;
    }

    status = light_network_validate_stream(
        gateway_received, client_stream.length, 0u, &at_gateway);
    if (status != UM_OK) {
        if (status == UM_ERR_CRC) {
            ++result->checksum_errors;
        } else {
            ++result->framing_errors;
        }
        result->session.final_connected = 0;
        goto done;
    }
    status = light_network_validate_stream(
        client_received, gateway_stream.length, 1u, &at_client);
    if (status != UM_OK) {
        if (status == UM_ERR_CRC) {
            ++result->checksum_errors;
        } else {
            ++result->framing_errors;
        }
        result->session.final_connected = 0;
        goto done;
    }
    if (at_gateway.packets != config->client_packet_count ||
        at_client.packets != config->gateway_packet_count ||
        at_gateway.udp_packets + at_client.udp_packets !=
            result->udp_packets ||
        at_gateway.tcp_packets + at_client.tcp_packets !=
            result->tcp_packets) {
        ++result->framing_errors;
        result->session.final_connected = 0;
        status = UM_ERR_HEADER;
        goto done;
    }
    result->gateway_packets_received = at_gateway.packets;
    result->client_packets_received = at_client.packets;
    result->gateway_ip_bytes_received = at_gateway.ip_bytes;
    result->client_ip_bytes_received = at_client.ip_bytes;
    if (result->session.elapsed_seconds > 0.0f) {
        result->ip_goodput_bps =
            (float)(8.0 *
                    (double)(result->gateway_ip_bytes_received +
                             result->client_ip_bytes_received) /
                    result->session.elapsed_seconds);
    }
    light_network_log(logger, logger_context,
                      "optical IPv4 delivery complete upload=%zu packets "
                      "download=%zu packets",
                      result->gateway_packets_received,
                      result->client_packets_received);
    status = UM_OK;

done:
    free(client_received);
    free(gateway_received);
    light_network_stream_destroy(&gateway_stream);
    light_network_stream_destroy(&client_stream);
    return status;
}
