// Paint Engine - JNI bridge (com.paintengine.engine.NativeEngine).
//
// Thin layer: converts Java types, then forwards to pe::Engine. No graphics
// API is called here; the engine marshals everything to its render thread.
// A C++ exception crossing the JNI boundary aborts the process, so every
// entry point is wrapped in guard().
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>

#include "core/Log.h"
#include "core/ShutdownReport.h"
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
                                                                              jboolean diagnostics, jint scene,
                                                                              jlong canvasBudgetBytes) {
    return guard("nativeCreate", jlong{0}, [&] {
        pe::EngineConfig config;
        config.backend = pe::gfx::backendPreferenceFromString(toStdString(env, backend));
        config.allowFallback = allowFallback == JNI_TRUE;
        config.enableValidation = validation == JNI_TRUE;
        config.renderMode = renderModeFromString(toStdString(env, renderMode));
        config.diagnostics = diagnostics == JNI_TRUE;
        config.scene = scene == 1 ? pe::SceneMode::Canvas : pe::SceneMode::Diagnostic;
        config.canvasMemoryBudgetBytes = canvasBudgetBytes > 0 ? static_cast<uint64_t>(canvasBudgetBytes) : 0;
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

JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeStatusJson(JNIEnv* env, jclass,
                                                                                    jlong handle) {
    return toJString(env, guard("nativeStatusJson", std::string("{}"),
                                [&] { return fromHandle(handle)->statusJson(); }));
}

// Static: works without an engine instance (e.g. between two engines).
JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeRecentLog(JNIEnv* env, jclass,
                                                                                   jint maxLines) {
    return toJString(env, guard("nativeRecentLog", std::string(), [&] {
        std::string out;
        for (const auto& line : pe::log::recentLines(static_cast<size_t>(maxLines > 0 ? maxLines : 0))) {
            out += line;
            out += '\n';
        }
        return out;
    }));
}

// Static: bookkeeping of the most recently destroyed graphics device (JSON).
JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeLastShutdownReport(JNIEnv* env, jclass) {
    return toJString(env, guard("nativeLastShutdownReport", std::string("{}"), [&] {
        const pe::shutdown::Report r = pe::shutdown::last();
        std::string text = pe::shutdown::describe(r);
        for (char& c : text)
            if (c == '"' || c == '\\') c = '\'';
        return std::string("{\"valid\":") + (r.valid ? "true" : "false") + ",\"backend\":\"" + r.backend +
               "\",\"clean\":" + (r.clean() ? "true" : "false") +
               ",\"created\":" + std::to_string(r.resourcesCreated) +
               ",\"destroyed\":" + std::to_string(r.resourcesDestroyed) +
               ",\"leaked\":" + std::to_string(r.resourcesLeaked) +
               ",\"memoryLeaked\":" + std::to_string(r.memoryAllocationsLeaked) +
               ",\"devices\":" + std::to_string(r.devicesShutDown) + ",\"text\":\"" + text + "\"}";
    }));
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeLogReport(JNIEnv*, jclass, jlong handle) {
    guard("nativeLogReport", 0, [&] {
        pe::Engine* engine = fromHandle(handle);
        pe::diagnostics::logMultiline("Diagnostics", engine->capabilityReport());
        pe::diagnostics::logMultiline("Diagnostics", engine->statusText());
        return 0;
    });
}

// ------------------------------------------------------------------ canvas
JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeSetSceneMode(JNIEnv*, jclass, jlong handle,
                                                                                   jint scene) {
    guard("nativeSetSceneMode", 0, [&] {
        fromHandle(handle)->setSceneMode(scene == 1 ? pe::SceneMode::Canvas : pe::SceneMode::Diagnostic);
        return 0;
    });
}

JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeCreateCanvas(JNIEnv* env, jclass,
                                                                                      jlong handle, jint width,
                                                                                      jint height, jint paperArgb,
                                                                                      jboolean transparent) {
    return guard("nativeCreateCanvas", static_cast<jstring>(nullptr), [&] {
        if (width <= 0 || height <= 0) return toJString(env, "width and height must be at least 1 px");
        pe::CanvasSpec spec;
        spec.width = static_cast<uint32_t>(width);
        spec.height = static_cast<uint32_t>(height);
        spec.paperArgb = static_cast<uint32_t>(paperArgb);
        spec.transparentBackground = transparent == JNI_TRUE;
        return toJString(env, fromHandle(handle)->createCanvas(spec));
    });
}

JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeCheckCanvasSize(JNIEnv* env, jclass,
                                                                                         jlong handle, jint width,
                                                                                         jint height) {
    return guard("nativeCheckCanvasSize", static_cast<jstring>(nullptr), [&] {
        const auto plan = fromHandle(handle)->checkCanvasSize(static_cast<uint32_t>(std::max(0, width)),
                                                             static_cast<uint32_t>(std::max(0, height)));
        std::string reason;
        for (char c : plan.reason) {
            if (c == '"' || c == '\\') reason += '\\';
            reason += c;
        }
        return toJString(env, std::string("{\"ok\":") + (plan.ok ? "true" : "false") + ",\"tiled\":" +
                                  (plan.mode == pe::canvas::StorageMode::Tiled ? "true" : "false") +
                                  ",\"maxBytes\":" + std::to_string(plan.maxAllocatedBytes) + ",\"reason\":\"" +
                                  reason + "\"}");
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeCloseCanvas(JNIEnv*, jclass, jlong handle) {
    guard("nativeCloseCanvas", 0, [&] {
        fromHandle(handle)->closeCanvas();
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeSetCanvasColors(JNIEnv*, jclass, jlong handle,
                                                                                      jint workspaceArgb,
                                                                                      jint paperArgb,
                                                                                      jboolean transparent) {
    guard("nativeSetCanvasColors", 0, [&] {
        fromHandle(handle)->setCanvasColors(static_cast<uint32_t>(workspaceArgb), static_cast<uint32_t>(paperArgb),
                                            transparent == JNI_TRUE);
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeClearCanvas(JNIEnv*, jclass, jlong handle) {
    guard("nativeClearCanvas", 0, [&] {
        fromHandle(handle)->clearCanvasContent();
        return 0;
    });
}

// Returns {zoom, rotation, centerX, centerY, fitted, minZoom, maxZoom,
//          canvasW, canvasH, screenW, screenH}.
JNIEXPORT jdoubleArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeViewportOp(JNIEnv* env, jclass,
                                                                                         jlong handle, jint op,
                                                                                         jdoubleArray args) {
    return guard("nativeViewportOp", static_cast<jdoubleArray>(nullptr), [&] {
        double a[8] = {};
        jsize n = args ? std::min<jsize>(env->GetArrayLength(args), 8) : 0;
        if (n > 0) env->GetDoubleArrayRegion(args, 0, n, a);
        const int code = op >= 0 && op <= 8 ? op : 0;
        const auto info = fromHandle(handle)->viewportOp(static_cast<pe::ViewOp>(code), a, static_cast<size_t>(n));
        const double out[11] = {info.state.zoom,
                                info.state.rotation,
                                info.state.center.x,
                                info.state.center.y,
                                info.state.fitted ? 1.0 : 0.0,
                                info.minZoom,
                                info.maxZoom,
                                static_cast<double>(info.canvasWidth),
                                static_cast<double>(info.canvasHeight),
                                static_cast<double>(info.screenWidth),
                                static_cast<double>(info.screenHeight)};
        jdoubleArray result = env->NewDoubleArray(11);
        if (result) env->SetDoubleArrayRegion(result, 0, 11, out);
        return result;
    });
}

JNIEXPORT jdoubleArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeMapPoint(JNIEnv* env, jclass,
                                                                                       jlong handle,
                                                                                       jboolean toCanvas, jdouble x,
                                                                                       jdouble y) {
    return guard("nativeMapPoint", static_cast<jdoubleArray>(nullptr), [&] {
        pe::Engine* e = fromHandle(handle);
        const pe::canvas::Vec2 p = toCanvas == JNI_TRUE ? e->screenToCanvas({x, y}) : e->canvasToScreen({x, y});
        const double out[2] = {p.x, p.y};
        jdoubleArray result = env->NewDoubleArray(2);
        if (result) env->SetDoubleArrayRegion(result, 0, 2, out);
        return result;
    });
}

JNIEXPORT jboolean JNICALL Java_com_paintengine_engine_NativeEngine_nativeStamp(JNIEnv*, jclass, jlong handle,
                                                                                jdouble x, jdouble y, jint size,
                                                                                jint argb) {
    return guard("nativeStamp", jboolean{JNI_FALSE}, [&] {
        return fromHandle(handle)->stampAt(x, y, static_cast<uint32_t>(std::max(1, size)),
                                           static_cast<uint32_t>(argb), nullptr)
                   ? jboolean{JNI_TRUE}
                   : jboolean{JNI_FALSE};
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeRunCanvasSelfTest(JNIEnv*, jclass,
                                                                                        jlong handle) {
    guard("nativeRunCanvasSelfTest", 0, [&] {
        fromHandle(handle)->runCanvasSelfTest();
        return 0;
    });
}

}  // extern "C"
