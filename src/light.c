#include "um_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LIGHT_BORDER_MODULES 2u
#define LIGHT_SEPARATOR_MODULES 1u
#define LIGHT_DATA_FIRST                                                \
    (LIGHT_BORDER_MODULES + LIGHT_SEPARATOR_MODULES)
#define LIGHT_DATA_SIDE (UM_LIGHT_GRID_SIZE - 2u * LIGHT_DATA_FIRST)
#define LIGHT_DATA_MODULES (LIGHT_DATA_SIDE * LIGHT_DATA_SIDE)
#define LIGHT_RAW_BYTES 109u
#define LIGHT_RAW_BITS (LIGHT_RAW_BYTES * 8u)
#define LIGHT_HEADER_BYTES 14u
#define LIGHT_CRC_BYTES 4u
#define LIGHT_CODED_BITS (2u * (LIGHT_RAW_BITS + UM_FEC_TAIL_BITS))
#define LIGHT_MAGIC_0 UINT8_C(0xb6)
#define LIGHT_MAGIC_1 UINT8_C(0x4d)
#define LIGHT_WIRE_VERSION UINT8_C(1)

typedef struct {
    double value[9];
} light_matrix;

typedef struct {
    size_t count;
    size_t min_x;
    size_t max_x;
    size_t min_y;
    size_t max_y;
    int64_t min_sum;
    int64_t max_sum;
    int64_t min_difference;
    int64_t max_difference;
    um_light_point min_sum_point;
    um_light_point max_sum_point;
    um_light_point min_difference_point;
    um_light_point max_difference_point;
} light_component;

_Static_assert(LIGHT_DATA_SIDE == 42u, "unexpected light grid geometry");
_Static_assert(LIGHT_CODED_BITS <= LIGHT_DATA_MODULES,
               "light FEC does not fit the module grid");
_Static_assert(UM_LIGHT_MAX_PAYLOAD ==
                   LIGHT_RAW_BYTES - LIGHT_HEADER_BYTES - LIGHT_CRC_BYTES,
               "public light payload capacity is stale");

static void light_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void light_write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static uint16_t light_read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t light_read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) |
           ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | bytes[3];
}

static uint32_t light_random(uint32_t *state)
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

static float light_random_unit(uint32_t *state)
{
    return (float)(light_random(state) >> 8u) /
           (float)UINT32_C(0x01000000);
}

static void light_whiten(uint8_t bytes[LIGHT_RAW_BYTES])
{
    uint32_t state = UINT32_C(0x87a4c31d);
    size_t i;
    for (i = 0u; i < LIGHT_RAW_BYTES; ++i) {
        bytes[i] ^= (uint8_t)(light_random(&state) >> 24u);
    }
}

static int light_homography(const um_light_point corners[4],
                            light_matrix *matrix)
{
    double dx1 = (double)corners[1].x - corners[2].x;
    double dx2 = (double)corners[3].x - corners[2].x;
    double dx3 = (double)corners[0].x - corners[1].x + corners[2].x -
                 corners[3].x;
    double dy1 = (double)corners[1].y - corners[2].y;
    double dy2 = (double)corners[3].y - corners[2].y;
    double dy3 = (double)corners[0].y - corners[1].y + corners[2].y -
                 corners[3].y;
    double projective_x = 0.0;
    double projective_y = 0.0;

    if (fabs(dx3) > 1.0e-9 || fabs(dy3) > 1.0e-9) {
        double denominator = dx1 * dy2 - dx2 * dy1;
        if (fabs(denominator) < 1.0e-9) {
            return UM_ERR_CONFIG;
        }
        projective_x = (dx3 * dy2 - dx2 * dy3) / denominator;
        projective_y = (dx1 * dy3 - dx3 * dy1) / denominator;
    }

    matrix->value[0] = (double)corners[1].x - corners[0].x +
                       projective_x * corners[1].x;
    matrix->value[1] = (double)corners[3].x - corners[0].x +
                       projective_y * corners[3].x;
    matrix->value[2] = corners[0].x;
    matrix->value[3] = (double)corners[1].y - corners[0].y +
                       projective_x * corners[1].y;
    matrix->value[4] = (double)corners[3].y - corners[0].y +
                       projective_y * corners[3].y;
    matrix->value[5] = corners[0].y;
    matrix->value[6] = projective_x;
    matrix->value[7] = projective_y;
    matrix->value[8] = 1.0;
    return UM_OK;
}

