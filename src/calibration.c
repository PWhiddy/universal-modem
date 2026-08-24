#include "um_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned first_bin;
    unsigned last_bin;
} frequency_range;

typedef struct {
    um_modem_config config;
    float score;
    float payload_bps;
    float evm_rms;
} viable_candidate;

static const frequency_range default_ranges[] = {
    {12u, 60u}, {16u, 72u}, {20u, 88u}
};

static const frequency_range high_ranges[] = {
    {8u, 48u},  {8u, 64u},   {10u, 76u}, {12u, 60u},
    {12u, 84u}, {16u, 72u},  {16u, 96u}, {20u, 88u},
    {20u, 104u}, {24u, 112u}
};

static const unsigned qam_options[] = {2u, 4u, 6u};
static const unsigned default_prefixes[] = {16u, 32u, 64u};
static const unsigned high_prefixes[] = {8u, 16u, 24u, 32u, 48u, 64u, 96u};
static const unsigned default_windows[] = {8u};
static const unsigned high_windows[] = {0u, 4u, 8u, 16u};
static const um_fec_rate fec_options[] = {UM_FEC_RATE_1_2, UM_FEC_RATE_2_3,
                                          UM_FEC_RATE_3_4};

/*
 * Use a balanced one-third fractional grid for live sweeps. Every option in
 * every dimension is retained while the deterministic acoustic schedule stays
 * near 15 seconds by default and several minutes in high-quality mode.
 */
static size_t live_candidate_count_for_grid(const frequency_range *ranges,
                                            size_t range_count,
                                            const unsigned *prefixes,
                                            size_t prefix_count,
                                            const unsigned *windows,
                                            size_t window_count)
{
    size_t count = 0u;
    size_t range_index;
    size_t qam_index;
    size_t prefix_index;
    size_t fec_index;
    size_t window_index;
    for (range_index = 0u; range_index < range_count; ++range_index) {
        for (qam_index = 0u;
             qam_index < sizeof(qam_options) / sizeof(qam_options[0]);
             ++qam_index) {
            for (prefix_index = 0u; prefix_index < prefix_count;
                 ++prefix_index) {
                for (fec_index = 0u;
                     fec_index < sizeof(fec_options) / sizeof(fec_options[0]);
                     ++fec_index) {
                    for (window_index = 0u; window_index < window_count;
                         ++window_index) {
                        um_modem_config candidate = um_modem_default_config();
                        size_t selector = range_index + qam_index +
                                          prefix_index + fec_index +
                                          window_index;
                        candidate.first_bin = ranges[range_index].first_bin;
                        candidate.last_bin = ranges[range_index].last_bin;
                        candidate.qam_bits = qam_options[qam_index];
                        candidate.cyclic_prefix = prefixes[prefix_index];
                        candidate.fec_rate = fec_options[fec_index];
                        candidate.window_samples = windows[window_index];
                        if (selector % 3u == 0u &&
                            um_modem_config_validate(&candidate) == UM_OK) {
                            ++count;
                        }
                    }
                }
            }
        }
    }
    return count;
}

size_t um_live_calibration_candidate_count(int high_quality)
{
    if (high_quality != 0) {
        return live_candidate_count_for_grid(
            high_ranges, sizeof(high_ranges) / sizeof(high_ranges[0]),
            high_prefixes,
            sizeof(high_prefixes) / sizeof(high_prefixes[0]), high_windows,
            sizeof(high_windows) / sizeof(high_windows[0]));
    }
    return live_candidate_count_for_grid(
        default_ranges, sizeof(default_ranges) / sizeof(default_ranges[0]),
        default_prefixes,
        sizeof(default_prefixes) / sizeof(default_prefixes[0]),
        default_windows,
        sizeof(default_windows) / sizeof(default_windows[0]));
}

