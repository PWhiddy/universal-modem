#import "light_video.h"

#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIGHT_MAC_QUIET_MODULES 2u

/* TCC reads this Mach-O section before allowing AVFoundation camera access.
 * Keeping it here makes the command-line build self-contained. */
__attribute__((used, section("__TEXT,__info_plist")))
static const char light_mac_info_plist[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<plist version=\"1.0\"><dict>"
    "<key>CFBundleIdentifier</key>"
    "<string>org.universal-modem.cli</string>"
    "<key>CFBundleName</key><string>Universal Modem</string>"
    "<key>CFBundlePackageType</key><string>APPL</string>"
    "<key>CFBundleShortVersionString</key><string>0.1</string>"
    "<key>CFBundleVersion</key><string>1</string>"
    "<key>NSCameraUsageDescription</key>"
    "<string>Universal Modem uses the camera to receive optical data from "
    "the peer display.</string>"
    "<key>NSHighResolutionCapable</key><true/>"
    "</dict></plist>";

@interface UMLightCaptureSink
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
    NSCondition *_condition;
    CVPixelBufferRef _latest;
    uint64_t _generation;
}
- (CVPixelBufferRef)copyFrameAfter:(uint64_t *)generation
               timeoutMilliseconds:(unsigned)timeoutMilliseconds;
@end

@implementation UMLightCaptureSink
- (instancetype)init
{
    self = [super init];
    if (self != nil) {
        _condition = [[NSCondition alloc] init];
        _latest = NULL;
        _generation = 0u;
    }
    return self;
}

- (void)dealloc
{
    if (_latest != NULL) {
        CVPixelBufferRelease(_latest);
    }
}

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection
{
    CVPixelBufferRef incoming = CMSampleBufferGetImageBuffer(sampleBuffer);
    CVPixelBufferRef previous;
    (void)output;
    (void)connection;
    if (incoming == NULL) {
        return;
    }
    CVPixelBufferRetain(incoming);
    [_condition lock];
    previous = _latest;
    _latest = incoming;
    ++_generation;
    [_condition broadcast];
    [_condition unlock];
    if (previous != NULL) {
        CVPixelBufferRelease(previous);
    }
}

- (CVPixelBufferRef)copyFrameAfter:(uint64_t *)generation
               timeoutMilliseconds:(unsigned)timeoutMilliseconds
{
    NSDate *deadline = [NSDate
        dateWithTimeIntervalSinceNow:(double)timeoutMilliseconds / 1000.0];
    CVPixelBufferRef result = NULL;
    [_condition lock];
    while (_latest == NULL || _generation <= *generation) {
        if (![_condition waitUntilDate:deadline]) {
            break;
        }
    }
    if (_latest != NULL && _generation > *generation) {
        result = CVPixelBufferRetain(_latest);
        *generation = _generation;
    }
    [_condition unlock];
    return result;
}
@end

@interface UMLightOutputView : NSView
@property(nonatomic, strong) NSData *moduleData;
- (void)showModules:(const uint8_t *)modules count:(size_t)count;
@end

@implementation UMLightOutputView
- (BOOL)isFlipped
{
    return YES;
}

