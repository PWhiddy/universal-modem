#include "um.h"
#include "../src/live_wire.h"
#include "../src/traffic_policy.h"
#include "../src/um_internal.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static int tests_run = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                               \
            ++failures;                                                        \
            return;                                                            \
        }                                                                      \
    } while (0)

static uint32_t test_random_state = UINT32_C(0x91e10da5);

static uint32_t test_random(void)
{
    test_random_state ^= test_random_state << 13u;
    test_random_state ^= test_random_state >> 17u;
    test_random_state ^= test_random_state << 5u;
    return test_random_state;
}

static uint32_t policy_checksum_add(uint32_t sum, const uint8_t *bytes,
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

static uint16_t policy_checksum_finish(uint32_t sum)
{
    while ((sum >> 16u) != 0u) {
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16u);
    }
    return (uint16_t)~sum;
}

static void policy_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static size_t make_policy_dns_query(uint8_t *packet, size_t capacity,
                                    const char *name, uint16_t type)
{
    uint8_t *dns;
    size_t offset = 12u;
    const char *label = name;
    const char *position = name;
    size_t dns_length;
    size_t packet_length;
    uint16_t checksum;
    uint32_t sum;
    memset(packet, 0, capacity);
    while (1) {
        if (*position == '.' || *position == '\0') {
            size_t label_length = (size_t)(position - label);
            if (label_length == 0u || label_length > 63u ||
                28u + offset + label_length + 6u > capacity) {
                return 0u;
            }
            packet[28u + offset++] = (uint8_t)label_length;
            memcpy(&packet[28u + offset], label, label_length);
            offset += label_length;
            if (*position == '\0') {
                break;
            }
            label = position + 1;
        }
        ++position;
    }
    packet[28u + offset++] = 0u;
    policy_write_u16(&packet[28u + offset], type);
    offset += 2u;
    policy_write_u16(&packet[28u + offset], 1u);
    offset += 2u;
    dns_length = offset;
    packet_length = 28u + dns_length;
    packet[0] = 0x45u;
    policy_write_u16(&packet[2], (uint16_t)packet_length);
    packet[8] = 64u;
    packet[9] = 17u;
    packet[12] = 10u;
    packet[13] = 77u;
    packet[14] = 0u;
    packet[15] = 2u;
    packet[16] = 1u;
    packet[17] = 1u;
    packet[18] = 1u;
    packet[19] = 1u;
    policy_write_u16(&packet[20], 53000u);
    policy_write_u16(&packet[22], 53u);
    policy_write_u16(&packet[24], (uint16_t)(packet_length - 20u));
    dns = &packet[28];
    policy_write_u16(dns, UINT16_C(0x4d2a));
    policy_write_u16(&dns[2], UINT16_C(0x0100));
    policy_write_u16(&dns[4], 1u);
    checksum = policy_checksum_finish(policy_checksum_add(0u, packet, 20u));
    policy_write_u16(&packet[10], checksum);
    sum = policy_checksum_add(0u, &packet[12], 8u);
    sum += 17u;
    sum += (uint32_t)(packet_length - 20u);
    sum = policy_checksum_add(sum, &packet[20], packet_length - 20u);
    checksum = policy_checksum_finish(sum);
    policy_write_u16(&packet[26],
                     checksum == 0u ? UINT16_C(0xffff) : checksum);
    return packet_length;
}

static void test_quiet_traffic_policy(void)
{
    static const char *allowed[] = {
        "example.com",
        "google.com",
        "ocsp.apple.com",
        "valid.apple.com",
        "notapple.com"
    };
    static const char *blocked[] = {
        "news-edge.apple.com",
        "apple.com",
        "www.apple.com",
        "init.itunes.apple.com",
        "apps.mzstatic.com",
        "api.apple-cloudkit.com",
        "mask.icloud.com",
        "gateway.fe2.apple-dns.net",
        "detectportal.firefox.com",
        "firefox.settings.services.mozilla.com",
        "incoming.telemetry.mozilla.org",
        "mozilla.cloudflare-dns.com",
        "safebrowsing.googleapis.com",
        "one.one.one.one"
    };
    uint8_t query[512];
    uint8_t response[512];
    size_t query_length;
    size_t response_length = 0u;
    size_t index;
    um_traffic_policy_decision decision;
    uint32_t sum;
    ++tests_run;
    for (index = 0u; index < sizeof(allowed) / sizeof(allowed[0]); ++index) {
        query_length = make_policy_dns_query(query, sizeof(query),
                                             allowed[index], 1u);
        CHECK(query_length != 0u);
        CHECK(um_traffic_policy_decide(query, query_length, 1, 1,
                                       &decision) == 0);
        CHECK(decision.action == UM_TRAFFIC_POLICY_PASS);
    }
    for (index = 0u; index < sizeof(blocked) / sizeof(blocked[0]); ++index) {
        query_length = make_policy_dns_query(query, sizeof(query),
                                             blocked[index], 65u);
        CHECK(query_length != 0u);
        CHECK(um_traffic_policy_decide(query, query_length, 1, 1,
                                       &decision) == 0);
        CHECK(decision.action ==
              UM_TRAFFIC_POLICY_REJECT_BACKGROUND_DNS);
        CHECK(strcmp(decision.dns_name, blocked[index]) == 0);
    }
    query_length = make_policy_dns_query(query, sizeof(query),
                                         "news-edge.apple.com", 1u);
    CHECK(um_traffic_policy_decide(query, query_length, 1, 0, &decision) ==
          0);
    CHECK(decision.action == UM_TRAFFIC_POLICY_PASS);
    CHECK(um_traffic_policy_build_dns_rejection(
              query, query_length, response, sizeof(response),
              &response_length) == 0);
    CHECK(response_length == query_length);
    CHECK(memcmp(&response[12], &query[16], 4u) == 0);
    CHECK(memcmp(&response[16], &query[12], 4u) == 0);
    CHECK(response[20] == query[22] && response[21] == query[23]);
    CHECK(response[22] == query[20] && response[23] == query[21]);
    CHECK((response[30] & UINT8_C(0x80)) != 0u);
    CHECK((response[31] & UINT8_C(0x0f)) == 3u);
    CHECK(policy_checksum_finish(policy_checksum_add(0u, response, 20u)) ==
          0u);
    sum = policy_checksum_add(0u, &response[12], 8u);
    sum += 17u;
    sum += (uint32_t)(response_length - 20u);
    sum = policy_checksum_add(sum, &response[20], response_length - 20u);
    CHECK(policy_checksum_finish(sum) == 0u);

    query_length = make_policy_dns_query(
        query, sizeof(query), "b._dns-sd._udp.0.0.77.10.in-addr.arpa", 12u);
    CHECK(um_traffic_policy_is_tunnel_discovery_dns(query, query_length) !=
          0);
    CHECK(um_traffic_policy_decide(query, query_length, 1, 1, &decision) ==
          0);
    CHECK(decision.action == UM_TRAFFIC_POLICY_REJECT_BACKGROUND_DNS);
}

static void test_crc(void)
{
    static const uint8_t check[] = "123456789";
    ++tests_run;
    CHECK(um_crc32(check, sizeof(check) - 1u) == UINT32_C(0xcbf43926));
    CHECK(um_crc16(check, sizeof(check) - 1u) == UINT16_C(0x29b1));
    CHECK(strcmp(um_status_string(UM_ERR_RELIABILITY),
                 "insufficient reliability margin") == 0);
}

static void test_fft_round_trip(void)
{
    um_complex values[UM_FFT_SIZE];
    um_complex original[UM_FFT_SIZE];
    size_t i;
    float maximum_error = 0.0f;
    ++tests_run;
    for (i = 0u; i < UM_FFT_SIZE; ++i) {
        values[i].re = sinf(2.0f * UM_PI * 7.0f * (float)i /
                            (float)UM_FFT_SIZE) +
                       0.2f * cosf(2.0f * UM_PI * 31.0f * (float)i /
                                   (float)UM_FFT_SIZE);
        values[i].im = 0.1f * sinf(2.0f * UM_PI * 3.0f * (float)i /
                                   (float)UM_FFT_SIZE);
    }
    memcpy(original, values, sizeof(values));
    CHECK(um_fft(values, UM_FFT_SIZE, 0) == UM_OK);
    CHECK(um_fft(values, UM_FFT_SIZE, 1) == UM_OK);
    for (i = 0u; i < UM_FFT_SIZE; ++i) {
        float error = hypotf(values[i].re - original[i].re,
                             values[i].im - original[i].im);
        if (error > maximum_error) {
            maximum_error = error;
        }
    }
    CHECK(maximum_error < 2.0e-5f);
}

static void test_qam_constellations(void)
{
    const unsigned bit_counts[] = {2u, 4u, 6u};
    size_t mode;
    ++tests_run;
    for (mode = 0u; mode < sizeof(bit_counts) / sizeof(bit_counts[0]); ++mode) {
        unsigned bit_count = bit_counts[mode];
        unsigned points = 1u << bit_count;
        unsigned label;
        double energy = 0.0;
        for (label = 0u; label < points; ++label) {
            uint8_t bits[6];
            float soft[6];
            um_complex point;
            unsigned bit;
            for (bit = 0u; bit < bit_count; ++bit) {
                bits[bit] =
                    (uint8_t)((label >> (bit_count - 1u - bit)) & 1u);
            }
            point = um_qam_map(bits, bit_count);
            energy += (double)point.re * point.re +
                      (double)point.im * point.im;
            CHECK(um_qam_soft_demod(point, bit_count, soft) == UM_OK);
            for (bit = 0u; bit < bit_count; ++bit) {
                CHECK((soft[bit] >= 0.0f ? 1u : 0u) == bits[bit]);
            }
        }
        CHECK(fabs(energy / (double)points - 1.0) < 1.0e-6);
    }
}

