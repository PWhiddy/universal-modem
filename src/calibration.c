#include "um_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    um_modem_config config;
    size_t candidate_id;
} viable_candidate;

#define CALIBRATION_DEFAULT_BUDGET 12u
#define CALIBRATION_HIGH_BUDGET 64u
#define CALIBRATION_HIGH_RATE_PAYLOAD_BYTES 512u

static const unsigned calibration_prefix_values[] = {
    1024u, 896u, 768u, 640u, 512u, 384u, 256u, 128u, 64u, 32u};
static const unsigned calibration_high_values[] = {
    298u, 362u, 448u, 512u, 640u, 768u, 853u, 896u};
static const unsigned calibration_low_values[] = {128u, 96u, 64u, 48u, 32u};
static const unsigned calibration_narrow_values[] = {48u, 64u, 96u, 128u};
static const unsigned calibration_sync_values[] = {
    2048u, 1536u, 1024u, 768u, 512u};
static const unsigned calibration_gap_values[] = {
    3072u, 2560u, 2048u, 1536u, 1024u, 512u, 0u};

static um_modem_config calibration_wide_anchor(void)
{
    um_modem_config config = um_modem_default_config();
    config.first_bin = 48u;
    config.last_bin = 768u;
    config.cyclic_prefix = 256u;
    config.window_samples = 64u;
    config.sync_samples = 1024u;
    config.sync_gap = 1536u;
    config.training_symbols = 2u;
    config.qam_bits = 4u;
    config.fec_rate = UM_FEC_RATE_2_3;
    return config;
}

static um_modem_config calibration_pristine_anchor(void)
{
    um_modem_config config = um_modem_default_config();
    config.first_bin = 32u;
    config.last_bin = 896u;
    config.cyclic_prefix = 64u;
    config.window_samples = 32u;
    config.sync_samples = 512u;
    config.sync_gap = 512u;
    config.training_symbols = 2u;
    config.qam_bits = 6u;
    config.fec_rate = UM_FEC_RATE_3_4;
    return config;
}

static size_t divide_round_up(size_t value, size_t divisor)
{
    return value / divisor + (value % divisor != 0u ? 1u : 0u);
}

static int config_equal(const um_modem_config *left,
                        const um_modem_config *right)
{
    return left->fft_size == right->fft_size &&
           left->first_bin == right->first_bin &&
           left->last_bin == right->last_bin &&
           left->cyclic_prefix == right->cyclic_prefix &&
           left->window_samples == right->window_samples &&
           left->sync_samples == right->sync_samples &&
           left->sync_gap == right->sync_gap &&
           left->training_symbols == right->training_symbols &&
           left->symbol_repetitions == right->symbol_repetitions &&
           left->qam_bits == right->qam_bits &&
           left->fec_rate == right->fec_rate;
}

float um_calibration_payload_rate(const um_modem_config *config,
                                  size_t payload_bytes)
{
    size_t carriers;
    size_t header_bits;
    size_t payload_bits;
    size_t header_symbols;
    size_t payload_symbols;
    size_t total_symbols;
    size_t sample_count;
    if (um_modem_config_validate(config) != UM_OK || payload_bytes == 0u ||
        payload_bytes > UM_MAX_PAYLOAD) {
        return 0.0f;
    }
    carriers = um_modem_data_carriers(config);
    header_bits = um_fec_encoded_bits(UM_HEADER_BITS, UM_FEC_RATE_1_2);
    payload_bits = um_fec_encoded_bits(payload_bytes * 8u, config->fec_rate);
    header_symbols = divide_round_up(header_bits, carriers * 2u);
    payload_symbols =
        divide_round_up(payload_bits, carriers * config->qam_bits);
    total_symbols = config->training_symbols +
                    (header_symbols + payload_symbols) *
                        config->symbol_repetitions;
    sample_count = UM_SYNC_LEAD + config->sync_samples + config->sync_gap +
                   total_symbols *
                       (config->fft_size + config->cyclic_prefix) +
                   config->window_samples + 64u;
    return (float)(payload_bytes * 8u) * (float)UM_SAMPLE_RATE /
           (float)sample_count;
}

/*
 * Calibration deliberately probes until it finds the channel boundary.  A
 * mode at that boundary is useful evidence, but it is the wrong long-lived
 * operating point: a person moving, fan noise, clock drift, or a slightly
 * longer payload can turn a short successful probe into repeated retries.
 *
 * Convert the measured frontier into a deterministic guarded mode.  Every
 * change is monotonic with respect to payload robustness or acquisition
 * margin, and the resulting mode is verified over the real link before use.
 * This is intentionally a structural backoff rather than many more trials of
 * the same marginal mode.
 */
