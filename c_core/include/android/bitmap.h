#ifndef _ANDROID_BITMAP_H
#define _ANDROID_BITMAP_H

#include <stdint.h>
#include "jni.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANDROID_BITMAP_RESULT_SUCCESS            0
#define ANDROID_BITMAP_RESULT_BAD_PARAMETER     -1
#define ANDROID_BITMAP_RESULT_JNI_EXCEPTION     -2
#define ANDROID_BITMAP_RESULT_ALLOCATION_FAILED -3

enum AndroidBitmapFormat {
    ANDROID_BITMAP_FORMAT_NONE      = 0,
    ANDROID_BITMAP_FORMAT_RGBA_8888 = 1,
    ANDROID_BITMAP_FORMAT_RGB_565   = 4,
    ANDROID_BITMAP_FORMAT_RGBA_4444 = 7,
    ANDROID_BITMAP_FORMAT_A_8       = 8,
    ANDROID_BITMAP_FORMAT_RGBA_F16  = 9,
    ANDROID_BITMAP_FORMAT_RGBA_1010102 = 10
};

typedef struct {
    uint32_t    width;
    uint32_t    height;
    uint32_t    stride;
    int32_t     format;
    uint32_t    flags;
} AndroidBitmapInfo;

int AndroidBitmap_getInfo(JNIEnv* env, jobject jbitmap, AndroidBitmapInfo* info);
int AndroidBitmap_lockPixels(JNIEnv* env, jobject jbitmap, void** addrPtr);
int AndroidBitmap_unlockPixels(JNIEnv* env, jobject jbitmap);

#ifdef __cplusplus
}
#endif

#endif /* _ANDROID_BITMAP_H */

