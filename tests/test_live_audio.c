#define _POSIX_C_SOURCE 200809L

#include "um.h"
#include "../src/audio.h"
#include "../src/live_wire.h"
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

#define SIM_DNS_REQUEST_BYTES 64u
#define SIM_DNS_RESPONSE_BYTES 96u
#define SIM_LARGE_REQUEST_BYTES UM_NETWORK_MTU
#define SIM_LARGE_RESPONSE_BYTES 413u
#define SIM_SMALL_TCP_BYTES 60u
#define SIM_BACKGROUND_BYTES 64u
#define SIM_BACKGROUND_PACKETS 16u
#define SIM_FILTERED_BACKGROUND_PACKETS 3u
#define SIM_COALESCED_BACKGROUND_PACKETS 1u
#define SIM_FORWARDED_BACKGROUND_PACKETS                              \
    (SIM_BACKGROUND_PACKETS - SIM_FILTERED_BACKGROUND_PACKETS -      \
     SIM_COALESCED_BACKGROUND_PACKETS)
#define SIM_PROXY_PACKETS 10u
#define SIM_FORWARDED_PACKETS \
    (SIM_PROXY_PACKETS + SIM_FORWARDED_BACKGROUND_PACKETS)

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
    int offer_drop_enabled;
    int offer_drop_armed;
    unsigned offer_drops;
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
    int commit_drop_armed;
    unsigned commit_drops;
    int body_drop_endpoint;
    int body_drop_armed;
    unsigned body_drops;
    int network_opened[SIM_ENDPOINTS];
    int network_input_ready[SIM_ENDPOINTS];
    int network_dns_retry_ready[SIM_ENDPOINTS];
    unsigned network_background_remaining[SIM_ENDPOINTS];
    unsigned network_dns_retry_reads[SIM_ENDPOINTS];
    unsigned network_dns_retries_injected;
    int network_dns_response_delayed;
    int client_tcp_before_dns_drained;
    unsigned network_targets_read[SIM_ENDPOINTS];
    unsigned network_reads[SIM_ENDPOINTS];
    unsigned network_writes[SIM_ENDPOINTS];
    unsigned network_matches[SIM_ENDPOINTS];
    unsigned network_background_matches[SIM_ENDPOINTS];
    uint32_t network_target_seen[SIM_ENDPOINTS];
    uint32_t network_background_seen[SIM_ENDPOINTS];
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
    unsigned offer_refreshes;
    unsigned network_ready;
    unsigned proxying;
    unsigned proxy_completed;
    unsigned proxy_start_retries;
    unsigned proxy_retries;
    unsigned commit_retries;
    unsigned packet_token_commits;
    unsigned packet_token_accepts;
    unsigned dns_query_logs;
    unsigned dns_response_logs;
    unsigned dns_retries_suppressed;
    unsigned dns_response_bursts;
    unsigned multicast_dropped;
    unsigned broadcast_dropped;
    unsigned stale_dns_icmp_dropped;
    unsigned discovery_dns_deprioritized;
    unsigned tcp_acks_coalesced;
    unsigned multi_packet_batches;
    unsigned rate_breakdowns;
    unsigned body_stepdowns;
    unsigned reconnecting;
} runner;

static uint32_t test_checksum_add(uint32_t sum, const uint8_t *bytes,
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

static uint16_t test_checksum_finish(uint32_t sum)
{
    while ((sum >> 16u) != 0u) {
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16u);
    }
    return (uint16_t)~sum;
}

