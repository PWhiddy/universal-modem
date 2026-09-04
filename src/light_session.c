#include "um_internal.h"
#include "light_packet.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIGHT_SESSION_VERSION UINT8_C(1)
#define LIGHT_SESSION_ACK_BYTES 8u
#define LIGHT_SESSION_DATA_BYTES                                           \
    (UM_LIGHT_MAX_PAYLOAD - LIGHT_SESSION_ACK_BYTES)
#define LIGHT_SESSION_ACK_BITS 16u
#define LIGHT_SESSION_MAX_WINDOW LIGHT_SESSION_ACK_BITS
#define LIGHT_SESSION_MAX_BYTES (16u * 1024u * 1024u)

enum light_session_frame_type {
    LIGHT_FRAME_IDLE = 0,
    LIGHT_FRAME_DISCOVER = 1,
    LIGHT_FRAME_OFFER = 2,
    LIGHT_FRAME_CONFIRM = 3,
    LIGHT_FRAME_READY = 4,
    LIGHT_FRAME_DATA = 5,
    LIGHT_FRAME_ACK = 6
};

enum light_client_phase {
    LIGHT_CLIENT_DISCOVER = 0,
    LIGHT_CLIENT_CONFIRM,
    LIGHT_CLIENT_CONNECTED
};

enum light_gateway_phase {
    LIGHT_GATEWAY_LISTENING = 0,
    LIGHT_GATEWAY_OFFER,
    LIGHT_GATEWAY_READY,
    LIGHT_GATEWAY_CONNECTED
};

typedef struct {
    size_t total_bytes;
    size_t chunk_count;
    const uint8_t *source;
    uint8_t *sink;
    uint8_t *sent;
    uint8_t *acked;
    uint8_t *received;
    size_t *last_sent_frame;
    size_t receive_base;
    size_t received_bytes;
    int completion_logged;
} light_stream;

typedef struct {
    uint8_t type;
    uint32_t session_id;
    uint32_t sequence;
    uint8_t payload[UM_LIGHT_MAX_PAYLOAD];
    size_t payload_length;
} light_outbound_frame;

typedef struct {
    int present;
    uint8_t type;
    uint32_t session_id;
    uint32_t sequence;
    uint8_t payload[UM_LIGHT_MAX_PAYLOAD];
    size_t payload_length;
} light_received_frame;

typedef struct {
    const um_light_session_simulation_config *config;
    um_light_session_simulation_result *result;
    um_log_callback logger;
    void *logger_context;
    size_t frame;
    uint32_t random_state;
    uint32_t client_session_id;
    uint32_t gateway_session_id;
    enum light_client_phase client_phase;
    enum light_gateway_phase gateway_phase;
    size_t client_last_receive;
    size_t gateway_last_receive;
    int client_has_received;
    int gateway_has_received;
    int ever_connected;
    double client_to_gateway_correction_sum;
    double gateway_to_client_correction_sum;
    light_stream client_to_gateway;
    light_stream gateway_to_client;
    light_packet_transport *packet_transport;
} light_session;

struct um_light_peer {
    um_live_role role;
    um_light_session_simulation_config config;
    um_light_session_simulation_result result;
    light_session session;
    size_t last_frame;
    int have_frame;
};

_Static_assert(LIGHT_SESSION_DATA_BYTES == 83u,
               "unexpected optical session payload capacity");

static void light_session_log(light_session *session, const char *format, ...)
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
    (void)snprintf(line, sizeof(line), "[frame %4zu] %s", session->frame,
                   message);
    session->logger(session->logger_context, line);
}

static void light_session_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void light_session_write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static uint16_t light_session_read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t light_session_read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) |
           ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | bytes[3];
}

