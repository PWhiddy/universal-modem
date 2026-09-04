#include "light_video.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIGHT_VIDEO_QUIET_MODULES 2u
#define LIGHT_VIDEO_MAX_CAMERA_SIDE 4096u
#define LIGHT_VIDEO_BUFFER_COUNT 4u

#if !defined(__APPLE__)
static void light_video_log(um_log_callback logger, void *context,
                            const char *format, ...)
{
    char message[384];
    va_list arguments;
    if (logger == NULL) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    logger(context, message);
}
#endif

static int light_video_image_fits(size_t width, size_t height, size_t stride,
                                  size_t capacity)
{
    return width != 0u && height != 0u && stride >= width &&
           height - 1u <= (SIZE_MAX - width) / stride &&
           (height - 1u) * stride + width <= capacity;
}

int um_light_rasterize_modules(const uint8_t *modules, size_t module_count,
                               uint8_t *pixels, size_t width, size_t height,
                               size_t stride, size_t pixel_capacity)
{
    const size_t symbol_side =
        UM_LIGHT_GRID_SIZE + 2u * LIGHT_VIDEO_QUIET_MODULES;
    size_t side;
    size_t origin_x;
    size_t origin_y;
    size_t y;
    if (modules == NULL ||
        module_count != UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE ||
        pixels == NULL || width < symbol_side || height < symbol_side ||
        !light_video_image_fits(width, height, stride, pixel_capacity)) {
        return UM_ERR_ARGUMENT;
    }
    side = width < height ? width : height;
    origin_x = (width - side) / 2u;
    origin_y = (height - side) / 2u;
    for (y = 0u; y < height; ++y) {
        memset(&pixels[y * stride], 255, width);
    }
    for (y = origin_y; y < origin_y + side; ++y) {
        size_t symbol_y = (y - origin_y) * symbol_side / side;
        size_t x;
        if (symbol_y < LIGHT_VIDEO_QUIET_MODULES ||
            symbol_y >= LIGHT_VIDEO_QUIET_MODULES + UM_LIGHT_GRID_SIZE) {
            continue;
        }
        for (x = origin_x; x < origin_x + side; ++x) {
            size_t symbol_x = (x - origin_x) * symbol_side / side;
            size_t module_x;
            size_t module_y;
            if (symbol_x < LIGHT_VIDEO_QUIET_MODULES ||
                symbol_x >=
                    LIGHT_VIDEO_QUIET_MODULES + UM_LIGHT_GRID_SIZE) {
                continue;
            }
            module_x = symbol_x - LIGHT_VIDEO_QUIET_MODULES;
            module_y = symbol_y - LIGHT_VIDEO_QUIET_MODULES;
            if (modules[module_y * UM_LIGHT_GRID_SIZE + module_x] != 0u) {
                pixels[y * stride + x] = 0u;
            }
        }
    }
    return UM_OK;
}

int um_light_capture_to_gray(unsigned format, const uint8_t *source,
                             size_t source_length, size_t width,
                             size_t height, size_t source_stride,
                             uint8_t *destination,
                             size_t destination_capacity)
{
    size_t required_source_width;
    size_t y;
    if (format == UM_LIGHT_CAPTURE_GRAY8) {
        required_source_width = width;
    } else if (format == UM_LIGHT_CAPTURE_YUYV && (width & 1u) == 0u) {
        if (width > SIZE_MAX / 2u) {
            return UM_ERR_ARGUMENT;
        }
        required_source_width = 2u * width;
    } else if (format == UM_LIGHT_CAPTURE_BGRA32) {
        if (width > SIZE_MAX / 4u) {
            return UM_ERR_ARGUMENT;
        }
        required_source_width = 4u * width;
    } else {
        return UM_ERR_UNSUPPORTED;
    }
    if (source == NULL || destination == NULL || width == 0u ||
        height == 0u || source_stride < required_source_width ||
        !light_video_image_fits(required_source_width, height,
                                source_stride, source_length) ||
        width > SIZE_MAX / height) {
        return UM_ERR_ARGUMENT;
    }
    if (destination_capacity < width * height) {
        return UM_ERR_CAPACITY;
    }
    for (y = 0u; y < height; ++y) {
        const uint8_t *row = &source[y * source_stride];
        uint8_t *output = &destination[y * width];
        if (format == UM_LIGHT_CAPTURE_GRAY8) {
            memcpy(output, row, width);
        } else if (format == UM_LIGHT_CAPTURE_YUYV) {
            size_t x;
            for (x = 0u; x < width; ++x) {
                output[x] = row[2u * x];
            }
        } else {
            size_t x;
            for (x = 0u; x < width; ++x) {
                unsigned blue = row[4u * x];
                unsigned green = row[4u * x + 1u];
                unsigned red = row[4u * x + 2u];
                output[x] = (uint8_t)((29u * blue + 150u * green +
                                       77u * red + 128u) >> 8u);
            }
        }
    }
    return UM_OK;
}

