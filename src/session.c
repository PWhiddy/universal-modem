#include "um_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum wire_type {
    WIRE_DISCOVER = 1,
    WIRE_OFFER = 2,
    WIRE_CONFIRM = 3,
    WIRE_DATA = 4,
    WIRE_ACK = 5
};

#define WIRE_HEADER_SIZE 12u

typedef struct {
    const um_session_simulation_config *config;
    um_session_simulation_result *result;
    um_log_callback logger;
    void *logger_context;
    float now;
    float blackout_start;
    float blackout_end;
    uint32_t session_id;
    uint32_t transmission_number;
} session_context;

static void session_log(session_context *session, const char *format, ...)
{
    char message[384];
    char line[448];
    va_list arguments;
    if (session->logger == NULL) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    (void)snprintf(line, sizeof(line), "[%7.3fs] %s", session->now, message);
    session->logger(session->logger_context, line);
}

static void wire_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void wire_write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static uint16_t wire_read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t wire_read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | bytes[3];
}

static um_modem_config bootstrap_config(void)
{
    um_modem_config config = um_modem_default_config();
    config.first_bin = 12u;
    config.last_bin = 60u;
    config.cyclic_prefix = 64u;
    config.window_samples = 8u;
    config.qam_bits = 2u;
    config.fec_rate = UM_FEC_RATE_1_2;
    return config;
}

static int overlaps_blackout(const session_context *session, float start,
                              float end)
{
    return session->blackout_end > session->blackout_start &&
           start < session->blackout_end && end > session->blackout_start;
}

