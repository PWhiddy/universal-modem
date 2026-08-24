#include "audio.h"
#include "um_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void audio_log(um_log_callback logger, void *context,
                      const char *format, ...)
{
    char line[512];
    va_list arguments;
    if (logger == NULL) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    logger(context, line);
}

#if defined(__linux__)

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdint.h>

struct um_audio {
    snd_pcm_t *capture;
    snd_pcm_t *playback;
    int capture_enabled;
    um_log_callback logger;
    void *logger_context;
};

static void flatten_description(char *description)
{
    char *cursor;
    if (description == NULL) {
        return;
    }
    for (cursor = description; *cursor != '\0'; ++cursor) {
        if (*cursor == '\n' || *cursor == '\r') {
            *cursor = ' ';
        }
    }
}

int um_audio_list_devices(um_log_callback logger, void *logger_context)
{
    void **hints = NULL;
    void **hint;
    int status;
    if (logger == NULL) {
        return UM_ERR_ARGUMENT;
    }
    audio_log(logger, logger_context,
              "Audio backend: ALSA, format: 48000 Hz mono signed-16");
    status = snd_device_name_hint(-1, "pcm", &hints);
    if (status < 0) {
        audio_log(logger, logger_context, "Unable to enumerate ALSA devices: %s",
                  snd_strerror(status));
        return UM_ERR_AUDIO;
    }
    audio_log(logger, logger_context, "Audio input devices:");
    audio_log(logger, logger_context, "  default | ALSA default input");
    for (hint = hints; *hint != NULL; ++hint) {
        char *name = snd_device_name_get_hint(*hint, "NAME");
        char *description = snd_device_name_get_hint(*hint, "DESC");
        char *io = snd_device_name_get_hint(*hint, "IOID");
        if (name != NULL && strcmp(name, "default") != 0 &&
            (io == NULL || strcmp(io, "Input") == 0)) {
            flatten_description(description);
            audio_log(logger, logger_context, "  %s | %s", name,
                      description != NULL ? description : "ALSA PCM input");
        }
        free(io);
        free(description);
        free(name);
    }
    audio_log(logger, logger_context, "Audio output devices:");
    audio_log(logger, logger_context, "  default | ALSA default output");
    for (hint = hints; *hint != NULL; ++hint) {
        char *name = snd_device_name_get_hint(*hint, "NAME");
        char *description = snd_device_name_get_hint(*hint, "DESC");
        char *io = snd_device_name_get_hint(*hint, "IOID");
        if (name != NULL && strcmp(name, "default") != 0 &&
            (io == NULL || strcmp(io, "Output") == 0)) {
            flatten_description(description);
            audio_log(logger, logger_context, "  %s | %s", name,
                      description != NULL ? description : "ALSA PCM output");
        }
        free(io);
        free(description);
        free(name);
    }
    snd_device_name_free_hint(hints);
    return UM_OK;
}

static int configure_pcm(snd_pcm_t *pcm)
{
    int status = snd_pcm_set_params(
        pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1u,
        UM_SAMPLE_RATE, 1, 50000u);
    return status < 0 ? UM_ERR_AUDIO : UM_OK;
}

int um_audio_open(um_audio **audio, const char *input_device,
                  const char *output_device, um_log_callback logger,
                  void *logger_context)
{
    um_audio *opened;
    int status;
    if (audio == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (input_device == NULL || *input_device == '\0') {
        input_device = "default";
    }
    if (output_device == NULL || *output_device == '\0') {
        output_device = "default";
    }
    opened = (um_audio *)calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        return UM_ERR_MEMORY;
    }
    opened->logger = logger;
    opened->logger_context = logger_context;
    audio_log(logger, logger_context, "Selected audio input:  %s", input_device);
    audio_log(logger, logger_context, "Selected audio output: %s", output_device);
    status = snd_pcm_open(&opened->capture, input_device,
                          SND_PCM_STREAM_CAPTURE, 0);
    if (status < 0) {
        audio_log(logger, logger_context, "Cannot open input '%s': %s",
                  input_device, snd_strerror(status));
        um_audio_close(opened);
        return UM_ERR_AUDIO;
    }
    if (configure_pcm(opened->capture) != UM_OK) {
        audio_log(logger, logger_context,
                  "Input '%s' cannot provide 48 kHz mono audio", input_device);
        um_audio_close(opened);
        return UM_ERR_AUDIO;
    }
    status = snd_pcm_open(&opened->playback, output_device,
                          SND_PCM_STREAM_PLAYBACK, 0);
    if (status < 0) {
        audio_log(logger, logger_context, "Cannot open output '%s': %s",
                  output_device, snd_strerror(status));
        um_audio_close(opened);
        return UM_ERR_AUDIO;
    }
    if (configure_pcm(opened->playback) != UM_OK) {
        audio_log(logger, logger_context,
                  "Output '%s' cannot provide 48 kHz mono audio", output_device);
        um_audio_close(opened);
        return UM_ERR_AUDIO;
    }
    (void)snd_pcm_nonblock(opened->capture, 1);
    status = snd_pcm_prepare(opened->capture);
    if (status >= 0) {
        status = snd_pcm_start(opened->capture);
    }
    if (status < 0) {
        audio_log(logger, logger_context, "Cannot start audio capture: %s",
                  snd_strerror(status));
        um_audio_close(opened);
        return UM_ERR_AUDIO;
    }
    opened->capture_enabled = 1;
    *audio = opened;
    return UM_OK;
}

