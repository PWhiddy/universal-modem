#include "um_internal.h"

#include <float.h>
#include <stdlib.h>

typedef struct {
    const uint8_t *bits;
    size_t length;
} puncture_pattern;

static const uint8_t pattern_1_2[] = {1u, 1u};
static const uint8_t pattern_2_3[] = {1u, 1u, 1u, 0u};
static const uint8_t pattern_3_4[] = {1u, 1u, 1u, 0u, 0u, 1u};

static int get_pattern(um_fec_rate rate, puncture_pattern *pattern)
{
    switch (rate) {
    case UM_FEC_RATE_1_2:
        pattern->bits = pattern_1_2;
        pattern->length = sizeof(pattern_1_2);
        return UM_OK;
    case UM_FEC_RATE_2_3:
        pattern->bits = pattern_2_3;
        pattern->length = sizeof(pattern_2_3);
        return UM_OK;
    case UM_FEC_RATE_3_4:
        pattern->bits = pattern_3_4;
        pattern->length = sizeof(pattern_3_4);
        return UM_OK;
    default:
        return UM_ERR_ARGUMENT;
    }
}

static uint8_t parity7(uint8_t value)
{
    value ^= (uint8_t)(value >> 4u);
    value ^= (uint8_t)(value >> 2u);
    value ^= (uint8_t)(value >> 1u);
    return (uint8_t)(value & 1u);
}

size_t um_fec_encoded_bits(size_t data_bits, um_fec_rate rate)
{
    puncture_pattern pattern;
    size_t full_count;
    size_t groups;
    size_t remainder;
    size_t ones = 0u;
    size_t i;

    if (get_pattern(rate, &pattern) != UM_OK ||
        data_bits > (SIZE_MAX / 2u) - UM_FEC_TAIL_BITS) {
        return 0u;
    }
    full_count = 2u * (data_bits + UM_FEC_TAIL_BITS);
    for (i = 0; i < pattern.length; ++i) {
        ones += pattern.bits[i];
    }
    groups = full_count / pattern.length;
    remainder = full_count % pattern.length;
    ones *= groups;
    for (i = 0; i < remainder; ++i) {
        ones += pattern.bits[i];
    }
    return ones;
}

int um_fec_encode(const uint8_t *bits, size_t data_bits, um_fec_rate rate,
                  uint8_t *encoded, size_t capacity, size_t *encoded_bits)
{
    puncture_pattern pattern;
    size_t required;
    size_t step;
    size_t serial = 0u;
    size_t output = 0u;
    uint8_t state = 0u;

    if ((data_bits != 0u && bits == NULL) || encoded == NULL ||
        encoded_bits == NULL || get_pattern(rate, &pattern) != UM_OK) {
        return UM_ERR_ARGUMENT;
    }
    required = um_fec_encoded_bits(data_bits, rate);
    if (required == 0u || capacity < required) {
        return UM_ERR_CAPACITY;
    }

    for (step = 0u; step < data_bits + UM_FEC_TAIL_BITS; ++step) {
        uint8_t input = step < data_bits ? (uint8_t)(bits[step] & 1u) : 0u;
        uint8_t reg = (uint8_t)((state << 1u) | input);
        uint8_t pair[2];
        unsigned j;
        pair[0] = parity7((uint8_t)(reg & UINT8_C(0x79)));
        pair[1] = parity7((uint8_t)(reg & UINT8_C(0x5b)));
        state = (uint8_t)(reg & UINT8_C(0x3f));

        for (j = 0u; j < 2u; ++j, ++serial) {
            if (pattern.bits[serial % pattern.length] != 0u) {
                encoded[output++] = pair[j];
            }
        }
    }
    *encoded_bits = output;
    return UM_OK;
}

int um_fec_decode(const float *soft_bits, size_t soft_count, size_t data_bits,
                  um_fec_rate rate, uint8_t *decoded, size_t capacity)
{
    puncture_pattern pattern;
    size_t full_count;
    size_t expected_count;
    size_t steps;
    float *depunctured = NULL;
    uint8_t *predecessors = NULL;
    float metrics[64];
    float next_metrics[64];
    size_t serial;
    size_t received = 0u;
    size_t step;
    unsigned state;
    int status = UM_OK;

    if ((soft_count != 0u && soft_bits == NULL) || decoded == NULL ||
        capacity < data_bits || get_pattern(rate, &pattern) != UM_OK) {
        return UM_ERR_ARGUMENT;
    }
    expected_count = um_fec_encoded_bits(data_bits, rate);
    if (expected_count == 0u || soft_count != expected_count) {
        return UM_ERR_ARGUMENT;
    }
    steps = data_bits + UM_FEC_TAIL_BITS;
    full_count = steps * 2u;
    depunctured = (float *)calloc(full_count, sizeof(*depunctured));
    predecessors = (uint8_t *)malloc(steps * 64u * sizeof(*predecessors));
    if (depunctured == NULL || predecessors == NULL) {
        status = UM_ERR_MEMORY;
        goto done;
    }

    for (serial = 0u; serial < full_count; ++serial) {
        if (pattern.bits[serial % pattern.length] != 0u) {
            depunctured[serial] = soft_bits[received++];
        }
    }

    for (state = 0u; state < 64u; ++state) {
        metrics[state] = state == 0u ? 0.0f : -FLT_MAX / 4.0f;
    }

    for (step = 0u; step < steps; ++step) {
        float first = depunctured[step * 2u];
        float second = depunctured[step * 2u + 1u];
        unsigned previous;
        for (state = 0u; state < 64u; ++state) {
            next_metrics[state] = -FLT_MAX / 4.0f;
        }
        for (previous = 0u; previous < 64u; ++previous) {
            unsigned input;
            for (input = 0u; input < 2u; ++input) {
                uint8_t reg = (uint8_t)((previous << 1u) | input);
                uint8_t out0 = parity7((uint8_t)(reg & UINT8_C(0x79)));
                uint8_t out1 = parity7((uint8_t)(reg & UINT8_C(0x5b)));
                unsigned next = reg & 0x3fu;
                float score = metrics[previous] +
                              (out0 != 0u ? first : -first) +
                              (out1 != 0u ? second : -second);
                if (score > next_metrics[next]) {
                    next_metrics[next] = score;
                    predecessors[step * 64u + next] = (uint8_t)previous;
                }
            }
        }
        for (state = 0u; state < 64u; ++state) {
            metrics[state] = next_metrics[state];
        }
    }

    state = 0u;
    for (step = steps; step-- > 0u;) {
        if (step < data_bits) {
            decoded[step] = (uint8_t)(state & 1u);
        }
        state = predecessors[step * 64u + state];
    }

done:
    free(predecessors);
    free(depunctured);
    return status;
}
