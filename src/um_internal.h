#ifndef UM_INTERNAL_H
#define UM_INTERNAL_H

#include "um.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define UM_PI 3.14159265358979323846f
#define UM_SYNC_LEAD 128u
#define UM_SYNC_SAMPLES 1024u
#define UM_SYNC_GAP 2048u
#define UM_TRAINING_SYMBOLS 3u
#define UM_MAX_SYNC_SAMPLES 2048u
#define UM_MAX_TRAINING_SYMBOLS 4u
#define UM_MAX_SYMBOL_REPETITIONS 4u
#define UM_HEADER_BYTES 16u
#define UM_HEADER_BITS (UM_HEADER_BYTES * 8u)
#define UM_FEC_TAIL_BITS 6u
#define UM_MAX_PAYLOAD 65535u
#define UM_CALIBRATION_SEARCH_MAX_NODES 192u
#define UM_CALIBRATION_PROBE_BYTES 128u

typedef enum {
    UM_CALIB_STEP_BASELINE = 0,
    UM_CALIB_STEP_REPETITIONS,
    UM_CALIB_STEP_QAM,
    UM_CALIB_STEP_FEC,
    UM_CALIB_STEP_PREFIX,
    UM_CALIB_STEP_HIGH_BAND,
    UM_CALIB_STEP_LOW_BAND,
    UM_CALIB_STEP_SYNC,
    UM_CALIB_STEP_GAP,
    UM_CALIB_STEP_TRAINING,
    UM_CALIB_STEP_NARROW_BAND,
    UM_CALIB_STEP_WINDOW,
    UM_CALIB_STEP_DATA_DEFAULT,
    UM_CALIB_STEP_COUNT
} um_calibration_step;

typedef struct {
    um_modem_config config;
    float priority;
    size_t parent;
    um_calibration_step step;
    int tested;
    int recorded;
    int passed;
} um_calibration_search_node;

typedef struct {
    int high_quality;
    size_t budget;
    size_t node_count;
    size_t tested_count;
    size_t passed_count;
    um_calibration_search_node nodes[UM_CALIBRATION_SEARCH_MAX_NODES];
} um_calibration_search;

static inline um_complex um_cadd(um_complex a, um_complex b)
{
    um_complex value = {a.re + b.re, a.im + b.im};
    return value;
}

static inline um_complex um_csub(um_complex a, um_complex b)
{
    um_complex value = {a.re - b.re, a.im - b.im};
    return value;
}

static inline um_complex um_cmul(um_complex a, um_complex b)
{
    um_complex value = {a.re * b.re - a.im * b.im,
                        a.re * b.im + a.im * b.re};
    return value;
}

static inline um_complex um_cconj(um_complex value)
{
    um_complex result = {value.re, -value.im};
    return result;
}

static inline um_complex um_cscale(um_complex value, float scale)
{
    um_complex result = {value.re * scale, value.im * scale};
    return result;
}

static inline float um_cabs2(um_complex value)
{
    return value.re * value.re + value.im * value.im;
}

static inline um_complex um_cdiv(um_complex numerator, um_complex denominator)
{
    float magnitude = um_cabs2(denominator);
    um_complex result;
    if (magnitude < 1.0e-20f) {
        result.re = 0.0f;
        result.im = 0.0f;
        return result;
    }
    result.re = (numerator.re * denominator.re +
                 numerator.im * denominator.im) / magnitude;
    result.im = (numerator.im * denominator.re -
                 numerator.re * denominator.im) / magnitude;
    return result;
}

void um_bytes_to_bits(const uint8_t *bytes, size_t byte_count, uint8_t *bits);
void um_bits_to_bytes(const uint8_t *bits, size_t bit_count, uint8_t *bytes);
size_t um_gcd_size(size_t a, size_t b);
size_t um_interleave_stride(size_t count);

um_modem_config um_modem_robust_config(void);
um_channel_config um_channel_recorded_v2_config(unsigned direction);
int um_modem_metrics_have_baseline_margin(const um_rx_metrics *metrics);
int um_modem_metrics_have_margin(const um_modem_config *config,
                                 const um_rx_metrics *metrics);
size_t um_calibration_search_budget(int high_quality);
int um_calibration_search_init(um_calibration_search *search,
                               int high_quality);
int um_calibration_search_next(um_calibration_search *search,
                               size_t *candidate_id,
                               um_modem_config *config,
                               um_calibration_step *step);
int um_calibration_search_record(um_calibration_search *search,
                                 size_t candidate_id, int passed);
void um_calibration_search_step_results(const um_calibration_search *search,
                                        um_calibration_step step,
                                        unsigned *attempts,
                                        unsigned *passes);
size_t um_calibration_rank_candidates(
    const um_calibration_search *search,
    const float scores[UM_CALIBRATION_SEARCH_MAX_NODES], size_t *ranked,
    size_t ranked_capacity);
const char *um_calibration_step_name(um_calibration_step step);
float um_calibration_payload_rate(const um_modem_config *config,
                                  size_t payload_bytes);

#endif