- (void)showModules:(const uint8_t *)modules count:(size_t)count
{
    self.moduleData = [NSData dataWithBytes:modules length:count];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect
{
    const uint8_t *modules = self.moduleData.bytes;
    NSRect bounds = self.bounds;
    CGFloat boundsWidth = NSWidth(bounds);
    CGFloat boundsHeight = NSHeight(bounds);
    CGFloat side = boundsWidth < boundsHeight ? boundsWidth : boundsHeight;
    CGFloat originX = NSMinX(bounds) + (NSWidth(bounds) - side) / 2.0;
    CGFloat originY = NSMinY(bounds) + (NSHeight(bounds) - side) / 2.0;
    const CGFloat symbolSide =
        (CGFloat)(UM_LIGHT_GRID_SIZE + 2u * LIGHT_MAC_QUIET_MODULES);
    size_t row;
    (void)dirtyRect;
    [[NSColor whiteColor] setFill];
    NSRectFill(bounds);
    if (self.moduleData.length !=
        UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE) {
        return;
    }
    [[NSColor blackColor] setFill];
    for (row = 0u; row < UM_LIGHT_GRID_SIZE; ++row) {
        size_t column = 0u;
        while (column < UM_LIGHT_GRID_SIZE) {
            size_t start;
            CGFloat x0;
            CGFloat x1;
            CGFloat y0;
            CGFloat y1;
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
            x0 = originX +
                 (CGFloat)(start + LIGHT_MAC_QUIET_MODULES) * side /
                     symbolSide;
            x1 = originX +
                 (CGFloat)(column + LIGHT_MAC_QUIET_MODULES) * side /
                     symbolSide;
            y0 = originY +
                 (CGFloat)(row + LIGHT_MAC_QUIET_MODULES) * side /
                     symbolSide;
            y1 = originY +
                 (CGFloat)(row + LIGHT_MAC_QUIET_MODULES + 1u) * side /
                     symbolSide;
            NSRectFill(NSMakeRect(x0, y0, x1 - x0, y1 - y0));
        }
    }
}
@end

@interface UMLightVideoState : NSObject
@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic, strong) AVCaptureVideoDataOutput *output;
@property(nonatomic, strong) UMLightCaptureSink *captureSink;
@property(nonatomic, strong) dispatch_queue_t captureQueue;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) UMLightOutputView *outputView;
@end

@implementation UMLightVideoState
@end

struct um_light_video {
    void *stateReference;
    uint64_t capturedGeneration;
};

static UMLightVideoState *light_mac_state(um_light_video *video)
{
    return (__bridge UMLightVideoState *)video->stateReference;
}

static void light_mac_log(um_log_callback logger, void *context,
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

static NSArray<AVCaptureDevice *> *light_mac_camera_devices(void)
{
    NSMutableArray<AVCaptureDeviceType> *types = [NSMutableArray array];
    [types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
    AVCaptureDeviceDiscoverySession *discovery =
        [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:types
                                     mediaType:AVMediaTypeVideo
                                      position:AVCaptureDevicePositionUnspecified];
    return discovery.devices;
}

static int light_mac_camera_authorized(void)
{
    AVAuthorizationStatus authorization =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (authorization == AVAuthorizationStatusAuthorized) {
        return 1;
    }
    if (authorization != AVAuthorizationStatusNotDetermined) {
        return 0;
    }
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    __block BOOL allowed = NO;
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL granted) {
                               allowed = granted;
                               dispatch_semaphore_signal(completed);
                             }];
    (void)dispatch_semaphore_wait(completed, DISPATCH_TIME_FOREVER);
    return allowed ? 1 : 0;
}

static AVCaptureDevice *light_mac_select_camera(const char *identifier)
{
    NSArray<AVCaptureDevice *> *devices = light_mac_camera_devices();
    if (strcmp(identifier, "default") == 0) {
        return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    }
    for (AVCaptureDevice *device in devices) {
        const char *uniqueID = device.uniqueID.UTF8String;
        const char *name = device.localizedName.UTF8String;
        if ((uniqueID != NULL && strcmp(identifier, uniqueID) == 0) ||
            (name != NULL && strcmp(identifier, name) == 0)) {
            return device;
        }
    }
    return nil;
}

static void light_mac_set_frame_rate(AVCaptureDevice *device,
                                     unsigned framesPerSecond)
{
    BOOL supported = NO;
    for (AVFrameRateRange *range in
         device.activeFormat.videoSupportedFrameRateRanges) {
        if ((double)framesPerSecond >= range.minFrameRate &&
            (double)framesPerSecond <= range.maxFrameRate) {
            supported = YES;
            break;
        }
    }
    if (supported) {
        NSError *error = nil;
        if ([device lockForConfiguration:&error]) {
            CMTime duration = CMTimeMake(1, (int32_t)framesPerSecond);
            device.activeVideoMinFrameDuration = duration;
            device.activeVideoMaxFrameDuration = duration;
            [device unlockForConfiguration];
        }
    }
}