int um_calibration_guard_config(const um_modem_config *measured,
                                um_modem_config *guarded)
{
    size_t index;
    unsigned lower_high_edge = measured != NULL ? measured->last_bin : 0u;
    unsigned higher_low_edge = measured != NULL ? measured->first_bin : 0u;
    unsigned longer_prefix = UINT_MAX;
    if (measured == NULL || guarded == NULL ||
        um_modem_config_validate(measured) != UM_OK) {
        return UM_ERR_ARGUMENT;
    }
    *guarded = *measured;

    /* Spend the reliability reserve on coding before constellation size.
     * Dropping both by one tier turned a verified 16-QAM frontier into a
     * QPSK 2/3 link whose physical ceiling was below common TLS handshake
     * deadlines.  Rate-1/2 coding retains every convolutional output bit;
     * 16-QAM with that code has more redundancy than the measured 16-QAM
     * 2/3 or 3/4 frontier while carrying 50-100% more useful bits than the
     * old guarded mode.  Only a measured 64-QAM mode is stepped down to
     * 16-QAM. */
    guarded->fec_rate = UM_FEC_RATE_1_2;
    if (guarded->qam_bits > 4u) {
        guarded->qam_bits = 4u;
    }
    if (guarded->qam_bits == 2u &&
        measured->fec_rate == UM_FEC_RATE_1_2 &&
        guarded->symbol_repetitions < 2u) {
        guarded->symbol_repetitions = 2u;
    }

    /* Move each explored spectrum edge inward.  The top two high-band tiers
     * are close together and device-dependent, so never deploy above the
     * broadly useful 18 kHz tier. */
    if (measured->last_bin > 298u) {
        lower_high_edge = 0u;
        for (index = 0u;
             index < sizeof(calibration_high_values) /
                         sizeof(calibration_high_values[0]);
             ++index) {
            unsigned value = calibration_high_values[index];
            if (value < measured->last_bin && value > lower_high_edge) {
                lower_high_edge = value;
            }
        }
        if (lower_high_edge > 768u) {
            lower_high_edge = 768u;
        }
        if (lower_high_edge >= measured->first_bin) {
            guarded->last_bin = lower_high_edge;
        }
    }
    if (measured->first_bin < 64u) {
        higher_low_edge = UINT_MAX;
        for (index = 0u;
             index < sizeof(calibration_low_values) /
                         sizeof(calibration_low_values[0]);
             ++index) {
            unsigned value = calibration_low_values[index];
            if (value > measured->first_bin && value < higher_low_edge) {
                higher_low_edge = value;
            }
        }
        if (higher_low_edge <= guarded->last_bin) {
            guarded->first_bin = higher_low_edge;
        }
    }

    /* Back off delay-spread tolerance by one measured tier, with a 10.7 ms
     * room-acoustics floor.  Forcing every passing mode all the way to the
     * 21.3 ms maximum discarded up to a third of the symbol capacity even
     * when calibration had demonstrated a much shorter prefix. */
    for (index = 0u;
         index < sizeof(calibration_prefix_values) /
                     sizeof(calibration_prefix_values[0]);
         ++index) {
        unsigned value = calibration_prefix_values[index];
        if (value > measured->cyclic_prefix && value < longer_prefix) {
            longer_prefix = value;
        }
    }
    if (longer_prefix != UINT_MAX) {
        guarded->cyclic_prefix = longer_prefix;
    }
    if (guarded->cyclic_prefix < 512u) {
        guarded->cyclic_prefix = 512u;
    }
    if (guarded->window_samples < 64u) {
        guarded->window_samples = 64u;
    }
    if (guarded->sync_samples < 1536u) {
        guarded->sync_samples = 1536u;
    }
    if (guarded->sync_gap < 2048u) {
        guarded->sync_gap = 2048u;
    }
    if (guarded->training_symbols < 3u) {
        guarded->training_symbols = 3u;
    }
    return um_modem_config_validate(guarded);
}

size_t um_calibration_search_budget(int high_quality)
{
    return high_quality != 0 ? CALIBRATION_HIGH_BUDGET
                             : CALIBRATION_DEFAULT_BUDGET;
}

const char *um_calibration_step_name(um_calibration_step step)
{
    switch (step) {
    case UM_CALIB_STEP_BASELINE:
        return "working-baseline";
    case UM_CALIB_STEP_REPETITIONS:
        return "fewer-repetitions";
    case UM_CALIB_STEP_QAM:
        return "higher-qam";
    case UM_CALIB_STEP_FEC:
        return "higher-code-rate";
    case UM_CALIB_STEP_PREFIX:
        return "shorter-prefix";
    case UM_CALIB_STEP_HIGH_BAND:
        return "wider-high-band";
    case UM_CALIB_STEP_LOW_BAND:
        return "wider-low-band";
    case UM_CALIB_STEP_SYNC:
        return "shorter-sync";
    case UM_CALIB_STEP_GAP:
        return "shorter-settling-gap";
    case UM_CALIB_STEP_TRAINING:
        return "less-training";
    case UM_CALIB_STEP_NARROW_BAND:
        return "narrower-clean-band";
    case UM_CALIB_STEP_DATA_DEFAULT:
        return "grounded-data-default";
    case UM_CALIB_STEP_MORE_REPETITIONS:
        return "more-repetitions-recovery";
    case UM_CALIB_STEP_WIDE_ANCHOR:
        return "wide-speed-anchor";
    case UM_CALIB_STEP_PRISTINE_ANCHOR:
        return "pristine-speed-anchor";
    case UM_CALIB_STEP_ULTRA_ROBUST_BAND:
        return "ultra-robust-band";
    default:
        return "unknown";
    }
}