static int light_matrix_inverse(const light_matrix *input,
                                light_matrix *inverse)
{
    const double *m = input->value;
    double determinant =
        m[0] * (m[4] * m[8] - m[5] * m[7]) -
        m[1] * (m[3] * m[8] - m[5] * m[6]) +
        m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (fabs(determinant) < 1.0e-12) {
        return UM_ERR_CONFIG;
    }
    inverse->value[0] = (m[4] * m[8] - m[5] * m[7]) / determinant;
    inverse->value[1] = (m[2] * m[7] - m[1] * m[8]) / determinant;
    inverse->value[2] = (m[1] * m[5] - m[2] * m[4]) / determinant;
    inverse->value[3] = (m[5] * m[6] - m[3] * m[8]) / determinant;
    inverse->value[4] = (m[0] * m[8] - m[2] * m[6]) / determinant;
    inverse->value[5] = (m[2] * m[3] - m[0] * m[5]) / determinant;
    inverse->value[6] = (m[3] * m[7] - m[4] * m[6]) / determinant;
    inverse->value[7] = (m[1] * m[6] - m[0] * m[7]) / determinant;
    inverse->value[8] = (m[0] * m[4] - m[1] * m[3]) / determinant;
    return UM_OK;
}

static int light_project(const light_matrix *matrix, double x, double y,
                         double *projected_x, double *projected_y)
{
    double denominator = matrix->value[6] * x +
                         matrix->value[7] * y + matrix->value[8];
    if (fabs(denominator) < 1.0e-12) {
        return 0;
    }
    *projected_x = (matrix->value[0] * x + matrix->value[1] * y +
                    matrix->value[2]) /
                   denominator;
    *projected_y = (matrix->value[3] * x + matrix->value[4] * y +
                    matrix->value[5]) /
                   denominator;
    return 1;
}

static double light_quad_area(const um_light_point corners[4])
{
    double twice_area = 0.0;
    unsigned i;
    for (i = 0u; i < 4u; ++i) {
        unsigned next = (i + 1u) % 4u;
        twice_area += (double)corners[i].x * corners[next].y -
                      (double)corners[next].x * corners[i].y;
    }
    return fabs(twice_area) * 0.5;
}

static int light_channel_validate(const um_light_channel_config *config)
{
    unsigned i;
    if (config == NULL || config->image_width < UM_LIGHT_GRID_SIZE ||
        config->image_height < UM_LIGHT_GRID_SIZE ||
        config->image_width > SIZE_MAX / config->image_height ||
        !isfinite(config->black_level) ||
        !isfinite(config->white_level) ||
        !isfinite(config->noise_stddev) || config->black_level < 0.0f ||
        config->black_level >= config->white_level ||
        config->white_level > 1.0f || config->noise_stddev < 0.0f ||
        config->noise_stddev > 1.0f || config->blur_radius > 16u ||
        !isfinite(config->occlusion_x) ||
        !isfinite(config->occlusion_y) ||
        !isfinite(config->occlusion_width) ||
        !isfinite(config->occlusion_height) || config->occlusion_x < 0.0f ||
        config->occlusion_y < 0.0f || config->occlusion_width < 0.0f ||
        config->occlusion_height < 0.0f ||
        config->occlusion_x + config->occlusion_width > 1.0f ||
        config->occlusion_y + config->occlusion_height > 1.0f) {
        return UM_ERR_CONFIG;
    }
    for (i = 0u; i < 4u; ++i) {
        if (!isfinite(config->corners[i].x) ||
            !isfinite(config->corners[i].y) || config->corners[i].x < 0.0f ||
            config->corners[i].y < 0.0f ||
            config->corners[i].x >= (float)config->image_width ||
            config->corners[i].y >= (float)config->image_height) {
            return UM_ERR_CONFIG;
        }
    }
    if (light_quad_area(config->corners) <
        (double)UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE / 4.0) {
        return UM_ERR_CONFIG;
    }
    return UM_OK;
}

um_light_channel_config um_light_channel_default_config(void)
{
    um_light_channel_config config;
    memset(&config, 0, sizeof(config));
    config.image_width = 640u;
    config.image_height = 480u;
    config.corners[0].x = 142.0f;
    config.corners[0].y = 74.0f;
    config.corners[1].x = 514.0f;
    config.corners[1].y = 104.0f;
    config.corners[2].x = 548.0f;
    config.corners[2].y = 406.0f;
    config.corners[3].x = 104.0f;
    config.corners[3].y = 434.0f;
    config.black_level = 0.06f;
    config.white_level = 0.94f;
    config.noise_stddev = 0.035f;
    config.blur_radius = 1u;
    config.random_seed = UINT32_C(0x51d7a93b);
    return config;
}

