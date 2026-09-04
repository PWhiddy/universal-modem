#include "um.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int tests_run = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                               \
            ++failures;                                                        \
            return;                                                            \
        }                                                                      \
    } while (0)

static void fill_payload(uint8_t *payload, size_t length, uint32_t seed)
{
    size_t i;
    for (i = 0u; i < length; ++i) {
        seed ^= seed << 13u;
        seed ^= seed >> 17u;
        seed ^= seed << 5u;
        payload[i] = (uint8_t)(seed >> 24u);
    }
}

static int light_round_trip(const um_light_channel_config *config,
                            const uint8_t *modules, const uint8_t *expected,
                            size_t expected_length,
                            um_light_rx_metrics *metrics)
{
    uint8_t decoded[UM_LIGHT_MAX_PAYLOAD];
    uint8_t *pixels = NULL;
    size_t pixel_count = 0u;
    size_t decoded_length = 0u;
    uint8_t type = 0u;
    uint32_t session = 0u;
    uint32_t sequence = 0u;
    int status;

    status = um_light_render_frame(
        modules, UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE, config, &pixels,
        &pixel_count);
    if (status != UM_OK) {
        return status;
    }
    if (pixel_count != config->image_width * config->image_height) {
        free(pixels);
        return UM_ERR_CONFIG;
    }
    status = um_light_decode_frame(
        pixels, config->image_width, config->image_height,
        config->image_width, &type, &session, &sequence, decoded,
        sizeof(decoded), &decoded_length, metrics);
    free(pixels);
    if (status != UM_OK) {
        return status;
    }
    if (type != 7u || session != UINT32_C(0x1234abcd) ||
        sequence != UINT32_C(0x10203040) ||
        decoded_length != expected_length ||
        memcmp(decoded, expected, expected_length) != 0) {
        return UM_ERR_CRC;
    }
    return UM_OK;
}

static void test_light_clean_frame(void)
{
    uint8_t payload[UM_LIGHT_MAX_PAYLOAD];
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    um_light_channel_config config = um_light_channel_default_config();
    um_light_rx_metrics metrics;
    size_t row;
    size_t column;
    ++tests_run;

    fill_payload(payload, sizeof(payload), UINT32_C(0x77118a3f));
    CHECK(um_light_encode_frame(7u, UINT32_C(0x1234abcd),
                                UINT32_C(0x10203040), payload,
                                sizeof(payload), modules,
                                sizeof(modules)) == UM_OK);
    /* Pin the complete module layout, not merely this encoder to its decoder. */
    CHECK(um_crc32(modules, sizeof(modules)) == UINT32_C(0x06dee956));
    for (row = 0u; row < UM_LIGHT_GRID_SIZE; ++row) {
        for (column = 0u; column < UM_LIGHT_GRID_SIZE; ++column) {
            if (row < 2u || row >= UM_LIGHT_GRID_SIZE - 2u || column < 2u ||
                column >= UM_LIGHT_GRID_SIZE - 2u) {
                CHECK(modules[row * UM_LIGHT_GRID_SIZE + column] == 1u);
            } else if (row == 2u || row == UM_LIGHT_GRID_SIZE - 3u ||
                       column == 2u ||
                       column == UM_LIGHT_GRID_SIZE - 3u) {
                CHECK(modules[row * UM_LIGHT_GRID_SIZE + column] == 0u);
            }
        }
    }

    config.image_width = 480u;
    config.image_height = 360u;
    config.corners[0].x = 100.0f;
    config.corners[0].y = 40.0f;
    config.corners[1].x = 380.0f;
    config.corners[1].y = 40.0f;
    config.corners[2].x = 380.0f;
    config.corners[2].y = 320.0f;
    config.corners[3].x = 100.0f;
    config.corners[3].y = 320.0f;
    config.noise_stddev = 0.0f;
    config.blur_radius = 0u;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &metrics) == UM_OK);
    CHECK(metrics.contrast > 0.80f);
    CHECK(metrics.corrected_bit_fraction < 0.001f);
    CHECK(metrics.image_coverage > 0.40f);
}

