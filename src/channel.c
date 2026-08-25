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
    config.secondary_echo_delay = 0u;
    config.secondary_echo_gain = 0.0f;
    config.lowpass_hz = 0.0f;
    config.lowpass_stages = 0u;
    config.interference_hz[0] = 0.0f;
    config.interference_hz[1] = 0.0f;
    config.interference_amplitude[0] = 0.0f;
    config.interference_amplitude[1] = 0.0f;
    config.sample_rate_offset_ppm = 0.0f;
    config.clip_level = 0.0f;
    config.dropout_start = 0u;
    config.dropout_length = 0u;
    config.random_seed = UINT32_C(0x12345678);
    return config;
}

um_channel_config um_channel_recorded_v2_config(unsigned direction)
{
    um_channel_config channel = um_channel_default_config();
    int reverse = direction != 0u;
    channel.leading_silence = reverse != 0 ? 1061u : 839u;
    channel.gain = reverse != 0 ? 0.0110f : 0.0120f;
    channel.noise_stddev = reverse != 0 ? 0.00115f : 0.00120f;
    channel.echo_delay = reverse != 0 ? 811u : 731u;
    channel.echo_gain = reverse != 0 ? 0.50f : 0.48f;
    channel.secondary_echo_delay = reverse != 0 ? 313u : 239u;
    channel.secondary_echo_gain = reverse != 0 ? 0.22f : 0.20f;
    channel.lowpass_hz = reverse != 0 ? 7800.0f : 8500.0f;
    channel.lowpass_stages = 2u;
    channel.interference_hz[0] = 117.0f;
    channel.interference_hz[1] = 347.0f;
    channel.interference_amplitude[0] = reverse != 0 ? 0.0080f : 0.0075f;
    channel.interference_amplitude[1] = reverse != 0 ? 0.0028f : 0.0025f;
    channel.sample_rate_offset_ppm = reverse != 0 ? -180.0f : 180.0f;
    channel.random_seed = reverse != 0 ? UINT32_C(0x734b2a19)
                                       : UINT32_C(0x19d4c37b);
    return channel;
}

int um_channel_apply(const float *input, size_t input_count,
                     const um_channel_config *config,
                     float **output, size_t *output_count)
{
    size_t echo_tail;
    size_t path_count;
    size_t count = 0u;
    float *path = NULL;
    float *result = NULL;
    size_t i;
    uint32_t random_state;
    float sample_rate_ratio;

    if ((input_count != 0u && input == NULL) || config == NULL ||
        output == NULL || output_count == NULL || config->gain < 0.0f ||
        config->noise_stddev < 0.0f || config->echo_gain < -1.0f ||
        config->echo_gain > 1.0f || config->secondary_echo_gain < -1.0f ||
        config->secondary_echo_gain > 1.0f || config->lowpass_hz < 0.0f ||
        config->lowpass_hz >= 0.5f * (float)UM_SAMPLE_RATE ||
        config->lowpass_stages > 4u ||
        config->interference_hz[0] < 0.0f ||
        config->interference_hz[1] < 0.0f ||
        config->interference_hz[0] >= 0.5f * (float)UM_SAMPLE_RATE ||
        config->interference_hz[1] >= 0.5f * (float)UM_SAMPLE_RATE ||
        config->interference_amplitude[0] < 0.0f ||
        config->interference_amplitude[1] < 0.0f ||
        config->sample_rate_offset_ppm <= -2000.0f ||
        config->sample_rate_offset_ppm >= 2000.0f ||
        config->clip_level < 0.0f) {
        return UM_ERR_ARGUMENT;
    }
    echo_tail = config->echo_gain != 0.0f ? config->echo_delay : 0u;
    if (config->secondary_echo_gain != 0.0f &&
        config->secondary_echo_delay > echo_tail) {
        echo_tail = config->secondary_echo_delay;
    }
    if (input_count > SIZE_MAX - config->leading_silence - echo_tail) {
        return UM_ERR_ARGUMENT;
    }
    path_count = config->leading_silence + input_count + echo_tail;
    path = (float *)calloc(path_count == 0u ? 1u : path_count,
                           sizeof(*path));
    if (path == NULL) {
        return UM_ERR_MEMORY;
    }

    for (i = 0u; i < input_count; ++i) {
        size_t direct = config->leading_silence + i;
        path[direct] += config->gain * input[i];
        if (config->echo_gain != 0.0f) {
            path[direct + config->echo_delay] +=
                config->gain * config->echo_gain * input[i];
        }
        if (config->secondary_echo_gain != 0.0f) {
            path[direct + config->secondary_echo_delay] +=
                config->gain * config->secondary_echo_gain * input[i];
        }
    }

    if (config->lowpass_hz > 0.0f && config->lowpass_stages != 0u) {
        float alpha = 1.0f -
                      expf(-2.0f * UM_PI * config->lowpass_hz /
                           (float)UM_SAMPLE_RATE);
        unsigned stage;
        for (stage = 0u; stage < config->lowpass_stages; ++stage) {
            float state = 0.0f;
            for (i = 0u; i < path_count; ++i) {
                state += alpha * (path[i] - state);
                path[i] = state;
            }
        }
    }

    sample_rate_ratio =
        1.0f + config->sample_rate_offset_ppm * 1.0e-6f;
    if (path_count != 0u) {
        double last = (double)(path_count - 1u) /
                      (double)sample_rate_ratio;
        if (last > (double)(SIZE_MAX - 1u)) {
            free(path);
            return UM_ERR_ARGUMENT;
        }
        count = (size_t)floor(last) + 1u;
    }
    result = (float *)calloc(count == 0u ? 1u : count, sizeof(*result));
    if (result == NULL) {
        free(path);
        return UM_ERR_MEMORY;
    }
    for (i = 0u; i < count; ++i) {
        double source = (double)i * (double)sample_rate_ratio;
        size_t first = (size_t)source;
        float fraction = (float)(source - (double)first);
        float first_sample = path[first];
        float second_sample = first + 1u < path_count
                                  ? path[first + 1u]
                                  : first_sample;
        result[i] = first_sample + (second_sample - first_sample) * fraction;
    }

    random_state = config->random_seed;
    for (i = 0u; i < count; ++i) {
        float sample = result[i];
        unsigned tone;
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
        for (tone = 0u; tone < 2u; ++tone) {
            if (config->interference_amplitude[tone] > 0.0f &&
                config->interference_hz[tone] > 0.0f) {
                sample += config->interference_amplitude[tone] *
                          sinf(2.0f * UM_PI *
                               config->interference_hz[tone] * (float)i /
                               (float)UM_SAMPLE_RATE);
            }
        }
        result[i] = sample;
    }

    free(path);
    *output = result;
    *output_count = count;
    return UM_OK;
}