int um_light_encode_frame(uint8_t frame_type, uint32_t session_id,
                          uint32_t sequence, const uint8_t *payload,
                          size_t payload_length, uint8_t *modules,
                          size_t module_capacity)
{
    uint8_t raw[LIGHT_RAW_BYTES];
    uint8_t bits[LIGHT_RAW_BITS];
    uint8_t coded[LIGHT_CODED_BITS];
    uint8_t interleaved[LIGHT_CODED_BITS];
    size_t coded_count = 0u;
    size_t row;
    size_t column;
    size_t i;
    uint32_t crc;
    int status;

    if ((payload_length != 0u && payload == NULL) || modules == NULL ||
        module_capacity < UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE) {
        return UM_ERR_ARGUMENT;
    }
    if (payload_length > UM_LIGHT_MAX_PAYLOAD) {
        return UM_ERR_CAPACITY;
    }

    memset(modules, 0,
           UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE * sizeof(*modules));
    for (row = 0u; row < UM_LIGHT_GRID_SIZE; ++row) {
        for (column = 0u; column < UM_LIGHT_GRID_SIZE; ++column) {
            if (row < LIGHT_BORDER_MODULES ||
                row >= UM_LIGHT_GRID_SIZE - LIGHT_BORDER_MODULES ||
                column < LIGHT_BORDER_MODULES ||
                column >= UM_LIGHT_GRID_SIZE - LIGHT_BORDER_MODULES) {
                modules[row * UM_LIGHT_GRID_SIZE + column] = 1u;
            }
        }
    }

    memset(raw, 0, sizeof(raw));
    raw[0] = LIGHT_MAGIC_0;
    raw[1] = LIGHT_MAGIC_1;
    raw[2] = LIGHT_WIRE_VERSION;
    raw[3] = frame_type;
    light_write_u32(&raw[4], session_id);
    light_write_u32(&raw[8], sequence);
    light_write_u16(&raw[12], (uint16_t)payload_length);
    if (payload_length != 0u) {
        memcpy(&raw[LIGHT_HEADER_BYTES], payload, payload_length);
    }
    crc = um_crc32(raw, LIGHT_HEADER_BYTES + payload_length);
    light_write_u32(&raw[LIGHT_RAW_BYTES - LIGHT_CRC_BYTES], crc);
    light_whiten(raw);
    um_bytes_to_bits(raw, sizeof(raw), bits);
    status = um_fec_encode(bits, LIGHT_RAW_BITS, UM_FEC_RATE_1_2, coded,
                           sizeof(coded), &coded_count);
    if (status != UM_OK || coded_count != LIGHT_CODED_BITS) {
        return status != UM_OK ? status : UM_ERR_CONFIG;
    }
    status = um_interleave_bits(coded, interleaved, coded_count);
    if (status != UM_OK) {
        return status;
    }

    for (i = 0u; i < LIGHT_DATA_MODULES; ++i) {
        row = LIGHT_DATA_FIRST + i / LIGHT_DATA_SIDE;
        column = LIGHT_DATA_FIRST + i % LIGHT_DATA_SIDE;
        modules[row * UM_LIGHT_GRID_SIZE + column] =
            i < coded_count ? interleaved[i] : (uint8_t)(i & 1u);
    }
    return UM_OK;
}

static int light_box_blur(uint8_t *pixels, size_t width, size_t height,
                          unsigned radius)
{
    uint8_t *temporary;
    size_t x;
    size_t y;
    if (radius == 0u) {
        return UM_OK;
    }
    temporary = (uint8_t *)malloc(width * height);
    if (temporary == NULL) {
        return UM_ERR_MEMORY;
    }
    for (y = 0u; y < height; ++y) {
        for (x = 0u; x < width; ++x) {
            size_t first = x > radius ? x - radius : 0u;
            size_t last = x + radius < width ? x + radius : width - 1u;
            size_t cursor;
            unsigned sum = 0u;
            for (cursor = first; cursor <= last; ++cursor) {
                sum += pixels[y * width + cursor];
            }
            temporary[y * width + x] =
                (uint8_t)(sum / (unsigned)(last - first + 1u));
        }
    }
    for (y = 0u; y < height; ++y) {
        for (x = 0u; x < width; ++x) {
            size_t first = y > radius ? y - radius : 0u;
            size_t last = y + radius < height ? y + radius : height - 1u;
            size_t cursor;
            unsigned sum = 0u;
            for (cursor = first; cursor <= last; ++cursor) {
                sum += temporary[cursor * width + x];
            }
            pixels[y * width + x] =
                (uint8_t)(sum / (unsigned)(last - first + 1u));
        }
    }
    free(temporary);
    return UM_OK;
}