void um_audio_close(um_audio *audio)
{
    if (audio == NULL) {
        return;
    }
    if (audio->capture != NULL) {
        (void)snd_pcm_drop(audio->capture);
        snd_pcm_close(audio->capture);
    }
    if (audio->playback != NULL) {
        (void)snd_pcm_drop(audio->playback);
        snd_pcm_close(audio->playback);
    }
    free(audio);
}

int um_audio_capture_enable(um_audio *audio, int enabled)
{
    int status;
    if (audio == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if ((enabled != 0) == (audio->capture_enabled != 0)) {
        return UM_OK;
    }
    if (enabled == 0) {
        status = snd_pcm_drop(audio->capture);
        if (status < 0) {
            return UM_ERR_AUDIO;
        }
        audio->capture_enabled = 0;
        return UM_OK;
    }
    status = snd_pcm_prepare(audio->capture);
    if (status >= 0) {
        status = snd_pcm_start(audio->capture);
    }
    if (status < 0) {
        audio_log(audio->logger, audio->logger_context,
                  "Cannot resume audio capture: %s", snd_strerror(status));
        return UM_ERR_AUDIO;
    }
    audio->capture_enabled = 1;
    return UM_OK;
}

int um_audio_flush_capture(um_audio *audio)
{
    if (audio == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (audio->capture_enabled == 0) {
        return UM_OK;
    }
    if (snd_pcm_drop(audio->capture) < 0 ||
        snd_pcm_prepare(audio->capture) < 0 ||
        snd_pcm_start(audio->capture) < 0) {
        return UM_ERR_AUDIO;
    }
    return UM_OK;
}

int um_audio_read(um_audio *audio, float *samples, size_t capacity,
                  unsigned timeout_ms, size_t *frames_read)
{
    int16_t converted[1024];
    snd_pcm_sframes_t count;
    int status;
    size_t request;
    size_t i;
    if (audio == NULL || samples == NULL || capacity == 0u ||
        frames_read == NULL || audio->capture_enabled == 0) {
        return UM_ERR_ARGUMENT;
    }
    request = capacity < sizeof(converted) / sizeof(converted[0])
                  ? capacity
                  : sizeof(converted) / sizeof(converted[0]);
    status = snd_pcm_wait(audio->capture, (int)timeout_ms);
    if (status == 0) {
        *frames_read = 0u;
        return UM_ERR_TIMEOUT;
    }
    if (status < 0) {
        status = snd_pcm_recover(audio->capture, status, 1);
        if (status < 0) {
            return UM_ERR_AUDIO;
        }
    }
    count = snd_pcm_readi(audio->capture, converted, request);
    if (count == -EAGAIN) {
        *frames_read = 0u;
        return UM_ERR_TIMEOUT;
    }
    if (count < 0) {
        status = snd_pcm_recover(audio->capture, (int)count, 1);
        if (status < 0) {
            audio_log(audio->logger, audio->logger_context,
                      "Audio capture failed: %s", snd_strerror(status));
            return UM_ERR_AUDIO;
        }
        *frames_read = 0u;
        return UM_ERR_TIMEOUT;
    }
    for (i = 0u; i < (size_t)count; ++i) {
        samples[i] = (float)converted[i] / 32768.0f;
    }
    *frames_read = (size_t)count;
    return UM_OK;
}

int um_audio_write(um_audio *audio, const float *samples, size_t frame_count)
{
    int16_t converted[1024];
    size_t offset = 0u;
    int status;
    if (audio == NULL || (frame_count != 0u && samples == NULL)) {
        return UM_ERR_ARGUMENT;
    }
    status = snd_pcm_prepare(audio->playback);
    if (status < 0) {
        return UM_ERR_AUDIO;
    }
    while (offset < frame_count) {
        size_t chunk = frame_count - offset;
        size_t i;
        size_t written = 0u;
        if (chunk > sizeof(converted) / sizeof(converted[0])) {
            chunk = sizeof(converted) / sizeof(converted[0]);
        }
        for (i = 0u; i < chunk; ++i) {
            float value = samples[offset + i];
            if (value > 0.999969f) {
                value = 0.999969f;
            } else if (value < -1.0f) {
                value = -1.0f;
            }
            converted[i] = (int16_t)(value * 32767.0f);
        }
        while (written < chunk) {
            snd_pcm_sframes_t result = snd_pcm_writei(
                audio->playback, converted + written, chunk - written);
            if (result < 0) {
                status = snd_pcm_recover(audio->playback, (int)result, 1);
                if (status < 0) {
                    audio_log(audio->logger, audio->logger_context,
                              "Audio playback failed: %s",
                              snd_strerror(status));
                    return UM_ERR_AUDIO;
                }
                continue;
            }
            written += (size_t)result;
        }
        offset += chunk;
    }
    status = snd_pcm_drain(audio->playback);
    return status < 0 ? UM_ERR_AUDIO : UM_OK;
}

#elif defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define UM_AUDIO_QUEUE_BUFFERS 4u
#define UM_AUDIO_QUEUE_FRAMES 1024u
#define UM_AUDIO_RING_FRAMES (UM_SAMPLE_RATE * 4u)

struct um_audio {
    AudioQueueRef capture;
    AudioQueueRef playback;
    AudioQueueBufferRef capture_buffers[UM_AUDIO_QUEUE_BUFFERS];
    AudioQueueBufferRef playback_buffers[UM_AUDIO_QUEUE_BUFFERS];
    int playback_busy[UM_AUDIO_QUEUE_BUFFERS];
    int16_t *ring;
    size_t ring_read;
    size_t ring_write;
    size_t ring_count;
    unsigned playback_inflight;
    int capture_enabled;
    int playback_started;
    int closing;
    int mutex_initialized;
    int condition_initialized;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
};

static int cf_string_to_utf8(CFStringRef string, char *buffer, size_t capacity)
{
    return string != NULL &&
           CFStringGetCString(string, buffer, (CFIndex)capacity,
                              kCFStringEncodingUTF8);
}

static unsigned device_channels(AudioDeviceID device,
                                AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyStreamConfiguration, scope,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0u;
    AudioBufferList *list;
    unsigned channels = 0u;
    UInt32 i;
    if (AudioObjectGetPropertyDataSize(device, &address, 0u, NULL, &size) !=
        noErr || size == 0u) {
        return 0u;
    }
    list = (AudioBufferList *)malloc(size);
    if (list == NULL) {
        return 0u;
    }
    if (AudioObjectGetPropertyData(device, &address, 0u, NULL, &size, list) ==
        noErr) {
        for (i = 0u; i < list->mNumberBuffers; ++i) {
            channels += list->mBuffers[i].mNumberChannels;
        }
    }
    free(list);
    return channels;
}

static AudioDeviceID default_device(AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address = {
        selector, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    (void)AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0u,
                                     NULL, &size, &device);
    return device;
}

int um_audio_list_devices(um_log_callback logger, void *logger_context)
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0u;
    AudioDeviceID *devices;
    size_t count;
    size_t i;
    AudioDeviceID default_input;
    AudioDeviceID default_output;
    if (logger == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0u,
                                       NULL, &size) != noErr) {
        return UM_ERR_AUDIO;
    }
    devices = (AudioDeviceID *)malloc(size);
    if (devices == NULL) {
        return UM_ERR_MEMORY;
    }
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0u,
                                   NULL, &size, devices) != noErr) {
        free(devices);
        return UM_ERR_AUDIO;
    }
    count = size / sizeof(*devices);
    default_input = default_device(kAudioHardwarePropertyDefaultInputDevice);
    default_output = default_device(kAudioHardwarePropertyDefaultOutputDevice);
    audio_log(logger, logger_context,
              "Audio backend: CoreAudio, format: 48000 Hz mono signed-16");
    audio_log(logger, logger_context, "Audio input devices:");
    for (i = 0u; i < count; ++i) {
        if (device_channels(devices[i], kAudioDevicePropertyScopeInput) != 0u) {
            CFStringRef name = NULL;
            CFStringRef uid = NULL;
            char name_text[256] = "CoreAudio input";
            char uid_text[256] = "unknown";
            UInt32 value_size = sizeof(name);
            AudioObjectPropertyAddress name_address = {
                kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectPropertyAddress uid_address = {
                kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            (void)AudioObjectGetPropertyData(devices[i], &name_address, 0u,
                                             NULL, &value_size, &name);
            value_size = sizeof(uid);
            (void)AudioObjectGetPropertyData(devices[i], &uid_address, 0u,
                                             NULL, &value_size, &uid);
            (void)cf_string_to_utf8(name, name_text, sizeof(name_text));
            (void)cf_string_to_utf8(uid, uid_text, sizeof(uid_text));
            audio_log(logger, logger_context, "  %s%s | %s", uid_text,
                      devices[i] == default_input ? " [default]" : "",
                      name_text);
            if (name != NULL) {
                CFRelease(name);
            }
            if (uid != NULL) {
                CFRelease(uid);
            }
        }
    }
    audio_log(logger, logger_context, "Audio output devices:");
    for (i = 0u; i < count; ++i) {
        if (device_channels(devices[i], kAudioDevicePropertyScopeOutput) != 0u) {
            CFStringRef name = NULL;
            CFStringRef uid = NULL;
            char name_text[256] = "CoreAudio output";
            char uid_text[256] = "unknown";
            UInt32 value_size = sizeof(name);
            AudioObjectPropertyAddress name_address = {
                kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectPropertyAddress uid_address = {
                kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            (void)AudioObjectGetPropertyData(devices[i], &name_address, 0u,
                                             NULL, &value_size, &name);
            value_size = sizeof(uid);
            (void)AudioObjectGetPropertyData(devices[i], &uid_address, 0u,
                                             NULL, &value_size, &uid);
            (void)cf_string_to_utf8(name, name_text, sizeof(name_text));
            (void)cf_string_to_utf8(uid, uid_text, sizeof(uid_text));
            audio_log(logger, logger_context, "  %s%s | %s", uid_text,
                      devices[i] == default_output ? " [default]" : "",
                      name_text);
            if (name != NULL) {
                CFRelease(name);
            }
            if (uid != NULL) {
                CFRelease(uid);
            }
        }
    }
    free(devices);
    return UM_OK;
}

static void capture_callback(void *context, AudioQueueRef queue,
                             AudioQueueBufferRef buffer,
                             const AudioTimeStamp *start_time,
                             UInt32 packet_count,
                             const AudioStreamPacketDescription *descriptions)
{
    um_audio *audio = (um_audio *)context;
    const int16_t *samples = (const int16_t *)buffer->mAudioData;
    size_t count = buffer->mAudioDataByteSize / sizeof(*samples);
    size_t i;
    int reenqueue;
    (void)start_time;
    (void)packet_count;
    (void)descriptions;
    pthread_mutex_lock(&audio->mutex);
    if (audio->capture_enabled != 0) {
        for (i = 0u; i < count; ++i) {
            if (audio->ring_count == UM_AUDIO_RING_FRAMES) {
                audio->ring_read =
                    (audio->ring_read + 1u) % UM_AUDIO_RING_FRAMES;
                --audio->ring_count;
            }
            audio->ring[audio->ring_write] = samples[i];
            audio->ring_write =
                (audio->ring_write + 1u) % UM_AUDIO_RING_FRAMES;
            ++audio->ring_count;
        }
        pthread_cond_broadcast(&audio->condition);
    }
    reenqueue = audio->closing == 0;
    pthread_mutex_unlock(&audio->mutex);
    if (reenqueue != 0) {
        (void)AudioQueueEnqueueBuffer(queue, buffer, 0u, NULL);
    }
}

static void playback_callback(void *context, AudioQueueRef queue,
                              AudioQueueBufferRef buffer)
{
    um_audio *audio = (um_audio *)context;
    unsigned i;
    (void)queue;
    pthread_mutex_lock(&audio->mutex);
    for (i = 0u; i < UM_AUDIO_QUEUE_BUFFERS; ++i) {
        if (audio->playback_buffers[i] == buffer) {
            audio->playback_busy[i] = 0;
            break;
        }
    }
    if (audio->playback_inflight != 0u) {
        --audio->playback_inflight;
    }
    pthread_cond_broadcast(&audio->condition);
    pthread_mutex_unlock(&audio->mutex);
}

static int set_queue_device(AudioQueueRef queue, const char *uid)
{
    CFStringRef value;
    OSStatus status;
    if (uid == NULL || *uid == '\0' || strcmp(uid, "default") == 0) {
        return UM_OK;
    }
    value = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    if (value == NULL) {
        return UM_ERR_ARGUMENT;
    }
    status = AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice,
                                   &value, sizeof(value));
    CFRelease(value);
    return status == noErr ? UM_OK : UM_ERR_AUDIO;
}

int um_audio_open(um_audio **audio, const char *input_device,
                  const char *output_device, um_log_callback logger,
                  void *logger_context)
{
    AudioStreamBasicDescription format;
    um_audio *opened;
    unsigned i;
    if (audio == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if (input_device == NULL || *input_device == '\0') {
        input_device = "default";
    }
    if (output_device == NULL || *output_device == '\0') {
        output_device = "default";
    }
    opened = (um_audio *)calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        return UM_ERR_MEMORY;
    }
    opened->ring =
        (int16_t *)malloc(UM_AUDIO_RING_FRAMES * sizeof(*opened->ring));
    if (opened->ring == NULL) {
        um_audio_close(opened);
        return UM_ERR_MEMORY;
    }
    if (pthread_mutex_init(&opened->mutex, NULL) != 0) {
        um_audio_close(opened);
        return UM_ERR_MEMORY;
    }
    opened->mutex_initialized = 1;
    if (pthread_cond_init(&opened->condition, NULL) != 0) {
        um_audio_close(opened);
        return UM_ERR_MEMORY;
    }
    opened->condition_initialized = 1;
    audio_log(logger, logger_context, "Selected audio input:  %s", input_device);
    audio_log(logger, logger_context, "Selected audio output: %s", output_device);
    memset(&format, 0, sizeof(format));
    format.mSampleRate = UM_SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger |
                          kLinearPCMFormatFlagIsPacked |
                          kAudioFormatFlagsNativeEndian;
    format.mBytesPerPacket = 2u;
    format.mFramesPerPacket = 1u;
    format.mBytesPerFrame = 2u;
    format.mChannelsPerFrame = 1u;
    format.mBitsPerChannel = 16u;
    if (AudioQueueNewInput(&format, capture_callback, opened, NULL, NULL, 0u,
                           &opened->capture) != noErr ||
        set_queue_device(opened->capture, input_device) != UM_OK ||
        AudioQueueNewOutput(&format, playback_callback, opened, NULL, NULL, 0u,
                            &opened->playback) != noErr ||
        set_queue_device(opened->playback, output_device) != UM_OK) {
        audio_log(logger, logger_context,
                  "Cannot open selected CoreAudio input/output devices");
        um_audio_close(opened);
        return UM_ERR_AUDIO;
    }
    for (i = 0u; i < UM_AUDIO_QUEUE_BUFFERS; ++i) {
        if (AudioQueueAllocateBuffer(opened->capture,
                                     UM_AUDIO_QUEUE_FRAMES * 2u,
                                     &opened->capture_buffers[i]) != noErr ||
            AudioQueueEnqueueBuffer(opened->capture,
                                    opened->capture_buffers[i], 0u,
                                    NULL) != noErr ||
            AudioQueueAllocateBuffer(opened->playback,
                                     UM_AUDIO_QUEUE_FRAMES * 2u,
                                     &opened->playback_buffers[i]) != noErr) {
            um_audio_close(opened);
            return UM_ERR_AUDIO;
        }
    }
    pthread_mutex_lock(&opened->mutex);
    opened->capture_enabled = 1;
    pthread_mutex_unlock(&opened->mutex);
    if (AudioQueueStart(opened->capture, NULL) != noErr) {
        pthread_mutex_lock(&opened->mutex);
        opened->capture_enabled = 0;
        pthread_mutex_unlock(&opened->mutex);
        um_audio_close(opened);
        return UM_ERR_AUDIO;
    }
    *audio = opened;
    return UM_OK;
}

void um_audio_close(um_audio *audio)
{
    if (audio == NULL) {
        return;
    }
    if (audio->mutex_initialized != 0) {
        pthread_mutex_lock(&audio->mutex);
        audio->closing = 1;
        pthread_mutex_unlock(&audio->mutex);
    }
    if (audio->capture != NULL) {
        (void)AudioQueueDispose(audio->capture, true);
    }
    if (audio->playback != NULL) {
        (void)AudioQueueDispose(audio->playback, true);
    }
    if (audio->condition_initialized != 0) {
        pthread_cond_destroy(&audio->condition);
    }
    if (audio->mutex_initialized != 0) {
        pthread_mutex_destroy(&audio->mutex);
    }
    free(audio->ring);
    free(audio);
}

int um_audio_capture_enable(um_audio *audio, int enabled)
{
    OSStatus status;
    if (audio == NULL) {
        return UM_ERR_ARGUMENT;
    }
    if ((enabled != 0) == (audio->capture_enabled != 0)) {
        return UM_OK;
    }
    if (enabled == 0) {
        pthread_mutex_lock(&audio->mutex);
        audio->capture_enabled = 0;
        pthread_mutex_unlock(&audio->mutex);
        status = AudioQueuePause(audio->capture);
    } else {
        pthread_mutex_lock(&audio->mutex);
        audio->ring_read = 0u;
        audio->ring_write = 0u;
        audio->ring_count = 0u;
        audio->capture_enabled = 1;
        pthread_mutex_unlock(&audio->mutex);
        status = AudioQueueStart(audio->capture, NULL);
        if (status != noErr) {
            pthread_mutex_lock(&audio->mutex);
            audio->capture_enabled = 0;
            pthread_mutex_unlock(&audio->mutex);
        }
    }
    return status == noErr ? UM_OK : UM_ERR_AUDIO;
}

int um_audio_flush_capture(um_audio *audio)
{
    if (audio == NULL) {
        return UM_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&audio->mutex);
    audio->ring_read = 0u;
    audio->ring_write = 0u;
    audio->ring_count = 0u;
    pthread_mutex_unlock(&audio->mutex);
    return UM_OK;
}

int um_audio_read(um_audio *audio, float *samples, size_t capacity,
                  unsigned timeout_ms, size_t *frames_read)
{
    struct timespec deadline;
    size_t count;
    size_t i;
    int wait_status = 0;
    if (audio == NULL || samples == NULL || capacity == 0u ||
        frames_read == NULL || audio->capture_enabled == 0) {
        return UM_ERR_ARGUMENT;
    }
    (void)timespec_get(&deadline, TIME_UTC);
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&audio->mutex);
    while (audio->ring_count == 0u && wait_status == 0) {
        wait_status = pthread_cond_timedwait(&audio->condition, &audio->mutex,
                                             &deadline);
    }
    if (audio->ring_count == 0u) {
        pthread_mutex_unlock(&audio->mutex);
        *frames_read = 0u;
        return UM_ERR_TIMEOUT;
    }
    count = capacity < audio->ring_count ? capacity : audio->ring_count;
    for (i = 0u; i < count; ++i) {
        samples[i] = (float)audio->ring[audio->ring_read] / 32768.0f;
        audio->ring_read = (audio->ring_read + 1u) % UM_AUDIO_RING_FRAMES;
    }
    audio->ring_count -= count;
    pthread_mutex_unlock(&audio->mutex);
    *frames_read = count;
    return UM_OK;
}

int um_audio_write(um_audio *audio, const float *samples, size_t frame_count)
{
    size_t offset = 0u;
    if (audio == NULL || (frame_count != 0u && samples == NULL)) {
        return UM_ERR_ARGUMENT;
    }
    while (offset < frame_count) {
        unsigned selected = UM_AUDIO_QUEUE_BUFFERS;
        size_t chunk = frame_count - offset;
        int16_t *destination;
        size_t i;
        if (chunk > UM_AUDIO_QUEUE_FRAMES) {
            chunk = UM_AUDIO_QUEUE_FRAMES;
        }
        pthread_mutex_lock(&audio->mutex);
        while (selected == UM_AUDIO_QUEUE_BUFFERS) {
            for (i = 0u; i < UM_AUDIO_QUEUE_BUFFERS; ++i) {
                if (audio->playback_busy[i] == 0) {
                    selected = (unsigned)i;
                    break;
                }
            }
            if (selected == UM_AUDIO_QUEUE_BUFFERS) {
                pthread_cond_wait(&audio->condition, &audio->mutex);
            }
        }
        audio->playback_busy[selected] = 1;
        ++audio->playback_inflight;
        pthread_mutex_unlock(&audio->mutex);
        destination = (int16_t *)audio->playback_buffers[selected]->mAudioData;
        for (i = 0u; i < chunk; ++i) {
            float value = samples[offset + i];
            if (value > 0.999969f) {
                value = 0.999969f;
            } else if (value < -1.0f) {
                value = -1.0f;
            }
            destination[i] = (int16_t)(value * 32767.0f);
        }
        audio->playback_buffers[selected]->mAudioDataByteSize =
            (UInt32)(chunk * sizeof(*destination));
        if (AudioQueueEnqueueBuffer(audio->playback,
                                    audio->playback_buffers[selected], 0u,
                                    NULL) != noErr) {
            return UM_ERR_AUDIO;
        }
        if (audio->playback_started == 0) {
            if (AudioQueueStart(audio->playback, NULL) != noErr) {
                return UM_ERR_AUDIO;
            }
            audio->playback_started = 1;
        }
        offset += chunk;
    }
    pthread_mutex_lock(&audio->mutex);
    while (audio->playback_inflight != 0u) {
        pthread_cond_wait(&audio->condition, &audio->mutex);
    }
    pthread_mutex_unlock(&audio->mutex);
    if (audio->playback_started != 0) {
        unsigned polls;
        if (AudioQueueStop(audio->playback, false) != noErr) {
            return UM_ERR_AUDIO;
        }
        for (polls = 0u; polls < 2000u; ++polls) {
            UInt32 property_size = sizeof(UInt32);
            UInt32 running = 0u;
            struct timespec pause = {0, 5000000L};
            if (AudioQueueGetProperty(audio->playback,
                                      kAudioQueueProperty_IsRunning,
                                      &running, &property_size) != noErr) {
                return UM_ERR_AUDIO;
            }
            if (running == 0u) {
                break;
            }
            (void)nanosleep(&pause, NULL);
        }
        if (polls == 2000u) {
            return UM_ERR_TIMEOUT;
        }
        pthread_mutex_lock(&audio->mutex);
        memset(audio->playback_busy, 0, sizeof(audio->playback_busy));
        audio->playback_inflight = 0u;
        audio->playback_started = 0;
        pthread_mutex_unlock(&audio->mutex);
    }
    return UM_OK;
}

#else

struct um_audio {
    int unused;
};

int um_audio_list_devices(um_log_callback logger, void *logger_context)
{
    audio_log(logger, logger_context, "No native audio backend on this system");
    return UM_ERR_UNSUPPORTED;
}

int um_audio_open(um_audio **audio, const char *input_device,
                  const char *output_device, um_log_callback logger,
                  void *logger_context)
{
    (void)audio;
    (void)input_device;
    (void)output_device;
    (void)logger;
    (void)logger_context;
    return UM_ERR_UNSUPPORTED;
}

void um_audio_close(um_audio *audio)
{
    (void)audio;
}

int um_audio_capture_enable(um_audio *audio, int enabled)
{
    (void)audio;
    (void)enabled;
    return UM_ERR_UNSUPPORTED;
}

int um_audio_flush_capture(um_audio *audio)
{
    (void)audio;
    return UM_ERR_UNSUPPORTED;
}

int um_audio_read(um_audio *audio, float *samples, size_t capacity,
                  unsigned timeout_ms, size_t *frames_read)
{
    (void)audio;
    (void)samples;
    (void)capacity;
    (void)timeout_ms;
    (void)frames_read;
    return UM_ERR_UNSUPPORTED;
}

int um_audio_write(um_audio *audio, const float *samples, size_t frame_count)
{
    (void)audio;
    (void)samples;
    (void)frame_count;
    return UM_ERR_UNSUPPORTED;
}

#endif
