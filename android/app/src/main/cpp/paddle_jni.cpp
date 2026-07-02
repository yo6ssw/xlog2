// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

// paddle_jni — JNI bridge for the remote paddle keyer (RemotePaddleKeyer). The
// iambic keyer + UDP edge streaming + AAudio sidetone all run in the C++ core;
// Kotlin feeds paddle contacts (setDit/setDah, from the USB HID reader) and gets
// status/transmit callbacks. Mirrors how the desktop shells drive paddle_.
//
// onStatus/onTransmit fire on the keyer's native worker thread; a thread_local
// attach helper attaches that thread to the JVM and detaches it cleanly on exit.

#include "RemotePaddleKeyer.h"
#include "IUiDispatcher.h"

#include <jni.h>

#include <functional>
#include <memory>
#include <string>

namespace {

JavaVM* g_paddleVm = nullptr;

// Attaches the calling native thread to the JVM on first use and detaches it when
// the thread exits (thread_local destructor). A JVM-owned thread (a Java caller)
// is left alone — GetEnv succeeds, so we never detach it.
struct JniAttach {
    JNIEnv* env      = nullptr;
    bool    attached = false;
    JNIEnv* get() {
        if (env != nullptr)
            return env;
        if (g_paddleVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
            return env;
        if (g_paddleVm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
            return env;
        }
        env = nullptr;
        return nullptr;
    }
    ~JniAttach() {
        if (attached && g_paddleVm != nullptr)
            g_paddleVm->DetachCurrentThread();
    }
};
thread_local JniAttach t_jni;

// Runs posted closures inline: the keyer's callbacks only push to thread-safe
// Kotlin StateFlows, so no UI-thread hop is needed.
struct InlineDispatcher : IUiDispatcher {
    void post(std::function<void()> fn) override { fn(); }
};

struct PaddleHolder {
    InlineDispatcher  disp;
    RemotePaddleKeyer keyer{disp};
    jobject           listener = nullptr;
    jmethodID         mStatus   = nullptr;
    jmethodID         mTransmit = nullptr;
};

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_ro_scripca_xlog2_PaddleKeyer_nativeCreate(JNIEnv* env, jobject, jobject listener) {
    if (g_paddleVm == nullptr)
        env->GetJavaVM(&g_paddleVm);
    auto* h = new PaddleHolder();
    h->listener = env->NewGlobalRef(listener);
    jclass cls = env->GetObjectClass(listener);
    h->mStatus   = env->GetMethodID(cls, "onStatus", "(Ljava/lang/String;)V");
    h->mTransmit = env->GetMethodID(cls, "onTransmit", "(Z)V");

    h->keyer.onStatus = [h](const std::string& s) {
        if (JNIEnv* e = t_jni.get()) {
            jstring js = e->NewStringUTF(s.c_str());
            e->CallVoidMethod(h->listener, h->mStatus, js);
            e->DeleteLocalRef(js);
        }
    };
    h->keyer.onTransmit = [h](bool tx) {
        if (JNIEnv* e = t_jni.get())
            e->CallVoidMethod(h->listener, h->mTransmit, static_cast<jboolean>(tx));
    };
    return reinterpret_cast<jlong>(h);
}

JNIEXPORT void JNICALL
Java_ro_scripca_xlog2_PaddleKeyer_nativeStart(JNIEnv* env, jobject, jlong handle,
        jstring host, jint port, jint wpm, jboolean iambicB, jboolean ultimatic,
        jboolean autospace, jboolean sidetone, jint toneHz, jint level, jint muteTailMs) {
    auto* h = reinterpret_cast<PaddleHolder*>(handle);
    if (h == nullptr)
        return;
    RemotePaddleConfig pc;
    const char* c = env->GetStringUTFChars(host, nullptr);
    pc.host = (c != nullptr) ? c : "";
    if (c != nullptr)
        env->ReleaseStringUTFChars(host, c);
    pc.port       = port;
    pc.wpm        = wpm;
    pc.iambicB    = iambicB;
    pc.ultimatic  = ultimatic;
    pc.autospace  = autospace;
    pc.sidetone   = sidetone;
    pc.toneHz     = toneHz;
    pc.level      = level;
    pc.muteTailMs = muteTailMs;
    h->keyer.start(pc);
}

JNIEXPORT void JNICALL
Java_ro_scripca_xlog2_PaddleKeyer_nativeStop(JNIEnv*, jobject, jlong handle) {
    auto* h = reinterpret_cast<PaddleHolder*>(handle);
    if (h != nullptr)
        h->keyer.stop();
}

JNIEXPORT void JNICALL
Java_ro_scripca_xlog2_PaddleKeyer_nativeSetDit(JNIEnv*, jobject, jlong handle, jboolean p) {
    auto* h = reinterpret_cast<PaddleHolder*>(handle);
    if (h != nullptr)
        h->keyer.setDit(p);
}

JNIEXPORT void JNICALL
Java_ro_scripca_xlog2_PaddleKeyer_nativeSetDah(JNIEnv*, jobject, jlong handle, jboolean p) {
    auto* h = reinterpret_cast<PaddleHolder*>(handle);
    if (h != nullptr)
        h->keyer.setDah(p);
}

JNIEXPORT void JNICALL
Java_ro_scripca_xlog2_PaddleKeyer_nativeDestroy(JNIEnv* env, jobject, jlong handle) {
    auto* h = reinterpret_cast<PaddleHolder*>(handle);
    if (h == nullptr)
        return;
    h->keyer.stop();  // joins the worker before we drop the listener
    if (h->listener != nullptr)
        env->DeleteGlobalRef(h->listener);
    delete h;
}

}  // extern "C"