int um_light_render_frame(const uint8_t *modules, size_t module_count,
                          const um_light_channel_config *config,
                          uint8_t **pixels, size_t *pixel_count)
{
    light_matrix forward;
    light_matrix inverse;
    uint8_t *rendered = NULL;
    size_t count;
    size_t i;
    size_t x;
    size_t y;
    uint32_t random_state;
    int status;

    if (modules == NULL || pixels == NULL || pixel_count == NULL) {
        return UM_ERR_ARGUMENT;
    }
    *pixels = NULL;
    *pixel_count = 0u;
    if (module_count != UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE) {
        return UM_ERR_ARGUMENT;
    }
    status = light_channel_validate(config);
    if (status != UM_OK) {
        return status;
    }
    status = light_homography(config->corners, &forward);
    if (status != UM_OK) {
        return status;
    }
    status = light_matrix_inverse(&forward, &inverse);
    if (status != UM_OK) {
        return status;
    }

    count = config->image_width * config->image_height;
    rendered = (uint8_t *)malloc(count);
    if (rendered == NULL) {
        return UM_ERR_MEMORY;
    }
    for (y = 0u; y < config->image_height; ++y) {
        for (x = 0u; x < config->image_width; ++x) {
            double u;
            double v;
            float level = config->white_level;
            if (light_project(&inverse, (double)x + 0.5, (double)y + 0.5,
                              &u, &v) != 0 &&
                u >= 0.0 && u < 1.0 && v >= 0.0 && v < 1.0) {
                size_t column = (size_t)(u * UM_LIGHT_GRID_SIZE);
                size_t row = (size_t)(v * UM_LIGHT_GRID_SIZE);
                if (modules[row * UM_LIGHT_GRID_SIZE + column] != 0u) {
                    level = config->black_level;
                }
            }
            rendered[y * config->image_width + x] =
                (uint8_t)lrintf(level * 255.0f);
        }
    }

    if (config->occlusion_width > 0.0f && config->occlusion_height > 0.0f) {
        size_t first_x =
            (size_t)(config->occlusion_x * config->image_width);
        size_t first_y =
            (size_t)(config->occlusion_y * config->image_height);
        size_t last_x = (size_t)((config->occlusion_x +
                                  config->occlusion_width) *
                                 config->image_width);
        size_t last_y = (size_t)((config->occlusion_y +
                                  config->occlusion_height) *
                                 config->image_height);
        uint8_t white = (uint8_t)lrintf(config->white_level * 255.0f);
        if (last_x > config->image_width) {
            last_x = config->image_width;
        }
        if (last_y > config->image_height) {
            last_y = config->image_height;
        }
        for (y = first_y; y < last_y; ++y) {
            for (x = first_x; x < last_x; ++x) {
                rendered[y * config->image_width + x] = white;
            }
        }
    }

    status = light_box_blur(rendered, config->image_width,
                            config->image_height, config->blur_radius);
    if (status != UM_OK) {
        free(rendered);
        return status;
    }
    random_state = config->random_seed;
    if (config->noise_stddev > 0.0f) {
        for (i = 0u; i < count; ++i) {
            float gaussian = -3.0f;
            unsigned sample;
            float noisy;
            for (sample = 0u; sample < 6u; ++sample) {
                gaussian += light_random_unit(&random_state);
            }
            gaussian *= 1.41421356f;
            noisy = (float)rendered[i] +
                    gaussian * config->noise_stddev * 255.0f;
            if (noisy < 0.0f) {
                noisy = 0.0f;
            } else if (noisy > 255.0f) {
                noisy = 255.0f;
            }
            rendered[i] = (uint8_t)lrintf(noisy);
        }
    }

    *pixels = rendered;
    *pixel_count = count;
    return UM_OK;
}