static void finalize_ipv4_checksums(uint8_t *packet, size_t length)
{
    size_t header_length = (size_t)(packet[0] & 0x0fu) * 4u;
    size_t transport_length = length - header_length;
    uint32_t sum;
    uint16_t checksum;
    packet[10] = 0u;
    packet[11] = 0u;
    checksum = test_checksum_finish(test_checksum_add(
        0u, packet, header_length));
    packet[10] = (uint8_t)(checksum >> 8u);
    packet[11] = (uint8_t)checksum;
    if (packet[9] != 6u && packet[9] != 17u) {
        return;
    }
    if (packet[9] == 17u) {
        packet[header_length + 6u] = 0u;
        packet[header_length + 7u] = 0u;
    } else {
        packet[header_length + 16u] = 0u;
        packet[header_length + 17u] = 0u;
    }
    sum = test_checksum_add(0u, &packet[12], 8u);
    sum += packet[9];
    sum += (uint32_t)transport_length;
    sum = test_checksum_add(sum, packet + header_length,
                            transport_length);
    checksum = test_checksum_finish(sum);
    if (checksum == 0u && packet[9] == 17u) {
        checksum = UINT16_C(0xffff);
    }
    if (packet[9] == 17u) {
        packet[header_length + 6u] = (uint8_t)(checksum >> 8u);
        packet[header_length + 7u] = (uint8_t)checksum;
    } else {
        packet[header_length + 16u] = (uint8_t)(checksum >> 8u);
        packet[header_length + 17u] = (uint8_t)checksum;
    }
}

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
    finalize_ipv4_checksums(packet, length);
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
    finalize_ipv4_checksums(packet, length);
}

static void make_proxy_target(uint8_t *packet, size_t length, int endpoint,
                              unsigned target_index)
{
    if (target_index == 0u) {
        uint8_t *dns;
        make_ipv4_packet(packet, length,
                         endpoint == SIM_CLIENT ? 0x31u : 0xa7u,
                         endpoint == SIM_CLIENT ? 53000u : 53u,
                         endpoint == SIM_CLIENT ? 53u : 53000u);
        if (endpoint != SIM_CLIENT) {
            packet[12] = 1u;
            packet[13] = 1u;
            packet[14] = 1u;
            packet[15] = 1u;
            packet[16] = 10u;
            packet[17] = 0u;
            packet[18] = 0u;
            packet[19] = 2u;
        }
        dns = &packet[28];
        memset(dns, 0, 26u);
        dns[0] = 0x4du;
        dns[1] = 0x2au;
        dns[2] = endpoint == SIM_CLIENT ? 0x01u : 0x81u;
        dns[3] = endpoint == SIM_CLIENT ? 0x00u : 0x80u;
        dns[5] = 1u;
        dns[7] = endpoint == SIM_CLIENT ? 0u : 1u;
        dns[12] = 3u;
        memcpy(&dns[13], "dns", 3u);
        dns[16] = 4u;
        memcpy(&dns[17], "test", 4u);
        dns[23] = 1u;
        dns[25] = 1u;
        if (endpoint != SIM_CLIENT) {
            dns[26] = 0xc0u;
            dns[27] = 0x0cu;
            dns[29] = 1u;
            dns[31] = 1u;
            dns[35] = 60u;
            dns[37] = 4u;
            dns[38] = 93u;
            dns[39] = 184u;
            dns[40] = 216u;
            dns[41] = 34u;
        }
        finalize_ipv4_checksums(packet, length);
    } else {
        make_ipv4_tcp_packet(
            packet, length,
            (uint8_t)((endpoint == SIM_CLIENT ? 0x50u : 0xb0u) +
                      target_index),
            endpoint == SIM_CLIENT ? 50000u : 443u,
            endpoint == SIM_CLIENT ? 443u : 50000u);
    }
}

static size_t proxy_target_length(int endpoint, unsigned target_index)
{
    if (target_index == 0u) {
        return endpoint == SIM_CLIENT ? SIM_DNS_REQUEST_BYTES
                                      : SIM_DNS_RESPONSE_BYTES;
    }
    if (target_index == 1u) {
        return endpoint == SIM_CLIENT ? SIM_LARGE_REQUEST_BYTES
                                      : SIM_LARGE_RESPONSE_BYTES;
    }
    return SIM_SMALL_TCP_BYTES;
}

static size_t proxy_background_length(unsigned background_number)
{
    if (background_number == 14u) {
        return 56u;
    }
    if (background_number == 13u || background_number == 12u) {
        return 40u;
    }
    if (background_number == 11u) {
        return 68u;
    }
    return SIM_BACKGROUND_BYTES;
}

