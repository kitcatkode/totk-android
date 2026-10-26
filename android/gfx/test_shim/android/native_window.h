/* The few NDK declarations the desktop presenter test needs (test_window_mock.c); the app build uses the NDK's own header. */
#pragma once
#include <stdint.h>
typedef struct ANativeWindow ANativeWindow;
typedef struct ARect { int32_t left, top, right, bottom; } ARect;
typedef struct ANativeWindow_Buffer { int32_t width, height, stride, format; void *bits; uint32_t reserved[6]; } ANativeWindow_Buffer;
enum { WINDOW_FORMAT_RGBA_8888 = 1, WINDOW_FORMAT_RGBX_8888 = 2, WINDOW_FORMAT_RGB_565 = 4 };
void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow *window, ANativeWindow_Buffer *outBuffer, ARect *inOutDirtyBounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow *window);
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