static void test_fec_rates(void)
{
    const um_fec_rate rates[] = {UM_FEC_RATE_1_2, UM_FEC_RATE_2_3,
                                 UM_FEC_RATE_3_4};
    uint8_t input[511];
    uint8_t decoded[511];
    size_t rate_index;
    size_t i;
    ++tests_run;
    for (i = 0u; i < sizeof(input); ++i) {
        input[i] = (uint8_t)(test_random() & 1u);
    }
    for (rate_index = 0u;
         rate_index < sizeof(rates) / sizeof(rates[0]); ++rate_index) {
        size_t count = um_fec_encoded_bits(sizeof(input), rates[rate_index]);
        uint8_t *coded = (uint8_t *)malloc(count);
        float *soft = (float *)malloc(count * sizeof(*soft));
        size_t actual = 0u;
        CHECK(coded != NULL && soft != NULL);
        CHECK(um_fec_encode(input, sizeof(input), rates[rate_index], coded,
                            count, &actual) == UM_OK);
        CHECK(actual == count);
        for (i = 0u; i < count; ++i) {
            soft[i] = coded[i] != 0u ? 4.0f : -4.0f;
        }
        CHECK(um_fec_decode(soft, count, sizeof(input), rates[rate_index],
                            decoded, sizeof(decoded)) == UM_OK);
        CHECK(memcmp(input, decoded, sizeof(input)) == 0);
        free(soft);
        free(coded);
    }
}

static void test_interleaved_burst(void)
{
    enum { DATA_BITS = 1200 };
    uint8_t input[DATA_BITS];
    uint8_t decoded[DATA_BITS];
    size_t fec_count = um_fec_encoded_bits(DATA_BITS, UM_FEC_RATE_1_2);
    uint8_t *coded = (uint8_t *)malloc(fec_count);
    uint8_t *interleaved = (uint8_t *)malloc(fec_count);
    float *received = (float *)malloc(fec_count * sizeof(*received));
    float *ordered = (float *)malloc(fec_count * sizeof(*ordered));
    size_t actual = 0u;
    size_t i;
    ++tests_run;
    CHECK(coded != NULL && interleaved != NULL && received != NULL &&
          ordered != NULL);
    for (i = 0u; i < DATA_BITS; ++i) {
        input[i] = (uint8_t)(test_random() & 1u);
    }
    CHECK(um_fec_encode(input, DATA_BITS, UM_FEC_RATE_1_2, coded, fec_count,
                        &actual) == UM_OK);
    CHECK(um_interleave_bits(coded, interleaved, fec_count) == UM_OK);
    for (i = 0u; i < fec_count; ++i) {
        received[i] = interleaved[i] != 0u ? 5.0f : -5.0f;
    }
    for (i = fec_count / 3u; i < fec_count / 3u + 18u; ++i) {
        received[i] = -received[i];
    }
    CHECK(um_deinterleave_soft(received, ordered, fec_count) == UM_OK);
    CHECK(um_fec_decode(ordered, fec_count, DATA_BITS, UM_FEC_RATE_1_2,
                        decoded, DATA_BITS) == UM_OK);
    CHECK(memcmp(input, decoded, DATA_BITS) == 0);
    free(ordered);
    free(received);
    free(interleaved);
    free(coded);
}

static int frame_round_trip(const um_modem_config *config,
                            const um_channel_config *channel,
                            size_t byte_count, um_rx_metrics *metrics)
{
    uint8_t *input = (uint8_t *)malloc(byte_count == 0u ? 1u : byte_count);
    uint8_t *decoded = (uint8_t *)malloc(byte_count == 0u ? 1u : byte_count);
    float *transmitted = NULL;
    float *received = NULL;
    size_t transmitted_count = 0u;
    size_t received_count = 0u;
    size_t decoded_count = 0u;
    uint16_t sequence = 0u;
    size_t i;
    int status;
    if (input == NULL || decoded == NULL) {
        free(decoded);
        free(input);
        return UM_ERR_MEMORY;
    }
    for (i = 0u; i < byte_count; ++i) {
        input[i] = (uint8_t)test_random();
    }
    status = um_modulate_frame(config, input, byte_count, UINT16_C(0xb17e),
                               &transmitted, &transmitted_count);
    if (status == UM_OK) {
        status = um_channel_apply(transmitted, transmitted_count, channel,
                                  &received, &received_count);
    }
    if (status == UM_OK) {
        status = um_demodulate_frame(config, received, received_count, decoded,
                                     byte_count, &decoded_count, &sequence,
                                     metrics);
    }
    if (status == UM_OK &&
        (decoded_count != byte_count || sequence != UINT16_C(0xb17e) ||
         memcmp(input, decoded, byte_count) != 0)) {
        status = UM_ERR_CRC;
    }
    free(received);
    free(transmitted);
    free(decoded);
    free(input);
    return status;
}

static void test_clean_frames(void)
{
    const unsigned qam_bits[] = {2u, 4u, 6u};
    const um_fec_rate rates[] = {UM_FEC_RATE_1_2, UM_FEC_RATE_2_3,
                                 UM_FEC_RATE_3_4};
    um_channel_config channel = um_channel_default_config();
    size_t qam;
    size_t rate;
    ++tests_run;
    for (qam = 0u; qam < sizeof(qam_bits) / sizeof(qam_bits[0]); ++qam) {
        for (rate = 0u; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
            um_modem_config config = um_modem_default_config();
            um_rx_metrics metrics;
            config.qam_bits = qam_bits[qam];
            config.fec_rate = rates[rate];
            CHECK(frame_round_trip(&config, &channel, 257u, &metrics) == UM_OK);
            CHECK(metrics.sync_correlation > 0.99f);
            CHECK(metrics.evm_rms < 2.0e-4f);
        }
    }
    {
        um_modem_config config = um_modem_default_config();
        CHECK(frame_round_trip(&config, &channel, 0u, NULL) == UM_OK);
    }
}

static void test_impaired_frames(void)
{
    um_modem_config config = um_modem_default_config();
    um_channel_config channel = um_channel_default_config();
    um_rx_metrics metrics;
    ++tests_run;

    config.qam_bits = 2u;
    config.fec_rate = UM_FEC_RATE_1_2;
    channel.leading_silence = 713u;
    channel.gain = 0.075f;
    channel.noise_stddev = 0.00045f;
    channel.echo_delay = 15u;
    channel.echo_gain = 0.38f;
    channel.clip_level = 0.018f;
    channel.random_seed = UINT32_C(0x10203040);
    CHECK(frame_round_trip(&config, &channel, 600u, &metrics) == UM_OK);
    CHECK(metrics.frame_start >= channel.leading_silence + UM_SYNC_LEAD - 2u);
    CHECK(metrics.sync_correlation > 0.55f);

    config.qam_bits = 4u;
    channel.leading_silence = 91u;
    channel.gain = 1.6f;
    channel.noise_stddev = 0.003f;
    channel.echo_delay = 19u;
    channel.echo_gain = 0.22f;
    channel.clip_level = 0.70f;
    channel.random_seed = UINT32_C(0xaabbccdd);
    CHECK(frame_round_trip(&config, &channel, 400u, &metrics) == UM_OK);

    config.qam_bits = 2u;
    channel.leading_silence = 233u;
    channel.gain = 0.22f;
    channel.noise_stddev = 0.0012f;
    channel.echo_delay = 11u;
    channel.echo_gain = 0.31f;
    channel.clip_level = 0.0f;
    channel.dropout_start = channel.leading_silence + UM_SYNC_LEAD +
                            config.sync_samples + config.sync_gap +
                            24u * (config.fft_size + config.cyclic_prefix);
    channel.dropout_length = 96u;
    channel.random_seed = UINT32_C(0x31415926);
    CHECK(frame_round_trip(&config, &channel, 700u, &metrics) == UM_OK);
}

static void test_input_normalization(void)
{
    um_modem_config config = um_modem_default_config();
    uint8_t payload[192];
    uint8_t decoded[sizeof(payload)];
    float *transmitted = NULL;
    float *received = NULL;
    size_t transmitted_count = 0u;
    size_t received_count;
    size_t decoded_count = 0u;
    uint16_t sequence = 0u;
    um_rx_metrics metrics;
    const float gains[] = {0.0001f, 0.35f, 1.10f};
    const float offsets[] = {0.015f, -0.08f, 0.05f};
    size_t trial;
    size_t i;

    ++tests_run;
    config.qam_bits = 2u;
    config.fec_rate = UM_FEC_RATE_1_2;
    for (i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)test_random();
    }
    CHECK(um_modulate_frame(&config, payload, sizeof(payload), 0x4815u,
                            &transmitted, &transmitted_count) == UM_OK);
    received_count = transmitted_count + 317u;
    received = (float *)malloc(received_count * sizeof(*received));
    CHECK(received != NULL);
    for (trial = 0u; trial < sizeof(gains) / sizeof(gains[0]); ++trial) {
        for (i = 0u; i < received_count; ++i) {
            float value = i < 317u ? 0.0f : transmitted[i - 317u];
            received[i] = value * gains[trial] + offsets[trial];
        }
        memset(&metrics, 0, sizeof(metrics));
        CHECK(um_demodulate_frame(&config, received, received_count, decoded,
                                  sizeof(decoded), &decoded_count, &sequence,
                                  &metrics) == UM_OK);
        CHECK(decoded_count == sizeof(payload));
        CHECK(sequence == 0x4815u);
        CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
        CHECK(metrics.sync_correlation > 0.99f);
        CHECK(metrics.normalization_gain > 0.0f);
        CHECK(metrics.clipped_sample_fraction == 0.0f);
        if (trial == 0u) {
            CHECK(metrics.normalization_gain > 1000.0f);
        } else if (trial + 1u == sizeof(gains) / sizeof(gains[0])) {
            CHECK(metrics.normalization_gain < 0.5f);
        }
    }
    free(received);
    free(transmitted);
}

