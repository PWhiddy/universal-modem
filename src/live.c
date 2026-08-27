#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "live_wire.h"
#include "network.h"
#include "um_internal.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LIVE_RX_CAPACITY (UM_SAMPLE_RATE * 4u)
#define LIVE_READ_CHUNK 960u
#define LIVE_TX_PRE_SAMPLES 960u
#define LIVE_TX_POST_SAMPLES 2400u
#define LIVE_TURNAROUND_MS 80u
#define LIVE_CALIBRATION_SETTLE_MS 160u
#define LIVE_RECEIVER_ARM_MS 120u
#define LIVE_VERIFY_TRIALS_DEFAULT 3u
#define LIVE_VERIFY_TRIALS_HIGH 5u
#define LIVE_CALIBRATION_RANKS 5u
#define LIVE_CALIB_BEGIN_BYTES 6u
#define LIVE_CALIB_SIZE_RESULT_BYTES 3u
#define LIVE_CALIB_BODY_BASE_BYTES UM_LIVE_MIN_BODY
#define LIVE_CONFIG_BYTES 17u
#define LIVE_READY_BYTES (3u + LIVE_CONFIG_BYTES)
#define LIVE_REPORT_ENTRY_BYTES (2u + LIVE_CONFIG_BYTES)
#define LIVE_CACHE_BODY_BYTES (1u + LIVE_CONFIG_BYTES + 2u)
#define LIVE_PROXY_FRAGMENT_HEADER_BYTES 8u
#define LIVE_PROXY_BATCH_HEADER_BYTES 6u
#define LIVE_PROXY_BATCH_ENTRY_BYTES 2u
#define LIVE_PROXY_ACK_BYTES 5u
#define LIVE_PROXY_BEGIN_BYTES 7u
#define LIVE_PROXY_BURST_PACKETS 4u
#define LIVE_PROXY_QUEUE_PACKETS 64u
#define LIVE_PROXY_RECENT_DNS 64u
#define LIVE_PROXY_INFLIGHT_DNS 64u
#define LIVE_PROXY_DNS_RETRY_SUPPRESS_MS 30000u
#define LIVE_PROXY_DNS_INFLIGHT_SUPPRESS_MS 15000u
#define LIVE_PROXY_DNS_QUEUE_LIMIT 48u
#define LIVE_PROXY_DISCOVERY_DNS_QUEUE_LIMIT 4u
#define LIVE_PROXY_DNS_BURST_PACKETS 3u
#define LIVE_PROXY_INGRESS_DRAIN_LIMIT 512u
#define LIVE_PROXY_IDLE_MS 350u
#define LIVE_PROXY_ACK_TIMEOUT_MS 5000u
#define LIVE_PROXY_RECEIVE_TIMEOUT_MS 6500u
#define LIVE_PROXY_TOTAL_MASK UINT16_C(0x7fff)
#define LIVE_PROXY_YIELD_FLAG UINT16_C(0x8000)
#define LIVE_PROXY_ACK_YIELD 0x01u
#define LIVE_PROXY_BATCH_YIELD 0x01u

enum {
    LIVE_PROXY_PRIORITY_BULK = 0,
    LIVE_PROXY_PRIORITY_NORMAL = 1,
    LIVE_PROXY_PRIORITY_CONTROL = 2,
    LIVE_PROXY_PRIORITY_DNS = 3
};

typedef struct {
    uint8_t packet[UM_NETWORK_MAX_PACKET];
    size_t length;
    unsigned priority;
} live_proxy_queued_packet;

typedef struct {
    uint32_t client_address;
    uint32_t question_hash;
    uint16_t client_port;
    uint16_t transaction_id;
    uint16_t question_type;
    uint16_t question_class;
} live_proxy_dns_key;

typedef struct {
    live_proxy_dns_key key;
    uint64_t completed_ms;
    int valid;
} live_proxy_recent_dns;

typedef struct {
    live_proxy_dns_key key;
    uint64_t sent_ms;
    int valid;
} live_proxy_inflight_dns;

typedef struct {
    um_live_audio_options options;
    um_audio *audio;
    um_network *network;
    um_log_callback logger;
    void *logger_context;
    uint32_t session_id;
    um_modem_config client_to_gateway;
    um_modem_config gateway_to_client;
    size_t client_to_gateway_body_bytes;
    size_t gateway_to_client_body_bytes;
    int have_client_to_gateway;
    int have_gateway_to_client;
    int link_stage_started;
    float *receive_samples;
} live_context;

typedef struct {
    uint16_t transmit_sequence;
    uint16_t receive_sequence;
    uint16_t turn_sequence;
    uint16_t last_committed_turn;
    uint16_t last_commit_packet_sequence;
    uint32_t transmit_packet_id;
    uint32_t transmit_batch_id;
    uint32_t receive_packet_id;
    uint32_t last_completed_packet_id;
    uint32_t last_completed_batch_id;
    uint32_t last_commit_packet_id;
    size_t receive_total;
    size_t receive_offset;
    size_t receive_fragments;
    uint8_t receive_packet[UM_NETWORK_MAX_PACKET];
    size_t packets_sent;
    size_t packets_received;
    size_t batches_sent;
    size_t batches_received;
    size_t bytes_sent;
    size_t bytes_received;
    size_t dns_bytes_sent;
    size_t dns_bytes_received;
    live_proxy_queued_packet queue[LIVE_PROXY_QUEUE_PACKETS];
    live_proxy_recent_dns recent_dns[LIVE_PROXY_RECENT_DNS];
    live_proxy_inflight_dns inflight_dns[LIVE_PROXY_INFLIGHT_DNS];
    size_t queue_count;
    size_t queue_dropped;
    size_t queue_duplicates;
    size_t dns_retries_suppressed;
    size_t multicast_dropped;
    size_t broadcast_dropped;
    size_t stale_dns_icmp_dropped;
    size_t discovery_dns_deprioritized;
    size_t discovery_dns_dropped;
    size_t tcp_acks_coalesced;
    size_t queue_priority_evictions;
    size_t queue_logged_dropped;
    size_t queue_logged_duplicates;
    size_t dns_retries_logged_suppressed;
    size_t multicast_logged_dropped;
    size_t broadcast_logged_dropped;
    size_t stale_dns_icmp_logged_dropped;
    size_t discovery_dns_logged_deprioritized;
    size_t discovery_dns_logged_dropped;
    size_t tcp_acks_logged_coalesced;
    size_t queue_logged_priority_evictions;
    size_t consecutive_dns_sent;
    uint64_t started_ms;
    int have_last_completed_packet;
    int have_last_completed_batch;
    int have_last_commit;
    int last_commit_piggybacked;
    int receive_yield_requested;
} live_proxy_state;

typedef struct {
    size_t index;
    um_modem_config config;
    float score;
} live_ranked_candidate;

static volatile sig_atomic_t live_interrupted = 0;

static int fatal_live_status(int status);
static const char *direction_label(unsigned direction);
static int proxy_packet_is_tunnel_discovery_dns(const uint8_t *packet,
                                                size_t length);

static void handle_signal(int signal_number)
{
    (void)signal_number;
    live_interrupted = 1;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static void sleep_milliseconds(unsigned milliseconds)
{
    struct timespec duration;
    duration.tv_sec = (time_t)(milliseconds / 1000u);
    duration.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&duration, &duration) != 0 && !live_interrupted) {
    }
}

static void sleep_until(uint64_t target)
{
    while (!live_interrupted) {
        uint64_t now = monotonic_milliseconds();
        uint64_t remaining;
        if (now >= target) {
            return;
        }
        remaining = target - now;
        sleep_milliseconds((unsigned)(remaining > 50u ? 50u : remaining));
    }
}

static void live_log(live_context *context, const char *format, ...)
{
    char line[512];
    va_list arguments;
    if (context->logger == NULL) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    context->logger(context->logger_context, line);
}

static const char *wire_name(um_live_wire_type type)
{
    switch (type) {
    case UM_WIRE_DISCOVER:
        return "DISCOVER";
    case UM_WIRE_OFFER:
        return "OFFER";
    case UM_WIRE_CONFIRM:
        return "CONFIRM";
    case UM_WIRE_CONNECTED:
        return "CONNECTED";
    case UM_WIRE_CALIB_BEGIN:
        return "CALIB_BEGIN";
    case UM_WIRE_CALIB_READY:
        return "CALIB_READY";
    case UM_WIRE_CALIB_PROBE:
        return "CALIB_PROBE";
    case UM_WIRE_CALIB_REPORT:
        return "CALIB_REPORT";
    case UM_WIRE_CALIB_VERIFY:
        return "CALIB_VERIFY";
    case UM_WIRE_CALIB_VERIFY_RESULT:
        return "CALIB_VERIFY_RESULT";
    case UM_WIRE_TEST_BEGIN:
        return "TEST_BEGIN";
    case UM_WIRE_DATA:
        return "DATA";
    case UM_WIRE_ACK:
        return "ACK";
    case UM_WIRE_TURN:
        return "TURN";
    case UM_WIRE_COMPLETE:
        return "COMPLETE";
    case UM_WIRE_CALIB_CACHE:
        return "CALIB_CACHE";
    case UM_WIRE_PROXY_BEGIN:
        return "PROXY_BEGIN";
    case UM_WIRE_IP_FRAGMENT:
        return "IP_FRAGMENT";
    case UM_WIRE_IP_ACK:
        return "IP_ACK";
    case UM_WIRE_PROXY_TURN:
        return "PROXY_TURN";
    case UM_WIRE_PROXY_TURN_ACK:
        return "PROXY_TURN_ACK";
    case UM_WIRE_PROXY_TURN_COMMIT:
        return "PROXY_TURN_COMMIT";
    case UM_WIRE_PROXY_COMPLETE:
        return "PROXY_COMPLETE";
    case UM_WIRE_IP_BATCH:
        return "IP_BATCH";
    case UM_WIRE_CALIB_BODY_PROBE:
        return "CALIB_BODY_PROBE";
    case UM_WIRE_CALIB_BODY_RESULT:
        return "CALIB_BODY_RESULT";
    default:
        return "UNKNOWN";
    }
}

static const char *fec_name(um_fec_rate rate)
{
    switch (rate) {
    case UM_FEC_RATE_1_2:
        return "1/2";
    case UM_FEC_RATE_2_3:
        return "2/3";
    case UM_FEC_RATE_3_4:
        return "3/4";
    default:
        return "?";
    }
}

static unsigned live_verification_trials(int high_quality)
{
    return high_quality != 0 ? LIVE_VERIFY_TRIALS_HIGH
                             : LIVE_VERIFY_TRIALS_DEFAULT;
}

static unsigned live_body_verification_trials(int high_quality)
{
    return high_quality != 0 ? 3u : 2u;
}

static const size_t live_body_candidates[] = {256u, 384u, 512u};

static void log_received_quality(live_context *context, const char *message,
                                 const um_rx_metrics *metrics)
{
    double level_dbfs = metrics->signal_rms > 1.0e-12f
                            ? 20.0 * log10((double)metrics->signal_rms)
                            : -240.0;
    live_log(context,
             "%s sync=%.3f level=%.1fdBFS peak=%.3f norm=%.2fx "
             "clip=%.3f%% snr=%.1fdB evm=%.3f",
             message, metrics->sync_correlation, level_dbfs,
             metrics->input_peak, metrics->normalization_gain,
             100.0 * (double)metrics->clipped_sample_fraction,
             metrics->estimated_snr_db, metrics->evm_rms);
}

static um_modem_config live_bootstrap_config(void)
{
    return um_modem_robust_config();
}

static um_modem_config live_calibration_control_config(void)
{
    return live_bootstrap_config();
}

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

void um_live_handshake_body(uint8_t body[UM_LIVE_HANDSHAKE_BYTES],
                            int link_test)
{
    body[0] = UM_LIVE_PROTOCOL_VERSION;
    body[1] = UM_LIVE_CONFIG_FORMAT_VERSION;
    write_u16(&body[2], (uint16_t)UM_CALIBRATION_PROBE_BYTES);
    /* Keep the proven five-byte bootstrap payload.  The low bit is mode and
     * the remaining bits carry the proxy format generation. */
    body[4] = (uint8_t)((UM_LIVE_PROXY_FORMAT_VERSION << 1u) |
                        (link_test != 0 ? 1u : 0u));
}

int um_live_handshake_validate(const uint8_t *body, size_t body_length,
                               int link_test)
{
    if (body == NULL || body_length != UM_LIVE_HANDSHAKE_BYTES) {
        return UM_ERR_UNSUPPORTED;
    }
    return body[0] == UM_LIVE_PROTOCOL_VERSION &&
                   body[1] == UM_LIVE_CONFIG_FORMAT_VERSION &&
                   read_u16(&body[2]) == UM_CALIBRATION_PROBE_BYTES &&
                   (body[4] & 1u) == (link_test != 0 ? 1u : 0u) &&
                   (body[4] >> 1u) == UM_LIVE_PROXY_FORMAT_VERSION
               ? UM_OK
               : UM_ERR_UNSUPPORTED;
}

static int validate_handshake_body(live_context *context, const char *frame,
                                   const um_live_wire_message *message)
{
    unsigned protocol = 0u;
    unsigned config_format = 0u;
    unsigned probe_bytes = 0u;
    unsigned peer_link_test = 2u;
    unsigned proxy_format = 0u;
    if (message->body_length >= 1u) {
        protocol = message->body[0];
    }
    if (message->body_length >= 2u) {
        config_format = message->body[1];
    }
    if (message->body_length >= 4u) {
        probe_bytes = read_u16(&message->body[2]);
    }
    if (message->body_length >= 6u &&
        (message->body[4] == 0u || message->body[4] == 1u)) {
        /* Diagnose the immediately preceding six-byte capability layout. */
        peer_link_test = message->body[4];
        proxy_format = message->body[5];
    } else if (message->body_length >= 5u) {
        peer_link_test = message->body[4] & 1u;
        proxy_format = message->body[4] >> 1u;
    }
    if (message->body_length == UM_LIVE_HANDSHAKE_BYTES) {
        if (um_live_handshake_validate(message->body,
                                       message->body_length,
                                       context->options.link_test) == UM_OK) {
            return UM_OK;
        }
    }
    live_log(context,
             "Incompatible peer %s capability length=%zu protocol=%u "
             "config-format=%u probe-bytes=%u mode=%s proxy-format=%u; "
             "need protocol=%u config-format=%u probe-bytes=%u mode=%s "
             "proxy-format=%u. Rebuild both machines "
             "from the same revision and use --link-test on both or neither",
             frame, message->body_length, protocol, config_format,
             probe_bytes,
             peer_link_test == 1u ? "link-test"
                                  : peer_link_test == 0u ? "proxy" : "?",
             proxy_format,
             UM_LIVE_PROTOCOL_VERSION, UM_LIVE_CONFIG_FORMAT_VERSION,
             UM_CALIBRATION_PROBE_BYTES,
             context->options.link_test != 0 ? "link-test" : "proxy",
             UM_LIVE_PROXY_FORMAT_VERSION);
    return UM_ERR_UNSUPPORTED;
}

static void encode_modem_config(const um_modem_config *config, uint8_t *bytes)
{
    bytes[0] = UM_LIVE_CONFIG_FORMAT_VERSION;
    write_u16(&bytes[1], (uint16_t)config->first_bin);
    write_u16(&bytes[3], (uint16_t)config->last_bin);
    write_u16(&bytes[5], (uint16_t)config->cyclic_prefix);
    write_u16(&bytes[7], (uint16_t)config->window_samples);
    write_u16(&bytes[9], (uint16_t)config->sync_samples);
    write_u16(&bytes[11], (uint16_t)config->sync_gap);
    bytes[13] = (uint8_t)config->training_symbols;
    bytes[14] = (uint8_t)config->symbol_repetitions;
    bytes[15] = (uint8_t)config->qam_bits;
    bytes[16] = (uint8_t)config->fec_rate;
}

static int decode_modem_config(const uint8_t *bytes, size_t length,
                               um_modem_config *config)
{
    if (bytes == NULL || config == NULL || length != LIVE_CONFIG_BYTES ||
        bytes[0] != UM_LIVE_CONFIG_FORMAT_VERSION) {
        return UM_ERR_HEADER;
    }
    memset(config, 0, sizeof(*config));
    config->fft_size = UM_FFT_SIZE;
    config->first_bin = read_u16(&bytes[1]);
    config->last_bin = read_u16(&bytes[3]);
    config->cyclic_prefix = read_u16(&bytes[5]);
    config->window_samples = read_u16(&bytes[7]);
    config->sync_samples = read_u16(&bytes[9]);
    config->sync_gap = read_u16(&bytes[11]);
    config->training_symbols = bytes[13];
    config->symbol_repetitions = bytes[14];
    config->qam_bits = bytes[15];
    config->fec_rate = (um_fec_rate)bytes[16];
    return um_modem_config_validate(config) == UM_OK ? UM_OK : UM_ERR_HEADER;
}

static size_t encode_calibration_cache(
    int available, const um_modem_config *config, size_t frame_body_bytes,
    uint8_t body[LIVE_CACHE_BODY_BYTES])
{
    body[0] = available != 0 ? 1u : 0u;
    if (available == 0) {
        return 1u;
    }
    encode_modem_config(config, &body[1]);
    write_u16(&body[1u + LIVE_CONFIG_BYTES],
              (uint16_t)frame_body_bytes);
    return LIVE_CACHE_BODY_BYTES;
}

static int decode_calibration_cache(const um_live_wire_message *message,
                                    int *available,
                                    um_modem_config *config,
                                    size_t *frame_body_bytes)
{
    if (message == NULL || available == NULL || config == NULL ||
        frame_body_bytes == NULL ||
        message->body_length == 0u || message->body[0] > 1u) {
        return UM_ERR_HEADER;
    }
    *available = message->body[0] != 0u;
    if (*available == 0) {
        *frame_body_bytes = 0u;
        return message->body_length == 1u ? UM_OK : UM_ERR_HEADER;
    }
    if (message->body_length != LIVE_CACHE_BODY_BYTES) {
        return UM_ERR_HEADER;
    }
    *frame_body_bytes = read_u16(&message->body[1u + LIVE_CONFIG_BYTES]);
    if (*frame_body_bytes < LIVE_CALIB_BODY_BASE_BYTES ||
        *frame_body_bytes > UM_LIVE_MAX_BODY) {
        return UM_ERR_HEADER;
    }
    return decode_modem_config(&message->body[1], LIVE_CONFIG_BYTES, config);
}

static int send_wire(live_context *context, const um_modem_config *config,
                     um_live_wire_type type, uint16_t sequence,
                     const uint8_t *body, size_t body_length,
                     int resume_capture, float *duration_seconds)
{
    uint8_t wire[UM_LIVE_MAX_WIRE];
    size_t wire_length = 0u;
    float *modulated = NULL;
    size_t modulated_count = 0u;
    float *guarded = NULL;
    size_t guarded_count;
    int status;

    status = um_live_wire_encode(type, context->session_id, sequence, body,
                                 body_length, wire, sizeof(wire), &wire_length);
    if (status != UM_OK) {
        return status;
    }
    status = um_modulate_frame(config, wire, wire_length, sequence, &modulated,
                               &modulated_count);
    if (status != UM_OK) {
        return status;
    }
    if (modulated_count > SIZE_MAX - LIVE_TX_PRE_SAMPLES -
                              LIVE_TX_POST_SAMPLES) {
        free(modulated);
        return UM_ERR_CAPACITY;
    }
    guarded_count = LIVE_TX_PRE_SAMPLES + modulated_count +
                    LIVE_TX_POST_SAMPLES;
    guarded = (float *)calloc(guarded_count, sizeof(*guarded));
    if (guarded == NULL) {
        free(modulated);
        return UM_ERR_MEMORY;
    }
    memcpy(guarded + LIVE_TX_PRE_SAMPLES, modulated,
           modulated_count * sizeof(*modulated));
    status = um_audio_capture_enable(context->audio, 0);
    if (status == UM_OK) {
        status = um_audio_write(context->audio, guarded, guarded_count);
    }
    if (resume_capture != 0) {
        int resume_status = um_audio_capture_enable(context->audio, 1);
        if (status == UM_OK) {
            status = resume_status;
        }
        if (status == UM_OK) {
            status = um_audio_flush_capture(context->audio);
        }
    }
    if (duration_seconds != NULL) {
        *duration_seconds = (float)guarded_count / (float)UM_SAMPLE_RATE;
    }
    free(guarded);
    free(modulated);
    return status;
}