um_light_video_config um_light_video_default_config(void)
{
    um_light_video_config config;
    config.camera_device = "default";
    config.camera_width = 1280u;
    config.camera_height = 720u;
    config.frames_per_second = 15u;
    config.window_size = 720u;
    return config;
}

#if defined(__linux__)

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(UM_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif

typedef struct {
    void *address;
    size_t length;
} light_video_buffer;

struct um_light_video {
    int camera_fd;
    unsigned camera_width;
    unsigned camera_height;
    size_t camera_stride;
    unsigned capture_format;
    enum v4l2_buf_type buffer_type;
    light_video_buffer *buffers;
    size_t buffer_count;
    int streaming;
    int close_requested;
    um_log_callback logger;
    void *logger_context;
#if defined(UM_HAVE_X11)
    Display *display;
    int screen;
    Window window;
    GC graphics;
    Atom delete_message;
    unsigned window_width;
    unsigned window_height;
#endif
};

static int light_video_ioctl(int descriptor, unsigned long request,
                             void *argument)
{
    int status;
    do {
        status = ioctl(descriptor, request, argument);
    } while (status < 0 && errno == EINTR);
    return status;
}

static uint32_t light_video_capabilities(const struct v4l2_capability *cap)
{
    return (cap->capabilities & V4L2_CAP_DEVICE_CAPS) != 0u
               ? cap->device_caps
               : cap->capabilities;
}

int um_light_video_list_devices(um_log_callback logger, void *logger_context)
{
    unsigned index;
    unsigned found = 0u;
    if (logger == NULL) {
        return UM_ERR_ARGUMENT;
    }
    light_video_log(logger, logger_context,
                    "Video capture backend: V4L2 gray/YUYV");
    light_video_log(logger, logger_context, "Video input devices:");
    for (index = 0u; index < 64u; ++index) {
        char path[32];
        struct v4l2_capability capability;
        int descriptor;
        (void)snprintf(path, sizeof(path), "/dev/video%u", index);
        descriptor = open(path, O_RDONLY | O_NONBLOCK);
        if (descriptor < 0) {
            continue;
        }
        memset(&capability, 0, sizeof(capability));
        if (light_video_ioctl(descriptor, VIDIOC_QUERYCAP,
                              &capability) == 0 &&
            (light_video_capabilities(&capability) &
             V4L2_CAP_VIDEO_CAPTURE) != 0u) {
            light_video_log(logger, logger_context, "  %s | %s (%s)", path,
                            (const char *)capability.card,
                            (const char *)capability.bus_info);
            ++found;
        }
        (void)close(descriptor);
    }
    if (found == 0u) {
        light_video_log(logger, logger_context,
                        "  no V4L2 capture devices found");
    }
#if defined(UM_HAVE_X11)
    light_video_log(logger, logger_context,
                    "Video output backend: X11 default display");
#elif !defined(__APPLE__)
    light_video_log(logger, logger_context,
                    "Video output backend: unavailable (X11 headers missing)");
#endif
    return UM_OK;
}

#if defined(UM_HAVE_X11)
static int light_video_set_capture_format(um_light_video *video,
                                          const um_light_video_config *config)
{
    static const struct {
        uint32_t v4l2_format;
        unsigned capture_format;
    } formats[] = {
        {V4L2_PIX_FMT_GREY, UM_LIGHT_CAPTURE_GRAY8},
        {V4L2_PIX_FMT_YUYV, UM_LIGHT_CAPTURE_YUYV}
    };
    size_t index;
    for (index = 0u; index < sizeof(formats) / sizeof(formats[0]); ++index) {
        struct v4l2_format format;
        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = config->camera_width;
        format.fmt.pix.height = config->camera_height;
        format.fmt.pix.pixelformat = formats[index].v4l2_format;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (light_video_ioctl(video->camera_fd, VIDIOC_S_FMT, &format) == 0 &&
            format.fmt.pix.pixelformat == formats[index].v4l2_format &&
            format.fmt.pix.width != 0u && format.fmt.pix.height != 0u) {
            size_t minimum_stride =
                formats[index].capture_format == UM_LIGHT_CAPTURE_GRAY8
                    ? format.fmt.pix.width
                    : 2u * format.fmt.pix.width;
            video->camera_width = format.fmt.pix.width;
            video->camera_height = format.fmt.pix.height;
            video->camera_stride = format.fmt.pix.bytesperline;
            if (video->camera_stride < minimum_stride) {
                video->camera_stride = minimum_stride;
            }
            video->capture_format = formats[index].capture_format;
            return UM_OK;
        }
    }
    return UM_ERR_UNSUPPORTED;
}

static void light_video_set_frame_rate(um_light_video *video,
                                       unsigned frames_per_second)
{
    struct v4l2_streamparm parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1u;
    parameters.parm.capture.timeperframe.denominator = frames_per_second;
    (void)light_video_ioctl(video->camera_fd, VIDIOC_S_PARM, &parameters);
}

static int light_video_map_buffers(um_light_video *video)
{
    struct v4l2_requestbuffers request;
    size_t index;
    memset(&request, 0, sizeof(request));
    request.count = LIGHT_VIDEO_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (light_video_ioctl(video->camera_fd, VIDIOC_REQBUFS, &request) < 0 ||
        request.count < 2u) {
        return UM_ERR_VIDEO;
    }
    video->buffers = (light_video_buffer *)calloc(
        request.count, sizeof(*video->buffers));
    if (video->buffers == NULL) {
        return UM_ERR_MEMORY;
    }
    video->buffer_count = request.count;
    for (index = 0u; index < video->buffer_count; ++index) {
        struct v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = (uint32_t)index;
        if (light_video_ioctl(video->camera_fd, VIDIOC_QUERYBUF, &buffer) <
            0) {
            return UM_ERR_VIDEO;
        }
        video->buffers[index].length = buffer.length;
        video->buffers[index].address = mmap(
            NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
            video->camera_fd, (off_t)buffer.m.offset);
        if (video->buffers[index].address == MAP_FAILED) {
            video->buffers[index].address = NULL;
            return UM_ERR_VIDEO;
        }
        if (light_video_ioctl(video->camera_fd, VIDIOC_QBUF, &buffer) < 0) {
            return UM_ERR_VIDEO;
        }
    }
    video->buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (light_video_ioctl(video->camera_fd, VIDIOC_STREAMON,
                          &video->buffer_type) < 0) {
        return UM_ERR_VIDEO;
    }
    video->streaming = 1;
    return UM_OK;
}

static void light_video_pump_events(um_light_video *video)
{
    while (XPending(video->display) != 0) {
        XEvent event;
        XNextEvent(video->display, &event);
        if (event.type == ClientMessage &&
            (Atom)event.xclient.data.l[0] == video->delete_message) {
            video->close_requested = 1;
        } else if (event.type == KeyPress) {
            KeySym key = XLookupKeysym(&event.xkey, 0);
            if (key == XK_Escape || key == XK_q || key == XK_Q) {
                video->close_requested = 1;
            }
        } else if (event.type == ConfigureNotify) {
            video->window_width = (unsigned)event.xconfigure.width;
            video->window_height = (unsigned)event.xconfigure.height;
        }
    }
}

static int light_video_open_window(um_light_video *video,
                                   unsigned window_size)
{
    unsigned long black;
    unsigned long white;
    video->display = XOpenDisplay(NULL);
    if (video->display == NULL) {
        return UM_ERR_VIDEO;
    }
    video->screen = DefaultScreen(video->display);
    black = BlackPixel(video->display, video->screen);
    white = WhitePixel(video->display, video->screen);
    video->window = XCreateSimpleWindow(
        video->display, RootWindow(video->display, video->screen), 0, 0,
        window_size, window_size, 0u, black, white);
    if (video->window == 0u) {
        return UM_ERR_VIDEO;
    }
    XStoreName(video->display, video->window,
               "Universal Modem optical output");
    XSelectInput(video->display, video->window,
                 ExposureMask | KeyPressMask | StructureNotifyMask);
    video->delete_message = XInternAtom(video->display,
                                        "WM_DELETE_WINDOW", False);
    (void)XSetWMProtocols(video->display, video->window,
                          &video->delete_message, 1);
    video->graphics = XCreateGC(video->display, video->window, 0u, NULL);
    if (video->graphics == NULL) {
        return UM_ERR_VIDEO;
    }
    video->window_width = window_size;
    video->window_height = window_size;
    XMapRaised(video->display, video->window);
    XSync(video->display, False);
    return UM_OK;
}
#endif

int um_light_video_open(um_light_video **video,
                        const um_light_video_config *config,
                        um_log_callback logger, void *logger_context)
{
    if (video == NULL || config == NULL || config->camera_device == NULL ||
        config->camera_width < UM_LIGHT_GRID_SIZE ||
        config->camera_height < UM_LIGHT_GRID_SIZE ||
        config->camera_width > LIGHT_VIDEO_MAX_CAMERA_SIDE ||
        config->camera_height > LIGHT_VIDEO_MAX_CAMERA_SIDE ||
        config->frames_per_second == 0u ||
        config->frames_per_second > 120u || config->window_size < 256u ||
        config->window_size > 4096u) {
        return UM_ERR_ARGUMENT;
    }
    *video = NULL;
#if !defined(UM_HAVE_X11)
    (void)logger;
    (void)logger_context;
    return UM_ERR_UNSUPPORTED;
#else
    const char *device;
    struct v4l2_capability capability;
    um_light_video *opened;
    int status;
    opened = (um_light_video *)calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        return UM_ERR_MEMORY;
    }
    opened->camera_fd = -1;
    opened->logger = logger;
    opened->logger_context = logger_context;
    device = strcmp(config->camera_device, "default") == 0
                 ? "/dev/video0"
                 : config->camera_device;
    opened->camera_fd = open(device, O_RDWR | O_NONBLOCK);
    if (opened->camera_fd < 0) {
        light_video_log(logger, logger_context,
                        "Cannot open camera '%s': %s", device,
                        strerror(errno));
        status = UM_ERR_VIDEO;
        goto failed;
    }
    memset(&capability, 0, sizeof(capability));
    if (light_video_ioctl(opened->camera_fd, VIDIOC_QUERYCAP,
                          &capability) < 0 ||
        (light_video_capabilities(&capability) &
         (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING)) !=
            (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING)) {
        light_video_log(logger, logger_context,
                        "Camera '%s' does not support streaming capture",
                        device);
        status = UM_ERR_VIDEO;
        goto failed;
    }
    status = light_video_set_capture_format(opened, config);
    if (status != UM_OK) {
        light_video_log(logger, logger_context,
                        "Camera '%s' offers neither gray8 nor YUYV", device);
        goto failed;
    }
    light_video_set_frame_rate(opened, config->frames_per_second);
    status = light_video_map_buffers(opened);
    if (status != UM_OK) {
        light_video_log(logger, logger_context,
                        "Could not start camera '%s' streaming", device);
        goto failed;
    }
    status = light_video_open_window(opened, config->window_size);
    if (status != UM_OK) {
        light_video_log(logger, logger_context,
                        "Could not open the X11 optical output window");
        goto failed;
    }
    light_video_log(logger, logger_context,
                    "Opened camera %s: %ux%u %s at requested %u fps",
                    device, opened->camera_width, opened->camera_height,
                    opened->capture_format == UM_LIGHT_CAPTURE_GRAY8
                        ? "gray8"
                        : "YUYV",
                    config->frames_per_second);
    *video = opened;
    return UM_OK;

failed:
    um_light_video_close(opened);
    return status;
#endif
}