static float search_step_bias(um_calibration_step step)
{
    switch (step) {
    case UM_CALIB_STEP_REPETITIONS:
        return 1.12f;
    case UM_CALIB_STEP_QAM:
        return 1.03f;
    case UM_CALIB_STEP_FEC:
        return 1.07f;
    case UM_CALIB_STEP_HIGH_BAND:
        return 1.08f;
    case UM_CALIB_STEP_LOW_BAND:
        return 0.92f;
    case UM_CALIB_STEP_SYNC:
    case UM_CALIB_STEP_GAP:
        return 1.01f;
    case UM_CALIB_STEP_PREFIX:
        return 1.03f;
    case UM_CALIB_STEP_TRAINING:
        return 1.04f;
    case UM_CALIB_STEP_NARROW_BAND:
        return 0.80f;
    case UM_CALIB_STEP_DATA_DEFAULT:
        return 1.20f;
    case UM_CALIB_STEP_MORE_REPETITIONS:
        return 0.85f;
    case UM_CALIB_STEP_WIDE_ANCHOR:
    case UM_CALIB_STEP_PRISTINE_ANCHOR:
        return 1.10f;
    case UM_CALIB_STEP_ULTRA_ROBUST_BAND:
        return 0.90f;
    default:
        return 1.0f;
    }
}

static int search_add(um_calibration_search *search, size_t parent,
                      um_calibration_step step,
                      const um_modem_config *config)
{
    size_t index;
    if (um_modem_config_validate(config) != UM_OK) {
        return UM_OK;
    }
    for (index = 0u; index < search->node_count; ++index) {
        if (config_equal(config, &search->nodes[index].config) != 0) {
            return UM_OK;
        }
    }
    if (search->node_count == UM_CALIBRATION_SEARCH_MAX_NODES) {
        /* The bounded frontier already contains more than either probe budget. */
        return UM_OK;
    }
    index = search->node_count++;
    search->nodes[index].config = *config;
    search->nodes[index].priority =
        um_calibration_payload_rate(config, search->rate_payload_bytes) *
        search_step_bias(step);
    search->nodes[index].parent = parent;
    search->nodes[index].step = step;
    search->nodes[index].tested = 0;
    search->nodes[index].recorded = 0;
    search->nodes[index].passed = 0;
    return UM_OK;
}

static unsigned next_lower_value(unsigned current, const unsigned *values,
                                 size_t count)
{
    size_t index;
    for (index = 0u; index < count; ++index) {
        if (values[index] < current) {
            return values[index];
        }
    }
    return current;
}

static unsigned next_higher_value(unsigned current, const unsigned *values,
                                  size_t count)
{
    size_t index;
    for (index = 0u; index < count; ++index) {
        if (values[index] > current) {
            return values[index];
        }
    }
    return current;
}

static int improves_probe_rate(const um_calibration_search *search,
                               const um_modem_config *base,
                               const um_modem_config *candidate)
{
    return um_calibration_payload_rate(candidate,
                                       search->rate_payload_bytes) >
           um_calibration_payload_rate(base,
                                       search->rate_payload_bytes) +
               0.5f;
}