static void test_robust_low_level_colored_noise(void)
{
    um_modem_config config = um_modem_robust_config();
    um_channel_config channel = um_channel_default_config();
    uint8_t payload[128];
    uint8_t decoded[sizeof(payload)];
    float *transmitted = NULL;
    float *received = NULL;
    size_t transmitted_count = 0u;
    size_t received_count = 0u;
    size_t decoded_count = 0u;
    uint16_t sequence = 0u;
    um_rx_metrics metrics;
    size_t i;

    ++tests_run;
    for (i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)test_random();
    }
    CHECK(config.first_bin == 64u);
    CHECK(config.last_bin == 298u);
    CHECK(config.symbol_repetitions == 2u);
    CHECK(um_modulate_frame(&config, payload, sizeof(payload), 0x6c21u,
                            &transmitted, &transmitted_count) == UM_OK);
    channel.leading_silence = 839u;
    channel.gain = 0.012f;
    channel.noise_stddev = 0.0012f;
    channel.echo_delay = 731u;
    channel.echo_gain = 0.48f;
    channel.random_seed = UINT32_C(0x2408e113);
    CHECK(um_channel_apply(transmitted, transmitted_count, &channel,
                           &received, &received_count) == UM_OK);
    for (i = 0u; i < received_count; ++i) {
        float time = (float)i / (float)UM_SAMPLE_RATE;
        received[i] += 0.008f * sinf(2.0f * UM_PI * 117.0f * time) +
                       0.003f * sinf(2.0f * UM_PI * 347.0f * time);
    }
    memset(&metrics, 0, sizeof(metrics));
    CHECK(um_demodulate_frame(&config, received, received_count, decoded,
                              sizeof(decoded), &decoded_count, &sequence,
                              &metrics) == UM_OK);
    CHECK(decoded_count == sizeof(payload));
    CHECK(sequence == 0x6c21u);
    CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
    printf("robust low-level: peak=%.1f dBFS sync=%.3f snr=%.1f dB "
           "evm=%.3f\n",
           20.0 * log10((double)metrics.input_peak),
           metrics.sync_correlation, metrics.estimated_snr_db,
           metrics.evm_rms);
    CHECK(metrics.input_peak < 0.03f);
    CHECK(metrics.sync_correlation > 0.35f);
    CHECK(metrics.evm_rms < 0.25f);
    CHECK(um_modem_metrics_have_margin(&config, &metrics));
    CHECK(metrics.ofdm_symbols > config.training_symbols);
    free(received);
    free(transmitted);
}

static void test_recorded_v2_async_session(void)
{
    um_modem_config robust = um_modem_robust_config();
    um_session_simulation_config config =
        um_session_simulation_default_config();
    um_session_simulation_result result;
    um_rx_metrics forward_metrics;
    um_rx_metrics reverse_metrics;

    ++tests_run;
    config.client_to_gateway = um_channel_recorded_v2_config(0u);
    config.gateway_to_client = um_channel_recorded_v2_config(1u);
    config.client_payload_bytes = 1024u;
    config.gateway_payload_bytes = 1024u;
    config.frame_payload_bytes = 128u;
    config.blackout_after_data_seconds = 0.0f;
    config.blackout_duration_seconds = 0.0f;
    config.retry_limit = 5u;
    config.reconnect_limit = 4u;
    config.random_seed = UINT32_C(0x7632726d);

    memset(&forward_metrics, 0, sizeof(forward_metrics));
    memset(&reverse_metrics, 0, sizeof(reverse_metrics));
    CHECK(frame_round_trip(&robust, &config.client_to_gateway, 128u,
                           &forward_metrics) == UM_OK);
    CHECK(frame_round_trip(&robust, &config.gateway_to_client, 128u,
                           &reverse_metrics) == UM_OK);
    printf("v2 channel: c2g peak=%.1fdBFS sync=%.3f snr=%.1fdB evm=%.3f "
           "g2c peak=%.1fdBFS sync=%.3f snr=%.1fdB evm=%.3f\n",
           20.0 * log10((double)forward_metrics.input_peak),
           forward_metrics.sync_correlation,
           forward_metrics.estimated_snr_db, forward_metrics.evm_rms,
           20.0 * log10((double)reverse_metrics.input_peak),
           reverse_metrics.sync_correlation,
           reverse_metrics.estimated_snr_db, reverse_metrics.evm_rms);
    /* v2 measured 0.02017 peak and approximately 15.1 dB training SNR. */
    CHECK(forward_metrics.input_peak <= 0.0205f);
    CHECK(reverse_metrics.input_peak <= 0.0205f);
    CHECK(forward_metrics.estimated_snr_db <= 15.1f);
    CHECK(reverse_metrics.estimated_snr_db <= 15.1f);
    CHECK(um_modem_metrics_have_baseline_margin(&forward_metrics));
    CHECK(um_modem_metrics_have_baseline_margin(&reverse_metrics));

    CHECK(um_simulate_session(&config, &result, NULL, NULL) == UM_OK);
    printf("v2 async session: bytes=%zu/%zu discoveries=%zu calibrations=%zu "
           "candidates=%zu verification=%zu retries=%zu reconnects=%zu "
           "modes=%u/%u-qam repeats=%u/%u\n",
           result.gateway_received_bytes, result.client_received_bytes,
           result.discovery_requests, result.calibrations_completed,
           result.calibration_candidates,
           result.calibration_verification_frames, result.retries,
           result.reconnects,
           1u << result.client_to_gateway_config.qam_bits,
           1u << result.gateway_to_client_config.qam_bits,
           result.client_to_gateway_config.symbol_repetitions,
           result.gateway_to_client_config.symbol_repetitions);
    CHECK(result.final_connected != 0);
    CHECK(result.discovery_requests >= 1u);
    CHECK(result.offers >= 1u);
    CHECK(result.confirmations >= 1u);
    CHECK(result.calibrations_completed == 2u);
    CHECK(result.calibration_candidates >= 2u);
    CHECK(result.calibration_candidates <=
          2u * um_calibration_search_budget(0));
    CHECK(result.calibration_verification_frames >= 6u);
    CHECK(result.gateway_received_bytes == config.client_payload_bytes);
    CHECK(result.client_received_bytes == config.gateway_payload_bytes);
    CHECK(result.data_frames >= 16u);
    CHECK(result.acknowledgements >= 16u);
    CHECK(um_modem_config_validate(&result.client_to_gateway_config) == UM_OK);
    CHECK(um_modem_config_validate(&result.gateway_to_client_config) == UM_OK);
    CHECK(um_calibration_payload_rate(&result.client_to_gateway_config, 128u) >=
          um_calibration_payload_rate(&robust, 128u));
    CHECK(um_calibration_payload_rate(&result.gateway_to_client_config, 128u) >=
          um_calibration_payload_rate(&robust, 128u));
}

static void test_room_reverb_and_clock_drift(void)
{
    static const unsigned delays[] = {0u, 127u, 361u, 593u, 731u};
    static const float tap_gains[] = {0.65f, 1.0f, -0.31f, 0.22f, -0.14f};
    um_modem_config config = um_modem_default_config();
    uint8_t payload[1024];
    uint8_t decoded[sizeof(payload)];
    float *transmitted = NULL;
    float *reverberant = NULL;
    float *drifted = NULL;
    size_t transmitted_count = 0u;
    size_t reverberant_count;
    size_t drifted_count;
    size_t decoded_count = 0u;
    uint16_t sequence = 0u;
    um_rx_metrics metrics;
    const size_t leading = 509u;
    const double rate_ratio = 1.00020;
    size_t i;
    size_t tap;

    ++tests_run;
    config.qam_bits = 2u;
    config.fec_rate = UM_FEC_RATE_1_2;
    CHECK(config.cyclic_prefix > delays[sizeof(delays) / sizeof(delays[0]) -
                                         1u]);
    for (i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)test_random();
    }
    CHECK(um_modulate_frame(&config, payload, sizeof(payload), 0x7219u,
                            &transmitted, &transmitted_count) == UM_OK);
    reverberant_count = leading + transmitted_count +
                        delays[sizeof(delays) / sizeof(delays[0]) - 1u];
    reverberant = (float *)calloc(reverberant_count, sizeof(*reverberant));
    CHECK(reverberant != NULL);
    for (i = 0u; i < transmitted_count; ++i) {
        for (tap = 0u; tap < sizeof(delays) / sizeof(delays[0]); ++tap) {
            reverberant[leading + i + delays[tap]] +=
                0.018f * tap_gains[tap] * transmitted[i];
        }
    }
    for (i = 0u; i < reverberant_count; ++i) {
        reverberant[i] +=
            ((float)(test_random() & 0xffffu) / 32768.0f - 1.0f) * 0.0010f;
    }
    drifted_count = (size_t)((double)reverberant_count * rate_ratio);
    drifted = (float *)malloc(drifted_count * sizeof(*drifted));
    CHECK(drifted != NULL);
    for (i = 0u; i < drifted_count; ++i) {
        double source = (double)i / rate_ratio;
        size_t first = (size_t)source;
        float fraction = (float)(source - (double)first);
        float a = first < reverberant_count ? reverberant[first] : 0.0f;
        float b = first + 1u < reverberant_count
                      ? reverberant[first + 1u]
                      : a;
        drifted[i] = a + (b - a) * fraction;
    }
    CHECK(um_demodulate_frame(&config, drifted, drifted_count, decoded,
                              sizeof(decoded), &decoded_count, &sequence,
                              &metrics) == UM_OK);
    CHECK(decoded_count == sizeof(payload));
    CHECK(sequence == 0x7219u);
    CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
    CHECK(metrics.normalization_gain > 10.0f);
    CHECK(metrics.sync_correlation > 0.50f);
    free(drifted);
    free(reverberant);
    free(transmitted);
}

