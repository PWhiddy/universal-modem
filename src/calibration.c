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
    int baseline;
} viable_candidate;

static const frequency_range default_ranges[] = {
    {48u, 362u}, {64u, 490u}, {128u, 640u}
};

static const frequency_range high_ranges[] = {
    {48u, 320u}, {48u, 448u}, {48u, 576u}, {64u, 362u},
    {64u, 490u}, {64u, 640u}, {96u, 448u}, {96u, 576u},
    {128u, 640u}, {128u, 768u}
};

static const unsigned qam_options[] = {2u, 4u, 6u};
static const unsigned default_prefixes[] = {384u, 768u, 1024u};
static const unsigned high_prefixes[] = {256u, 384u, 512u, 640u,
                                         768u, 896u, 1024u};
static const unsigned default_windows[] = {64u};
static const unsigned high_windows[] = {0u, 32u, 64u, 128u};
static const um_fec_rate fec_options[] = {UM_FEC_RATE_1_2, UM_FEC_RATE_2_3,
                                          UM_FEC_RATE_3_4};

/* High-quality live calibration samples the full grid uniformly. */
static size_t live_high_candidate_count(void)
{
    size_t count = 0u;
    size_t valid = 0u;
    size_t range_index;
    size_t qam_index;
    size_t prefix_index;
    size_t fec_index;
    size_t window_index;
    for (range_index = 0u;
         range_index < sizeof(high_ranges) / sizeof(high_ranges[0]);
         ++range_index) {
        for (qam_index = 0u;
             qam_index < sizeof(qam_options) / sizeof(qam_options[0]);
             ++qam_index) {
            for (prefix_index = 0u;
                 prefix_index < sizeof(high_prefixes) /
                                    sizeof(high_prefixes[0]);
                 ++prefix_index) {
                for (fec_index = 0u;
                     fec_index < sizeof(fec_options) / sizeof(fec_options[0]);
                     ++fec_index) {
                    for (window_index = 0u;
                         window_index < sizeof(high_windows) /
                                            sizeof(high_windows[0]);
                         ++window_index) {
                        um_modem_config candidate = um_modem_default_config();
                        candidate.first_bin = high_ranges[range_index].first_bin;
                        candidate.last_bin = high_ranges[range_index].last_bin;
                        candidate.qam_bits = qam_options[qam_index];
                        candidate.cyclic_prefix = high_prefixes[prefix_index];
                        candidate.fec_rate = fec_options[fec_index];
                        candidate.window_samples = high_windows[window_index];
                        if (um_modem_config_validate(&candidate) != UM_OK) {
                            continue;
                        }
                        if (valid++ % 27u == 0u) {
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
        return 1u + live_high_candidate_count();
    }
    return 10u;
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
    size_t selected_index = 0u;
    size_t range_index;
    size_t qam_index;
    size_t prefix_index;
    size_t fec_index;
    size_t window_index;
    if (config == NULL) {
        return UM_ERR_ARGUMENT;
    }
    /* Candidate zero is the non-negotiable working baseline. */
    if (index == 0u) {
        *config = um_modem_robust_config();
        return um_modem_config_validate(config);
    }
    --index;
    if (high_quality == 0) {
        size_t range;
        size_t qam;
        if (index >= 9u) {
            return UM_ERR_ARGUMENT;
        }
        range = index / 3u;
        qam = index % 3u;
        *config = um_modem_default_config();
        config->first_bin = default_ranges[range].first_bin;
        config->last_bin = default_ranges[range].last_bin;
        config->qam_bits = qam_options[qam];
        config->cyclic_prefix = default_prefixes[(range + qam) % 3u];
        config->fec_rate = fec_options[(range + 2u * qam) % 3u];
        config->window_samples = default_windows[0];
        return um_modem_config_validate(config);
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
                        candidate.first_bin = ranges[range_index].first_bin;
                        candidate.last_bin = ranges[range_index].last_bin;
                        candidate.qam_bits = qam_options[qam_index];
                        candidate.cyclic_prefix = prefixes[prefix_index];
                        candidate.fec_rate = fec_options[fec_index];
                        candidate.window_samples = windows[window_index];
                        if (um_modem_config_validate(&candidate) != UM_OK) {
                            continue;
                        }
                        if (current++ % 27u != 0u) {
                            continue;
                        }
                        if (selected_index++ == index) {
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
    uint8_t probe[32];
    uint8_t verification_probe[128];
    size_t candidate_count = um_live_calibration_candidate_count(high_quality);
    viable_candidate *viable = NULL;
    size_t viable_count = 0u;
    size_t candidate_index;

    if (channel == NULL || result == NULL || candidate_count == 0u) {
        return UM_ERR_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->config = um_modem_robust_config();
    fill_probe(probe, sizeof(probe), UINT32_C(0xc001d00d));
    viable = (viable_candidate *)malloc(candidate_count * sizeof(*viable));
    if (viable == NULL) {
        return UM_ERR_MEMORY;
    }

    for (candidate_index = 0u; candidate_index < candidate_count;
         ++candidate_index) {
        um_modem_config candidate;
        um_rx_metrics metrics;
        float duration = 0.0f;
        float payload_bps = 0.0f;
        float raw_bps;
        float quality;
        float score;
        int baseline = candidate_index == 0u;
        int reliable;
        int status;
        char line[320];

        status = um_live_calibration_candidate_get(
            high_quality, candidate_index, &candidate);
        if (status != UM_OK) {
            free(viable);
            return status;
        }
        ++result->candidates_tested;
        memset(&metrics, 0, sizeof(metrics));
        status = try_candidate(
            &candidate, channel, (uint32_t)result->candidates_tested, probe,
            sizeof(probe), &duration, &payload_bps, &metrics);
        if (status == UM_ERR_MEMORY) {
            free(viable);
            return status;
        }
        reliable = status == UM_OK &&
                   (baseline != 0
                        ? um_modem_metrics_have_baseline_margin(&metrics)
                        : um_modem_metrics_have_margin(&candidate, &metrics)) !=
                       0;
        result->estimated_seconds += duration;
        raw_bps = (float)um_modem_data_carriers(&candidate) *
                  (float)candidate.qam_bits * fec_ratio(candidate.fec_rate) *
                  (float)UM_SAMPLE_RATE /
                  ((float)(candidate.fft_size + candidate.cyclic_prefix) *
                   (float)candidate.symbol_repetitions);
        quality = reliable != 0
                      ? 1.0f /
                            (1.0f + 4.0f * metrics.evm_rms * metrics.evm_rms)
                      : 0.0f;
        if (candidate.window_samples == 0u) {
            quality *= 0.94f;
        } else if (candidate.window_samples < 8u) {
            quality *= 0.98f;
        } else if (candidate.window_samples > 8u) {
            quality *= 0.995f;
        }
        score = raw_bps * quality;
        if (reliable != 0) {
            ++result->candidates_viable;
            viable[viable_count].config = candidate;
            viable[viable_count].score = score;
            viable[viable_count].payload_bps = payload_bps;
            viable[viable_count].evm_rms = metrics.evm_rms;
            viable[viable_count].baseline = baseline;
            ++viable_count;
        }
        if (logger != NULL) {
            (void)snprintf(
                line, sizeof(line),
                "calib %zu%s band=%.0f-%.0fHz qam=%u cp=%.2fms fec=%s "
                "win=%u repeats=%u: %s snr=%.1fdB evm=%.3f "
                "payload=%.0fbps score=%.0f",
                result->candidates_tested, baseline != 0 ? " baseline" : "",
                (double)candidate.first_bin * (double)UM_SAMPLE_RATE /
                    (double)candidate.fft_size,
                (double)candidate.last_bin * (double)UM_SAMPLE_RATE /
                    (double)candidate.fft_size,
                1u << candidate.qam_bits,
                1000.0 * (double)candidate.cyclic_prefix /
                    (double)UM_SAMPLE_RATE,
                fec_name(candidate.fec_rate), candidate.window_samples,
                candidate.symbol_repetitions,
                reliable != 0
                    ? "pass"
                    : status == UM_OK ? "marginal"
                                      : um_status_string(status),
                metrics.estimated_snr_db, metrics.evm_rms, payload_bps,
                score);
            logger(logger_context, line);
        }
        if (baseline != 0 && reliable == 0) {
            free(viable);
            return status != UM_OK ? status : UM_ERR_RELIABILITY;
        }
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
                int reliable;
                char line[256];
                fill_probe(verification_probe, sizeof(verification_probe),
                           seed ^ UINT32_C(0x6a09e667));
                memset(&metrics, 0, sizeof(metrics));
                status = try_candidate(
                    &viable[rank].config, channel, seed, verification_probe,
                    sizeof(verification_probe), &duration, &payload_bps,
                    &metrics);
                reliable = status == UM_OK &&
                           (viable[rank].baseline != 0
                                ? um_modem_metrics_have_baseline_margin(
                                      &metrics)
                                : um_modem_metrics_have_margin(
                                      &viable[rank].config, &metrics)) != 0;
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
                        reliable != 0
                            ? "pass"
                            : status == UM_OK ? "marginal"
                                              : um_status_string(status),
                        metrics.estimated_snr_db, metrics.evm_rms);
                    logger(logger_context, line);
                }
                if (status == UM_ERR_MEMORY) {
                    free(viable);
                    return status;
                }
                if (reliable == 0) {
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
