/* A stand-in ANativeWindow for testing the app's presenter on the desktop (test_vkdraw.c, built with
   -DTOTK_ANDROID_APP): ANativeWindow_lock hands out a plain buffer, unlockAndPost counts frames. */
#include <android/native_window.h>
#include <stdlib.h>
#include <string.h>
struct ANativeWindow { int refs; };
static struct ANativeWindow g_mock = {0};
uint8_t *mock_window_pixels; int32_t mock_window_w, mock_window_h, mock_window_stride; int mock_window_posts;
ANativeWindow *mock_window(void) { return &g_mock; }
void ANativeWindow_acquire(ANativeWindow *w) { w->refs++; }
void ANativeWindow_release(ANativeWindow *w) { w->refs--; }
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format) {
    (void)w; (void)format; mock_window_w = width; mock_window_h = height; mock_window_stride = width + 7;   /* a stride wider than a row */
    free(mock_window_pixels); mock_window_pixels = calloc((size_t)mock_window_stride * height, 4); return 0;
}
int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *out, ARect *dirty) {
    (void)w; (void)dirty; if (!mock_window_pixels) return -1;
    memset(out, 0, sizeof *out); out->width = mock_window_w; out->height = mock_window_h; out->stride = mock_window_stride; out->format = 2; out->bits = mock_window_pixels; return 0;
}
int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) { (void)w; mock_window_posts++; return 0; }
int32_t ANativeWindow_getWidth(ANativeWindow *w) { (void)w; return 96; }   /* wider than the crop's aspect: black bars */
int32_t ANativeWindow_getHeight(ANativeWindow *w) { (void)w; return 32; }
void front_frame_presented(void) {}