static void make_stale_dns_icmp(uint8_t *packet, size_t length)
{
    uint8_t *quoted;
    memset(packet, 0, length);
    packet[0] = 0x45u;
    packet[2] = (uint8_t)(length >> 8u);
    packet[3] = (uint8_t)length;
    packet[8] = 64u;
    packet[9] = 1u;
    packet[12] = 10u;
    packet[13] = 77u;
    packet[15] = 2u;
    packet[16] = 1u;
    packet[17] = 1u;
    packet[18] = 1u;
    packet[19] = 1u;
    packet[20] = 3u;
    packet[21] = 3u;
    quoted = &packet[28];
    quoted[0] = 0x45u;
    quoted[2] = 0u;
    quoted[3] = 48u;
    quoted[8] = 64u;
    quoted[9] = 17u;
    quoted[12] = 1u;
    quoted[13] = 1u;
    quoted[14] = 1u;
    quoted[15] = 1u;
    quoted[16] = 10u;
    quoted[17] = 77u;
    quoted[19] = 2u;
    quoted[20] = 0u;
    quoted[21] = 53u;
    quoted[22] = 0x9cu;
    quoted[23] = 0x40u;
    quoted[24] = 0u;
    quoted[25] = 28u;
    finalize_ipv4_checksums(packet, length);
}

static void make_coalescible_tcp_ack(uint8_t *packet, int endpoint,
                                     unsigned background_number)
{
    make_ipv4_tcp_packet(packet, 40u, 0u,
                         endpoint == SIM_CLIENT ? 51000u : 443u,
                         endpoint == SIM_CLIENT ? 443u : 51000u);
    if (endpoint == SIM_GATEWAY) {
        packet[12] = 1u;
        packet[13] = 1u;
        packet[14] = 1u;
        packet[15] = 1u;
        packet[16] = 10u;
        packet[17] = 0u;
        packet[18] = 0u;
        packet[19] = 2u;
    }
    packet[4] = 0u;
    packet[5] = (uint8_t)background_number;
    packet[31] = (uint8_t)(14u - background_number);
    packet[33] = 0x10u;
    finalize_ipv4_checksums(packet, 40u);
}

static void make_tunnel_discovery_dns(uint8_t *packet, int endpoint)
{
    uint8_t *dns;
    make_ipv4_packet(packet, 68u, 0u,
                     endpoint == SIM_CLIENT ? 52000u : 53u,
                     endpoint == SIM_CLIENT ? 53u : 52000u);
    if (endpoint == SIM_GATEWAY) {
        packet[12] = 1u;
        packet[13] = 1u;
        packet[14] = 1u;
        packet[15] = 1u;
        packet[16] = 10u;
        packet[17] = 77u;
        packet[18] = 0u;
        packet[19] = 2u;
    }
    dns = &packet[28];
    memset(dns, 0, 40u);
    dns[0] = 0x22u;
    dns[1] = 0x11u;
    dns[2] = endpoint == SIM_CLIENT ? 0x01u : 0x81u;
    dns[3] = endpoint == SIM_CLIENT ? 0x00u : 0x83u;
    dns[5] = 1u;
    dns[12] = 1u;
    dns[13] = '2';
    dns[14] = 1u;
    dns[15] = '0';
    dns[16] = 2u;
    dns[17] = '7';
    dns[18] = '7';
    dns[19] = 2u;
    dns[20] = '1';
    dns[21] = '0';
    dns[22] = 7u;
    memcpy(&dns[23], "in-addr", 7u);
    dns[30] = 4u;
    memcpy(&dns[31], "arpa", 4u);
    dns[36] = 0u;
    dns[37] = 12u;
    dns[39] = 1u;
    finalize_ipv4_checksums(packet, 68u);
}

