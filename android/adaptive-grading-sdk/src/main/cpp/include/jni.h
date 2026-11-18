/**
 * @file jni.h
 * @brief 标准 JNI (Java Native Interface) 兼容头文件
 * @details 独立自包含定义，支持在无宿主 JDK 的 PC 交叉编译与模拟测试环境中编译 JNI 桥接库，
 *          完全兼容 Android NDK 标准 JNI 规范。
 */

#ifndef _JNI_H_
#define _JNI_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JNIEXPORT
#define JNICALL

#define JNI_FALSE 0
#define JNI_TRUE  1

#define JNI_OK           0
#define JNI_ERR         (-1)
#define JNI_EDETACHED   (-2)
#define JNI_EVERSION    (-3)

#define JNI_VERSION_1_1 0x00010001
#define JNI_VERSION_1_2 0x00010002
#define JNI_VERSION_1_4 0x00010004
#define JNI_VERSION_1_6 0x00010006

typedef uint8_t  jboolean;
typedef int8_t   jbyte;
typedef uint16_t jchar;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;

typedef void* jobject;
typedef jobject jclass;
typedef jobject jthrowable;
typedef jobject jstring;
typedef jobject jarray;
typedef jobject jbooleanArray;
typedef jobject jbyteArray;
typedef jobject jcharArray;
typedef jobject jshortArray;
typedef jobject jintArray;
typedef jobject jlongArray;
typedef jobject jfloatArray;
typedef jobject jdoubleArray;
typedef jobject jobjectArray;

typedef void* jfieldID;
typedef void* jmethodID;

struct JNINativeInterface;
typedef const struct JNINativeInterface* JNIEnv;

struct JNIInvokeInterface;
typedef const struct JNIInvokeInterface* JavaVM;

struct JNINativeInterface {
    void* reserved0;
    void* reserved1;
    void* reserved2;
    void* reserved3;

    jint (JNICALL *GetVersion)(JNIEnv*);
    jclass (JNICALL *FindClass)(JNIEnv*, const char*);
    jint (JNICALL *ThrowNew)(JNIEnv*, jclass, const char*);

    jstring (JNICALL *NewStringUTF)(JNIEnv*, const char*);
    const char* (JNICALL *GetStringUTFChars)(JNIEnv*, jstring, jboolean*);
    void (JNICALL *ReleaseStringUTFChars)(JNIEnv*, jstring, const char*);

    jsize (JNICALL *GetArrayLength)(JNIEnv*, jarray);
    jbyteArray (JNICALL *NewByteArray)(JNIEnv*, jsize);
    jfloatArray (JNICALL *NewFloatArray)(JNIEnv*, jsize);

    jbyte* (JNICALL *GetByteArrayElements)(JNIEnv*, jbyteArray, jboolean*);
    void (JNICALL *ReleaseByteArrayElements)(JNIEnv*, jbyteArray, jbyte*, jint);

    jfloat* (JNICALL *GetFloatArrayElements)(JNIEnv*, jfloatArray, jboolean*);
    void (JNICALL *ReleaseFloatArrayElements)(JNIEnv*, jfloatArray, jfloat*, jint);

    void (JNICALL *GetFloatArrayRegion)(JNIEnv*, jfloatArray, jsize, jsize, jfloat*);
    void (JNICALL *SetFloatArrayRegion)(JNIEnv*, jfloatArray, jsize, jsize, const jfloat*);

    void* (JNICALL *GetDirectBufferAddress)(JNIEnv*, jobject);
    jlong (JNICALL *GetDirectBufferCapacity)(JNIEnv*, jobject);
};

struct JNIInvokeInterface {
    void* reserved0;
    void* reserved1;
    void* reserved2;
    jint (JNICALL *DestroyJavaVM)(JavaVM*);
    jint (JNICALL *AttachCurrentThread)(JavaVM*, void**, void*);
    jint (JNICALL *DetachCurrentThread)(JavaVM*);
    jint (JNICALL *GetEnv)(JavaVM*, void**, jint);
};

#ifdef __cplusplus
}
#endif

#endif /* _JNI_H_ */