void um_light_video_close(um_light_video *video)
{
    size_t index;
    if (video == NULL) {
        return;
    }
#if defined(UM_HAVE_X11)
    if (video->display != NULL && video->graphics != NULL) {
        XFreeGC(video->display, video->graphics);
    }
    if (video->display != NULL && video->window != 0u) {
        XDestroyWindow(video->display, video->window);
    }
    if (video->display != NULL) {
        XCloseDisplay(video->display);
    }
#endif
    if (video->camera_fd >= 0 && video->streaming != 0) {
        (void)light_video_ioctl(video->camera_fd, VIDIOC_STREAMOFF,
                                &video->buffer_type);
    }
    for (index = 0u; index < video->buffer_count; ++index) {
        if (video->buffers[index].address != NULL) {
            (void)munmap(video->buffers[index].address,
                         video->buffers[index].length);
        }
    }
    free(video->buffers);
    if (video->camera_fd >= 0) {
        (void)close(video->camera_fd);
    }
    free(video);
}

int um_light_video_present(um_light_video *video, const uint8_t *modules,
                           size_t module_count)
{
    if (video == NULL || modules == NULL ||
        module_count != UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE) {
        return UM_ERR_ARGUMENT;
    }
#if !defined(UM_HAVE_X11)
    return UM_ERR_UNSUPPORTED;
#else
    size_t row;
    light_video_pump_events(video);
    if (video->close_requested != 0) {
        return UM_ERR_INTERRUPTED;
    }
    XSetForeground(video->display, video->graphics,
                   WhitePixel(video->display, video->screen));
    XFillRectangle(video->display, video->window, video->graphics, 0, 0,
                   video->window_width, video->window_height);
    XSetForeground(video->display, video->graphics,
                   BlackPixel(video->display, video->screen));
    for (row = 0u; row < UM_LIGHT_GRID_SIZE; ++row) {
        size_t column = 0u;
        while (column < UM_LIGHT_GRID_SIZE) {
            size_t start;
            size_t side;
            size_t origin_x;
            size_t origin_y;
            size_t x0;
            size_t x1;
            size_t y0;
            size_t y1;
            while (column < UM_LIGHT_GRID_SIZE &&
                   modules[row * UM_LIGHT_GRID_SIZE + column] == 0u) {
                ++column;
            }
            if (column == UM_LIGHT_GRID_SIZE) {
                break;
            }
            start = column;
            while (column < UM_LIGHT_GRID_SIZE &&
                   modules[row * UM_LIGHT_GRID_SIZE + column] != 0u) {
                ++column;
            }
            side = video->window_width < video->window_height
                       ? video->window_width
                       : video->window_height;
            origin_x = (video->window_width - side) / 2u;
            origin_y = (video->window_height - side) / 2u;
            x0 = origin_x +
                 (start + LIGHT_VIDEO_QUIET_MODULES) * side /
                     (UM_LIGHT_GRID_SIZE + 2u * LIGHT_VIDEO_QUIET_MODULES);
            x1 = origin_x +
                 (column + LIGHT_VIDEO_QUIET_MODULES) * side /
                     (UM_LIGHT_GRID_SIZE + 2u * LIGHT_VIDEO_QUIET_MODULES);
            y0 = origin_y +
                 (row + LIGHT_VIDEO_QUIET_MODULES) * side /
                     (UM_LIGHT_GRID_SIZE + 2u * LIGHT_VIDEO_QUIET_MODULES);
            y1 = origin_y +
                 (row + LIGHT_VIDEO_QUIET_MODULES + 1u) * side /
                     (UM_LIGHT_GRID_SIZE + 2u * LIGHT_VIDEO_QUIET_MODULES);
            XFillRectangle(video->display, video->window, video->graphics,
                           (int)x0, (int)y0, (unsigned)(x1 - x0),
                           (unsigned)(y1 - y0));
        }
    }
    XFlush(video->display);
    return UM_OK;
#endif
}

