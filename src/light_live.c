#include "light_video.h"
#include "network.h"
#include "traffic_policy.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIGHT_LIVE_MAX_TEST_BYTES (16u * 1024u * 1024u)
#define LIGHT_LIVE_MAX_PIXELS ((size_t)4096u * 4096u)
#define LIGHT_LIVE_CAPTURE_FPS 30u
#define LIGHT_LIVE_SYMBOL_FPS 10u
#define LIGHT_LIVE_NETWORK_MTU 1500u
#define LIGHT_LIVE_PACKET_QUEUE 128u
#define LIGHT_LIVE_INGRESS_BURST 32u

_Static_assert(UM_LIGHT_MAX_PACKET == UM_NETWORK_MAX_PACKET,
               "optical and TUN packet capacities must match");

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

static double light_live_now_seconds(void)
{
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return (double)time(NULL);
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
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

typedef struct {
    double started_at;
    size_t tun_packets_read;
    size_t tun_bytes_read;
    size_t tun_packets_written;
    size_t tun_bytes_written;
    size_t multicast_dropped;
    size_t broadcast_dropped;
    size_t stale_dns_icmp_dropped;
    size_t background_dropped;
    size_t background_dns_rejected;
    size_t quic_rejected;
} light_live_network_stats;

static int light_live_filter_packet(
    const um_live_light_options *options, um_network *network,
    const uint8_t *packet, size_t packet_length,
    light_live_network_stats *stats, int *pass)
{
    um_traffic_policy_decision decision;
    uint8_t response[UM_NETWORK_MAX_PACKET];
    size_t response_length = 0u;
    int status;
    *pass = 0;
    if (um_traffic_policy_decide(
            packet, packet_length,
            options->role == UM_LIVE_CLIENT,
            options->filter_background_traffic,
            options->allow_messages_traffic, &decision) != 0) {
        return UM_ERR_HEADER;
    }
    switch (decision.action) {
    case UM_TRAFFIC_POLICY_PASS:
        *pass = 1;
        return UM_OK;
    case UM_TRAFFIC_POLICY_DROP_MULTICAST:
        ++stats->multicast_dropped;
        return UM_OK;
    case UM_TRAFFIC_POLICY_DROP_BROADCAST:
        ++stats->broadcast_dropped;
        return UM_OK;
    case UM_TRAFFIC_POLICY_DROP_STALE_DNS_ICMP:
        ++stats->stale_dns_icmp_dropped;
        return UM_OK;
    case UM_TRAFFIC_POLICY_DROP_BACKGROUND:
        ++stats->background_dropped;
        return UM_OK;
    case UM_TRAFFIC_POLICY_REJECT_BACKGROUND_DNS:
        if (um_traffic_policy_build_dns_rejection(
                packet, packet_length, response, sizeof(response),
                &response_length) != 0) {
            return UM_ERR_HEADER;
        }
        status = um_network_write(network, response, response_length,
                                  1000u);
        if (status == UM_OK) {
            ++stats->background_dns_rejected;
        }
        return status;
    case UM_TRAFFIC_POLICY_REJECT_QUIC:
        if (um_traffic_policy_build_port_unreachable(
                packet, packet_length, response, sizeof(response),
                &response_length) != 0) {
            return UM_ERR_HEADER;
        }
        status = um_network_write(network, response, response_length,
                                  1000u);
        if (status == UM_OK) {
            ++stats->quic_rejected;
        }
        return status;
    }
    return UM_ERR_HEADER;
}

static int light_live_collect_packets(
    const um_live_light_options *options, um_network *network,
    um_light_peer *peer, light_live_network_stats *stats)
{
    unsigned read_count;
    for (read_count = 0u; read_count < LIGHT_LIVE_INGRESS_BURST;
         ++read_count) {
        um_light_peer_status peer_status;
        uint8_t packet[UM_NETWORK_MAX_PACKET];
        size_t packet_length = 0u;
        int pass;
        int status;
        um_light_peer_get_status(peer, &peer_status);
        if (peer_status.outgoing_packets_queued >=
            LIGHT_LIVE_PACKET_QUEUE) {
            break;
        }
        status = um_network_read(network, packet, sizeof(packet), 0u,
                                 &packet_length);
        if (status == UM_ERR_TIMEOUT) {
            break;
        }
        if (status != UM_OK) {
            return status;
        }
        status = light_live_filter_packet(options, network, packet,
                                          packet_length, stats, &pass);
        if (status != UM_OK) {
            return status;
        }
        if (pass == 0) {
            continue;
        }
        status = um_light_peer_enqueue_packet(peer, packet,
                                              packet_length);
        if (status != UM_OK) {
            return status;
        }
        ++stats->tun_packets_read;
        stats->tun_bytes_read += packet_length;
    }
    return UM_OK;
}

static int light_live_deliver_packets(um_network *network,
                                      um_light_peer *peer,
                                      light_live_network_stats *stats)
{
    for (;;) {
        uint8_t packet[UM_NETWORK_MAX_PACKET];
        size_t packet_length = 0u;
        int status = um_light_peer_dequeue_packet(
            peer, packet, sizeof(packet), &packet_length);
        if (status == UM_ERR_TIMEOUT) {
            return UM_OK;
        }
        if (status != UM_OK) {
            return status;
        }
        status = um_network_write(network, packet, packet_length, 1000u);
        if (status != UM_OK) {
            return status;
        }
        ++stats->tun_packets_written;
        stats->tun_bytes_written += packet_length;
    }
}

um_live_light_options um_live_light_default_options(um_live_role role)
{
    um_live_light_options options;
    options.role = role;
    options.camera_device = "default";
    options.link_test = 0;
    options.test_bytes = 1024u;
    options.window_size = 720u;
    options.completion_linger_frames = 30u;
    options.filter_background_traffic = 1;
    options.allow_messages_traffic = 0;
    return options;
}

int um_run_live_light(const um_live_light_options *options,
                      um_log_callback logger, void *logger_context)
{
    um_light_video_config video_config = um_light_video_default_config();
    um_light_peer_config peer_config = um_light_peer_default_config();
    um_light_packet_peer_config packet_config =
        um_light_packet_peer_default_config();
    um_light_video *video = NULL;
    um_light_peer *peer = NULL;
    um_network *network = NULL;
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t *pixels = NULL;
    uint8_t *outgoing = NULL;
    uint8_t *incoming = NULL;
    uint8_t *expected = NULL;
    size_t local_frame = 0u;
    size_t displayed_frames = 0u;
    size_t captured_frames = 0u;
    size_t decoded_frames = 0u;
    size_t decode_misses = 0u;
    size_t sync_misses = 0u;
    size_t header_misses = 0u;
    size_t crc_misses = 0u;
    size_t completion_frame = SIZE_MAX;
    um_light_rx_metrics last_metrics;
    light_live_network_stats network_stats;
    double started_at = 0.0;
    double next_symbol_at = 0.0;
    double next_report_at = 0.0;
    int status = UM_OK;

    if (options == NULL ||
        (options->role != UM_LIVE_CLIENT &&
         options->role != UM_LIVE_GATEWAY) ||
        options->camera_device == NULL ||
        (options->link_test != 0 &&
         (options->test_bytes == 0u ||
          options->test_bytes > LIGHT_LIVE_MAX_TEST_BYTES)) ||
        options->window_size < 256u || options->window_size > 4096u ||
        (options->link_test != 0 &&
         options->completion_linger_frames == 0u)) {
        return UM_ERR_ARGUMENT;
    }
    memset(&last_metrics, 0, sizeof(last_metrics));
    memset(&network_stats, 0, sizeof(network_stats));

    pixels = (uint8_t *)malloc(LIGHT_LIVE_MAX_PIXELS);
    if (options->link_test != 0) {
        outgoing = (uint8_t *)malloc(options->test_bytes);
        incoming = (uint8_t *)calloc(options->test_bytes, 1u);
        expected = (uint8_t *)malloc(options->test_bytes);
    }
    if (pixels == NULL ||
        (options->link_test != 0 &&
         (outgoing == NULL || incoming == NULL || expected == NULL))) {
        status = UM_ERR_MEMORY;
        goto done;
    }
    peer_config.random_seed = light_live_seed(options->role);
    if (options->link_test != 0) {
        light_live_fill(outgoing, options->test_bytes,
                        options->role == UM_LIVE_CLIENT
                            ? UINT32_C(0xc11e4701)
                            : UINT32_C(0x6a7e5a91));
        light_live_fill(expected, options->test_bytes,
                        options->role == UM_LIVE_CLIENT
                            ? UINT32_C(0x6a7e5a91)
                            : UINT32_C(0xc11e4701));
        status = um_light_peer_create(
            &peer, options->role, &peer_config, outgoing,
            options->test_bytes, incoming, options->test_bytes, logger,
            logger_context);
    } else {
        packet_config.link = peer_config;
        packet_config.max_packet_bytes = UM_NETWORK_MAX_PACKET;
        packet_config.queue_packets = LIGHT_LIVE_PACKET_QUEUE;
        status = um_light_packet_peer_create(
            &peer, options->role, &packet_config, logger, logger_context);
    }
    if (status != UM_OK) {
        goto done;
    }

    video_config.camera_device = options->camera_device;
    video_config.window_size = options->window_size;
    video_config.frames_per_second = LIGHT_LIVE_CAPTURE_FPS;
    status = um_light_video_open(&video, &video_config, logger,
                                 logger_context);
    if (status != UM_OK) {
        goto done;
    }
    if (options->link_test != 0) {
        light_live_log(
            logger, logger_context,
            "Light link test role=%s bytes=%zu capture=%ufps "
            "symbols=%ufps; discovery waits indefinitely for the peer",
            options->role == UM_LIVE_CLIENT ? "client" : "gateway",
            options->test_bytes, LIGHT_LIVE_CAPTURE_FPS,
            LIGHT_LIVE_SYMBOL_FPS);
    } else {
        light_live_log(
            logger, logger_context,
            "Light network role=%s capture=%ufps symbols=%ufps "
            "packet-queue=%u; waiting for peer before configuring TUN",
            options->role == UM_LIVE_CLIENT ? "client" : "gateway",
            LIGHT_LIVE_CAPTURE_FPS, LIGHT_LIVE_SYMBOL_FPS,
            LIGHT_LIVE_PACKET_QUEUE);
    }
    started_at = light_live_now_seconds();
    next_symbol_at = started_at;
    next_report_at = started_at;

    while (um_light_video_should_close(video) == 0) {
        um_light_peer_frame received;
        um_light_peer_status peer_status;
        um_light_rx_metrics metrics;
        size_t width = 0u;
        size_t height = 0u;
        size_t payload_length = 0u;
        double now = light_live_now_seconds();
        int capture_status;
        int decode_status;

        if (displayed_frames == 0u || now >= next_symbol_at) {
            um_light_peer_frame outbound;
            local_frame = displayed_frames;
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
            status =
                um_light_video_present(video, modules, sizeof(modules));
            if (status != UM_OK) {
                break;
            }
            ++displayed_frames;
            next_symbol_at =
                now + 1.0 / (double)LIGHT_LIVE_SYMBOL_FPS;
        }

        memset(&received, 0, sizeof(received));
        capture_status = um_light_video_capture(
            video, pixels, LIGHT_LIVE_MAX_PIXELS, 250u, &width, &height);
        if (capture_status == UM_OK) {
            ++captured_frames;
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
        if (options->link_test == 0 && peer_status.connected != 0) {
            if (network == NULL) {
                light_live_log(logger, logger_context,
                               "state=NETWORK_CONFIGURING role=%s",
                               options->role == UM_LIVE_CLIENT
                                   ? "client"
                                   : "gateway");
                status = um_network_open(
                    &network, options->role, LIGHT_LIVE_NETWORK_MTU,
                    logger, logger_context);
                if (status != UM_OK) {
                    break;
                }
                network_stats.started_at = light_live_now_seconds();
                light_live_log(
                    logger, logger_context,
                    "state=PROXYING interface=%s mtu=%u full-duplex",
                    um_network_interface_name(network),
                    um_network_mtu(network));
            }
            status = light_live_deliver_packets(network, peer,
                                                &network_stats);
            if (status == UM_OK) {
                status = light_live_collect_packets(
                    options, network, peer, &network_stats);
            }
            if (status != UM_OK) {
                break;
            }
            um_light_peer_get_status(peer, &peer_status);
        }
        now = light_live_now_seconds();
        if (now >= next_report_at) {
            if (options->link_test != 0) {
                light_live_log(
                    logger, logger_context,
                    "light link symbol=%zu captured=%zu state=%s "
                    "decoded=%zu misses=%zu "
                    "(sync=%zu header=%zu crc=%zu) "
                    "upload-acked=%zu/%zu download=%zu/%zu retries=%zu "
                    "timeouts=%zu quality=%.1f%%/%.3f/%.2f%%",
                    local_frame, captured_frames,
                    peer_status.connected != 0 ? "CONNECTED"
                                               : "DISCOVERY",
                    decoded_frames, decode_misses, sync_misses,
                    header_misses, crc_misses,
                    peer_status.outgoing_bytes_acked,
                    options->test_bytes,
                    peer_status.incoming_bytes_received,
                    options->test_bytes, peer_status.retransmissions,
                    peer_status.link_timeouts,
                    100.0f * last_metrics.image_coverage,
                    last_metrics.contrast,
                    100.0f * last_metrics.corrected_bit_fraction);
            } else {
                double network_seconds =
                    network_stats.started_at > 0.0
                        ? now - network_stats.started_at
                        : 0.0;
                size_t upload_bytes =
                    options->role == UM_LIVE_CLIENT
                        ? network_stats.tun_bytes_read
                        : network_stats.tun_bytes_written;
                size_t download_bytes =
                    options->role == UM_LIVE_CLIENT
                        ? network_stats.tun_bytes_written
                        : network_stats.tun_bytes_read;
                light_live_log(
                    logger, logger_context,
                    "light network symbol=%zu state=%s decoded=%zu "
                    "misses=%zu retries=%zu reconnects=%zu "
                    "queue=%zu+%zu received=%zu delivered=%zu "
                    "internet-goodput wall=%.1fs upload=%.0fbps "
                    "download=%.0fbps total=%.0fbps "
                    "filtered=%zu/%zu/%zu/%zu/%zu/%zu "
                    "quality=%.1f%%/%.3f/%.2f%%",
                    local_frame,
                    peer_status.connected != 0 ? "CONNECTED"
                                               : "DISCOVERY",
                    decoded_frames, decode_misses,
                    peer_status.retransmissions, peer_status.reconnects,
                    peer_status.outgoing_packets_queued,
                    peer_status.outgoing_cells_in_flight,
                    peer_status.incoming_packets_received,
                    network_stats.tun_packets_written, network_seconds,
                    network_seconds > 0.0
                        ? 8.0 * (double)upload_bytes / network_seconds
                        : 0.0,
                    network_seconds > 0.0
                        ? 8.0 * (double)download_bytes / network_seconds
                        : 0.0,
                    network_seconds > 0.0
                        ? 8.0 * (double)(upload_bytes + download_bytes) /
                              network_seconds
                        : 0.0,
                    network_stats.multicast_dropped,
                    network_stats.broadcast_dropped,
                    network_stats.stale_dns_icmp_dropped,
                    network_stats.background_dropped,
                    network_stats.background_dns_rejected,
                    network_stats.quic_rejected,
                    100.0f * last_metrics.image_coverage,
                    last_metrics.contrast,
                    100.0f * last_metrics.corrected_bit_fraction);
            }
            next_report_at = now + 5.0;
        }

        if (options->link_test != 0 && um_light_peer_complete(peer) != 0) {
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
        } else if (options->link_test != 0) {
            completion_frame = SIZE_MAX;
        }
    }

    if (status == UM_OK &&
        (options->link_test == 0 || um_light_peer_complete(peer) == 0)) {
        status = UM_ERR_INTERRUPTED;
    }
    if (options->link_test != 0 && status == UM_OK &&
        memcmp(incoming, expected, options->test_bytes) != 0) {
        status = UM_ERR_CRC;
    }
    if (options->link_test != 0 && status == UM_OK) {
        um_light_peer_status peer_status;
        double elapsed;
        um_light_peer_get_status(peer, &peer_status);
        elapsed = light_live_now_seconds() - started_at;
        light_live_log(
            logger, logger_context,
            "Light link test complete role=%s symbols=%zu captured=%zu "
            "upload-acked=%zu/%zu download=%zu/%zu decoded=%zu "
            "misses=%zu retries=%zu reconnects=%zu elapsed=%.1fs "
            "combined-goodput=%.0fbps",
            options->role == UM_LIVE_CLIENT ? "client" : "gateway",
            displayed_frames, captured_frames,
            peer_status.outgoing_bytes_acked, options->test_bytes,
            peer_status.incoming_bytes_received, options->test_bytes,
            decoded_frames, decode_misses, peer_status.retransmissions,
            peer_status.reconnects, elapsed,
            elapsed > 0.0
                ? 8.0 * (double)(peer_status.outgoing_bytes_acked +
                                 peer_status.incoming_bytes_received) /
                      elapsed
                : 0.0);
    }

done:
    um_network_close(network);
    um_light_video_close(video);
    um_light_peer_destroy(peer);
    free(expected);
    free(incoming);
    free(outgoing);
    free(pixels);
    return status;
}
