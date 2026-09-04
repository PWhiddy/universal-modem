#include "light_video.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIGHT_LIVE_MAX_TEST_BYTES (16u * 1024u * 1024u)
#define LIGHT_LIVE_MAX_PIXELS ((size_t)4096u * 4096u)

static void light_live_log(um_log_callback logger, void *context,
                           const char *format, ...)
{
    char message[512];
    va_list arguments;
    if (logger == NULL) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    logger(context, message);
}

static uint32_t light_live_seed(um_live_role role)
{
    struct timespec now;
    uint32_t seed = role == UM_LIVE_CLIENT ? UINT32_C(0x434c4945)
                                           : UINT32_C(0x47415445);
    if (timespec_get(&now, TIME_UTC) == TIME_UTC) {
        seed ^= (uint32_t)now.tv_sec;
        seed ^= (uint32_t)now.tv_nsec;
    }
    return seed != 0u ? seed : UINT32_C(1);
}

static void light_live_fill(uint8_t *bytes, size_t length, uint32_t seed)
{
    size_t index;
    uint32_t value = seed;
    for (index = 0u; index < length; ++index) {
        value ^= value << 13u;
        value ^= value >> 17u;
        value ^= value << 5u;
        bytes[index] = (uint8_t)value;
    }
}

um_live_light_options um_live_light_default_options(um_live_role role)
{
    um_live_light_options options;
    options.role = role;
    options.camera_device = "default";
    options.test_bytes = 1024u;
    options.window_size = 720u;
    options.completion_linger_frames = 30u;
    return options;
}