static int transmit_wire(session_context *session,
                         const um_modem_config *modem,
                         const um_channel_config *channel,
                         enum wire_type type, uint16_t wire_sequence,
                         const uint8_t *body, size_t body_length,
                         enum wire_type *received_type,
                         uint16_t *received_sequence, uint8_t *received_body,
                         size_t received_capacity, size_t *received_length,
                         float *transmission_duration)
{
    uint8_t *wire = NULL;
    uint8_t *decoded = NULL;
    float *transmitted = NULL;
    float *received = NULL;
    size_t wire_length;
    size_t transmitted_count = 0u;
    size_t received_count = 0u;
    size_t decoded_count = 0u;
    uint16_t modem_sequence = 0u;
    um_channel_config varied_channel = *channel;
    float duration = 0.0f;
    int status;

    if (body_length > UINT16_MAX ||
        (body_length != 0u && body == NULL) ||
        body_length > SIZE_MAX - WIRE_HEADER_SIZE) {
        return UM_ERR_ARGUMENT;
    }
    wire_length = WIRE_HEADER_SIZE + body_length;
    wire = (uint8_t *)malloc(wire_length);
    decoded = (uint8_t *)malloc(wire_length);
    if (wire == NULL || decoded == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    wire[0] = UINT8_C(0x4c);
    wire[1] = UINT8_C(0x4b);
    wire[2] = 1u;
    wire[3] = (uint8_t)type;
    wire_write_u32(&wire[4], session->session_id);
    wire_write_u16(&wire[8], wire_sequence);
    wire_write_u16(&wire[10], (uint16_t)body_length);
    if (body_length != 0u) {
        memcpy(wire + WIRE_HEADER_SIZE, body, body_length);
    }

    status = um_modulate_frame(modem, wire, wire_length, wire_sequence,
                               &transmitted, &transmitted_count);
    if (status != UM_OK) {
        goto done;
    }
    duration = (float)transmitted_count / (float)UM_SAMPLE_RATE;
    *transmission_duration = duration;
    if (overlaps_blackout(session, session->now, session->now + duration)) {
        status = UM_ERR_SYNC;
        goto done;
    }
    varied_channel.random_seed ^=
        ++session->transmission_number * UINT32_C(0x9e3779b9);
    status = um_channel_apply(transmitted, transmitted_count, &varied_channel,
                              &received, &received_count);
    if (status != UM_OK) {
        goto done;
    }
    status = um_demodulate_frame(modem, received, received_count, decoded,
                                 wire_length, &decoded_count, &modem_sequence,
                                 NULL);
    if (status != UM_OK) {
        goto done;
    }
    if (decoded_count < WIRE_HEADER_SIZE || decoded[0] != UINT8_C(0x4c) ||
        decoded[1] != UINT8_C(0x4b) || decoded[2] != 1u ||
        wire_read_u32(&decoded[4]) != session->session_id ||
        wire_read_u16(&decoded[10]) != decoded_count - WIRE_HEADER_SIZE) {
        status = UM_ERR_HEADER;
        goto done;
    }
    *received_type = (enum wire_type)decoded[3];
    *received_sequence = wire_read_u16(&decoded[8]);
    *received_length = decoded_count - WIRE_HEADER_SIZE;
    if (*received_length > received_capacity) {
        status = UM_ERR_CAPACITY;
        goto done;
    }
    if (*received_length != 0u) {
        memcpy(received_body, decoded + WIRE_HEADER_SIZE, *received_length);
    }

done:
    free(received);
    free(transmitted);
    free(decoded);
    free(wire);
    return status;
}

static int send_control(session_context *session,
                        const um_modem_config *bootstrap,
                        const um_channel_config *channel, enum wire_type type,
                        uint16_t sequence, enum wire_type expected,
                        float *duration)
{
    uint8_t received[1];
    size_t received_length = 0u;
    uint16_t received_sequence = 0u;
    enum wire_type received_type = 0;
    int status = transmit_wire(
        session, bootstrap, channel, type, sequence, NULL, 0u,
        &received_type, &received_sequence, received, sizeof(received),
        &received_length, duration);
    if (status == UM_OK &&
        (received_type != expected || received_sequence != sequence ||
         received_length != 0u)) {
        status = UM_ERR_HEADER;
    }
    return status;
}

static int establish_connection(session_context *session, int reconnecting)
{
    um_modem_config bootstrap = bootstrap_config();
    unsigned attempt;
    uint16_t handshake_sequence = (uint16_t)session->result->confirmations;

    session_log(session, reconnecting != 0
                             ? "link lost; both endpoints returned to discovery"
                             : "gateway listening; client entering discovery");
    for (attempt = 0u; attempt < 32u; ++attempt) {
        float started = session->now;
        float duration = 0.0f;
        int status;
        ++session->result->discovery_requests;
        session_log(session, "client tx DISCOVER attempt=%u", attempt + 1u);
        status = send_control(session, &bootstrap,
                              &session->config->client_to_gateway,
                              WIRE_DISCOVER, handshake_sequence, WIRE_DISCOVER,
                              &duration);
        session->now += duration;
        if (status != UM_OK) {
            ++session->result->decode_failures;
            session_log(session, "gateway missed DISCOVER (%s)",
                        um_status_string(status));
            session->now = started +
                           session->config->discovery_interval_seconds;
            continue;
        }

        session->now += 0.060f;
        ++session->result->offers;
        session_log(session, "gateway tx OFFER");
        status = send_control(session, &bootstrap,
                              &session->config->gateway_to_client, WIRE_OFFER,
                              handshake_sequence, WIRE_OFFER, &duration);
        session->now += duration;
        if (status != UM_OK) {
            ++session->result->decode_failures;
            session_log(session, "client missed OFFER (%s)",
                        um_status_string(status));
            session->now = started +
                           session->config->discovery_interval_seconds;
            continue;
        }

        session->now += 0.060f;
        session_log(session, "client tx CONFIRM");
        status = send_control(session, &bootstrap,
                              &session->config->client_to_gateway,
                              WIRE_CONFIRM, handshake_sequence, WIRE_CONFIRM,
                              &duration);
        session->now += duration;
        if (status != UM_OK) {
            ++session->result->decode_failures;
            session_log(session, "gateway missed CONFIRM (%s)",
                        um_status_string(status));
            session->now = started +
                           session->config->discovery_interval_seconds;
            continue;
        }
        ++session->result->confirmations;
        if (reconnecting != 0) {
            ++session->result->reconnects;
        }
        session_log(session, "connection confirmed");
        return UM_OK;
    }
    return UM_ERR_SYNC;
}

static uint8_t stream_byte(uint32_t seed, size_t offset)
{
    uint32_t value = seed ^ (uint32_t)offset * UINT32_C(0x9e3779b9);
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16u;
    return (uint8_t)value;
}

static int transfer_stream(session_context *session, int client_to_gateway,
                           size_t total_bytes)
{
    um_modem_config ack_bootstrap = bootstrap_config();
    const um_modem_config *data_modem =
        client_to_gateway != 0
            ? &session->result->client_to_gateway_config
            : &session->result->gateway_to_client_config;
    const um_modem_config *ack_modem = &ack_bootstrap;
    const um_channel_config *data_channel =
        client_to_gateway != 0
            ? &session->config->client_to_gateway
            : &session->config->gateway_to_client;
    const um_channel_config *ack_channel =
        client_to_gateway != 0
            ? &session->config->gateway_to_client
            : &session->config->client_to_gateway;
    size_t sent_offset = 0u;
    size_t accepted_offset = 0u;
    uint16_t sequence = 0u;
    uint16_t receiver_next = 0u;
    unsigned attempts = 0u;
    size_t event_guard = 0u;
    uint8_t *body = NULL;
    uint8_t *received = NULL;
    int status = UM_OK;

    body = (uint8_t *)malloc(session->config->frame_payload_bytes);
    received = (uint8_t *)malloc(session->config->frame_payload_bytes);
    if (body == NULL || received == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    session_log(session, "%s starting %zu-byte transfer",
                client_to_gateway != 0 ? "client" : "gateway", total_bytes);
    while (sent_offset < total_bytes && event_guard++ < 1024u) {
        size_t chunk = total_bytes - sent_offset;
        size_t received_length = 0u;
        uint16_t received_sequence = 0u;
        enum wire_type received_type = 0;
        float duration = 0.0f;
        size_t i;
        int delivered;
        int wire_status;
        if (chunk > session->config->frame_payload_bytes) {
            chunk = session->config->frame_payload_bytes;
        }
        for (i = 0u; i < chunk; ++i) {
            body[i] = stream_byte(session->config->random_seed +
                                      (client_to_gateway != 0 ? 0u : 1u),
                                  sent_offset + i);
        }
        ++session->result->data_frames;
        session_log(session, "%s tx DATA seq=%u bytes=%zu attempt=%u",
                    client_to_gateway != 0 ? "client" : "gateway",
                    (unsigned)sequence, chunk, attempts + 1u);
        wire_status = transmit_wire(
            session, data_modem, data_channel, WIRE_DATA, sequence, body, chunk,
            &received_type, &received_sequence, received,
            session->config->frame_payload_bytes, &received_length,
            &duration);
        delivered = wire_status == UM_OK;
        session->now += duration;
        if (delivered != 0 && received_type == WIRE_DATA &&
            received_sequence == sequence && received_length == chunk) {
            if (sequence == receiver_next) {
                if (memcmp(received, body, chunk) != 0 ||
                    accepted_offset != sent_offset) {
                    status = UM_ERR_CRC;
                    goto done;
                }
                accepted_offset += chunk;
                ++receiver_next;
                if (client_to_gateway != 0) {
                    session->result->gateway_received_bytes += chunk;
                } else {
                    session->result->client_received_bytes += chunk;
                }
            }
            session->now += 0.025f;
            wire_status = send_control(session, ack_modem, ack_channel,
                                       WIRE_ACK, sequence, WIRE_ACK,
                                       &duration);
            delivered = wire_status == UM_OK;
            session->now += duration;
            if (delivered != 0) {
                ++session->result->acknowledgements;
            } else {
                session_log(session, "receiver rejected ACK seq=%u: %s",
                            (unsigned)sequence,
                            um_status_string(wire_status));
            }
        } else {
            if (wire_status == UM_OK) {
                wire_status = UM_ERR_HEADER;
            }
            session_log(session, "receiver rejected DATA seq=%u: %s",
                        (unsigned)sequence,
                        um_status_string(wire_status));
            delivered = 0;
        }

        if (delivered != 0) {
            sent_offset += chunk;
            ++sequence;
            attempts = 0u;
        } else {
            ++session->result->decode_failures;
            ++session->result->retries;
            ++attempts;
            session_log(session, "DATA/ACK seq=%u timed out", (unsigned)sequence);
            session->now += session->config->ack_timeout_seconds;
            if (attempts >= session->config->retry_limit) {
                if (session->result->reconnects >=
                    session->config->reconnect_limit) {
                    session_log(session, "reconnect limit reached");
                    status = UM_ERR_SYNC;
                    goto done;
                }
                status = establish_connection(session, 1);
                if (status != UM_OK) {
                    goto done;
                }
                attempts = 0u;
            }
        }
    }
    if (sent_offset != total_bytes || accepted_offset != total_bytes) {
        status = UM_ERR_SYNC;
    }
    if (status == UM_OK) {
        session_log(session, "%s transfer complete bytes=%zu",
                    client_to_gateway != 0 ? "client" : "gateway",
                    total_bytes);
    }

done:
    free(received);
    free(body);
    return status;
}

um_session_simulation_config um_session_simulation_default_config(void)
{
    um_session_simulation_config config;
    config.client_to_gateway = um_channel_default_config();
    config.gateway_to_client = um_channel_default_config();
    config.client_to_gateway.leading_silence = 89u;
    config.client_to_gateway.gain = 0.23f;
    config.client_to_gateway.noise_stddev = 0.0014f;
    config.client_to_gateway.echo_delay = 13u;
    config.client_to_gateway.echo_gain = 0.30f;
    config.client_to_gateway.random_seed = UINT32_C(0x11112222);
    config.gateway_to_client.leading_silence = 117u;
    config.gateway_to_client.gain = 0.18f;
    config.gateway_to_client.noise_stddev = 0.0018f;
    config.gateway_to_client.echo_delay = 19u;
    config.gateway_to_client.echo_gain = 0.36f;
    config.gateway_to_client.random_seed = UINT32_C(0x33334444);
    config.client_payload_bytes = 3072u;
    config.gateway_payload_bytes = 1536u;
    config.frame_payload_bytes = 192u;
    config.discovery_interval_seconds = 1.25f;
    config.ack_timeout_seconds = 0.55f;
    config.blackout_after_data_seconds = 0.35f;
    config.blackout_duration_seconds = 2.8f;
    config.retry_limit = 3u;
    config.reconnect_limit = 8u;
    config.calibrate_high_quality = 0;
    config.random_seed = UINT32_C(0xdecafbad);
    return config;
}

int um_simulate_session(const um_session_simulation_config *config,
                        um_session_simulation_result *result,
                        um_log_callback logger, void *logger_context)
{
    session_context session;
    um_calibration_result forward;
    um_calibration_result reverse;
    int status;

    if (config == NULL || result == NULL ||
        config->frame_payload_bytes == 0u ||
        config->frame_payload_bytes > 4096u ||
        config->discovery_interval_seconds <= 0.0f ||
        config->ack_timeout_seconds <= 0.0f || config->retry_limit == 0u ||
        config->reconnect_limit == 0u ||
        config->blackout_after_data_seconds < 0.0f ||
        config->blackout_duration_seconds < 0.0f) {
        return UM_ERR_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    memset(&session, 0, sizeof(session));
    session.config = config;
    session.result = result;
    session.logger = logger;
    session.logger_context = logger_context;
    session.session_id = config->random_seed ^ UINT32_C(0x554d4f44);

    status = establish_connection(&session, 0);
    if (status != UM_OK) {
        goto done;
    }
    session_log(&session, "calibrating client -> gateway");
    status = um_calibrate_simulated(&config->client_to_gateway,
                                    config->calibrate_high_quality, &forward,
                                    NULL, NULL);
    if (status != UM_OK) {
        goto done;
    }
    session.now += forward.estimated_seconds;
    result->client_to_gateway_config = forward.config;
    session_log(&session,
                "client -> gateway calibrated qam=%u fec=%u cp=%u payload=%.0fbps",
                1u << forward.config.qam_bits, (unsigned)forward.config.fec_rate,
                forward.config.cyclic_prefix, forward.payload_bps);

    session_log(&session, "calibrating gateway -> client");
    status = um_calibrate_simulated(&config->gateway_to_client,
                                    config->calibrate_high_quality, &reverse,
                                    NULL, NULL);
    if (status != UM_OK) {
        goto done;
    }
    session.now += reverse.estimated_seconds;
    result->gateway_to_client_config = reverse.config;
    session_log(&session,
                "gateway -> client calibrated qam=%u fec=%u cp=%u payload=%.0fbps",
                1u << reverse.config.qam_bits, (unsigned)reverse.config.fec_rate,
                reverse.config.cyclic_prefix, reverse.payload_bps);

    session.blackout_start = session.now + config->blackout_after_data_seconds;
    session.blackout_end = session.blackout_start +
                           config->blackout_duration_seconds;
    session_log(&session, "link connected; routing enabled");
    status = transfer_stream(&session, 1, config->client_payload_bytes);
    if (status != UM_OK) {
        goto done;
    }
    status = transfer_stream(&session, 0, config->gateway_payload_bytes);
    if (status != UM_OK) {
        goto done;
    }
    result->final_connected = 1;

done:
    result->elapsed_seconds = session.now;
    return status;
}
