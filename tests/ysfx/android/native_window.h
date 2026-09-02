#pragma once

#include <cstdint>

struct ANativeWindow {};
struct ARect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};
struct ANativeWindow_Buffer {
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
    void* bits;
    uint32_t reserved[6];
};

constexpr int32_t WINDOW_FORMAT_RGBA_8888 = 1;

extern "C" {
void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window,
                                          int32_t width,
                                          int32_t height,
                                          int32_t format);
int32_t ANativeWindow_lock(ANativeWindow* window,
                           ANativeWindow_Buffer* outBuffer,
                           ARect* inOutDirtyBounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window);
}