static void test_light_perspective_and_noise(void)
{
    uint8_t payload[73];
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    um_light_channel_config config = um_light_channel_default_config();
    um_light_rx_metrics metrics;
    ++tests_run;

    fill_payload(payload, sizeof(payload), UINT32_C(0xf03ba492));
    CHECK(um_light_encode_frame(7u, UINT32_C(0x1234abcd),
                                UINT32_C(0x10203040), payload,
                                sizeof(payload), modules,
                                sizeof(modules)) == UM_OK);
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &metrics) == UM_OK);
    CHECK(metrics.contrast > 0.60f);
    CHECK(metrics.image_coverage > 0.30f);
    CHECK(metrics.image_coverage < 0.55f);

    config.black_level = 0.22f;
    config.white_level = 0.76f;
    config.noise_stddev = 0.04f;
    config.blur_radius = 2u;
    config.corners[0].x = 252.0f;
    config.corners[0].y = 38.0f;
    config.corners[1].x = 558.0f;
    config.corners[1].y = 188.0f;
    config.corners[2].x = 390.0f;
    config.corners[2].y = 448.0f;
    config.corners[3].x = 92.0f;
    config.corners[3].y = 276.0f;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &metrics) == UM_OK);
    CHECK(metrics.contrast > 0.40f);
}

static void test_light_scale_envelope(void)
{
    uint8_t payload[UM_LIGHT_MAX_PAYLOAD];
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    um_light_channel_config config = um_light_channel_default_config();
    um_light_rx_metrics metrics;
    ++tests_run;

    fill_payload(payload, sizeof(payload), UINT32_C(0xab81c76d));
    CHECK(um_light_encode_frame(7u, UINT32_C(0x1234abcd),
                                UINT32_C(0x10203040), payload,
                                sizeof(payload), modules,
                                sizeof(modules)) == UM_OK);
    config.image_width = 800u;
    config.image_height = 600u;
    config.corners[0].x = 280.0f;
    config.corners[0].y = 160.0f;
    config.corners[1].x = 505.0f;
    config.corners[1].y = 180.0f;
    config.corners[2].x = 520.0f;
    config.corners[2].y = 390.0f;
    config.corners[3].x = 255.0f;
    config.corners[3].y = 410.0f;
    config.noise_stddev = 0.025f;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &metrics) == UM_OK);
    CHECK(metrics.image_coverage > 0.09f);
    CHECK(metrics.image_coverage < 0.14f);

    config.corners[0].x = 30.0f;
    config.corners[0].y = 20.0f;
    config.corners[1].x = 760.0f;
    config.corners[1].y = 55.0f;
    config.corners[2].x = 735.0f;
    config.corners[2].y = 570.0f;
    config.corners[3].x = 55.0f;
    config.corners[3].y = 585.0f;
    config.noise_stddev = 0.06f;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &metrics) == UM_OK);
    CHECK(metrics.image_coverage > 0.70f);
    CHECK(metrics.image_coverage < 0.90f);
}