int um_run_live_light(const um_live_light_options *options,
                      um_log_callback logger, void *logger_context)
{
    um_light_video_config video_config = um_light_video_default_config();
    um_light_peer_config peer_config = um_light_peer_default_config();
    um_light_video *video = NULL;
    um_light_peer *peer = NULL;
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t *pixels = NULL;
    uint8_t *outgoing = NULL;
    uint8_t *incoming = NULL;
    uint8_t *expected = NULL;
    size_t local_frame = 0u;
    size_t decoded_frames = 0u;
    size_t decode_misses = 0u;
    size_t sync_misses = 0u;
    size_t header_misses = 0u;
    size_t crc_misses = 0u;
    size_t completion_frame = SIZE_MAX;
    um_light_rx_metrics last_metrics;
    time_t started_at;
    int status = UM_OK;

    if (options == NULL ||
        (options->role != UM_LIVE_CLIENT &&
         options->role != UM_LIVE_GATEWAY) ||
        options->camera_device == NULL || options->test_bytes == 0u ||
        options->test_bytes > LIGHT_LIVE_MAX_TEST_BYTES ||
        options->window_size < 256u || options->window_size > 4096u ||
        options->completion_linger_frames == 0u) {
        return UM_ERR_ARGUMENT;
    }
    memset(&last_metrics, 0, sizeof(last_metrics));

    pixels = (uint8_t *)malloc(LIGHT_LIVE_MAX_PIXELS);
    outgoing = (uint8_t *)malloc(options->test_bytes);
    incoming = (uint8_t *)calloc(options->test_bytes, 1u);
    expected = (uint8_t *)malloc(options->test_bytes);
    if (pixels == NULL || outgoing == NULL || incoming == NULL ||
        expected == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    light_live_fill(outgoing, options->test_bytes,
                    options->role == UM_LIVE_CLIENT
                        ? UINT32_C(0xc11e4701)
                        : UINT32_C(0x6a7e5a91));
    light_live_fill(expected, options->test_bytes,
                    options->role == UM_LIVE_CLIENT
                        ? UINT32_C(0x6a7e5a91)
                        : UINT32_C(0xc11e4701));
    peer_config.random_seed = light_live_seed(options->role);
    status = um_light_peer_create(
        &peer, options->role, &peer_config, outgoing, options->test_bytes,
        incoming, options->test_bytes, logger, logger_context);
    if (status != UM_OK) {
        goto done;
    }

    video_config.camera_device = options->camera_device;
    video_config.window_size = options->window_size;
    status = um_light_video_open(&video, &video_config, logger,
                                 logger_context);
    if (status != UM_OK) {
        goto done;
    }
    light_live_log(
        logger, logger_context,
        "Light link test role=%s bytes=%zu; discovery waits indefinitely "
        "for the peer",
        options->role == UM_LIVE_CLIENT ? "client" : "gateway",
        options->test_bytes);
    started_at = time(NULL);

    while (um_light_video_should_close(video) == 0) {
        um_light_peer_frame outbound;
        um_light_peer_frame received;
        um_light_peer_status peer_status;
        um_light_rx_metrics metrics;
        size_t width = 0u;
        size_t height = 0u;
        size_t payload_length = 0u;
        int capture_status;
        int decode_status;

        status = um_light_peer_build(peer, local_frame, &outbound);
        if (status != UM_OK) {
            break;
        }
        status = um_light_encode_frame(
            outbound.type, outbound.session_id, outbound.sequence,
            outbound.payload, outbound.payload_length, modules,
            sizeof(modules));
        if (status != UM_OK) {
            break;
        }
        status = um_light_video_present(video, modules, sizeof(modules));
        if (status != UM_OK) {
            break;
        }

        memset(&received, 0, sizeof(received));
        capture_status = um_light_video_capture(
            video, pixels, LIGHT_LIVE_MAX_PIXELS, 250u, &width, &height);
        if (capture_status == UM_OK) {
            memset(&metrics, 0, sizeof(metrics));
            decode_status = um_light_decode_frame(
                pixels, width, height, width, &received.type,
                &received.session_id, &received.sequence, received.payload,
                sizeof(received.payload), &payload_length, &metrics);
            if (decode_status == UM_OK) {
                received.present = 1;
                received.payload_length = payload_length;
                last_metrics = metrics;
                ++decoded_frames;
            } else if (decode_status == UM_ERR_SYNC) {
                ++sync_misses;
                ++decode_misses;
            } else if (decode_status == UM_ERR_CRC) {
                ++crc_misses;
                ++decode_misses;
            } else if (decode_status == UM_ERR_HEADER) {
                ++header_misses;
                ++decode_misses;
            } else {
                status = decode_status;
                break;
            }
        } else if (capture_status == UM_ERR_TIMEOUT) {
            ++decode_misses;
        } else {
            status = capture_status;
            break;
        }

        status = um_light_peer_process(
            peer, local_frame, received.present != 0 ? &received : NULL);
        if (status != UM_OK) {
            break;
        }
        um_light_peer_get_status(peer, &peer_status);
        if (local_frame == 0u ||
            local_frame % (5u * video_config.frames_per_second) == 0u) {
            light_live_log(
                logger, logger_context,
                "light link frame=%zu state=%s decoded=%zu misses=%zu "
                "(sync=%zu header=%zu crc=%zu) "
                "upload-acked=%zu/%zu download=%zu/%zu retries=%zu "
                "timeouts=%zu quality=%.1f%%/%.3f/%.2f%%",
                local_frame,
                peer_status.connected != 0 ? "CONNECTED" : "DISCOVERY",
                decoded_frames, decode_misses, sync_misses, header_misses,
                crc_misses,
                peer_status.outgoing_bytes_acked, options->test_bytes,
                peer_status.incoming_bytes_received, options->test_bytes,
                peer_status.retransmissions, peer_status.link_timeouts,
                100.0f * last_metrics.image_coverage,
                last_metrics.contrast,
                100.0f * last_metrics.corrected_bit_fraction);
        }

        if (um_light_peer_complete(peer) != 0) {
            if (completion_frame == SIZE_MAX) {
                completion_frame = local_frame;
                light_live_log(
                    logger, logger_context,
                    "Light payload complete; repeating final ACKs for %u "
                    "frames",
                    options->completion_linger_frames);
            }
            if (local_frame - completion_frame >=
                options->completion_linger_frames) {
                status = UM_OK;
                break;
            }
        } else {
            completion_frame = SIZE_MAX;
        }
        ++local_frame;
    }

    if (status == UM_OK && um_light_peer_complete(peer) == 0) {
        status = UM_ERR_INTERRUPTED;
    }
    if (status == UM_OK &&
        memcmp(incoming, expected, options->test_bytes) != 0) {
        status = UM_ERR_CRC;
    }
    if (status == UM_OK) {
        um_light_peer_status peer_status;
        time_t elapsed;
        um_light_peer_get_status(peer, &peer_status);
        elapsed = time(NULL) - started_at;
        light_live_log(
            logger, logger_context,
            "Light link test complete role=%s frames=%zu "
            "upload-acked=%zu/%zu download=%zu/%zu decoded=%zu "
            "misses=%zu retries=%zu reconnects=%zu elapsed=%lds "
            "combined-goodput=%.0fbps",
            options->role == UM_LIVE_CLIENT ? "client" : "gateway",
            local_frame + 1u, peer_status.outgoing_bytes_acked,
            options->test_bytes, peer_status.incoming_bytes_received,
            options->test_bytes, decoded_frames, decode_misses,
            peer_status.retransmissions, peer_status.reconnects,
            (long)elapsed,
            elapsed > 0
                ? 8.0 * (double)(peer_status.outgoing_bytes_acked +
                                 peer_status.incoming_bytes_received) /
                      (double)elapsed
                : 0.0);
    }

done:
    um_light_video_close(video);
    um_light_peer_destroy(peer);
    free(expected);
    free(incoming);
    free(outgoing);
    free(pixels);
    return status;
}
