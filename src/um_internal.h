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

#endif