static int search_expand(um_calibration_search *search, size_t parent)
{
    const um_modem_config *base = &search->nodes[parent].config;
    um_modem_config candidate;
    size_t option;
    unsigned value;
    int status;

#define ADD_CHANGED(field, changed_value, changed_step)                         \
    do {                                                                        \
        candidate = *base;                                                      \
        candidate.field = (changed_value);                                      \
        status = search_add(search, parent, (changed_step), &candidate);        \
        if (status != UM_OK) {                                                  \
            return status;                                                      \
        }                                                                       \
    } while (0)

    if (parent == 0u) {
        candidate = um_modem_default_config();
        status = search_add(search, parent, UM_CALIB_STEP_DATA_DEFAULT,
                            &candidate);
        if (status != UM_OK) {
            return status;
        }
        if (search->high_quality != 0) {
            candidate = calibration_wide_anchor();
            status = search_add(search, parent, UM_CALIB_STEP_WIDE_ANCHOR,
                                &candidate);
            if (status != UM_OK) {
                return status;
            }
            candidate = calibration_pristine_anchor();
            status = search_add(search, parent,
                                UM_CALIB_STEP_PRISTINE_ANCHOR, &candidate);
            if (status != UM_OK) {
                return status;
            }
        }
    }
    if (base->symbol_repetitions > 1u) {
        ADD_CHANGED(symbol_repetitions, base->symbol_repetitions - 1u,
                    UM_CALIB_STEP_REPETITIONS);
    }
    for (value = base->qam_bits + 2u; value <= 6u; value += 2u) {
        candidate = *base;
        candidate.qam_bits = value;
        if (improves_probe_rate(search, base, &candidate) != 0) {
            status = search_add(search, parent, UM_CALIB_STEP_QAM,
                                &candidate);
            if (status != UM_OK) {
                return status;
            }
            break;
        }
    }
    for (value = (unsigned)base->fec_rate + 1u;
         value <= (unsigned)UM_FEC_RATE_3_4; ++value) {
        candidate = *base;
        candidate.fec_rate = (um_fec_rate)value;
        if (improves_probe_rate(search, base, &candidate) != 0) {
            status = search_add(search, parent, UM_CALIB_STEP_FEC,
                                &candidate);
            if (status != UM_OK) {
                return status;
            }
            break;
        }
    }
    value = next_lower_value(
        base->cyclic_prefix, calibration_prefix_values,
        sizeof(calibration_prefix_values) /
            sizeof(calibration_prefix_values[0]));
    if (value != base->cyclic_prefix) {
        candidate = *base;
        candidate.cyclic_prefix = value;
        if (candidate.window_samples > value) {
            candidate.window_samples = value;
        }
        status = search_add(search, parent, UM_CALIB_STEP_PREFIX,
                            &candidate);
        if (status != UM_OK) {
            return status;
        }
    }
    for (option = 0u;
         option < (search->high_quality != 0
                       ? sizeof(calibration_high_values) /
                             sizeof(calibration_high_values[0])
                       : 4u);
         ++option) {
        if (calibration_high_values[option] <= base->last_bin) {
            continue;
        }
        candidate = *base;
        candidate.last_bin = calibration_high_values[option];
        if (improves_probe_rate(search, base, &candidate) != 0) {
            status = search_add(search, parent, UM_CALIB_STEP_HIGH_BAND,
                                &candidate);
            if (status != UM_OK) {
                return status;
            }
            break;
        }
    }
    for (option = 0u;
         option < sizeof(calibration_low_values) /
                      sizeof(calibration_low_values[0]);
         ++option) {
        if (calibration_low_values[option] >= base->first_bin) {
            continue;
        }
        candidate = *base;
        candidate.first_bin = calibration_low_values[option];
        if (improves_probe_rate(search, base, &candidate) != 0) {
            status = search_add(search, parent, UM_CALIB_STEP_LOW_BAND,
                                &candidate);
            if (status != UM_OK) {
                return status;
            }
            break;
        }
    }
    if (base->training_symbols > 2u) {
        ADD_CHANGED(training_symbols, base->training_symbols - 1u,
                    UM_CALIB_STEP_TRAINING);
    }
    if (search->high_quality != 0) {
        value = next_lower_value(
            base->sync_samples, calibration_sync_values,
            sizeof(calibration_sync_values) /
                sizeof(calibration_sync_values[0]));
        if (value != base->sync_samples) {
            ADD_CHANGED(sync_samples, value, UM_CALIB_STEP_SYNC);
        }
        value = next_lower_value(
            base->sync_gap, calibration_gap_values,
            sizeof(calibration_gap_values) /
                sizeof(calibration_gap_values[0]));
        if (value != base->sync_gap) {
            ADD_CHANGED(sync_gap, value, UM_CALIB_STEP_GAP);
        }
        value = next_higher_value(
            base->first_bin, calibration_narrow_values,
            sizeof(calibration_narrow_values) /
                sizeof(calibration_narrow_values[0]));
        if (value != base->first_bin && value < base->last_bin) {
            ADD_CHANGED(first_bin, value, UM_CALIB_STEP_NARROW_BAND);
        }
    }
#undef ADD_CHANGED
    return UM_OK;
}

