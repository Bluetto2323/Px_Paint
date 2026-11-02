// Paint Engine - JNI bridge (com.paintengine.engine.NativeEngine).
//
// Thin layer: converts Java types, then forwards to pe::Engine. No graphics
// API is called here; the engine marshals everything to its render thread.
// A C++ exception crossing the JNI boundary aborts the process, so every
// entry point is wrapped in guard().
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <exception>
#include <memory>
#include <string>

#include "core/Log.h"
#include "diagnostics/Diagnostics.h"
#include "engine/Engine.h"
#include "platform/android/AndroidWindow.h"

namespace {

constexpr const char* kTag = "JNI";

pe::Engine* fromHandle(jlong handle) { return reinterpret_cast<pe::Engine*>(handle); }

std::string toStdString(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(s, chars);
    return out;
}

// NewStringUTF expects *modified* UTF-8; driver strings are not guaranteed to
// be valid, so non-ASCII bytes are replaced to avoid a JNI abort.
jstring toJString(JNIEnv* env, std::string text) {
    for (char& c : text)
        if (static_cast<unsigned char>(c) >= 0x80) c = '?';
    return env->NewStringUTF(text.c_str());
}

pe::gfx::RenderMode renderModeFromString(const std::string& s) {
    return s == "continuous" ? pe::gfx::RenderMode::Continuous : pe::gfx::RenderMode::OnDemand;
}

template <class F, class R>
R guard(const char* function, R fallback, F&& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        PE_LOGE(kTag, "%s: exception %s", function, e.what());
    } catch (...) {
        PE_LOGE(kTag, "%s: unknown exception", function);
    }
    return fallback;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    pe::installAndroidLogSink();
    PE_LOGI(kTag, "libpaintengine loaded");
    return JNI_VERSION_1_6;
}

JNIEXPORT jlong JNICALL Java_com_paintengine_engine_NativeEngine_nativeCreate(JNIEnv* env, jclass, jstring backend,
                                                                              jboolean allowFallback,
                                                                              jboolean validation,
                                                                              jstring renderMode,
                                                                              jboolean diagnostics) {
    return guard("nativeCreate", jlong{0}, [&] {
        pe::EngineConfig config;
        config.backend = pe::gfx::backendPreferenceFromString(toStdString(env, backend));
        config.allowFallback = allowFallback == JNI_TRUE;
        config.enableValidation = validation == JNI_TRUE;
        config.renderMode = renderModeFromString(toStdString(env, renderMode));
        config.diagnostics = diagnostics == JNI_TRUE;
        auto engine = std::make_unique<pe::Engine>(config);
        engine->start();
        return reinterpret_cast<jlong>(engine.release());
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    guard("nativeDestroy", 0, [&] {
        delete fromHandle(handle);  // ~Engine joins the render thread (ordered teardown)
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeSurfaceCreated(JNIEnv* env, jclass, jlong handle,
                                                                                     jobject surface) {
    // ANativeWindow_fromSurface returns an acquired reference; AndroidWindow
    // releases it on the render thread once the swapchain/EGLSurface is gone.
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (!window) {
        PE_LOGE(kTag, "ANativeWindow_fromSurface returned null");
        return;
    }
    // Ownership passes to AndroidWindow before anything can throw.
    auto wrapped = std::make_unique<pe::AndroidWindow>(window);
    guard("nativeSurfaceCreated", 0, [&] {
        fromHandle(handle)->surfaceCreated(std::move(wrapped));
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeSurfaceChanged(JNIEnv*, jclass, jlong handle,
                                                                                     jint width, jint height) {
    guard("nativeSurfaceChanged", 0, [&] {
        fromHandle(handle)->surfaceChanged(static_cast<uint32_t>(width > 0 ? width : 0),
                                           static_cast<uint32_t>(height > 0 ? height : 0));
        return 0;
    });
}

// Blocking: returns only once the native side no longer uses the window.
JNIEXPORT jboolean JNICALL Java_com_paintengine_engine_NativeEngine_nativeSurfaceDestroyed(JNIEnv*, jclass,
                                                                                           jlong handle) {
    return guard("nativeSurfaceDestroyed", static_cast<jboolean>(JNI_FALSE), [&] {
        return fromHandle(handle)->surfaceDestroyed() ? JNI_TRUE : JNI_FALSE;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativePause(JNIEnv*, jclass, jlong handle) {
    guard("nativePause", 0, [&] {
        fromHandle(handle)->pause();
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeResume(JNIEnv*, jclass, jlong handle) {
    guard("nativeResume", 0, [&] {
        fromHandle(handle)->resume();
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeRequestRender(JNIEnv*, jclass, jlong handle) {
    guard("nativeRequestRender", 0, [&] {
        fromHandle(handle)->requestRender();
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeSetRenderMode(JNIEnv* env, jclass, jlong handle,
                                                                                    jstring mode) {
    guard("nativeSetRenderMode", 0, [&] {
        fromHandle(handle)->setRenderMode(renderModeFromString(toStdString(env, mode)));
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeSetDiagnosticsEnabled(JNIEnv*, jclass,
                                                                                            jlong handle,
                                                                                            jboolean enabled) {
    guard("nativeSetDiagnosticsEnabled", 0, [&] {
        fromHandle(handle)->setDiagnosticsEnabled(enabled == JNI_TRUE);
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeRunSelfTest(JNIEnv*, jclass, jlong handle) {
    guard("nativeRunSelfTest", 0, [&] {
        fromHandle(handle)->runSelfTest();
        return 0;
    });
}

JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeStatusText(JNIEnv* env, jclass,
                                                                                    jlong handle) {
    return toJString(env, guard("nativeStatusText", std::string("status unavailable"),
                                [&] { return fromHandle(handle)->statusText(); }));
}

JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeCapabilityReport(JNIEnv* env, jclass,
                                                                                          jlong handle) {
    return toJString(env, guard("nativeCapabilityReport", std::string(),
                                [&] { return fromHandle(handle)->capabilityReport(); }));
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeLogReport(JNIEnv*, jclass, jlong handle) {
    guard("nativeLogReport", 0, [&] {
        pe::Engine* engine = fromHandle(handle);
        pe::diagnostics::logMultiline("Diagnostics", engine->capabilityReport());
        pe::diagnostics::logMultiline("Diagnostics", engine->statusText());
        return 0;
    });
}

}  // extern "C"