static void light_mac_pump_events(void)
{
    NSEvent *event;
    do {
        event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                   untilDate:[NSDate date]
                                      inMode:NSDefaultRunLoopMode
                                     dequeue:YES];
        if (event != nil) {
            [NSApp sendEvent:event];
        }
    } while (event != nil);
    [NSApp updateWindows];
}

static UMLightVideoState *
light_mac_create_state(const um_light_video_config *config,
                       um_log_callback logger, void *loggerContext,
                       int *status)
{
    AVCaptureDevice *device =
        light_mac_select_camera(config->camera_device);
    NSError *error = nil;
    AVCaptureDeviceInput *input;
    UMLightVideoState *state;
    NSRect rectangle;
    NSString *sessionPreset;
    const char *identifier;
    const char *name;

    *status = UM_ERR_VIDEO;
    if (device == nil) {
        light_mac_log(logger, loggerContext, "Cannot find camera '%s'",
                      config->camera_device);
        return nil;
    }
    input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (input == nil) {
        const char *description = error.localizedDescription.UTF8String;
        light_mac_log(logger, loggerContext, "Cannot open camera '%s': %s",
                      config->camera_device,
                      description != NULL ? description : "unknown error");
        return nil;
    }

    state = [[UMLightVideoState alloc] init];
    if (state == nil) {
        *status = UM_ERR_MEMORY;
        return nil;
    }
    state.session = [[AVCaptureSession alloc] init];
    state.captureSink = [[UMLightCaptureSink alloc] init];
    state.output = [[AVCaptureVideoDataOutput alloc] init];
    state.captureQueue = dispatch_queue_create(
        "org.universal-modem.light.capture", DISPATCH_QUEUE_SERIAL);
    if (state == nil || state.session == nil || state.captureSink == nil ||
        state.output == nil || state.captureQueue == nil) {
        *status = UM_ERR_MEMORY;
        return nil;
    }
    state.output.alwaysDiscardsLateVideoFrames = YES;
    state.output.videoSettings = @{
        (NSString *)kCVPixelBufferPixelFormatTypeKey :
            @(kCVPixelFormatType_32BGRA),
        (NSString *)kCVPixelBufferWidthKey : @(config->camera_width),
        (NSString *)kCVPixelBufferHeightKey : @(config->camera_height)
    };
    [state.output setSampleBufferDelegate:state.captureSink
                                    queue:state.captureQueue];
    [state.session beginConfiguration];
    if (config->camera_width >= 1920u && config->camera_height >= 1080u) {
        sessionPreset = AVCaptureSessionPreset1920x1080;
    } else if (config->camera_width >= 1280u &&
               config->camera_height >= 720u) {
        sessionPreset = AVCaptureSessionPreset1280x720;
    } else {
        sessionPreset = AVCaptureSessionPreset640x480;
    }
    if (![state.session canSetSessionPreset:sessionPreset]) {
        sessionPreset = AVCaptureSessionPresetHigh;
    }
    if ([state.session canSetSessionPreset:sessionPreset]) {
        state.session.sessionPreset = sessionPreset;
    } else {
        sessionPreset = nil;
    }
    if (![state.session canAddInput:input] ||
        ![state.session canAddOutput:state.output]) {
        [state.session commitConfiguration];
        light_mac_log(logger, loggerContext,
                      "Camera cannot provide a video-data output");
        return nil;
    }
    [state.session addInput:input];
    [state.session addOutput:state.output];
    [state.session commitConfiguration];
    light_mac_set_frame_rate(device, config->frames_per_second);

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    rectangle = NSMakeRect(0.0, 0.0, (CGFloat)config->window_size,
                           (CGFloat)config->window_size);
    state.window = [[NSWindow alloc]
        initWithContentRect:rectangle
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    state.outputView = [[UMLightOutputView alloc] initWithFrame:rectangle];
    if (state.window == nil || state.outputView == nil) {
        *status = UM_ERR_MEMORY;
        return nil;
    }
    state.window.title = @"Universal Modem optical output";
    state.window.releasedWhenClosed = NO;
    state.window.contentView = state.outputView;
    [state.window center];
    [state.window makeKeyAndOrderFront:nil];
    [state.session startRunning];
    if (!state.session.running) {
        [state.window orderOut:nil];
        [state.window close];
        light_mac_log(logger, loggerContext,
                      "AVFoundation camera session did not start");
        return nil;
    }

    identifier = device.uniqueID.UTF8String;
    name = device.localizedName.UTF8String;
    light_mac_log(logger, loggerContext,
                  "Opened camera %s (%s): requested %ux%u BGRA at %u fps "
                  "preset=%s",
                  identifier != NULL ? identifier : "unknown",
                  name != NULL ? name : "AVFoundation camera",
                  config->camera_width, config->camera_height,
                  config->frames_per_second,
                  sessionPreset != nil ? sessionPreset.UTF8String
                                       : "device-default");
    *status = UM_OK;
    return state;
}

int um_light_video_list_devices(um_log_callback logger, void *loggerContext)
{
    if (logger == NULL) {
        return UM_ERR_ARGUMENT;
    }
    @autoreleasepool {
        NSArray<AVCaptureDevice *> *devices = light_mac_camera_devices();
        light_mac_log(logger, loggerContext,
                      "Video capture backend: macOS AVFoundation BGRA");
        light_mac_log(logger, loggerContext, "Video input devices:");
        if (devices.count == 0u) {
            light_mac_log(logger, loggerContext,
                          "  no AVFoundation capture devices found");
        }
        for (AVCaptureDevice *device in devices) {
            const char *name = device.localizedName.UTF8String;
            const char *identifier = device.uniqueID.UTF8String;
            light_mac_log(logger, loggerContext, "  %s | %s",
                          identifier != NULL ? identifier : "unknown",
                          name != NULL ? name : "AVFoundation camera");
        }
        light_mac_log(logger, loggerContext,
                      "Video output backend: native Cocoa window");
    }
    return UM_OK;
}

int um_light_video_open(um_light_video **video,
                        const um_light_video_config *config,
                        um_log_callback logger, void *loggerContext)
{
    um_light_video *opened;
    int result;
    if (video == NULL || config == NULL || config->camera_device == NULL ||
        config->camera_width < UM_LIGHT_GRID_SIZE ||
        config->camera_height < UM_LIGHT_GRID_SIZE ||
        config->camera_width > 4096u || config->camera_height > 4096u ||
        config->frames_per_second == 0u ||
        config->frames_per_second > 120u || config->window_size < 256u ||
        config->window_size > 4096u) {
        return UM_ERR_ARGUMENT;
    }
    *video = NULL;
    opened = (um_light_video *)calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        return UM_ERR_MEMORY;
    }
    @autoreleasepool {
        UMLightVideoState *state;
        if (![NSThread isMainThread]) {
            light_mac_log(
                logger, loggerContext,
                "The Cocoa optical window must open on the main thread");
            result = UM_ERR_VIDEO;
        } else if (!light_mac_camera_authorized()) {
            light_mac_log(
                logger, loggerContext,
                "Camera access is not authorized for this executable");
            result = UM_ERR_VIDEO;
        } else {
            state = light_mac_create_state(config, logger, loggerContext,
                                           &result);
            if (state != nil) {
                opened->stateReference = (__bridge_retained void *)state;
            }
        }
    }
    if (result == UM_OK && opened->stateReference != NULL) {
        *video = opened;
        return UM_OK;
    }
    free(opened);
    return result;
}

