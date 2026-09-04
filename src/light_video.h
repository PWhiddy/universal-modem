#ifndef UM_LIGHT_VIDEO_H
#define UM_LIGHT_VIDEO_H

#include "um.h"

#include <stddef.h>
#include <stdint.h>

#define UM_LIGHT_CAPTURE_GRAY8 1u
#define UM_LIGHT_CAPTURE_YUYV 2u
#define UM_LIGHT_CAPTURE_BGRA32 3u

typedef struct um_light_video um_light_video;

typedef struct {
    const char *camera_device;
    unsigned camera_width;
    unsigned camera_height;
    unsigned frames_per_second;
    unsigned window_size;
} um_light_video_config;

um_light_video_config um_light_video_default_config(void);
int um_light_video_list_devices(um_log_callback logger, void *logger_context);
int um_light_video_open(um_light_video **video,
                        const um_light_video_config *config,
                        um_log_callback logger, void *logger_context);
void um_light_video_close(um_light_video *video);
int um_light_video_present(um_light_video *video, const uint8_t *modules,
                           size_t module_count);
int um_light_video_capture(um_light_video *video, uint8_t *pixels,
                           size_t pixel_capacity, unsigned timeout_ms,
                           size_t *width, size_t *height);
int um_light_video_should_close(um_light_video *video);

int um_light_rasterize_modules(const uint8_t *modules, size_t module_count,
                               uint8_t *pixels, size_t width, size_t height,
                               size_t stride, size_t pixel_capacity);
int um_light_capture_to_gray(unsigned format, const uint8_t *source,
                             size_t source_length, size_t width,
                             size_t height, size_t source_stride,
                             uint8_t *destination,
                             size_t destination_capacity);

#endif
