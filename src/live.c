#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "live_wire.h"
#include "um_internal.h"

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
#define LIVE_VERIFY_TRIALS 3u
#define LIVE_CALIBRATION_RANKS 5u

typedef struct {
    um_live_audio_options options;
    um_audio *audio;
    um_log_callback logger;
    void *logger_context;
    uint32_t session_id;
    um_modem_config client_to_gateway;
    um_modem_config gateway_to_client;
    float *receive_samples;
} live_context;

typedef struct {
    size_t index;
    float score;
} live_ranked_candidate;

static volatile sig_atomic_t live_interrupted = 0;

static int fatal_live_status(int status);

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
    um_modem_config config = um_modem_default_config();
    config.first_bin = 64u;
    config.last_bin = 448u;
    config.cyclic_prefix = 1024u;
    config.window_samples = 96u;
    config.sync_samples = UM_MAX_SYNC_SAMPLES;
    config.sync_gap = 3072u;
    config.training_symbols = UM_MAX_TRAINING_SYMBOLS;
    config.qam_bits = 2u;
    config.fec_rate = UM_FEC_RATE_1_2;
    return config;
}

static um_modem_config live_calibration_control_config(void)
{
    um_modem_config config = live_bootstrap_config();
    config.cyclic_prefix = 768u;
    config.window_samples = 64u;
    config.sync_samples = UM_SYNC_SAMPLES;
    config.sync_gap = 1792u;
    config.training_symbols = 2u;
    return config;
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
    uint16_t attempt = 0u;
    while (!live_interrupted) {
        um_live_wire_message message;
        um_rx_metrics metrics;
        int status;
        uint64_t started = monotonic_milliseconds();
        live_log(context, "state=DISCOVERING tx=DISCOVER attempt=%u session=%08x",
                 (unsigned)attempt + 1u, context->session_id);
        status = send_wire(context, &bootstrap, UM_WIRE_DISCOVER, attempt,
                           NULL, 0u, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        status = receive_expected(context, &bootstrap, UM_WIRE_OFFER, attempt,
                                  2200u, &message, &metrics);
        if (status == UM_OK) {
            log_received_quality(context, "rx=OFFER", &metrics);
            sleep_milliseconds(LIVE_TURNAROUND_MS);
            live_log(context, "state=NEGOTIATING rx=OFFER tx=CONFIRM");
            status = send_wire(context, &bootstrap, UM_WIRE_CONFIRM, attempt,
                               NULL, 0u, 1, NULL);
            if (status != UM_OK) {
                return status;
            }
            status = receive_expected(context, &bootstrap, UM_WIRE_CONNECTED,
                                      attempt, 2500u, &message, &metrics);
            if (status == UM_OK) {
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
        log_received_quality(context, "rx=DISCOVER", &metrics);
        context->session_id = message.session_id;
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        live_log(context, "state=NEGOTIATING rx=DISCOVER tx=OFFER session=%08x",
                 context->session_id);
        status = send_wire(context, &bootstrap, UM_WIRE_OFFER,
                           message.sequence, NULL, 0u, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        status = receive_expected(context, &bootstrap, UM_WIRE_CONFIRM,
                                  message.sequence, 2600u, &message, &metrics);
        if (status != UM_OK) {
            live_log(context, "CONFIRM not received (%s); listening again",
                     um_status_string(status));
            continue;
        }
        log_received_quality(context, "rx=CONFIRM", &metrics);
        sleep_milliseconds(LIVE_TURNAROUND_MS);
        status = send_wire(context, &bootstrap, UM_WIRE_CONNECTED,
                           message.sequence, NULL, 0u, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        live_log(context, "state=CONNECTED handshake complete");
        return UM_OK;
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

static float live_candidate_score(const um_modem_config *config,
                                  const um_rx_metrics *metrics)
{
    float rate = config->fec_rate == UM_FEC_RATE_1_2
                     ? 0.5f
                     : config->fec_rate == UM_FEC_RATE_2_3 ? 2.0f / 3.0f
                                                            : 0.75f;
    float raw = (float)um_modem_data_carriers(config) *
                (float)config->qam_bits * rate * (float)UM_SAMPLE_RATE /
                (float)(config->fft_size + config->cyclic_prefix);
    float quality = 1.0f /
                    (1.0f + 4.0f * metrics->evm_rms * metrics->evm_rms);
    if (config->window_samples == 0u) {
        quality *= 0.94f;
    }
    return raw * quality;
}

static void live_rank_candidate(live_ranked_candidate *ranked,
                                size_t *rank_count, size_t candidate,
                                float score)
{
    size_t position = 0u;
    size_t move;
    while (position < *rank_count && ranked[position].score >= score) {
        ++position;
    }
    if (position >= LIVE_CALIBRATION_RANKS) {
        return;
    }
    move = *rank_count < LIVE_CALIBRATION_RANKS
               ? *rank_count
               : LIVE_CALIBRATION_RANKS - 1u;
    while (move > position) {
        ranked[move] = ranked[move - 1u];
        --move;
    }
    ranked[position].index = candidate;
    ranked[position].score = score;
    if (*rank_count < LIVE_CALIBRATION_RANKS) {
        ++*rank_count;
    }
}

static double live_calibration_primary_seconds(int high_quality)
{
    um_modem_config calibration_control =
        live_calibration_control_config();
    size_t candidate_count =
        um_live_calibration_candidate_count(high_quality);
    float *samples = NULL;
    uint8_t ready_wire[UM_LIVE_WIRE_HEADER_SIZE] = {0u};
    size_t sample_count = 0u;
    double total_samples = 0.0;
    size_t candidate;

    if (um_modulate_frame(&calibration_control, ready_wire,
                          sizeof(ready_wire), 0u,
                          &samples,
                          &sample_count) != UM_OK) {
        return 0.0;
    }
    total_samples += (double)candidate_count *
                     (double)(sample_count + LIVE_TX_PRE_SAMPLES +
                              LIVE_TX_POST_SAMPLES);
    free(samples);
    for (candidate = 0u; candidate < candidate_count; ++candidate) {
        um_modem_config config;
        uint8_t probe[UM_LIVE_WIRE_HEADER_SIZE + 16u] = {0u};
        samples = NULL;
        if (um_live_calibration_candidate_get(high_quality, candidate,
                                              &config) != UM_OK ||
            um_modulate_frame(&config, probe, sizeof(probe),
                              (uint16_t)candidate, &samples,
                              &sample_count) != UM_OK) {
            free(samples);
            return 0.0;
        }
        total_samples += sample_count + LIVE_TX_PRE_SAMPLES +
                         LIVE_TX_POST_SAMPLES;
        free(samples);
    }
    return total_samples / (double)UM_SAMPLE_RATE +
           (double)candidate_count *
               (double)LIVE_CALIBRATION_SETTLE_MS / 1000.0;
}

static int calibration_sender(live_context *context, unsigned direction,
                              um_modem_config *selected)
{
    um_modem_config bootstrap = live_bootstrap_config();
    um_modem_config calibration_control =
        live_calibration_control_config();
    size_t candidate_count = um_live_calibration_candidate_count(
        context->options.calibrate_high_quality);
    uint8_t begin[4];
    um_live_wire_message message;
    size_t candidate;
    int status;
    if (candidate_count == 0u || candidate_count > UINT16_MAX) {
        return UM_ERR_CONFIG;
    }
    begin[0] = (uint8_t)direction;
    begin[1] = (uint8_t)(context->options.calibrate_high_quality != 0);
    write_u16(&begin[2], (uint16_t)candidate_count);
    live_log(context, "state=CALIBRATING direction=%s candidates=%zu",
             direction == 0u ? "client->gateway" : "gateway->client",
             candidate_count);
    sleep_milliseconds(LIVE_TURNAROUND_MS);
    status = send_wire(context, &bootstrap, UM_WIRE_CALIB_BEGIN,
                       (uint16_t)direction, begin, sizeof(begin), 1, NULL);
    if (status != UM_OK) {
        return status;
    }
    for (candidate = 0u; candidate < candidate_count && !live_interrupted;
         ++candidate) {
        um_modem_config config;
        uint8_t probe[16];
        float duration = 0.0f;
        status = receive_expected(context, &calibration_control,
                                  UM_WIRE_CALIB_READY, (uint16_t)candidate,
                                  3500u, &message, NULL);
        if (status != UM_OK) {
            break;
        }
        status = um_live_calibration_candidate_get(
            context->options.calibrate_high_quality, candidate, &config);
        if (status != UM_OK) {
            break;
        }
        fill_calibration_body(candidate, 0u, probe, sizeof(probe));
        status = send_wire(context, &config, UM_WIRE_CALIB_PROBE,
                           (uint16_t)candidate, probe, sizeof(probe), 1,
                           &duration);
        live_log(context,
                 "calib tx=%zu/%zu qam=%u fec=%s cp=%u window=%u "
                 "band=%.0f-%.0fHz duration=%.3fs",
                 candidate + 1u, candidate_count, 1u << config.qam_bits,
                 fec_name(config.fec_rate), config.cyclic_prefix,
                 config.window_samples,
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
    status = receive_expected(context, &bootstrap, UM_WIRE_CALIB_REPORT,
                              (uint16_t)direction, 4000u, &message, NULL);
    if (status != UM_OK || message.body_length < 4u) {
        return status != UM_OK ? status : UM_ERR_HEADER;
    }
    {
        size_t usable = read_u16(message.body);
        size_t rank_count = message.body[2];
        size_t rank;
        if (message.body[3] != 0u || rank_count == 0u ||
            rank_count > LIVE_CALIBRATION_RANKS ||
            message.body_length != 4u + rank_count * 2u) {
            return UM_ERR_HEADER;
        }
        live_log(context, "calib primary sweep usable=%zu ranked=%zu",
                 usable, rank_count);
        for (rank = 0u; rank < rank_count; ++rank) {
            size_t selected_candidate = read_u16(&message.body[4u + rank * 2u]);
            unsigned trial;
            int verified = 1;
            if (selected_candidate >= candidate_count ||
                um_live_calibration_candidate_get(
                    context->options.calibrate_high_quality,
                    selected_candidate, selected) != UM_OK) {
                return UM_ERR_HEADER;
            }
            live_log(context,
                     "calib verify rank=%zu index=%zu qam=%u fec=%s cp=%u "
                     "window=%u band=%.0f-%.0fHz",
                     rank + 1u, selected_candidate,
                     1u << selected->qam_bits, fec_name(selected->fec_rate),
                     selected->cyclic_prefix, selected->window_samples,
                     (double)selected->first_bin * UM_SAMPLE_RATE /
                         selected->fft_size,
                     (double)selected->last_bin * UM_SAMPLE_RATE /
                         selected->fft_size);
            for (trial = 0u; trial < LIVE_VERIFY_TRIALS; ++trial) {
                uint8_t verify[128];
                uint16_t verify_sequence =
                    (uint16_t)(rank * LIVE_VERIFY_TRIALS + trial);
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
                                           verify_sequence, 2500u, &message,
                                           NULL);
                if (status != UM_OK || message.body_length != 1u) {
                    return status != UM_OK ? status : UM_ERR_HEADER;
                }
                if (message.body[0] == 0u) {
                    verified = 0;
                    live_log(context,
                             "calib verify rank=%zu trial=%u/%u FAIL; "
                             "trying safer ranked candidate",
                             rank + 1u, trial + 1u, LIVE_VERIFY_TRIALS);
                    break;
                }
                live_log(context,
                         "calib verify rank=%zu trial=%u/%u PASS",
                         rank + 1u, trial + 1u, LIVE_VERIFY_TRIALS);
            }
            if (verified != 0) {
                live_log(context, "calib selected rank=%zu index=%zu",
                         rank + 1u, selected_candidate);
                return UM_OK;
            }
        }
    }
    return UM_ERR_CRC;
}

static int calibration_receiver(live_context *context, unsigned direction,
                                const um_live_wire_message *begin_message,
                                um_modem_config *selected)
{
    um_modem_config bootstrap = live_bootstrap_config();
    um_modem_config calibration_control =
        live_calibration_control_config();
    int high_quality;
    size_t expected_count;
    size_t candidate;
    size_t usable = 0u;
    live_ranked_candidate ranked[LIVE_CALIBRATION_RANKS];
    size_t rank_count = 0u;
    int status;
    if (begin_message->body_length != 4u ||
        begin_message->body[0] != direction || begin_message->body[1] > 1u) {
        return UM_ERR_HEADER;
    }
    high_quality = begin_message->body[1] != 0u;
    expected_count = um_live_calibration_candidate_count(high_quality);
    if (read_u16(&begin_message->body[2]) != expected_count) {
        return UM_ERR_HEADER;
    }
    live_log(context, "state=CALIBRATING direction=%s candidates=%zu",
             direction == 0u ? "client->gateway" : "gateway->client",
             expected_count);
    for (candidate = 0u; candidate < expected_count && !live_interrupted;
         ++candidate) {
        um_modem_config config;
        um_live_wire_message probe;
        um_rx_metrics metrics;
        uint8_t expected[16];
        status = um_live_calibration_candidate_get(
            high_quality, candidate, &config);
        if (status != UM_OK) {
            return status;
        }
        sleep_milliseconds(LIVE_CALIBRATION_SETTLE_MS);
        status = send_wire(context, &calibration_control,
                           UM_WIRE_CALIB_READY,
                           (uint16_t)candidate, NULL, 0u, 1, NULL);
        if (status != UM_OK) {
            return status;
        }
        memset(&metrics, 0, sizeof(metrics));
        status = receive_wire(context, &config, context->session_id, 3000u,
                              &probe, &metrics);
        fill_calibration_body(candidate, 0u, expected, sizeof(expected));
        if (status == UM_OK && probe.type == UM_WIRE_CALIB_PROBE &&
            probe.sequence == (uint16_t)candidate &&
            probe.body_length == sizeof(expected) &&
            memcmp(probe.body, expected, sizeof(expected)) == 0) {
            float score = live_candidate_score(&config, &metrics);
            ++usable;
            live_rank_candidate(ranked, &rank_count, candidate, score);
            live_log(context,
                     "calib rx=%zu/%zu PASS qam=%u fec=%s cp=%u window=%u "
                     "band=%.0f-%.0fHz level=%.1fdBFS norm=%.2fx "
                     "clip=%.3f%% snr=%.1fdB evm=%.3f score=%.0f",
                     candidate + 1u, expected_count, 1u << config.qam_bits,
                     fec_name(config.fec_rate), config.cyclic_prefix,
                     config.window_samples,
                     (double)config.first_bin * UM_SAMPLE_RATE /
                         config.fft_size,
                     (double)config.last_bin * UM_SAMPLE_RATE /
                         config.fft_size,
                     metrics.signal_rms > 1.0e-12f
                         ? 20.0 * log10((double)metrics.signal_rms)
                         : -240.0,
                     metrics.normalization_gain,
                     100.0 * (double)metrics.clipped_sample_fraction,
                     metrics.estimated_snr_db, metrics.evm_rms, score);
        } else {
            if (status == UM_OK) {
                status = UM_ERR_CRC;
            }
            live_log(context,
                     "calib rx=%zu/%zu FAIL qam=%u fec=%s cp=%u window=%u "
                     "band=%.0f-%.0fHz sync=%.3f level=%.1fdBFS "
                     "norm=%.2fx clip=%.3f%% reason=%s",
                     candidate + 1u, expected_count, 1u << config.qam_bits,
                     fec_name(config.fec_rate), config.cyclic_prefix,
                     config.window_samples,
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
    }
    if (live_interrupted) {
        return UM_ERR_INTERRUPTED;
    }
    {
        uint8_t report[4u + LIVE_CALIBRATION_RANKS * 2u];
        size_t rank;
        write_u16(report, (uint16_t)usable);
        report[2] = (uint8_t)rank_count;
        report[3] = 0u;
        for (rank = 0u; rank < rank_count; ++rank) {
            write_u16(&report[4u + rank * 2u],
                      (uint16_t)ranked[rank].index);
        }
        sleep_milliseconds(LIVE_CALIBRATION_SETTLE_MS);
        status = send_wire(context, &bootstrap, UM_WIRE_CALIB_REPORT,
                           (uint16_t)direction, report,
                           4u + rank_count * 2u, 1, NULL);
    }
    if (status != UM_OK || rank_count == 0u) {
        return status != UM_OK ? status : UM_ERR_CRC;
    }
    {
        size_t rank;
        for (rank = 0u; rank < rank_count; ++rank) {
            unsigned trial;
            int verified = 1;
            status = um_live_calibration_candidate_get(
                high_quality, ranked[rank].index, selected);
            if (status != UM_OK) {
                return status;
            }
            for (trial = 0u; trial < LIVE_VERIFY_TRIALS; ++trial) {
                um_live_wire_message verify;
                uint8_t expected[128];
                uint8_t result_body[1] = {0u};
                uint16_t verify_sequence =
                    (uint16_t)(rank * LIVE_VERIFY_TRIALS + trial);
                fill_calibration_body(ranked[rank].index, trial + 1u,
                                      expected, sizeof(expected));
                status = receive_wire(context, selected,
                                      context->session_id, 2600u, &verify,
                                      NULL);
                if (status == UM_OK &&
                    verify.type == UM_WIRE_CALIB_VERIFY &&
                    verify.sequence == verify_sequence &&
                    verify.body_length == sizeof(expected) &&
                    memcmp(verify.body, expected, sizeof(expected)) == 0) {
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
                    break;
                }
                live_log(context,
                         "calib verify rank=%zu trial=%u/%u PASS",
                         rank + 1u, trial + 1u, LIVE_VERIFY_TRIALS);
            }
            if (verified != 0) {
                live_log(context,
                         "calib selected rank=%zu index=%zu usable=%zu",
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
                          const um_modem_config *config, size_t total,
                          size_t chunk_size, uint32_t seed, const char *label)
{
    um_modem_config bootstrap = live_bootstrap_config();
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
            status = receive_expected(context, &bootstrap, UM_WIRE_ACK,
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
                             const um_modem_config *config, size_t total,
                             size_t chunk_size, uint32_t seed,
                             const char *label)
{
    um_modem_config bootstrap = live_bootstrap_config();
    size_t offset = 0u;
    uint16_t expected_sequence = 0u;
    unsigned misses = 0u;
    uint64_t started = monotonic_milliseconds();
    while (offset < total && !live_interrupted) {
        um_live_wire_message data;
        um_rx_metrics metrics;
        memset(&metrics, 0, sizeof(metrics));
        int status = receive_wire(context, config, context->session_id, 2600u,
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
        status = send_wire(context, &bootstrap, UM_WIRE_ACK, data.sequence,
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

static int client_session(live_context *context)
{
    um_live_wire_message message;
    uint8_t begin[10];
    uint32_t forward_seed;
    uint32_t reverse_seed;
    unsigned attempt;
    int status;
    context->session_id = make_session_id();
    status = client_connect(context);
    if (status != UM_OK) {
        return status;
    }
    status = calibration_sender(context, 0u,
                                &context->client_to_gateway);
    if (status != UM_OK) {
        return status;
    }
    status = receive_control(context, UM_WIRE_CALIB_BEGIN, 1u, 3500u,
                             &message);
    if (status != UM_OK) {
        return status;
    }
    status = calibration_receiver(context, 1u, &message,
                                  &context->gateway_to_client);
    if (status != UM_OK) {
        return status;
    }
    forward_seed = context->session_id ^ UINT32_C(0xc2a70001);
    reverse_seed = context->session_id ^ UINT32_C(0x6a2c0002);
    write_u32(begin, (uint32_t)context->options.test_bytes);
    write_u16(&begin[4], (uint16_t)context->options.chunk_bytes);
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
                            context->options.test_bytes,
                            context->options.chunk_bytes, forward_seed,
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
                               context->options.test_bytes,
                               context->options.chunk_bytes, reverse_seed,
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
    status = receive_control(context, UM_WIRE_CALIB_BEGIN, 0u, 3500u,
                             &message);
    if (status != UM_OK) {
        return status;
    }
    status = calibration_receiver(context, 0u, &message,
                                  &context->client_to_gateway);
    if (status != UM_OK) {
        return status;
    }
    status = calibration_sender(context, 1u,
                                &context->gateway_to_client);
    if (status != UM_OK) {
        return status;
    }
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
    status = send_test_data(context, &context->gateway_to_client, test_bytes,
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
           status == UM_ERR_UNSUPPORTED || status == UM_ERR_CAPACITY;
}

um_live_audio_options um_live_audio_default_options(um_live_role role)
{
    um_live_audio_options options;
    options.role = role;
    options.input_device = "default";
    options.output_device = "default";
    options.test_bytes = 1024u;
    options.chunk_bytes = 128u;
    options.retry_limit = 4u;
    options.discovery_interval_seconds = 2.0f;
    options.calibrate_high_quality = 0;
    return options;
}

int um_run_live_audio(const um_live_audio_options *options,
                      um_log_callback logger, void *logger_context)
{
    live_context context;
    um_modem_config bootstrap;
    size_t calibration_candidates;
    void (*previous_sigint)(int) = SIG_DFL;
    void (*previous_sigterm)(int) = SIG_DFL;
    int status;
    if (options == NULL || logger == NULL ||
        (options->role != UM_LIVE_GATEWAY &&
         options->role != UM_LIVE_CLIENT) ||
        options->test_bytes == 0u || options->test_bytes > UINT32_MAX ||
        options->chunk_bytes == 0u ||
        options->chunk_bytes > UM_LIVE_MAX_BODY ||
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
    live_log(&context, "Opening real audio; no TUN/utun or routes will be changed");
    status = um_audio_open(&context.audio, options->input_device,
                           options->output_device, logger, logger_context);
    if (status != UM_OK) {
        free(context.receive_samples);
        return status;
    }
    bootstrap = live_bootstrap_config();
    calibration_candidates = um_live_calibration_candidate_count(
        options->calibrate_high_quality);
    live_log(&context,
             "Live role=%s test-bytes=%zu chunk-bytes=%zu retries=%u",
             options->role == UM_LIVE_CLIENT ? "client" : "gateway",
             options->test_bytes, options->chunk_bytes,
             options->retry_limit);
    live_log(&context,
             "Bootstrap qam=%u fec=%s cp=%u window=%u band=%.0f-%.0fHz",
             1u << bootstrap.qam_bits, fec_name(bootstrap.fec_rate),
             bootstrap.cyclic_prefix, bootstrap.window_samples,
             (double)bootstrap.first_bin * UM_SAMPLE_RATE /
                 bootstrap.fft_size,
             (double)bootstrap.last_bin * UM_SAMPLE_RATE /
                 bootstrap.fft_size);
    live_log(&context,
             "Local calibration mode=%s candidates=%zu receiver-synchronized "
             "primary-sweep=%.1fs per direction",
             options->calibrate_high_quality != 0 ? "high" : "default",
             calibration_candidates,
             live_calibration_primary_seconds(
                 options->calibrate_high_quality));
    while (!live_interrupted) {
        status = options->role == UM_LIVE_CLIENT ? client_session(&context)
                                                 : gateway_session(&context);
        if (status == UM_OK || fatal_live_status(status)) {
            break;
        }
        live_log(&context,
                 "state=RECONNECTING link stage failed: %s; returning to %s",
                 um_status_string(status),
                 options->role == UM_LIVE_CLIENT ? "discovery" : "listening");
        (void)um_audio_flush_capture(context.audio);
        sleep_milliseconds(1000u);
    }
    if (live_interrupted) {
        status = UM_ERR_INTERRUPTED;
    }
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