int um_light_video_capture(um_light_video *video, uint8_t *pixels,
                           size_t pixel_capacity, unsigned timeout_ms,
                           size_t *width, size_t *height)
{
    struct pollfd poll_descriptor;
    struct v4l2_buffer buffer;
    int poll_status;
    int status;
    if (video == NULL || pixels == NULL || width == NULL || height == NULL ||
        timeout_ms > 60000u) {
        return UM_ERR_ARGUMENT;
    }
#if defined(UM_HAVE_X11)
    light_video_pump_events(video);
    if (video->close_requested != 0) {
        return UM_ERR_INTERRUPTED;
    }
#endif
    poll_descriptor.fd = video->camera_fd;
    poll_descriptor.events = POLLIN;
    poll_descriptor.revents = 0;
    do {
        poll_status = poll(&poll_descriptor, 1u, (int)timeout_ms);
    } while (poll_status < 0 && errno == EINTR);
    if (poll_status == 0) {
        return UM_ERR_TIMEOUT;
    }
    if (poll_status < 0 ||
        (poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return UM_ERR_VIDEO;
    }
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (light_video_ioctl(video->camera_fd, VIDIOC_DQBUF, &buffer) < 0) {
        return errno == EAGAIN ? UM_ERR_TIMEOUT : UM_ERR_VIDEO;
    }
    if (buffer.index >= video->buffer_count ||
        buffer.bytesused > video->buffers[buffer.index].length) {
        status = UM_ERR_VIDEO;
    } else {
        status = um_light_capture_to_gray(
            video->capture_format,
            (const uint8_t *)video->buffers[buffer.index].address,
            buffer.bytesused, video->camera_width, video->camera_height,
            video->camera_stride, pixels, pixel_capacity);
    }
    if (light_video_ioctl(video->camera_fd, VIDIOC_QBUF, &buffer) < 0 &&
        status == UM_OK) {
        status = UM_ERR_VIDEO;
    }
    if (status == UM_OK) {
        *width = video->camera_width;
        *height = video->camera_height;
    }
    return status;
}

int um_light_video_should_close(um_light_video *video)
{
    if (video == NULL) {
        return 1;
    }
#if defined(UM_HAVE_X11)
    light_video_pump_events(video);
#endif
    return video->close_requested;
}

#elif !defined(__APPLE__)

struct um_light_video {
    int unused;
};

int um_light_video_list_devices(um_log_callback logger, void *logger_context)
{
    if (logger == NULL) {
        return UM_ERR_ARGUMENT;
    }
    light_video_log(logger, logger_context,
                    "Native light video I/O is not implemented on this "
                    "platform yet");
    return UM_ERR_UNSUPPORTED;
}

int um_light_video_open(um_light_video **video,
                        const um_light_video_config *config,
                        um_log_callback logger, void *logger_context)
{
    (void)config;
    (void)logger;
    (void)logger_context;
    if (video == NULL) {
        return UM_ERR_ARGUMENT;
    }
    *video = NULL;
    return UM_ERR_UNSUPPORTED;
}

void um_light_video_close(um_light_video *video)
{
    (void)video;
}

int um_light_video_present(um_light_video *video, const uint8_t *modules,
                           size_t module_count)
{
    (void)video;
    (void)modules;
    (void)module_count;
    return UM_ERR_UNSUPPORTED;
}

int um_light_video_capture(um_light_video *video, uint8_t *pixels,
                           size_t pixel_capacity, unsigned timeout_ms,
                           size_t *width, size_t *height)
{
    (void)video;
    (void)pixels;
    (void)pixel_capacity;
    (void)timeout_ms;
    (void)width;
    (void)height;
    return UM_ERR_UNSUPPORTED;
}

int um_light_video_should_close(um_light_video *video)
{
    (void)video;
    return 1;
}

#endif
