/* The part of the NDK's <android/native_window.h> the Android port's presenter uses outside its __ANDROID__ blocks
   (android/gfx/vk_draw.c, the copy path: geometry, lock, row copy, unlockAndPost), declared as in the NDK and
   implemented on HarmonyOS's OHNativeWindow by native-harmonyos/ohos/anative_window_ohos.c. An ANativeWindow * here is
   the XComponent surface's OHNativeWindow *. */
#pragma once
#include <stdint.h>

typedef struct ANativeWindow ANativeWindow;
typedef struct ARect { int32_t left, top, right, bottom; } ARect;
enum { WINDOW_FORMAT_RGBA_8888 = 1, WINDOW_FORMAT_RGBX_8888 = 2, WINDOW_FORMAT_RGB_565 = 4 };
typedef struct ANativeWindow_Buffer {
    int32_t width, height, stride, format;   /* stride in pixels */
    void *bits;
    uint32_t reserved[6];
} ANativeWindow_Buffer;

void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_getFormat(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow *window, ANativeWindow_Buffer *out_buffer, ARect *in_out_dirty_bounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow *window);

/* HarmonyOS: the surface's own size, from the XComponent (ANativeWindow_getWidth / getHeight report it). */
void anw_ohos_set_size(ANativeWindow *window, int32_t width, int32_t height);