static void make_proxy_background(uint8_t *packet, int endpoint,
                                  unsigned background_number)
{
    size_t length = proxy_background_length(background_number);
    if (background_number == 15u) {
        make_ipv4_packet(packet, length, 0x15u, 137u, 137u);
        packet[16] = 10u;
        packet[17] = 77u;
        packet[18] = 0u;
        packet[19] = 3u;
        finalize_ipv4_checksums(packet, length);
        return;
    }
    if (background_number == 14u) {
        make_stale_dns_icmp(packet, length);
        return;
    }
    if (background_number == 13u || background_number == 12u) {
        make_coalescible_tcp_ack(packet, endpoint, background_number);
        return;
    }
    if (background_number == 11u) {
        make_tunnel_discovery_dns(packet, endpoint);
        return;
    }
    if (endpoint == SIM_GATEWAY) {
        uint8_t *dns;
        make_ipv4_packet(packet, length,
                         (uint8_t)(0x70u + background_number), 53u,
                         (uint16_t)(40000u + background_number));
        packet[12] = 1u;
        packet[13] = 1u;
        packet[14] = 1u;
        packet[15] = 1u;
        packet[16] = 10u;
        packet[17] = 0u;
        packet[18] = 0u;
        packet[19] = 2u;
        dns = &packet[28];
        memset(dns, 0, length - 28u);
        dns[0] = 0x70u;
        dns[1] = (uint8_t)background_number;
        dns[2] = 0x81u;
        dns[3] = 0x83u;
        dns[5] = 1u;
        dns[12] = 2u;
        memcpy(&dns[13], "bg", 2u);
        dns[15] = 4u;
        memcpy(&dns[16], "test", 4u);
        dns[22] = 1u;
        dns[24] = 1u;
        finalize_ipv4_checksums(packet, length);
    } else {
        make_ipv4_packet(packet, length,
                         (uint8_t)(0x40u + background_number),
                         (uint16_t)(40000u + background_number), 53u);
    }
    if (background_number == SIM_BACKGROUND_PACKETS) {
        packet[16] = 224u;
        packet[17] = 0u;
        packet[18] = 0u;
        packet[19] = 251u;
        finalize_ipv4_checksums(packet, length);
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
    if (run->options.role == UM_LIVE_GATEWAY &&
        strstr(message, "state=NEGOTIATING rx=DISCOVER tx=OFFER") != NULL) {
        (void)pthread_mutex_lock(&bus.mutex);
        if (bus.offer_drop_enabled != 0 && bus.offer_drops == 0u) {
            bus.offer_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (strstr(message,
               "state=NEGOTIATING rx=DISCOVER retry tx=OFFER") != NULL) {
        ++run->offer_refreshes;
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
    if (strstr(message, "calib body tx direction=") != NULL &&
        strstr(message, "size=384 trial=2/2") != NULL) {
        int endpoint = run->options.role == UM_LIVE_CLIENT
                           ? SIM_CLIENT
                           : SIM_GATEWAY;
        (void)pthread_mutex_lock(&bus.mutex);
        if (bus.body_drop_endpoint == endpoint && bus.body_drops == 0u) {
            bus.body_drop_armed = 1;
        }
        (void)pthread_mutex_unlock(&bus.mutex);
    }
    if (strstr(message, "calib body size=512 failed; selected=384") != NULL) {
        ++run->body_stepdowns;
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
    if (strstr(message, "proxy token commit wait retry=") != NULL) {
        ++run->commit_retries;
    }
    if (strstr(message, "proxy packet token commit wait retry=") != NULL) {
        ++run->commit_retries;
    }
    if (strstr(message, "proxy packet token accept sequence=") != NULL) {
        ++run->packet_token_accepts;
    }
    if (strstr(message, "proxy packet token commit sequence=") != NULL) {
        ++run->packet_token_commits;
    }
    if (strstr(message, "DNS query dns.test A") != NULL) {
        ++run->dns_query_logs;
    }
    if (strstr(message, "DNS response dns.test A") != NULL &&
        strstr(message, "rcode=0 answers=1") != NULL) {
        ++run->dns_response_logs;
        if (run->options.role == UM_LIVE_CLIENT) {
            /* log_proxy_packet follows remember_completed_dns, so this
             * retry deterministically exercises the completed cache. */
            (void)pthread_mutex_lock(&bus.mutex);
            if (bus.network_dns_retries_injected == 2u) {
                ++bus.network_dns_retry_ready[SIM_CLIENT];
                ++bus.network_dns_retries_injected;
                (void)pthread_cond_broadcast(&bus.changed);
            }
            (void)pthread_mutex_unlock(&bus.mutex);
        }
    }
    if (strstr(message,
               "proxy DNS response backlog continuing turn") != NULL) {
        ++run->dns_response_bursts;
    }
    {
        const char *batch = strstr(message, " batch=");
        const char *packets = strstr(message, " packets=");
        unsigned count;
        if (batch != NULL && packets != NULL &&
            sscanf(packets + strlen(" packets="), "%u", &count) == 1 &&
            count > 1u) {
            ++run->multi_packet_batches;
        }
    }
    {
        const char *suppressed = strstr(message,
                                        "dns-retries-suppressed=");
        unsigned count;
        if (suppressed != NULL &&
            sscanf(suppressed + strlen("dns-retries-suppressed="),
                   "%u", &count) == 1) {
            run->dns_retries_suppressed = count;
        }
    }
    {
        const char *dropped = strstr(message, "multicast-dropped=");
        unsigned count;
        if (dropped != NULL &&
            sscanf(dropped + strlen("multicast-dropped="),
                   "%u", &count) == 1) {
            run->multicast_dropped = count;
        }
    }
    {
        const char *dropped = strstr(message, "broadcast-dropped=");
        unsigned count;
        if (dropped != NULL &&
            sscanf(dropped + strlen("broadcast-dropped="),
                   "%u", &count) == 1) {
            run->broadcast_dropped = count;
        }
    }
    {
        const char *dropped = strstr(message,
                                     "stale-dns-icmp-dropped=");
        unsigned count;
        if (dropped != NULL &&
            sscanf(dropped + strlen("stale-dns-icmp-dropped="),
                   "%u", &count) == 1) {
            run->stale_dns_icmp_dropped = count;
        }
    }
    {
        const char *deprioritized = strstr(
            message, "discovery-dns-deprioritized=");
        unsigned count;
        if (deprioritized != NULL &&
            sscanf(deprioritized +
                       strlen("discovery-dns-deprioritized="),
                   "%u", &count) == 1) {
            run->discovery_dns_deprioritized = count;
        }
    }
    {
        const char *coalesced = strstr(message, "tcp-acks-coalesced=");
        unsigned count;
        if (coalesced != NULL &&
            sscanf(coalesced + strlen("tcp-acks-coalesced="),
                   "%u", &count) == 1) {
            run->tcp_acks_coalesced = count;
        }
    }
    if (strstr(message, "dns-rate=") != NULL &&
        strstr(message, "non-dns-rate=") != NULL) {
        ++run->rate_breakdowns;
    }
    if (run->options.role == UM_LIVE_CLIENT &&
        (strstr(message, "proxy token commit sequence=") != NULL ||
         strstr(message, "proxy packet token commit sequence=") != NULL)) {
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
    if (bus.offer_drop_armed != 0 && audio->endpoint == SIM_GATEWAY) {
        bus.offer_drop_armed = 0;
        ++bus.offer_drops;
        drop_write = 1;
    }
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
    if (bus.commit_drop_armed != 0 && audio->endpoint == SIM_CLIENT) {
        bus.commit_drop_armed = 0;
        ++bus.commit_drops;
        drop_write = 1;
    }
    if (bus.body_drop_armed != 0 &&
        bus.body_drop_endpoint == audio->endpoint) {
        bus.body_drop_armed = 0;
        ++bus.body_drops;
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
    bus.network_dns_retry_ready[network->endpoint] = 0;
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
    int dns_retry = 0;
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
           bus.network_dns_retry_ready[network->endpoint] == 0 &&
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
        bus.network_dns_retry_ready[network->endpoint] == 0 &&
        bus.network_background_remaining[network->endpoint] == 0u) {
        (void)pthread_mutex_unlock(&bus.mutex);
        return UM_ERR_TIMEOUT;
    }
    if (network->endpoint == SIM_CLIENT &&
        bus.network_targets_read[SIM_CLIENT] == 0u &&
        bus.network_input_ready[SIM_CLIENT] != 0) {
        --bus.network_input_ready[SIM_CLIENT];
        target_index = bus.network_targets_read[SIM_CLIENT]++;
        ++bus.network_dns_retry_ready[SIM_CLIENT];
        ++bus.network_dns_retries_injected;
        (void)pthread_cond_broadcast(&bus.changed);
    } else if (bus.network_dns_retry_ready[network->endpoint] != 0) {
        --bus.network_dns_retry_ready[network->endpoint];
        ++bus.network_dns_retry_reads[network->endpoint];
        dns_retry = 1;
        target_index = 0u;
    } else if (bus.network_input_ready[network->endpoint] != 0) {
        --bus.network_input_ready[network->endpoint];
        target_index = bus.network_targets_read[network->endpoint]++;
    } else if (bus.network_background_remaining[network->endpoint] != 0u) {
        background = 1;
        background_number =
            bus.network_background_remaining[network->endpoint];
        --bus.network_background_remaining[network->endpoint];
    } else {
        (void)pthread_mutex_unlock(&bus.mutex);
        return UM_ERR_TIMEOUT;
    }
    ++bus.network_reads[network->endpoint];
    (void)pthread_mutex_unlock(&bus.mutex);

    if (background != 0) {
        length = proxy_background_length(background_number);
        make_proxy_background(generated, network->endpoint,
                              background_number);
    } else {
        length = proxy_target_length(network->endpoint, target_index);
        make_proxy_target(generated, length, network->endpoint,
                          target_index);
        if (dns_retry != 0) {
            generated[16] = 8u;
            generated[17] = 8u;
            generated[18] = 8u;
            generated[19] = 8u;
            finalize_ipv4_checksums(generated, length);
        }
    }
    memcpy(packet, generated, length);
    *packet_length = length;
    return UM_OK;
}

int um_network_write(um_network *network, const uint8_t *packet,
                     size_t packet_length, unsigned timeout_ms)
{
    uint8_t expected[UM_NETWORK_MTU];
    size_t expected_length = 0u;
    unsigned target_index;
    unsigned target_candidate;
    unsigned background_number;
    int source_endpoint;
    int target_matches;
    int background_matches = 0;
    (void)timeout_ms;
    if (network == NULL || packet == NULL) {
        return UM_ERR_ARGUMENT;
    }
    (void)pthread_mutex_lock(&bus.mutex);
    if (bus.network_opened[network->endpoint] == 0) {
        (void)pthread_mutex_unlock(&bus.mutex);
        return UM_ERR_NETWORK;
    }
    ++bus.network_writes[network->endpoint];
    target_index = 0u;
    source_endpoint = network->endpoint == SIM_GATEWAY ? SIM_CLIENT
                                                        : SIM_GATEWAY;
    target_matches = 0;
    for (target_candidate = 0u;
         target_candidate < SIM_PROXY_PACKETS; ++target_candidate) {
        uint32_t bit = UINT32_C(1) << target_candidate;
        if ((bus.network_target_seen[network->endpoint] & bit) != 0u) {
            continue;
        }
        expected_length = proxy_target_length(source_endpoint,
                                               target_candidate);
        make_proxy_target(expected, expected_length, source_endpoint,
                          target_candidate);
        if (packet_length == expected_length &&
            memcmp(packet, expected, expected_length) == 0) {
            bus.network_target_seen[network->endpoint] |= bit;
            target_index = target_candidate;
            target_matches = 1;
            break;
        }
    }
    if (target_matches == 0) {
        for (background_number = 1u;
             background_number < SIM_BACKGROUND_PACKETS;
             ++background_number) {
            uint32_t bit = UINT32_C(1) << background_number;
            size_t background_length =
                proxy_background_length(background_number);
            if ((bus.network_background_seen[network->endpoint] & bit) !=
                0u) {
                continue;
            }
            make_proxy_background(expected, source_endpoint,
                                  background_number);
            if (packet_length == background_length &&
                memcmp(packet, expected, background_length) == 0) {
                bus.network_background_seen[network->endpoint] |= bit;
                ++bus.network_background_matches[network->endpoint];
                background_matches = 1;
                break;
            }
        }
    }
    if (target_matches != 0) {
        ++bus.network_matches[network->endpoint];
        if (network->endpoint == SIM_GATEWAY) {
            if (target_index == 0u) {
                bus.network_dns_response_delayed = 1;
                bus.network_input_ready[SIM_CLIENT] +=
                    SIM_PROXY_PACKETS - 1u;
                ++bus.network_dns_retry_ready[SIM_CLIENT];
                ++bus.network_dns_retries_injected;
            } else {
                ++bus.network_input_ready[SIM_GATEWAY];
            }
            if (target_index != 0u &&
                bus.network_background_matches[SIM_GATEWAY] <
                    SIM_FORWARDED_BACKGROUND_PACKETS) {
                bus.client_tcp_before_dns_drained = 1;
            }
        }
    }
    if (background_matches != 0 && network->endpoint == SIM_CLIENT &&
        bus.network_dns_response_delayed != 0) {
        bus.network_dns_response_delayed = 0;
        ++bus.network_input_ready[SIM_GATEWAY];
    }
    if (target_matches != 0 || background_matches != 0) {
        (void)pthread_cond_broadcast(&bus.changed);
    }
    (void)pthread_mutex_unlock(&bus.mutex);
    return target_matches != 0 || background_matches != 0 ? UM_OK
                                                           : UM_ERR_NETWORK;
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
        link_test == 0 ? SIM_FORWARDED_PACKETS : 0u;
    run->options.test_bytes = 256u;
    run->options.chunk_bytes = link_test != 0 ? 64u : UM_LIVE_MAX_BODY;
    run->options.retry_limit = 5u;
    run->options.discovery_interval_seconds = 0.4f;
    run->options.calibration_path = calibration_path;
}

static int run_pair(const char *label,
                    const um_channel_config *client_to_gateway,
                    const um_channel_config *gateway_to_client,
                    int link_test, int inject_offer_retry,
                    int inject_proxy_retry,
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
    bus.offer_drop_enabled = inject_offer_retry;
    bus.offer_drop_armed = 0;
    bus.offer_drops = 0u;
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
    bus.commit_drop_armed = 0;
    bus.commit_drops = 0u;
    bus.body_drop_endpoint = link_test == 0 ? SIM_GATEWAY : -1;
    bus.body_drop_armed = 0;
    bus.body_drops = 0u;
    memset(bus.network_opened, 0, sizeof(bus.network_opened));
    memset(bus.network_input_ready, 0, sizeof(bus.network_input_ready));
    memset(bus.network_dns_retry_ready, 0,
           sizeof(bus.network_dns_retry_ready));
    memset(bus.network_background_remaining, 0,
           sizeof(bus.network_background_remaining));
    memset(bus.network_dns_retry_reads, 0,
           sizeof(bus.network_dns_retry_reads));
    bus.network_dns_retries_injected = 0u;
    bus.network_dns_response_delayed = 0;
    bus.client_tcp_before_dns_drained = 0;
    memset(bus.network_targets_read, 0, sizeof(bus.network_targets_read));
    memset(bus.network_reads, 0, sizeof(bus.network_reads));
    memset(bus.network_writes, 0, sizeof(bus.network_writes));
    memset(bus.network_matches, 0, sizeof(bus.network_matches));
    memset(bus.network_background_matches, 0,
           sizeof(bus.network_background_matches));
    memset(bus.network_target_seen, 0,
           sizeof(bus.network_target_seen));
    memset(bus.network_background_seen, 0,
           sizeof(bus.network_background_seen));
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
        (inject_offer_retry != 0 &&
         (bus.offer_drops != 1u || gateway.offer_refreshes == 0u)) ||
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
              SIM_BACKGROUND_PACKETS + SIM_PROXY_PACKETS + 3u ||
          bus.network_reads[SIM_GATEWAY] !=
              SIM_BACKGROUND_PACKETS + SIM_PROXY_PACKETS ||
          bus.network_writes[SIM_CLIENT] != SIM_FORWARDED_PACKETS ||
          bus.network_writes[SIM_GATEWAY] != SIM_FORWARDED_PACKETS ||
          bus.network_matches[SIM_CLIENT] != SIM_PROXY_PACKETS ||
          bus.network_matches[SIM_GATEWAY] != SIM_PROXY_PACKETS ||
          bus.network_background_matches[SIM_CLIENT] !=
              SIM_FORWARDED_BACKGROUND_PACKETS ||
          bus.network_background_matches[SIM_GATEWAY] !=
              SIM_FORWARDED_BACKGROUND_PACKETS ||
          bus.network_dns_retries_injected != 3u ||
          bus.network_dns_retry_reads[SIM_CLIENT] != 3u ||
          client.dns_retries_suppressed != 3u ||
          gateway.dns_response_bursts == 0u ||
          bus.client_tcp_before_dns_drained == 0 ||
          client.multicast_dropped != 1u ||
          gateway.multicast_dropped != 1u ||
          client.broadcast_dropped != 1u ||
          gateway.broadcast_dropped != 1u ||
          client.stale_dns_icmp_dropped != 1u ||
          gateway.stale_dns_icmp_dropped != 1u ||
          client.discovery_dns_deprioritized != 1u ||
          gateway.discovery_dns_deprioritized != 1u ||
          client.tcp_acks_coalesced != 1u ||
          gateway.tcp_acks_coalesced != 1u ||
          client.rate_breakdowns == 0u ||
          gateway.rate_breakdowns == 0u)) ||
        (inject_proxy_retry != 0 &&
         (bus.proxy_drops != 2u || bus.begin_ack_drops != 1u ||
          bus.commit_drops != 1u ||
          client.proxy_start_retries + gateway.proxy_start_retries == 0u ||
          client.proxy_retries + gateway.proxy_retries == 0u ||
          client.commit_retries + gateway.commit_retries == 0u)) ||
        (link_test == 0 &&
         (client.packet_token_commits + gateway.packet_token_commits < 2u ||
          client.packet_token_accepts + gateway.packet_token_accepts < 2u ||
          client.dns_query_logs + gateway.dns_query_logs < 2u ||
          client.dns_response_logs + gateway.dns_response_logs < 2u ||
          client.multi_packet_batches == 0u ||
          gateway.multi_packet_batches == 0u || bus.body_drops != 1u ||
          client.body_stepdowns + gateway.body_stepdowns < 2u))) {
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
           "cache-skips=%u recovery-probes=%u offer-refreshes=%u "
           "dns-response-bursts=%u/%u\n",
           label,
           link_test != 0 ? "256+256-byte explicit link test"
                          : "queued/in-flight/completed DNS coalescing plus "
                            "TCP progress through DNS-saturated queues",
           client.adaptive_upgrades, gateway.adaptive_upgrades,
           client.baseline_selections, gateway.baseline_selections,
           client.verification_fallbacks + gateway.verification_fallbacks,
           expected_calibrations, client.cache_skips + gateway.cache_skips,
           client.recovery_probes + gateway.recovery_probes,
           gateway.offer_refreshes, client.dns_response_bursts,
           gateway.dns_response_bursts);
    return 0;
}

int main(void)
{
    um_channel_config v2_forward = um_channel_recorded_v2_config(0u);
    um_channel_config v2_reverse = um_channel_recorded_v2_config(1u);
    um_distortion_profile strong;
    um_modem_config cached;
    size_t cached_body_bytes;
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
                      0, 1, 1, 0, 0, 1, 0, 2u, client_path, gateway_path);
    if (status == 0) {
        found = 0;
        if (um_calibration_config_load(client_path, UM_LIVE_CLIENT, &cached,
                                       &cached_body_bytes, &found) != UM_OK ||
            found == 0 || cached_body_bytes != 384u) {
            fprintf(stderr, "client calibration cache was not saved\n");
            status = 1;
        }
    }
    if (status == 0) {
        found = 0;
        if (um_calibration_config_load(gateway_path, UM_LIVE_GATEWAY, &cached,
                                       &cached_body_bytes, &found) != UM_OK ||
            found == 0 || cached_body_bytes != UM_LIVE_MAX_BODY) {
            fprintf(stderr, "gateway calibration cache was not saved\n");
            status = 1;
        }
    }
    if (status == 0) {
        status = run_pair("both-cached-link-test", &v2_forward, &v2_reverse,
                          1, 0, 0, 0, 0, 0, 0, 0u, client_path,
                          gateway_path);
    }
    if (status == 0) {
        (void)remove(gateway_path);
        status = run_pair("one-side-missing-recovery", &v2_forward,
                          &v2_reverse, 1, 0, 0, 0, 0, 0, 1, 1u,
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
                              &strong.gateway_to_client, 1, 0, 0, 0, 1, 0,
                              0, 2u, client_path, gateway_path);
        }
    }

    (void)remove(client_path);
    (void)remove(gateway_path);
    free(bus.queued[SIM_CLIENT]);
    free(bus.queued[SIM_GATEWAY]);
    return status;
}
