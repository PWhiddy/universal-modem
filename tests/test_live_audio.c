#define _POSIX_C_SOURCE 200809L

#include "um.h"
#include "../src/audio.h"
#include "../src/network.h"
#include "../src/um_internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { SIM_CLIENT = 0, SIM_GATEWAY = 1, SIM_ENDPOINTS = 2 };

#define SIM_REQUEST_BYTES UM_NETWORK_MTU
#define SIM_RESPONSE_BYTES 413u
#define SIM_BACKGROUND_BYTES 64u
#define SIM_BACKGROUND_PACKETS 16u
#define SIM_PROXY_PACKETS 5u

/* CoreAudio capture callbacks can resume after AudioQueueStart returns. */
#define SIM_CAPTURE_START_MS 60u

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    float *queued[SIM_ENDPOINTS];
    size_t queued_count[SIM_ENDPOINTS];
    size_t queued_capacity[SIM_ENDPOINTS];
    int opened[SIM_ENDPOINTS];
    int capture_enabled[SIM_ENDPOINTS];
    um_channel_config channel[SIM_ENDPOINTS];
    uint32_t transmission;
    int verify_drop_endpoint;
    int verify_drop_armed;
    unsigned verification_drops;
    int baseline_drop_endpoint;
    int baseline_drop_armed;
    unsigned baseline_drops;
    int proxy_drop_endpoint;
    int proxy_drop_armed;
    unsigned proxy_drops;
    int begin_ack_drop_armed;
    unsigned begin_ack_drops;
    int turn_ack_drop_armed;
    unsigned turn_ack_drops;
    int commit_drop_armed;
    unsigned commit_drops;
    int network_opened[SIM_ENDPOINTS];
    int network_input_ready[SIM_ENDPOINTS];
    unsigned network_background_remaining[SIM_ENDPOINTS];
    unsigned network_targets_read[SIM_ENDPOINTS];
    unsigned network_reads[SIM_ENDPOINTS];
    unsigned network_writes[SIM_ENDPOINTS];
    unsigned network_matches[SIM_ENDPOINTS];
    int runners_done;
} simulated_bus;

static simulated_bus bus = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
    .verify_drop_endpoint = -1,
    .baseline_drop_endpoint = -1,
    .proxy_drop_endpoint = -1
};

struct um_audio {
    int endpoint;
};

struct um_network {
    int endpoint;
    char interface_name[24];
};

typedef struct {
    const char *name;
    um_live_audio_options options;
    int status;
    size_t log_count;
    char logs[256][512];
    unsigned connected;
    unsigned calibrating;
    unsigned robust_passes;
    unsigned recovery_probes;
    unsigned cache_exchanges;
    unsigned cache_skips;
    unsigned cache_loads;
    unsigned cache_saves;
    unsigned calibration_selections;
    unsigned baseline_selections;
    unsigned adaptive_upgrades;
    unsigned verification_fallbacks;
    unsigned forward_transfer;
    unsigned reverse_transfer;
    unsigned completed;
    unsigned network_ready;
    unsigned proxying;
    unsigned proxy_completed;
    unsigned proxy_start_retries;
    unsigned proxy_retries;
    unsigned token_retries;
    unsigned commit_retries;
    unsigned reconnecting;
} runner;

static void make_ipv4_packet(uint8_t *packet, size_t length, uint8_t seed,
                             uint16_t source_port,
                             uint16_t destination_port)
{
    size_t index;
    memset(packet, 0, length);
    packet[0] = 0x45u;
    packet[2] = (uint8_t)(length >> 8u);
    packet[3] = (uint8_t)length;
    packet[8] = 64u;
    packet[9] = 17u;
    packet[12] = 10u;
    packet[15] = 2u;
    packet[16] = 1u;
    packet[17] = 1u;
    packet[18] = 1u;
    packet[19] = 1u;
    packet[20] = (uint8_t)(source_port >> 8u);
    packet[21] = (uint8_t)source_port;
    packet[22] = (uint8_t)(destination_port >> 8u);
    packet[23] = (uint8_t)destination_port;
    packet[24] = (uint8_t)((length - 20u) >> 8u);
    packet[25] = (uint8_t)(length - 20u);
    for (index = 28u; index < length; ++index) {
        packet[index] = (uint8_t)(seed + index * 29u);
    }
}

static void make_ipv4_tcp_packet(uint8_t *packet, size_t length,
                                 uint8_t seed, uint16_t source_port,
                                 uint16_t destination_port)
{
    size_t index;
    memset(packet, 0, length);
    packet[0] = 0x45u;
    packet[2] = (uint8_t)(length >> 8u);
    packet[3] = (uint8_t)length;
    packet[8] = 64u;
    packet[9] = 6u;
    packet[12] = 10u;
    packet[15] = 2u;
    packet[16] = 1u;
    packet[17] = 1u;
    packet[18] = 1u;
    packet[19] = 1u;
    packet[20] = (uint8_t)(source_port >> 8u);
    packet[21] = (uint8_t)source_port;
    packet[22] = (uint8_t)(destination_port >> 8u);
    packet[23] = (uint8_t)destination_port;
    packet[32] = 0x50u;
    packet[33] = 0x18u;
    for (index = 40u; index < length; ++index) {
        packet[index] = (uint8_t)(seed + index * 31u);
    }
}