static void test_default_calibration(void)
{
    um_channel_config channel = um_channel_default_config();
    um_modem_config balanced = um_modem_default_config();
    um_calibration_result result;
    ++tests_run;
    CHECK(balanced.first_bin == 64u);
    CHECK(balanced.last_bin == 362u);
    CHECK(balanced.qam_bits == 2u);
    CHECK(balanced.fec_rate == UM_FEC_RATE_2_3);
    CHECK(balanced.cyclic_prefix == 1024u);
    CHECK(balanced.sync_samples == 1536u);
    CHECK(balanced.sync_gap == 2560u);
    CHECK(balanced.symbol_repetitions == 1u);
    channel.leading_silence = 121u;
    channel.gain = 0.26f;
    channel.noise_stddev = 0.0015f;
    channel.echo_delay = 13u;
    channel.echo_gain = 0.29f;
    channel.random_seed = UINT32_C(0x5eed1234);
    CHECK(um_calibrate_simulated(&channel, 0, &result, NULL, NULL) == UM_OK);
    CHECK(result.candidates_tested > 1u);
    CHECK(result.candidates_tested <= um_calibration_search_budget(0));
    CHECK(result.candidates_viable > 0u);
    CHECK(result.candidates_viable <= result.candidates_tested);
    CHECK(result.candidates_verified > 0u);
    CHECK(result.verification_frames >= 3u);
    CHECK(result.success_rate == 1.0f);
    CHECK(result.estimated_seconds * 2.0f > 10.0f);
    CHECK(result.estimated_seconds * 2.0f < 25.0f);
    CHECK(result.payload_bps > 1000.0f);
    CHECK(um_modem_config_validate(&result.config) == UM_OK);
}

static void test_distortion_profiles(void)
{
    um_distortion_profile previous;
    um_distortion_profile current;
    size_t count = um_distortion_profile_count();
    size_t level;
    int saw_all_acoustic = 0;
    ++tests_run;
    CHECK(count >= 6u);
    CHECK(um_distortion_profile_get(0u, &previous) == UM_OK);
    CHECK(previous.impairment_mask == 0u);
    CHECK(previous.client_to_gateway.gain == 1.0f);
    CHECK(previous.gateway_to_client.gain == 1.0f);
    CHECK(previous.client_to_gateway.noise_stddev == 0.0f);
    CHECK(previous.client_to_gateway.dropout_length == 0u);
    for (level = 1u; level < count; ++level) {
        CHECK(um_distortion_profile_get(level, &current) == UM_OK);
        CHECK((current.impairment_mask | previous.impairment_mask) ==
              current.impairment_mask);
        if ((current.impairment_mask & UM_IMPAIR_ACOUSTIC_ALL) ==
            UM_IMPAIR_ACOUSTIC_ALL) {
            saw_all_acoustic = 1;
            CHECK(current.client_to_gateway.dropout_length > 0u);
            CHECK(current.client_to_gateway.clip_level > 0.0f);
            CHECK(current.client_to_gateway.noise_stddev > 0.0f ||
                  level + 1u == count);
        }
        previous = current;
    }
    CHECK(saw_all_acoustic != 0);
    CHECK(previous.client_to_gateway.gain == 0.0f);
    CHECK(previous.gateway_to_client.gain == 0.0f);
    CHECK(um_distortion_profile_get(count, &current) == UM_ERR_ARGUMENT);
}

static void test_calibration_distortion_ladder(void)
{
    um_distortion_ladder_result result;
    um_distortion_profile last_passing;
    ++tests_run;
    CHECK(um_run_calibration_distortion_ladder(0, &result, NULL, NULL) ==
          UM_OK);
    CHECK(result.passes_passed >= 5u);
    CHECK(result.passes_attempted == result.passes_passed + 1u);
    CHECK(result.first_failed_level == result.passes_passed);
    CHECK(result.first_failure_status != UM_OK);
    CHECK((result.impairments_exercised & UM_IMPAIR_ACOUSTIC_ALL) ==
          UM_IMPAIR_ACOUSTIC_ALL);
    CHECK(result.calibrations_run >= result.passes_passed * 2u + 1u);
    CHECK(result.candidates_tested >= result.calibrations_run);
    CHECK(result.verification_frames >= result.passes_passed * 6u);
    CHECK(um_distortion_profile_get(result.passes_passed - 1u,
                                    &last_passing) == UM_OK);
    CHECK((last_passing.impairment_mask & UM_IMPAIR_ACOUSTIC_ALL) ==
          UM_IMPAIR_ACOUSTIC_ALL);
}

static double out_of_band_ratio(const float *samples, size_t sample_count,
                                const um_modem_config *config)
{
    size_t start = UM_SYNC_LEAD + config->sync_samples + config->sync_gap;
    size_t useful = sample_count > start + 64u ? sample_count - start - 64u : 0u;
    size_t fft_count = 1u;
    um_complex *spectrum;
    size_t i;
    double total = 0.0;
    double outside = 0.0;
    while (fft_count < useful) {
        fft_count <<= 1u;
    }
    spectrum = (um_complex *)calloc(fft_count, sizeof(*spectrum));
    if (spectrum == NULL) {
        return 1.0;
    }
    for (i = 0u; i < useful; ++i) {
        spectrum[i].re = samples[start + i];
    }
    if (um_fft(spectrum, fft_count, 0) != UM_OK) {
        free(spectrum);
        return 1.0;
    }
    for (i = 1u; i < fft_count / 2u; ++i) {
        double power = um_cabs2(spectrum[i]);
        double bin_at_modem_fft = (double)i * (double)config->fft_size /
                                  (double)fft_count;
        total += power;
        if (bin_at_modem_fft < (double)config->first_bin - 0.75 ||
            bin_at_modem_fft > (double)config->last_bin + 0.75) {
            outside += power;
        }
    }
    free(spectrum);
    return total > 0.0 ? outside / total : 1.0;
}

static void test_boundary_window(void)
{
    uint8_t payload[180];
    um_modem_config rectangular = um_modem_default_config();
    um_modem_config windowed = rectangular;
    float *plain_samples = NULL;
    float *windowed_samples = NULL;
    size_t plain_count = 0u;
    size_t windowed_count = 0u;
    double plain_leakage;
    double windowed_leakage;
    float peak = 0.0f;
    float training_peak = 0.0f;
    double training_energy = 0.0;
    double header_energy = 0.0;
    double payload_energy = 0.0;
    size_t over_full_scale = 0u;
    size_t i;
    ++tests_run;
    for (i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)test_random();
    }
    rectangular.window_samples = 0u;
    windowed.window_samples = 64u;
    CHECK(um_modulate_frame(&rectangular, payload, sizeof(payload), 1u,
                            &plain_samples, &plain_count) == UM_OK);
    CHECK(um_modulate_frame(&windowed, payload, sizeof(payload), 1u,
                            &windowed_samples, &windowed_count) == UM_OK);
    plain_leakage = out_of_band_ratio(plain_samples, plain_count, &rectangular);
    windowed_leakage =
        out_of_band_ratio(windowed_samples, windowed_count, &windowed);
    for (i = 0u; i < windowed_count; ++i) {
        float magnitude = fabsf(windowed_samples[i]);
        if (magnitude > peak) {
            peak = magnitude;
        }
        if (magnitude >= 1.0f) {
            ++over_full_scale;
        }
    }
    {
        size_t training_start = UM_SYNC_LEAD + windowed.sync_samples +
                                windowed.sync_gap + windowed.cyclic_prefix;
        size_t hop = windowed.fft_size + windowed.cyclic_prefix;
        size_t header_start = training_start + windowed.training_symbols * hop;
        size_t payload_start = header_start + hop;
        for (i = 0u; i < windowed.fft_size; ++i) {
            float value = windowed_samples[training_start + i];
            float magnitude = fabsf(value);
            training_energy += (double)value * value;
            header_energy += (double)windowed_samples[header_start + i] *
                             windowed_samples[header_start + i];
            payload_energy += (double)windowed_samples[payload_start + i] *
                              windowed_samples[payload_start + i];
            if (magnitude > training_peak) {
                training_peak = magnitude;
            }
        }
    }
    printf("spectral leakage: rectangular %.2f dB, windowed %.2f dB\n",
           10.0 * log10(plain_leakage), 10.0 * log10(windowed_leakage));
    printf("waveform peak=%.3f full-scale-overflows=%zu\n", peak,
           over_full_scale);
    printf("training crest=%.2f peak=%.3f rms=%.3f\n",
           training_peak /
               sqrt(training_energy / (double)windowed.fft_size),
           training_peak,
           sqrt(training_energy / (double)windowed.fft_size));
    printf("symbol rms: training=%.3f header=%.3f payload=%.3f\n",
           sqrt(training_energy / (double)windowed.fft_size),
           sqrt(header_energy / (double)windowed.fft_size),
           sqrt(payload_energy / (double)windowed.fft_size));
    CHECK(peak <= 0.901f);
    CHECK(over_full_scale == 0u);
    CHECK(training_peak /
              sqrt(training_energy / (double)windowed.fft_size) <
          2.5);
    CHECK(fabs(sqrt(training_energy / (double)windowed.fft_size) - 0.24) <
          0.005);
    CHECK(sqrt(header_energy / (double)windowed.fft_size) > 0.20);
    CHECK(sqrt(payload_energy / (double)windowed.fft_size) > 0.20);
    CHECK(windowed_leakage < plain_leakage);
    free(windowed_samples);
    free(plain_samples);
}

static double symbol_rms(const float *samples, size_t core, size_t fft_size)
{
    double energy = 0.0;
    size_t i;
    for (i = 0u; i < fft_size; ++i) {
        energy += (double)samples[core + i] * samples[core + i];
    }
    return sqrt(energy / (double)fft_size);
}