static int search_expand_recovery(um_calibration_search *search,
                                  size_t failed)
{
    const um_calibration_search_node *node = &search->nodes[failed];
    um_modem_config candidate;
    int status;
    if ((failed != 0u && node->step != UM_CALIB_STEP_MORE_REPETITIONS) ||
        um_modem_config_uses_robust_gate(&node->config) == 0) {
        return UM_OK;
    }
    if (node->config.symbol_repetitions < UM_MAX_SYMBOL_REPETITIONS) {
        candidate = node->config;
        ++candidate.symbol_repetitions;
        return search_add(search, failed, UM_CALIB_STEP_MORE_REPETITIONS,
                          &candidate);
    }
    if (node->config.first_bin != 64u || node->config.last_bin != 298u) {
        return UM_OK;
    }

    /*
     * Repetition cannot repair carriers that are consistently buried in a
     * fade or narrow-band interferer.  Once the full-band four-repeat mode
     * has failed, try several deliberately low-rate acoustic cores.  These
     * are independent alternatives: failure in one core must not prune the
     * others, while a passing core becomes a normal parent from which the
     * adaptive search can cautiously recover speed.
     */
    candidate = node->config;
    candidate.last_bin = 234u; /* 1.500-5.484 kHz */
    status = search_add(search, failed, UM_CALIB_STEP_ULTRA_ROBUST_BAND,
                        &candidate);
    if (status != UM_OK) {
        return status;
    }
    candidate = node->config;
    candidate.last_bin = 192u; /* 1.500-4.500 kHz */
    status = search_add(search, failed, UM_CALIB_STEP_ULTRA_ROBUST_BAND,
                        &candidate);
    if (status != UM_OK) {
        return status;
    }
    candidate = node->config;
    candidate.first_bin = 96u;
    candidate.last_bin = 234u; /* 2.250-5.484 kHz */
    status = search_add(search, failed, UM_CALIB_STEP_ULTRA_ROBUST_BAND,
                        &candidate);
    if (status != UM_OK) {
        return status;
    }
    candidate = node->config;
    candidate.first_bin = 128u; /* 3.000-6.984 kHz */
    return search_add(search, failed, UM_CALIB_STEP_ULTRA_ROBUST_BAND,
                      &candidate);
}

int um_calibration_search_init(um_calibration_search *search,
                               int high_quality,
                               size_t rate_payload_bytes)
{
    um_modem_config baseline;
    if (search == NULL || rate_payload_bytes == 0u ||
        rate_payload_bytes > UM_MAX_PAYLOAD) {
        return UM_ERR_ARGUMENT;
    }
    memset(search, 0, sizeof(*search));
    search->high_quality = high_quality != 0;
    search->budget = um_calibration_search_budget(high_quality);
    search->rate_payload_bytes = rate_payload_bytes;
    baseline = um_modem_robust_config();
    return search_add(search, 0u, UM_CALIB_STEP_BASELINE, &baseline);
}

static unsigned search_variant(const um_calibration_search_node *node)
{
    switch (node->step) {
    case UM_CALIB_STEP_REPETITIONS:
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
    case UM_CALIB_STEP_DATA_DEFAULT:
        return 0u;
    case UM_CALIB_STEP_MORE_REPETITIONS:
        return node->config.symbol_repetitions;
    case UM_CALIB_STEP_WIDE_ANCHOR:
    case UM_CALIB_STEP_PRISTINE_ANCHOR:
        return 0u;
    case UM_CALIB_STEP_ULTRA_ROBUST_BAND:
        return (node->config.first_bin << 16u) | node->config.last_bin;
    default:
        return 0u;
    }
}

static void search_outcomes(const um_calibration_search *search,
                            um_calibration_step step, unsigned variant,
                            int match_variant, unsigned *attempts,
                            unsigned *passes)
{
    size_t index;
    *attempts = 0u;
    *passes = 0u;
    for (index = 0u; index < search->node_count; ++index) {
        const um_calibration_search_node *node = &search->nodes[index];
        if (node->recorded == 0 || node->step != step ||
            (match_variant != 0 && search_variant(node) != variant)) {
            continue;
        }
        ++*attempts;
        if (node->passed != 0) {
            ++*passes;
        }
    }
}

void um_calibration_search_step_results(const um_calibration_search *search,
                                        um_calibration_step step,
                                        unsigned *attempts,
                                        unsigned *passes)
{
    if (attempts == NULL || passes == NULL) {
        return;
    }
    if (search == NULL || step >= UM_CALIB_STEP_COUNT) {
        *attempts = 0u;
        *passes = 0u;
        return;
    }
    search_outcomes(search, step, 0u, 0, attempts, passes);
}

static unsigned search_failure_limit(um_calibration_step step)
{
    switch (step) {
    case UM_CALIB_STEP_QAM:
    case UM_CALIB_STEP_REPETITIONS:
    case UM_CALIB_STEP_LOW_BAND:
    case UM_CALIB_STEP_NARROW_BAND:
        return 2u;
    case UM_CALIB_STEP_WIDE_ANCHOR:
    case UM_CALIB_STEP_PRISTINE_ANCHOR:
    case UM_CALIB_STEP_ULTRA_ROBUST_BAND:
        return 1u;
    case UM_CALIB_STEP_FEC:
    case UM_CALIB_STEP_PREFIX:
        return 4u;
    default:
        return 3u;
    }
}