static void test_light_rotation_and_occlusion(void)
{
    uint8_t payload[68];
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t rotated[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t reflected[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    um_light_channel_config config = um_light_channel_default_config();
    um_light_rx_metrics metrics;
    size_t row;
    size_t column;
    ++tests_run;

    fill_payload(payload, sizeof(payload), UINT32_C(0x11b34e09));
    CHECK(um_light_encode_frame(7u, UINT32_C(0x1234abcd),
                                UINT32_C(0x10203040), payload,
                                sizeof(payload), modules,
                                sizeof(modules)) == UM_OK);
    for (row = 0u; row < UM_LIGHT_GRID_SIZE; ++row) {
        for (column = 0u; column < UM_LIGHT_GRID_SIZE; ++column) {
            rotated[column * UM_LIGHT_GRID_SIZE +
                    (UM_LIGHT_GRID_SIZE - 1u - row)] =
                modules[row * UM_LIGHT_GRID_SIZE + column];
        }
    }
    config.occlusion_x = 0.48f;
    config.occlusion_y = 0.46f;
    config.occlusion_width = 0.018f;
    config.occlusion_height = 0.045f;
    CHECK(light_round_trip(&config, rotated, payload, sizeof(payload),
                           &metrics) == UM_OK);
    CHECK(metrics.orientation != 0u);
    CHECK(metrics.corrected_bit_fraction > 0.0f);

    for (row = 0u; row < UM_LIGHT_GRID_SIZE; ++row) {
        for (column = 0u; column < UM_LIGHT_GRID_SIZE; ++column) {
            reflected[row * UM_LIGHT_GRID_SIZE +
                      (UM_LIGHT_GRID_SIZE - 1u - column)] =
                modules[row * UM_LIGHT_GRID_SIZE + column];
        }
    }
    config.occlusion_width = 0.0f;
    config.occlusion_height = 0.0f;
    CHECK(light_round_trip(&config, reflected, payload, sizeof(payload),
                           &metrics) == UM_OK);
    CHECK(metrics.orientation >= 4u);
}

static void test_light_rejects_damage_and_bad_arguments(void)
{
    uint8_t payload[UM_LIGHT_MAX_PAYLOAD + 1u];
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t decoded[UM_LIGHT_MAX_PAYLOAD];
    uint8_t *pixels = NULL;
    um_light_channel_config config = um_light_channel_default_config();
    um_light_rx_metrics metrics;
    size_t pixel_count = 0u;
    size_t decoded_length = 0u;
    uint8_t type = 0u;
    uint32_t session = 0u;
    uint32_t sequence = 0u;
    size_t row;
    size_t column;
    int status;
    ++tests_run;

    fill_payload(payload, sizeof(payload), UINT32_C(0xc191ed82));
    CHECK(um_light_encode_frame(7u, 1u, 1u, payload, sizeof(payload),
                                modules, sizeof(modules)) ==
          UM_ERR_CAPACITY);
    CHECK(um_light_encode_frame(7u, UINT32_C(0x1234abcd),
                                UINT32_C(0x10203040), payload,
                                UM_LIGHT_MAX_PAYLOAD, modules,
                                sizeof(modules)) == UM_OK);
    CHECK(um_light_render_frame(
              modules, UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE, &config,
              &pixels, &pixel_count) == UM_OK);
    CHECK(um_light_decode_frame(
              pixels, config.image_width, config.image_height,
              config.image_width, &type, &session, &sequence, decoded, 1u,
              &decoded_length, &metrics) == UM_ERR_CAPACITY);
    CHECK(decoded_length == UM_LIGHT_MAX_PAYLOAD);
    free(pixels);
    pixels = NULL;
    for (row = 10u; row < 26u; ++row) {
        for (column = 8u; column < 28u; ++column) {
            modules[row * UM_LIGHT_GRID_SIZE + column] ^= 1u;
        }
    }
    CHECK(um_light_render_frame(
              modules, UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE, &config,
              &pixels, &pixel_count) == UM_OK);
    status = um_light_decode_frame(
        pixels, config.image_width, config.image_height, config.image_width,
        &type, &session, &sequence, decoded, sizeof(decoded), &decoded_length,
        &metrics);
    CHECK(status == UM_ERR_CRC || status == UM_ERR_SYNC);
    free(pixels);

    config.black_level = config.white_level;
    CHECK(um_light_render_frame(
              modules, UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE, &config,
              &pixels, &pixel_count) == UM_ERR_CONFIG);
}

static void test_light_cluttered_dark_scene(void)
{
    uint8_t payload[34];
    uint8_t decoded[UM_LIGHT_MAX_PAYLOAD];
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t *pixels = NULL;
    um_light_channel_config config = um_light_channel_default_config();
    um_light_rx_metrics metrics;
    size_t pixel_count = 0u;
    size_t decoded_length = 0u;
    uint8_t type = 0u;
    uint32_t session = 0u;
    uint32_t sequence = 0u;
    size_t x;
    size_t y;
    ++tests_run;

    fill_payload(payload, sizeof(payload), UINT32_C(0x94a71c23));
    CHECK(um_light_encode_frame(0x70u, UINT32_C(0x554d5649), 271u,
                                payload, sizeof(payload), modules,
                                sizeof(modules)) == UM_OK);
    config.image_width = 640u;
    config.image_height = 480u;
    config.corners[0] = (um_light_point){180.0f, 100.0f};
    config.corners[1] = (um_light_point){460.0f, 85.0f};
    config.corners[2] = (um_light_point){475.0f, 365.0f};
    config.corners[3] = (um_light_point){165.0f, 380.0f};
    config.noise_stddev = 0.01f;
    config.blur_radius = 1u;
    CHECK(um_light_render_frame(modules, sizeof(modules), &config, &pixels,
                                &pixel_count) == UM_OK);

    /* A real camera sees dark bezels, furniture, and the rest of the room.
     * This boundary-connected region is intentionally much larger than the
     * locator, matching the recorded webcam failure that this test guards. */
    for (y = 0u; y < config.image_height; ++y) {
        for (x = 0u; x < config.image_width; ++x) {
            if (x < 95u || x >= 545u || y < 35u || y >= 435u) {
                pixels[y * config.image_width + x] = 20u;
            }
        }
    }
    CHECK(um_light_decode_frame(
              pixels, config.image_width, config.image_height,
              config.image_width, &type, &session, &sequence, decoded,
              sizeof(decoded), &decoded_length, &metrics) == UM_OK);
    CHECK(type == 0x70u);
    CHECK(session == UINT32_C(0x554d5649));
    CHECK(sequence == 271u);
    CHECK(decoded_length == sizeof(payload));
    CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
    free(pixels);
}

static void test_light_channel_failure_boundaries(void)
{
    uint8_t payload[UM_LIGHT_MAX_PAYLOAD];
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    um_light_channel_config config = um_light_channel_default_config();
    size_t first_failure = SIZE_MAX;
    size_t level;
    ++tests_run;

    fill_payload(payload, sizeof(payload), UINT32_C(0x45b81d03));
    CHECK(um_light_encode_frame(7u, UINT32_C(0x1234abcd),
                                UINT32_C(0x10203040), payload,
                                sizeof(payload), modules,
                                sizeof(modules)) == UM_OK);

    /* Sweep through the decoder cliff instead of testing only hand-picked
     * successful channels.  With a fixed image and noise seed, every level
     * through 0.55 decodes and 0.60 is the first rejected frame. */
    for (level = 0u; level <= 12u; ++level) {
        um_light_rx_metrics metrics;
        int status;
        config.noise_stddev = 0.05f * (float)level;
        status = light_round_trip(&config, modules, payload,
                                  sizeof(payload), &metrics);
        if (status != UM_OK && first_failure == SIZE_MAX) {
            first_failure = level;
        }
        if (first_failure != SIZE_MAX) {
            CHECK(status != UM_OK);
        }
    }
    CHECK(first_failure == 12u);

    config = um_light_channel_default_config();
    config.noise_stddev = 0.0f;
    config.blur_radius = 2u;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &(um_light_rx_metrics){0}) == UM_OK);
    config.blur_radius = 16u;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &(um_light_rx_metrics){0}) != UM_OK);

    config = um_light_channel_default_config();
    config.noise_stddev = 0.0f;
    config.corners[0].x = 307.0f;
    config.corners[0].y = 227.0f;
    config.corners[1].x = 332.0f;
    config.corners[1].y = 227.0f;
    config.corners[2].x = 332.0f;
    config.corners[2].y = 252.0f;
    config.corners[3].x = 307.0f;
    config.corners[3].y = 252.0f;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &(um_light_rx_metrics){0}) != UM_OK);

    config = um_light_channel_default_config();
    config.noise_stddev = 0.0f;
    config.occlusion_x = 0.46f;
    config.occlusion_y = 0.46f;
    config.occlusion_width = 0.025f;
    config.occlusion_height = 0.050f;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &(um_light_rx_metrics){0}) == UM_OK);
    config.occlusion_x = 0.30f;
    config.occlusion_y = 0.30f;
    config.occlusion_width = 0.40f;
    config.occlusion_height = 0.40f;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &(um_light_rx_metrics){0}) != UM_OK);

    config = um_light_channel_default_config();
    config.black_level = 0.23f;
    config.white_level = 0.77f;
    config.noise_stddev = 0.04f;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &(um_light_rx_metrics){0}) == UM_OK);
    config.black_level = 0.48f;
    config.white_level = 0.52f;
    CHECK(light_round_trip(&config, modules, payload, sizeof(payload),
                           &(um_light_rx_metrics){0}) != UM_OK);
}

int main(void)
{
    test_light_clean_frame();
    test_light_perspective_and_noise();
    test_light_scale_envelope();
    test_light_rotation_and_occlusion();
    test_light_rejects_damage_and_bad_arguments();
    test_light_cluttered_dark_scene();
    test_light_channel_failure_boundaries();

    if (failures != 0) {
        fprintf(stderr, "%d of %d light tests failed\n", failures,
                tests_run);
        return 1;
    }
    printf("all %d light codec tests passed\n", tests_run);
    return 0;
}
