#include "../src/light_video.h"

#include <stdio.h>
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

static void count_log(void *context, const char *message)
{
    size_t *count = (size_t *)context;
    if (message != NULL && message[0] != '\0') {
        ++*count;
    }
}

static void test_default_video_configuration(void)
{
    um_light_video_config config = um_light_video_default_config();
    ++tests_run;

    CHECK(strcmp(config.camera_device, "default") == 0);
    CHECK(config.camera_width == 640u);
    CHECK(config.camera_height == 480u);
    CHECK(config.frames_per_second == 15u);
    CHECK(config.window_size == 720u);
}

static void test_module_raster_has_quiet_zone_and_exact_cells(void)
{
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t pixels[108u * 104u];
    size_t y;
    ++tests_run;

    memset(modules, 0, sizeof(modules));
    modules[0] = 1u;
    modules[sizeof(modules) - 1u] = 1u;
    memset(pixels, 0x7bu, sizeof(pixels));
    CHECK(um_light_rasterize_modules(modules, sizeof(modules), pixels,
                                     104u, 104u, 108u,
                                     sizeof(pixels)) == UM_OK);
    CHECK(pixels[0] == 255u);
    CHECK(pixels[3u * 108u + 4u] == 255u);
    CHECK(pixels[4u * 108u + 3u] == 255u);
    CHECK(pixels[4u * 108u + 4u] == 0u);
    CHECK(pixels[5u * 108u + 5u] == 0u);
    CHECK(pixels[4u * 108u + 6u] == 255u);
    CHECK(pixels[98u * 108u + 98u] == 0u);
    CHECK(pixels[99u * 108u + 99u] == 0u);
    CHECK(pixels[100u * 108u + 100u] == 255u);
    for (y = 0u; y < 104u; ++y) {
        CHECK(pixels[y * 108u + 104u] == 0x7bu);
    }
}

static void test_rectangular_window_centers_square_symbol(void)
{
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t pixels[156u * 104u];
    ++tests_run;

    memset(modules, 0, sizeof(modules));
    modules[0] = 1u;
    CHECK(um_light_rasterize_modules(modules, sizeof(modules), pixels,
                                     156u, 104u, 156u,
                                     sizeof(pixels)) == UM_OK);
    CHECK(pixels[4u * 156u + 29u] == 255u);
    CHECK(pixels[4u * 156u + 30u] == 0u);
    CHECK(pixels[5u * 156u + 31u] == 0u);
    CHECK(pixels[4u * 156u + 32u] == 255u);
    CHECK(pixels[4u * 156u + 25u] == 255u);
    CHECK(pixels[4u * 156u + 130u] == 255u);
}

static void test_native_capture_formats_convert_to_gray(void)
{
    static const uint8_t gray_source[] = {
        1u, 2u, 3u, 4u, 90u, 91u,
        5u, 6u, 7u, 8u, 92u, 93u
    };
    static const uint8_t yuyv_source[] = {
        10u, 100u, 20u, 110u, 30u, 120u, 40u, 130u, 90u, 91u,
        50u, 101u, 60u, 111u, 70u, 121u, 80u, 131u, 92u, 93u
    };
    static const uint8_t bgra_source[] = {
        0u, 0u, 255u, 255u, 0u, 255u, 0u, 255u,
        255u, 0u, 0u, 255u, 255u, 255u, 255u, 255u
    };
    uint8_t output[8];
    ++tests_run;

    CHECK(um_light_capture_to_gray(
              UM_LIGHT_CAPTURE_GRAY8, gray_source, sizeof(gray_source),
              4u, 2u, 6u, output, sizeof(output)) == UM_OK);
    CHECK(memcmp(output, (const uint8_t[]){1u, 2u, 3u, 4u,
                                           5u, 6u, 7u, 8u},
                 sizeof(output)) == 0);
    CHECK(um_light_capture_to_gray(
              UM_LIGHT_CAPTURE_YUYV, yuyv_source, sizeof(yuyv_source),
              4u, 2u, 10u, output, sizeof(output)) == UM_OK);
    CHECK(memcmp(output, (const uint8_t[]){10u, 20u, 30u, 40u,
                                           50u, 60u, 70u, 80u},
                 sizeof(output)) == 0);
    CHECK(um_light_capture_to_gray(
              UM_LIGHT_CAPTURE_BGRA32, bgra_source, sizeof(bgra_source),
              2u, 2u, 8u, output, sizeof(output)) == UM_OK);
    CHECK(output[0] == 77u);
    CHECK(output[1] == 149u || output[1] == 150u);
    CHECK(output[2] == 29u);
    CHECK(output[3] == 255u);
}

static void test_video_errors_and_enumeration(void)
{
    uint8_t modules[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE] = {0};
    uint8_t pixels[64] = {0};
    um_light_video_config config = um_light_video_default_config();
    um_light_video *video = NULL;
    size_t messages = 0u;
    int list_status;
    ++tests_run;

    CHECK(um_light_rasterize_modules(NULL, sizeof(modules), pixels,
                                     52u, 52u, 52u,
                                     sizeof(pixels)) == UM_ERR_ARGUMENT);
    CHECK(um_light_rasterize_modules(modules, sizeof(modules) - 1u, pixels,
                                     52u, 52u, 52u,
                                     sizeof(pixels)) == UM_ERR_ARGUMENT);
    CHECK(um_light_capture_to_gray(
              UM_LIGHT_CAPTURE_GRAY8, pixels, sizeof(pixels), 8u, 8u, 8u,
              pixels, 63u) == UM_ERR_CAPACITY);
    CHECK(um_light_capture_to_gray(
              UM_LIGHT_CAPTURE_YUYV, pixels, sizeof(pixels), 3u, 2u, 8u,
              pixels, sizeof(pixels)) == UM_ERR_UNSUPPORTED);
    CHECK(um_light_capture_to_gray(
              99u, pixels, sizeof(pixels), 2u, 2u, 8u, pixels,
              sizeof(pixels)) == UM_ERR_UNSUPPORTED);
    config.camera_width = 0u;
    CHECK(um_light_video_open(&video, &config, NULL, NULL) ==
          UM_ERR_ARGUMENT);
    CHECK(video == NULL);
    CHECK(um_light_video_should_close(NULL) != 0);
    CHECK(um_light_video_list_devices(NULL, NULL) == UM_ERR_ARGUMENT);
    list_status = um_light_video_list_devices(count_log, &messages);
#if defined(__linux__) || defined(__APPLE__)
    CHECK(list_status == UM_OK);
    CHECK(messages >= 3u);
#else
    CHECK(list_status == UM_ERR_UNSUPPORTED);
    CHECK(messages >= 1u);
#endif
}

int main(void)
{
    test_default_video_configuration();
    test_module_raster_has_quiet_zone_and_exact_cells();
    test_rectangular_window_centers_square_symbol();
    test_native_capture_formats_convert_to_gray();
    test_video_errors_and_enumeration();

    if (failures != 0) {
        fprintf(stderr, "%d of %d light video tests failed\n", failures,
                tests_run);
        return 1;
    }
    printf("all %d light video tests passed\n", tests_run);
    return 0;
}
