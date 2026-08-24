#include "um.h"
#include "../src/um_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void test_crc(void)
{
    static const uint8_t check[] = "123456789";
    ++tests_run;
    CHECK(um_crc32(check, sizeof(check) - 1u) == UINT32_C(0xcbf43926));
    CHECK(um_crc16(check, sizeof(check) - 1u) == UINT16_C(0x29b1));
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
    channel.clip_level = 0.42f;
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
                            UM_SYNC_SAMPLES + UM_SYNC_GAP + 24u * 288u;
    channel.dropout_length = 96u;
    channel.random_seed = UINT32_C(0x31415926);
    CHECK(frame_round_trip(&config, &channel, 700u, &metrics) == UM_OK);
}

static void test_default_calibration(void)
{
    um_channel_config channel = um_channel_default_config();
    um_calibration_result result;
    ++tests_run;
    channel.leading_silence = 121u;
    channel.gain = 0.26f;
    channel.noise_stddev = 0.0015f;
    channel.echo_delay = 13u;
    channel.echo_gain = 0.29f;
    channel.random_seed = UINT32_C(0x5eed1234);
    CHECK(um_calibrate_simulated(&channel, 0, &result, NULL, NULL) == UM_OK);
    CHECK(result.candidates_tested == 81u);
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
    CHECK(result.candidates_tested > result.passes_attempted * 81u);
    CHECK(result.verification_frames >= result.passes_passed * 6u);
    CHECK(um_distortion_profile_get(result.passes_passed - 1u,
                                    &last_passing) == UM_OK);
    CHECK((last_passing.impairment_mask & UM_IMPAIR_ACOUSTIC_ALL) ==
          UM_IMPAIR_ACOUSTIC_ALL);
}

static double out_of_band_ratio(const float *samples, size_t sample_count,
                                const um_modem_config *config)
{
    size_t start = UM_SYNC_LEAD + UM_SYNC_SAMPLES + UM_SYNC_GAP;
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
    size_t i;
    ++tests_run;
    for (i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)test_random();
    }
    rectangular.window_samples = 0u;
    windowed.window_samples = 8u;
    CHECK(um_modulate_frame(&rectangular, payload, sizeof(payload), 1u,
                            &plain_samples, &plain_count) == UM_OK);
    CHECK(um_modulate_frame(&windowed, payload, sizeof(payload), 1u,
                            &windowed_samples, &windowed_count) == UM_OK);
    plain_leakage = out_of_band_ratio(plain_samples, plain_count, &rectangular);
    windowed_leakage =
        out_of_band_ratio(windowed_samples, windowed_count, &windowed);
    printf("spectral leakage: rectangular %.2f dB, windowed %.2f dB\n",
           10.0 * log10(plain_leakage), 10.0 * log10(windowed_leakage));
    CHECK(windowed_leakage < plain_leakage);
    free(windowed_samples);
    free(plain_samples);
}

static void test_rejects_noise(void)
{
    um_modem_config config = um_modem_default_config();
    float noise[2500];
    uint8_t decoded[64];
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

int main(void)
{
    test_crc();
    test_fft_round_trip();
    test_qam_constellations();
    test_fec_rates();
    test_interleaved_burst();
    test_clean_frames();
    test_impaired_frames();
    test_default_calibration();
    test_distortion_profiles();
    test_calibration_distortion_ladder();
    test_boundary_window();
    test_rejects_noise();
    test_async_session_distortion_ladder();

    if (failures != 0) {
        fprintf(stderr, "%d of %d tests failed\n", failures, tests_run);
        return 1;
    }
    printf("all %d modem tests passed\n", tests_run);
    return 0;
}