int um_live_calibration_candidate_get(int high_quality, size_t index,
                                      um_modem_config *config)
{
    const frequency_range *ranges = high_quality != 0 ? high_ranges
                                                       : default_ranges;
    size_t range_count = high_quality != 0
                             ? sizeof(high_ranges) / sizeof(high_ranges[0])
                             : sizeof(default_ranges) /
                                   sizeof(default_ranges[0]);
    const unsigned *prefixes = high_quality != 0 ? high_prefixes
                                                  : default_prefixes;
    size_t prefix_count = high_quality != 0
                              ? sizeof(high_prefixes) /
                                    sizeof(high_prefixes[0])
                              : sizeof(default_prefixes) /
                                    sizeof(default_prefixes[0]);
    const unsigned *windows = high_quality != 0 ? high_windows
                                                 : default_windows;
    size_t window_count = high_quality != 0
                              ? sizeof(high_windows) / sizeof(high_windows[0])
                              : sizeof(default_windows) /
                                    sizeof(default_windows[0]);
    size_t current = 0u;
    size_t range_index;
    size_t qam_index;
    size_t prefix_index;
    size_t fec_index;
    size_t window_index;
    if (config == NULL) {
        return UM_ERR_ARGUMENT;
    }
    for (range_index = 0u; range_index < range_count; ++range_index) {
        for (qam_index = 0u;
             qam_index < sizeof(qam_options) / sizeof(qam_options[0]);
             ++qam_index) {
            for (prefix_index = 0u; prefix_index < prefix_count;
                 ++prefix_index) {
                for (fec_index = 0u;
                     fec_index < sizeof(fec_options) / sizeof(fec_options[0]);
                     ++fec_index) {
                    for (window_index = 0u; window_index < window_count;
                         ++window_index) {
                        um_modem_config candidate = um_modem_default_config();
                        size_t selector = range_index + qam_index +
                                          prefix_index + fec_index +
                                          window_index;
                        candidate.first_bin = ranges[range_index].first_bin;
                        candidate.last_bin = ranges[range_index].last_bin;
                        candidate.qam_bits = qam_options[qam_index];
                        candidate.cyclic_prefix = prefixes[prefix_index];
                        candidate.fec_rate = fec_options[fec_index];
                        candidate.window_samples = windows[window_index];
                        if (selector % 3u != 0u ||
                            um_modem_config_validate(&candidate) != UM_OK) {
                            continue;
                        }
                        if (current++ == index) {
                            *config = candidate;
                            return UM_OK;
                        }
                    }
                }
            }
        }
    }
    return UM_ERR_ARGUMENT;
}

