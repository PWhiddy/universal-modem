#include "um_internal.h"

#include <float.h>

unsigned um_qam_bits_per_symbol(unsigned qam_order)
{
    switch (qam_order) {
    case 4u:
        return 2u;
    case 16u:
        return 4u;
    case 64u:
        return 6u;
    default:
        return 0u;
    }
}

static unsigned gray_to_binary(unsigned gray)
{
    unsigned binary = gray;
    while (gray > 0u) {
        gray >>= 1u;
        binary ^= gray;
    }
    return binary;
}

static float axis_level(unsigned gray, unsigned axis_bits, float normalization)
{
    unsigned levels = 1u << axis_bits;
    unsigned binary = gray_to_binary(gray);
    int unscaled = (int)(2u * binary + 1u) - (int)levels;
    return (float)unscaled * normalization;
}

um_complex um_qam_map(const uint8_t *bits, unsigned bit_count)
{
    um_complex result = {0.0f, 0.0f};
    unsigned axis_bits;
    unsigned i_gray = 0u;
    unsigned q_gray = 0u;
    unsigned i;
    float normalization;

    if (bits == NULL || (bit_count != 2u && bit_count != 4u && bit_count != 6u)) {
        return result;
    }
    axis_bits = bit_count / 2u;
    for (i = 0u; i < axis_bits; ++i) {
        i_gray = (i_gray << 1u) | (bits[i] & 1u);
        q_gray = (q_gray << 1u) | (bits[axis_bits + i] & 1u);
    }
    normalization = 1.0f / sqrtf((2.0f / 3.0f) *
                                  (float)((1u << bit_count) - 1u));
    result.re = axis_level(i_gray, axis_bits, normalization);
    result.im = axis_level(q_gray, axis_bits, normalization);
    return result;
}

int um_qam_soft_demod(um_complex sample, unsigned bit_count, float *soft_bits)
{
    unsigned constellation_size;
    unsigned label;
    unsigned bit;
    float min_zero[6];
    float min_one[6];

    if (soft_bits == NULL ||
        (bit_count != 2u && bit_count != 4u && bit_count != 6u)) {
        return UM_ERR_ARGUMENT;
    }
    for (bit = 0u; bit < bit_count; ++bit) {
        min_zero[bit] = FLT_MAX;
        min_one[bit] = FLT_MAX;
    }

    constellation_size = 1u << bit_count;
    for (label = 0u; label < constellation_size; ++label) {
        uint8_t label_bits[6];
        um_complex point;
        float distance;
        for (bit = 0u; bit < bit_count; ++bit) {
            label_bits[bit] =
                (uint8_t)((label >> (bit_count - 1u - bit)) & 1u);
        }
        point = um_qam_map(label_bits, bit_count);
        distance = (sample.re - point.re) * (sample.re - point.re) +
                   (sample.im - point.im) * (sample.im - point.im);
        for (bit = 0u; bit < bit_count; ++bit) {
            float *minimum = label_bits[bit] != 0u ? &min_one[bit]
                                                   : &min_zero[bit];
            if (distance < *minimum) {
                *minimum = distance;
            }
        }
    }
    for (bit = 0u; bit < bit_count; ++bit) {
        soft_bits[bit] = min_zero[bit] - min_one[bit];
    }
    return UM_OK;
}