static uint8_t light_otsu_threshold(const uint8_t *pixels, size_t width,
                                    size_t height, size_t stride)
{
    uint64_t histogram[256] = {0u};
    uint64_t total = (uint64_t)width * height;
    uint64_t total_sum = 0u;
    uint64_t background_count = 0u;
    uint64_t background_sum = 0u;
    double best_score = -1.0;
    unsigned best = 127u;
    size_t x;
    size_t y;
    unsigned level;

    for (y = 0u; y < height; ++y) {
        for (x = 0u; x < width; ++x) {
            ++histogram[pixels[y * stride + x]];
        }
    }
    for (level = 0u; level < 256u; ++level) {
        total_sum += histogram[level] * level;
    }
    for (level = 0u; level < 255u; ++level) {
        uint64_t foreground_count;
        double background_mean;
        double foreground_mean;
        double difference;
        double score;
        background_count += histogram[level];
        background_sum += histogram[level] * level;
        if (background_count == 0u) {
            continue;
        }
        foreground_count = total - background_count;
        if (foreground_count == 0u) {
            break;
        }
        background_mean = (double)background_sum / background_count;
        foreground_mean =
            (double)(total_sum - background_sum) / foreground_count;
        difference = background_mean - foreground_mean;
        score = (double)background_count * foreground_count * difference *
                difference;
        if (score > best_score) {
            best_score = score;
            best = level;
        }
    }
    return (uint8_t)best;
}

static void light_component_begin(light_component *component, size_t x,
                                  size_t y)
{
    int64_t sum = (int64_t)x + (int64_t)y;
    int64_t difference = (int64_t)x - (int64_t)y;
    component->count = 0u;
    component->min_x = x;
    component->max_x = x;
    component->min_y = y;
    component->max_y = y;
    component->min_sum = sum;
    component->max_sum = sum;
    component->min_difference = difference;
    component->max_difference = difference;
    component->min_sum_point.x = (float)x;
    component->min_sum_point.y = (float)y;
    component->max_sum_point = component->min_sum_point;
    component->min_difference_point = component->min_sum_point;
    component->max_difference_point = component->min_sum_point;
}

static void light_component_add(light_component *component, size_t x,
                                size_t y)
{
    int64_t sum = (int64_t)x + (int64_t)y;
    int64_t difference = (int64_t)x - (int64_t)y;
    ++component->count;
    if (x < component->min_x) {
        component->min_x = x;
    }
    if (x > component->max_x) {
        component->max_x = x;
    }
    if (y < component->min_y) {
        component->min_y = y;
    }
    if (y > component->max_y) {
        component->max_y = y;
    }
    if (sum < component->min_sum) {
        component->min_sum = sum;
        component->min_sum_point.x = (float)x;
        component->min_sum_point.y = (float)y;
    }
    if (sum > component->max_sum) {
        component->max_sum = sum;
        component->max_sum_point.x = (float)x;
        component->max_sum_point.y = (float)y;
    }
    if (difference < component->min_difference) {
        component->min_difference = difference;
        component->min_difference_point.x = (float)x;
        component->min_difference_point.y = (float)y;
    }
    if (difference > component->max_difference) {
        component->max_difference = difference;
        component->max_difference_point.x = (float)x;
        component->max_difference_point.y = (float)y;
    }
}

static int light_find_locator(const uint8_t *pixels, size_t width,
                              size_t height, size_t stride, uint8_t threshold,
                              um_light_point corners[4])
{
    uint8_t *visited = NULL;
    size_t *queue = NULL;
    size_t total = width * height;
    size_t best_area = 0u;
    light_component best;
    size_t start;
    int found = 0;

    if (total > SIZE_MAX / sizeof(*queue)) {
        return UM_ERR_MEMORY;
    }
    visited = (uint8_t *)calloc(total, 1u);
    queue = (size_t *)malloc(total * sizeof(*queue));
    if (visited == NULL || queue == NULL) {
        free(queue);
        free(visited);
        return UM_ERR_MEMORY;
    }

    for (start = 0u; start < total; ++start) {
        size_t start_x = start % width;
        size_t start_y = start / width;
        light_component component;
        size_t head = 0u;
        size_t tail = 0u;
        size_t area;
        if (visited[start] != 0u ||
            pixels[start_y * stride + start_x] > threshold) {
            continue;
        }
        light_component_begin(&component, start_x, start_y);
        visited[start] = 1u;
        queue[tail++] = start;
        while (head < tail) {
            size_t position = queue[head++];
            size_t x = position % width;
            size_t y = position / width;
            int dy;
            light_component_add(&component, x, y);
            for (dy = -1; dy <= 1; ++dy) {
                int dx;
                for (dx = -1; dx <= 1; ++dx) {
                    size_t neighbor;
                    size_t nx;
                    size_t ny;
                    if ((dx == 0 && dy == 0) ||
                        (dx < 0 && x == 0u) ||
                        (dx > 0 && x + 1u == width) ||
                        (dy < 0 && y == 0u) ||
                        (dy > 0 && y + 1u == height)) {
                        continue;
                    }
                    nx = (size_t)((int64_t)x + dx);
                    ny = (size_t)((int64_t)y + dy);
                    neighbor = ny * width + nx;
                    if (visited[neighbor] == 0u &&
                        pixels[ny * stride + nx] <= threshold) {
                        visited[neighbor] = 1u;
                        queue[tail++] = neighbor;
                    }
                }
            }
        }
        area = (component.max_x - component.min_x + 1u) *
               (component.max_y - component.min_y + 1u);
        if (component.min_x == 0u || component.min_y == 0u ||
            component.max_x == width - 1u ||
            component.max_y == height - 1u ||
            component.max_x - component.min_x + 1u <
                UM_LIGHT_GRID_SIZE / 2u ||
            component.max_y - component.min_y + 1u <
                UM_LIGHT_GRID_SIZE / 2u ||
            component.count < (component.max_x - component.min_x + 1u) +
                                  (component.max_y - component.min_y + 1u)) {
            continue;
        }
        if (!found || area > best_area) {
            best = component;
            best_area = area;
            found = 1;
        }
    }
    free(queue);
    free(visited);
    if (!found) {
        return UM_ERR_SYNC;
    }

    corners[0] = best.min_sum_point;
    corners[1] = best.max_difference_point;
    corners[2] = best.max_sum_point;
    corners[3] = best.min_difference_point;
    if (light_quad_area(corners) <
        (double)UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE / 8.0) {
        return UM_ERR_SYNC;
    }
    return UM_OK;
}