static uint32_t light_session_random(uint32_t *state)
{
    uint32_t value = *state;
    if (value == 0u) {
        value = UINT32_C(0xa511e9b3);
    }
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static float light_session_random_signed(uint32_t *state)
{
    float unit = (float)(light_session_random(state) >> 8u) /
                 (float)UINT32_C(0x01000000);
    return 2.0f * unit - 1.0f;
}

static uint8_t light_stream_byte(uint32_t seed, size_t offset)
{
    uint32_t value = seed ^
                     (uint32_t)offset * UINT32_C(0x9e3779b9);
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16u;
    return (uint8_t)value;
}

static int light_stream_init(light_stream *stream, size_t total_bytes,
                             const uint8_t *source, uint8_t *sink)
{
    size_t sequence;
    memset(stream, 0, sizeof(*stream));
    stream->total_bytes = total_bytes;
    stream->source = source;
    stream->sink = sink;
    if (total_bytes != 0u && source == NULL && sink == NULL) {
        return UM_ERR_ARGUMENT;
    }
    stream->chunk_count =
        (total_bytes + LIGHT_SESSION_DATA_BYTES - 1u) /
        LIGHT_SESSION_DATA_BYTES;
    if (stream->chunk_count == 0u) {
        return UM_OK;
    }
    stream->sent = (uint8_t *)calloc(stream->chunk_count, 1u);
    stream->acked = (uint8_t *)calloc(stream->chunk_count, 1u);
    stream->received = (uint8_t *)calloc(stream->chunk_count, 1u);
    stream->last_sent_frame =
        (size_t *)malloc(stream->chunk_count * sizeof(size_t));
    if (stream->sent == NULL || stream->acked == NULL ||
        stream->received == NULL || stream->last_sent_frame == NULL) {
        free(stream->last_sent_frame);
        free(stream->received);
        free(stream->acked);
        free(stream->sent);
        memset(stream, 0, sizeof(*stream));
        return UM_ERR_MEMORY;
    }
    for (sequence = 0u; sequence < stream->chunk_count; ++sequence) {
        stream->last_sent_frame[sequence] = SIZE_MAX;
    }
    return UM_OK;
}

static void light_stream_destroy(light_stream *stream)
{
    free(stream->last_sent_frame);
    free(stream->received);
    free(stream->acked);
    free(stream->sent);
    memset(stream, 0, sizeof(*stream));
}

static size_t light_stream_chunk_length(const light_stream *stream,
                                        size_t sequence)
{
    size_t offset = sequence * LIGHT_SESSION_DATA_BYTES;
    size_t remaining = stream->total_bytes - offset;
    return remaining < LIGHT_SESSION_DATA_BYTES
               ? remaining
               : LIGHT_SESSION_DATA_BYTES;
}

static int light_stream_sender_done(const light_stream *stream)
{
    size_t sequence;
    for (sequence = 0u; sequence < stream->chunk_count; ++sequence) {
        if (stream->acked[sequence] == 0u) {
            return 0;
        }
    }
    return 1;
}

static int light_stream_receiver_done(const light_stream *stream)
{
    return stream->receive_base == stream->chunk_count;
}

static uint16_t light_stream_ack_mask(const light_stream *stream)
{
    uint16_t mask = 0u;
    unsigned bit;
    for (bit = 0u; bit < LIGHT_SESSION_ACK_BITS; ++bit) {
        size_t sequence = stream->receive_base + bit;
        if (sequence < stream->chunk_count &&
            stream->received[sequence] != 0u) {
            mask |= (uint16_t)(UINT16_C(1) << bit);
        }
    }
    return mask;
}

static void light_stream_write_ack(const light_stream *stream,
                                   uint8_t payload[LIGHT_SESSION_ACK_BYTES])
{
    payload[0] = LIGHT_SESSION_VERSION;
    payload[1] = 0u;
    light_session_write_u32(&payload[2], (uint32_t)stream->receive_base);
    light_session_write_u16(&payload[6], light_stream_ack_mask(stream));
}

static int light_stream_apply_ack(light_stream *stream,
                                  const uint8_t *payload,
                                  size_t payload_length)
{
    size_t base;
    uint16_t mask;
    size_t sequence;
    unsigned bit;
    if (payload_length < LIGHT_SESSION_ACK_BYTES ||
        payload[0] != LIGHT_SESSION_VERSION || payload[1] != 0u) {
        return UM_ERR_HEADER;
    }
    base = light_session_read_u32(&payload[2]);
    mask = light_session_read_u16(&payload[6]);
    if (base > stream->chunk_count) {
        return UM_ERR_HEADER;
    }
    for (sequence = 0u; sequence < base; ++sequence) {
        stream->acked[sequence] = 1u;
    }
    for (bit = 0u; bit < LIGHT_SESSION_ACK_BITS; ++bit) {
        sequence = base + bit;
        if (sequence < stream->chunk_count &&
            (mask & (uint16_t)(UINT16_C(1) << bit)) != 0u) {
            stream->acked[sequence] = 1u;
        }
    }
    return UM_OK;
}

static int light_stream_receive(light_stream *stream, uint32_t sequence,
                                const uint8_t *payload,
                                size_t payload_length,
                                size_t *duplicate_count)
{
    size_t expected_length;
    size_t offset;
    if ((size_t)sequence >= stream->chunk_count || stream->sink == NULL) {
        return UM_ERR_HEADER;
    }
    expected_length = light_stream_chunk_length(stream, sequence);
    if (payload_length != expected_length) {
        return UM_ERR_HEADER;
    }
    offset = (size_t)sequence * LIGHT_SESSION_DATA_BYTES;
    if (stream->received[sequence] != 0u) {
        ++*duplicate_count;
        return UM_OK;
    }
    stream->received[sequence] = 1u;
    memcpy(stream->sink + offset, payload, payload_length);
    stream->received_bytes += payload_length;
    while (stream->receive_base < stream->chunk_count &&
           stream->received[stream->receive_base] != 0u) {
        ++stream->receive_base;
    }
    return UM_OK;
}

static size_t light_stream_in_flight(const light_stream *stream)
{
    size_t count = 0u;
    size_t sequence;
    for (sequence = 0u; sequence < stream->chunk_count; ++sequence) {
        if (stream->sent[sequence] != 0u &&
            stream->acked[sequence] == 0u) {
            ++count;
        }
    }
    return count;
}

static size_t light_stream_retry_delay(const light_session *session,
                                       const light_stream *stream,
                                       size_t sequence)
{
    size_t base = session->config->retransmit_after_frames;
    size_t spread = base < 4u ? 4u : base;
    uint32_t value = session->config->random_seed ^
                     (uint32_t)sequence * UINT32_C(0x9e3779b9) ^
                     (uint32_t)stream->last_sent_frame[sequence] *
                         UINT32_C(0x85ebca6b);
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    return base + value % spread;
}

static int light_stream_select(light_session *session, light_stream *stream,
                               size_t *selected, int *retransmission)
{
    size_t sequence;
    size_t eligible = 0u;
    size_t choice;
    for (sequence = 0u; sequence < stream->chunk_count; ++sequence) {
        if (stream->sent[sequence] != 0u &&
            stream->acked[sequence] == 0u &&
            session->frame - stream->last_sent_frame[sequence] >=
                light_stream_retry_delay(session, stream, sequence)) {
            ++eligible;
        }
    }
    if (eligible != 0u) {
        choice = light_session_random(&session->random_state) % eligible;
        for (sequence = 0u; sequence < stream->chunk_count; ++sequence) {
            if (stream->sent[sequence] != 0u &&
                stream->acked[sequence] == 0u &&
                session->frame - stream->last_sent_frame[sequence] >=
                    light_stream_retry_delay(session, stream, sequence)) {
                if (choice == 0u) {
                    *selected = sequence;
                    *retransmission = 1;
                    return 1;
                }
                --choice;
            }
        }
    }
    if (light_stream_in_flight(stream) >=
        session->config->transmit_window) {
        return 0;
    }
    for (sequence = 0u; sequence < stream->chunk_count; ++sequence) {
        if (stream->sent[sequence] == 0u) {
            *selected = sequence;
            *retransmission = 0;
            return 1;
        }
    }
    return 0;
}

static void light_build_control(const light_session *session,
                                light_outbound_frame *frame, uint8_t type,
                                uint32_t session_id, uint32_t sequence)
{
    memset(frame, 0, sizeof(*frame));
    frame->type = type;
    frame->session_id = session_id;
    frame->sequence = sequence;
    frame->payload[0] = LIGHT_SESSION_VERSION;
    if (session->packet_transport != NULL) {
        frame->payload[1] = LIGHT_PACKET_MODE;
        light_session_write_u16(
            &frame->payload[2],
            (uint16_t)light_packet_transport_max_packet(
                session->packet_transport));
        frame->payload_length = 4u;
    } else {
        frame->payload_length = 1u;
    }
}

static void light_build_stream_frame(light_session *session,
                                     const char *sender_name,
                                     light_stream *transmit,
                                     const light_stream *receive,
                                     uint32_t session_id,
                                     light_outbound_frame *frame)
{
    size_t sequence = 0u;
    int retransmission = 0;
    memset(frame, 0, sizeof(*frame));
    if (session->packet_transport != NULL) {
        int has_data = 0;
        light_packet_transport_build(
            session->packet_transport, session->frame, &frame->sequence,
            frame->payload, &frame->payload_length, &has_data,
            &retransmission);
        frame->session_id = session_id;
        frame->type = has_data != 0 ? LIGHT_FRAME_DATA : LIGHT_FRAME_ACK;
        if (has_data != 0) {
            ++session->result->data_frames;
        } else {
            ++session->result->acknowledgement_frames;
        }
        if (retransmission != 0) {
            ++session->result->retransmissions;
            light_session_log(session, "%s retransmit DATA seq=%u",
                              sender_name, frame->sequence);
        }
        return;
    }
    light_stream_write_ack(receive, frame->payload);
    frame->session_id = session_id;
    if (light_stream_select(session, transmit, &sequence,
                            &retransmission) == 0) {
        frame->type = LIGHT_FRAME_ACK;
        frame->sequence = UINT32_MAX;
        frame->payload_length = LIGHT_SESSION_ACK_BYTES;
        ++session->result->acknowledgement_frames;
        return;
    }

    frame->type = LIGHT_FRAME_DATA;
    frame->sequence = (uint32_t)sequence;
    frame->payload_length = LIGHT_SESSION_ACK_BYTES +
                            light_stream_chunk_length(transmit, sequence);
    {
        size_t offset = sequence * LIGHT_SESSION_DATA_BYTES;
        size_t i;
        for (i = LIGHT_SESSION_ACK_BYTES; i < frame->payload_length; ++i) {
            frame->payload[i] =
                transmit->source[offset + i - LIGHT_SESSION_ACK_BYTES];
        }
    }
    if (retransmission != 0) {
        ++session->result->retransmissions;
        light_session_log(session, "%s retransmit DATA seq=%zu",
                          sender_name, sequence);
    }
    transmit->sent[sequence] = 1u;
    transmit->last_sent_frame[sequence] = session->frame;
    ++session->result->data_frames;
}

static void light_build_client_frame(light_session *session,
                                     light_outbound_frame *frame)
{
    if (session->client_phase == LIGHT_CLIENT_DISCOVER) {
        light_build_control(session, frame, LIGHT_FRAME_DISCOVER,
                            session->client_session_id,
                            (uint32_t)session->frame);
        ++session->result->handshake_frames;
    } else if (session->client_phase == LIGHT_CLIENT_CONFIRM) {
        light_build_control(session, frame, LIGHT_FRAME_CONFIRM,
                            session->client_session_id,
                            (uint32_t)session->frame);
        ++session->result->handshake_frames;
    } else {
        light_build_stream_frame(session, "client",
                                 &session->client_to_gateway,
                                 &session->gateway_to_client,
                                 session->client_session_id, frame);
    }
}

static void light_build_gateway_frame(light_session *session,
                                      light_outbound_frame *frame)
{
    if (session->gateway_phase == LIGHT_GATEWAY_LISTENING) {
        light_build_control(session, frame, LIGHT_FRAME_IDLE, 0u,
                            (uint32_t)session->frame);
    } else if (session->gateway_phase == LIGHT_GATEWAY_OFFER) {
        light_build_control(session, frame, LIGHT_FRAME_OFFER,
                            session->gateway_session_id,
                            (uint32_t)session->frame);
        ++session->result->handshake_frames;
    } else if (session->gateway_phase == LIGHT_GATEWAY_READY) {
        light_build_control(session, frame, LIGHT_FRAME_READY,
                            session->gateway_session_id,
                            (uint32_t)session->frame);
        ++session->result->handshake_frames;
    } else {
        light_build_stream_frame(session, "gateway",
                                 &session->gateway_to_client,
                                 &session->client_to_gateway,
                                 session->gateway_session_id, frame);
    }
}

static float light_clamp_corner(float value, size_t extent)
{
    float maximum = (float)extent - 2.0f;
    if (value < 1.0f) {
        return 1.0f;
    }
    return value > maximum ? maximum : value;
}

static void light_vary_channel(light_session *session,
                               const um_light_channel_config *source,
                               unsigned direction,
                               um_light_channel_config *varied)
{
    uint32_t state = session->random_state ^
                     (uint32_t)(session->frame + 1u) *
                         UINT32_C(0x9e3779b9) ^
                     direction * UINT32_C(0x85ebca6b);
    unsigned corner;
    *varied = *source;
    varied->random_seed = light_session_random(&state);
    for (corner = 0u; corner < 4u; ++corner) {
        float dx = session->config->corner_jitter_pixels *
                   light_session_random_signed(&state);
        float dy = session->config->corner_jitter_pixels *
                   light_session_random_signed(&state);
        varied->corners[corner].x = light_clamp_corner(
            varied->corners[corner].x + dx, varied->image_width);
        varied->corners[corner].y = light_clamp_corner(
            varied->corners[corner].y + dy, varied->image_height);
    }
}

static int light_frame_is_scheduled_drop(const light_session *session,
                                         unsigned direction,
                                         unsigned period)
{
    size_t blackout_end = session->config->blackout_start_frame +
                          session->config->blackout_frame_count;
    if (session->config->blackout_frame_count != 0u &&
        session->frame >= session->config->blackout_start_frame &&
        session->frame < blackout_end) {
        return 1;
    }
    if (period != 0u &&
        (session->frame + direction + 1u) % period == 0u) {
        return 1;
    }
    return 0;
}

static int light_deliver_frame(light_session *session,
                               const light_outbound_frame *outbound,
                               const um_light_channel_config *channel,
                               unsigned direction,
                               unsigned drop_period,
                               light_received_frame *received)
{
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t *pixels = NULL;
    size_t pixel_count = 0u;
    um_light_channel_config varied;
    um_light_rx_metrics metrics;
    int status;
    memset(received, 0, sizeof(*received));
    if (light_frame_is_scheduled_drop(session, direction,
                                      drop_period) != 0) {
        ++session->result->scheduled_frame_drops;
        light_session_log(session, "%s frame erased by channel",
                          direction == 0u ? "client -> gateway"
                                          : "gateway -> client");
        return UM_ERR_SYNC;
    }
    status = um_light_encode_frame(
        outbound->type, outbound->session_id, outbound->sequence,
        outbound->payload, outbound->payload_length, modules,
        sizeof(modules));
    if (status != UM_OK) {
        return status;
    }
    light_vary_channel(session, channel, direction, &varied);
    status = um_light_render_frame(modules, sizeof(modules), &varied,
                                   &pixels, &pixel_count);
    if (status != UM_OK) {
        return status;
    }
    status = um_light_decode_frame(
        pixels, varied.image_width, varied.image_height,
        varied.image_width, &received->type, &received->session_id,
        &received->sequence, received->payload, sizeof(received->payload),
        &received->payload_length, &metrics);
    free(pixels);
    if (status != UM_OK) {
        ++session->result->decode_failures;
        light_session_log(session, "%s decoder rejected frame: %s",
                          direction == 0u ? "client -> gateway"
                                          : "gateway -> client",
                          um_status_string(status));
        return status;
    }
    received->present = 1;
    if (direction == 0u) {
        ++session->result->client_to_gateway_decoded_frames;
        session->client_to_gateway_correction_sum +=
            metrics.corrected_bit_fraction;
    } else {
        ++session->result->gateway_to_client_decoded_frames;
        session->gateway_to_client_correction_sum +=
            metrics.corrected_bit_fraction;
    }
    return UM_OK;
}

static int light_control_valid(const light_session *session,
                               const light_received_frame *frame)
{
    if (frame->payload_length == 0u ||
        frame->payload[0] != LIGHT_SESSION_VERSION) {
        return 0;
    }
    if (session->packet_transport == NULL) {
        return frame->payload_length == 1u;
    }
    return frame->payload_length == 4u &&
           frame->payload[1] == LIGHT_PACKET_MODE &&
           light_session_read_u16(&frame->payload[2]) ==
               light_packet_transport_max_packet(
                   session->packet_transport);
}

static int light_process_stream_frame(light_session *session,
                                      const light_received_frame *frame,
                                      light_stream *received_stream,
                                      light_stream *acknowledged_stream)
{
    if (session->packet_transport != NULL) {
        light_packet_transport_status packet_status;
        int packet_result;
        if (frame->type != LIGHT_FRAME_DATA &&
            frame->type != LIGHT_FRAME_ACK) {
            return UM_ERR_HEADER;
        }
        packet_result = light_packet_transport_process(
            session->packet_transport, frame->sequence, frame->payload,
            frame->payload_length, frame->type == LIGHT_FRAME_DATA);
        light_packet_transport_get_status(session->packet_transport,
                                          &packet_status);
        session->result->duplicate_data_frames =
            packet_status.duplicate_cells;
        return packet_result;
    }
    int status = light_stream_apply_ack(acknowledged_stream,
                                        frame->payload,
                                        frame->payload_length);
    if (status != UM_OK) {
        return status;
    }
    if (frame->type == LIGHT_FRAME_ACK) {
        return frame->payload_length == LIGHT_SESSION_ACK_BYTES
                   ? UM_OK
                   : UM_ERR_HEADER;
    }
    if (frame->type != LIGHT_FRAME_DATA ||
        frame->payload_length <= LIGHT_SESSION_ACK_BYTES) {
        return UM_ERR_HEADER;
    }
    status = light_stream_receive(
        received_stream, frame->sequence,
        &frame->payload[LIGHT_SESSION_ACK_BYTES],
        frame->payload_length - LIGHT_SESSION_ACK_BYTES,
        &session->result->duplicate_data_frames);
    if (status == UM_OK &&
        light_stream_receiver_done(received_stream) != 0 &&
        received_stream->completion_logged == 0) {
        received_stream->completion_logged = 1;
        light_session_log(session, "received complete %zu-byte stream",
                          received_stream->total_bytes);
    }
    return status;
}

static void light_reject_protocol(light_session *session,
                                  const char *receiver, int status)
{
    ++session->result->protocol_rejections;
    light_session_log(session, "%s rejected protocol frame: %s",
                      receiver, um_status_string(status));
}

static void light_process_gateway_receive(light_session *session,
                                          const light_received_frame *frame)
{
    int status;
    if (frame->present == 0) {
        return;
    }
    if (frame->type == LIGHT_FRAME_DISCOVER &&
        frame->session_id != 0u &&
        light_control_valid(session, frame) != 0) {
        /*
         * A client starts a new session after its own receive timeout.  Do
         * not let that unilaterally replace a session that the gateway still
         * considers connected: the two peers may simply have different
         * symbol clocks or a short one-way camera outage.  The gateway will
         * accept the new session after its own timeout returns it to
         * LISTENING.
         */
        if (session->gateway_phase == LIGHT_GATEWAY_CONNECTED &&
            session->gateway_session_id != frame->session_id) {
            return;
        }
        if (session->gateway_session_id != frame->session_id ||
            session->gateway_phase == LIGHT_GATEWAY_LISTENING) {
            session->gateway_session_id = frame->session_id;
            session->gateway_phase = LIGHT_GATEWAY_OFFER;
            light_session_log(session,
                              "gateway accepted DISCOVER session=%08x",
                              frame->session_id);
        }
        session->gateway_last_receive = session->frame;
        session->gateway_has_received = 1;
        return;
    }
    if (frame->session_id != session->gateway_session_id ||
        session->gateway_session_id == 0u) {
        return;
    }
    if (session->gateway_phase == LIGHT_GATEWAY_OFFER &&
        frame->type == LIGHT_FRAME_CONFIRM &&
        light_control_valid(session, frame) != 0) {
        session->gateway_phase = LIGHT_GATEWAY_READY;
        session->gateway_last_receive = session->frame;
        session->gateway_has_received = 1;
        light_session_log(session, "gateway accepted CONFIRM");
        return;
    }
    if ((session->gateway_phase == LIGHT_GATEWAY_READY ||
         session->gateway_phase == LIGHT_GATEWAY_CONNECTED) &&
        (frame->type == LIGHT_FRAME_DATA ||
         frame->type == LIGHT_FRAME_ACK)) {
        status = light_process_stream_frame(
            session, frame, &session->client_to_gateway,
            &session->gateway_to_client);
        if (status != UM_OK) {
            light_reject_protocol(session, "gateway", status);
            return;
        }
        session->gateway_last_receive = session->frame;
        session->gateway_has_received = 1;
        if (session->gateway_phase == LIGHT_GATEWAY_READY) {
            session->gateway_phase = LIGHT_GATEWAY_CONNECTED;
            light_session_log(session,
                              "gateway entered full-duplex data state");
        }
        return;
    }
    if (session->gateway_phase == LIGHT_GATEWAY_READY &&
        frame->type == LIGHT_FRAME_CONFIRM &&
        light_control_valid(session, frame) != 0) {
        session->gateway_last_receive = session->frame;
        session->gateway_has_received = 1;
    }
}

static void light_process_client_receive(light_session *session,
                                         const light_received_frame *frame)
{
    int status;
    if (frame->present == 0 ||
        frame->session_id != session->client_session_id) {
        return;
    }
    if ((session->client_phase == LIGHT_CLIENT_DISCOVER ||
         session->client_phase == LIGHT_CLIENT_CONFIRM) &&
        frame->type == LIGHT_FRAME_OFFER &&
        light_control_valid(session, frame) != 0) {
        if (session->client_phase != LIGHT_CLIENT_CONFIRM) {
            light_session_log(session, "client accepted OFFER");
        }
        session->client_phase = LIGHT_CLIENT_CONFIRM;
        session->client_last_receive = session->frame;
        session->client_has_received = 1;
        return;
    }
    if (session->client_phase == LIGHT_CLIENT_CONFIRM &&
        frame->type == LIGHT_FRAME_READY &&
        light_control_valid(session, frame) != 0) {
        session->client_phase = LIGHT_CLIENT_CONNECTED;
        session->client_last_receive = session->frame;
        session->client_has_received = 1;
        if (session->ever_connected != 0) {
            ++session->result->reconnects;
            light_session_log(session, "client reconnected");
        } else {
            session->ever_connected = 1;
            light_session_log(session, "client entered data state");
        }
        return;
    }
    if (session->client_phase == LIGHT_CLIENT_CONNECTED &&
        (frame->type == LIGHT_FRAME_DATA ||
         frame->type == LIGHT_FRAME_ACK)) {
        status = light_process_stream_frame(
            session, frame, &session->gateway_to_client,
            &session->client_to_gateway);
        if (status != UM_OK) {
            light_reject_protocol(session, "client", status);
            return;
        }
        session->client_last_receive = session->frame;
        session->client_has_received = 1;
        return;
    }
    if (session->client_phase == LIGHT_CLIENT_CONNECTED &&
        frame->type == LIGHT_FRAME_READY &&
        light_control_valid(session, frame) != 0) {
        session->client_last_receive = session->frame;
        session->client_has_received = 1;
    }
}

static uint32_t light_next_session_id(light_session *session)
{
    uint32_t value = light_session_random(&session->random_state);
    return value != 0u ? value : UINT32_C(1);
}

static void light_check_timeouts(light_session *session)
{
    if (session->client_phase == LIGHT_CLIENT_CONNECTED &&
        session->client_has_received != 0 &&
        session->frame - session->client_last_receive >=
            session->config->link_timeout_frames) {
        session->client_phase = LIGHT_CLIENT_DISCOVER;
        session->client_session_id = light_next_session_id(session);
        session->client_has_received = 0;
        ++session->result->link_timeouts;
        light_session_log(session,
                          "client link timeout; returning to discovery");
    }
    if (session->gateway_phase == LIGHT_GATEWAY_CONNECTED &&
        session->gateway_has_received != 0 &&
        session->frame - session->gateway_last_receive >=
            session->config->link_timeout_frames) {
        session->gateway_phase = LIGHT_GATEWAY_LISTENING;
        session->gateway_session_id = 0u;
        session->gateway_has_received = 0;
        ++session->result->link_timeouts;
        light_session_log(session,
                          "gateway link timeout; returning to listening");
    }
}

static int light_session_complete(const light_session *session)
{
    return session->client_phase == LIGHT_CLIENT_CONNECTED &&
           session->gateway_phase == LIGHT_GATEWAY_CONNECTED &&
           light_stream_receiver_done(&session->client_to_gateway) != 0 &&
           light_stream_sender_done(&session->client_to_gateway) != 0 &&
           light_stream_receiver_done(&session->gateway_to_client) != 0 &&
           light_stream_sender_done(&session->gateway_to_client) != 0;
}

static int light_validate_channel(const um_light_channel_config *channel)
{
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t payload = LIGHT_SESSION_VERSION;
    uint8_t *pixels = NULL;
    size_t pixel_count = 0u;
    int status = um_light_encode_frame(
        LIGHT_FRAME_IDLE, 0u, 0u, &payload, 1u, modules, sizeof(modules));
    if (status != UM_OK) {
        return status;
    }
    status = um_light_render_frame(modules, sizeof(modules), channel,
                                   &pixels, &pixel_count);
    free(pixels);
    return status;
}

um_light_session_simulation_config
um_light_session_simulation_default_config(void)
{
    um_light_session_simulation_config config;
    memset(&config, 0, sizeof(config));
    config.client_to_gateway = um_light_channel_default_config();
    config.gateway_to_client = um_light_channel_default_config();
    config.client_to_gateway.noise_stddev = 0.075f;
    config.gateway_to_client.corners[0].x = 102.0f;
    config.gateway_to_client.corners[0].y = 118.0f;
    config.gateway_to_client.corners[1].x = 526.0f;
    config.gateway_to_client.corners[1].y = 62.0f;
    config.gateway_to_client.corners[2].x = 566.0f;
    config.gateway_to_client.corners[2].y = 392.0f;
    config.gateway_to_client.corners[3].x = 154.0f;
    config.gateway_to_client.corners[3].y = 438.0f;
    config.gateway_to_client.black_level = 0.12f;
    config.gateway_to_client.white_level = 0.88f;
    config.gateway_to_client.noise_stddev = 0.09f;
    config.gateway_to_client.blur_radius = 2u;
    config.gateway_to_client.random_seed = UINT32_C(0x8b25e4c1);
    config.client_payload_bytes = 4096u;
    config.gateway_payload_bytes = 3072u;
    config.max_frames = 320u;
    config.client_start_frame = 0u;
    config.gateway_start_frame = 0u;
    config.frames_per_second = 15u;
    config.transmit_window = 8u;
    config.retransmit_after_frames = 4u;
    config.link_timeout_frames = 12u;
    config.client_to_gateway_drop_period = 11u;
    config.gateway_to_client_drop_period = 13u;
    config.corner_jitter_pixels = 4.0f;
    config.random_seed = UINT32_C(0x4c53534e);
    return config;
}

um_light_peer_config um_light_peer_default_config(void)
{
    um_light_peer_config config;
    config.transmit_window = 8u;
    config.retransmit_after_frames = 4u;
    config.link_timeout_frames = 45u;
    config.random_seed = UINT32_C(0x4c504545);
    return config;
}

static int light_peer_set_frame(um_light_peer *peer, size_t local_frame)
{
    if (peer->have_frame != 0 && local_frame < peer->last_frame) {
        return UM_ERR_ARGUMENT;
    }
    peer->last_frame = local_frame;
    peer->have_frame = 1;
    peer->session.frame = local_frame;
    return UM_OK;
}

int um_light_peer_create(um_light_peer **peer, um_live_role role,
                         const um_light_peer_config *config,
                         const uint8_t *outgoing, size_t outgoing_length,
                         uint8_t *incoming, size_t incoming_length,
                         um_log_callback logger, void *logger_context)
{
    um_light_peer *created;
    int status;
    if (peer == NULL || config == NULL ||
        (role != UM_LIVE_CLIENT && role != UM_LIVE_GATEWAY) ||
        outgoing_length > LIGHT_SESSION_MAX_BYTES ||
        incoming_length > LIGHT_SESSION_MAX_BYTES ||
        (outgoing_length != 0u && outgoing == NULL) ||
        (incoming_length != 0u && incoming == NULL) ||
        config->transmit_window == 0u ||
        config->transmit_window > LIGHT_SESSION_MAX_WINDOW ||
        config->retransmit_after_frames == 0u ||
        config->link_timeout_frames == 0u) {
        return UM_ERR_ARGUMENT;
    }
    *peer = NULL;
    created = (um_light_peer *)calloc(1u, sizeof(*created));
    if (created == NULL) {
        return UM_ERR_MEMORY;
    }
    created->role = role;
    created->config = um_light_session_simulation_default_config();
    created->config.client_payload_bytes =
        role == UM_LIVE_CLIENT ? outgoing_length : incoming_length;
    created->config.gateway_payload_bytes =
        role == UM_LIVE_GATEWAY ? outgoing_length : incoming_length;
    created->config.transmit_window = config->transmit_window;
    created->config.retransmit_after_frames =
        config->retransmit_after_frames;
    created->config.link_timeout_frames = config->link_timeout_frames;
    created->config.random_seed = config->random_seed;
    created->session.config = &created->config;
    created->session.result = &created->result;
    created->session.logger = logger;
    created->session.logger_context = logger_context;
    created->session.random_state = config->random_seed;
    created->session.client_session_id =
        light_next_session_id(&created->session);
    created->session.client_phase = LIGHT_CLIENT_DISCOVER;
    created->session.gateway_phase = LIGHT_GATEWAY_LISTENING;

    status = light_stream_init(
        &created->session.client_to_gateway,
        created->config.client_payload_bytes,
        role == UM_LIVE_CLIENT ? outgoing : NULL,
        role == UM_LIVE_GATEWAY ? incoming : NULL);
    if (status == UM_OK) {
        status = light_stream_init(
            &created->session.gateway_to_client,
            created->config.gateway_payload_bytes,
            role == UM_LIVE_GATEWAY ? outgoing : NULL,
            role == UM_LIVE_CLIENT ? incoming : NULL);
    }
    if (status != UM_OK) {
        um_light_peer_destroy(created);
        return status;
    }
    *peer = created;
    return UM_OK;
}

void um_light_peer_destroy(um_light_peer *peer)
{
    if (peer == NULL) {
        return;
    }
    light_packet_transport_destroy(peer->session.packet_transport);
    light_stream_destroy(&peer->session.gateway_to_client);
    light_stream_destroy(&peer->session.client_to_gateway);
    free(peer);
}

int um_light_peer_build(um_light_peer *peer, size_t local_frame,
                        um_light_peer_frame *frame)
{
    light_outbound_frame outbound;
    int status;
    if (peer == NULL || frame == NULL) {
        return UM_ERR_ARGUMENT;
    }
    status = light_peer_set_frame(peer, local_frame);
    if (status != UM_OK) {
        return status;
    }
    if (peer->role == UM_LIVE_CLIENT) {
        light_build_client_frame(&peer->session, &outbound);
    } else {
        light_build_gateway_frame(&peer->session, &outbound);
    }
    memset(frame, 0, sizeof(*frame));
    frame->present = 1;
    frame->type = outbound.type;
    frame->session_id = outbound.session_id;
    frame->sequence = outbound.sequence;
    frame->payload_length = outbound.payload_length;
    memcpy(frame->payload, outbound.payload, outbound.payload_length);
    return UM_OK;
}

int um_light_peer_process(um_light_peer *peer, size_t local_frame,
                          const um_light_peer_frame *frame)
{
    light_received_frame received;
    int status;
    if (peer == NULL ||
        (frame != NULL && frame->payload_length > UM_LIGHT_MAX_PAYLOAD)) {
        return UM_ERR_ARGUMENT;
    }
    status = light_peer_set_frame(peer, local_frame);
    if (status != UM_OK) {
        return status;
    }
    memset(&received, 0, sizeof(received));
    if (frame != NULL && frame->present != 0) {
        received.present = 1;
        received.type = frame->type;
        received.session_id = frame->session_id;
        received.sequence = frame->sequence;
        received.payload_length = frame->payload_length;
        memcpy(received.payload, frame->payload, frame->payload_length);
    }
    if (peer->role == UM_LIVE_CLIENT) {
        light_process_client_receive(&peer->session, &received);
    } else {
        light_process_gateway_receive(&peer->session, &received);
    }
    light_check_timeouts(&peer->session);
    return UM_OK;
}

static size_t light_stream_acked_bytes(const light_stream *stream)
{
    size_t bytes = 0u;
    size_t sequence;
    for (sequence = 0u; sequence < stream->chunk_count; ++sequence) {
        if (stream->acked[sequence] != 0u) {
            bytes += light_stream_chunk_length(stream, sequence);
        }
    }
    return bytes;
}

int um_light_peer_complete(const um_light_peer *peer)
{
    if (peer == NULL) {
        return 0;
    }
    if (peer->session.packet_transport != NULL) {
        return 0;
    }
    if (peer->role == UM_LIVE_CLIENT) {
        return peer->session.client_phase == LIGHT_CLIENT_CONNECTED &&
               light_stream_sender_done(
                   &peer->session.client_to_gateway) != 0 &&
               light_stream_receiver_done(
                   &peer->session.gateway_to_client) != 0;
    }
    return peer->session.gateway_phase == LIGHT_GATEWAY_CONNECTED &&
           light_stream_sender_done(&peer->session.gateway_to_client) != 0 &&
           light_stream_receiver_done(&peer->session.client_to_gateway) != 0;
}

void um_light_peer_get_status(const um_light_peer *peer,
                              um_light_peer_status *status)
{
    const light_stream *outgoing;
    const light_stream *incoming;
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (peer == NULL) {
        return;
    }
    if (peer->session.packet_transport != NULL) {
        light_packet_transport_status packet_status;
        light_packet_transport_get_status(peer->session.packet_transport,
                                          &packet_status);
        status->handshake_frames = peer->result.handshake_frames;
        status->data_frames = peer->result.data_frames;
        status->acknowledgement_frames =
            peer->result.acknowledgement_frames;
        status->retransmissions = packet_status.retransmissions;
        status->duplicate_data_frames = packet_status.duplicate_cells;
        status->protocol_rejections = peer->result.protocol_rejections;
        status->reconnects = peer->result.reconnects;
        status->link_timeouts = peer->result.link_timeouts;
        status->outgoing_bytes_acked =
            packet_status.packet_bytes_acked;
        status->incoming_bytes_received =
            packet_status.packet_bytes_received;
        status->outgoing_packets_accepted =
            packet_status.outgoing_packets_accepted;
        status->outgoing_packets_queued =
            packet_status.outgoing_packets_queued;
        status->incoming_packets_received =
            packet_status.incoming_packets_received;
        status->incoming_packets_queued =
            packet_status.incoming_packets_queued;
        status->outgoing_cells_in_flight =
            packet_status.outgoing_cells_in_flight;
        status->connected =
            peer->role == UM_LIVE_CLIENT
                ? peer->session.client_phase == LIGHT_CLIENT_CONNECTED
                : peer->session.gateway_phase == LIGHT_GATEWAY_CONNECTED;
        return;
    }
    outgoing = peer->role == UM_LIVE_CLIENT
                   ? &peer->session.client_to_gateway
                   : &peer->session.gateway_to_client;
    incoming = peer->role == UM_LIVE_CLIENT
                   ? &peer->session.gateway_to_client
                   : &peer->session.client_to_gateway;
    status->handshake_frames = peer->result.handshake_frames;
    status->data_frames = peer->result.data_frames;
    status->acknowledgement_frames = peer->result.acknowledgement_frames;
    status->retransmissions = peer->result.retransmissions;
    status->duplicate_data_frames = peer->result.duplicate_data_frames;
    status->protocol_rejections = peer->result.protocol_rejections;
    status->reconnects = peer->result.reconnects;
    status->link_timeouts = peer->result.link_timeouts;
    status->outgoing_bytes_acked = light_stream_acked_bytes(outgoing);
    status->incoming_bytes_received = incoming->received_bytes;
    status->connected =
        peer->role == UM_LIVE_CLIENT
            ? peer->session.client_phase == LIGHT_CLIENT_CONNECTED
            : peer->session.gateway_phase == LIGHT_GATEWAY_CONNECTED;
}

um_light_packet_peer_config um_light_packet_peer_default_config(void)
{
    um_light_packet_peer_config config;
    config.link = um_light_peer_default_config();
    config.max_packet_bytes = 1500u;
    config.queue_packets = 64u;
    return config;
}

int um_light_packet_peer_create(
    um_light_peer **peer, um_live_role role,
    const um_light_packet_peer_config *config, um_log_callback logger,
    void *logger_context)
{
    um_light_peer *created = NULL;
    int status;
    if (peer == NULL || config == NULL ||
        config->max_packet_bytes == 0u ||
        config->max_packet_bytes > UM_LIGHT_MAX_PACKET) {
        return UM_ERR_ARGUMENT;
    }
    *peer = NULL;
    status = um_light_peer_create(&created, role, &config->link, NULL, 0u,
                                  NULL, 0u, logger, logger_context);
    if (status != UM_OK) {
        return status;
    }
    status = light_packet_transport_create(
        &created->session.packet_transport, config->max_packet_bytes,
        config->queue_packets, config->link.transmit_window,
        config->link.retransmit_after_frames,
        config->link.random_seed ^ (uint32_t)role);
    if (status != UM_OK) {
        um_light_peer_destroy(created);
        return status;
    }
    *peer = created;
    return UM_OK;
}

int um_light_peer_enqueue_packet(um_light_peer *peer,
                                 const uint8_t *packet,
                                 size_t packet_length)
{
    if (peer == NULL || peer->session.packet_transport == NULL) {
        return UM_ERR_ARGUMENT;
    }
    return light_packet_transport_enqueue(peer->session.packet_transport,
                                          packet, packet_length);
}

int um_light_peer_dequeue_packet(um_light_peer *peer, uint8_t *packet,
                                 size_t capacity, size_t *packet_length)
{
    if (peer == NULL || peer->session.packet_transport == NULL) {
        return UM_ERR_ARGUMENT;
    }
    return light_packet_transport_dequeue(peer->session.packet_transport,
                                          packet, capacity,
                                          packet_length);
}

int um_light_simulate_payloads(
    const um_light_session_simulation_config *config,
    um_light_session_simulation_result *result,
    const uint8_t *client_payload, uint8_t *gateway_received,
    const uint8_t *gateway_payload, uint8_t *client_received,
    um_log_callback logger, void *logger_context)
{
    light_session session;
    int status;
    if (config == NULL || result == NULL) {
        return UM_ERR_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    if (config->client_payload_bytes > LIGHT_SESSION_MAX_BYTES ||
        config->gateway_payload_bytes > LIGHT_SESSION_MAX_BYTES ||
        config->max_frames == 0u || config->frames_per_second == 0u ||
        config->frames_per_second > 240u || config->transmit_window == 0u ||
        config->transmit_window > LIGHT_SESSION_MAX_WINDOW ||
        config->retransmit_after_frames == 0u ||
        config->link_timeout_frames == 0u ||
        config->blackout_start_frame >
            SIZE_MAX - config->blackout_frame_count ||
        !isfinite(config->corner_jitter_pixels) ||
        config->corner_jitter_pixels < 0.0f) {
        return UM_ERR_ARGUMENT;
    }
    status = light_validate_channel(&config->client_to_gateway);
    if (status != UM_OK) {
        return status;
    }
    status = light_validate_channel(&config->gateway_to_client);
    if (status != UM_OK) {
        return status;
    }

    memset(&session, 0, sizeof(session));
    session.config = config;
    session.result = result;
    session.logger = logger;
    session.logger_context = logger_context;
    session.random_state = config->random_seed;
    session.client_session_id = light_next_session_id(&session);
    session.client_phase = LIGHT_CLIENT_DISCOVER;
    session.gateway_phase = LIGHT_GATEWAY_LISTENING;
    status = light_stream_init(&session.client_to_gateway,
                               config->client_payload_bytes,
                               client_payload, gateway_received);
    if (status != UM_OK) {
        goto done;
    }
    status = light_stream_init(&session.gateway_to_client,
                               config->gateway_payload_bytes,
                               gateway_payload, client_received);
    if (status != UM_OK) {
        goto done;
    }

    light_session_log(
        &session,
        "full-duplex optical session starts at %u fps client-start=%zu "
        "gateway-start=%zu",
        config->frames_per_second, config->client_start_frame,
        config->gateway_start_frame);
    status = UM_ERR_TIMEOUT;
    for (session.frame = 0u; session.frame < config->max_frames;
         ++session.frame) {
        light_outbound_frame client_frame;
        light_outbound_frame gateway_frame;
        light_received_frame at_gateway;
        light_received_frame at_client;
        int forward_status;
        int reverse_status;
        int client_started =
            session.frame >= config->client_start_frame;
        int gateway_started =
            session.frame >= config->gateway_start_frame;

        memset(&client_frame, 0, sizeof(client_frame));
        memset(&gateway_frame, 0, sizeof(gateway_frame));
        memset(&at_gateway, 0, sizeof(at_gateway));
        memset(&at_client, 0, sizeof(at_client));
        if (client_started != 0) {
            light_build_client_frame(&session, &client_frame);
        }
        if (gateway_started != 0) {
            light_build_gateway_frame(&session, &gateway_frame);
        }
        if (client_started != 0 && gateway_started != 0 &&
            client_frame.type == LIGHT_FRAME_DATA &&
            gateway_frame.type == LIGHT_FRAME_DATA) {
            ++result->simultaneous_data_frames;
        }

        forward_status = UM_ERR_SYNC;
        reverse_status = UM_ERR_SYNC;
        if (client_started != 0 && gateway_started != 0) {
            forward_status = light_deliver_frame(
                &session, &client_frame, &config->client_to_gateway, 0u,
                config->client_to_gateway_drop_period, &at_gateway);
            reverse_status = light_deliver_frame(
                &session, &gateway_frame, &config->gateway_to_client, 1u,
                config->gateway_to_client_drop_period, &at_client);
        }
        if ((forward_status != UM_OK && forward_status != UM_ERR_SYNC &&
             forward_status != UM_ERR_CRC &&
             forward_status != UM_ERR_HEADER) ||
            (reverse_status != UM_OK && reverse_status != UM_ERR_SYNC &&
             reverse_status != UM_ERR_CRC &&
             reverse_status != UM_ERR_HEADER)) {
            status = forward_status != UM_OK ? forward_status
                                             : reverse_status;
            break;
        }

        light_process_gateway_receive(&session, &at_gateway);
        light_process_client_receive(&session, &at_client);
        light_check_timeouts(&session);
        if (light_session_complete(&session) != 0) {
            ++session.frame;
            status = UM_OK;
            break;
        }
    }
    result->frames_elapsed = session.frame;
    result->gateway_received_bytes = session.client_to_gateway.received_bytes;
    result->client_received_bytes = session.gateway_to_client.received_bytes;
    result->elapsed_seconds = (float)result->frames_elapsed /
                              (float)config->frames_per_second;
    if (result->elapsed_seconds > 0.0f) {
        result->payload_goodput_bps =
            (float)(8.0 * (double)(result->gateway_received_bytes +
                                  result->client_received_bytes) /
                    result->elapsed_seconds);
    }
    if (result->client_to_gateway_decoded_frames != 0u) {
        result->client_to_gateway_average_correction =
            (float)(session.client_to_gateway_correction_sum /
                    result->client_to_gateway_decoded_frames);
    }
    if (result->gateway_to_client_decoded_frames != 0u) {
        result->gateway_to_client_average_correction =
            (float)(session.gateway_to_client_correction_sum /
                    result->gateway_to_client_decoded_frames);
    }
    result->final_connected =
        status == UM_OK &&
        session.client_phase == LIGHT_CLIENT_CONNECTED &&
        session.gateway_phase == LIGHT_GATEWAY_CONNECTED;
    if (status == UM_OK) {
        light_session_log(&session,
                          "session complete upload=%zuB download=%zuB",
                          result->gateway_received_bytes,
                          result->client_received_bytes);
    } else {
        light_session_log(&session,
                          "session stopped after %zu frames: %s",
                          result->frames_elapsed,
                          um_status_string(status));
    }

done:
    light_stream_destroy(&session.gateway_to_client);
    light_stream_destroy(&session.client_to_gateway);
    return status;
}

int um_simulate_light_session(
    const um_light_session_simulation_config *config,
    um_light_session_simulation_result *result, um_log_callback logger,
    void *logger_context)
{
    uint8_t *client_payload = NULL;
    uint8_t *gateway_received = NULL;
    uint8_t *gateway_payload = NULL;
    uint8_t *client_received = NULL;
    size_t i;
    int status;
    if (config == NULL || result == NULL) {
        return UM_ERR_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    if (config->client_payload_bytes > LIGHT_SESSION_MAX_BYTES ||
        config->gateway_payload_bytes > LIGHT_SESSION_MAX_BYTES) {
        return UM_ERR_ARGUMENT;
    }
    if (config->client_payload_bytes != 0u) {
        client_payload = (uint8_t *)malloc(config->client_payload_bytes);
        gateway_received = (uint8_t *)malloc(config->client_payload_bytes);
        if (client_payload == NULL || gateway_received == NULL) {
            status = UM_ERR_MEMORY;
            goto done;
        }
        for (i = 0u; i < config->client_payload_bytes; ++i) {
            client_payload[i] = light_stream_byte(
                config->random_seed ^ UINT32_C(0xc11e4701), i);
        }
    }
    if (config->gateway_payload_bytes != 0u) {
        gateway_payload = (uint8_t *)malloc(config->gateway_payload_bytes);
        client_received = (uint8_t *)malloc(config->gateway_payload_bytes);
        if (gateway_payload == NULL || client_received == NULL) {
            status = UM_ERR_MEMORY;
            goto done;
        }
        for (i = 0u; i < config->gateway_payload_bytes; ++i) {
            gateway_payload[i] = light_stream_byte(
                config->random_seed ^ UINT32_C(0x6a7e5a91), i);
        }
    }
    status = um_light_simulate_payloads(
        config, result, client_payload, gateway_received, gateway_payload,
        client_received, logger, logger_context);
    if (status == UM_OK &&
        ((config->client_payload_bytes != 0u &&
          memcmp(client_payload, gateway_received,
                 config->client_payload_bytes) != 0) ||
         (config->gateway_payload_bytes != 0u &&
          memcmp(gateway_payload, client_received,
                 config->gateway_payload_bytes) != 0))) {
        status = UM_ERR_CRC;
        result->final_connected = 0;
    }

done:
    free(client_received);
    free(gateway_payload);
    free(gateway_received);
    free(client_payload);
    return status;
}