static void test_control_symbol_power(void)
{
    um_modem_config config = um_modem_robust_config();
    uint8_t payload[UM_LIVE_WIRE_HEADER_SIZE] = {0u};
    float *samples = NULL;
    size_t sample_count = 0u;
    size_t hop = config.fft_size + config.cyclic_prefix;
    size_t ofdm_start = UM_SYNC_LEAD + config.sync_samples + config.sync_gap;
    size_t training_core = ofdm_start + config.cyclic_prefix;
    size_t header_core = training_core + config.training_symbols * hop;
    size_t payload_core =
        header_core + config.symbol_repetitions * hop;
    double training_level;
    double header_level;
    double payload_level;

    ++tests_run;
    CHECK(um_modulate_frame(&config, payload, sizeof(payload), 7u, &samples,
                            &sample_count) == UM_OK);
    CHECK(payload_core + config.fft_size <= sample_count);
    training_level = symbol_rms(samples, training_core, config.fft_size);
    header_level = symbol_rms(samples, header_core, config.fft_size);
    payload_level = symbol_rms(samples, payload_core, config.fft_size);
    printf("control symbol rms: training=%.3f header=%.3f payload=%.3f\n",
           training_level, header_level, payload_level);
    CHECK(training_level > 0.23 && training_level < 0.25);
    CHECK(header_level > 0.20 && header_level < 0.25);
    CHECK(payload_level > 0.20 && payload_level < 0.25);
    CHECK(header_level / training_level > 0.83);
    CHECK(payload_level / training_level > 0.83);
    free(samples);
}

static void test_reliability_margin(void)
{
    um_modem_config config = um_modem_robust_config();
    um_modem_config recovered = config;
    um_modem_config optimized = config;
    um_rx_metrics metrics = {0};

    ++tests_run;
    metrics.sync_correlation = 0.43f;
    metrics.estimated_snr_db = 17.5f;
    metrics.evm_rms = 0.741f;
    CHECK(um_modem_metrics_have_baseline_margin(&metrics));
    CHECK(!um_modem_metrics_have_margin(&config, &metrics));
    CHECK(um_modem_config_uses_robust_gate(&config));
    recovered.symbol_repetitions = 3u;
    CHECK(um_modem_config_uses_robust_gate(&recovered));
    recovered.first_bin = 96u;
    recovered.last_bin = 234u;
    CHECK(um_modem_config_uses_robust_gate(&recovered));
    optimized.window_samples = 64u;
    CHECK(!um_modem_config_uses_robust_gate(&optimized));
    recovered = config;
    recovered.last_bin = 362u;
    CHECK(!um_modem_config_uses_robust_gate(&recovered));

    metrics.sync_correlation = 0.54f;
    metrics.estimated_snr_db = 15.0f;
    metrics.evm_rms = 0.265f;
    CHECK(um_modem_metrics_have_margin(&config, &metrics));

    metrics.estimated_snr_db = 12.0f;
    metrics.evm_rms = 0.43f;
    CHECK(!um_modem_metrics_have_margin(&optimized, &metrics));
    metrics.evm_rms = 0.41f;
    CHECK(um_modem_metrics_have_margin(&optimized, &metrics));

    config.qam_bits = 4u;
    CHECK(!um_modem_metrics_have_margin(&config, &metrics));

    config.qam_bits = 6u;
    metrics.estimated_snr_db = 24.0f;
    metrics.evm_rms = 0.060f;
    CHECK(um_modem_metrics_have_margin(&config, &metrics));

    metrics.sync_correlation = 0.20f;
    CHECK(!um_modem_metrics_have_baseline_margin(&metrics));
    CHECK(!um_modem_metrics_have_margin(&config, &metrics));
}

static void test_rejects_noise(void)
{
    um_modem_config config = um_modem_default_config();
    float noise[2500];
    uint8_t decoded[64];
    float *long_noise = NULL;
    size_t long_count = UM_SAMPLE_RATE * 2u;
    size_t decoded_count;
    uint16_t sequence;
    size_t i;
    int status;
    ++tests_run;
    for (i = 0u; i < sizeof(noise) / sizeof(noise[0]); ++i) {
        noise[i] = ((float)(test_random() & 0xffffu) / 32768.0f - 1.0f) *
                   0.2f;
    }
    status = um_demodulate_frame(&config, noise,
                                 sizeof(noise) / sizeof(noise[0]), decoded,
                                 sizeof(decoded), &decoded_count, &sequence,
                                 NULL);
    CHECK(status == UM_ERR_SYNC);
    config = um_modem_robust_config();
    long_noise = (float *)malloc(long_count * sizeof(*long_noise));
    CHECK(long_noise != NULL);
    for (i = 0u; i < long_count; ++i) {
        float time = (float)i / (float)UM_SAMPLE_RATE;
        long_noise[i] =
            0.012f * sinf(2.0f * UM_PI * 117.0f * time) +
            0.005f * sinf(2.0f * UM_PI * 347.0f * time) +
            ((float)(test_random() & 0xffffu) / 32768.0f - 1.0f) *
                0.003f;
    }
    status = um_demodulate_frame(&config, long_noise, long_count, decoded,
                                 sizeof(decoded), &decoded_count, &sequence,
                                 NULL);
    CHECK(status == UM_ERR_SYNC);
    free(long_noise);
}

static void test_async_session_distortion_ladder(void)
{
    um_distortion_ladder_result result;
    um_distortion_profile last_passing;
    ++tests_run;
    CHECK(um_run_session_distortion_ladder(0, &result, NULL, NULL) == UM_OK);
    CHECK(result.passes_passed >= 5u);
    CHECK(result.passes_attempted == result.passes_passed + 1u);
    CHECK(result.first_failed_level == result.passes_passed);
    CHECK(result.first_failure_status != UM_OK);
    CHECK(result.impairments_exercised == UM_IMPAIR_ALL);
    CHECK(result.session_retries >= 3u);
    CHECK(result.session_reconnects >= 1u);
    CHECK(result.simulated_seconds > 0.0f);
    CHECK(um_distortion_profile_get(result.passes_passed - 1u,
                                    &last_passing) == UM_OK);
    CHECK(last_passing.impairment_mask == UM_IMPAIR_ALL);
}

static void test_live_wire_protocol(void)
{
    uint8_t body[UM_LIVE_MAX_BODY];
    uint8_t capability[UM_LIVE_HANDSHAKE_BYTES];
    uint8_t wire[UM_LIVE_MAX_WIRE];
    uint8_t altered[UM_LIVE_MAX_WIRE];
    size_t wire_length = 0u;
    size_t i;
    um_live_wire_message message;
    ++tests_run;
    CHECK(UM_LIVE_HANDSHAKE_BYTES == 5u);
    for (i = 0u; i < sizeof(body); ++i) {
        body[i] = (uint8_t)test_random();
    }
    CHECK(um_live_wire_encode(UM_WIRE_DATA, UINT32_C(0x1234abcd),
                              UINT16_C(0xfedc), body, sizeof(body), wire,
                              sizeof(wire), &wire_length) == UM_OK);
    CHECK(wire_length == sizeof(wire));
    CHECK(um_live_wire_decode(wire, wire_length, &message) == UM_OK);
    CHECK(message.type == UM_WIRE_DATA);
    CHECK(message.session_id == UINT32_C(0x1234abcd));
    CHECK(message.sequence == UINT16_C(0xfedc));
    CHECK(message.body_length == sizeof(body));
    CHECK(memcmp(message.body, body, sizeof(body)) == 0);
    {
        um_modem_config config = um_modem_default_config();
        float *samples = NULL;
        size_t sample_count = 0u;
        uint8_t decoded_wire[UM_LIVE_MAX_WIRE];
        size_t decoded_length = 0u;
        uint16_t decoded_sequence = 0u;
        CHECK(um_modulate_frame(&config, wire, wire_length,
                                UINT16_C(0xfedc), &samples,
                                &sample_count) == UM_OK);
        CHECK(um_demodulate_frame(&config, samples, sample_count,
                                  decoded_wire, sizeof(decoded_wire),
                                  &decoded_length, &decoded_sequence,
                                  NULL) == UM_OK);
        CHECK(decoded_sequence == UINT16_C(0xfedc));
        CHECK(um_live_wire_decode(decoded_wire, decoded_length, &message) ==
              UM_OK);
        CHECK(message.body_length == sizeof(body));
        CHECK(memcmp(message.body, body, sizeof(body)) == 0);
        free(samples);
    }

    CHECK(um_live_wire_encode(UM_WIRE_DISCOVER, 1u, 2u, NULL, 0u, wire,
                              UM_LIVE_WIRE_HEADER_SIZE, &wire_length) ==
          UM_OK);
    CHECK(wire_length == UM_LIVE_WIRE_HEADER_SIZE);
    CHECK(um_live_wire_decode(wire, wire_length, &message) == UM_OK);
    CHECK(message.body_length == 0u);
    CHECK(um_live_wire_encode(UM_WIRE_DATA, 0u, 0u, body, sizeof(body), wire,
                              sizeof(wire) - 1u, &wire_length) ==
          UM_ERR_CAPACITY);
    CHECK(um_live_wire_encode((um_live_wire_type)0, 0u, 0u, NULL, 0u, wire,
                              sizeof(wire), &wire_length) == UM_ERR_ARGUMENT);
    CHECK(um_live_wire_encode(UM_WIRE_PROXY_COMPLETE, 3u, 4u, NULL, 0u,
                              wire, sizeof(wire), &wire_length) == UM_OK);
    CHECK(um_live_wire_decode(wire, wire_length, &message) == UM_OK);
    CHECK(message.type == UM_WIRE_PROXY_COMPLETE);
    CHECK(um_live_wire_encode(UM_WIRE_IP_WINDOW, 3u, 4u, body, 10u,
                              wire, sizeof(wire), &wire_length) == UM_OK);
    CHECK(um_live_wire_decode(wire, wire_length, &message) == UM_OK);
    CHECK(message.type == UM_WIRE_IP_WINDOW);
    CHECK(um_live_wire_encode((um_live_wire_type)26, 0u, 0u, NULL, 0u,
                              wire, sizeof(wire), &wire_length) ==
          UM_ERR_ARGUMENT);

    um_live_handshake_body(capability, 0);
    CHECK(um_live_handshake_validate(capability, sizeof(capability), 0) ==
          UM_OK);
    CHECK(um_live_wire_encode(UM_WIRE_DISCOVER, 1u, 2u, capability,
                              sizeof(capability), wire, sizeof(wire),
                              &wire_length) == UM_OK);
    CHECK(wire_length == UM_LIVE_WIRE_HEADER_SIZE + 5u);
    CHECK(um_live_wire_decode(wire, wire_length, &message) == UM_OK);
    CHECK(message.body_length == sizeof(capability));
    CHECK(um_live_handshake_validate(message.body, message.body_length, 0) ==
          UM_OK);
    CHECK(um_live_handshake_validate(capability, 0u, 0) ==
          UM_ERR_UNSUPPORTED);
    capability[0] ^= 1u;
    CHECK(um_live_handshake_validate(capability, sizeof(capability), 0) ==
          UM_ERR_UNSUPPORTED);
    um_live_handshake_body(capability, 0);
    capability[3] ^= 1u;
    CHECK(um_live_handshake_validate(capability, sizeof(capability), 0) ==
          UM_ERR_UNSUPPORTED);
    um_live_handshake_body(capability, 0);
    capability[4] ^= 2u;
    CHECK(um_live_handshake_validate(capability, sizeof(capability), 0) ==
          UM_ERR_UNSUPPORTED);
    um_live_handshake_body(capability, 1);
    CHECK(um_live_handshake_validate(capability, sizeof(capability), 0) ==
          UM_ERR_UNSUPPORTED);

    CHECK(um_live_wire_encode(UM_WIRE_ACK, 1u, 2u, NULL, 0u, wire,
                              sizeof(wire), &wire_length) == UM_OK);
    memcpy(altered, wire, wire_length);
    altered[0] ^= 1u;
    CHECK(um_live_wire_decode(altered, wire_length, &message) ==
          UM_ERR_HEADER);
    memcpy(altered, wire, wire_length);
    altered[11] = 1u;
    CHECK(um_live_wire_decode(altered, wire_length, &message) ==
          UM_ERR_HEADER);
}