void um_light_video_close(um_light_video *video)
{
    if (video == NULL) {
        return;
    }
    @autoreleasepool {
        UMLightVideoState *state = light_mac_state(video);
        if (state != nil) {
            [state.output setSampleBufferDelegate:nil queue:NULL];
            [state.session stopRunning];
            [state.window orderOut:nil];
            [state.window close];
        }
        if (video->stateReference != NULL) {
            UMLightVideoState *owned =
                (__bridge_transfer UMLightVideoState *)video->stateReference;
            video->stateReference = NULL;
            (void)owned;
        }
    }
    free(video);
}

int um_light_video_present(um_light_video *video, const uint8_t *modules,
                           size_t moduleCount)
{
    if (video == NULL || modules == NULL ||
        moduleCount != UM_LIGHT_GRID_SIZE * UM_LIGHT_GRID_SIZE) {
        return UM_ERR_ARGUMENT;
    }
    @autoreleasepool {
        UMLightVideoState *state = light_mac_state(video);
        light_mac_pump_events();
        if (!state.window.visible) {
            return UM_ERR_INTERRUPTED;
        }
        [state.outputView showModules:modules count:moduleCount];
        [state.outputView displayIfNeeded];
        [state.window displayIfNeeded];
    }
    return UM_OK;
}