static float fec_ratio(um_fec_rate rate)
{
    switch (rate) {
    case UM_FEC_RATE_1_2:
        return 0.5f;
    case UM_FEC_RATE_2_3:
        return 2.0f / 3.0f;
    case UM_FEC_RATE_3_4:
        return 0.75f;
    default:
        return 0.0f;
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

static void fill_probe(uint8_t *probe, size_t count, uint32_t seed)
{
    uint32_t state = seed != 0u ? seed : UINT32_C(0xc001d00d);
    size_t i;
    for (i = 0u; i < count; ++i) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        probe[i] = (uint8_t)state;
    }
}

static int compare_candidates(const void *left, const void *right)
{
    const viable_candidate *first = (const viable_candidate *)left;
    const viable_candidate *second = (const viable_candidate *)right;
    if (first->score < second->score) {
        return 1;
    }
    if (first->score > second->score) {
        return -1;
    }
    return 0;
}

static int try_candidate(const um_modem_config *config,
                         const um_channel_config *channel, uint32_t seed,
                         const uint8_t *probe, size_t probe_count,
                         float *duration, float *payload_bps,
                         um_rx_metrics *metrics)
{
    float *transmitted = NULL;
    float *received = NULL;
    uint8_t *decoded = NULL;
    size_t transmitted_count = 0u;
    size_t received_count = 0u;
    size_t decoded_count = 0u;
    uint16_t sequence = 0u;
    um_channel_config varied_channel = *channel;
    int status;

    varied_channel.random_seed ^= seed * UINT32_C(0x9e3779b9);
    decoded = (uint8_t *)malloc(probe_count);
    if (decoded == NULL) {
        return UM_ERR_MEMORY;
    }
    status = um_modulate_frame(config, probe, probe_count, (uint16_t)seed,
                               &transmitted, &transmitted_count);
    if (status != UM_OK) {
        goto done;
    }
    *duration = (float)transmitted_count / (float)UM_SAMPLE_RATE + 0.020f;
    *payload_bps = (float)(probe_count * 8u) / *duration;
    status = um_channel_apply(transmitted, transmitted_count, &varied_channel,
                              &received, &received_count);
    if (status != UM_OK) {
        goto done;
    }
    status = um_demodulate_frame(config, received, received_count, decoded,
                                 probe_count, &decoded_count, &sequence,
                                 metrics);
    if (status == UM_OK &&
        (decoded_count != probe_count || sequence != (uint16_t)seed ||
         memcmp(decoded, probe, probe_count) != 0)) {
        status = UM_ERR_CRC;
    }

done:
    free(decoded);
    free(received);
    free(transmitted);
    return status;
}

int um_calibrate_simulated(const um_channel_config *channel, int high_quality,
                           um_calibration_result *result,
                           um_log_callback logger, void *logger_context)
{
    const frequency_range *ranges = high_quality != 0 ? high_ranges
                                                       : default_ranges;
    size_t range_count = high_quality != 0
                             ? sizeof(high_ranges) / sizeof(high_ranges[0])
                             : sizeof(default_ranges) /
                                   sizeof(default_ranges[0]);
    const unsigned *prefixes = high_quality != 0 ? high_prefixes
                                                  : default_prefixes;
    size_t prefix_count = high_quality != 0
                              ? sizeof(high_prefixes) /
                                    sizeof(high_prefixes[0])
                              : sizeof(default_prefixes) /
                                    sizeof(default_prefixes[0]);
    const unsigned *windows = high_quality != 0 ? high_windows
                                                 : default_windows;
    size_t window_count = high_quality != 0
                              ? sizeof(high_windows) / sizeof(high_windows[0])
                              : sizeof(default_windows) /
                                    sizeof(default_windows[0]);
    uint8_t probe[32];
    uint8_t verification_probe[128];
    size_t maximum_candidates = range_count *
                                (sizeof(qam_options) /
                                 sizeof(qam_options[0])) *
                                prefix_count *
                                (sizeof(fec_options) /
                                 sizeof(fec_options[0])) *
                                window_count;
    viable_candidate *viable = NULL;
    size_t viable_count = 0u;
    size_t range_index;
    size_t qam_index;
    size_t prefix_index;
    size_t fec_index;
    size_t window_index;
    int fatal_status = UM_OK;

    if (channel == NULL || result == NULL) {
        return UM_ERR_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->config = um_modem_default_config();
    fill_probe(probe, sizeof(probe), UINT32_C(0xc001d00d));
    viable = (viable_candidate *)malloc(maximum_candidates * sizeof(*viable));
    if (viable == NULL) {
        return UM_ERR_MEMORY;
    }

    for (range_index = 0u; range_index < range_count; ++range_index) {
        for (qam_index = 0u;
             qam_index < sizeof(qam_options) / sizeof(qam_options[0]);
             ++qam_index) {
            for (prefix_index = 0u; prefix_index < prefix_count;
                 ++prefix_index) {
                for (fec_index = 0u;
                     fec_index < sizeof(fec_options) / sizeof(fec_options[0]);
                     ++fec_index) {
                    for (window_index = 0u; window_index < window_count;
                         ++window_index) {
                        um_modem_config candidate = um_modem_default_config();
                        um_rx_metrics metrics;
                        float duration = 0.0f;
                        float payload_bps = 0.0f;
                        float raw_bps;
                        float quality;
                        float score;
                        int status;
                        char line[320];

                        candidate.first_bin = ranges[range_index].first_bin;
                        candidate.last_bin = ranges[range_index].last_bin;
                        candidate.qam_bits = qam_options[qam_index];
                        candidate.cyclic_prefix = prefixes[prefix_index];
                        candidate.fec_rate = fec_options[fec_index];
                        candidate.window_samples = windows[window_index];
                        if (um_modem_config_validate(&candidate) != UM_OK) {
                            continue;
                        }
                        ++result->candidates_tested;
                        memset(&metrics, 0, sizeof(metrics));
                        status = try_candidate(
                            &candidate, channel,
                            (uint32_t)result->candidates_tested, probe,
                            sizeof(probe), &duration, &payload_bps, &metrics);
                        if (status == UM_ERR_MEMORY) {
                            fatal_status = status;
                            goto done;
                        }
                        result->estimated_seconds += duration;
                        raw_bps =
                            (float)um_modem_data_carriers(&candidate) *
                            (float)candidate.qam_bits *
                            fec_ratio(candidate.fec_rate) *
                            (float)UM_SAMPLE_RATE /
                            (float)(candidate.fft_size +
                                    candidate.cyclic_prefix);
                        quality = status == UM_OK
                                      ? 1.0f /
                                            (1.0f + 4.0f * metrics.evm_rms *
                                                        metrics.evm_rms)
                                      : 0.0f;
                        if (candidate.window_samples == 0u) {
                            quality *= 0.94f;
                        } else if (candidate.window_samples < 8u) {
                            quality *= 0.98f;
                        } else if (candidate.window_samples > 8u) {
                            quality *= 0.995f;
                        }
                        score = (raw_bps < payload_bps ? raw_bps : payload_bps) *
                                quality;
                        if (status == UM_OK) {
                            ++result->candidates_viable;
                            viable[viable_count].config = candidate;
                            viable[viable_count].score = score;
                            viable[viable_count].payload_bps = payload_bps;
                            viable[viable_count].evm_rms = metrics.evm_rms;
                            ++viable_count;
                        }
                        if (logger != NULL) {
                            (void)snprintf(
                                line, sizeof(line),
                                "calib %zu band=%.0f-%.0fHz qam=%u cp=%.2fms "
                                "fec=%s win=%u: %s snr=%.1fdB evm=%.3f "
                                "payload=%.0fbps score=%.0f",
                                result->candidates_tested,
                                (double)candidate.first_bin *
                                    (double)UM_SAMPLE_RATE /
                                    (double)candidate.fft_size,
                                (double)candidate.last_bin *
                                    (double)UM_SAMPLE_RATE /
                                    (double)candidate.fft_size,
                                1u << candidate.qam_bits,
                                1000.0 * (double)candidate.cyclic_prefix /
                                    (double)UM_SAMPLE_RATE,
                                fec_name(candidate.fec_rate),
                                candidate.window_samples,
                                status == UM_OK ? "pass" :
                                                  um_status_string(status),
                                metrics.estimated_snr_db, metrics.evm_rms,
                                payload_bps, score);
                            logger(logger_context, line);
                        }
                    }
                }
            }
        }
    }

done:
    if (fatal_status != UM_OK) {
        free(viable);
        return fatal_status;
    }
    if (viable_count != 0u) {
        size_t rank;
        unsigned verification_trials = high_quality != 0 ? 5u : 3u;
        qsort(viable, viable_count, sizeof(*viable), compare_candidates);
        for (rank = 0u; rank < viable_count; ++rank) {
            unsigned trial;
            unsigned passes = 1u;
            float evm_sum = viable[rank].evm_rms;
            float payload_sum = viable[rank].payload_bps;
            ++result->candidates_verified;
            for (trial = 0u; trial < verification_trials; ++trial) {
                um_rx_metrics metrics;
                float duration = 0.0f;
                float payload_bps = 0.0f;
                uint32_t seed = UINT32_C(0x10000) +
                                (uint32_t)(rank * verification_trials + trial);
                int status;
                char line[256];
                fill_probe(verification_probe, sizeof(verification_probe),
                           seed ^ UINT32_C(0x6a09e667));
                memset(&metrics, 0, sizeof(metrics));
                status = try_candidate(
                    &viable[rank].config, channel, seed, verification_probe,
                    sizeof(verification_probe), &duration, &payload_bps,
                    &metrics);
                ++result->verification_frames;
                result->estimated_seconds += duration;
                if (logger != NULL) {
                    (void)snprintf(
                        line, sizeof(line),
                        "verify rank=%zu trial=%u/%u qam=%u fec=%s: %s "
                        "snr=%.1fdB evm=%.3f",
                        rank + 1u, trial + 1u, verification_trials,
                        1u << viable[rank].config.qam_bits,
                        fec_name(viable[rank].config.fec_rate),
                        status == UM_OK ? "pass" :
                                          um_status_string(status),
                        metrics.estimated_snr_db, metrics.evm_rms);
                    logger(logger_context, line);
                }
                if (status == UM_ERR_MEMORY) {
                    free(viable);
                    return status;
                }
                if (status != UM_OK) {
                    break;
                }
                ++passes;
                evm_sum += metrics.evm_rms;
                payload_sum += payload_bps;
            }
            if (passes == verification_trials + 1u) {
                result->config = viable[rank].config;
                result->score = viable[rank].score;
                result->payload_bps = payload_sum / (float)passes;
                result->success_rate = 1.0f;
                result->evm_rms = evm_sum / (float)passes;
                free(viable);
                return UM_OK;
            }
        }
    }
    free(viable);
    return UM_ERR_CRC;
}
