#define _POSIX_C_SOURCE 200809L

#include "um.h"
#include "../src/audio.h"
#include "../src/um_internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SIM_CLIENT = 0, SIM_GATEWAY = 1, SIM_ENDPOINTS = 2 };

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
    int runners_done;
} simulated_bus;

static simulated_bus bus = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    {NULL, NULL},
    {0u, 0u},
    {0u, 0u},
    {0, 0},
    {0, 0},
    {{0}, {0}},
    0u,
    -1,
    0,
    0u,
    0
};

struct um_audio {
    int endpoint;
};

typedef struct {
    const char *name;
    um_live_audio_options options;
    int status;
    size_t log_count;
    char logs[256][512];
    unsigned connected;
    unsigned calibrating;
    unsigned baseline_passes;
    unsigned grounded_defaults;
    unsigned calibration_selections;
    unsigned baseline_selections;
    unsigned adaptive_upgrades;
    unsigned verification_fallbacks;
    unsigned forward_transfer;
    unsigned reverse_transfer;
    unsigned completed;
    unsigned reconnecting;
} runner;

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
    if (strstr(message, "id=0 step=working-baseline PASS") != NULL) {
        ++run->baseline_passes;
    }
    if (strstr(message, "step=grounded-data-default") != NULL) {
        ++run->grounded_defaults;
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

static int check_runner(const runner *run, int require_upgrade,
                        int require_baseline)
{
    return run->status == UM_OK && run->connected == 1u &&
           run->calibrating == 2u && run->baseline_passes >= 1u &&
           run->grounded_defaults >= 2u &&
           run->calibration_selections == 2u &&
           (require_upgrade == 0 || run->adaptive_upgrades >= 1u) &&
           (require_baseline == 0 || run->baseline_selections >= 1u) &&
           run->forward_transfer == 1u &&
           run->reverse_transfer == 1u && run->completed == 1u &&
           run->reconnecting == 0u;
}

static void configure_runner(runner *run, const char *name,
                             um_live_role role, const char *device)
{
    memset(run, 0, sizeof(*run));
    run->name = name;
    run->options = um_live_audio_default_options(role);
    run->options.input_device = device;
    run->options.output_device = device;
    run->options.test_bytes = 256u;
    run->options.chunk_bytes = 64u;
    run->options.retry_limit = 5u;
    run->options.discovery_interval_seconds = 0.4f;
}

static int run_pair(const char *label,
                    const um_channel_config *client_to_gateway,
                    const um_channel_config *gateway_to_client,
                    int require_upgrade, int require_baseline,
                    int inject_verification_fallback)
{
    runner client;
    runner gateway;
    pthread_t client_thread;
    pthread_t gateway_thread;
    struct timespec deadline;
    int timed_out = 0;
    int status;

    configure_runner(&client, "client", UM_LIVE_CLIENT, "sim-client");
    configure_runner(&gateway, "gateway", UM_LIVE_GATEWAY, "sim-gateway");

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
    add_milliseconds(&deadline, 45000u);
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
        !check_runner(&client, require_upgrade, require_baseline) ||
        !check_runner(&gateway, require_upgrade, require_baseline) ||
        (inject_verification_fallback != 0 &&
         (bus.verification_drops != 1u ||
          client.verification_fallbacks == 0u))) {
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
           "delayed capture restart, 256+256 bytes; upgrades=%u/%u "
           "fallbacks=%u/%u verification-stepdowns=%u\n",
           label, client.adaptive_upgrades, gateway.adaptive_upgrades,
           client.baseline_selections, gateway.baseline_selections,
           client.verification_fallbacks + gateway.verification_fallbacks);
    return 0;
}

int main(void)
{
    um_channel_config v2_forward = um_channel_recorded_v2_config(0u);
    um_channel_config v2_reverse = um_channel_recorded_v2_config(1u);
    um_distortion_profile strong;
    int status;

    status = run_pair("v2-worse", &v2_forward, &v2_reverse, 1, 0, 1);
    if (status == 0) {
        if (um_distortion_profile_get(5u, &strong) != UM_OK) {
            fprintf(stderr, "could not load baseline-fallback distortion\n");
            status = 1;
        } else {
            status = run_pair("baseline-fallback", &strong.client_to_gateway,
                              &strong.gateway_to_client, 0, 1, 0);
        }
    }

    free(bus.queued[SIM_CLIENT]);
    free(bus.queued[SIM_GATEWAY]);
    return status;
}