static float light_sample_pixel(const uint8_t *pixels, size_t width,
                                size_t height, size_t stride, double x,
                                double y)
{
    size_t x0;
    size_t y0;
    size_t x1;
    size_t y1;
    double fx;
    double fy;
    double top;
    double bottom;
    if (x < 0.0) {
        x = 0.0;
    } else if (x > (double)(width - 1u)) {
        x = (double)(width - 1u);
    }
    if (y < 0.0) {
        y = 0.0;
    } else if (y > (double)(height - 1u)) {
        y = (double)(height - 1u);
    }
    x0 = (size_t)x;
    y0 = (size_t)y;
    x1 = x0 + 1u < width ? x0 + 1u : x0;
    y1 = y0 + 1u < height ? y0 + 1u : y0;
    fx = x - (double)x0;
    fy = y - (double)y0;
    top = (1.0 - fx) * pixels[y0 * stride + x0] +
          fx * pixels[y0 * stride + x1];
    bottom = (1.0 - fx) * pixels[y1 * stride + x0] +
             fx * pixels[y1 * stride + x1];
    return (float)((1.0 - fy) * top + fy * bottom);
}

static int light_sample_grid(const uint8_t *pixels, size_t width,
                             size_t height, size_t stride,
                             const light_matrix *forward, double phase_x,
                             double phase_y, float *samples)
{
    static const double offsets[] = {-0.18, 0.0, 0.18};
    size_t row;
    size_t column;
    for (row = 0u; row < UM_LIGHT_GRID_SIZE; ++row) {
        for (column = 0u; column < UM_LIGHT_GRID_SIZE; ++column) {
            double sum = 0.0;
            unsigned count = 0u;
            unsigned oy;
            for (oy = 0u; oy < 3u; ++oy) {
                unsigned ox;
                for (ox = 0u; ox < 3u; ++ox) {
                    double u = ((double)column + 0.5 + offsets[ox] +
                                phase_x) /
                               UM_LIGHT_GRID_SIZE;
                    double v = ((double)row + 0.5 + offsets[oy] +
                                phase_y) /
                               UM_LIGHT_GRID_SIZE;
                    double x;
                    double y;
                    if (light_project(forward, u, v, &x, &y) == 0) {
                        return UM_ERR_SYNC;
                    }
                    sum += light_sample_pixel(pixels, width, height, stride,
                                              x, y);
                    ++count;
                }
            }
            samples[row * UM_LIGHT_GRID_SIZE + column] =
                (float)(sum / count);
        }
    }
    return UM_OK;
}

static void light_orient(unsigned orientation, size_t row, size_t column,
                         size_t *physical_row, size_t *physical_column)
{
    size_t last = UM_LIGHT_GRID_SIZE - 1u;
    switch (orientation) {
    case 0u:
        *physical_row = row;
        *physical_column = column;
        break;
    case 1u:
        *physical_row = column;
        *physical_column = last - row;
        break;
    case 2u:
        *physical_row = last - row;
        *physical_column = last - column;
        break;
    case 3u:
        *physical_row = last - column;
        *physical_column = row;
        break;
    case 4u:
        *physical_row = row;
        *physical_column = last - column;
        break;
    case 5u:
        *physical_row = last - column;
        *physical_column = last - row;
        break;
    case 6u:
        *physical_row = last - row;
        *physical_column = column;
        break;
    default:
        *physical_row = column;
        *physical_column = row;
        break;
    }
}

