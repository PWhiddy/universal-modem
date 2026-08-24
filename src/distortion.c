#include "um_internal.h"

#include <string.h>

typedef struct {
    const char *name;
    unsigned mask;
    unsigned leading[2];
    float gain[2];
    float noise[2];
    unsigned echo_delay[2];
    float echo_gain[2];
    float clip[2];
    unsigned dropout_offset;
    unsigned dropout_length;
    float blackout_after;
    float blackout_duration;
} profile_definition;

static const profile_definition profiles[] = {
    {
        "clean", 0u,
        {0u, 0u}, {1.0f, 1.0f}, {0.0f, 0.0f},
        {0u, 0u}, {0.0f, 0.0f}, {0.0f, 0.0f},
        0u, 0u, 0.0f, 0.0f
    },
    {
        "level-and-delay", UM_IMPAIR_LEVEL | UM_IMPAIR_DELAY,
        {137u, 211u}, {0.42f, 1.55f}, {0.0f, 0.0f},
        {0u, 0u}, {0.0f, 0.0f}, {0.0f, 0.0f},
        0u, 0u, 0.0f, 0.0f
    },
    {
        "mild-multipath",
        UM_IMPAIR_LEVEL | UM_IMPAIR_DELAY | UM_IMPAIR_ECHO,
        {173u, 251u}, {0.28f, 1.75f}, {0.0f, 0.0f},
        {11u, 17u}, {0.22f, 0.28f}, {0.0f, 0.0f},
        0u, 0u, 0.0f, 0.0f
    },
    {
        "noisy-multipath",
        UM_IMPAIR_LEVEL | UM_IMPAIR_DELAY | UM_IMPAIR_ECHO |
            UM_IMPAIR_NOISE,
        {211u, 293u}, {0.25f, 1.65f}, {0.0006f, 0.0008f},
        {13u, 21u}, {0.24f, 0.29f}, {0.0f, 0.0f},
        0u, 0u, 0.0f, 0.0f
    },
    {
        "nonlinear-cutout-and-outage", UM_IMPAIR_ALL,
        {257u, 337u}, {0.19f, 1.95f}, {0.0009f, 0.0013f},
        {15u, 23u}, {0.29f, 0.34f}, {0.10f, 0.70f},
        700u, 2u, 0.30f, 2.20f
    },
    {
        "strong-combined", UM_IMPAIR_ALL,
        {331u, 419u}, {0.070f, 2.80f}, {0.0035f, 0.0055f},
        {31u, 43u}, {0.50f, 0.57f}, {0.026f, 0.28f},
        700u, 88u, 0.22f, 2.20f
    },
    {
        "extreme-combined", UM_IMPAIR_ALL,
        {443u, 557u}, {0.035f, 3.50f}, {0.0075f, 0.0110f},
        {55u, 73u}, {0.66f, 0.72f}, {0.012f, 0.12f},
        700u, 220u, 0.16f, 3.20f
    },
    {
        "no-usable-signal", UM_IMPAIR_ALL,
        {601u, 733u}, {0.0f, 0.0f}, {0.0f, 0.0f},
        {97u, 113u}, {0.85f, 0.85f}, {0.001f, 0.001f},
        0u, 4096u, 0.0f, 10.0f
    }
};

size_t um_distortion_profile_count(void)
{
    return sizeof(profiles) / sizeof(profiles[0]);
}

static void build_channel(const profile_definition *definition,
                          unsigned direction, size_t level,
                          um_channel_config *channel)
{
    *channel = um_channel_default_config();
    channel->leading_silence = definition->leading[direction];
    channel->gain = definition->gain[direction];
    channel->noise_stddev = definition->noise[direction];
    channel->echo_delay = definition->echo_delay[direction];
    channel->echo_gain = definition->echo_gain[direction];
    channel->clip_level = definition->clip[direction];
    channel->dropout_length = definition->dropout_length;
    if (definition->dropout_length != 0u) {
        channel->dropout_start = channel->leading_silence + UM_SYNC_LEAD +
                                 UM_SYNC_SAMPLES + UM_SYNC_GAP +
                                 definition->dropout_offset;
    }
    channel->random_seed =
        UINT32_C(0x51f15eed) ^ (uint32_t)(level * 2u + direction) *
                                  UINT32_C(0x9e3779b9);
}

int um_distortion_profile_get(size_t level, um_distortion_profile *profile)
{
    const profile_definition *definition;
    if (profile == NULL || level >= um_distortion_profile_count()) {
        return UM_ERR_ARGUMENT;
    }
    definition = &profiles[level];
    memset(profile, 0, sizeof(*profile));
    profile->level = level;
    profile->name = definition->name;
    profile->impairment_mask = definition->mask;
    build_channel(definition, 0u, level, &profile->client_to_gateway);
    build_channel(definition, 1u, level, &profile->gateway_to_client);
    profile->blackout_after_data_seconds = definition->blackout_after;
    profile->blackout_duration_seconds = definition->blackout_duration;
    return UM_OK;
}