static void make_proxy_target(uint8_t *packet, size_t length, int endpoint,
                              unsigned target_index)
{
    if (target_index == 0u) {
        make_ipv4_packet(packet, length,
                         endpoint == SIM_CLIENT ? 0x31u : 0xa7u,
                         endpoint == SIM_CLIENT ? 53000u : 53u,
                         endpoint == SIM_CLIENT ? 53u : 53000u);
    } else {
        make_ipv4_tcp_packet(
            packet, length,
            (uint8_t)((endpoint == SIM_CLIENT ? 0x50u : 0xb0u) +
                      target_index),
            endpoint == SIM_CLIENT ? 50000u : 443u,
            endpoint == SIM_CLIENT ? 443u : 50000u);
    }
}

static void add_milliseconds(struct timespec *time, unsigned milliseconds)
{
    time->tv_sec += (time_t)(milliseconds / 1000u);
    time->tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
    if (time->tv_nsec >= 1000000000L) {
        ++time->tv_sec;
        time->tv_nsec -= 1000000000L;
    }
}

static void test_log(void *context, const char *message)
{
    runner *run = (runner *)context;
    if (run->log_count < sizeof(run->logs) / sizeof(run->logs[0])) {
        (void)snprintf(run->logs[run->log_count],
                       sizeof(run->logs[run->log_count]), "%s", message);
        ++run->log_count;
    }
    if (strstr(message, "state=CONNECTED handshake complete") != NULL) {
        ++run->connected;
    }
    if (strstr(message, "state=CALIBRATING direction=") != NULL) {
        ++run->calibrating;
    }
    if (strstr(message, "gate=crc+sync/snr") != NULL) {
        ++run->robust_passes;
    }
    if (strstr(message, "step=more-repetitions-recovery") != NULL) {
        ++run->recovery_probes;
    }
    if (strstr(message, "Calibration cache exchange") != NULL) {
        ++run->cache_exchanges;
    }
    if (strstr(message, "state=CALIBRATION_SKIPPED") != NULL) {
        ++run->cache_skips;
    }
    if (strstr(message, "Loaded calibration cache") != NULL) {
        ++run->cache_loads;
    }
    if (strstr(message, "Saved calibration cache") != NULL) {
        ++run->cache_saves;
    }
    if (strstr(message, "calib selected") != NULL) {
        ++run->calibration_selections;
        if (strstr(message, "id=0") != NULL) {
            ++run->baseline_selections;
        } else {
            ++run->adaptive_upgrades;
        }
    }
    if (strstr(message,
               "FAIL; trying safer ranked candidate") != NULL) {
        ++run->verification_fallbacks;
    }
    if (strstr(message, "calib verify rank=1 id=") != NULL) {
        int endpoint = run->options.role == UM_LIVE_CLIENT
                           ? SIM_CLIENT
                           : SIM_GATEWAY;
        (void)pthread_mutex_lock(&bus.mutex);
        if (bus.verify_drop_endpoint == endpoint &&
            bus.verification_drops == 0u) {
            bus.verify_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (run->options.role == UM_LIVE_GATEWAY &&
        strstr(message,
               "state=CALIBRATING direction=client->gateway") != NULL) {
        (void)pthread_mutex_lock(&bus.mutex);
        if (bus.baseline_drop_endpoint == SIM_CLIENT &&
            bus.baseline_drops == 0u) {
            bus.baseline_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (strstr(message,
               "state=TEST_TRANSFER direction=client->gateway") != NULL) {
        ++run->forward_transfer;
    }
    if (strstr(message,
               "state=TEST_TRANSFER direction=gateway->client") != NULL) {
        ++run->reverse_transfer;
    }
    if (strstr(message,
               "state=COMPLETE bidirectional real-audio test passed") !=
        NULL) {
        ++run->completed;
    }
    if (strstr(message, "state=NETWORK_READY") != NULL) {
        ++run->network_ready;
        (void)pthread_mutex_lock(&bus.mutex);
        if (run->options.role == UM_LIVE_GATEWAY &&
            bus.proxy_drop_endpoint >= 0 && bus.begin_ack_drops == 0u) {
            bus.begin_ack_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (strstr(message, "state=PROXYING") != NULL) {
        ++run->proxying;
        (void)pthread_mutex_lock(&bus.mutex);
        if (run->options.role == UM_LIVE_CLIENT &&
            bus.proxy_drop_endpoint >= 0 && bus.proxy_drops == 0u) {
            bus.proxy_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (strstr(message,
               "state=COMPLETE bidirectional network proxy test passed") !=
        NULL) {
        ++run->proxy_completed;
    }
    if (strstr(message, "proxy ") != NULL &&
        strstr(message, " cumulative-ack retry=") != NULL) {
        ++run->proxy_retries;
    }
    if (run->options.role == UM_LIVE_GATEWAY &&
        strstr(message, "proxy client->gateway packet=1 ") != NULL) {
        (void)pthread_mutex_lock(&bus.mutex);
        if (bus.proxy_drop_endpoint >= 0 && bus.proxy_drops == 1u) {
            bus.proxy_drop_endpoint = SIM_GATEWAY;
            bus.proxy_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (strstr(message, "proxy start retry=") != NULL) {
        ++run->proxy_start_retries;
    }
    if (strstr(message, "proxy token handoff retry=") != NULL) {
        ++run->token_retries;
    }
    if (strstr(message, "proxy token commit wait retry=") != NULL) {
        ++run->commit_retries;
    }
    if (run->options.role == UM_LIVE_GATEWAY &&
        strstr(message, "proxy token accept sequence=") != NULL) {
        (void)pthread_mutex_lock(&bus.mutex);
        if (bus.proxy_drop_endpoint >= 0 && bus.turn_ack_drops == 0u) {
            bus.turn_ack_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (run->options.role == UM_LIVE_CLIENT &&
        strstr(message, "proxy token commit sequence=") != NULL) {
        (void)pthread_mutex_lock(&bus.mutex);
        if (bus.proxy_drop_endpoint >= 0 && bus.commit_drops == 0u) {
            bus.commit_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (strstr(message, "state=RECONNECTING") != NULL) {
        ++run->reconnecting;
    }
}

int um_audio_list_devices(um_log_callback logger, void *logger_context)
{
    if (logger == NULL) {
        return UM_ERR_ARGUMENT;
    }
    logger(logger_context,
           "Audio backend: paired simulated audio, format: 48000 Hz mono "
           "signed-16");
    logger(logger_context, "Audio input devices:");
    logger(logger_context, "  sim-client | simulated client microphone");
    logger(logger_context, "  sim-gateway | simulated gateway microphone");
    logger(logger_context, "Audio output devices:");
    logger(logger_context, "  sim-client | simulated client speaker");
    logger(logger_context, "  sim-gateway | simulated gateway speaker");
    return UM_OK;
}

int um_audio_open(um_audio **audio, const char *input_device,
                  const char *output_device, um_log_callback logger,
                  void *logger_context)
{
    um_audio *opened;
    int endpoint;
    if (audio == NULL || input_device == NULL || output_device == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (strcmp(input_device, "sim-client") == 0 &&
        strcmp(output_device, "sim-client") == 0) {
        endpoint = SIM_CLIENT;
    } else if (strcmp(input_device, "sim-gateway") == 0 &&
               strcmp(output_device, "sim-gateway") == 0) {
        endpoint = SIM_GATEWAY;
    } else {
        return UM_ERR_ARGUMENT;
    }
    opened = (um_audio *)malloc(sizeof(*opened));
    if (opened == NULL) {
        return UM_ERR_MEMORY;
    }
    opened->endpoint = endpoint;
    (void)pthread_mutex_lock(&bus.mutex);
    if (bus.opened[endpoint] != 0) {
        (void)pthread_mutex_unlock(&bus.mutex);
        free(opened);
        return UM_ERR_AUDIO;
    }
    bus.opened[endpoint] = 1;
    bus.capture_enabled[endpoint] = 1;
    bus.queued_count[endpoint] = 0u;
    (void)pthread_cond_broadcast(&bus.changed);
    (void)pthread_mutex_unlock(&bus.mutex);
    if (logger != NULL) {
        logger(logger_context,
               endpoint == SIM_CLIENT
                   ? "Opened paired simulated client audio"
                   : "Opened paired simulated gateway audio");
    }
    *audio = opened;
    return UM_OK;
}

void um_audio_close(um_audio *audio)
{
    if (audio == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&bus.mutex);
    bus.opened[audio->endpoint] = 0;
    bus.capture_enabled[audio->endpoint] = 0;
    bus.queued_count[audio->endpoint] = 0u;
    (void)pthread_cond_broadcast(&bus.changed);
    (void)pthread_mutex_unlock(&bus.mutex);
    free(audio);
}

int um_audio_capture_enable(um_audio *audio, int enabled)
{
    if (audio == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (enabled != 0) {
        struct timespec start_delay = {
            (time_t)(SIM_CAPTURE_START_MS / 1000u),
            (long)(SIM_CAPTURE_START_MS % 1000u) * 1000000L
        };
        while (nanosleep(&start_delay, &start_delay) != 0 && errno == EINTR) {
        }
    }
    (void)pthread_mutex_lock(&bus.mutex);
    bus.capture_enabled[audio->endpoint] = enabled != 0;
    if (enabled == 0) {
        bus.queued_count[audio->endpoint] = 0u;
    }
    (void)pthread_cond_broadcast(&bus.changed);
    (void)pthread_mutex_unlock(&bus.mutex);
    return UM_OK;
}

int um_audio_flush_capture(um_audio *audio)
{
    if (audio == NULL) {
        return UM_ERR_ARGUMENT;
    }
    (void)pthread_mutex_lock(&bus.mutex);
    bus.queued_count[audio->endpoint] = 0u;
    (void)pthread_mutex_unlock(&bus.mutex);
    return UM_OK;
}

int um_audio_read(um_audio *audio, float *samples, size_t capacity,
                  unsigned timeout_ms, size_t *frames_read)
{
    struct timespec deadline;
    int wait_status = 0;
    size_t count;
    if (audio == NULL || samples == NULL || capacity == 0u ||
        frames_read == NULL) {
        return UM_ERR_ARGUMENT;
    }
    *frames_read = 0u;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    add_milliseconds(&deadline, timeout_ms);
    (void)pthread_mutex_lock(&bus.mutex);
    while (bus.queued_count[audio->endpoint] == 0u &&
           bus.capture_enabled[audio->endpoint] != 0 &&
           bus.opened[audio->endpoint] != 0 && wait_status != ETIMEDOUT) {
        wait_status = pthread_cond_timedwait(&bus.changed, &bus.mutex,
                                             &deadline);
    }
    if (bus.capture_enabled[audio->endpoint] == 0 ||
        bus.opened[audio->endpoint] == 0) {
        (void)pthread_mutex_unlock(&bus.mutex);
        return UM_ERR_AUDIO;
    }
    if (bus.queued_count[audio->endpoint] == 0u) {
        (void)pthread_mutex_unlock(&bus.mutex);
        return UM_ERR_TIMEOUT;
    }
    count = bus.queued_count[audio->endpoint] < capacity
                ? bus.queued_count[audio->endpoint]
                : capacity;
    memcpy(samples, bus.queued[audio->endpoint], count * sizeof(*samples));
    bus.queued_count[audio->endpoint] -= count;
    if (bus.queued_count[audio->endpoint] != 0u) {
        memmove(bus.queued[audio->endpoint],
                bus.queued[audio->endpoint] + count,
                bus.queued_count[audio->endpoint] * sizeof(*samples));
    }
    (void)pthread_mutex_unlock(&bus.mutex);
    *frames_read = count;
    return UM_OK;
}

int um_audio_write(um_audio *audio, const float *samples, size_t frame_count)
{
    um_channel_config channel;
    float *received = NULL;
    size_t received_count = 0u;
    size_t needed;
    int peer;
    int drop_write = 0;
    int status;
    if (audio == NULL || (frame_count != 0u && samples == NULL)) {
        return UM_ERR_ARGUMENT;
    }
    peer = audio->endpoint == SIM_CLIENT ? SIM_GATEWAY : SIM_CLIENT;
    (void)pthread_mutex_lock(&bus.mutex);
    channel = bus.channel[audio->endpoint];
    channel.random_seed ^=
        ++bus.transmission * UINT32_C(0x9e3779b9);
    if (bus.verify_drop_armed != 0 &&
        bus.verify_drop_endpoint == audio->endpoint) {
        bus.verify_drop_armed = 0;
        ++bus.verification_drops;
        drop_write = 1;
    }
    if (bus.baseline_drop_armed != 0 &&
        bus.baseline_drop_endpoint == audio->endpoint) {
        bus.baseline_drop_armed = 0;
        ++bus.baseline_drops;
        drop_write = 1;
    }
    if (bus.proxy_drop_armed != 0 &&
        bus.proxy_drop_endpoint == audio->endpoint) {
        bus.proxy_drop_armed = 0;
        ++bus.proxy_drops;
        drop_write = 1;
    }
    if (bus.begin_ack_drop_armed != 0 && audio->endpoint == SIM_GATEWAY) {
        bus.begin_ack_drop_armed = 0;
        ++bus.begin_ack_drops;
        drop_write = 1;
    }
    if (bus.turn_ack_drop_armed != 0 && audio->endpoint == SIM_GATEWAY) {
        bus.turn_ack_drop_armed = 0;
        ++bus.turn_ack_drops;
        drop_write = 1;
    }
    if (bus.commit_drop_armed != 0 && audio->endpoint == SIM_CLIENT) {
        bus.commit_drop_armed = 0;
        ++bus.commit_drops;
        drop_write = 1;
    }
    (void)pthread_mutex_unlock(&bus.mutex);
    if (drop_write != 0) {
        return UM_OK;
    }
    status = um_channel_apply(samples, frame_count, &channel, &received,
                              &received_count);
    if (status != UM_OK) {
        return status;
    }
    (void)pthread_mutex_lock(&bus.mutex);
    if (bus.opened[peer] != 0 && bus.capture_enabled[peer] != 0) {
        if (received_count > SIZE_MAX - bus.queued_count[peer]) {
            status = UM_ERR_CAPACITY;
        } else {
            needed = bus.queued_count[peer] + received_count;
            if (needed > bus.queued_capacity[peer]) {
                size_t capacity = bus.queued_capacity[peer] != 0u
                                      ? bus.queued_capacity[peer]
                                      : 4096u;
                float *larger;
                while (capacity < needed && capacity <= SIZE_MAX / 2u) {
                    capacity *= 2u;
                }
                if (capacity < needed ||
                    capacity > SIZE_MAX / sizeof(*larger)) {
                    status = UM_ERR_CAPACITY;
                } else {
                    larger = (float *)realloc(bus.queued[peer],
                                              capacity * sizeof(*larger));
                    if (larger == NULL) {
                        status = UM_ERR_MEMORY;
                    } else {
                        bus.queued[peer] = larger;
                        bus.queued_capacity[peer] = capacity;
                    }
                }
            }
            if (status == UM_OK) {
                memcpy(bus.queued[peer] + bus.queued_count[peer], received,
                       received_count * sizeof(*received));
                bus.queued_count[peer] += received_count;
                (void)pthread_cond_broadcast(&bus.changed);
            }
        }
    }
    (void)pthread_mutex_unlock(&bus.mutex);
    free(received);
    return status;
}

int um_network_open(um_network **network, um_live_role role,
                    um_log_callback logger, void *logger_context)
{
    um_network *opened;
    int endpoint;
    if (network == NULL ||
        (role != UM_LIVE_CLIENT && role != UM_LIVE_GATEWAY)) {
        return UM_ERR_ARGUMENT;
    }
    endpoint = role == UM_LIVE_CLIENT ? SIM_CLIENT : SIM_GATEWAY;
    opened = (um_network *)malloc(sizeof(*opened));
    if (opened == NULL) {
        return UM_ERR_MEMORY;
    }
    opened->endpoint = endpoint;
    (void)snprintf(opened->interface_name, sizeof(opened->interface_name),
                   "sim-tun-%s", endpoint == SIM_CLIENT ? "client"
                                                        : "gateway");
    (void)pthread_mutex_lock(&bus.mutex);
    if (bus.network_opened[endpoint] != 0) {
        (void)pthread_mutex_unlock(&bus.mutex);
        free(opened);
        return UM_ERR_NETWORK;
    }
    bus.network_opened[endpoint] = 1;
    bus.network_background_remaining[endpoint] = SIM_BACKGROUND_PACKETS;
    if (endpoint == SIM_CLIENT) {
        bus.network_input_ready[endpoint] = 1;
    }
    (void)pthread_cond_broadcast(&bus.changed);
    (void)pthread_mutex_unlock(&bus.mutex);
    if (logger != NULL) {
        logger(logger_context,
               endpoint == SIM_CLIENT
                   ? "Opened independent simulated client TUN"
                   : "Opened independent simulated gateway TUN");
    }
    *network = opened;
    return UM_OK;
}

void um_network_close(um_network *network)
{
    if (network == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&bus.mutex);
    bus.network_opened[network->endpoint] = 0;
    bus.network_input_ready[network->endpoint] = 0;
    bus.network_background_remaining[network->endpoint] = 0u;
    (void)pthread_cond_broadcast(&bus.changed);
    (void)pthread_mutex_unlock(&bus.mutex);
    free(network);
}

int um_network_read(um_network *network, uint8_t *packet, size_t capacity,
                    unsigned timeout_ms, size_t *packet_length)
{
    struct timespec deadline;
    uint8_t generated[UM_NETWORK_MTU];
    size_t length;
    unsigned background_number = 0u;
    unsigned target_index = 0u;
    int background = 0;
    int wait_status = 0;
    if (network == NULL || packet == NULL || packet_length == NULL) {
        return UM_ERR_ARGUMENT;
    }
    *packet_length = 0u;
    if (capacity < UM_NETWORK_MTU) {
        return UM_ERR_CAPACITY;
    }
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    add_milliseconds(&deadline, timeout_ms);
    (void)pthread_mutex_lock(&bus.mutex);
    while (bus.network_input_ready[network->endpoint] == 0 &&
           bus.network_background_remaining[network->endpoint] == 0u &&
           bus.network_opened[network->endpoint] != 0 &&
           wait_status != ETIMEDOUT && timeout_ms != 0u) {
        wait_status = pthread_cond_timedwait(&bus.changed, &bus.mutex,
                                             &deadline);
    }
    if (bus.network_opened[network->endpoint] == 0) {
        (void)pthread_mutex_unlock(&bus.mutex);
        return UM_ERR_NETWORK;
    }
    if (bus.network_input_ready[network->endpoint] == 0 &&
        bus.network_background_remaining[network->endpoint] == 0u) {
        (void)pthread_mutex_unlock(&bus.mutex);
        return UM_ERR_TIMEOUT;
    }
    if (bus.network_background_remaining[network->endpoint] != 0u) {
        background = 1;
        background_number =
            bus.network_background_remaining[network->endpoint];
        --bus.network_background_remaining[network->endpoint];
    } else {
        --bus.network_input_ready[network->endpoint];
        target_index = bus.network_targets_read[network->endpoint]++;
    }
    ++bus.network_reads[network->endpoint];
    (void)pthread_mutex_unlock(&bus.mutex);

    if (background != 0) {
        length = SIM_BACKGROUND_BYTES;
        make_ipv4_packet(
            generated, length,
            (uint8_t)(0x40u + background_number +
                      (unsigned)network->endpoint * 0x30u),
            (uint16_t)(40000u + background_number), 443u);
    } else {
        length = network->endpoint == SIM_CLIENT ? SIM_REQUEST_BYTES
                                                 : SIM_RESPONSE_BYTES;
        make_proxy_target(generated, length, network->endpoint,
                          target_index);
    }
    memcpy(packet, generated, length);
    *packet_length = length;
    return UM_OK;
}

int um_network_write(um_network *network, const uint8_t *packet,
                     size_t packet_length, unsigned timeout_ms)
{
    uint8_t expected[UM_NETWORK_MTU];
    size_t expected_length;
    unsigned target_index;
    int matches;
    (void)timeout_ms;
    if (network == NULL || packet == NULL) {
        return UM_ERR_ARGUMENT;
    }
    expected_length = network->endpoint == SIM_GATEWAY
                          ? SIM_REQUEST_BYTES
                          : SIM_RESPONSE_BYTES;
    (void)pthread_mutex_lock(&bus.mutex);
    if (bus.network_opened[network->endpoint] == 0) {
        (void)pthread_mutex_unlock(&bus.mutex);
        return UM_ERR_NETWORK;
    }
    target_index = bus.network_writes[network->endpoint]++;
    make_proxy_target(expected, expected_length,
                      network->endpoint == SIM_GATEWAY ? SIM_CLIENT
                                                       : SIM_GATEWAY,
                      target_index);
    matches = packet_length == expected_length &&
              memcmp(packet, expected, expected_length) == 0;
    if (matches != 0) {
        ++bus.network_matches[network->endpoint];
        if (network->endpoint == SIM_GATEWAY) {
            ++bus.network_input_ready[SIM_GATEWAY];
            if (target_index == 0u) {
                bus.network_input_ready[SIM_CLIENT] +=
                    SIM_PROXY_PACKETS - 1u;
            }
        }
        (void)pthread_cond_broadcast(&bus.changed);
    }
    (void)pthread_mutex_unlock(&bus.mutex);
    return matches != 0 ? UM_OK : UM_ERR_NETWORK;
}

const char *um_network_interface_name(const um_network *network)
{
    return network != NULL ? network->interface_name : NULL;
}

static void *run_endpoint(void *argument)
{
    runner *run = (runner *)argument;
    run->status = um_run_live_audio(&run->options, test_log, run);
    (void)pthread_mutex_lock(&bus.mutex);
    ++bus.runners_done;
    (void)pthread_cond_broadcast(&bus.changed);
    (void)pthread_mutex_unlock(&bus.mutex);
    return NULL;
}

static void print_logs(const runner *run)
{
    size_t i;
    fprintf(stderr, "%s simulated live log:\n", run->name);
    for (i = 0u; i < run->log_count; ++i) {
        fprintf(stderr, "  %s\n", run->logs[i]);
    }
}

static int check_runner(const runner *run, unsigned expected_calibrations,
                        int link_test)
{
    int common = run->status == UM_OK && run->connected == 1u &&
                 run->calibrating == expected_calibrations &&
                 run->calibration_selections == expected_calibrations &&
                 run->cache_exchanges == 1u &&
                 run->cache_skips == 2u - expected_calibrations &&
                 run->reconnecting == 0u;
    if (link_test != 0) {
        return common && run->forward_transfer == 1u &&
               run->reverse_transfer == 1u && run->completed == 1u &&
               run->network_ready == 0u && run->proxying == 0u &&
               run->proxy_completed == 0u;
    }
    return common && run->forward_transfer == 0u &&
           run->reverse_transfer == 0u && run->completed == 0u &&
           run->network_ready == 1u && run->proxying == 1u &&
           run->proxy_completed == 1u;
}

static void configure_runner(runner *run, const char *name,
                             um_live_role role, const char *device,
                             const char *calibration_path, int link_test)
{
    memset(run, 0, sizeof(*run));
    run->name = name;
    run->options = um_live_audio_default_options(role);
    run->options.input_device = device;
    run->options.output_device = device;
    run->options.link_test = link_test;
    run->options.proxy_test_packets =
        link_test == 0 ? SIM_PROXY_PACKETS : 0u;
    run->options.test_bytes = 256u;
    run->options.chunk_bytes = link_test != 0 ? 64u : 128u;
    run->options.retry_limit = 5u;
    run->options.discovery_interval_seconds = 0.4f;
    run->options.calibration_path = calibration_path;
}

static int run_pair(const char *label,
                    const um_channel_config *client_to_gateway,
                    const um_channel_config *gateway_to_client,
                    int link_test, int inject_proxy_retry,
                    int require_upgrade, int require_baseline,
                    int inject_verification_fallback,
                    int inject_baseline_recovery,
                    unsigned expected_calibrations,
                    const char *client_calibration_path,
                    const char *gateway_calibration_path)
{
    runner client;
    runner gateway;
    pthread_t client_thread;
    pthread_t gateway_thread;
    struct timespec deadline;
    int timed_out = 0;
    int status;

    configure_runner(&client, "client", UM_LIVE_CLIENT, "sim-client",
                     client_calibration_path, link_test);
    configure_runner(&gateway, "gateway", UM_LIVE_GATEWAY, "sim-gateway",
                     gateway_calibration_path, link_test);

    (void)pthread_mutex_lock(&bus.mutex);
    bus.channel[SIM_CLIENT] = *client_to_gateway;
    bus.channel[SIM_GATEWAY] = *gateway_to_client;
    bus.queued_count[SIM_CLIENT] = 0u;
    bus.queued_count[SIM_GATEWAY] = 0u;
    bus.opened[SIM_CLIENT] = 0;
    bus.opened[SIM_GATEWAY] = 0;
    bus.capture_enabled[SIM_CLIENT] = 0;
    bus.capture_enabled[SIM_GATEWAY] = 0;
    bus.transmission = 0u;
    bus.verify_drop_endpoint = inject_verification_fallback != 0
                                   ? SIM_CLIENT
                                   : -1;
    bus.verify_drop_armed = 0;
    bus.verification_drops = 0u;
    bus.baseline_drop_endpoint = inject_baseline_recovery != 0
                                     ? SIM_CLIENT
                                     : -1;
    bus.baseline_drop_armed = 0;
    bus.baseline_drops = 0u;
    bus.proxy_drop_endpoint = inject_proxy_retry != 0
                                  ? SIM_CLIENT
                                  : -1;
    bus.proxy_drop_armed = 0;
    bus.proxy_drops = 0u;
    bus.begin_ack_drop_armed = 0;
    bus.begin_ack_drops = 0u;
    bus.turn_ack_drop_armed = 0;
    bus.turn_ack_drops = 0u;
    bus.commit_drop_armed = 0;
    bus.commit_drops = 0u;
    memset(bus.network_opened, 0, sizeof(bus.network_opened));
    memset(bus.network_input_ready, 0, sizeof(bus.network_input_ready));
    memset(bus.network_background_remaining, 0,
           sizeof(bus.network_background_remaining));
    memset(bus.network_targets_read, 0, sizeof(bus.network_targets_read));
    memset(bus.network_reads, 0, sizeof(bus.network_reads));
    memset(bus.network_writes, 0, sizeof(bus.network_writes));
    memset(bus.network_matches, 0, sizeof(bus.network_matches));
    bus.runners_done = 0;
    (void)pthread_mutex_unlock(&bus.mutex);

    status = pthread_create(&gateway_thread, NULL, run_endpoint, &gateway);
    if (status != 0) {
        fprintf(stderr, "failed to create gateway thread: %s\n",
                strerror(status));
        return 1;
    }
    (void)pthread_mutex_lock(&bus.mutex);
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    add_milliseconds(&deadline, 2000u);
    while (bus.opened[SIM_GATEWAY] == 0) {
        status = pthread_cond_timedwait(&bus.changed, &bus.mutex, &deadline);
        if (status == ETIMEDOUT) {
            break;
        }
    }
    (void)pthread_mutex_unlock(&bus.mutex);
    status = pthread_create(&client_thread, NULL, run_endpoint, &client);
    if (status != 0) {
        (void)pthread_cancel(gateway_thread);
        (void)pthread_join(gateway_thread, NULL);
        fprintf(stderr, "failed to create client thread: %s\n",
                strerror(status));
        return 1;
    }

    (void)pthread_mutex_lock(&bus.mutex);
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    add_milliseconds(&deadline, link_test != 0 ? 60000u : 180000u);
    while (bus.runners_done < 2) {
        status = pthread_cond_timedwait(&bus.changed, &bus.mutex, &deadline);
        if (status == ETIMEDOUT) {
            timed_out = 1;
            break;
        }
    }
    (void)pthread_mutex_unlock(&bus.mutex);
    if (timed_out != 0) {
        (void)pthread_cancel(client_thread);
        (void)pthread_cancel(gateway_thread);
    }
    (void)pthread_join(client_thread, NULL);
    (void)pthread_join(gateway_thread, NULL);

    if (timed_out != 0 ||
        !check_runner(&client, expected_calibrations, link_test) ||
        !check_runner(&gateway, expected_calibrations, link_test) ||
        (expected_calibrations != 0u &&
         (client.robust_passes + gateway.robust_passes <
              expected_calibrations)) ||
        (require_upgrade != 0 &&
         client.adaptive_upgrades + gateway.adaptive_upgrades == 0u) ||
        (require_baseline != 0 &&
         client.baseline_selections + gateway.baseline_selections == 0u) ||
        (inject_verification_fallback != 0 &&
         (bus.verification_drops != 1u ||
          client.verification_fallbacks + gateway.verification_fallbacks ==
              0u)) ||
        (inject_baseline_recovery != 0 &&
         (bus.baseline_drops != 1u ||
          client.recovery_probes + gateway.recovery_probes == 0u)) ||
        (link_test == 0 &&
         (bus.network_opened[SIM_CLIENT] != 0 ||
          bus.network_opened[SIM_GATEWAY] != 0 ||
          bus.network_reads[SIM_CLIENT] !=
              SIM_BACKGROUND_PACKETS + SIM_PROXY_PACKETS ||
          bus.network_reads[SIM_GATEWAY] !=
              SIM_BACKGROUND_PACKETS + SIM_PROXY_PACKETS ||
          bus.network_writes[SIM_CLIENT] != SIM_PROXY_PACKETS ||
          bus.network_writes[SIM_GATEWAY] != SIM_PROXY_PACKETS ||
          bus.network_matches[SIM_CLIENT] != SIM_PROXY_PACKETS ||
          bus.network_matches[SIM_GATEWAY] != SIM_PROXY_PACKETS)) ||
        (inject_proxy_retry != 0 &&
         (bus.proxy_drops != 2u || bus.begin_ack_drops != 1u ||
          bus.turn_ack_drops != 1u ||
          bus.commit_drops != 1u ||
          client.proxy_start_retries + gateway.proxy_start_retries == 0u ||
          client.proxy_retries + gateway.proxy_retries == 0u ||
          client.token_retries + gateway.token_retries == 0u ||
          client.commit_retries + gateway.commit_retries == 0u))) {
        fprintf(stderr,
                "%s paired live simulation failed timeout=%d client=%s "
                "gateway=%s\n",
                label, timed_out, um_status_string(client.status),
                um_status_string(gateway.status));
        print_logs(&client);
        print_logs(&gateway);
        return 1;
    }
    printf("paired live %s passed: connection, adaptive 2-way calibration, "
           "delayed capture restart, %s; upgrades=%u/%u "
           "fallbacks=%u/%u verification-stepdowns=%u calibrations=%u "
           "cache-skips=%u recovery-probes=%u\n",
           label,
           link_test != 0 ? "256+256-byte explicit link test"
                          : "prioritized DNS exchange plus four-packet TCP "
                            "burst through saturated background queues",
           client.adaptive_upgrades, gateway.adaptive_upgrades,
           client.baseline_selections, gateway.baseline_selections,
           client.verification_fallbacks + gateway.verification_fallbacks,
           expected_calibrations, client.cache_skips + gateway.cache_skips,
           client.recovery_probes + gateway.recovery_probes);
    return 0;
}

int main(void)
{
    um_channel_config v2_forward = um_channel_recorded_v2_config(0u);
    um_channel_config v2_reverse = um_channel_recorded_v2_config(1u);
    um_distortion_profile strong;
    um_modem_config cached;
    char client_path[160];
    char gateway_path[160];
    int found;
    int status;

    (void)snprintf(client_path, sizeof(client_path),
                   "/tmp/universal-modem-live-client-%ld.config",
                   (long)getpid());
    (void)snprintf(gateway_path, sizeof(gateway_path),
                   "/tmp/universal-modem-live-gateway-%ld.config",
                   (long)getpid());
    (void)remove(client_path);
    (void)remove(gateway_path);

    status = run_pair("v2-worse-network-proxy", &v2_forward, &v2_reverse,
                      0, 1, 0, 0, 1, 0, 2u, client_path, gateway_path);
    if (status == 0) {
        found = 0;
        if (um_calibration_config_load(client_path, UM_LIVE_CLIENT, &cached,
                                       &found) != UM_OK ||
            found == 0) {
            fprintf(stderr, "client calibration cache was not saved\n");
            status = 1;
        }
    }
    if (status == 0) {
        found = 0;
        if (um_calibration_config_load(gateway_path, UM_LIVE_GATEWAY, &cached,
                                       &found) != UM_OK ||
            found == 0) {
            fprintf(stderr, "gateway calibration cache was not saved\n");
            status = 1;
        }
    }
    if (status == 0) {
        status = run_pair("both-cached-link-test", &v2_forward, &v2_reverse,
                          1, 0, 0, 0, 0, 0, 0u, client_path,
                          gateway_path);
    }
    if (status == 0) {
        (void)remove(gateway_path);
        status = run_pair("one-side-missing-recovery", &v2_forward,
                          &v2_reverse, 1, 0, 0, 0, 0, 1, 1u,
                          client_path, gateway_path);
    }
    if (status == 0) {
        if (um_distortion_profile_get(5u, &strong) != UM_OK) {
            fprintf(stderr, "could not load baseline-fallback distortion\n");
            status = 1;
        } else {
            (void)remove(client_path);
            (void)remove(gateway_path);
            status = run_pair("baseline-fallback", &strong.client_to_gateway,
                              &strong.gateway_to_client, 1, 0, 0, 1, 0, 0,
                              2u, client_path, gateway_path);
        }
    }

    (void)remove(client_path);
    (void)remove(gateway_path);
    free(bus.queued[SIM_CLIENT]);
    free(bus.queued[SIM_GATEWAY]);
    return status;
}