static int light_decode_orientation(const float *grid, float threshold,
                                    float contrast, unsigned orientation,
                                    uint8_t *frame_type,
                                    uint32_t *session_id, uint32_t *sequence,
                                    uint8_t *payload,
                                    size_t payload_capacity,
                                    size_t *payload_length,
                                    float *corrected_fraction)
{
    float interleaved_soft[LIGHT_CODED_BITS];
    float coded_soft[LIGHT_CODED_BITS];
    uint8_t decoded_bits[LIGHT_RAW_BITS];
    uint8_t reconstructed[LIGHT_CODED_BITS];
    uint8_t reconstructed_interleaved[LIGHT_CODED_BITS];
    uint8_t raw[LIGHT_RAW_BYTES];
    size_t reconstructed_count = 0u;
    size_t length;
    size_t i;
    size_t disagreements = 0u;
    uint32_t expected_crc;
    int status;

    for (i = 0u; i < LIGHT_CODED_BITS; ++i) {
        size_t logical_row = LIGHT_DATA_FIRST + i / LIGHT_DATA_SIDE;
        size_t logical_column = LIGHT_DATA_FIRST + i % LIGHT_DATA_SIDE;
        size_t row;
        size_t column;
        float soft;
        light_orient(orientation, logical_row, logical_column, &row,
                     &column);
        soft = (threshold - grid[row * UM_LIGHT_GRID_SIZE + column]) *
               (4.0f / contrast);
        if (soft > 6.0f) {
            soft = 6.0f;
        } else if (soft < -6.0f) {
            soft = -6.0f;
        }
        interleaved_soft[i] = soft;
    }
    status = um_deinterleave_soft(interleaved_soft, coded_soft,
                                  LIGHT_CODED_BITS);
    if (status != UM_OK) {
        return status;
    }
    status = um_fec_decode(coded_soft, LIGHT_CODED_BITS, LIGHT_RAW_BITS,
                           UM_FEC_RATE_1_2, decoded_bits,
                           sizeof(decoded_bits));
    if (status != UM_OK) {
        return status;
    }
    um_bits_to_bytes(decoded_bits, LIGHT_RAW_BITS, raw);
    light_whiten(raw);
    if (raw[0] != LIGHT_MAGIC_0 || raw[1] != LIGHT_MAGIC_1 ||
        raw[2] != LIGHT_WIRE_VERSION) {
        return UM_ERR_HEADER;
    }
    length = light_read_u16(&raw[12]);
    if (length > UM_LIGHT_MAX_PAYLOAD) {
        return UM_ERR_HEADER;
    }
    expected_crc = light_read_u32(&raw[LIGHT_RAW_BYTES - LIGHT_CRC_BYTES]);
    if (um_crc32(raw, LIGHT_HEADER_BYTES + length) != expected_crc) {
        return UM_ERR_CRC;
    }
    *frame_type = raw[3];
    *session_id = light_read_u32(&raw[4]);
    *sequence = light_read_u32(&raw[8]);
    *payload_length = length;
    if (payload_capacity < length || (length != 0u && payload == NULL)) {
        return UM_ERR_CAPACITY;
    }

    status = um_fec_encode(decoded_bits, LIGHT_RAW_BITS, UM_FEC_RATE_1_2,
                           reconstructed, sizeof(reconstructed),
                           &reconstructed_count);
    if (status != UM_OK || reconstructed_count != LIGHT_CODED_BITS) {
        return status != UM_OK ? status : UM_ERR_CONFIG;
    }
    status = um_interleave_bits(reconstructed, reconstructed_interleaved,
                                LIGHT_CODED_BITS);
    if (status != UM_OK) {
        return status;
    }
    for (i = 0u; i < LIGHT_CODED_BITS; ++i) {
        uint8_t observed = interleaved_soft[i] >= 0.0f ? 1u : 0u;
        if (observed != reconstructed_interleaved[i]) {
            ++disagreements;
        }
    }

    if (length != 0u) {
        memcpy(payload, &raw[LIGHT_HEADER_BYTES], length);
    }
    *corrected_fraction = (float)disagreements / LIGHT_CODED_BITS;
    return UM_OK;
}

