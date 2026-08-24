#include "um_internal.h"

#include <stdlib.h>

static uint32_t random_u32(uint32_t *state)
{
    uint32_t value = *state;
    if (value == 0u) {
        value = UINT32_C(0x6d2b79f5);
    }
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static float uniform_open(uint32_t *state)
{
    return ((float)(random_u32(state) >> 8u) + 1.0f) / 16777217.0f;
}

static float gaussian(uint32_t *state)
{
    float first = uniform_open(state);
    float second = uniform_open(state);
    return sqrtf(-2.0f * logf(first)) * cosf(2.0f * UM_PI * second);
}

um_channel_config um_channel_default_config(void)
{
    um_channel_config config;
    config.leading_silence = 0u;
    config.gain = 1.0f;
    config.noise_stddev = 0.0f;
    config.echo_delay = 0u;
    config.echo_gain = 0.0f;
    config.clip_level = 0.0f;
    config.dropout_start = 0u;
    config.dropout_length = 0u;
    config.random_seed = UINT32_C(0x12345678);
    return config;
}

int um_channel_apply(const float *input, size_t input_count,
                     const um_channel_config *config,
                     float **output, size_t *output_count)
{
    size_t echo_tail;
    size_t count;
    float *result;
    size_t i;
    uint32_t random_state;

    if ((input_count != 0u && input == NULL) || config == NULL ||
        output == NULL || output_count == NULL || config->gain < 0.0f ||
        config->noise_stddev < 0.0f || config->echo_gain < -1.0f ||
        config->echo_gain > 1.0f || config->clip_level < 0.0f) {
        return UM_ERR_ARGUMENT;
    }
    echo_tail = config->echo_gain != 0.0f ? config->echo_delay : 0u;
    if (input_count > SIZE_MAX - config->leading_silence - echo_tail) {
        return UM_ERR_ARGUMENT;
    }
    count = config->leading_silence + input_count + echo_tail;
    result = (float *)calloc(count == 0u ? 1u : count, sizeof(*result));
    if (result == NULL) {
        return UM_ERR_MEMORY;
    }

    for (i = 0u; i < input_count; ++i) {
        size_t direct = config->leading_silence + i;
        result[direct] += config->gain * input[i];
        if (config->echo_gain != 0.0f) {
            result[direct + config->echo_delay] +=
                config->gain * config->echo_gain * input[i];
        }
    }

    random_state = config->random_seed;
    for (i = 0u; i < count; ++i) {
        float sample = result[i];
        if (config->dropout_length != 0u && i >= config->dropout_start &&
            i - config->dropout_start < config->dropout_length) {
            sample = 0.0f;
        }
        if (config->clip_level > 0.0f) {
            if (sample > config->clip_level) {
                sample = config->clip_level;
            } else if (sample < -config->clip_level) {
                sample = -config->clip_level;
            }
        }
        if (config->noise_stddev > 0.0f) {
            sample += config->noise_stddev * gaussian(&random_state);
        }
        result[i] = sample;
    }

    *output = result;
    *output_count = count;
    return UM_OK;
}
