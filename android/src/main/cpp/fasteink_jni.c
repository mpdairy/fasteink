/* JNI shim: Android Bitmaps and exceptions in, core calls out. No display logic here. */
#include <jni.h>
#include <android/bitmap.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fasteink.h"

static void fail(JNIEnv *env, const char *message) {
    jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
    if (type) (*env)->ThrowNew(env,type,message);
}
static void fail_with(JNIEnv *env, fe_error error) {
    fail(env,error.message ? error.message : strerror(error.errnum));
}

static long long stamp(JNIEnv *env, jstring firmware) {
    if (!firmware) return 0;
    const char *build=(*env)->GetStringUTFChars(env,firmware,NULL);
    if (!build) return 0;
    long long result=fe_firmware_stamp(build);
    (*env)->ReleaseStringUTFChars(env,firmware,build);
    return result;
}

/* Locks an RGBA_8888 bitmap; 0 on success. */
static int lock(JNIEnv *env, jobject bitmap, fe_image *out) {
    AndroidBitmapInfo info;
    void *pixels=NULL;
    if (AndroidBitmap_getInfo(env,bitmap,&info)!=ANDROID_BITMAP_RESULT_SUCCESS
            || info.format!=ANDROID_BITMAP_FORMAT_RGBA_8888
            || AndroidBitmap_lockPixels(env,bitmap,&pixels)!=ANDROID_BITMAP_RESULT_SUCCESS) return -1;
    *out=(fe_image){pixels,info.stride,(int)info.width,(int)info.height};
    return 0;
}

// {buffer width, buffer height, binary plane}, or null when unsupported.
JNIEXPORT jintArray JNICALL
Java_io_github_mpdairy_fasteink_FastEink_nativeCapabilities(JNIEnv *env, jclass clazz, jstring firmware) {
    (void)clazz;
    fe_driver driver;
    if (fe_driver_open_ebc(&driver)!=0) return NULL;
    fe_caps caps;
    int supported=fe_probe(&driver,stamp(env,firmware),&caps)==0;
    driver.close(driver.ctx);
    if (!supported) return NULL;
    jint values[3]={caps.width,caps.height,caps.binary_plane};
    jintArray result=(*env)->NewIntArray(env,3);
    if (result) (*env)->SetIntArrayRegion(env,result,0,3,values);
    return result;
}

// The driver's info query, which makes no display writes and changes no permissions.
JNIEXPORT jstring JNICALL
Java_io_github_mpdairy_fasteink_FastEink_nativeDescribe(JNIEnv *env, jclass clazz) {
    (void)clazz;
    char result[512];
    fe_driver driver;
    int error=fe_driver_open_ebc(&driver);
    if (error!=0) snprintf(result,sizeof(result),"Display device open denied/unavailable: %s",strerror(-error));
    else { fe_describe(&driver,result,sizeof(result)); driver.close(driver.ctx); }
    return (*env)->NewStringUTF(env,result);
}

JNIEXPORT jlong JNICALL
Java_io_github_mpdairy_fasteink_Session_nativeOpen(JNIEnv *env, jclass clazz,
        jobject background, jint x, jint y, jint mode, jstring firmware) {
    (void)clazz;
    long long firmware_stamp=stamp(env,firmware);
    fe_driver driver;
    int error=fe_driver_open_ebc(&driver);
    if (error!=0) { fail(env,strerror(-error)); return 0; }
    fe_image image;
    if (lock(env,background,&image)!=0) {
        driver.close(driver.ctx); fail(env,"Cannot prepare previous display pixels"); return 0;
    }
    fe_error why={0};
    fe_session *session=fe_open(driver,(fe_mode)mode,firmware_stamp,x,y,image,&why);
    AndroidBitmap_unlockPixels(env,background);
    if (!session) { fail_with(env,why); return 0; }
    return (jlong)(intptr_t)session;
}

JNIEXPORT jint JNICALL
Java_io_github_mpdairy_fasteink_Session_nativePresent(JNIEnv *env, jclass clazz,
        jlong handle, jobject bitmap, jint left, jint top, jint right, jint bottom, jboolean force) {
    (void)clazz;
    fe_image image;
    if (!handle || lock(env,bitmap,&image)!=0) { fail(env,"Invalid display bitmap/session"); return FE_BUSY; }
    fe_error why={0};
    int result=fe_present((fe_session *)(intptr_t)handle,image,left,top,right,bottom,force==JNI_TRUE,&why);
    AndroidBitmap_unlockPixels(env,bitmap);
    if (result==FE_FAILED) { fail_with(env,why); return FE_BUSY; }
    return result;
}

JNIEXPORT void JNICALL
Java_io_github_mpdairy_fasteink_Session_nativeClose(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env; (void)clazz;
    fe_close((fe_session *)(intptr_t)handle);
}

JNIEXPORT jint JNICALL
Java_io_github_mpdairy_fasteink_Session_nativeReadGray(JNIEnv *env, jclass clazz, jlong handle, jint x, jint y) {
    (void)clazz;
    int gray=fe_read_gray((fe_session *)(intptr_t)handle,x,y);
    if (gray<0) fail(env,"Invalid display sample");
    return gray;
}

JNIEXPORT void JNICALL
Java_io_github_mpdairy_fasteink_Session_nativeRotate(JNIEnv *env, jclass clazz,
        jobject source, jobject target, jint quarter, jint left, jint top, jint right, jint bottom) {
    (void)clazz;
    fe_image input, output;
    if (lock(env,source,&input)!=0) { fail(env,"Cannot read panel rotation"); return; }
    if (lock(env,target,&output)!=0) {
        AndroidBitmap_unlockPixels(env,source); fail(env,"Cannot write panel rotation"); return;
    }
    int result=fe_rotate(input,(uint8_t *)output.pixels,output.pitch,output.width,output.height,
                         quarter,left,top,right,bottom);
    AndroidBitmap_unlockPixels(env,target); AndroidBitmap_unlockPixels(env,source);
    if (result!=0) fail(env,"Invalid panel rotation");
}