int um_light_decode_frame(const uint8_t *pixels, size_t width, size_t height,
                          size_t stride, uint8_t *frame_type,
                          uint32_t *session_id, uint32_t *sequence,
                          uint8_t *payload, size_t payload_capacity,
                          size_t *payload_length,
                          um_light_rx_metrics *metrics)
{
    static const struct {
        double x;
        double y;
    } phases[] = {
        /* Connected-component corners are quantized to whole camera pixels.
         * Try the adjacent sub-module alignments only when CRC requires it. */
        {0.0, 0.0},  {0.2, 0.0},  {-0.2, 0.0}, {0.0, 0.2},
        {0.0, -0.2}, {0.2, 0.2},  {0.2, -0.2}, {-0.2, 0.2},
        {-0.2, -0.2},
    };
    um_light_point corners[4];
    light_matrix forward;
    float grid[UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE];
    uint8_t locator_threshold;
    size_t phase;
    int found_locator_contrast = 0;
    int status;

    if (pixels == NULL || width < UM_LIGHT_GRID_SIZE ||
        height < UM_LIGHT_GRID_SIZE || stride < width ||
        height > SIZE_MAX / stride || frame_type == NULL ||
        session_id == NULL || sequence == NULL || payload_length == NULL ||
        metrics == NULL) {
        return UM_ERR_ARGUMENT;
    }
    memset(metrics, 0, sizeof(*metrics));
    locator_threshold = light_otsu_threshold(pixels, width, height, stride);
    status = light_find_locator(pixels, width, height, stride,
                                locator_threshold, corners);
    if (status != UM_OK) {
        return status;
    }
    status = light_homography(corners, &forward);
    if (status != UM_OK) {
        return UM_ERR_SYNC;
    }
    for (phase = 0u; phase < sizeof(phases) / sizeof(phases[0]); ++phase) {
        double black_sum = 0.0;
        double white_sum = 0.0;
        size_t black_count = 0u;
        size_t white_count = 0u;
        float black_mean;
        float white_mean;
        float threshold;
        float contrast;
        size_t row;
        size_t column;
        unsigned orientation;

        status = light_sample_grid(pixels, width, height, stride, &forward,
                                   phases[phase].x, phases[phase].y, grid);
        if (status != UM_OK) {
            continue;
        }
        for (row = 0u; row < UM_LIGHT_GRID_SIZE; ++row) {
            for (column = 0u; column < UM_LIGHT_GRID_SIZE; ++column) {
                float value = grid[row * UM_LIGHT_GRID_SIZE + column];
                if (row < LIGHT_BORDER_MODULES ||
                    row >= UM_LIGHT_GRID_SIZE - LIGHT_BORDER_MODULES ||
                    column < LIGHT_BORDER_MODULES ||
                    column >= UM_LIGHT_GRID_SIZE - LIGHT_BORDER_MODULES) {
                    black_sum += value;
                    ++black_count;
                } else if (
                    row == LIGHT_BORDER_MODULES ||
                    row == UM_LIGHT_GRID_SIZE - LIGHT_BORDER_MODULES - 1u ||
                    column == LIGHT_BORDER_MODULES ||
                    column ==
                        UM_LIGHT_GRID_SIZE - LIGHT_BORDER_MODULES - 1u) {
                    white_sum += value;
                    ++white_count;
                }
            }
        }
        black_mean = (float)(black_sum / black_count);
        white_mean = (float)(white_sum / white_count);
        contrast = white_mean - black_mean;
        if (contrast < 24.0f) {
            continue;
        }
        found_locator_contrast = 1;
        threshold = 0.5f * (black_mean + white_mean);

        for (orientation = 0u; orientation < 8u; ++orientation) {
            float corrected_fraction = 0.0f;
            status = light_decode_orientation(
                grid, threshold, contrast, orientation, frame_type,
                session_id, sequence, payload, payload_capacity,
                payload_length, &corrected_fraction);
            if (status == UM_OK || status == UM_ERR_CAPACITY) {
                unsigned i;
                for (i = 0u; i < 4u; ++i) {
                    metrics->corners[i] = corners[i];
                }
                metrics->threshold = threshold / 255.0f;
                metrics->contrast = contrast / 255.0f;
                metrics->image_coverage = (float)(
                    light_quad_area(corners) / ((double)width * height));
                metrics->corrected_bit_fraction = corrected_fraction;
                metrics->orientation = orientation;
                return status;
            }
        }
    }
    return found_locator_contrast != 0 ? UM_ERR_CRC : UM_ERR_SYNC;
}