static unsigned search_success_limit(um_calibration_step step)
{
    switch (step) {
    case UM_CALIB_STEP_BASELINE:
    case UM_CALIB_STEP_DATA_DEFAULT:
    case UM_CALIB_STEP_MORE_REPETITIONS:
    case UM_CALIB_STEP_WIDE_ANCHOR:
    case UM_CALIB_STEP_PRISTINE_ANCHOR:
    case UM_CALIB_STEP_ULTRA_ROBUST_BAND:
        return 1u;
    default:
        /*
         * One successful target may be combined once with the best other
         * branch.  Further copies spend the probe budget rediscovering an
         * already-known result instead of locating the next boundary.
         */
        return 2u;
    }
}

static int search_suppressed(const um_calibration_search *search,
                             size_t candidate)
{
    const um_calibration_search_node *node = &search->nodes[candidate];
    unsigned attempts;
    unsigned passes;
    search_outcomes(search, node->step, search_variant(node), 1, &attempts,
                    &passes);
    if (passes != 0u) {
        return attempts >= search_success_limit(node->step);
    }
    if (attempts < search_failure_limit(node->step)) {
        return 0;
    }
    return 1;
}

static float search_observed_factor(unsigned attempts, unsigned passes)
{
    if (attempts == 0u) {
        return 1.0f;
    }
    return 0.5f + (float)(passes + 1u) / (float)(attempts + 2u);
}

static float search_dynamic_priority(const um_calibration_search *search,
                                     size_t candidate)
{
    const um_calibration_search_node *node = &search->nodes[candidate];
    unsigned step_attempts;
    unsigned step_passes;
    unsigned variant_attempts;
    unsigned variant_passes;
    search_outcomes(search, node->step, 0u, 0, &step_attempts,
                    &step_passes);
    search_outcomes(search, node->step, search_variant(node), 1,
                    &variant_attempts, &variant_passes);
    {
        float exploration = 1.0f;
        float continuation = 1.0f;
        float variant_factor;
        if (step_attempts == 0u) {
            switch (node->step) {
            case UM_CALIB_STEP_REPETITIONS:
            case UM_CALIB_STEP_QAM:
            case UM_CALIB_STEP_FEC:
            case UM_CALIB_STEP_PREFIX:
            case UM_CALIB_STEP_HIGH_BAND:
            case UM_CALIB_STEP_TRAINING:
                /*
                 * Reserve the beginning of the bounded search for one
                 * measurement along every important rate axis.  Without a
                 * strong first-look bonus, descendants of the first fast
                 * mode can consume the default twelve-probe budget before
                 * code rate (or another independent dimension) is sampled.
                 */
                exploration = 5.00f;
                break;
            case UM_CALIB_STEP_SYNC:
            case UM_CALIB_STEP_GAP:
                if (search->high_quality != 0) {
                    exploration = 5.00f;
                }
                break;
            case UM_CALIB_STEP_DATA_DEFAULT:
                exploration = 5.00f;
                break;
            case UM_CALIB_STEP_WIDE_ANCHOR:
            case UM_CALIB_STEP_PRISTINE_ANCHOR:
                exploration = 5.00f;
                break;
            case UM_CALIB_STEP_MORE_REPETITIONS:
            case UM_CALIB_STEP_ULTRA_ROBUST_BAND:
                exploration = 1.50f;
                break;
            default:
                break;
            }
        }
        if (node->parent < search->node_count && node->parent != candidate &&
            search->nodes[node->parent].step == node->step) {
            continuation = 1.25f;
        }
        /*
         * Once a particular target value has worked, deepen that proven
         * branch instead of repeatedly applying the same mutation to every
         * neighboring configuration in the combinatorial frontier.
         */
        variant_factor =
            variant_passes != 0u
                ? 0.65f
                : search_observed_factor(variant_attempts, variant_passes);
        return node->priority * exploration *
               search_observed_factor(step_attempts, step_passes) *
               variant_factor * continuation;
    }
}