static unsigned config_difference_count(const um_modem_config *left,
                                        const um_modem_config *right)
{
    unsigned differences = 0u;
#define COUNT_DIFFERENCE(field)                                                 \
    do {                                                                        \
        if (left->field != right->field) {                                      \
            ++differences;                                                      \
        }                                                                       \
    } while (0)
    COUNT_DIFFERENCE(fft_size);
    COUNT_DIFFERENCE(first_bin);
    COUNT_DIFFERENCE(last_bin);
    COUNT_DIFFERENCE(cyclic_prefix);
    COUNT_DIFFERENCE(window_samples);
    COUNT_DIFFERENCE(sync_samples);
    COUNT_DIFFERENCE(sync_gap);
    COUNT_DIFFERENCE(training_symbols);
    COUNT_DIFFERENCE(symbol_repetitions);
    COUNT_DIFFERENCE(qam_bits);
    COUNT_DIFFERENCE(fec_rate);
#undef COUNT_DIFFERENCE
    return differences;
}

static unsigned calibration_variant(const um_calibration_search_node *node)
{
    switch (node->step) {
    case UM_CALIB_STEP_REPETITIONS:
    case UM_CALIB_STEP_MORE_REPETITIONS:
        return node->config.symbol_repetitions;
    case UM_CALIB_STEP_QAM:
        return node->config.qam_bits;
    case UM_CALIB_STEP_FEC:
        return (unsigned)node->config.fec_rate;
    case UM_CALIB_STEP_PREFIX:
        return node->config.cyclic_prefix;
    case UM_CALIB_STEP_HIGH_BAND:
        return node->config.last_bin;
    case UM_CALIB_STEP_LOW_BAND:
    case UM_CALIB_STEP_NARROW_BAND:
        return node->config.first_bin;
    case UM_CALIB_STEP_SYNC:
        return node->config.sync_samples;
    case UM_CALIB_STEP_GAP:
        return node->config.sync_gap;
    case UM_CALIB_STEP_TRAINING:
        return node->config.training_symbols;
    case UM_CALIB_STEP_ULTRA_ROBUST_BAND:
        return (node->config.first_bin << 16u) | node->config.last_bin;
    default:
        return 0u;
    }
}

static void test_calibration_config_file(void)
{
    char path[160];
    um_modem_config saved = um_modem_default_config();
    um_modem_config loaded;
    size_t loaded_body_bytes = 0u;
    FILE *file;
    int found = 0;

    ++tests_run;
    (void)snprintf(path, sizeof(path),
                   "/tmp/universal-modem-calibration-%ld.config",
                   (long)getpid());
    (void)remove(path);
    CHECK(um_calibration_config_load(path, UM_LIVE_CLIENT, &loaded,
                                     &loaded_body_bytes, &found) == UM_OK);
    CHECK(found == 0);
    CHECK(um_calibration_config_save(path, UM_LIVE_CLIENT, &saved, 384u) ==
          UM_OK);
    CHECK(um_calibration_config_load(path, UM_LIVE_CLIENT, &loaded,
                                     &loaded_body_bytes, &found) == UM_OK);
    CHECK(found == 1);
    CHECK(loaded_body_bytes == 384u);
    CHECK(config_difference_count(&saved, &loaded) == 0u);
    CHECK(um_calibration_config_load(path, UM_LIVE_GATEWAY, &loaded,
                                     &loaded_body_bytes, &found) ==
          UM_ERR_CONFIG);
    file = fopen(path, "w");
    CHECK(file != NULL);
    CHECK(fputs("format=2\nrole=client\n", file) >= 0);
    CHECK(fclose(file) == 0);
    CHECK(um_calibration_config_load(path, UM_LIVE_CLIENT, &loaded,
                                     &loaded_body_bytes, &found) ==
          UM_ERR_CONFIG);
    CHECK(remove(path) == 0);
}

static void test_calibration_safety_guard(void)
{
    um_modem_config frontier = um_modem_default_config();
    um_modem_config guarded;
    ++tests_run;

    frontier.first_bin = 32u;
    frontier.last_bin = 896u;
    frontier.cyclic_prefix = 64u;
    frontier.window_samples = 32u;
    frontier.sync_samples = 512u;
    frontier.sync_gap = 512u;
    frontier.training_symbols = 2u;
    frontier.qam_bits = 6u;
    frontier.fec_rate = UM_FEC_RATE_3_4;
    CHECK(um_calibration_guard_config(&frontier, &guarded) == UM_OK);
    CHECK(guarded.qam_bits == 4u);
    CHECK(guarded.fec_rate == UM_FEC_RATE_2_3);
    CHECK(guarded.first_bin == 48u);
    CHECK(guarded.last_bin == 768u);
    CHECK(guarded.cyclic_prefix == 1024u);
    CHECK(guarded.window_samples == 64u);
    CHECK(guarded.sync_samples == 1536u);
    CHECK(guarded.sync_gap == 2048u);
    CHECK(guarded.training_symbols == 3u);
    CHECK(um_calibration_payload_rate(&guarded, UM_LIVE_MAX_BODY) <
          um_calibration_payload_rate(&frontier, UM_LIVE_MAX_BODY));

    frontier = um_modem_default_config();
    frontier.qam_bits = 4u;
    frontier.fec_rate = UM_FEC_RATE_3_4;
    CHECK(um_calibration_guard_config(&frontier, &guarded) == UM_OK);
    CHECK(guarded.qam_bits == 2u);
    CHECK(guarded.fec_rate == UM_FEC_RATE_2_3);

    frontier = um_modem_robust_config();
    CHECK(um_calibration_guard_config(&frontier, &guarded) == UM_OK);
    CHECK(guarded.qam_bits == frontier.qam_bits);
    CHECK(guarded.fec_rate == frontier.fec_rate);
    CHECK(guarded.symbol_repetitions == frontier.symbol_repetitions);
    CHECK(um_calibration_guard_config(NULL, &guarded) == UM_ERR_ARGUMENT);
    CHECK(um_calibration_guard_config(&frontier, NULL) == UM_ERR_ARGUMENT);
}

