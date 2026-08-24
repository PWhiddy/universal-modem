#ifndef UM_AUDIO_H
#define UM_AUDIO_H

#include "um.h"

#include <stddef.h>

typedef struct um_audio um_audio;

int um_audio_open(um_audio **audio, const char *input_device,
                  const char *output_device, um_log_callback logger,
                  void *logger_context);
void um_audio_close(um_audio *audio);
int um_audio_capture_enable(um_audio *audio, int enabled);
int um_audio_flush_capture(um_audio *audio);
int um_audio_read(um_audio *audio, float *samples, size_t capacity,
                  unsigned timeout_ms, size_t *frames_read);
int um_audio_write(um_audio *audio, const float *samples, size_t frame_count);

#endif