int um_light_video_capture(um_light_video *video, uint8_t *pixels,
                           size_t pixelCapacity, unsigned timeoutMilliseconds,
                           size_t *width, size_t *height)
{
    int status = UM_ERR_TIMEOUT;
    if (video == NULL || pixels == NULL || width == NULL || height == NULL ||
        timeoutMilliseconds > 60000u) {
        return UM_ERR_ARGUMENT;
    }
    @autoreleasepool {
        UMLightVideoState *state = light_mac_state(video);
        CVPixelBufferRef frame;
        light_mac_pump_events();
        if (!state.window.visible) {
            return UM_ERR_INTERRUPTED;
        }
        frame = [state.captureSink
            copyFrameAfter:&video->capturedGeneration
               timeoutMilliseconds:timeoutMilliseconds];
        if (frame == NULL) {
            return UM_ERR_TIMEOUT;
        }
        if (CVPixelBufferGetPixelFormatType(frame) !=
            kCVPixelFormatType_32BGRA) {
            CVPixelBufferRelease(frame);
            return UM_ERR_UNSUPPORTED;
        }
        CVReturn lockStatus = CVPixelBufferLockBaseAddress(
            frame, kCVPixelBufferLock_ReadOnly);
        if (lockStatus == kCVReturnSuccess) {
            size_t frameWidth = CVPixelBufferGetWidth(frame);
            size_t frameHeight = CVPixelBufferGetHeight(frame);
            size_t stride = CVPixelBufferGetBytesPerRow(frame);
            const uint8_t *source =
                (const uint8_t *)CVPixelBufferGetBaseAddress(frame);
            size_t sourceLength = stride <= SIZE_MAX / frameHeight
                                      ? stride * frameHeight
                                      : 0u;
            *width = frameWidth;
            *height = frameHeight;
            status = sourceLength != 0u
                         ? um_light_capture_to_gray(
                               UM_LIGHT_CAPTURE_BGRA32, source, sourceLength,
                               frameWidth, frameHeight, stride, pixels,
                               pixelCapacity)
                         : UM_ERR_VIDEO;
            CVPixelBufferUnlockBaseAddress(frame,
                                           kCVPixelBufferLock_ReadOnly);
        } else {
            status = UM_ERR_VIDEO;
        }
        CVPixelBufferRelease(frame);
    }
    return status;
}

int um_light_video_should_close(um_light_video *video)
{
    int shouldClose;
    if (video == NULL) {
        return 1;
    }
    @autoreleasepool {
        UMLightVideoState *state = light_mac_state(video);
        light_mac_pump_events();
        shouldClose = !state.window.visible;
    }
    return shouldClose;
}