static void test_adaptive_calibration_search(void)
{
    const size_t guarded_samples = 960u + 2400u;
    const int quality_modes[] = {0, 1};
    size_t mode;
    ++tests_run;

    for (mode = 0u; mode < sizeof(quality_modes) / sizeof(quality_modes[0]);
         ++mode) {
        um_calibration_search search;
        unsigned qam_mask = 0u;
        unsigned fec_mask = 0u;
        unsigned step_mask = 0u;
        unsigned minimum_first_bin = UINT_MAX;
        unsigned maximum_last_bin = 0u;
        unsigned minimum_prefix = UINT_MAX;
        unsigned minimum_sync = UINT_MAX;
        unsigned minimum_gap = UINT_MAX;
        unsigned minimum_training = UINT_MAX;
        float maximum_rate = 0.0f;
        um_modem_config fastest = um_modem_robust_config();
        size_t maximum_samples = 0u;
        size_t tested = 0u;
        int next_status;
        CHECK(um_calibration_search_init(
                  &search, quality_modes[mode], UM_LIVE_MAX_BODY) == UM_OK);
        CHECK(search.budget ==
              (quality_modes[mode] == 0 ? 12u : 64u));
        while (1) {
            um_modem_config config;
            um_calibration_step step;
            size_t candidate_id;
            uint8_t probe_wire[UM_LIVE_WIRE_HEADER_SIZE +
                               UM_CALIBRATION_PROBE_BYTES];
            float *samples = NULL;
            size_t sample_count = 0u;
            next_status = um_calibration_search_next(
                &search, &candidate_id, &config, &step);
            CHECK(next_status >= 0);
            if (next_status <= 0) {
                break;
            }
            ++tested;
            CHECK(um_modem_config_validate(&config) == UM_OK);
            CHECK(config.window_samples != 0u);
            if (config.first_bin < minimum_first_bin) {
                minimum_first_bin = config.first_bin;
            }
            if (config.last_bin > maximum_last_bin) {
                maximum_last_bin = config.last_bin;
            }
            if (config.cyclic_prefix < minimum_prefix) {
                minimum_prefix = config.cyclic_prefix;
            }
            if (config.sync_samples < minimum_sync) {
                minimum_sync = config.sync_samples;
            }
            if (config.sync_gap < minimum_gap) {
                minimum_gap = config.sync_gap;
            }
            if (config.training_symbols < minimum_training) {
                minimum_training = config.training_symbols;
            }
            {
                float rate = um_calibration_payload_rate(
                    &config, search.rate_payload_bytes);
                if (rate > maximum_rate) {
                    maximum_rate = rate;
                    fastest = config;
                }
            }
            CHECK(candidate_id < search.node_count);
            if (candidate_id == 0u) {
                CHECK(step == UM_CALIB_STEP_BASELINE);
                CHECK(config.first_bin == 64u);
                CHECK(config.last_bin == 298u);
                CHECK(config.qam_bits == 2u);
                CHECK(config.fec_rate == UM_FEC_RATE_1_2);
                CHECK(config.cyclic_prefix == 1024u);
                CHECK(config.training_symbols == 4u);
                CHECK(config.symbol_repetitions == 2u);
            } else {
                size_t parent = search.nodes[candidate_id].parent;
                CHECK(parent < search.node_count);
                CHECK(search.nodes[parent].passed != 0);
                if (step == UM_CALIB_STEP_DATA_DEFAULT) {
                    um_modem_config balanced = um_modem_default_config();
                    CHECK(parent == 0u);
                    CHECK(config_difference_count(&config, &balanced) == 0u);
                } else if (step == UM_CALIB_STEP_WIDE_ANCHOR) {
                    CHECK(parent == 0u);
                    CHECK(config.first_bin == 48u);
                    CHECK(config.last_bin == 768u);
                    CHECK(config.qam_bits == 4u);
                    CHECK(config.fec_rate == UM_FEC_RATE_2_3);
                    CHECK(config.cyclic_prefix == 256u);
                    CHECK(config.training_symbols == 2u);
                } else if (step == UM_CALIB_STEP_PRISTINE_ANCHOR) {
                    CHECK(parent == 0u);
                    CHECK(config.first_bin == 32u);
                    CHECK(config.last_bin == 896u);
                    CHECK(config.qam_bits == 6u);
                    CHECK(config.fec_rate == UM_FEC_RATE_3_4);
                    CHECK(config.cyclic_prefix == 64u);
                    CHECK(config.sync_samples == 512u);
                    CHECK(config.sync_gap == 512u);
                    CHECK(config.training_symbols == 2u);
                } else {
                    unsigned differences = config_difference_count(
                        &config, &search.nodes[parent].config);
                    CHECK(differences == 1u ||
                          (step == UM_CALIB_STEP_PREFIX &&
                           differences == 2u));
                    if (step != UM_CALIB_STEP_NARROW_BAND &&
                        step != UM_CALIB_STEP_MORE_REPETITIONS) {
                        CHECK(um_calibration_payload_rate(
                                  &config, search.rate_payload_bytes) >
                              um_calibration_payload_rate(
                                  &search.nodes[parent].config,
                                  search.rate_payload_bytes) +
                                  0.5f);
                    }
                }
                CHECK(step != UM_CALIB_STEP_BASELINE);
            }
            qam_mask |= 1u << (config.qam_bits / 2u);
            fec_mask |= 1u << (unsigned)config.fec_rate;
            step_mask |= 1u << (unsigned)step;
            memset(probe_wire, (int)(candidate_id & 0xffu),
                   sizeof(probe_wire));
            CHECK(um_modulate_frame(&config, probe_wire, sizeof(probe_wire),
                                    (uint16_t)candidate_id, &samples,
                                    &sample_count) == UM_OK);
            CHECK(sample_count + guarded_samples < 2u * UM_SAMPLE_RATE);
            if (sample_count > maximum_samples) {
                maximum_samples = sample_count;
            }
            free(samples);
            CHECK(um_calibration_search_record(&search, candidate_id, 1) ==
                  UM_OK);
        }
        printf("adaptive calibration %s budget=%zu generated=%zu "
               "max-frame=%.1f ms best@%zuB=%.0fbps "
               "best=%u-qam/%u-%uHz/cp%u steps=0x%x qam=0x%x fec=0x%x\n",
               quality_modes[mode] == 0 ? "default" : "high", tested,
               search.node_count,
               1000.0 * (double)maximum_samples / (double)UM_SAMPLE_RATE,
               search.rate_payload_bytes, maximum_rate,
               1u << fastest.qam_bits,
               fastest.first_bin * UM_SAMPLE_RATE / fastest.fft_size,
               fastest.last_bin * UM_SAMPLE_RATE / fastest.fft_size,
               fastest.cyclic_prefix,
               step_mask, qam_mask, fec_mask);
        CHECK(tested <= search.budget);
        CHECK(tested >= 12u);
        CHECK((qam_mask & 6u) == 6u);
        CHECK((fec_mask & 3u) == 3u);
        if (quality_modes[mode] != 0) {
            CHECK(qam_mask == 14u);
            CHECK(fec_mask == 7u);
            CHECK(minimum_first_bin == 32u);
            CHECK(maximum_last_bin == 896u);
            CHECK(minimum_prefix <= 64u);
            CHECK(minimum_sync == 512u);
            CHECK(minimum_gap <= 512u);
            CHECK(minimum_training == 2u);
            CHECK(maximum_rate > 15000.0f);
            CHECK(fastest.qam_bits == 6u);
            CHECK(fastest.fec_rate == UM_FEC_RATE_3_4);
            CHECK(fastest.first_bin == 32u);
            CHECK(fastest.last_bin == 896u);
            CHECK(fastest.cyclic_prefix <= 64u);
        }
        CHECK((step_mask & (1u << UM_CALIB_STEP_QAM)) != 0u);
        CHECK((step_mask & (1u << UM_CALIB_STEP_FEC)) != 0u);
        CHECK((step_mask & (1u << UM_CALIB_STEP_PREFIX)) != 0u);
        CHECK((step_mask & (1u << UM_CALIB_STEP_HIGH_BAND)) != 0u);
        CHECK((step_mask & (1u << UM_CALIB_STEP_DATA_DEFAULT)) != 0u);
        if (quality_modes[mode] != 0) {
            CHECK((step_mask & (1u << UM_CALIB_STEP_SYNC)) != 0u);
            CHECK((step_mask & (1u << UM_CALIB_STEP_GAP)) != 0u);
            CHECK((step_mask & (1u << UM_CALIB_STEP_WIDE_ANCHOR)) != 0u);
            CHECK((step_mask & (1u << UM_CALIB_STEP_PRISTINE_ANCHOR)) !=
                  0u);
        }
        {
            size_t index;
            for (index = 0u; index < search.node_count; ++index) {
                size_t compare;
                unsigned attempts = 0u;
                int any_pass = 0;
                const um_calibration_search_node *node =
                    &search.nodes[index];
                if (node->recorded == 0 ||
                    node->step == UM_CALIB_STEP_BASELINE ||
                    node->step == UM_CALIB_STEP_DATA_DEFAULT ||
                    node->step == UM_CALIB_STEP_MORE_REPETITIONS) {
                    continue;
                }
                for (compare = 0u; compare < search.node_count; ++compare) {
                    const um_calibration_search_node *other =
                        &search.nodes[compare];
                    if (other->recorded != 0 && other->step == node->step &&
                        calibration_variant(other) ==
                            calibration_variant(node)) {
                        ++attempts;
                        any_pass |= other->passed;
                    }
                }
                if (any_pass != 0) {
                    CHECK(attempts <= 2u);
                }
            }
        }
    }

    {
        um_calibration_search search;
        size_t tested = 0u;
        int next_status;
        CHECK(um_calibration_search_init(&search, 0, UM_LIVE_MAX_BODY) ==
              UM_OK);
        while (1) {
            um_modem_config config;
            um_calibration_step step;
            size_t candidate_id;
            next_status = um_calibration_search_next(
                &search, &candidate_id, &config, &step);
            CHECK(next_status >= 0);
            if (next_status <= 0) {
                break;
            }
            ++tested;
            if (candidate_id == 0u) {
                CHECK(um_calibration_search_record(&search, candidate_id, 1) ==
                      UM_OK);
            } else {
                CHECK(search.nodes[candidate_id].parent == 0u);
                CHECK(um_calibration_search_record(&search, candidate_id, 0) ==
                      UM_OK);
            }
        }
        CHECK(tested < search.budget);
        /* Every rate-useful direct alternative was tried exactly once. */
        CHECK(tested == search.node_count);
        CHECK(tested >= 8u);
        CHECK(search.passed_count == 1u);
    }

    {
        um_calibration_search search;
        unsigned qam_attempts = 0u;
        unsigned qam_passes = 0u;
        unsigned observed_qam_attempts = 0u;
        unsigned widest_band = 0u;
        unsigned shortest_prefix = UINT_MAX;
        size_t tested = 0u;
        int next_status;
        CHECK(um_calibration_search_init(&search, 1, UM_LIVE_MAX_BODY) ==
              UM_OK);
        while (1) {
            um_modem_config config;
            um_calibration_step step;
            size_t candidate_id;
            int passed;
            next_status = um_calibration_search_next(
                &search, &candidate_id, &config, &step);
            CHECK(next_status >= 0);
            if (next_status <= 0) {
                break;
            }
            ++tested;
            /* Model a channel on which every mode above QPSK fails. */
            passed = config.qam_bits == 2u;
            if (step == UM_CALIB_STEP_QAM) {
                ++observed_qam_attempts;
            }
            if (passed != 0 && config.last_bin > widest_band) {
                widest_band = config.last_bin;
            }
            if (passed != 0 && config.cyclic_prefix < shortest_prefix) {
                shortest_prefix = config.cyclic_prefix;
            }
            CHECK(um_calibration_search_record(&search, candidate_id,
                                               passed) == UM_OK);
        }
        um_calibration_search_step_results(
            &search, UM_CALIB_STEP_QAM, &qam_attempts, &qam_passes);
        printf("adaptive learned failure: qam=%u/%u widest-bin=%u "
               "shortest-prefix=%u probes=%zu\n",
               qam_passes, qam_attempts, widest_band, shortest_prefix,
               tested);
        CHECK(tested <= search.budget);
        CHECK(tested >= 24u);
        CHECK(qam_attempts == observed_qam_attempts);
        CHECK(qam_attempts >= 1u);
        CHECK(qam_attempts <= 2u);
        CHECK(qam_passes == 0u);
        CHECK(widest_band >= 512u);
        CHECK(shortest_prefix <= 512u);
    }

    {
        um_calibration_search search;
        float scores[UM_CALIBRATION_SEARCH_MAX_NODES];
        size_t ranked[5u];
        size_t rank_count;
        size_t index;
        memset(&search, 0, sizeof(search));
        for (index = 0u; index < UM_CALIBRATION_SEARCH_MAX_NODES; ++index) {
            scores[index] = -1.0f;
        }
        search.node_count = 6u;
        search.nodes[1].parent = 0u;
        search.nodes[2].parent = 1u;
        search.nodes[3].parent = 2u;
        search.nodes[4].parent = 3u;
        search.nodes[5].parent = 0u;
        scores[0] = 1.0f;
        scores[1] = 2.0f;
        scores[2] = 3.0f;
        scores[3] = 4.0f;
        scores[4] = 5.0f;
        scores[5] = 4.5f;
        rank_count = um_calibration_rank_candidates(
            &search, scores, ranked, sizeof(ranked) / sizeof(ranked[0]));
        CHECK(rank_count == 5u);
        CHECK(ranked[0] == 4u);
        CHECK(ranked[1] == 3u);
        CHECK(ranked[2] == 2u);
        CHECK(ranked[3] == 1u);
        CHECK(ranked[4] == 0u);
    }

    {
        um_calibration_search search;
        um_modem_config config;
        um_calibration_step step;
        size_t candidate_id;
        unsigned recovery_band_mask = 0u;
        unsigned recovery_bands = 0u;
        CHECK(um_calibration_search_init(&search, 1, UM_LIVE_MAX_BODY) ==
              UM_OK);
        CHECK(um_calibration_search_next(&search, &candidate_id, &config,
                                         &step) == 1);
        CHECK(candidate_id == 0u);
        CHECK(step == UM_CALIB_STEP_BASELINE);
        CHECK(config_difference_count(&config,
                                      &search.nodes[0].config) == 0u);
        CHECK(um_calibration_search_record(&search, candidate_id, 0) ==
              UM_OK);
        CHECK(um_calibration_search_next(&search, &candidate_id, &config,
                                         &step) == 1);
        CHECK(step == UM_CALIB_STEP_MORE_REPETITIONS);
        CHECK(config.symbol_repetitions == 3u);
        CHECK(search.nodes[candidate_id].parent == 0u);
        CHECK(um_calibration_search_record(&search, candidate_id, 0) ==
              UM_OK);
        CHECK(um_calibration_search_next(&search, &candidate_id, &config,
                                         &step) == 1);
        CHECK(step == UM_CALIB_STEP_MORE_REPETITIONS);
        CHECK(config.symbol_repetitions == 4u);
        CHECK(um_calibration_search_record(&search, candidate_id, 0) ==
              UM_OK);
        while (um_calibration_search_next(&search, &candidate_id, &config,
                                          &step) == 1) {
            uint8_t probe_wire[UM_LIVE_WIRE_HEADER_SIZE +
                               UM_CALIBRATION_PROBE_BYTES] = {0u};
            float *samples = NULL;
            size_t sample_count = 0u;
            CHECK(step == UM_CALIB_STEP_ULTRA_ROBUST_BAND);
            CHECK(config.symbol_repetitions == 4u);
            CHECK(config.qam_bits == 2u);
            CHECK(config.fec_rate == UM_FEC_RATE_1_2);
            CHECK(um_modem_config_uses_robust_gate(&config));
            CHECK(um_calibration_payload_rate(&config, UM_LIVE_MAX_BODY) <
                  um_calibration_payload_rate(&search.nodes[0].config,
                                              UM_LIVE_MAX_BODY));
            CHECK(um_modulate_frame(&config, probe_wire, sizeof(probe_wire),
                                    (uint16_t)candidate_id, &samples,
                                    &sample_count) == UM_OK);
            CHECK(sample_count + guarded_samples < 4u * UM_SAMPLE_RATE);
            free(samples);
            if (config.first_bin == 64u && config.last_bin == 234u) {
                recovery_band_mask |= 1u;
            } else if (config.first_bin == 64u &&
                       config.last_bin == 192u) {
                recovery_band_mask |= 2u;
            } else if (config.first_bin == 96u &&
                       config.last_bin == 234u) {
                recovery_band_mask |= 4u;
            } else if (config.first_bin == 128u &&
                       config.last_bin == 298u) {
                recovery_band_mask |= 8u;
            } else {
                CHECK(0);
            }
            ++recovery_bands;
            CHECK(um_calibration_search_record(&search, candidate_id, 0) ==
                  UM_OK);
        }
        CHECK(recovery_bands == 4u);
        CHECK(recovery_band_mask == 15u);
        CHECK(search.tested_count == 7u);
        CHECK(search.passed_count == 0u);
    }

    {
        um_calibration_search search;
        um_modem_config config;
        um_calibration_step step;
        size_t candidate_id;
        size_t recovery_id;
        CHECK(um_calibration_search_init(&search, 1, UM_LIVE_MAX_BODY) ==
              UM_OK);
        CHECK(um_calibration_search_next(&search, &candidate_id, &config,
                                         &step) == 1);
        CHECK(um_calibration_search_record(&search, candidate_id, 0) ==
              UM_OK);
        CHECK(um_calibration_search_next(&search, &recovery_id, &config,
                                         &step) == 1);
        CHECK(step == UM_CALIB_STEP_MORE_REPETITIONS);
        CHECK(config.symbol_repetitions == 3u);
        CHECK(um_calibration_search_record(&search, recovery_id, 1) ==
              UM_OK);
        CHECK(um_calibration_search_next(&search, &candidate_id, &config,
                                         &step) == 1);
        CHECK(search.nodes[candidate_id].parent == recovery_id);
        CHECK(search.nodes[recovery_id].passed != 0);
        CHECK(step != UM_CALIB_STEP_MORE_REPETITIONS);
    }

    {
        um_calibration_search search;
        um_modem_config config;
        um_calibration_step step;
        size_t candidate_id;
        size_t robust_band_id = SIZE_MAX;
        int saw_robust_child = 0;
        CHECK(um_calibration_search_init(&search, 1, UM_LIVE_MAX_BODY) ==
              UM_OK);
        while (um_calibration_search_next(&search, &candidate_id, &config,
                                          &step) == 1) {
            int passed = 0;
            if (step == UM_CALIB_STEP_ULTRA_ROBUST_BAND &&
                config.first_bin == 64u && config.last_bin == 192u) {
                passed = 1;
                robust_band_id = candidate_id;
            } else if (robust_band_id != SIZE_MAX &&
                       search.nodes[candidate_id].parent == robust_band_id) {
                CHECK(search.nodes[robust_band_id].passed != 0);
                CHECK(um_calibration_payload_rate(
                          &config, search.rate_payload_bytes) >
                      um_calibration_payload_rate(
                          &search.nodes[robust_band_id].config,
                          search.rate_payload_bytes));
                saw_robust_child = 1;
                break;
            }
            CHECK(um_calibration_search_record(&search, candidate_id,
                                               passed) == UM_OK);
        }
        CHECK(robust_band_id != SIZE_MAX);
        CHECK(saw_robust_child != 0);
    }
}

int main(void)
{
    test_crc();
    test_quiet_traffic_policy();
    test_fft_round_trip();
    test_qam_constellations();
    test_fec_rates();
    test_interleaved_burst();
    test_clean_frames();
    test_impaired_frames();
    test_input_normalization();
    test_robust_low_level_colored_noise();
    test_recorded_v2_async_session();
    test_room_reverb_and_clock_drift();
    test_default_calibration();
    test_distortion_profiles();
    test_calibration_distortion_ladder();
    test_boundary_window();
    test_control_symbol_power();
    test_reliability_margin();
    test_rejects_noise();
    test_async_session_distortion_ladder();
    test_live_wire_protocol();
    test_calibration_config_file();
    test_calibration_safety_guard();
    test_adaptive_calibration_search();

    if (failures != 0) {
        fprintf(stderr, "%d of %d tests failed\n", failures, tests_run);
        return 1;
    }
    printf("all %d modem tests passed\n", tests_run);
    return 0;
}