static int receive_wire(live_context *context, const um_modem_config *config,
                        uint32_t expected_session, unsigned timeout_ms,
                        um_live_wire_message *message,
                        um_rx_metrics *received_metrics)
{
    uint64_t deadline = monotonic_milliseconds() + timeout_ms;
    size_t sample_count = 0u;
    um_rx_metrics metrics;
    while (!live_interrupted && monotonic_milliseconds() < deadline) {
        size_t frames = 0u;
        unsigned wait_ms;
        int status;
        uint64_t now = monotonic_milliseconds();
        if (sample_count == LIVE_RX_CAPACITY) {
            size_t retain = config->sync_samples * 2u;
            memmove(context->receive_samples,
                    context->receive_samples + sample_count - retain,
                    retain * sizeof(*context->receive_samples));
            sample_count = retain;
        }
        wait_ms = (unsigned)(deadline - now);
        if (wait_ms > 50u) {
            wait_ms = 50u;
        }
        status = um_audio_read(
            context->audio, context->receive_samples + sample_count,
            LIVE_RX_CAPACITY - sample_count < LIVE_READ_CHUNK
                ? LIVE_RX_CAPACITY - sample_count
                : LIVE_READ_CHUNK,
            wait_ms, &frames);
        if (status == UM_ERR_TIMEOUT) {
            continue;
        }
        if (status != UM_OK) {
            return status;
        }
        sample_count += frames;
        if (sample_count < config->sync_samples + config->sync_gap +
                               config->fft_size) {
            continue;
        }
        {
            uint8_t wire[UM_LIVE_MAX_WIRE];
            size_t wire_length = 0u;
            uint16_t modem_sequence = 0u;
            memset(&metrics, 0, sizeof(metrics));
            status = um_demodulate_frame(
                config, context->receive_samples, sample_count, wire,
                sizeof(wire), &wire_length, &modem_sequence, &metrics);
            if (received_metrics != NULL) {
                *received_metrics = metrics;
            }
            if (status == UM_OK) {
                status = um_live_wire_decode(wire, wire_length, message);
                if (status != UM_OK) {
                    return status;
                }
                if (message->sequence != modem_sequence) {
                    return UM_ERR_HEADER;
                }
                if (expected_session != 0u &&
                    message->session_id != expected_session) {
                    sample_count = 0u;
                    continue;
                }
                return UM_OK;
            }
            if (status == UM_ERR_SYNC) {
                size_t retain = config->sync_samples * 2u;
                if (sample_count > retain) {
                    memmove(context->receive_samples,
                            context->receive_samples + sample_count - retain,
                            retain * sizeof(*context->receive_samples));
                    sample_count = retain;
                }
            } else if (status != UM_ERR_TRUNCATED) {
                return status;
            }
        }
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_ERR_TIMEOUT;
}

static int receive_expected(live_context *context,
                            const um_modem_config *config,
                            um_live_wire_type expected_type,
                            uint16_t expected_sequence, unsigned timeout_ms,
                            um_live_wire_message *message,
                            um_rx_metrics *metrics)
{
    uint64_t deadline = monotonic_milliseconds() + timeout_ms;
    while (!live_interrupted && monotonic_milliseconds() < deadline) {
        uint64_t now = monotonic_milliseconds();
        unsigned remaining = (unsigned)(deadline - now);
        int status = receive_wire(context, config, context->session_id,
                                  remaining, message, metrics);
        if (status == UM_ERR_HEADER || status == UM_ERR_CRC) {
            live_log(context, "Rejected damaged frame while waiting for %s: "
                              "%s",
                     wire_name(expected_type), um_status_string(status));
            continue;
        }
        if (status != UM_OK) {
            return status;
        }
        if (message->type == expected_type &&
            message->sequence == expected_sequence) {
            return UM_OK;
        }
        live_log(context, "Ignoring %s while waiting for %s",
                 wire_name(message->type), wire_name(expected_type));
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_ERR_TIMEOUT;
}

static int receive_calibration_ready(live_context *context,
                                     const um_modem_config *config,
                                     unsigned direction, size_t candidate,
                                     unsigned timeout_ms,
                                     um_live_wire_message *message)
{
    uint64_t deadline = monotonic_milliseconds() + timeout_ms;
    while (!live_interrupted && monotonic_milliseconds() < deadline) {
        uint64_t now = monotonic_milliseconds();
        unsigned remaining = (unsigned)(deadline - now);
        int status = receive_wire(context, config, context->session_id,
                                  remaining, message, NULL);
        if (status == UM_ERR_HEADER || status == UM_ERR_CRC) {
            live_log(context,
                     "Rejected damaged frame while waiting for CALIB_READY: "
                     "%s",
                     um_status_string(status));
            continue;
        }
        if (status != UM_OK) {
            return status;
        }
        if (message->type == UM_WIRE_CALIB_READY &&
            message->sequence == (uint16_t)candidate) {
            return UM_OK;
        }
        /* A report may end an adaptive search before its maximum budget. */
        if (message->type == UM_WIRE_CALIB_REPORT &&
            message->sequence == (uint16_t)direction &&
            message->body_length >= 4u) {
            if (read_u16(message->body) == 0u && message->body[2] == 0u &&
                message->body[3] != 0u) {
                const char *reason =
                    message->body[3] == 1u
                        ? "baseline decode failed"
                        : message->body[3] == 2u
                              ? "baseline margin failed"
                              : "no full-size robust mode passed";
                live_log(context,
                         "calibration peer aborted at probe=%zu reason=%s",
                         candidate + 1u, reason);
                return message->body[3] == 2u ? UM_ERR_RELIABILITY
                                              : UM_ERR_CRC;
            }
            return UM_OK;
        }
        live_log(context, "Ignoring %s while waiting for CALIB_READY",
                 wire_name(message->type));
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_ERR_TIMEOUT;
}

static uint32_t make_session_id(void)
{
    struct timespec now;
    uint32_t value;
    (void)clock_gettime(CLOCK_REALTIME, &now);
    value = (uint32_t)now.tv_nsec ^ (uint32_t)now.tv_sec ^
            (uint32_t)getpid() * UINT32_C(0x9e3779b9);
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    return value != 0u ? value : UINT32_C(0x554d0001);
}

static int client_connect(live_context *context)
{
    um_modem_config bootstrap = live_bootstrap_config();
    uint8_t handshake[UM_LIVE_HANDSHAKE_BYTES];
    uint16_t attempt = 0u;
    um_live_handshake_body(handshake, context->options.link_test);
    while (!live_interrupted) {
        um_live_wire_message message;
        um_rx_metrics metrics;
        int status;
        uint64_t started = monotonic_milliseconds();
        live_log(context, "state=DISCOVERING tx=DISCOVER attempt=%u session=%08x",
                 (unsigned)attempt + 1u, context->session_id);
        status = send_wire(context, &bootstrap, UM_WIRE_DISCOVER, attempt,
                           handshake, sizeof(handshake), 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        status = receive_expected(context, &bootstrap, UM_WIRE_OFFER, attempt,
                                  2200u, &message, &metrics);
        if (status == UM_OK) {
            status = validate_handshake_body(context, "OFFER", &message);
            if (status != UM_OK) {
                return status;
            }
            log_received_quality(context, "rx=OFFER", &metrics);
            sleep_milliseconds(LIVE_TURNAROUND_MS);
            live_log(context, "state=NEGOTIATING rx=OFFER tx=CONFIRM");
            status = send_wire(context, &bootstrap, UM_WIRE_CONFIRM, attempt,
                               handshake, sizeof(handshake), 1, NULL);
            if (status != UM_OK) {
                return status;
            }
            status = receive_expected(context, &bootstrap, UM_WIRE_CONNECTED,
                                      attempt, 2500u, &message, &metrics);
            if (status == UM_OK) {
                status = validate_handshake_body(context, "CONNECTED",
                                                 &message);
                if (status != UM_OK) {
                    return status;
                }
                log_received_quality(context, "rx=CONNECTED", &metrics);
                live_log(context, "state=CONNECTED handshake complete");
                return UM_OK;
            }
        }
        live_log(context, "No connection offer (%s); retrying",
                 um_status_string(status));
        {
            uint64_t interval =
                (uint64_t)(context->options.discovery_interval_seconds *
                           1000.0f);
            sleep_until(started + (interval > 200u ? interval : 200u));
        }
        ++attempt;
    }
    return UM_ERR_INTERRUPTED;
}

static int gateway_connect(live_context *context)
{
    um_modem_config bootstrap = live_bootstrap_config();
    uint8_t handshake[UM_LIVE_HANDSHAKE_BYTES];
    um_live_handshake_body(handshake, context->options.link_test);
    live_log(context, "state=LISTENING waiting for client DISCOVER");
    while (!live_interrupted) {
        um_live_wire_message message;
        um_rx_metrics metrics;
        int status = receive_wire(context, &bootstrap, 0u, 5000u, &message,
                                  &metrics);
        if (status == UM_ERR_TIMEOUT || status == UM_ERR_SYNC ||
            status == UM_ERR_HEADER || status == UM_ERR_CRC) {
            continue;
        }
        if (status != UM_OK) {
            return status;
        }
        if (message.type != UM_WIRE_DISCOVER) {
            continue;
        }
        status = validate_handshake_body(context, "DISCOVER", &message);
        if (status != UM_OK) {
            return status;
        }
        log_received_quality(context, "rx=DISCOVER", &metrics);
        context->session_id = message.session_id;
        {
            uint16_t offer_sequence = message.sequence;
            int refreshing = 0;
            for (;;) {
                uint64_t deadline;
                sleep_milliseconds(LIVE_TURNAROUND_MS);
                live_log(context,
                         refreshing != 0
                             ? "state=NEGOTIATING rx=DISCOVER retry "
                               "tx=OFFER session=%08x sequence=%u"
                             : "state=NEGOTIATING rx=DISCOVER tx=OFFER "
                               "session=%08x sequence=%u",
                         context->session_id, (unsigned)offer_sequence);
                status = send_wire(context, &bootstrap, UM_WIRE_OFFER,
                                   offer_sequence, handshake,
                                   sizeof(handshake), 1, NULL);
                if (status != UM_OK) {
                    return status;
                }
                refreshing = 0;
                deadline = monotonic_milliseconds() + 2600u;
                while (!live_interrupted &&
                       monotonic_milliseconds() < deadline) {
                    uint64_t now = monotonic_milliseconds();
                    unsigned remaining = (unsigned)(deadline - now);
                    status = receive_wire(context, &bootstrap,
                                          context->session_id, remaining,
                                          &message, &metrics);
                    if (status == UM_ERR_HEADER || status == UM_ERR_CRC ||
                        status == UM_ERR_SYNC ||
                        status == UM_ERR_TRUNCATED) {
                        live_log(context,
                                 "Rejected damaged frame while waiting for "
                                 "CONFIRM: %s",
                                 um_status_string(status));
                        continue;
                    }
                    if (status != UM_OK) {
                        break;
                    }
                    if (message.type == UM_WIRE_DISCOVER) {
                        status = validate_handshake_body(context, "DISCOVER",
                                                         &message);
                        if (status != UM_OK) {
                            return status;
                        }
                        log_received_quality(context, "rx=DISCOVER retry",
                                             &metrics);
                        offer_sequence = message.sequence;
                        refreshing = 1;
                        break;
                    }
                    if (message.type == UM_WIRE_CONFIRM &&
                        message.sequence == offer_sequence) {
                        status = validate_handshake_body(context, "CONFIRM",
                                                         &message);
                        if (status != UM_OK) {
                            return status;
                        }
                        log_received_quality(context, "rx=CONFIRM", &metrics);
                        sleep_milliseconds(LIVE_TURNAROUND_MS);
                        status = send_wire(context, &bootstrap,
                                           UM_WIRE_CONNECTED,
                                           message.sequence, handshake,
                                           sizeof(handshake), 1, NULL);
                        if (status != UM_OK) {
                            return status;
                        }
                        live_log(context,
                                 "state=CONNECTED handshake complete");
                        return UM_OK;
                    }
                    live_log(context, "Ignoring %s while waiting for CONFIRM",
                             wire_name(message.type));
                }
                if (refreshing != 0) {
                    continue;
                }
                live_log(context,
                         "CONFIRM not received (%s); listening again",
                         um_status_string(status));
                break;
            }
        }
    }
    return UM_ERR_INTERRUPTED;
}

static void fill_calibration_body(size_t candidate, unsigned trial,
                                  uint8_t *body, size_t count)
{
    uint32_t state = UINT32_C(0x6a09e667) ^ (uint32_t)candidate *
                                                    UINT32_C(0x9e3779b9) ^
                     trial * UINT32_C(0x85ebca6b);
    size_t i;
    for (i = 0u; i < count; ++i) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        body[i] = (uint8_t)state;
    }
}

static int calibration_body_sender(live_context *context,
                                   unsigned direction,
                                   const um_modem_config *config,
                                   size_t maximum_body_bytes,
                                   size_t *selected_body_bytes)
{
    um_modem_config bootstrap = live_bootstrap_config();
    unsigned trials = live_body_verification_trials(
        context->options.calibrate_high_quality);
    uint16_t sequence = 0u;
    size_t candidate_index;
    *selected_body_bytes = LIVE_CALIB_BODY_BASE_BYTES;
    for (candidate_index = 0u;
         candidate_index < sizeof(live_body_candidates) /
                               sizeof(live_body_candidates[0]);
         ++candidate_index) {
        size_t candidate = live_body_candidates[candidate_index];
        unsigned trial;
        if (candidate > maximum_body_bytes) {
            break;
        }
        for (trial = 0u; trial < trials; ++trial, ++sequence) {
            uint8_t probe[UM_LIVE_MAX_BODY];
            um_live_wire_message result;
            float duration = 0.0f;
            int status;
            fill_calibration_body(candidate, trial + 100u, probe,
                                  candidate);
            sleep_milliseconds(LIVE_RECEIVER_ARM_MS);
            status = send_wire(context, config, UM_WIRE_CALIB_BODY_PROBE,
                               sequence, probe, candidate, 1, &duration);
            if (status != UM_OK) {
                return status;
            }
            live_log(context,
                     "calib body tx direction=%s size=%zu trial=%u/%u "
                     "duration=%.3fs",
                     direction_label(direction), candidate, trial + 1u,
                     trials, duration);
            status = receive_expected(
                context, &bootstrap, UM_WIRE_CALIB_BODY_RESULT, sequence,
                8500u, &result, NULL);
            if (status != UM_OK) {
                return status;
            }
            if (result.body_length != LIVE_CALIB_SIZE_RESULT_BYTES ||
                read_u16(&result.body[1]) != candidate) {
                return UM_ERR_HEADER;
            }
            if (result.body[0] == 0u) {
                live_log(context,
                         "calib body size=%zu failed; selected=%zu",
                         candidate, *selected_body_bytes);
                return UM_OK;
            }
            if (result.body[0] != 1u) {
                return UM_ERR_HEADER;
            }
        }
        *selected_body_bytes = candidate;
    }
    live_log(context, "calib body selected direction=%s bytes=%zu",
             direction_label(direction), *selected_body_bytes);
    return UM_OK;
}

static int calibration_body_receiver(live_context *context,
                                     unsigned direction,
                                     const um_modem_config *config,
                                     int high_quality,
                                     size_t sender_maximum_body_bytes,
                                     size_t *selected_body_bytes)
{
    um_modem_config bootstrap = live_bootstrap_config();
    unsigned trials = live_body_verification_trials(high_quality);
    uint16_t sequence = 0u;
    size_t candidate_index;
    *selected_body_bytes = LIVE_CALIB_BODY_BASE_BYTES;
    for (candidate_index = 0u;
         candidate_index < sizeof(live_body_candidates) /
                               sizeof(live_body_candidates[0]);
         ++candidate_index) {
        size_t candidate = live_body_candidates[candidate_index];
        unsigned trial;
        if (candidate > sender_maximum_body_bytes) {
            break;
        }
        for (trial = 0u; trial < trials; ++trial, ++sequence) {
            um_live_wire_message probe;
            um_rx_metrics metrics;
            uint8_t expected[UM_LIVE_MAX_BODY];
            uint8_t result[LIVE_CALIB_SIZE_RESULT_BYTES];
            int passed = 0;
            int status;
            memset(&metrics, 0, sizeof(metrics));
            status = receive_wire(context, config, context->session_id,
                                  6000u, &probe, &metrics);
            fill_calibration_body(candidate, trial + 100u, expected,
                                  candidate);
            if (candidate <= context->options.chunk_bytes &&
                status == UM_OK &&
                probe.type == UM_WIRE_CALIB_BODY_PROBE &&
                probe.sequence == sequence &&
                probe.body_length == candidate &&
                memcmp(probe.body, expected, candidate) == 0 &&
                (um_modem_config_uses_robust_gate(config) != 0
                     ? um_modem_metrics_have_baseline_margin(&metrics)
                     : um_modem_metrics_have_margin(config, &metrics)) !=
                    0) {
                passed = 1;
            }
            result[0] = (uint8_t)passed;
            write_u16(&result[1], (uint16_t)candidate);
            live_log(context,
                     "calib body rx direction=%s size=%zu trial=%u/%u %s "
                     "status=%s sync=%.3f snr=%.1fdB evm=%.3f",
                     direction_label(direction), candidate, trial + 1u,
                     trials, passed != 0 ? "PASS" : "FAIL",
                     um_status_string(status), metrics.sync_correlation,
                     metrics.estimated_snr_db, metrics.evm_rms);
            sleep_milliseconds(status == UM_OK ? LIVE_TURNAROUND_MS : 600u);
            status = send_wire(context, &bootstrap,
                               UM_WIRE_CALIB_BODY_RESULT, sequence, result,
                               sizeof(result), 1, NULL);
            if (status != UM_OK) {
                return status;
            }
            if (passed == 0) {
                live_log(context,
                         "calib body size=%zu failed; selected=%zu",
                         candidate, *selected_body_bytes);
                return UM_OK;
            }
        }
        *selected_body_bytes = candidate;
    }
    live_log(context, "calib body selected direction=%s bytes=%zu",
             direction_label(direction), *selected_body_bytes);
    return UM_OK;
}

static float live_candidate_score(const um_modem_config *config,
                                  const um_rx_metrics *metrics)
{
    float raw = um_calibration_payload_rate(config, 128u);
    float quality = 1.0f /
                    (1.0f + 4.0f * metrics->evm_rms * metrics->evm_rms);
    return raw * quality;
}

static int send_calibration_abort(live_context *context,
                                  const um_modem_config *bootstrap,
                                  unsigned direction, size_t candidate,
                                  uint8_t reason)
{
    uint8_t report[4] = {0u, 0u, 0u, reason};
    int status;
    sleep_milliseconds(LIVE_CALIBRATION_SETTLE_MS);
    status = send_wire(context, bootstrap, UM_WIRE_CALIB_REPORT,
                       (uint16_t)direction, report, sizeof(report), 1, NULL);
    if (status != UM_OK) {
        live_log(context,
                 "failed to signal calibration abort at candidate=%zu: %s",
                 candidate + 1u, um_status_string(status));
    }
    return status;
}

static void log_calibration_search_results(
    live_context *context, const um_calibration_search *search)
{
    unsigned repetition_attempts;
    unsigned repetition_passes;
    unsigned qam_attempts;
    unsigned qam_passes;
    unsigned fec_attempts;
    unsigned fec_passes;
    unsigned band_attempts;
    unsigned band_passes;
    unsigned prefix_attempts;
    unsigned prefix_passes;
    unsigned recovery_attempts;
    unsigned recovery_passes;
    const char *qam_state;
    um_calibration_search_step_results(
        search, UM_CALIB_STEP_REPETITIONS, &repetition_attempts,
        &repetition_passes);
    um_calibration_search_step_results(search, UM_CALIB_STEP_QAM,
                                       &qam_attempts, &qam_passes);
    um_calibration_search_step_results(search, UM_CALIB_STEP_FEC,
                                       &fec_attempts, &fec_passes);
    um_calibration_search_step_results(search, UM_CALIB_STEP_HIGH_BAND,
                                       &band_attempts, &band_passes);
    um_calibration_search_step_results(search, UM_CALIB_STEP_PREFIX,
                                       &prefix_attempts, &prefix_passes);
    um_calibration_search_step_results(
        search, UM_CALIB_STEP_MORE_REPETITIONS, &recovery_attempts,
        &recovery_passes);
    qam_state = qam_attempts != 0u && qam_passes == 0u
                    ? "(no-pass; deprioritized)"
                    : "";
    live_log(context,
             "calib search learned pass/try repetitions=%u/%u qam=%u/%u%s "
             "fec=%u/%u high-band=%u/%u prefix=%u/%u "
             "recovery-repeats=%u/%u",
             repetition_passes, repetition_attempts, qam_passes,
             qam_attempts, qam_state,
             fec_passes, fec_attempts, band_passes, band_attempts,
             prefix_passes, prefix_attempts, recovery_passes,
             recovery_attempts);
}

static double live_calibration_primary_seconds(int high_quality)
{
    um_modem_config calibration_control =
        live_calibration_control_config();
    um_calibration_search search;
    float *samples = NULL;
    uint8_t ready_wire[UM_LIVE_WIRE_HEADER_SIZE + LIVE_READY_BYTES] = {0u};
    size_t sample_count = 0u;
    double total_samples = 0.0;
    size_t probes = 0u;
    int next_status;

    if (um_calibration_search_init(&search, high_quality) != UM_OK) {
        return 0.0;
    }
    if (um_modulate_frame(&calibration_control, ready_wire,
                          sizeof(ready_wire), 0u,
                          &samples,
                          &sample_count) != UM_OK) {
        return 0.0;
    }
    {
        size_t ready_samples = sample_count + LIVE_TX_PRE_SAMPLES +
                               LIVE_TX_POST_SAMPLES;
        free(samples);
        samples = NULL;
        while (1) {
            um_modem_config config;
            um_calibration_step step;
            size_t candidate;
            uint8_t probe[UM_LIVE_WIRE_HEADER_SIZE +
                          UM_CALIBRATION_PROBE_BYTES] = {0u};
            next_status = um_calibration_search_next(
                &search, &candidate, &config, &step);
            (void)step;
            if (next_status <= 0) {
                break;
            }
            if (um_modulate_frame(&config, probe, sizeof(probe),
                                  (uint16_t)candidate, &samples,
                                  &sample_count) != UM_OK) {
                free(samples);
                return 0.0;
            }
            total_samples += ready_samples + sample_count +
                             LIVE_TX_PRE_SAMPLES + LIVE_TX_POST_SAMPLES;
            ++probes;
            free(samples);
            samples = NULL;
            if (um_calibration_search_record(&search, candidate, 1) != UM_OK) {
                return 0.0;
            }
        }
    }
    return total_samples / (double)UM_SAMPLE_RATE +
           (double)probes *
               (double)(LIVE_CALIBRATION_SETTLE_MS + LIVE_RECEIVER_ARM_MS) /
               1000.0;
}

static int calibration_sender(live_context *context, unsigned direction,
                              um_modem_config *selected,
                              size_t *selected_body_bytes)
{
    um_modem_config bootstrap = live_bootstrap_config();
    um_modem_config calibration_control =
        live_calibration_control_config();
    size_t probe_budget = um_calibration_search_budget(
        context->options.calibrate_high_quality);
    uint8_t begin[LIVE_CALIB_BEGIN_BYTES];
    um_live_wire_message message;
    size_t probe_number;
    int report_received = 0;
    unsigned verification_trials = live_verification_trials(
        context->options.calibrate_high_quality);
    int status;
    if (probe_budget == 0u || probe_budget > UINT16_MAX) {
        return UM_ERR_CONFIG;
    }
    begin[0] = (uint8_t)direction;
    begin[1] = (uint8_t)(context->options.calibrate_high_quality != 0);
    write_u16(&begin[2], (uint16_t)probe_budget);
    write_u16(&begin[4], (uint16_t)context->options.chunk_bytes);
    live_log(context,
             "state=CALIBRATING direction=%s adaptive-max-probes=%zu",
             direction == 0u ? "client->gateway" : "gateway->client",
             probe_budget);
    sleep_milliseconds(LIVE_TURNAROUND_MS);
    status = send_wire(context, &bootstrap, UM_WIRE_CALIB_BEGIN,
                       (uint16_t)direction, begin, sizeof(begin), 1, NULL);
    if (status != UM_OK) {
        return status;
    }
    for (probe_number = 0u;
         probe_number < probe_budget && !live_interrupted; ++probe_number) {
        um_modem_config config;
        size_t candidate_id;
        um_calibration_step step;
        uint8_t probe[UM_CALIBRATION_PROBE_BYTES];
        float duration = 0.0f;
        status = receive_calibration_ready(
            context, &calibration_control, direction, probe_number, 6000u,
            &message);
        if (status != UM_OK) {
            break;
        }
        if (message.type == UM_WIRE_CALIB_REPORT) {
            report_received = 1;
            break;
        }
        if (message.body_length != LIVE_READY_BYTES ||
            message.body[2] >= (uint8_t)UM_CALIB_STEP_COUNT) {
            return UM_ERR_HEADER;
        }
        candidate_id = read_u16(message.body);
        step = (um_calibration_step)message.body[2];
        status = decode_modem_config(&message.body[3], LIVE_CONFIG_BYTES,
                                     &config);
        if (status != UM_OK) {
            return status;
        }
        /*
         * READY can be decoded before its sender has drained the quiet tail
         * and restarted capture.  Do not consume the probe preamble while the
         * receiver's microphone is still coming back online.
         */
        sleep_milliseconds(LIVE_RECEIVER_ARM_MS);
        fill_calibration_body(candidate_id, 0u, probe, sizeof(probe));
        status = send_wire(context, &config, UM_WIRE_CALIB_PROBE,
                           (uint16_t)probe_number, probe, sizeof(probe), 1,
                           &duration);
        live_log(context,
                 "calib tx probe=%zu/%zu id=%zu step=%s qam=%u fec=%s "
                 "cp=%u window=%u repeats=%u band=%.0f-%.0fHz "
                 "duration=%.3fs",
                 probe_number + 1u, probe_budget, candidate_id,
                 um_calibration_step_name(step), 1u << config.qam_bits,
                 fec_name(config.fec_rate), config.cyclic_prefix,
                 config.window_samples, config.symbol_repetitions,
                 (double)config.first_bin * UM_SAMPLE_RATE / config.fft_size,
                 (double)config.last_bin * UM_SAMPLE_RATE / config.fft_size,
                 duration);
        if (status != UM_OK) {
            break;
        }
    }
    if (live_interrupted) {
        status = UM_ERR_INTERRUPTED;
    }
    if (status != UM_OK) {
        return status;
    }
    if (report_received == 0) {
        status = receive_expected(context, &bootstrap, UM_WIRE_CALIB_REPORT,
                                  (uint16_t)direction, 6000u, &message, NULL);
        if (status != UM_OK) {
            return status;
        }
    }
    if (message.body_length < 4u) {
        return UM_ERR_HEADER;
    }
    {
        size_t usable = read_u16(message.body);
        size_t rank_count = message.body[2];
        size_t rank;
        if (message.body[3] != 0u || rank_count == 0u ||
            rank_count > LIVE_CALIBRATION_RANKS ||
            message.body_length !=
                4u + rank_count * LIVE_REPORT_ENTRY_BYTES) {
            return UM_ERR_HEADER;
        }
        live_log(context,
                 "calib adaptive search probes=%zu usable=%zu ranked=%zu",
                 probe_number, usable, rank_count);
        for (rank = 0u; rank < rank_count; ++rank) {
            size_t offset = 4u + rank * LIVE_REPORT_ENTRY_BYTES;
            size_t selected_candidate = read_u16(&message.body[offset]);
            unsigned trial;
            int verified = 1;
            status = decode_modem_config(&message.body[offset + 2u],
                                         LIVE_CONFIG_BYTES, selected);
            if (status != UM_OK) {
                return status;
            }
            live_log(context,
                     "calib verify rank=%zu id=%zu qam=%u fec=%s cp=%u "
                     "window=%u repeats=%u band=%.0f-%.0fHz",
                     rank + 1u, selected_candidate,
                     1u << selected->qam_bits, fec_name(selected->fec_rate),
                     selected->cyclic_prefix, selected->window_samples,
                     selected->symbol_repetitions,
                     (double)selected->first_bin * UM_SAMPLE_RATE /
                         selected->fft_size,
                     (double)selected->last_bin * UM_SAMPLE_RATE /
                         selected->fft_size);
            for (trial = 0u; trial < verification_trials; ++trial) {
                uint8_t verify[128];
                um_live_wire_message verify_result;
                uint16_t verify_sequence =
                    (uint16_t)(rank * verification_trials + trial);
                sleep_milliseconds(LIVE_TURNAROUND_MS);
                fill_calibration_body(selected_candidate, trial + 1u,
                                      verify, sizeof(verify));
                status = send_wire(context, selected, UM_WIRE_CALIB_VERIFY,
                                   verify_sequence, verify, sizeof(verify), 1,
                                   NULL);
                if (status != UM_OK) {
                    return status;
                }
                status = receive_expected(context, &bootstrap,
                                           UM_WIRE_CALIB_VERIFY_RESULT,
                                           verify_sequence, 4500u,
                                           &verify_result, NULL);
                if (status != UM_OK || verify_result.body_length != 1u) {
                    return status != UM_OK ? status : UM_ERR_HEADER;
                }
                if (verify_result.body[0] == 0u) {
                    verified = 0;
                    live_log(context,
                             "calib verify rank=%zu trial=%u/%u FAIL; "
                             "trying safer ranked candidate",
                             rank + 1u, trial + 1u, verification_trials);
                    break;
                }
                live_log(context,
                         "calib verify rank=%zu trial=%u/%u PASS",
                         rank + 1u, trial + 1u, verification_trials);
            }
            if (verified != 0) {
                status = calibration_body_sender(
                    context, direction, selected,
                    context->options.chunk_bytes, selected_body_bytes);
                if (status != UM_OK) {
                    return status;
                }
                live_log(context, "calib selected rank=%zu id=%zu",
                         rank + 1u, selected_candidate);
                return UM_OK;
            }
        }
    }
    return UM_ERR_CRC;
}

static int calibration_receiver(live_context *context, unsigned direction,
                                const um_live_wire_message *begin_message,
                                um_modem_config *selected,
                                size_t *selected_body_bytes)
{
    um_modem_config bootstrap = live_bootstrap_config();
    um_modem_config calibration_control =
        live_calibration_control_config();
    um_calibration_search search;
    int high_quality;
    unsigned verification_trials;
    size_t probe_budget;
    size_t probe_number = 0u;
    size_t usable = 0u;
    live_ranked_candidate ranked[LIVE_CALIBRATION_RANKS];
    size_t ranked_ids[LIVE_CALIBRATION_RANKS];
    float candidate_scores[UM_CALIBRATION_SEARCH_MAX_NODES];
    size_t rank_count = 0u;
    size_t sender_maximum_body_bytes;
    int status;
    {
        size_t index;
        for (index = 0u; index < UM_CALIBRATION_SEARCH_MAX_NODES; ++index) {
            candidate_scores[index] = -1.0f;
        }
    }
    if (begin_message->body_length != LIVE_CALIB_BEGIN_BYTES ||
        begin_message->body[0] != direction || begin_message->body[1] > 1u) {
        return UM_ERR_HEADER;
    }
    sender_maximum_body_bytes = read_u16(&begin_message->body[4]);
    if (sender_maximum_body_bytes == 0u ||
        sender_maximum_body_bytes > UM_LIVE_MAX_BODY) {
        return UM_ERR_HEADER;
    }
    high_quality = begin_message->body[1] != 0u;
    verification_trials = live_verification_trials(high_quality);
    status = um_calibration_search_init(&search, high_quality);
    if (status != UM_OK) {
        return status;
    }
    probe_budget = search.budget;
    if (read_u16(&begin_message->body[2]) != probe_budget) {
        return UM_ERR_HEADER;
    }
    live_log(context,
             "state=CALIBRATING direction=%s adaptive-max-probes=%zu",
             direction == 0u ? "client->gateway" : "gateway->client",
             probe_budget);
    while (!live_interrupted) {
        um_modem_config config;
        um_calibration_step step;
        size_t candidate_id;
        um_live_wire_message probe;
        um_rx_metrics metrics;
        uint8_t ready[LIVE_READY_BYTES];
        uint8_t expected[UM_CALIBRATION_PROBE_BYTES];
        int next_status = um_calibration_search_next(
            &search, &candidate_id, &config, &step);
        int reliable = 0;
        if (next_status < 0) {
            return next_status;
        }
        if (next_status == 0) {
            break;
        }
        if (candidate_id > UINT16_MAX || probe_number > UINT16_MAX) {
            return UM_ERR_CAPACITY;
        }
        write_u16(ready, (uint16_t)candidate_id);
        ready[2] = (uint8_t)step;
        encode_modem_config(&config, &ready[3]);
        sleep_milliseconds(LIVE_CALIBRATION_SETTLE_MS);
        status = send_wire(context, &calibration_control,
                           UM_WIRE_CALIB_READY, (uint16_t)probe_number,
                           ready, sizeof(ready), 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        memset(&metrics, 0, sizeof(metrics));
        status = receive_wire(context, &config, context->session_id, 4000u,
                              &probe, &metrics);
        if (status == UM_ERR_MEMORY || status == UM_ERR_AUDIO ||
            status == UM_ERR_INTERRUPTED) {
            return status;
        }
        fill_calibration_body(candidate_id, 0u, expected, sizeof(expected));
        if (status == UM_OK && probe.type == UM_WIRE_CALIB_PROBE &&
            probe.sequence == (uint16_t)probe_number &&
            probe.body_length == sizeof(expected) &&
            memcmp(probe.body, expected, sizeof(expected)) == 0) {
            float score = live_candidate_score(&config, &metrics);
            int robust_gate =
                um_modem_config_uses_robust_gate(&config);
            int has_margin =
                robust_gate != 0
                    ? um_modem_metrics_have_baseline_margin(&metrics)
                    : um_modem_metrics_have_margin(&config, &metrics);
            if (has_margin == 0) {
                live_log(context,
                         "calib rx probe=%zu/%zu id=%zu step=%s MARGINAL "
                         "qam=%u fec=%s repeats=%u sync=%.3f snr=%.1fdB "
                         "evm=%.3f; branch stopped",
                         probe_number + 1u, probe_budget, candidate_id,
                         um_calibration_step_name(step),
                         1u << config.qam_bits, fec_name(config.fec_rate),
                         config.symbol_repetitions,
                         metrics.sync_correlation, metrics.estimated_snr_db,
                         metrics.evm_rms);
                status = UM_ERR_RELIABILITY;
            } else {
                reliable = 1;
                ++usable;
                candidate_scores[candidate_id] = score;
                live_log(context,
                         "calib rx probe=%zu/%zu id=%zu step=%s PASS qam=%u "
                         "fec=%s cp=%u window=%u repeats=%u "
                         "band=%.0f-%.0fHz level=%.1fdBFS norm=%.2fx "
                         "clip=%.3f%% snr=%.1fdB evm=%.3f rate=%.0fbps "
                         "score=%.0f gate=%s",
                         probe_number + 1u, probe_budget, candidate_id,
                         um_calibration_step_name(step),
                         1u << config.qam_bits, fec_name(config.fec_rate),
                         config.cyclic_prefix, config.window_samples,
                         config.symbol_repetitions,
                         (double)config.first_bin * UM_SAMPLE_RATE /
                             config.fft_size,
                         (double)config.last_bin * UM_SAMPLE_RATE /
                             config.fft_size,
                         metrics.signal_rms > 1.0e-12f
                             ? 20.0 * log10((double)metrics.signal_rms)
                             : -240.0,
                         metrics.normalization_gain,
                         100.0 * (double)metrics.clipped_sample_fraction,
                         metrics.estimated_snr_db, metrics.evm_rms,
                         um_calibration_payload_rate(&config, 128u), score,
                         robust_gate != 0 ? "crc+sync/snr" : "snr/evm");
            }
        } else {
            if (status == UM_OK) {
                status = UM_ERR_CRC;
            }
            live_log(context,
                     "calib rx probe=%zu/%zu id=%zu step=%s FAIL qam=%u "
                     "fec=%s cp=%u window=%u repeats=%u "
                     "band=%.0f-%.0fHz sync=%.3f level=%.1fdBFS "
                     "norm=%.2fx clip=%.3f%% reason=%s; branch stopped",
                     probe_number + 1u, probe_budget, candidate_id,
                     um_calibration_step_name(step),
                     1u << config.qam_bits, fec_name(config.fec_rate),
                     config.cyclic_prefix, config.window_samples,
                     config.symbol_repetitions,
                     (double)config.first_bin * UM_SAMPLE_RATE /
                         config.fft_size,
                     (double)config.last_bin * UM_SAMPLE_RATE /
                         config.fft_size,
                     metrics.sync_correlation,
                     metrics.signal_rms > 1.0e-12f
                         ? 20.0 * log10((double)metrics.signal_rms)
                         : -240.0,
                     metrics.normalization_gain,
                     100.0 * (double)metrics.clipped_sample_fraction,
                     um_status_string(status));
        }
        next_status = um_calibration_search_record(&search, candidate_id,
                                                   reliable);
        if (next_status != UM_OK) {
            return next_status;
        }
        ++probe_number;
        if (candidate_id == 0u && reliable == 0) {
            live_log(context,
                     "calib two-repeat baseline failed; trying stronger "
                     "physical-repetition recovery");
        }
    }
    if (live_interrupted) {
        return UM_ERR_INTERRUPTED;
    }
    log_calibration_search_results(context, &search);
    rank_count = um_calibration_rank_candidates(
        &search, candidate_scores, ranked_ids, LIVE_CALIBRATION_RANKS);
    if (rank_count == 0u) {
        int abort_status;
        live_log(context,
                 "calib no full-size mode passed through four physical "
                 "repetitions; adaptive search stopped");
        abort_status = send_calibration_abort(context, &bootstrap, direction,
                                              probe_number, 3u);
        return abort_status == UM_OK ? UM_ERR_CRC : abort_status;
    }
    {
        size_t rank;
        for (rank = 0u; rank < rank_count; ++rank) {
            ranked[rank].index = ranked_ids[rank];
            ranked[rank].config = search.nodes[ranked_ids[rank]].config;
            ranked[rank].score = candidate_scores[ranked_ids[rank]];
        }
    }
    {
        uint8_t report[4u + LIVE_CALIBRATION_RANKS *
                                LIVE_REPORT_ENTRY_BYTES];
        size_t rank;
        write_u16(report, (uint16_t)usable);
        report[2] = (uint8_t)rank_count;
        report[3] = 0u;
        for (rank = 0u; rank < rank_count; ++rank) {
            size_t offset = 4u + rank * LIVE_REPORT_ENTRY_BYTES;
            write_u16(&report[offset], (uint16_t)ranked[rank].index);
            encode_modem_config(&ranked[rank].config, &report[offset + 2u]);
        }
        sleep_milliseconds(LIVE_CALIBRATION_SETTLE_MS);
        status = send_wire(context, &bootstrap, UM_WIRE_CALIB_REPORT,
                           (uint16_t)direction, report,
                           4u + rank_count * LIVE_REPORT_ENTRY_BYTES, 1,
                           NULL);
    }
    if (status != UM_OK || rank_count == 0u) {
        return status != UM_OK ? status : UM_ERR_CRC;
    }
    {
        size_t rank;
        for (rank = 0u; rank < rank_count; ++rank) {
            unsigned trial;
            int verified = 1;
            *selected = ranked[rank].config;
            for (trial = 0u; trial < verification_trials; ++trial) {
                um_live_wire_message verify;
                um_rx_metrics verify_metrics;
                uint8_t expected[128];
                uint8_t result_body[1] = {0u};
                uint16_t verify_sequence =
                    (uint16_t)(rank * verification_trials + trial);
                fill_calibration_body(ranked[rank].index, trial + 1u,
                                      expected, sizeof(expected));
                memset(&verify_metrics, 0, sizeof(verify_metrics));
                status = receive_wire(context, selected,
                                      context->session_id, 4000u, &verify,
                                      &verify_metrics);
                if (status == UM_OK &&
                    verify.type == UM_WIRE_CALIB_VERIFY &&
                    verify.sequence == verify_sequence &&
                    verify.body_length == sizeof(expected) &&
                    memcmp(verify.body, expected, sizeof(expected)) == 0 &&
                    (um_modem_config_uses_robust_gate(selected) != 0
                         ? um_modem_metrics_have_baseline_margin(
                               &verify_metrics)
                         : um_modem_metrics_have_margin(selected,
                                                        &verify_metrics)) !=
                        0) {
                    result_body[0] = 1u;
                }
                sleep_milliseconds(status == UM_OK
                                       ? LIVE_TURNAROUND_MS
                                       : 600u);
                {
                    int send_status = send_wire(
                        context, &bootstrap, UM_WIRE_CALIB_VERIFY_RESULT,
                        verify_sequence, result_body, sizeof(result_body), 1,
                        NULL);
                    if (send_status != UM_OK) {
                        return send_status;
                    }
                }
                if (result_body[0] == 0u) {
                    verified = 0;
                    live_log(context,
                             "calib verify rank=%zu trial=%u/%u FAIL "
                             "status=%s sync=%.3f snr=%.1fdB evm=%.3f",
                             rank + 1u, trial + 1u, verification_trials,
                             um_status_string(status),
                             verify_metrics.sync_correlation,
                             verify_metrics.estimated_snr_db,
                             verify_metrics.evm_rms);
                    break;
                }
                live_log(context,
                         "calib verify rank=%zu trial=%u/%u PASS",
                         rank + 1u, trial + 1u, verification_trials);
            }
            if (verified != 0) {
                status = calibration_body_receiver(
                    context, direction, selected, high_quality,
                    sender_maximum_body_bytes, selected_body_bytes);
                if (status != UM_OK) {
                    return status;
                }
                live_log(context,
                         "calib selected rank=%zu id=%zu usable=%zu",
                         rank + 1u, ranked[rank].index, usable);
                return UM_OK;
            }
        }
    }
    return UM_ERR_CRC;
}

static uint8_t test_byte(uint32_t seed, size_t offset)
{
    uint32_t value = seed ^ (uint32_t)offset * UINT32_C(0x9e3779b9);
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    return (uint8_t)value;
}

static int send_test_data(live_context *context,
                          const um_modem_config *config,
                          const um_modem_config *ack_config, size_t total,
                          size_t chunk_size, uint32_t seed, const char *label)
{
    size_t offset = 0u;
    uint16_t sequence = 0u;
    uint64_t started = monotonic_milliseconds();
    while (offset < total && !live_interrupted) {
        size_t chunk = total - offset;
        uint8_t body[UM_LIVE_MAX_BODY];
        unsigned attempt;
        size_t i;
        int acknowledged = 0;
        if (chunk > chunk_size) {
            chunk = chunk_size;
        }
        for (i = 0u; i < chunk; ++i) {
            body[i] = test_byte(seed, offset + i);
        }
        for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
            um_live_wire_message ack;
            int status;
            sleep_milliseconds(LIVE_TURNAROUND_MS);
            status = send_wire(context, config, UM_WIRE_DATA, sequence, body,
                               chunk, 1, NULL);
            if (status != UM_OK) {
                return status;
            }
            status = receive_expected(context, ack_config, UM_WIRE_ACK,
                                      sequence, 1800u, &ack, NULL);
            if (status == UM_OK) {
                acknowledged = 1;
                break;
            }
            live_log(context, "%s DATA seq=%u retry=%u/%u (%s)", label,
                     (unsigned)sequence, attempt + 1u,
                     context->options.retry_limit, um_status_string(status));
        }
        if (acknowledged == 0) {
            return UM_ERR_TIMEOUT;
        }
        offset += chunk;
        ++sequence;
        {
            float seconds =
                (float)(monotonic_milliseconds() - started) / 1000.0f;
            live_log(context,
                     "%s tx bytes=%zu/%zu rate=%.0f bps seq=%u", label,
                     offset, total,
                     seconds > 0.0f ? (float)(offset * 8u) / seconds : 0.0f,
                     (unsigned)(sequence - 1u));
        }
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_OK;
}

static int receive_test_data(live_context *context,
                             const um_modem_config *config,
                             const um_modem_config *ack_config, size_t total,
                             size_t chunk_size, uint32_t seed,
                             const char *label)
{
    size_t offset = 0u;
    uint16_t expected_sequence = 0u;
    unsigned misses = 0u;
    uint64_t started = monotonic_milliseconds();
    while (offset < total && !live_interrupted) {
        um_live_wire_message data;
        um_rx_metrics metrics;
        memset(&metrics, 0, sizeof(metrics));
        int status = receive_wire(context, config, context->session_id, 4000u,
                                  &data, &metrics);
        if (status != UM_OK) {
            if (++misses > context->options.retry_limit + 1u) {
                return status;
            }
            live_log(context,
                     "%s receive miss=%u sync=%.3f level=%.1fdBFS "
                     "norm=%.2fx clip=%.3f%% (%s)",
                     label, misses, metrics.sync_correlation,
                     metrics.signal_rms > 1.0e-12f
                         ? 20.0 * log10((double)metrics.signal_rms)
                         : -240.0,
                     metrics.normalization_gain,
                     100.0 * (double)metrics.clipped_sample_fraction,
                     um_status_string(status));
            continue;
        }
        if (data.type != UM_WIRE_DATA || data.body_length > chunk_size) {
            continue;
        }
        if (data.sequence == expected_sequence) {
            size_t i;
            size_t remaining = total - offset;
            size_t expected_length = remaining < chunk_size ? remaining
                                                            : chunk_size;
            if (data.body_length != expected_length) {
                return UM_ERR_HEADER;
            }
            for (i = 0u; i < data.body_length; ++i) {
                if (data.body[i] != test_byte(seed, offset + i)) {
                    return UM_ERR_CRC;
                }
            }
            offset += data.body_length;
            ++expected_sequence;
            misses = 0u;
        } else if (expected_sequence == 0u ||
                   data.sequence != (uint16_t)(expected_sequence - 1u)) {
            continue;
        }
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        status = send_wire(context, ack_config, UM_WIRE_ACK, data.sequence,
                           NULL, 0u, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        {
            float seconds =
                (float)(monotonic_milliseconds() - started) / 1000.0f;
            live_log(context,
                     "%s rx bytes=%zu/%zu rate=%.0f bps seq=%u snr=%.1fdB "
                     "evm=%.3f sync=%.3f level=%.1fdBFS norm=%.2fx "
                     "clip=%.3f%%",
                     label,
                     offset, total,
                     seconds > 0.0f ? (float)(offset * 8u) / seconds : 0.0f,
                     (unsigned)data.sequence, metrics.estimated_snr_db,
                     metrics.evm_rms, metrics.sync_correlation,
                     metrics.signal_rms > 1.0e-12f
                         ? 20.0 * log10((double)metrics.signal_rms)
                         : -240.0,
                     metrics.normalization_gain,
                     100.0 * (double)metrics.clipped_sample_fraction);
        }
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_OK;
}

static int receive_control(live_context *context, um_live_wire_type type,
                           uint16_t sequence, unsigned timeout_ms,
                           um_live_wire_message *message)
{
    um_modem_config bootstrap = live_bootstrap_config();
    return receive_expected(context, &bootstrap, type, sequence, timeout_ms,
                            message, NULL);
}

static int send_control(live_context *context, um_live_wire_type type,
                        uint16_t sequence, const uint8_t *body,
                        size_t body_length)
{
    um_modem_config bootstrap = live_bootstrap_config();
    sleep_milliseconds(LIVE_TURNAROUND_MS);
    return send_wire(context, &bootstrap, type, sequence, body, body_length, 1,
                     NULL);
}

static const char *direction_label(unsigned direction)
{
    return direction == 0u ? "client->gateway" : "gateway->client";
}

static void log_cached_config(live_context *context, const char *prefix,
                              unsigned direction,
                              const um_modem_config *config,
                              size_t frame_body_bytes)
{
    live_log(context,
             "%s direction=%s qam=%u fec=%s cp=%u window=%u repeats=%u "
             "training=%u sync=%.1fms band=%.0f-%.0fHz body=%zu",
             prefix, direction_label(direction), 1u << config->qam_bits,
             fec_name(config->fec_rate), config->cyclic_prefix,
             config->window_samples, config->symbol_repetitions,
             config->training_symbols,
             1000.0 * (double)config->sync_samples / UM_SAMPLE_RATE,
             (double)config->first_bin * UM_SAMPLE_RATE / config->fft_size,
             (double)config->last_bin * UM_SAMPLE_RATE / config->fft_size,
             frame_body_bytes);
}

static void load_local_calibration(live_context *context)
{
    const char *path = context->options.calibration_path;
    um_modem_config config;
    size_t frame_body_bytes = 0u;
    int found = 0;
    int status;
    unsigned direction = context->options.role == UM_LIVE_GATEWAY ? 0u : 1u;
    if (path == NULL || *path == '\0') {
        live_log(context,
                 "Calibration cache disabled; local receive direction will "
                 "calibrate");
        return;
    }
    status = um_calibration_config_load(path, context->options.role, &config,
                                        &frame_body_bytes, &found);
    if (status != UM_OK) {
        live_log(context,
                 "Calibration cache '%s' is invalid or unreadable; ignoring "
                 "it and recalibrating",
                 path);
        return;
    }
    if (found == 0) {
        live_log(context,
                 "Calibration cache '%s' not found; local receive direction "
                 "will calibrate",
                 path);
        return;
    }
    if (context->options.chunk_bytes >= LIVE_CALIB_BODY_BASE_BYTES &&
        frame_body_bytes > context->options.chunk_bytes) {
        live_log(context,
                 "Capping cached frame body from %zu to CLI maximum %zu",
                 frame_body_bytes, context->options.chunk_bytes);
        frame_body_bytes = context->options.chunk_bytes;
    }
    if (direction == 0u) {
        context->client_to_gateway = config;
        context->client_to_gateway_body_bytes = frame_body_bytes;
        context->have_client_to_gateway = 1;
    } else {
        context->gateway_to_client = config;
        context->gateway_to_client_body_bytes = frame_body_bytes;
        context->have_gateway_to_client = 1;
    }
    log_cached_config(context, "Loaded calibration cache", direction,
                      &config, frame_body_bytes);
}

static void save_local_calibration(live_context *context, unsigned direction,
                                   const um_modem_config *config,
                                   size_t frame_body_bytes)
{
    const char *path = context->options.calibration_path;
    int status;
    if (direction == 0u) {
        context->client_to_gateway = *config;
        context->client_to_gateway_body_bytes = frame_body_bytes;
        context->have_client_to_gateway = 1;
    } else {
        context->gateway_to_client = *config;
        context->gateway_to_client_body_bytes = frame_body_bytes;
        context->have_gateway_to_client = 1;
    }
    if (path == NULL || *path == '\0') {
        return;
    }
    status = um_calibration_config_save(path, context->options.role, config,
                                        frame_body_bytes);
    if (status != UM_OK) {
        live_log(context,
                 "Could not write calibration cache '%s' (%s); continuing "
                 "with the measured mode for this run",
                 path, um_status_string(status));
        return;
    }
    log_cached_config(context, "Saved calibration cache", direction, config,
                      frame_body_bytes);
}

static int exchange_calibration_caches(live_context *context)
{
    uint8_t body[LIVE_CACHE_BODY_BYTES];
    um_live_wire_message message;
    size_t body_length;
    int status;
    if (context->options.role == UM_LIVE_CLIENT) {
        body_length = encode_calibration_cache(
            context->have_gateway_to_client,
            &context->gateway_to_client,
            context->gateway_to_client_body_bytes, body);
        status = send_control(context, UM_WIRE_CALIB_CACHE, 1u, body,
                              body_length);
        if (status != UM_OK) {
            return status;
        }
        status = receive_control(context, UM_WIRE_CALIB_CACHE, 0u, 3500u,
                                 &message);
        if (status != UM_OK) {
            return status;
        }
        status = decode_calibration_cache(
            &message, &context->have_client_to_gateway,
            &context->client_to_gateway,
            &context->client_to_gateway_body_bytes);
    } else {
        status = receive_control(context, UM_WIRE_CALIB_CACHE, 1u, 3500u,
                                 &message);
        if (status != UM_OK) {
            return status;
        }
        status = decode_calibration_cache(
            &message, &context->have_gateway_to_client,
            &context->gateway_to_client,
            &context->gateway_to_client_body_bytes);
        if (status != UM_OK) {
            return status;
        }
        body_length = encode_calibration_cache(
            context->have_client_to_gateway,
            &context->client_to_gateway,
            context->client_to_gateway_body_bytes, body);
        status = send_control(context, UM_WIRE_CALIB_CACHE, 0u, body,
                              body_length);
    }
    if (status != UM_OK) {
        return status;
    }
    live_log(context,
             "Calibration cache exchange client->gateway=%s "
             "gateway->client=%s",
             context->have_client_to_gateway != 0 ? "cached" : "calibrate",
             context->have_gateway_to_client != 0 ? "cached" : "calibrate");
    return UM_OK;
}

static const um_modem_config *proxy_transmit_config(
    const live_context *context)
{
    return context->options.role == UM_LIVE_CLIENT
               ? &context->client_to_gateway
               : &context->gateway_to_client;
}

static const um_modem_config *proxy_receive_config(
    const live_context *context)
{
    return context->options.role == UM_LIVE_CLIENT
               ? &context->gateway_to_client
               : &context->client_to_gateway;
}

static const char *proxy_transmit_label(const live_context *context)
{
    return context->options.role == UM_LIVE_CLIENT ? "client->gateway"
                                                   : "gateway->client";
}

static const char *proxy_receive_label(const live_context *context)
{
    return context->options.role == UM_LIVE_CLIENT ? "gateway->client"
                                                   : "client->gateway";
}

static int validate_ip_packet(const uint8_t *packet, size_t length)
{
    unsigned version;
    if (packet == NULL || length == 0u || length > UM_NETWORK_MAX_PACKET) {
        return UM_ERR_HEADER;
    }
    version = packet[0] >> 4u;
    if (version == 4u) {
        size_t header_length;
        if (length < 20u) {
            return UM_ERR_HEADER;
        }
        header_length = (size_t)(packet[0] & 0x0fu) * 4u;
        if (header_length < 20u || header_length > length ||
            read_u16(&packet[2]) != length) {
            return UM_ERR_HEADER;
        }
        return UM_OK;
    }
    if (version == 6u) {
        if (length < 40u || (size_t)read_u16(&packet[4]) + 40u != length) {
            return UM_ERR_HEADER;
        }
        return UM_OK;
    }
    return UM_ERR_HEADER;
}

static int proxy_packet_is_dns_traffic(const uint8_t *packet, size_t length)
{
    size_t transport_offset;
    unsigned protocol;
    if (validate_ip_packet(packet, length) != UM_OK) {
        return 0;
    }
    if ((packet[0] >> 4u) == 4u) {
        transport_offset = (size_t)(packet[0] & 0x0fu) * 4u;
        protocol = packet[9];
        if ((read_u16(&packet[6]) & UINT16_C(0x1fff)) != 0u) {
            return 0;
        }
    } else {
        transport_offset = 40u;
        protocol = packet[6];
    }
    return (protocol == 6u || protocol == 17u) &&
           transport_offset + 4u <= length &&
           (read_u16(&packet[transport_offset]) == 53u ||
            read_u16(&packet[transport_offset + 2u]) == 53u);
}

static int proxy_packet_is_tunnel_broadcast(const uint8_t *packet,
                                            size_t length)
{
    static const uint8_t subnet_broadcast[4] = {10u, 77u, 0u, 3u};
    static const uint8_t limited_broadcast[4] = {255u, 255u, 255u, 255u};
    return validate_ip_packet(packet, length) == UM_OK &&
           (packet[0] >> 4u) == 4u &&
           (memcmp(&packet[16], subnet_broadcast,
                   sizeof(subnet_broadcast)) == 0 ||
            memcmp(&packet[16], limited_broadcast,
                   sizeof(limited_broadcast)) == 0);
}

static int proxy_packet_is_stale_dns_icmp(const uint8_t *packet,
                                          size_t length)
{
    size_t header_length;
    size_t quoted_offset;
    size_t quoted_header_length;
    if (validate_ip_packet(packet, length) != UM_OK ||
        (packet[0] >> 4u) != 4u || packet[9] != 1u) {
        return 0;
    }
    header_length = (size_t)(packet[0] & 0x0fu) * 4u;
    quoted_offset = header_length + 8u;
    if (quoted_offset + 28u > length || packet[header_length] != 3u ||
        packet[header_length + 1u] != 3u ||
        (packet[quoted_offset] >> 4u) != 4u ||
        packet[quoted_offset + 9u] != 17u) {
        return 0;
    }
    quoted_header_length =
        (size_t)(packet[quoted_offset] & 0x0fu) * 4u;
    return quoted_header_length >= 20u &&
           quoted_offset + quoted_header_length + 8u <= length &&
           read_u16(&packet[quoted_offset + quoted_header_length]) == 53u;
}

static int tcp_ack_options_are_replaceable(const uint8_t *tcp,
                                           size_t header_length)
{
    size_t offset = 20u;
    while (offset < header_length) {
        unsigned kind = tcp[offset];
        size_t option_length;
        if (kind == 0u) {
            return 1;
        }
        if (kind == 1u) {
            ++offset;
            continue;
        }
        if (offset + 2u > header_length) {
            return 0;
        }
        option_length = tcp[offset + 1u];
        if (option_length < 2u || option_length > header_length - offset ||
            kind != 8u || option_length != 10u) {
            return 0;
        }
        offset += option_length;
    }
    return 1;
}

static int tcp_ack_supersedes(const uint8_t *new_packet, size_t new_length,
                              const uint8_t *old_packet, size_t old_length)
{
    const uint8_t *new_tcp;
    const uint8_t *old_tcp;
    size_t new_tcp_header;
    size_t old_tcp_header;
    uint32_t acknowledgement_delta;
    if (validate_ip_packet(new_packet, new_length) != UM_OK ||
        validate_ip_packet(old_packet, old_length) != UM_OK ||
        (new_packet[0] >> 4u) != 4u ||
        (old_packet[0] >> 4u) != 4u ||
        (new_packet[0] & 0x0fu) != 5u ||
        (old_packet[0] & 0x0fu) != 5u || new_packet[9] != 6u ||
        old_packet[9] != 6u ||
        (read_u16(&new_packet[6]) & UINT16_C(0x3fff)) != 0u ||
        (read_u16(&old_packet[6]) & UINT16_C(0x3fff)) != 0u ||
        memcmp(&new_packet[12], &old_packet[12], 8u) != 0) {
        return 0;
    }
    new_tcp = &new_packet[20];
    old_tcp = &old_packet[20];
    if (new_length < 40u || old_length < 40u ||
        memcmp(new_tcp, old_tcp, 8u) != 0 ||
        new_tcp[13] != 0x10u || old_tcp[13] != 0x10u) {
        return 0;
    }
    new_tcp_header = (size_t)(new_tcp[12] >> 4u) * 4u;
    old_tcp_header = (size_t)(old_tcp[12] >> 4u) * 4u;
    if (new_tcp_header < 20u || old_tcp_header < 20u ||
        20u + new_tcp_header != new_length ||
        20u + old_tcp_header != old_length ||
        tcp_ack_options_are_replaceable(new_tcp, new_tcp_header) == 0 ||
        tcp_ack_options_are_replaceable(old_tcp, old_tcp_header) == 0) {
        return 0;
    }
    acknowledgement_delta = read_u32(&new_tcp[8]) - read_u32(&old_tcp[8]);
    return acknowledgement_delta <= UINT32_C(0x7fffffff);
}

static unsigned proxy_packet_priority(const uint8_t *packet, size_t length)
{
    size_t transport_offset;
    unsigned protocol;
    unsigned version;
    uint16_t source_port = 0u;
    uint16_t destination_port = 0u;

    if (validate_ip_packet(packet, length) != UM_OK) {
        return LIVE_PROXY_PRIORITY_BULK;
    }
    version = packet[0] >> 4u;
    if (version == 4u) {
        transport_offset = (size_t)(packet[0] & 0x0fu) * 4u;
        protocol = packet[9];
        if ((read_u16(&packet[6]) & UINT16_C(0x1fff)) != 0u) {
            return LIVE_PROXY_PRIORITY_NORMAL;
        }
    } else {
        transport_offset = 40u;
        protocol = packet[6];
    }
    if ((protocol == 6u || protocol == 17u) &&
        transport_offset + 4u <= length) {
        source_port = read_u16(&packet[transport_offset]);
        destination_port = read_u16(&packet[transport_offset + 2u]);
        if (source_port == 53u || destination_port == 53u) {
            return proxy_packet_is_tunnel_discovery_dns(packet, length) != 0
                       ? LIVE_PROXY_PRIORITY_NORMAL
                       : LIVE_PROXY_PRIORITY_DNS;
        }
    }
    if (protocol == 1u || protocol == 58u) {
        return LIVE_PROXY_PRIORITY_CONTROL;
    }
    if (protocol == 6u && transport_offset + 20u <= length) {
        size_t tcp_header = (size_t)(packet[transport_offset + 12u] >> 4u) *
                            4u;
        unsigned flags = packet[transport_offset + 13u];
        if ((flags & 0x07u) != 0u ||
            (tcp_header >= 20u && transport_offset + tcp_header == length)) {
            return LIVE_PROXY_PRIORITY_CONTROL;
        }
        return LIVE_PROXY_PRIORITY_NORMAL;
    }
    return LIVE_PROXY_PRIORITY_BULK;
}

static const char *dns_record_type(uint16_t type, char text[16])
{
    switch (type) {
    case 1u:
        return "A";
    case 5u:
        return "CNAME";
    case 12u:
        return "PTR";
    case 16u:
        return "TXT";
    case 28u:
        return "AAAA";
    case 64u:
        return "SVCB";
    case 65u:
        return "HTTPS";
    default:
        (void)snprintf(text, 16u, "TYPE%u", (unsigned)type);
        return text;
    }
}

static int read_dns_name(const uint8_t *dns, size_t dns_length,
                         size_t start, char *name, size_t name_capacity,
                         size_t *next)
{
    size_t position = start;
    size_t output = 0u;
    size_t jump_count = 0u;
    int jumped = 0;
    if (name == NULL || name_capacity == 0u || next == NULL) {
        return 0;
    }
    while (position < dns_length && jump_count <= 16u) {
        size_t label_length = dns[position];
        if ((label_length & 0xc0u) == 0xc0u) {
            size_t target;
            if (position + 1u >= dns_length) {
                return 0;
            }
            target = ((label_length & 0x3fu) << 8u) | dns[position + 1u];
            if (target >= dns_length) {
                return 0;
            }
            if (jumped == 0) {
                *next = position + 2u;
            }
            position = target;
            jumped = 1;
            ++jump_count;
            continue;
        }
        if (label_length == 0u) {
            if (jumped == 0) {
                *next = position + 1u;
            }
            if (output == 0u) {
                if (name_capacity < 2u) {
                    return 0;
                }
                name[output++] = '.';
            }
            name[output] = '\0';
            return 1;
        }
        if (label_length > 63u ||
            position + 1u + label_length > dns_length) {
            return 0;
        }
        if (output != 0u) {
            if (output + 1u >= name_capacity) {
                return 0;
            }
            name[output++] = '.';
        }
        ++position;
        while (label_length-- != 0u) {
            unsigned char character = dns[position++];
            if (output + 1u >= name_capacity) {
                return 0;
            }
            name[output++] =
                (char)(isalnum(character) != 0 || character == '-' ||
                               character == '_'
                           ? character
                           : '?');
        }
        if (jumped == 0) {
            *next = position;
        }
    }
    return 0;
}

static int text_has_case_insensitive_suffix(const char *text,
                                            const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    size_t index;
    if (suffix_length > text_length) {
        return 0;
    }
    text += text_length - suffix_length;
    for (index = 0u; index < suffix_length; ++index) {
        if (tolower((unsigned char)text[index]) !=
            tolower((unsigned char)suffix[index])) {
            return 0;
        }
    }
    return 1;
}

static int proxy_packet_is_tunnel_discovery_dns(const uint8_t *packet,
                                                size_t length)
{
    const uint8_t *dns;
    size_t header_length;
    size_t dns_length;
    size_t question_end = 0u;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t udp_length;
    char name[96];
    if (validate_ip_packet(packet, length) != UM_OK ||
        (packet[0] >> 4u) != 4u || packet[9] != 17u ||
        (read_u16(&packet[6]) & UINT16_C(0x3fff)) != 0u) {
        return 0;
    }
    header_length = (size_t)(packet[0] & 0x0fu) * 4u;
    if (header_length + 20u > length) {
        return 0;
    }
    source_port = read_u16(&packet[header_length]);
    destination_port = read_u16(&packet[header_length + 2u]);
    udp_length = read_u16(&packet[header_length + 4u]);
    if ((source_port != 53u && destination_port != 53u) ||
        udp_length != length - header_length || udp_length < 20u) {
        return 0;
    }
    dns = &packet[header_length + 8u];
    dns_length = udp_length - 8u;
    if (read_u16(&dns[4]) == 0u ||
        read_dns_name(dns, dns_length, 12u, name, sizeof(name),
                      &question_end) == 0 ||
        question_end + 4u > dns_length ||
        read_u16(&dns[question_end]) != 12u) {
        return 0;
    }
    return text_has_case_insensitive_suffix(
        name, ".0.77.10.in-addr.arpa");
}

static uint32_t dns_name_hash(const char *name)
{
    uint32_t hash = UINT32_C(2166136261);
    while (*name != '\0') {
        unsigned char character = (unsigned char)*name++;
        hash ^= (uint32_t)tolower(character);
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static int dns_packet_key(const uint8_t *packet, size_t length,
                          int response, live_proxy_dns_key *key)
{
    const uint8_t *dns;
    size_t header_length;
    size_t dns_length;
    size_t question_end = 0u;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t udp_length;
    uint16_t flags;
    char name[96];

    if (key == NULL || validate_ip_packet(packet, length) != UM_OK ||
        (packet[0] >> 4u) != 4u || packet[9] != 17u ||
        (read_u16(&packet[6]) & UINT16_C(0x3fff)) != 0u) {
        return 0;
    }
    header_length = (size_t)(packet[0] & 0x0fu) * 4u;
    if (header_length + 20u > length) {
        return 0;
    }
    source_port = read_u16(&packet[header_length]);
    destination_port = read_u16(&packet[header_length + 2u]);
    udp_length = read_u16(&packet[header_length + 4u]);
    if (udp_length != length - header_length || udp_length < 20u) {
        return 0;
    }
    dns = &packet[header_length + 8u];
    dns_length = udp_length - 8u;
    flags = read_u16(&dns[2]);
    if (((flags & UINT16_C(0x8000)) != 0u) != (response != 0) ||
        read_u16(&dns[4]) == 0u ||
        read_dns_name(dns, dns_length, 12u, name, sizeof(name),
                      &question_end) == 0 ||
        question_end + 4u > dns_length) {
        return 0;
    }
    if ((response == 0 && destination_port != 53u) ||
        (response != 0 && source_port != 53u)) {
        return 0;
    }
    key->client_address = read_u32(&packet[response != 0 ? 16u : 12u]);
    key->question_hash = dns_name_hash(name);
    key->client_port = response != 0 ? destination_port : source_port;
    key->transaction_id = read_u16(dns);
    key->question_type = read_u16(&dns[question_end]);
    key->question_class = read_u16(&dns[question_end + 2u]);
    return 1;
}

static int dns_keys_equal(const live_proxy_dns_key *left,
                          const live_proxy_dns_key *right)
{
    /* A resolver may retry one logical transaction against another server.
     * A response from either server completes that transaction, so the
     * server address is deliberately not part of the comparison. */
    return left->client_address == right->client_address &&
           left->question_hash == right->question_hash &&
           left->client_port == right->client_port &&
           left->transaction_id == right->transaction_id &&
           left->question_type == right->question_type &&
           left->question_class == right->question_class;
}

static int describe_dns(const uint8_t *packet, size_t length,
                        size_t transport_offset, char *description,
                        size_t description_capacity)
{
    const uint8_t *dns;
    size_t dns_length;
    size_t question_end = 0u;
    uint16_t udp_length;
    uint16_t flags;
    uint16_t type;
    char name[96];
    char type_text[16];
    if (transport_offset + 8u > length) {
        return 0;
    }
    udp_length = read_u16(&packet[transport_offset + 4u]);
    if (udp_length < 20u || transport_offset + udp_length > length) {
        return 0;
    }
    dns = &packet[transport_offset + 8u];
    dns_length = udp_length - 8u;
    if (read_u16(&dns[4]) == 0u ||
        read_dns_name(dns, dns_length, 12u, name, sizeof(name),
                      &question_end) == 0 ||
        question_end + 4u > dns_length) {
        return 0;
    }
    flags = read_u16(&dns[2]);
    type = read_u16(&dns[question_end]);
    if ((flags & UINT16_C(0x8000)) == 0u) {
        (void)snprintf(description, description_capacity,
                       "query %s %s id=%u", name,
                       dns_record_type(type, type_text),
                       (unsigned)read_u16(dns));
    } else {
        (void)snprintf(description, description_capacity,
                       "response %s %s id=%u rcode=%u answers=%u", name,
                       dns_record_type(type, type_text),
                       (unsigned)read_u16(dns), (unsigned)(flags & 0x0fu),
                       (unsigned)read_u16(&dns[6]));
    }
    return 1;
}

static void describe_icmp_packet(const uint8_t *packet, size_t length,
                                 size_t transport_offset, unsigned version,
                                 const char *family, char *description,
                                 size_t description_capacity)
{
    unsigned type;
    unsigned code;
    if (transport_offset + 2u > length) {
        (void)snprintf(description, description_capacity, "%s/ICMP", family);
        return;
    }
    type = packet[transport_offset];
    code = packet[transport_offset + 1u];
    if (version == 4u) {
        const uint8_t *quoted;
        size_t quoted_length;
        size_t quoted_header_length;
        unsigned quoted_protocol;
        if (type == 3u && transport_offset + 8u + 20u <= length) {
            quoted = &packet[transport_offset + 8u];
            quoted_length = length - transport_offset - 8u;
            quoted_header_length = (size_t)(quoted[0] & 0x0fu) * 4u;
            quoted_protocol = quoted[9];
            if ((quoted[0] >> 4u) == 4u &&
                quoted_header_length >= 20u &&
                quoted_header_length + 4u <= quoted_length &&
                (quoted_protocol == 6u || quoted_protocol == 17u)) {
                (void)snprintf(
                    description, description_capacity,
                    "%s/ICMP %u.%u.%u.%u->%u.%u.%u.%u type=%u code=%u "
                    "quoted=%s %u.%u.%u.%u:%u->%u.%u.%u.%u:%u",
                    family, packet[12], packet[13], packet[14], packet[15],
                    packet[16], packet[17], packet[18], packet[19], type,
                    code, quoted_protocol == 6u ? "TCP" : "UDP", quoted[12],
                    quoted[13], quoted[14], quoted[15],
                    (unsigned)read_u16(&quoted[quoted_header_length]),
                    quoted[16], quoted[17], quoted[18], quoted[19],
                    (unsigned)read_u16(&quoted[quoted_header_length + 2u]));
                return;
            }
        }
        (void)snprintf(description, description_capacity,
                       "%s/ICMP %u.%u.%u.%u->%u.%u.%u.%u type=%u code=%u",
                       family, packet[12], packet[13], packet[14], packet[15],
                       packet[16], packet[17], packet[18], packet[19], type,
                       code);
        return;
    }
    (void)snprintf(description, description_capacity,
                   "%s/ICMP type=%u code=%u", family, type, code);
}

static void describe_proxy_packet(const uint8_t *packet, size_t length,
                                  char *description,
                                  size_t description_capacity)
{
    size_t transport_offset = 0u;
    unsigned protocol = 0u;
    unsigned version = length != 0u ? packet[0] >> 4u : 0u;
    const char *family = version == 4u ? "IPv4" : version == 6u ? "IPv6" :
                                                                  "IP";
    if (version == 4u && length >= 20u) {
        transport_offset = (size_t)(packet[0] & 0x0fu) * 4u;
        protocol = packet[9];
    } else if (version == 6u && length >= 40u) {
        transport_offset = 40u;
        protocol = packet[6];
    }
    if ((protocol == 6u || protocol == 17u) &&
        transport_offset + 4u <= length) {
        char dns_description[160];
        int is_dns = proxy_packet_priority(packet, length) ==
                     LIVE_PROXY_PRIORITY_DNS;
        int have_dns_description =
            is_dns != 0 && protocol == 17u &&
            describe_dns(packet, length, transport_offset,
                         dns_description, sizeof(dns_description)) != 0;
        if (version == 4u) {
            (void)snprintf(
                description, description_capacity,
                "%s/%s %u.%u.%u.%u:%u->%u.%u.%u.%u:%u%s%s%s",
                family, protocol == 6u ? "TCP" : "UDP",
                packet[12], packet[13], packet[14], packet[15],
                (unsigned)read_u16(&packet[transport_offset]), packet[16],
                packet[17], packet[18], packet[19],
                (unsigned)read_u16(&packet[transport_offset + 2u]),
                is_dns != 0 ? " DNS" : "",
                have_dns_description != 0 ? " " : "",
                have_dns_description != 0 ? dns_description : "");
        } else {
            (void)snprintf(
                description, description_capacity, "%s/%s %u->%u%s%s%s",
                family, protocol == 6u ? "TCP" : "UDP",
                (unsigned)read_u16(&packet[transport_offset]),
                (unsigned)read_u16(&packet[transport_offset + 2u]),
                is_dns != 0 ? " DNS" : "",
                have_dns_description != 0 ? " " : "",
                have_dns_description != 0 ? dns_description : "");
        }
    } else if (protocol == 1u || protocol == 58u) {
        describe_icmp_packet(packet, length, transport_offset, version,
                             family, description, description_capacity);
    } else {
        (void)snprintf(description, description_capacity, "%s/proto-%u",
                       family, protocol);
    }
}

static void remove_proxy_queue_entry(live_proxy_state *state, size_t index)
{
    if (index + 1u < state->queue_count) {
        memmove(&state->queue[index], &state->queue[index + 1u],
                (state->queue_count - index - 1u) * sizeof(state->queue[0]));
    }
    --state->queue_count;
}

static int proxy_queue_has_non_dns(const live_proxy_state *state)
{
    size_t index;
    for (index = 0u; index < state->queue_count; ++index) {
        if (state->queue[index].priority != LIVE_PROXY_PRIORITY_DNS) {
            return 1;
        }
    }
    return 0;
}

static int proxy_packet_is_dns_response(const uint8_t *packet,
                                        size_t packet_length)
{
    live_proxy_dns_key key;
    return dns_packet_key(packet, packet_length, 1, &key);
}

static int proxy_queue_has_dns_response(const live_proxy_state *state)
{
    size_t index;
    for (index = 0u; index < state->queue_count; ++index) {
        if (proxy_packet_is_dns_response(state->queue[index].packet,
                                         state->queue[index].length) != 0) {
            return 1;
        }
    }
    return 0;
}

static int select_proxy_queue_index(const live_proxy_state *state,
                                    int allow_dns, int require_fit,
                                    size_t maximum_packet_bytes,
                                    size_t *selected)
{
    size_t index;
    int found = 0;
    for (index = 0u; index < state->queue_count; ++index) {
        const live_proxy_queued_packet *queued = &state->queue[index];
        if ((allow_dns == 0 &&
             queued->priority == LIVE_PROXY_PRIORITY_DNS) ||
            (require_fit != 0 &&
             queued->length > maximum_packet_bytes)) {
            continue;
        }
        if (found == 0 ||
            queued->priority > state->queue[*selected].priority) {
            *selected = index;
            found = 1;
        }
    }
    return found;
}

static int dns_query_recently_completed(live_proxy_state *state,
                                        const uint8_t *packet,
                                        size_t packet_length)
{
    live_proxy_dns_key key;
    uint64_t now;
    size_t index;
    if (dns_packet_key(packet, packet_length, 0, &key) == 0) {
        return 0;
    }
    now = monotonic_milliseconds();
    for (index = 0u; index < LIVE_PROXY_RECENT_DNS; ++index) {
        live_proxy_recent_dns *recent = &state->recent_dns[index];
        if (recent->valid == 0) {
            continue;
        }
        if (now - recent->completed_ms >
            LIVE_PROXY_DNS_RETRY_SUPPRESS_MS) {
            recent->valid = 0;
            continue;
        }
        if (dns_keys_equal(&recent->key, &key) != 0) {
            return 1;
        }
    }
    return 0;
}

static int dns_query_is_inflight(live_proxy_state *state,
                                 const uint8_t *packet,
                                 size_t packet_length)
{
    live_proxy_dns_key key;
    uint64_t now;
    size_t index;
    if (dns_packet_key(packet, packet_length, 0, &key) == 0) {
        return 0;
    }
    /* Suppressed retries do not refresh sent_ms. A query accepted by the
     * peer therefore gets a bounded quiet period, then one fresh upstream
     * attempt is allowed if no response ever returned. */
    now = monotonic_milliseconds();
    for (index = 0u; index < LIVE_PROXY_INFLIGHT_DNS; ++index) {
        live_proxy_inflight_dns *inflight = &state->inflight_dns[index];
        if (inflight->valid == 0) {
            continue;
        }
        if (now - inflight->sent_ms >
            LIVE_PROXY_DNS_INFLIGHT_SUPPRESS_MS) {
            inflight->valid = 0;
            continue;
        }
        if (dns_keys_equal(&inflight->key, &key) != 0) {
            return 1;
        }
    }
    return 0;
}

static int dns_query_is_queued(const live_proxy_state *state,
                               const uint8_t *packet,
                               size_t packet_length)
{
    live_proxy_dns_key key;
    size_t index;
    if (dns_packet_key(packet, packet_length, 0, &key) == 0) {
        return 0;
    }
    for (index = 0u; index < state->queue_count; ++index) {
        live_proxy_dns_key queued;
        if (dns_packet_key(state->queue[index].packet,
                           state->queue[index].length, 0, &queued) != 0 &&
            dns_keys_equal(&queued, &key) != 0) {
            return 1;
        }
    }
    return 0;
}

static void remember_inflight_dns(live_proxy_state *state,
                                  const uint8_t *packet,
                                  size_t packet_length)
{
    live_proxy_dns_key key;
    uint64_t now;
    uint64_t oldest = UINT64_MAX;
    size_t oldest_slot = 0u;
    size_t slot = LIVE_PROXY_INFLIGHT_DNS;
    size_t index;
    if (dns_packet_key(packet, packet_length, 0, &key) == 0) {
        return;
    }
    now = monotonic_milliseconds();
    for (index = 0u; index < LIVE_PROXY_INFLIGHT_DNS; ++index) {
        live_proxy_inflight_dns *inflight = &state->inflight_dns[index];
        if (inflight->valid != 0 &&
            dns_keys_equal(&inflight->key, &key) != 0) {
            slot = index;
            break;
        }
        if (inflight->valid == 0 && slot == LIVE_PROXY_INFLIGHT_DNS) {
            slot = index;
        } else if (inflight->valid != 0 && inflight->sent_ms < oldest) {
            oldest = inflight->sent_ms;
            oldest_slot = index;
        }
    }
    if (slot == LIVE_PROXY_INFLIGHT_DNS) {
        slot = oldest_slot;
    }
    state->inflight_dns[slot].key = key;
    state->inflight_dns[slot].sent_ms = now;
    state->inflight_dns[slot].valid = 1;
}

static void remember_completed_dns(live_proxy_state *state,
                                   const uint8_t *packet,
                                   size_t packet_length)
{
    live_proxy_dns_key key;
    uint64_t now;
    uint64_t oldest = UINT64_MAX;
    size_t oldest_slot = 0u;
    size_t slot = LIVE_PROXY_RECENT_DNS;
    size_t index;
    if (dns_packet_key(packet, packet_length, 1, &key) == 0) {
        return;
    }
    now = monotonic_milliseconds();
    for (index = 0u; index < LIVE_PROXY_INFLIGHT_DNS; ++index) {
        live_proxy_inflight_dns *inflight = &state->inflight_dns[index];
        if (inflight->valid != 0 &&
            dns_keys_equal(&inflight->key, &key) != 0) {
            inflight->valid = 0;
        }
    }
    for (index = 0u; index < LIVE_PROXY_RECENT_DNS; ++index) {
        live_proxy_recent_dns *recent = &state->recent_dns[index];
        if (recent->valid != 0 &&
            dns_keys_equal(&recent->key, &key) != 0) {
            slot = index;
            break;
        }
        if (recent->valid == 0 && slot == LIVE_PROXY_RECENT_DNS) {
            slot = index;
        } else if (recent->valid != 0 &&
                   recent->completed_ms < oldest) {
            oldest = recent->completed_ms;
            oldest_slot = index;
        }
    }
    if (slot == LIVE_PROXY_RECENT_DNS) {
        slot = oldest_slot;
    }
    state->recent_dns[slot].key = key;
    state->recent_dns[slot].completed_ms = now;
    state->recent_dns[slot].valid = 1;

    index = 0u;
    while (index < state->queue_count) {
        live_proxy_dns_key queued;
        if (dns_packet_key(state->queue[index].packet,
                           state->queue[index].length, 0, &queued) != 0 &&
            dns_keys_equal(&queued, &key) != 0) {
            remove_proxy_queue_entry(state, index);
            ++state->dns_retries_suppressed;
            continue;
        }
        ++index;
    }
}

static int enqueue_proxy_packet(live_proxy_state *state,
                                const uint8_t *packet, size_t packet_length)
{
    unsigned priority;
    size_t queued_dns = 0u;
    size_t queued_discovery_dns = 0u;
    size_t index;
    if (validate_ip_packet(packet, packet_length) != UM_OK) {
        return UM_ERR_HEADER;
    }
    /* Link-local multicast has no useful path through an Internet gateway.
     * In particular, mDNS and SSDP daemons advertise on every new TUN and
     * otherwise consume the acoustic link indefinitely. */
    if ((packet[0] >> 4u) == 4u &&
        (packet[16] & UINT8_C(0xf0)) == UINT8_C(0xe0)) {
        ++state->multicast_dropped;
        return UM_OK;
    }
    /* The peer address space is a /30, making 10.77.0.3 its directed
     * broadcast address. Neither it nor 255.255.255.255 can be forwarded by
     * the gateway; dropping them here prevents NetBIOS and similar discovery
     * retries from consuming acoustic frames. */
    if (proxy_packet_is_tunnel_broadcast(packet, packet_length) != 0) {
        ++state->broadcast_dropped;
        return UM_OK;
    }
    /* A UDP port-unreachable quoting a DNS response is only notice that the
     * local resolver already closed its socket. Recursive DNS is stateless,
     * so relaying that ICMP packet cannot recover anything. */
    if (proxy_packet_is_stale_dns_icmp(packet, packet_length) != 0) {
        ++state->stale_dns_icmp_dropped;
        return UM_OK;
    }
    if (dns_query_recently_completed(state, packet, packet_length) != 0) {
        ++state->dns_retries_suppressed;
        return UM_OK;
    }
    if (dns_query_is_inflight(state, packet, packet_length) != 0 ||
        dns_query_is_queued(state, packet, packet_length) != 0) {
        ++state->dns_retries_suppressed;
        return UM_OK;
    }
    priority = proxy_packet_priority(packet, packet_length);
    for (index = 0u; index < state->queue_count; ++index) {
        if (state->queue[index].length == packet_length &&
            memcmp(state->queue[index].packet, packet, packet_length) == 0) {
            ++state->queue_duplicates;
            return UM_OK;
        }
        /* Unsent cumulative ACKs for the same TCP flow can be replaced by a
         * newer ACK with the same sequence number. This is deliberately
         * limited to payload-free ACK-only packets with no SACK or unknown
         * options; SYN/FIN/RST, data, and already-transmitted ACKs are never
         * coalesced. */
        if (tcp_ack_supersedes(packet, packet_length,
                               state->queue[index].packet,
                               state->queue[index].length) != 0) {
            state->queue[index].length = packet_length;
            state->queue[index].priority = priority;
            memcpy(state->queue[index].packet, packet, packet_length);
            ++state->tcp_acks_coalesced;
            return UM_OK;
        }
    }
    if (proxy_packet_is_tunnel_discovery_dns(packet, packet_length) != 0) {
        for (index = 0u; index < state->queue_count; ++index) {
            if (proxy_packet_is_tunnel_discovery_dns(
                    state->queue[index].packet,
                    state->queue[index].length) != 0) {
                ++queued_discovery_dns;
            }
        }
        if (queued_discovery_dns >=
            LIVE_PROXY_DISCOVERY_DNS_QUEUE_LIMIT) {
            ++state->discovery_dns_dropped;
            return UM_OK;
        }
        ++state->discovery_dns_deprioritized;
    }
    if (priority == LIVE_PROXY_PRIORITY_DNS) {
        /* Keep part of the bounded queue available for traffic that must
         * follow resolution (notably TCP), even during an OS DNS burst. */
        for (index = 0u; index < state->queue_count; ++index) {
            if (state->queue[index].priority == LIVE_PROXY_PRIORITY_DNS) {
                ++queued_dns;
            }
        }
        if (queued_dns >= LIVE_PROXY_DNS_QUEUE_LIMIT) {
            ++state->queue_dropped;
            return UM_OK;
        }
    }
    if (state->queue_count == LIVE_PROXY_QUEUE_PACKETS) {
        size_t lowest_index = 0u;
        unsigned lowest_priority = state->queue[0].priority;
        for (index = 1u; index < state->queue_count; ++index) {
            if (state->queue[index].priority < lowest_priority) {
                lowest_index = index;
                lowest_priority = state->queue[index].priority;
            }
        }
        if (priority <= lowest_priority) {
            ++state->queue_dropped;
            return UM_OK;
        }
        remove_proxy_queue_entry(state, lowest_index);
        ++state->queue_priority_evictions;
    }
    state->queue[state->queue_count].length = packet_length;
    state->queue[state->queue_count].priority = priority;
    memcpy(state->queue[state->queue_count].packet, packet, packet_length);
    ++state->queue_count;
    return UM_OK;
}

static int drain_proxy_ingress(live_context *context,
                               live_proxy_state *state,
                               unsigned initial_timeout)
{
    size_t read_count;
    for (read_count = 0u;
         read_count < LIVE_PROXY_INGRESS_DRAIN_LIMIT; ++read_count) {
        uint8_t packet[UM_NETWORK_MAX_PACKET];
        size_t packet_length = 0u;
        unsigned timeout = read_count == 0u && state->queue_count == 0u
                               ? initial_timeout
                               : 0u;
        int status = um_network_read(context->network, packet,
                                     sizeof(packet), timeout,
                                     &packet_length);
        if (status == UM_ERR_TIMEOUT) {
            break;
        }
        if (status != UM_OK) {
            return status;
        }
        status = enqueue_proxy_packet(state, packet, packet_length);
        if (status != UM_OK) {
            return status;
        }
    }
    if (state->queue_dropped != state->queue_logged_dropped ||
        state->queue_duplicates != state->queue_logged_duplicates ||
        state->dns_retries_suppressed !=
            state->dns_retries_logged_suppressed ||
        state->multicast_dropped != state->multicast_logged_dropped ||
        state->broadcast_dropped != state->broadcast_logged_dropped ||
        state->stale_dns_icmp_dropped !=
            state->stale_dns_icmp_logged_dropped ||
        state->discovery_dns_deprioritized !=
            state->discovery_dns_logged_deprioritized ||
        state->discovery_dns_dropped !=
            state->discovery_dns_logged_dropped ||
        state->tcp_acks_coalesced != state->tcp_acks_logged_coalesced ||
        state->queue_priority_evictions !=
            state->queue_logged_priority_evictions) {
        live_log(context,
                 "proxy ingress queue pending=%zu dropped=%zu "
                 "duplicates=%zu dns-retries-suppressed=%zu "
                 "multicast-dropped=%zu broadcast-dropped=%zu "
                 "stale-dns-icmp-dropped=%zu "
                 "discovery-dns-deprioritized=%zu "
                 "discovery-dns-dropped=%zu tcp-acks-coalesced=%zu "
                 "priority-evictions=%zu",
                 state->queue_count, state->queue_dropped,
                 state->queue_duplicates,
                 state->dns_retries_suppressed,
                 state->multicast_dropped,
                 state->broadcast_dropped,
                 state->stale_dns_icmp_dropped,
                 state->discovery_dns_deprioritized,
                 state->discovery_dns_dropped,
                 state->tcp_acks_coalesced,
                 state->queue_priority_evictions);
        state->queue_logged_dropped = state->queue_dropped;
        state->queue_logged_duplicates = state->queue_duplicates;
        state->dns_retries_logged_suppressed =
            state->dns_retries_suppressed;
        state->multicast_logged_dropped = state->multicast_dropped;
        state->broadcast_logged_dropped = state->broadcast_dropped;
        state->stale_dns_icmp_logged_dropped =
            state->stale_dns_icmp_dropped;
        state->discovery_dns_logged_deprioritized =
            state->discovery_dns_deprioritized;
        state->discovery_dns_logged_dropped =
            state->discovery_dns_dropped;
        state->tcp_acks_logged_coalesced = state->tcp_acks_coalesced;
        state->queue_logged_priority_evictions =
            state->queue_priority_evictions;
    }
    return UM_OK;
}

static int pop_proxy_packet(live_proxy_state *state, uint8_t *packet,
                            size_t *packet_length, unsigned *priority)
{
    size_t selected;
    int allow_dns;
    if (state->queue_count == 0u) {
        return 0;
    }
    /* DNS stays first, but an unbroken DNS stream cannot exclude every
     * already-queued non-DNS packet from the link forever. */
    allow_dns = !(state->consecutive_dns_sent >=
                      LIVE_PROXY_DNS_BURST_PACKETS &&
                  proxy_queue_has_non_dns(state) != 0);
    if (select_proxy_queue_index(state, allow_dns, 0, 0u,
                                 &selected) == 0) {
        return 0;
    }
    *packet_length = state->queue[selected].length;
    *priority = state->queue[selected].priority;
    memcpy(packet, state->queue[selected].packet, *packet_length);
    remove_proxy_queue_entry(state, selected);
    return 1;
}

static size_t build_proxy_batch(live_proxy_state *state, uint8_t *body,
                                size_t body_capacity,
                                unsigned *highest_priority,
                                size_t *packet_bytes,
                                int *leading_dns_response)
{
    size_t offset = LIVE_PROXY_BATCH_HEADER_BYTES;
    size_t count = 0u;
    size_t dns_selected = 0u;
    if (body == NULL || highest_priority == NULL || packet_bytes == NULL ||
        leading_dns_response == NULL ||
        body_capacity > UM_LIVE_MAX_BODY ||
        body_capacity <= LIVE_PROXY_BATCH_HEADER_BYTES) {
        return 0u;
    }
    *highest_priority = LIVE_PROXY_PRIORITY_BULK;
    *packet_bytes = 0u;
    *leading_dns_response = 0;
    while (state->queue_count != 0u && count < UINT8_MAX) {
        const live_proxy_queued_packet *queued;
        size_t selected;
        size_t needed;
        int have_non_dns = proxy_queue_has_non_dns(state);
        int allow_dns =
            have_non_dns == 0 ||
            state->consecutive_dns_sent + dns_selected <
                LIVE_PROXY_DNS_BURST_PACKETS;
        if (count == 0u) {
            if (select_proxy_queue_index(state, allow_dns, 0, 0u,
                                         &selected) == 0) {
                break;
            }
        } else {
            size_t maximum_packet_bytes;
            if (body_capacity - offset <=
                LIVE_PROXY_BATCH_ENTRY_BYTES) {
                break;
            }
            maximum_packet_bytes = body_capacity - offset -
                                   LIVE_PROXY_BATCH_ENTRY_BYTES;
            if (select_proxy_queue_index(state, allow_dns, 1,
                                         maximum_packet_bytes,
                                         &selected) == 0) {
                break;
            }
        }
        queued = &state->queue[selected];
        needed = LIVE_PROXY_BATCH_ENTRY_BYTES + queued->length;
        if (needed > body_capacity - offset) {
            break;
        }
        if (queued->priority > *highest_priority) {
            *highest_priority = queued->priority;
        }
        if (count == 0u) {
            *leading_dns_response = proxy_packet_is_dns_response(
                queued->packet, queued->length);
        }
        if (queued->priority == LIVE_PROXY_PRIORITY_DNS) {
            ++dns_selected;
        }
        write_u16(&body[offset], (uint16_t)queued->length);
        memcpy(&body[offset + LIVE_PROXY_BATCH_ENTRY_BYTES], queued->packet,
               queued->length);
        offset += needed;
        *packet_bytes += queued->length;
        remove_proxy_queue_entry(state, selected);
        ++count;
    }
    if (count == 0u) {
        return 0u;
    }
    body[5] = (uint8_t)count;
    return offset;
}

static int ensure_network(live_context *context)
{
    int status;
    if (context->network != NULL) {
        return UM_OK;
    }
    live_log(context, "state=NETWORK_CONFIGURING role=%s",
             context->options.role == UM_LIVE_CLIENT ? "client" : "gateway");
    status = um_network_open(&context->network, context->options.role,
                             context->logger, context->logger_context);
    if (status != UM_OK) {
        return status;
    }
    live_log(context, "state=NETWORK_READY interface=%s mtu=%u",
             um_network_interface_name(context->network), UM_NETWORK_MTU);
    return UM_OK;
}

static void log_proxy_packet(live_context *context,
                             const live_proxy_state *state, int transmitted,
                             uint32_t packet_id, size_t packet_length,
                             size_t fragments, const uint8_t *packet)
{
    char description[256];
    double seconds = (double)(monotonic_milliseconds() - state->started_ms) /
                     1000.0;
    size_t bytes = transmitted != 0 ? state->bytes_sent
                                    : state->bytes_received;
    size_t dns_bytes = transmitted != 0 ? state->dns_bytes_sent
                                        : state->dns_bytes_received;
    describe_proxy_packet(packet, packet_length, description,
                          sizeof(description));
    live_log(context,
             "proxy %s packet=%u traffic=%s bytes=%zu fragments=%zu "
             "total-packets=%zu total-bytes=%zu rate=%.0fbps "
             "dns-rate=%.0fbps non-dns-rate=%.0fbps",
             transmitted != 0 ? proxy_transmit_label(context)
                              : proxy_receive_label(context),
             packet_id, description, packet_length, fragments,
             transmitted != 0 ? state->packets_sent
                              : state->packets_received,
             bytes, seconds > 0.0 ? (double)(bytes * 8u) / seconds : 0.0,
             seconds > 0.0 ? (double)(dns_bytes * 8u) / seconds : 0.0,
             seconds > 0.0
                 ? (double)((bytes - dns_bytes) * 8u) / seconds
                 : 0.0);
}

static int inspect_proxy_batch(const uint8_t *body, size_t body_length,
                               uint32_t *batch_id, int *yield_token,
                               size_t *packet_count, size_t *packet_bytes)
{
    size_t offset = LIVE_PROXY_BATCH_HEADER_BYTES;
    size_t count;
    size_t index;
    if (body == NULL || body_length <= LIVE_PROXY_BATCH_HEADER_BYTES ||
        batch_id == NULL || yield_token == NULL || packet_count == NULL ||
        packet_bytes == NULL ||
        (body[4] & (uint8_t)~LIVE_PROXY_BATCH_YIELD) != 0u) {
        return UM_ERR_HEADER;
    }
    count = body[5];
    if (count == 0u) {
        return UM_ERR_HEADER;
    }
    *batch_id = read_u32(body);
    *yield_token = (body[4] & LIVE_PROXY_BATCH_YIELD) != 0u;
    *packet_count = count;
    *packet_bytes = 0u;
    for (index = 0u; index < count; ++index) {
        size_t packet_length;
        if (body_length - offset < LIVE_PROXY_BATCH_ENTRY_BYTES) {
            return UM_ERR_HEADER;
        }
        packet_length = read_u16(&body[offset]);
        offset += LIVE_PROXY_BATCH_ENTRY_BYTES;
        if (packet_length == 0u || packet_length > body_length - offset ||
            validate_ip_packet(&body[offset], packet_length) != UM_OK) {
            return UM_ERR_HEADER;
        }
        *packet_bytes += packet_length;
        offset += packet_length;
    }
    return offset == body_length ? UM_OK : UM_ERR_HEADER;
}

static void account_transmitted_packet_priority(live_proxy_state *state,
                                                const uint8_t *packet,
                                                size_t packet_length)
{
    live_proxy_dns_key query;
    if (dns_packet_key(packet, packet_length, 0, &query) != 0) {
        remember_inflight_dns(state, packet, packet_length);
    }
    if (proxy_packet_priority(packet, packet_length) ==
        LIVE_PROXY_PRIORITY_DNS) {
        if (state->consecutive_dns_sent != SIZE_MAX) {
            ++state->consecutive_dns_sent;
        }
    } else {
        state->consecutive_dns_sent = 0u;
    }
}

static void account_proxy_packet_bytes(live_proxy_state *state,
                                       int transmitted,
                                       const uint8_t *packet,
                                       size_t packet_length)
{
    int dns = proxy_packet_is_dns_traffic(packet, packet_length);
    if (transmitted != 0) {
        ++state->packets_sent;
        state->bytes_sent += packet_length;
        if (dns != 0) {
            state->dns_bytes_sent += packet_length;
        }
    } else {
        ++state->packets_received;
        state->bytes_received += packet_length;
        if (dns != 0) {
            state->dns_bytes_received += packet_length;
        }
    }
}

static void account_proxy_batch(live_context *context,
                                live_proxy_state *state, int transmitted,
                                const uint8_t *body, size_t body_length)
{
    uint32_t batch_id = 0u;
    int yield_token = 0;
    size_t packet_count = 0u;
    size_t packet_bytes = 0u;
    size_t offset = LIVE_PROXY_BATCH_HEADER_BYTES;
    size_t index;
    if (inspect_proxy_batch(body, body_length, &batch_id, &yield_token,
                            &packet_count, &packet_bytes) != UM_OK) {
        return;
    }
    (void)yield_token;
    if (transmitted != 0) {
        ++state->batches_sent;
    } else {
        ++state->batches_received;
    }
    live_log(context,
             "proxy %s batch=%u packets=%zu bytes=%zu acoustic-body=%zu",
             transmitted != 0 ? proxy_transmit_label(context)
                              : proxy_receive_label(context),
             batch_id, packet_count, packet_bytes, body_length);
    for (index = 0u; index < packet_count; ++index) {
        size_t packet_length = read_u16(&body[offset]);
        offset += LIVE_PROXY_BATCH_ENTRY_BYTES;
        if (transmitted != 0) {
            account_transmitted_packet_priority(state, &body[offset],
                                                packet_length);
            account_proxy_packet_bytes(state, 1, &body[offset],
                                       packet_length);
            log_proxy_packet(context, state, 1,
                             (uint32_t)state->packets_sent, packet_length,
                             1u, &body[offset]);
        } else {
            account_proxy_packet_bytes(state, 0, &body[offset],
                                       packet_length);
            log_proxy_packet(context, state, 0,
                             (uint32_t)state->packets_received,
                             packet_length, 1u, &body[offset]);
        }
        offset += packet_length;
    }
}

static int commit_proxy_packet_token(live_context *context,
                                     live_proxy_state *state,
                                     uint16_t final_sequence,
                                     uint32_t transaction_id,
                                     int *token_yielded)
{
    int status;
    live_log(context, "proxy packet token commit sequence=%u packet=%u",
             (unsigned)state->turn_sequence, transaction_id);
    sleep_milliseconds(LIVE_TURNAROUND_MS);
    status = send_wire(context, proxy_transmit_config(context),
                       UM_WIRE_PROXY_TURN_COMMIT, state->turn_sequence,
                       NULL, 0u, 1, NULL);
    if (status != UM_OK) {
        return status;
    }
    state->last_committed_turn = state->turn_sequence;
    state->last_commit_packet_sequence = final_sequence;
    state->last_commit_packet_id = transaction_id;
    state->have_last_commit = 1;
    state->last_commit_piggybacked = 1;
    ++state->turn_sequence;
    *token_yielded = 1;
    return UM_OK;
}

static int send_proxy_packet(live_context *context, live_proxy_state *state,
                             const uint8_t *packet, size_t packet_length,
                             size_t frame_body_limit, int yield_token,
                             int *token_yielded)
{
    const um_modem_config *transmit = proxy_transmit_config(context);
    const um_modem_config *receive = proxy_receive_config(context);
    size_t fragment_capacity = frame_body_limit -
                               LIVE_PROXY_FRAGMENT_HEADER_BYTES;
    size_t fragments;
    uint16_t first_sequence = state->transmit_sequence;
    uint16_t final_sequence;
    uint32_t packet_id = ++state->transmit_packet_id;
    unsigned attempt;
    int status = validate_ip_packet(packet, packet_length);
    if (token_yielded == NULL) {
        return UM_ERR_ARGUMENT;
    }
    *token_yielded = 0;
    if (status != UM_OK || packet_length > LIVE_PROXY_TOTAL_MASK) {
        return status != UM_OK ? status : UM_ERR_CAPACITY;
    }
    fragments = (packet_length + fragment_capacity - 1u) / fragment_capacity;
    final_sequence = (uint16_t)(first_sequence + fragments - 1u);
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        size_t offset = 0u;
        size_t fragment = 0u;
        um_live_wire_message acknowledgement;
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        while (offset < packet_length && !live_interrupted) {
            uint8_t body[UM_LIVE_MAX_BODY];
            size_t fragment_length = packet_length - offset;
            int final_fragment;
            if (fragment_length > fragment_capacity) {
                fragment_length = fragment_capacity;
            }
            final_fragment = offset + fragment_length == packet_length;
            write_u32(body, packet_id);
            write_u16(&body[4],
                      (uint16_t)packet_length |
                          (yield_token != 0 ? LIVE_PROXY_YIELD_FLAG : 0u));
            write_u16(&body[6], (uint16_t)offset);
            memcpy(body + LIVE_PROXY_FRAGMENT_HEADER_BYTES, packet + offset,
                   fragment_length);
            status = send_wire(
                context, transmit, UM_WIRE_IP_FRAGMENT,
                (uint16_t)(first_sequence + fragment), body,
                LIVE_PROXY_FRAGMENT_HEADER_BYTES + fragment_length,
                final_fragment, NULL);
            if (status != UM_OK) {
                return status;
            }
            offset += fragment_length;
            ++fragment;
        }
        if (live_interrupted) {
            return UM_ERR_INTERRUPTED;
        }
        status = receive_expected(context, receive, UM_WIRE_IP_ACK,
                                  final_sequence,
                                  LIVE_PROXY_ACK_TIMEOUT_MS,
                                  &acknowledgement, NULL);
        if (status == UM_OK &&
            acknowledgement.body_length == LIVE_PROXY_ACK_BYTES &&
            read_u32(acknowledgement.body) == packet_id &&
            ((acknowledgement.body[4] & LIVE_PROXY_ACK_YIELD) != 0u) ==
                (yield_token != 0)) {
            state->transmit_sequence = (uint16_t)(final_sequence + 1u);
            account_transmitted_packet_priority(state, packet,
                                                packet_length);
            account_proxy_packet_bytes(state, 1, packet, packet_length);
            log_proxy_packet(context, state, 1, packet_id, packet_length,
                             fragments, packet);
            if (yield_token != 0) {
                return commit_proxy_packet_token(
                    context, state, final_sequence, packet_id,
                    token_yielded);
            }
            return UM_OK;
        }
        live_log(context,
                 "proxy %s packet=%u cumulative-ack retry=%u/%u (%s)",
                 proxy_transmit_label(context), packet_id, attempt + 1u,
                 context->options.retry_limit,
                 status == UM_OK ? "invalid acknowledgement"
                                 : um_status_string(status));
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_ERR_TIMEOUT;
}

static int send_proxy_batch(live_context *context, live_proxy_state *state,
                            uint8_t *body, size_t body_length,
                            int yield_token, int *token_yielded)
{
    const um_modem_config *transmit = proxy_transmit_config(context);
    const um_modem_config *receive = proxy_receive_config(context);
    uint32_t batch_id = ++state->transmit_batch_id;
    uint16_t sequence = state->transmit_sequence;
    unsigned attempt;
    if (body == NULL || token_yielded == NULL ||
        body_length <= LIVE_PROXY_BATCH_HEADER_BYTES ||
        body_length > UM_LIVE_MAX_BODY) {
        return UM_ERR_ARGUMENT;
    }
    write_u32(body, batch_id);
    body[4] = yield_token != 0 ? LIVE_PROXY_BATCH_YIELD : 0u;
    *token_yielded = 0;
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        um_live_wire_message acknowledgement;
        int status;
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        status = send_wire(context, transmit, UM_WIRE_IP_BATCH, sequence,
                           body, body_length, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        status = receive_expected(context, receive, UM_WIRE_IP_ACK,
                                  sequence, LIVE_PROXY_ACK_TIMEOUT_MS,
                                  &acknowledgement, NULL);
        if (status == UM_OK &&
            acknowledgement.body_length == LIVE_PROXY_ACK_BYTES &&
            read_u32(acknowledgement.body) == batch_id &&
            ((acknowledgement.body[4] & LIVE_PROXY_ACK_YIELD) != 0u) ==
                (yield_token != 0)) {
            ++state->transmit_sequence;
            account_proxy_batch(context, state, 1, body, body_length);
            if (yield_token != 0) {
                return commit_proxy_packet_token(context, state, sequence,
                                                 batch_id, token_yielded);
            }
            return UM_OK;
        }
        live_log(context,
                 "proxy %s batch=%u cumulative-ack retry=%u/%u (%s)",
                 proxy_transmit_label(context), batch_id, attempt + 1u,
                 context->options.retry_limit,
                 status == UM_OK ? "invalid acknowledgement"
                                 : um_status_string(status));
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_ERR_TIMEOUT;
}

static int send_proxy_ack(live_context *context, uint16_t sequence,
                          uint32_t packet_id, int yield_token)
{
    uint8_t body[LIVE_PROXY_ACK_BYTES];
    write_u32(body, packet_id);
    body[4] = yield_token != 0 ? LIVE_PROXY_ACK_YIELD : 0u;
    sleep_milliseconds(LIVE_TURNAROUND_MS);
    return send_wire(context, proxy_transmit_config(context),
                     UM_WIRE_IP_ACK, sequence, body, sizeof(body), 1, NULL);
}

static int accept_packet_token(live_context *context,
                               live_proxy_state *state,
                               um_live_wire_type transaction_type,
                               uint16_t packet_sequence,
                               uint32_t packet_id)
{
    const um_modem_config *receive = proxy_receive_config(context);
    unsigned attempt;
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        uint64_t deadline;
        int status;
        live_log(context,
                 "proxy packet token accept sequence=%u packet=%u "
                 "attempt=%u/%u",
                 (unsigned)state->turn_sequence, packet_id, attempt + 1u,
                 context->options.retry_limit);
        status = send_proxy_ack(context, packet_sequence, packet_id, 1);
        if (status != UM_OK) {
            return status;
        }
        deadline = monotonic_milliseconds() + LIVE_PROXY_ACK_TIMEOUT_MS;
        while (!live_interrupted && monotonic_milliseconds() < deadline) {
            um_live_wire_message message;
            unsigned remaining =
                (unsigned)(deadline - monotonic_milliseconds());
            status = receive_wire(context, receive, context->session_id,
                                  remaining, &message, NULL);
            if (status == UM_ERR_HEADER || status == UM_ERR_CRC ||
                status == UM_ERR_SYNC || status == UM_ERR_TRUNCATED) {
                continue;
            }
            if (status != UM_OK) {
                break;
            }
            if (message.type == UM_WIRE_PROXY_TURN_COMMIT &&
                message.sequence == state->turn_sequence) {
                ++state->turn_sequence;
                return UM_OK;
            }
            if (message.type == UM_WIRE_IP_FRAGMENT &&
                transaction_type == UM_WIRE_IP_FRAGMENT &&
                message.sequence == packet_sequence &&
                message.body_length > LIVE_PROXY_FRAGMENT_HEADER_BYTES &&
                read_u32(message.body) == packet_id) {
                uint16_t encoded_total = read_u16(&message.body[4]);
                size_t total = encoded_total & LIVE_PROXY_TOTAL_MASK;
                size_t offset = read_u16(&message.body[6]);
                size_t fragment_length =
                    message.body_length - LIVE_PROXY_FRAGMENT_HEADER_BYTES;
                if ((encoded_total & LIVE_PROXY_YIELD_FLAG) != 0u &&
                    offset <= total && fragment_length == total - offset) {
                    break;
                }
            }
            if (message.type == UM_WIRE_IP_BATCH &&
                transaction_type == UM_WIRE_IP_BATCH &&
                message.sequence == packet_sequence &&
                message.body_length > LIVE_PROXY_BATCH_HEADER_BYTES &&
                read_u32(message.body) == packet_id &&
                (message.body[4] & LIVE_PROXY_BATCH_YIELD) != 0u) {
                break;
            }
        }
        live_log(context, "proxy packet token commit wait retry=%u/%u",
                 attempt + 1u, context->options.retry_limit);
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_ERR_TIMEOUT;
}

static int receive_proxy_fragment(live_context *context,
                                  live_proxy_state *state,
                                  const um_live_wire_message *message,
                                  int *token_received)
{
    uint16_t encoded_total;
    uint32_t packet_id;
    size_t total;
    size_t offset;
    size_t fragment_length;
    int yield_requested;
    int status;
    if (token_received == NULL ||
        message->body_length <= LIVE_PROXY_FRAGMENT_HEADER_BYTES) {
        return UM_ERR_HEADER;
    }
    *token_received = 0;
    packet_id = read_u32(message->body);
    encoded_total = read_u16(&message->body[4]);
    total = encoded_total & LIVE_PROXY_TOTAL_MASK;
    yield_requested = (encoded_total & LIVE_PROXY_YIELD_FLAG) != 0u;
    offset = read_u16(&message->body[6]);
    fragment_length = message->body_length -
                      LIVE_PROXY_FRAGMENT_HEADER_BYTES;
    if (total == 0u || total > sizeof(state->receive_packet) ||
        offset > total || fragment_length > total - offset) {
        return UM_ERR_HEADER;
    }
    if (state->have_last_completed_packet != 0 &&
        packet_id == state->last_completed_packet_id) {
        if (offset + fragment_length != total) {
            return UM_OK;
        }
        return send_proxy_ack(context, message->sequence, packet_id,
                              yield_requested);
    }
    /*
     * Packet fragments are sent as a window and acknowledged cumulatively.
     * Ignore later or replayed fragments until the first missing sequence is
     * retried; the offset check below then resumes the partial packet.
     */
    if (message->sequence != state->receive_sequence) {
        return UM_OK;
    }
    if (message->sequence == state->receive_sequence) {
        if (offset == 0u) {
            state->receive_packet_id = packet_id;
            state->receive_total = total;
            state->receive_offset = 0u;
            state->receive_fragments = 0u;
            state->receive_yield_requested = yield_requested;
        }
        if (packet_id != state->receive_packet_id ||
            total != state->receive_total || offset != state->receive_offset ||
            yield_requested != state->receive_yield_requested) {
            return UM_ERR_HEADER;
        }
        memcpy(state->receive_packet + offset,
               message->body + LIVE_PROXY_FRAGMENT_HEADER_BYTES,
               fragment_length);
        state->receive_offset += fragment_length;
        ++state->receive_fragments;
        ++state->receive_sequence;
        if (state->receive_offset == state->receive_total) {
            size_t completed_length = state->receive_total;
            size_t completed_fragments = state->receive_fragments;
            status = validate_ip_packet(state->receive_packet,
                                        completed_length);
            if (status != UM_OK) {
                return status;
            }
            status = um_network_write(context->network,
                                      state->receive_packet,
                                      completed_length, 1000u);
            if (status != UM_OK) {
                return status;
            }
            remember_completed_dns(state, state->receive_packet,
                                   completed_length);
            account_proxy_packet_bytes(state, 0, state->receive_packet,
                                       completed_length);
            log_proxy_packet(context, state, 0, state->receive_packet_id,
                             completed_length, completed_fragments,
                             state->receive_packet);
            state->last_completed_packet_id = state->receive_packet_id;
            state->have_last_completed_packet = 1;
            state->receive_total = 0u;
            state->receive_offset = 0u;
            state->receive_fragments = 0u;
            state->receive_yield_requested = 0;
            if (yield_requested != 0) {
                status = accept_packet_token(
                    context, state, UM_WIRE_IP_FRAGMENT,
                    message->sequence, packet_id);
                if (status == UM_OK) {
                    *token_received = 1;
                }
                return status;
            }
            return send_proxy_ack(context, message->sequence, packet_id, 0);
        }
    }
    return UM_OK;
}

static int receive_proxy_batch(live_context *context,
                               live_proxy_state *state,
                               const um_live_wire_message *message,
                               int *token_received)
{
    uint32_t batch_id = 0u;
    int yield_requested = 0;
    size_t packet_count = 0u;
    size_t packet_bytes = 0u;
    size_t offset = LIVE_PROXY_BATCH_HEADER_BYTES;
    size_t index;
    int status;
    if (message == NULL || token_received == NULL) {
        return UM_ERR_ARGUMENT;
    }
    *token_received = 0;
    status = inspect_proxy_batch(message->body, message->body_length,
                                 &batch_id, &yield_requested,
                                 &packet_count, &packet_bytes);
    if (status != UM_OK) {
        return status;
    }
    (void)packet_bytes;
    if (state->have_last_completed_batch != 0 &&
        batch_id == state->last_completed_batch_id) {
        return send_proxy_ack(context, message->sequence, batch_id,
                              yield_requested);
    }
    if (message->sequence != state->receive_sequence) {
        return UM_OK;
    }
    for (index = 0u; index < packet_count; ++index) {
        size_t packet_length = read_u16(&message->body[offset]);
        offset += LIVE_PROXY_BATCH_ENTRY_BYTES;
        status = um_network_write(context->network, &message->body[offset],
                                  packet_length, 1000u);
        if (status != UM_OK) {
            return status;
        }
        remember_completed_dns(state, &message->body[offset],
                               packet_length);
        offset += packet_length;
    }
    ++state->receive_sequence;
    account_proxy_batch(context, state, 0, message->body,
                        message->body_length);
    state->last_completed_batch_id = batch_id;
    state->have_last_completed_batch = 1;
    if (yield_requested != 0) {
        status = accept_packet_token(context, state, UM_WIRE_IP_BATCH,
                                     message->sequence, batch_id);
        if (status == UM_OK) {
            *token_received = 1;
        }
        return status;
    }
    return send_proxy_ack(context, message->sequence, batch_id, 0);
}

static int resend_proxy_commit(live_context *context,
                               live_proxy_state *state)
{
    if (state->have_last_commit == 0) {
        return UM_ERR_HEADER;
    }
    sleep_milliseconds(LIVE_TURNAROUND_MS);
    return send_wire(context, proxy_transmit_config(context),
                     UM_WIRE_PROXY_TURN_COMMIT,
                     state->last_committed_turn, NULL, 0u, 1, NULL);
}

static int yield_proxy_token(live_context *context,
                             live_proxy_state *state)
{
    const um_modem_config *transmit = proxy_transmit_config(context);
    const um_modem_config *receive = proxy_receive_config(context);
    unsigned attempt;
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        um_live_wire_message response;
        int status;
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        status = send_wire(context, transmit, UM_WIRE_PROXY_TURN,
                           state->turn_sequence, NULL, 0u, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        status = receive_expected(
            context, receive, UM_WIRE_PROXY_TURN_ACK,
            state->turn_sequence, LIVE_PROXY_ACK_TIMEOUT_MS, &response, NULL);
        if (status == UM_OK) {
            live_log(context, "proxy token commit sequence=%u",
                     (unsigned)state->turn_sequence);
            sleep_milliseconds(LIVE_TURNAROUND_MS);
            status = send_wire(context, transmit,
                               UM_WIRE_PROXY_TURN_COMMIT,
                               state->turn_sequence, NULL, 0u, 1, NULL);
            if (status != UM_OK) {
                return status;
            }
            state->last_committed_turn = state->turn_sequence;
            state->last_commit_packet_sequence = 0u;
            state->last_commit_packet_id = 0u;
            state->have_last_commit = 1;
            state->last_commit_piggybacked = 0;
            ++state->turn_sequence;
            return UM_OK;
        }
        live_log(context, "proxy token handoff retry=%u/%u (%s)",
                 attempt + 1u, context->options.retry_limit,
                 um_status_string(status));
    }
    return UM_ERR_TIMEOUT;
}

static int accept_proxy_token(live_context *context,
                              live_proxy_state *state,
                              uint16_t turn_sequence)
{
    const um_modem_config *transmit = proxy_transmit_config(context);
    const um_modem_config *receive = proxy_receive_config(context);
    unsigned attempt;
    if (turn_sequence != state->turn_sequence) {
        return UM_ERR_HEADER;
    }
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        uint64_t deadline;
        int status;
        live_log(context, "proxy token accept sequence=%u attempt=%u/%u",
                 (unsigned)turn_sequence, attempt + 1u,
                 context->options.retry_limit);
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        status = send_wire(context, transmit, UM_WIRE_PROXY_TURN_ACK,
                           turn_sequence, NULL, 0u, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        deadline = monotonic_milliseconds() + LIVE_PROXY_ACK_TIMEOUT_MS;
        while (!live_interrupted && monotonic_milliseconds() < deadline) {
            um_live_wire_message message;
            unsigned remaining =
                (unsigned)(deadline - monotonic_milliseconds());
            status = receive_wire(context, receive, context->session_id,
                                  remaining, &message, NULL);
            if (status == UM_ERR_HEADER || status == UM_ERR_CRC ||
                status == UM_ERR_SYNC || status == UM_ERR_TRUNCATED) {
                continue;
            }
            if (status != UM_OK) {
                break;
            }
            if (message.type == UM_WIRE_PROXY_TURN_COMMIT &&
                message.sequence == turn_sequence) {
                ++state->turn_sequence;
                return UM_OK;
            }
            if (message.type == UM_WIRE_PROXY_TURN &&
                message.sequence == turn_sequence) {
                break;
            }
        }
        live_log(context, "proxy token commit wait retry=%u/%u",
                 attempt + 1u, context->options.retry_limit);
    }
    return live_interrupted ? UM_ERR_INTERRUPTED : UM_ERR_TIMEOUT;
}

static int send_proxy_completion(live_context *context,
                                 live_proxy_state *state)
{
    const um_modem_config *transmit = proxy_transmit_config(context);
    const um_modem_config *receive = proxy_receive_config(context);
    unsigned attempt;
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        um_live_wire_message response;
        int status;
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        status = send_wire(context, transmit, UM_WIRE_PROXY_COMPLETE,
                           state->turn_sequence, NULL, 0u, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        status = receive_expected(
            context, receive, UM_WIRE_PROXY_COMPLETE,
            state->turn_sequence, LIVE_PROXY_ACK_TIMEOUT_MS, &response, NULL);
        if (status == UM_OK) {
            return UM_OK;
        }
    }
    return UM_ERR_TIMEOUT;
}

static int receive_until_proxy_token(live_context *context,
                                     live_proxy_state *state,
                                     int *peer_completed)
{
    const um_modem_config *receive = proxy_receive_config(context);
    unsigned misses = 0u;
    *peer_completed = 0;
    while (!live_interrupted) {
        um_live_wire_message message;
        int status = receive_wire(context, receive, context->session_id,
                                  LIVE_PROXY_RECEIVE_TIMEOUT_MS, &message,
                                  NULL);
        if (status == UM_ERR_HEADER || status == UM_ERR_CRC ||
            status == UM_ERR_SYNC || status == UM_ERR_TRUNCATED ||
            status == UM_ERR_TIMEOUT) {
            if (++misses > context->options.retry_limit + 1u) {
                return status;
            }
            live_log(context, "proxy receive miss=%u/%u (%s)", misses,
                     context->options.retry_limit + 1u,
                     um_status_string(status));
            continue;
        }
        if (status != UM_OK) {
            return status;
        }
        misses = 0u;
        if (message.type == UM_WIRE_IP_FRAGMENT) {
            int token_received = 0;
            status = receive_proxy_fragment(context, state, &message,
                                            &token_received);
            if (status != UM_OK) {
                return status;
            }
            if (token_received != 0) {
                return UM_OK;
            }
            continue;
        }
        if (message.type == UM_WIRE_IP_BATCH) {
            int token_received = 0;
            status = receive_proxy_batch(context, state, &message,
                                         &token_received);
            if (status != UM_OK) {
                return status;
            }
            if (token_received != 0) {
                return UM_OK;
            }
            continue;
        }
        if (message.type == UM_WIRE_PROXY_BEGIN && message.sequence == 0u &&
            message.body_length == LIVE_PROXY_BEGIN_BYTES &&
            message.body[0] == UM_LIVE_PROXY_FORMAT_VERSION &&
            read_u16(&message.body[1]) == UM_NETWORK_MTU &&
            read_u16(&message.body[3]) ==
                context->client_to_gateway_body_bytes &&
            read_u16(&message.body[5]) ==
                context->gateway_to_client_body_bytes) {
            live_log(context,
                     "Duplicate PROXY_BEGIN received; repeating ready ACK");
            sleep_milliseconds(LIVE_TURNAROUND_MS);
            status = send_wire(context, proxy_transmit_config(context),
                               UM_WIRE_ACK, 0u, NULL, 0u, 1, NULL);
            if (status != UM_OK) {
                return status;
            }
            continue;
        }
        if (message.type == UM_WIRE_PROXY_TURN) {
            return accept_proxy_token(context, state, message.sequence);
        }
        if (message.type == UM_WIRE_PROXY_TURN_ACK &&
            state->have_last_commit != 0 &&
            state->last_commit_piggybacked == 0 &&
            message.sequence == state->last_committed_turn) {
            status = resend_proxy_commit(context, state);
            if (status != UM_OK) {
                return status;
            }
            continue;
        }
        if (message.type == UM_WIRE_IP_ACK &&
            state->have_last_commit != 0 &&
            state->last_commit_piggybacked != 0 &&
            message.sequence == state->last_commit_packet_sequence &&
            message.body_length == LIVE_PROXY_ACK_BYTES &&
            read_u32(message.body) == state->last_commit_packet_id &&
            (message.body[4] & LIVE_PROXY_ACK_YIELD) != 0u) {
            status = resend_proxy_commit(context, state);
            if (status != UM_OK) {
                return status;
            }
            continue;
        }
        if (message.type == UM_WIRE_PROXY_COMPLETE &&
            context->options.proxy_test_packets != 0u) {
            sleep_milliseconds(LIVE_TURNAROUND_MS);
            status = send_wire(context, proxy_transmit_config(context),
                               UM_WIRE_PROXY_COMPLETE, message.sequence,
                               NULL, 0u, 1, NULL);
            if (status != UM_OK) {
                return status;
            }
            *peer_completed = 1;
            return UM_OK;
        }
        live_log(context, "Ignoring %s during network proxy receive turn",
                 wire_name(message.type));
    }
    return UM_ERR_INTERRUPTED;
}

static int run_proxy_loop(live_context *context)
{
    live_proxy_state state;
    size_t frame_body_limit = context->options.role == UM_LIVE_CLIENT
                                  ? context->client_to_gateway_body_bytes
                                  : context->gateway_to_client_body_bytes;
    size_t receive_frame_body_limit =
        context->options.role == UM_LIVE_CLIENT
            ? context->gateway_to_client_body_bytes
            : context->client_to_gateway_body_bytes;
    int have_token = context->options.role == UM_LIVE_CLIENT;
    memset(&state, 0, sizeof(state));
    state.started_ms = monotonic_milliseconds();
    live_log(context,
             "state=PROXYING interface=%s mtu=%u tx-frame-body=%zu "
             "rx-frame-body=%zu fragment-payload=%zu initial-token=%s",
             um_network_interface_name(context->network), UM_NETWORK_MTU,
             frame_body_limit, receive_frame_body_limit,
             frame_body_limit - LIVE_PROXY_FRAGMENT_HEADER_BYTES,
             have_token != 0 ? "local" : "peer");
    while (!live_interrupted) {
        int status;
        if (have_token != 0) {
            size_t burst;
            int token_yielded = 0;
            if (context->options.role == UM_LIVE_CLIENT &&
                context->options.proxy_test_packets != 0u &&
                state.packets_sent >= context->options.proxy_test_packets &&
                state.packets_received >=
                    context->options.proxy_test_packets) {
                status = send_proxy_completion(context, &state);
                if (status == UM_OK) {
                    live_log(context,
                             "state=COMPLETE bidirectional network proxy "
                             "test passed packets=%zu each direction",
                             context->options.proxy_test_packets);
                }
                return status;
            }
            for (burst = 0u; burst < LIVE_PROXY_BURST_PACKETS; ++burst) {
                uint8_t batch[UM_LIVE_MAX_BODY];
                uint8_t packet[UM_NETWORK_MAX_PACKET];
                size_t batch_length;
                size_t batch_packet_bytes = 0u;
                size_t packet_length = 0u;
                unsigned priority = LIVE_PROXY_PRIORITY_BULK;
                int leading_dns_response = 0;
                int continue_response_burst = 0;
                int yield_with_packet;
                status = drain_proxy_ingress(
                    context, &state,
                    burst == 0u ? LIVE_PROXY_IDLE_MS : 0u);
                if (status != UM_OK) {
                    return status;
                }
                batch_length = build_proxy_batch(
                    &state, batch, frame_body_limit, &priority,
                    &batch_packet_bytes, &leading_dns_response);
                (void)batch_packet_bytes;
                if (batch_length != 0u) {
                    continue_response_burst =
                        leading_dns_response != 0 &&
                        burst + 1u < LIVE_PROXY_BURST_PACKETS &&
                        proxy_queue_has_dns_response(&state) != 0;
                    yield_with_packet =
                        continue_response_burst == 0 &&
                        (priority >= LIVE_PROXY_PRIORITY_CONTROL ||
                         burst + 1u == LIVE_PROXY_BURST_PACKETS ||
                         state.queue_count == 0u);
                    if (continue_response_burst != 0) {
                        live_log(context,
                                 "proxy DNS response backlog continuing "
                                 "turn queued=%zu frame=%zu/%u",
                                 state.queue_count, burst + 1u,
                                 LIVE_PROXY_BURST_PACKETS);
                    }
                    status = send_proxy_batch(
                        context, &state, batch, batch_length,
                        yield_with_packet, &token_yielded);
                    if (status != UM_OK) {
                        return status;
                    }
                    if (token_yielded != 0 ||
                        (context->options.proxy_test_packets != 0u &&
                         state.packets_sent >=
                             context->options.proxy_test_packets) ||
                        (priority >= LIVE_PROXY_PRIORITY_CONTROL &&
                         continue_response_burst == 0)) {
                        break;
                    }
                    continue;
                }
                if (pop_proxy_packet(&state, packet, &packet_length,
                                     &priority) == 0) {
                    break;
                }
                continue_response_burst =
                    proxy_packet_is_dns_response(packet, packet_length) !=
                        0 &&
                    burst + 1u < LIVE_PROXY_BURST_PACKETS &&
                    proxy_queue_has_dns_response(&state) != 0;
                yield_with_packet =
                    continue_response_burst == 0 &&
                    (priority >= LIVE_PROXY_PRIORITY_CONTROL ||
                     burst + 1u == LIVE_PROXY_BURST_PACKETS ||
                     state.queue_count == 0u);
                if (continue_response_burst != 0) {
                    live_log(context,
                             "proxy DNS response backlog continuing turn "
                             "queued=%zu frame=%zu/%u",
                             state.queue_count, burst + 1u,
                             LIVE_PROXY_BURST_PACKETS);
                }
                status = send_proxy_packet(context, &state, packet,
                                           packet_length,
                                           frame_body_limit,
                                           yield_with_packet,
                                           &token_yielded);
                if (status != UM_OK) {
                    return status;
                }
                /* Return the half-duplex link promptly after interactive
                 * requests, except while draining an already-visible DNS
                 * response backlog that the peer is waiting on. */
                if (token_yielded != 0 ||
                    (context->options.proxy_test_packets != 0u &&
                     state.packets_sent >=
                         context->options.proxy_test_packets) ||
                    (priority >= LIVE_PROXY_PRIORITY_CONTROL &&
                     continue_response_burst == 0)) {
                    break;
                }
            }
            if (token_yielded == 0) {
                status = yield_proxy_token(context, &state);
                if (status != UM_OK) {
                    return status;
                }
            }
            have_token = 0;
        } else {
            int peer_completed = 0;
            status = receive_until_proxy_token(context, &state,
                                               &peer_completed);
            if (status != UM_OK) {
                return status;
            }
            if (peer_completed != 0) {
                live_log(context,
                         "state=COMPLETE bidirectional network proxy test "
                         "passed packets=%zu each direction",
                         context->options.proxy_test_packets);
                return UM_OK;
            }
            have_token = 1;
        }
    }
    return UM_ERR_INTERRUPTED;
}

static int client_proxy_session(live_context *context)
{
    uint8_t begin[LIVE_PROXY_BEGIN_BYTES];
    um_live_wire_message acknowledgement;
    unsigned attempt;
    int status = ensure_network(context);
    if (status != UM_OK) {
        return status;
    }
    context->link_stage_started = 1;
    begin[0] = UM_LIVE_PROXY_FORMAT_VERSION;
    write_u16(&begin[1], (uint16_t)UM_NETWORK_MTU);
    write_u16(&begin[3],
              (uint16_t)context->client_to_gateway_body_bytes);
    write_u16(&begin[5],
              (uint16_t)context->gateway_to_client_body_bytes);
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        status = send_wire(context, proxy_transmit_config(context),
                           UM_WIRE_PROXY_BEGIN, 0u, begin, sizeof(begin), 1,
                           NULL);
        if (status != UM_OK) {
            return status;
        }
        status = receive_expected(context, proxy_receive_config(context),
                                  UM_WIRE_ACK, 0u,
                                  LIVE_PROXY_ACK_TIMEOUT_MS,
                                  &acknowledgement, NULL);
        if (status == UM_OK) {
            break;
        }
        if (status == UM_ERR_INTERRUPTED) {
            return status;
        }
        live_log(context, "proxy start retry=%u/%u (%s)", attempt + 1u,
                 context->options.retry_limit, um_status_string(status));
    }
    if (status != UM_OK) {
        return UM_ERR_TIMEOUT;
    }
    return run_proxy_loop(context);
}

static int gateway_proxy_session(live_context *context)
{
    um_live_wire_message begin;
    int status;
    context->link_stage_started = 1;
    status = receive_expected(context, proxy_receive_config(context),
                              UM_WIRE_PROXY_BEGIN, 0u, 15000u, &begin, NULL);
    if (status != UM_OK) {
        return status;
    }
    if (begin.body_length != LIVE_PROXY_BEGIN_BYTES ||
        begin.body[0] != UM_LIVE_PROXY_FORMAT_VERSION ||
        read_u16(&begin.body[1]) != UM_NETWORK_MTU) {
        return UM_ERR_HEADER;
    }
    if (read_u16(&begin.body[3]) !=
            context->client_to_gateway_body_bytes ||
        read_u16(&begin.body[5]) !=
            context->gateway_to_client_body_bytes) {
        return UM_ERR_HEADER;
    }
    status = ensure_network(context);
    if (status != UM_OK) {
        return status;
    }
    sleep_milliseconds(LIVE_TURNAROUND_MS);
    status = send_wire(context, proxy_transmit_config(context), UM_WIRE_ACK,
                       0u, NULL, 0u, 1, NULL);
    if (status != UM_OK) {
        return status;
    }
    return run_proxy_loop(context);
}

static int client_session(live_context *context)
{
    um_live_wire_message message;
    uint8_t begin[10];
    size_t link_test_chunk_bytes;
    uint32_t forward_seed;
    uint32_t reverse_seed;
    unsigned attempt;
    int status;
    context->session_id = make_session_id();
    status = client_connect(context);
    if (status != UM_OK) {
        return status;
    }
    status = exchange_calibration_caches(context);
    if (status != UM_OK) {
        return status;
    }
    if (context->have_client_to_gateway != 0) {
        log_cached_config(context, "state=CALIBRATION_SKIPPED source=peer-cache",
                          0u, &context->client_to_gateway,
                          context->client_to_gateway_body_bytes);
    } else {
        status = calibration_sender(context, 0u,
                                    &context->client_to_gateway,
                                    &context->client_to_gateway_body_bytes);
        if (status != UM_OK) {
            return status;
        }
        context->have_client_to_gateway = 1;
    }
    if (context->have_gateway_to_client != 0) {
        log_cached_config(context,
                          "state=CALIBRATION_SKIPPED source=local-cache", 1u,
                          &context->gateway_to_client,
                          context->gateway_to_client_body_bytes);
    } else {
        status = receive_control(context, UM_WIRE_CALIB_BEGIN, 1u, 3500u,
                                 &message);
        if (status != UM_OK) {
            return status;
        }
        status = calibration_receiver(context, 1u, &message,
                                      &context->gateway_to_client,
                                      &context->gateway_to_client_body_bytes);
        if (status != UM_OK) {
            return status;
        }
        save_local_calibration(context, 1u,
                               &context->gateway_to_client,
                               context->gateway_to_client_body_bytes);
    }
    if (context->options.link_test == 0) {
        return client_proxy_session(context);
    }
    context->link_stage_started = 1;
    link_test_chunk_bytes = context->options.chunk_bytes;
    if (context->client_to_gateway_body_bytes < link_test_chunk_bytes) {
        link_test_chunk_bytes = context->client_to_gateway_body_bytes;
    }
    if (context->gateway_to_client_body_bytes < link_test_chunk_bytes) {
        link_test_chunk_bytes = context->gateway_to_client_body_bytes;
    }
    forward_seed = context->session_id ^ UINT32_C(0xc2a70001);
    reverse_seed = context->session_id ^ UINT32_C(0x6a2c0002);
    write_u32(begin, (uint32_t)context->options.test_bytes);
    write_u16(&begin[4], (uint16_t)link_test_chunk_bytes);
    write_u32(&begin[6], forward_seed);
    status = send_control(context, UM_WIRE_TEST_BEGIN, 0u, begin,
                          sizeof(begin));
    if (status != UM_OK) {
        return status;
    }
    status = receive_control(context, UM_WIRE_ACK, 0u, 2200u, &message);
    if (status != UM_OK) {
        return status;
    }
    live_log(context, "state=TEST_TRANSFER direction=client->gateway");
    status = send_test_data(context, &context->client_to_gateway,
                            &context->gateway_to_client,
                            context->options.test_bytes,
                            link_test_chunk_bytes, forward_seed,
                            "client->gateway");
    if (status != UM_OK) {
        return status;
    }
    status = send_control(context, UM_WIRE_TURN, 0u, NULL, 0u);
    if (status != UM_OK) {
        return status;
    }
    status = receive_control(context, UM_WIRE_ACK, 0u, 2200u, &message);
    if (status != UM_OK) {
        return status;
    }
    live_log(context, "state=TEST_TRANSFER direction=gateway->client");
    status = receive_test_data(context, &context->gateway_to_client,
                               &context->client_to_gateway,
                               context->options.test_bytes,
                               link_test_chunk_bytes, reverse_seed,
                               "gateway->client");
    if (status != UM_OK) {
        return status;
    }
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        status = receive_control(context, UM_WIRE_COMPLETE, 0u, 3000u,
                                 &message);
        if (status == UM_OK) {
            break;
        }
        live_log(context, "Waiting for COMPLETE retry=%u/%u (%s)",
                 attempt + 1u, context->options.retry_limit,
                 um_status_string(status));
    }
    if (status != UM_OK) {
        return status;
    }
    status = send_control(context, UM_WIRE_ACK, 0u, NULL, 0u);
    if (status != UM_OK) {
        return status;
    }
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        status = receive_control(context, UM_WIRE_COMPLETE, 0u, 3200u,
                                 &message);
        if (status == UM_ERR_TIMEOUT) {
            status = UM_OK;
            break;
        }
        if (status != UM_OK) {
            if (fatal_live_status(status)) {
                return status;
            }
            continue;
        }
        live_log(context, "Duplicate COMPLETE received; repeating final ACK");
        status = send_control(context, UM_WIRE_ACK, 0u, NULL, 0u);
        if (status != UM_OK) {
            return status;
        }
    }
    if (status == UM_OK) {
        live_log(context,
                 "state=COMPLETE bidirectional real-audio test passed bytes=%zu "
                 "each direction",
                 context->options.test_bytes);
    }
    return status;
}

static int gateway_session(live_context *context)
{
    um_live_wire_message message;
    size_t test_bytes;
    size_t chunk_bytes;
    uint32_t forward_seed;
    uint32_t reverse_seed;
    unsigned attempt;
    int status = gateway_connect(context);
    if (status != UM_OK) {
        return status;
    }
    status = exchange_calibration_caches(context);
    if (status != UM_OK) {
        return status;
    }
    if (context->have_client_to_gateway != 0) {
        log_cached_config(context,
                          "state=CALIBRATION_SKIPPED source=local-cache", 0u,
                          &context->client_to_gateway,
                          context->client_to_gateway_body_bytes);
    } else {
        status = receive_control(context, UM_WIRE_CALIB_BEGIN, 0u, 3500u,
                                 &message);
        if (status != UM_OK) {
            return status;
        }
        status = calibration_receiver(context, 0u, &message,
                                      &context->client_to_gateway,
                                      &context->client_to_gateway_body_bytes);
        if (status != UM_OK) {
            return status;
        }
        save_local_calibration(context, 0u,
                               &context->client_to_gateway,
                               context->client_to_gateway_body_bytes);
    }
    if (context->have_gateway_to_client != 0) {
        log_cached_config(context, "state=CALIBRATION_SKIPPED source=peer-cache",
                          1u, &context->gateway_to_client,
                          context->gateway_to_client_body_bytes);
    } else {
        status = calibration_sender(context, 1u,
                                    &context->gateway_to_client,
                                    &context->gateway_to_client_body_bytes);
        if (status != UM_OK) {
            return status;
        }
        context->have_gateway_to_client = 1;
    }
    if (context->options.link_test == 0) {
        return gateway_proxy_session(context);
    }
    context->link_stage_started = 1;
    status = receive_control(context, UM_WIRE_TEST_BEGIN, 0u, 3500u,
                             &message);
    if (status != UM_OK || message.body_length != 10u) {
        return status != UM_OK ? status : UM_ERR_HEADER;
    }
    test_bytes = read_u32(message.body);
    chunk_bytes = read_u16(&message.body[4]);
    forward_seed = read_u32(&message.body[6]);
    if (test_bytes == 0u || chunk_bytes == 0u ||
        chunk_bytes > UM_LIVE_MAX_BODY) {
        return UM_ERR_HEADER;
    }
    reverse_seed = context->session_id ^ UINT32_C(0x6a2c0002);
    live_log(context,
             "Test parameters accepted from client bytes=%zu chunk=%zu",
             test_bytes, chunk_bytes);
    status = send_control(context, UM_WIRE_ACK, 0u, NULL, 0u);
    if (status != UM_OK) {
        return status;
    }
    live_log(context, "state=TEST_TRANSFER direction=client->gateway");
    status = receive_test_data(context, &context->client_to_gateway,
                               &context->gateway_to_client,
                               test_bytes, chunk_bytes, forward_seed,
                               "client->gateway");
    if (status != UM_OK) {
        return status;
    }
    status = receive_control(context, UM_WIRE_TURN, 0u, 3000u, &message);
    if (status != UM_OK) {
        return status;
    }
    status = send_control(context, UM_WIRE_ACK, 0u, NULL, 0u);
    if (status != UM_OK) {
        return status;
    }
    live_log(context, "state=TEST_TRANSFER direction=gateway->client");
    status = send_test_data(context, &context->gateway_to_client,
                            &context->client_to_gateway, test_bytes,
                            chunk_bytes, reverse_seed, "gateway->client");
    if (status != UM_OK) {
        return status;
    }
    for (attempt = 0u; attempt < context->options.retry_limit; ++attempt) {
        status = send_control(context, UM_WIRE_COMPLETE, 0u, NULL, 0u);
        if (status != UM_OK) {
            return status;
        }
        status = receive_control(context, UM_WIRE_ACK, 0u, 2500u, &message);
        if (status == UM_OK) {
            break;
        }
        live_log(context, "COMPLETE acknowledgement retry=%u/%u (%s)",
                 attempt + 1u, context->options.retry_limit,
                 um_status_string(status));
    }
    if (status == UM_OK) {
        live_log(context,
                 "state=COMPLETE bidirectional real-audio test passed bytes=%zu "
                 "each direction",
                 test_bytes);
    }
    return status;
}

static int fatal_live_status(int status)
{
    return status == UM_ERR_ARGUMENT || status == UM_ERR_MEMORY ||
           status == UM_ERR_AUDIO || status == UM_ERR_CONFIG ||
           status == UM_ERR_UNSUPPORTED || status == UM_ERR_CAPACITY ||
           status == UM_ERR_NETWORK;
}

um_live_audio_options um_live_audio_default_options(um_live_role role)
{
    um_live_audio_options options;
    options.role = role;
    options.input_device = "default";
    options.output_device = "default";
    options.link_test = 0;
    options.test_bytes = 1024u;
    options.chunk_bytes = UM_LIVE_MAX_BODY;
    options.retry_limit = 4u;
    options.discovery_interval_seconds = 2.0f;
    options.calibrate_high_quality = 0;
    options.calibration_path = "calibration.config";
    options.proxy_test_packets = 0u;
    return options;
}

int um_run_live_audio(const um_live_audio_options *options,
                      um_log_callback logger, void *logger_context)
{
    live_context context;
    um_modem_config bootstrap;
    um_modem_config data_default;
    size_t data_default_active_carriers;
    size_t data_default_data_carriers;
    size_t data_default_pilot_carriers;
    size_t calibration_probe_budget;
    void (*previous_sigint)(int) = SIG_DFL;
    void (*previous_sigterm)(int) = SIG_DFL;
    int status;
    if (options == NULL || logger == NULL ||
        (options->role != UM_LIVE_GATEWAY &&
         options->role != UM_LIVE_CLIENT) ||
        (options->link_test != 0 &&
         (options->test_bytes == 0u || options->test_bytes > UINT32_MAX)) ||
        options->chunk_bytes == 0u ||
        options->chunk_bytes > UM_LIVE_MAX_BODY ||
        (options->link_test == 0 &&
         options->chunk_bytes < LIVE_CALIB_BODY_BASE_BYTES) ||
        options->chunk_bytes > UINT16_MAX || options->retry_limit == 0u ||
        options->discovery_interval_seconds <= 0.0f) {
        return UM_ERR_ARGUMENT;
    }
    memset(&context, 0, sizeof(context));
    context.options = *options;
    context.logger = logger;
    context.logger_context = logger_context;
    context.receive_samples =
        (float *)malloc(LIVE_RX_CAPACITY * sizeof(*context.receive_samples));
    if (context.receive_samples == NULL) {
        return UM_ERR_MEMORY;
    }
    status = um_audio_list_devices(logger, logger_context);
    if (status != UM_OK) {
        free(context.receive_samples);
        return status;
    }
    live_interrupted = 0;
    previous_sigint = signal(SIGINT, handle_signal);
    previous_sigterm = signal(SIGTERM, handle_signal);
    live_log(&context,
             options->link_test != 0
                 ? "Opening real audio; link-test mode will not change "
                   "TUN/utun, routes, DNS, forwarding, or firewall state"
                 : "Opening real audio; network proxy mode will configure "
                   "TUN/utun after connection and calibration");
    status = um_audio_open(&context.audio, options->input_device,
                           options->output_device, logger, logger_context);
    if (status != UM_OK) {
        free(context.receive_samples);
        return status;
    }
    bootstrap = live_bootstrap_config();
    data_default = um_modem_default_config();
    data_default_active_carriers =
        (size_t)(data_default.last_bin - data_default.first_bin) + 1u;
    data_default_data_carriers = um_modem_data_carriers(&data_default);
    data_default_pilot_carriers =
        data_default_active_carriers - data_default_data_carriers;
    calibration_probe_budget = um_calibration_search_budget(
        options->calibrate_high_quality);
    live_log(&context,
             "Live protocol=%u config-format=%u proxy-format=%u "
             "handshake-bytes=%u role=%s mode=%s test-bytes=%zu "
             "chunk-bytes=%zu retries=%u control-turnaround=%ums",
             UM_LIVE_PROTOCOL_VERSION, UM_LIVE_CONFIG_FORMAT_VERSION,
             UM_LIVE_PROXY_FORMAT_VERSION, UM_LIVE_HANDSHAKE_BYTES,
             options->role == UM_LIVE_CLIENT ? "client" : "gateway",
             options->link_test != 0 ? "link-test" : "network-proxy",
             options->test_bytes, options->chunk_bytes,
             options->retry_limit, LIVE_TURNAROUND_MS);
    live_log(&context,
             "Bootstrap qam=%u fec=%s cp=%u window=%u repeats=%u training=%u "
             "sync=%.1fms band=%.0f-%.0fHz",
             1u << bootstrap.qam_bits, fec_name(bootstrap.fec_rate),
             bootstrap.cyclic_prefix, bootstrap.window_samples,
             bootstrap.symbol_repetitions, bootstrap.training_symbols,
             1000.0 * (double)bootstrap.sync_samples / UM_SAMPLE_RATE,
             (double)bootstrap.first_bin * UM_SAMPLE_RATE /
                 bootstrap.fft_size,
             (double)bootstrap.last_bin * UM_SAMPLE_RATE /
                 bootstrap.fft_size);
    live_log(&context,
             "Grounded data default qam=%u fec=%s cp=%.1fms window=%u "
             "repeats=%u training=%u sync=%.1fms band=%.0f-%.0fHz "
             "active-carriers=%zu data-carriers=%zu pilots=%zu "
             "spacing=%.4fHz",
             1u << data_default.qam_bits, fec_name(data_default.fec_rate),
             1000.0 * (double)data_default.cyclic_prefix / UM_SAMPLE_RATE,
             data_default.window_samples, data_default.symbol_repetitions,
             data_default.training_symbols,
             1000.0 * (double)data_default.sync_samples / UM_SAMPLE_RATE,
             (double)data_default.first_bin * UM_SAMPLE_RATE /
                 data_default.fft_size,
             (double)data_default.last_bin * UM_SAMPLE_RATE /
                 data_default.fft_size,
             data_default_active_carriers, data_default_data_carriers,
             data_default_pilot_carriers,
             (double)UM_SAMPLE_RATE / data_default.fft_size);
    live_log(&context,
             "Local calibration mode=%s adaptive-max-probes=%zu "
             "probe-bytes=%u verification-trials=%u "
             "body-range=%u-%u body-trials=%u "
             "robust-recovery-repeats=2-%u receiver-driven "
             "all-pass-primary-estimate=%.1fs per direction",
             options->calibrate_high_quality != 0 ? "high" : "default",
             calibration_probe_budget, UM_CALIBRATION_PROBE_BYTES,
             live_verification_trials(options->calibrate_high_quality),
             UM_LIVE_MIN_BODY, UM_LIVE_MAX_BODY,
             live_body_verification_trials(
                 options->calibrate_high_quality),
             UM_MAX_SYMBOL_REPETITIONS,
             live_calibration_primary_seconds(
                 options->calibrate_high_quality));
    load_local_calibration(&context);
    while (!live_interrupted) {
        context.link_stage_started = 0;
        status = options->role == UM_LIVE_CLIENT ? client_session(&context)
                                                 : gateway_session(&context);
        if (status == UM_OK || fatal_live_status(status)) {
            break;
        }
        live_log(&context,
                 "state=RECONNECTING link stage failed: %s; returning to %s",
                 um_status_string(status),
                 options->role == UM_LIVE_CLIENT ? "discovery" : "listening");
        if (context.link_stage_started != 0) {
            context.have_client_to_gateway = 0;
            context.have_gateway_to_client = 0;
            live_log(&context,
                     "Discarding cached in-memory modes after a link-stage "
                     "failure; the next connection will recalibrate both "
                     "directions");
        }
        (void)um_audio_flush_capture(context.audio);
        sleep_milliseconds(1000u);
    }
    if (live_interrupted) {
        status = UM_ERR_INTERRUPTED;
    }
    um_network_close(context.network);
    um_audio_close(context.audio);
    free(context.receive_samples);
    if (previous_sigint != SIG_ERR) {
        (void)signal(SIGINT, previous_sigint);
    }
    if (previous_sigterm != SIG_ERR) {
        (void)signal(SIGTERM, previous_sigterm);
    }
    return status;
}