int um_calibration_search_next(um_calibration_search *search,
                               size_t *candidate_id,
                               um_modem_config *config,
                               um_calibration_step *step)
{
    size_t best = SIZE_MAX;
    size_t index;
    if (search == NULL || candidate_id == NULL || config == NULL ||
        step == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (search->tested_count >= search->budget) {
        return 0;
    }
    for (index = 0u; index < search->node_count; ++index) {
        if (search->nodes[index].tested == 0 &&
            search_suppressed(search, index) == 0) {
            float priority = search_dynamic_priority(search, index);
            float best_priority = 0.0f;
            if (best != SIZE_MAX) {
                best_priority = search_dynamic_priority(search, best);
            }
            if (best == SIZE_MAX || priority > best_priority) {
                best = index;
            }
        }
    }
    if (best == SIZE_MAX) {
        return 0;
    }
    search->nodes[best].tested = 1;
    ++search->tested_count;
    *candidate_id = best;
    *config = search->nodes[best].config;
    *step = search->nodes[best].step;
    return 1;
}

int um_calibration_search_record(um_calibration_search *search,
                                 size_t candidate_id, int passed)
{
    if (search == NULL || candidate_id >= search->node_count ||
        search->nodes[candidate_id].tested == 0 ||
        search->nodes[candidate_id].recorded != 0) {
        return UM_ERR_ARGUMENT;
    }
    search->nodes[candidate_id].recorded = 1;
    search->nodes[candidate_id].passed = passed != 0;
    if (passed == 0) {
        return search_expand_recovery(search, candidate_id);
    }
    ++search->passed_count;
    return search_expand(search, candidate_id);
}

static int ranked_contains(const size_t *ranked, size_t count,
                           size_t candidate)
{
    size_t index;
    for (index = 0u; index < count; ++index) {
        if (ranked[index] == candidate) {
            return 1;
        }
    }
    return 0;
}

size_t um_calibration_rank_candidates(
    const um_calibration_search *search,
    const float scores[UM_CALIBRATION_SEARCH_MAX_NODES], size_t *ranked,
    size_t ranked_capacity)
{
    size_t count = 0u;
    size_t nonbaseline_capacity;
    size_t best = SIZE_MAX;
    size_t index;
    size_t current;
    int have_baseline;
    if (search == NULL || scores == NULL || ranked == NULL ||
        ranked_capacity == 0u || search->node_count == 0u) {
        return 0u;
    }
    have_baseline = scores[0] >= 0.0f;
    nonbaseline_capacity = ranked_capacity - (have_baseline != 0 ? 1u : 0u);
    for (index = 1u; index < search->node_count; ++index) {
        if (scores[index] >= 0.0f &&
            (best == SIZE_MAX || scores[index] > scores[best])) {
            best = index;
        }
    }
    if (best != SIZE_MAX && count < nonbaseline_capacity) {
        ranked[count++] = best;
    }
    /*
     * Prefer the winning node's proven parent chain over unrelated modes
     * with nearly identical risk.  Each fallback removes exactly one tested
     * optimization, so verification failure degrades progressively instead
     * of jumping from four aggressive near-duplicates to the bootstrap.
     */
    current = best;
    while (current != SIZE_MAX && current != 0u &&
           count < nonbaseline_capacity) {
        current = search->nodes[current].parent;
        if (current != 0u && current < search->node_count &&
            scores[current] >= 0.0f &&
            ranked_contains(ranked, count, current) == 0) {
            ranked[count++] = current;
        }
    }
    while (count < nonbaseline_capacity) {
        size_t next = SIZE_MAX;
        for (index = 1u; index < search->node_count; ++index) {
            if (scores[index] >= 0.0f &&
                ranked_contains(ranked, count, index) == 0 &&
                (next == SIZE_MAX || scores[index] > scores[next])) {
                next = index;
            }
        }
        if (next == SIZE_MAX) {
            break;
        }
        ranked[count++] = next;
    }
    if (have_baseline != 0 && count < ranked_capacity) {
        ranked[count++] = 0u;
    }
    return count;
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
    uint8_t probe[UM_CALIBRATION_PROBE_BYTES];
    uint8_t verification_probe[128];
    um_calibration_search search;
    viable_candidate *viable = NULL;
    size_t viable_count = 0u;
    float candidate_scores[UM_CALIBRATION_SEARCH_MAX_NODES];
    size_t ranked[5u];
    size_t ranked_count;
    int search_status;

    if (channel == NULL || result == NULL) {
        return UM_ERR_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    {
        size_t index;
        for (index = 0u; index < UM_CALIBRATION_SEARCH_MAX_NODES; ++index) {
            candidate_scores[index] = -1.0f;
        }
    }
    result->config = um_modem_robust_config();
    fill_probe(probe, sizeof(probe), UINT32_C(0xc001d00d));
    search_status = um_calibration_search_init(
        &search, high_quality,
        high_quality != 0 ? CALIBRATION_HIGH_RATE_PAYLOAD_BYTES
                          : UM_CALIBRATION_PROBE_BYTES);
    if (search_status != UM_OK) {
        return search_status;
    }
    viable = (viable_candidate *)malloc(search.budget * sizeof(*viable));
    if (viable == NULL) {
        return UM_ERR_MEMORY;
    }

    while (1) {
        um_modem_config candidate;
        um_calibration_step step;
        size_t candidate_id;
        um_rx_metrics metrics;
        float duration = 0.0f;
        float payload_bps = 0.0f;
        float raw_bps;
        float quality;
        float score;
        int baseline;
        int robust_gate;
        int reliable;
        int status;
        char line[320];

        search_status = um_calibration_search_next(
            &search, &candidate_id, &candidate, &step);
        if (search_status < 0) {
            free(viable);
            return search_status;
        }
        if (search_status == 0) {
            break;
        }
        baseline = candidate_id == 0u;
        robust_gate = um_modem_config_uses_robust_gate(&candidate);
        ++result->candidates_tested;
        memset(&metrics, 0, sizeof(metrics));
        status = try_candidate(
            &candidate, channel, (uint32_t)candidate_id + 1u, probe,
            sizeof(probe), &duration, &payload_bps, &metrics);
        if (status == UM_ERR_MEMORY) {
            free(viable);
            return status;
        }
        reliable = status == UM_OK &&
                   (robust_gate != 0
                        ? um_modem_metrics_have_baseline_margin(&metrics)
                        : um_modem_metrics_have_margin(&candidate, &metrics)) !=
                       0;
        result->estimated_seconds += duration;
        raw_bps = um_calibration_payload_rate(
            &candidate, search.rate_payload_bytes);
        quality = reliable != 0
                      ? 1.0f /
                            (1.0f + 4.0f * metrics.evm_rms * metrics.evm_rms)
                      : 0.0f;
        score = raw_bps * quality;
        if (reliable != 0) {
            ++result->candidates_viable;
            viable[viable_count].config = candidate;
            viable[viable_count].candidate_id = candidate_id;
            candidate_scores[candidate_id] = score;
            ++viable_count;
        }
        if (logger != NULL) {
            (void)snprintf(
                line, sizeof(line),
                "calib %zu id=%zu step=%s%s band=%.0f-%.0fHz qam=%u "
                "cp=%.2fms fec=%s "
                "win=%u repeats=%u: %s snr=%.1fdB evm=%.3f "
                "payload=%.0fbps rate@%zuB=%.0fbps score=%.0f",
                result->candidates_tested, candidate_id,
                um_calibration_step_name(step),
                baseline != 0 ? " baseline" : "",
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
                search.rate_payload_bytes, raw_bps, score);
            logger(logger_context, line);
        }
        search_status = um_calibration_search_record(
            &search, candidate_id, reliable);
        if (search_status != UM_OK) {
            free(viable);
            return search_status;
        }
    }
    ranked_count = um_calibration_rank_candidates(
        &search, candidate_scores, ranked,
        sizeof(ranked) / sizeof(ranked[0]));
    if (ranked_count != 0u) {
        size_t rank;
        unsigned verification_trials = high_quality != 0 ? 4u : 3u;
        for (rank = 0u; rank < ranked_count; ++rank) {
            viable_candidate *ranked_candidate = NULL;
            um_modem_config guarded;
            size_t viable_index;
            unsigned trial;
            unsigned passes = 0u;
            float evm_sum = 0.0f;
            float payload_sum = 0.0f;
            for (viable_index = 0u; viable_index < viable_count;
                 ++viable_index) {
                if (viable[viable_index].candidate_id == ranked[rank]) {
                    ranked_candidate = &viable[viable_index];
                    break;
                }
            }
            if (ranked_candidate == NULL) {
                free(viable);
                return UM_ERR_CONFIG;
            }
            if (um_calibration_guard_config(&ranked_candidate->config,
                                            &guarded) != UM_OK) {
                free(viable);
                return UM_ERR_CONFIG;
            }
            ++result->candidates_verified;
            for (trial = 0u; trial < verification_trials; ++trial) {
                um_rx_metrics metrics;
                float duration = 0.0f;
                float payload_bps = 0.0f;
                uint32_t seed = UINT32_C(0x10000) +
                                (uint32_t)(ranked_candidate->candidate_id * 17u +
                                           trial);
                int status;
                int reliable;
                char line[256];
                fill_probe(verification_probe, sizeof(verification_probe),
                           seed ^ UINT32_C(0x6a09e667));
                memset(&metrics, 0, sizeof(metrics));
                status = try_candidate(
                    &guarded, channel, seed,
                    verification_probe,
                    sizeof(verification_probe), &duration, &payload_bps,
                    &metrics);
                reliable = status == UM_OK &&
                           (um_modem_config_uses_robust_gate(&guarded) != 0
                                ? um_modem_metrics_have_baseline_margin(
                                      &metrics)
                                : um_modem_metrics_have_margin(
                                      &guarded, &metrics)) !=
                               0;
                ++result->verification_frames;
                result->estimated_seconds += duration;
                if (logger != NULL) {
                    (void)snprintf(
                        line, sizeof(line),
                        "verify rank=%zu trial=%u/%u qam=%u fec=%s: %s "
                        "snr=%.1fdB evm=%.3f",
                        rank + 1u, trial + 1u, verification_trials,
                        1u << guarded.qam_bits,
                        fec_name(guarded.fec_rate),
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
            if (passes == verification_trials) {
                result->config = guarded;
                result->score =
                    um_calibration_payload_rate(&guarded,
                                                search.rate_payload_bytes) /
                    (1.0f + 4.0f * (evm_sum / (float)passes) *
                                    (evm_sum / (float)passes));
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
