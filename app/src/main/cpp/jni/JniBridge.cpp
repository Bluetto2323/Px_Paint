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
#include <climits>
#include <cstdint>
#include <exception>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "brush/BrushPreview.h"
#include "brush/BrushTextures.h"
#include "core/Log.h"
#include "core/ShutdownReport.h"
#include "diagnostics/Diagnostics.h"
#include "engine/Engine.h"
#include "platform/android/AndroidWindow.h"

namespace {

constexpr const char* kTag = "JNI";

pe::Engine* fromHandle(jlong handle) { return reinterpret_cast<pe::Engine*>(handle); }

// Layer names travel in a tab/newline separated table: escape them.
std::string escapeField(const std::string& v) {
    std::string o;
    for (char c : v) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

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
        // Brush textures (~1.3 MiB, tens of ms) are built off the UI and render
        // threads; a stroke that needs one before it is ready waits for it once.
        static std::once_flag prewarm;
        std::call_once(prewarm, [] { std::thread([] { pe::brush::prewarmBrushTextures(); }).detach(); });
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

// Eyedropper at a screen point: straight opaque ARGB (low 32 bits), or -1 if
// nothing is there (outside the canvas, transparent background).
JNIEXPORT jlong JNICALL Java_com_paintengine_engine_NativeEngine_nativeSampleColor(JNIEnv*, jclass, jlong handle, jdouble x,
                                                                                  jdouble y) {
    return guard("nativeSampleColor", jlong{-1}, [&]() -> jlong {
        uint32_t argb = 0;
        return fromHandle(handle)->sampleColor(x, y, argb) ? static_cast<jlong>(argb) : jlong{-1};
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

// --- Brush strokes ------------------------------------------------------------
// sample: Engine::kSampleFloats floats (x, y, t, pressure, tilt, orientation; NaN = none)
// params: diameter, opacity, spacing, minSizeFraction, minOpacityFraction, tiltStrength, pressureSmoothing
// flags : 1 erase, 2 pressure->size, 4 pressure->opacity, 8 tilt shapes the dab
// curve : PressureLut::kSize floats (pressure response), or null for linear
namespace {
// Brush parameters from Kotlin (NativeEngine.Brush.params / flags), see
// docs/ENGINE_API.md. Shorter arrays (older callers) keep the defaults of
// the missing fields.
constexpr jsize kBrushParams = 21;

bool decodeBrush(JNIEnv* env, jfloatArray params, jint argb, jint flags, jfloatArray curve, pe::brush::BrushSettings& b) {
    if (!params) return false;
    const jsize n = std::min(env->GetArrayLength(params), kBrushParams);
    if (n < 7) return false;
    jfloat p[kBrushParams];
    env->GetFloatArrayRegion(params, 0, n, p);
    b.diameter = p[0];
    b.opacity = p[1];
    b.spacing = p[2];
    b.minSizeFraction = p[3];
    b.minOpacityFraction = p[4];
    b.tiltStrength = p[5];
    b.pressureSmoothing = p[6];
    if (n >= kBrushParams) {
        b.hardness = p[7];
        b.flow = p[8];
        b.roundness = p[9];
        b.angle = static_cast<float>(p[10] * 3.14159265358979323846 / 180.0);
        b.sizeJitter = p[11];
        b.scatter = p[12];
        b.angleJitter = p[13];
        b.flowJitter = p[14];
        b.grainDepth = p[15];
        b.grainPressure = p[16];
        b.wetEdge = p[17];
        b.tip = static_cast<pe::brush::BrushTip>(std::clamp(static_cast<int>(p[18]), 0, pe::brush::kBrushTipCount - 1));
        b.grain = static_cast<pe::brush::BrushGrain>(std::clamp(static_cast<int>(p[19]), 0, pe::brush::kBrushGrainCount - 1));
        b.accumulation = p[20] >= 1.0f ? pe::brush::Accumulation::Buildup : pe::brush::Accumulation::StrokeMax;
    }
    b.argb = static_cast<uint32_t>(argb);
    b.erase = (flags & 1) != 0;
    b.pressureSize = (flags & 2) != 0;
    b.pressureOpacity = (flags & 4) != 0;
    b.tiltShape = (flags & 8) != 0;
    b.angleFollowsStroke = (flags & 16) != 0;
    if (curve && env->GetArrayLength(curve) == static_cast<jsize>(pe::brush::PressureLut::kSize)) {
        auto lut = std::make_shared<pe::brush::PressureLut>();
        env->GetFloatArrayRegion(curve, 0, static_cast<jsize>(pe::brush::PressureLut::kSize), lut->lut.data());
        b.pressureCurve = lut;  // validated by BrushSettings::sanitized()
    }
    return true;
}
}  // namespace

JNIEXPORT jboolean JNICALL Java_com_paintengine_engine_NativeEngine_nativeStrokeBegin(
    JNIEnv* env, jclass, jlong handle, jfloatArray sample, jfloatArray params, jint argb, jint flags, jfloatArray curve) {
    return guard("nativeStrokeBegin", jboolean{JNI_FALSE}, [&]() -> jboolean {
        if (!sample || env->GetArrayLength(sample) < static_cast<jsize>(pe::Engine::kSampleFloats)) return JNI_FALSE;
        jfloat s[pe::Engine::kSampleFloats];
        env->GetFloatArrayRegion(sample, 0, static_cast<jsize>(pe::Engine::kSampleFloats), s);
        pe::brush::BrushSettings b;
        if (!decodeBrush(env, params, argb, flags, curve, b)) return JNI_FALSE;
        return fromHandle(handle)->strokeBegin(b, s) ? JNI_TRUE : JNI_FALSE;
    });
}

// Brush preview (no engine needed, any thread): `strokes` = [n, then per
// stroke: count, diameter (<= 0: brush), opacity (< 0: brush), then count
// samples of 6 floats in preview pixels]. Returns width * height ARGB or null.
JNIEXPORT jintArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeRenderBrushPreview(
    JNIEnv* env, jclass, jfloatArray params, jint argb, jint flags, jfloatArray curve, jint width, jint height,
    jfloatArray strokes, jint background) {
    return guard("nativeRenderBrushPreview", static_cast<jintArray>(nullptr), [&]() -> jintArray {
        if (!strokes || width <= 0 || height <= 0 || width > pe::brush::kMaxPreviewSide || height > pe::brush::kMaxPreviewSide)
            return nullptr;
        pe::brush::BrushSettings b;
        if (!decodeBrush(env, params, argb, flags, curve, b)) return nullptr;
        const jsize len = env->GetArrayLength(strokes);
        std::vector<jfloat> f(static_cast<size_t>(len));
        env->GetFloatArrayRegion(strokes, 0, len, f.data());
        std::vector<pe::brush::StrokePoint> pts;
        std::vector<pe::brush::PreviewStroke> list;
        std::vector<size_t> starts;
        size_t i = 0;
        const size_t n = len > 0 ? static_cast<size_t>(std::max(0.0f, f[0])) : 0;
        i = 1;
        pts.reserve(static_cast<size_t>(len) / 6);
        for (size_t k = 0; k < n && i + 3 <= f.size(); ++k) {
            const size_t count = static_cast<size_t>(std::max(0.0f, f[i]));
            pe::brush::PreviewStroke ps;
            ps.diameter = f[i + 1];
            ps.opacity = f[i + 2];
            i += 3;
            if (i + count * 6 > f.size()) break;
            starts.push_back(pts.size());
            for (size_t j = 0; j < count; ++j, i += 6) {
                pe::brush::StrokePoint p{f[i], f[i + 1], f[i + 2]};
                p.axes.pressure = std::isfinite(f[i + 3]) ? f[i + 3] : pe::brush::kNoValue;
                p.axes.tilt = std::isfinite(f[i + 4]) ? f[i + 4] : pe::brush::kNoValue;
                p.axes.orientation = std::isfinite(f[i + 5]) ? f[i + 5] : pe::brush::kNoValue;
                pts.push_back(p);
            }
            ps.count = count;
            list.push_back(ps);
        }
        for (size_t k = 0; k < list.size(); ++k) list[k].points = pts.data() + starts[k];
        std::vector<uint32_t> out(static_cast<size_t>(width) * height);
        if (!pe::brush::renderBrushPreview(b, list.data(), list.size(), width, height, static_cast<uint32_t>(background),
                                           out.data()))
            return nullptr;
        jintArray result = env->NewIntArray(width * height);
        if (result) env->SetIntArrayRegion(result, 0, width * height, reinterpret_cast<const jint*>(out.data()));
        return result;
    });
}

// `samples`: `count` samples of Engine::kSampleFloats floats each, oldest first.
JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeStrokeSamples(JNIEnv* env, jclass, jlong handle,
                                                                                    jfloatArray samples, jint count) {
    guard("nativeStrokeSamples", 0, [&] {
        if (!samples || count <= 0) return 0;
        constexpr jsize kStride = static_cast<jsize>(pe::Engine::kSampleFloats);
        const jsize len = env->GetArrayLength(samples);
        const jsize n = std::min<jsize>(count, len / kStride);
        // Copied through a stack buffer: no allocation per touch event.
        jfloat chunk[64 * pe::Engine::kSampleFloats];
        for (jsize done = 0; done < n;) {
            const jsize k = std::min<jsize>(64, n - done);
            env->GetFloatArrayRegion(samples, done * kStride, k * kStride, chunk);
            fromHandle(handle)->strokeSamples(chunk, static_cast<size_t>(k));
            done += k;
        }
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeStrokeEnd(JNIEnv*, jclass, jlong handle) {
    guard("nativeStrokeEnd", 0, [&] {
        fromHandle(handle)->strokeEnd();
        return 0;
    });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeStrokeCancel(JNIEnv*, jclass, jlong handle) {
    guard("nativeStrokeCancel", 0, [&] {
        fromHandle(handle)->strokeCancel();
        return 0;
    });
}

// --- Persistence ----------------------------------------------------------------
// Tile records (see CanvasDocument): int32 count, then {tx, ty, flags, pixels?}.
JNIEXPORT jbyteArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeExportCanvasTiles(
    JNIEnv* env, jclass, jlong handle, jboolean changedOnly, jint maxTiles) {
    return guard("nativeExportCanvasTiles", static_cast<jbyteArray>(nullptr), [&]() -> jbyteArray {
        std::vector<uint8_t> out;
        fromHandle(handle)->exportCanvasTiles(changedOnly == JNI_TRUE, static_cast<size_t>(std::max(0, maxTiles)), out);
        if (out.size() > static_cast<size_t>(INT32_MAX)) return nullptr;
        jbyteArray arr = env->NewByteArray(static_cast<jsize>(out.size()));
        if (!arr) return nullptr;  // OutOfMemoryError pending in Java
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(out.size()), reinterpret_cast<const jbyte*>(out.data()));
        return arr;
    });
}

JNIEXPORT jint JNICALL Java_com_paintengine_engine_NativeEngine_nativeImportCanvasTiles(JNIEnv* env, jclass,
                                                                                       jlong handle, jbyteArray data) {
    return guard("nativeImportCanvasTiles", jint{-1}, [&]() -> jint {
        if (!data) return -1;
        const jsize len = env->GetArrayLength(data);
        jbyte* bytes = env->GetByteArrayElements(data, nullptr);
        if (!bytes) return -1;
        const long n = fromHandle(handle)->importCanvasTiles(reinterpret_cast<const uint8_t*>(bytes),
                                                             static_cast<size_t>(len));
        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
        return static_cast<jint>(n);
    });
}

// --- Layers (Block 6) --------------------------------------------------------
// "active\t<id>\n" then one line per layer, bottom first:
// id \t visible \t locked \t opacity \t blend \t tiles \t version \t name
JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeLayers(JNIEnv* env, jclass, jlong handle) {
    return guard("nativeLayers", static_cast<jstring>(nullptr), [&]() -> jstring {
        uint32_t active = 0;
        const auto list = fromHandle(handle)->layers(active);
        std::string out = "active\t" + std::to_string(active) + "\n";
        char buf[160];
        for (const auto& l : list) {
            std::snprintf(buf, sizeof(buf), "%u\t%d\t%d\t%.6f\t%d\t%zu\t%llu\t", l.info.id, l.info.visible ? 1 : 0,
                          l.info.locked ? 1 : 0, static_cast<double>(l.info.opacity), static_cast<int>(l.info.blend), l.tiles,
                          static_cast<unsigned long long>(l.version));
            out += buf;
            out += escapeField(l.info.name);
            out += "\n";
        }
        return env->NewStringUTF(out.c_str());
    });
}

// Returns (error << 32) | newId (error 0 = done; codes: pe::LayerError).
JNIEXPORT jlong JNICALL Java_com_paintengine_engine_NativeEngine_nativeLayerOp(JNIEnv* env, jclass, jlong handle,
                                                                              jint op, jint id, jint arg, jfloat value,
                                                                              jstring text) {
    return guard("nativeLayerOp", static_cast<jlong>(static_cast<int64_t>(pe::LayerError::BadValue) << 32),
                 [&]() -> jlong {
                     if (op < 0 || op > 12) return static_cast<jlong>(static_cast<int64_t>(pe::LayerError::BadValue) << 32);
                     uint32_t newId = 0;
                     const pe::LayerError e = fromHandle(handle)->layerOp(
                         static_cast<pe::Engine::LayerOp>(op), static_cast<uint32_t>(id), arg, value,
                         text ? toStdString(env, text) : std::string(), newId);
                     return static_cast<jlong>((static_cast<int64_t>(e) << 32) | static_cast<int64_t>(newId));
                 });
}

// --- Undo / redo history (Block 7) ---------------------------------------------
// Returns the steps done (-1: refused, history cleared). op: pe::Engine::HistoryOp.
JNIEXPORT jint JNICALL Java_com_paintengine_engine_NativeEngine_nativeHistoryOp(JNIEnv*, jclass, jlong handle, jint op,
                                                                               jint count) {
    return guard("nativeHistoryOp", jint{0}, [&]() -> jint {
        if (op < 0 || op > 2) return 0;
        return fromHandle(handle)->historyOp(static_cast<pe::Engine::HistoryOp>(op), count);
    });
}

// --- Selection (Block 10) -------------------------------------------------------
// shape: Engine::SelectShape, mode: SelectionMask::Mode, xy: screen points
// (canvasSpace: canvas pixels, for tools / tests). Returns pe::LayerError.
JNIEXPORT jint JNICALL Java_com_paintengine_engine_NativeEngine_nativeSelect(JNIEnv* env, jclass, jlong handle, jint shape,
                                                                           jint mode, jboolean antialias, jfloatArray xy,
                                                                           jint points, jboolean canvasSpace) {
    return guard("nativeSelect", static_cast<jint>(pe::LayerError::BadValue), [&]() -> jint {
        if (!xy || shape < 0 || shape > 2 || points <= 0) return static_cast<jint>(pe::LayerError::BadValue);
        const jsize n = std::min(env->GetArrayLength(xy), static_cast<jsize>(points) * 2);
        if (n < 4) return static_cast<jint>(pe::LayerError::BadValue);
        std::vector<float> f(static_cast<size_t>(n));
        env->GetFloatArrayRegion(xy, 0, n, f.data());
        const auto sh = static_cast<pe::Engine::SelectShape>(shape);
        auto* e = fromHandle(handle);
        const pe::LayerError r = canvasSpace ? e->selectShapeCanvas(sh, mode, antialias == JNI_TRUE, f.data(), f.size() / 2)
                                             : e->selectShape(sh, mode, antialias == JNI_TRUE, f.data(), f.size() / 2);
        return static_cast<jint>(r);
    });
}

// (LayerError << 32) | new layer id (paste / duplicate). op: Engine::SelectionOp.
JNIEXPORT jlong JNICALL Java_com_paintengine_engine_NativeEngine_nativeSelectionOp(JNIEnv*, jclass, jlong handle, jint op,
                                                                                  jint mode) {
    return guard("nativeSelectionOp", static_cast<jlong>(static_cast<int64_t>(pe::LayerError::BadValue) << 32), [&]() -> jlong {
        if (op < 0 || op > 12) return static_cast<jlong>(static_cast<int64_t>(pe::LayerError::BadValue) << 32);
        uint32_t newId = 0;
        const pe::LayerError e = fromHandle(handle)->selectionOp(static_cast<pe::Engine::SelectionOp>(op), mode, newId);
        return static_cast<jlong>((static_cast<int64_t>(e) << 32) | static_cast<int64_t>(newId));
    });
}

// Offset of the lifted content from where it was lifted, in whole canvas pixels.
JNIEXPORT jint JNICALL Java_com_paintengine_engine_NativeEngine_nativeSelectionMove(JNIEnv*, jclass, jlong handle, jint dx, jint dy) {
    return guard("nativeSelectionMove", static_cast<jint>(pe::LayerError::BadValue),
                 [&]() -> jint { return static_cast<jint>(fromHandle(handle)->selectionMoveCanvas(dx, dy)); });
}

// [active, x, y, w, h, area, maskBytes, version, moving, dx, dy, clipboard, cx, cy, cw, ch, clipboardBytes]
JNIEXPORT jdoubleArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeSelectionInfo(JNIEnv* env, jclass, jlong handle) {
    return guard("nativeSelectionInfo", static_cast<jdoubleArray>(nullptr), [&]() -> jdoubleArray {
        const auto i = fromHandle(handle)->selectionInfo();
        const double v[17] = {i.active ? 1.0 : 0.0, double(i.bounds.x), double(i.bounds.y), double(i.bounds.w), double(i.bounds.h),
                              i.area, double(i.bytes), double(i.version), i.moving ? 1.0 : 0.0, double(i.dx), double(i.dy),
                              i.clipboard ? 1.0 : 0.0, double(i.clipboardBounds.x), double(i.clipboardBounds.y),
                              double(i.clipboardBounds.w), double(i.clipboardBounds.h), double(i.clipboardBytes)};
        jdoubleArray r = env->NewDoubleArray(17);
        if (r) env->SetDoubleArrayRegion(r, 0, 17, v);
        return r;
    });
}

// Transform of the lifted content (Block 12): canvas affine a, b, c, d, tx, ty (floating -> canvas).
JNIEXPORT jint JNICALL Java_com_paintengine_engine_NativeEngine_nativeSelectionTransform(JNIEnv* env, jclass, jlong handle,
                                                                                       jdoubleArray m) {
    return guard("nativeSelectionTransform", static_cast<jint>(pe::LayerError::BadValue), [&]() -> jint {
        if (!m || env->GetArrayLength(m) < 6) return static_cast<jint>(pe::LayerError::BadValue);
        double v[6];
        env->GetDoubleArrayRegion(m, 0, 6, v);
        return static_cast<jint>(fromHandle(handle)->selectionTransform(v));
    });
}

// (version << 1) | active: polled by the UI (cheap; changes with undo / redo too).
JNIEXPORT jlong JNICALL Java_com_paintengine_engine_NativeEngine_nativeSelectionVersion(JNIEnv*, jclass, jlong handle) {
    return guard("nativeSelectionVersion", jlong{0},
                 [&]() -> jlong { return static_cast<jlong>(fromHandle(handle)->selectionVersion()); });
}

// Outline loops in canvas pixels: [loops, n0, x, y, ..., n1, ...] (SelectionMask::contours).
JNIEXPORT jfloatArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeSelectionContours(JNIEnv* env, jclass, jlong handle) {
    return guard("nativeSelectionContours", static_cast<jfloatArray>(nullptr), [&]() -> jfloatArray {
        std::vector<float> c;
        fromHandle(handle)->selectionContours(c);
        if (c.empty()) c.push_back(0.0f);
        jfloatArray r = env->NewFloatArray(static_cast<jsize>(c.size()));
        if (r) env->SetFloatArrayRegion(r, 0, static_cast<jsize>(c.size()), c.data());
        return r;
    });
}

// Inverse coverage (255 = not selected), longer side <= maxSide: [w, h] in size, bytes returned.
JNIEXPORT jbyteArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeSelectionPreview(JNIEnv* env, jclass, jlong handle,
                                                                                           jint maxSide, jintArray size) {
    return guard("nativeSelectionPreview", static_cast<jbyteArray>(nullptr), [&]() -> jbyteArray {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> px;
        fromHandle(handle)->selectionPreview(static_cast<uint32_t>(std::max(1, static_cast<int>(maxSide))), w, h, px);
        if (px.empty() || !size || env->GetArrayLength(size) < 2) return nullptr;
        const jint wh[2] = {static_cast<jint>(w), static_cast<jint>(h)};
        env->SetIntArrayRegion(size, 0, 2, wh);
        jbyteArray r = env->NewByteArray(static_cast<jsize>(px.size()));
        if (r) env->SetByteArrayRegion(r, 0, static_cast<jsize>(px.size()), reinterpret_cast<const jbyte*>(px.data()));
        return r;
    });
}

// Canvas -> screen: a, b, c, d, tx, ty (x' = a x + c y + tx, y' = b x + d y + ty). False without a view.
JNIEXPORT jboolean JNICALL Java_com_paintengine_engine_NativeEngine_nativeViewMatrix(JNIEnv* env, jclass, jlong handle,
                                                                                   jfloatArray out) {
    return guard("nativeViewMatrix", jboolean{JNI_FALSE}, [&]() -> jboolean {
        double m[6];
        if (!out || env->GetArrayLength(out) < 6 || !fromHandle(handle)->viewMatrix(m)) return JNI_FALSE;
        const float f[6] = {float(m[0]), float(m[1]), float(m[2]), float(m[3]), float(m[4]), float(m[5])};
        env->SetFloatArrayRegion(out, 0, 6, f);
        return JNI_TRUE;
    });
}

// (undo steps << 32) | redo steps, lock-free: cheap enough to poll from the UI thread.
JNIEXPORT jlong JNICALL Java_com_paintengine_engine_NativeEngine_nativeHistoryCounts(JNIEnv*, jclass, jlong handle) {
    return guard("nativeHistoryCounts", jlong{0},
                 [&]() -> jlong { return static_cast<jlong>(fromHandle(handle)->historyCounts()); });
}

// "history\t<undo>\t<redo>\t<bytes>\t<budget>\t<maxSteps>\t<dropped>\t<refused>\n" then one line
// per step, oldest first: kind \t undone(0/1) \t label. Undone steps follow the
// done ones, the next redo first.
JNIEXPORT jstring JNICALL Java_com_paintengine_engine_NativeEngine_nativeHistory(JNIEnv* env, jclass, jlong handle) {
    return guard("nativeHistory", static_cast<jstring>(nullptr), [&]() -> jstring {
        pe::Engine* e = fromHandle(handle);
        const auto list = e->historyEntries();
        const auto st = e->historyStats();
        char buf[200];
        std::snprintf(buf, sizeof(buf), "history\t%zu\t%zu\t%llu\t%llu\t%zu\t%llu\t%llu\n", st.undoSteps, st.redoSteps,
                      static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.budget), st.maxSteps,
                      static_cast<unsigned long long>(st.dropped), static_cast<unsigned long long>(st.refused));
        std::string out = buf;
        for (const auto& h : list) {
            std::snprintf(buf, sizeof(buf), "%d\t%d\t", static_cast<int>(h.kind), h.undone ? 1 : 0);
            out += buf;
            out += escapeField(h.label);
            out += "\n";
        }
        return env->NewStringUTF(out.c_str());
    });
}

// bytes / maxSteps <= 0 keep the current value.
JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeSetHistoryBudget(JNIEnv*, jclass, jlong handle,
                                                                                      jlong bytes, jint maxSteps) {
    guard("nativeSetHistoryBudget", 0, [&]() -> int {
        fromHandle(handle)->setHistoryBudget(bytes > 0 ? static_cast<uint64_t>(bytes) : 0,
                                             maxSteps > 0 ? static_cast<size_t>(maxSteps) : 0);
        return 0;
    });
}

// Project load: the layer stack (properties, no pixels yet). `flags`: 1 visible, 2 locked.
JNIEXPORT jboolean JNICALL Java_com_paintengine_engine_NativeEngine_nativeSetLayerStack(
    JNIEnv* env, jclass, jlong handle, jintArray ids, jobjectArray names, jintArray flags, jfloatArray opacity,
    jintArray blend, jint activeId) {
    return guard("nativeSetLayerStack", jboolean{JNI_FALSE}, [&]() -> jboolean {
        if (!ids || !names || !flags || !opacity || !blend) return JNI_FALSE;
        const jsize n = env->GetArrayLength(ids);
        if (n <= 0 || env->GetArrayLength(names) != n || env->GetArrayLength(flags) != n ||
            env->GetArrayLength(opacity) != n || env->GetArrayLength(blend) != n)
            return JNI_FALSE;
        std::vector<jint> idv(static_cast<size_t>(n)), fl(static_cast<size_t>(n)), bl(static_cast<size_t>(n));
        std::vector<jfloat> op(static_cast<size_t>(n));
        env->GetIntArrayRegion(ids, 0, n, idv.data());
        env->GetIntArrayRegion(flags, 0, n, fl.data());
        env->GetIntArrayRegion(blend, 0, n, bl.data());
        env->GetFloatArrayRegion(opacity, 0, n, op.data());
        std::vector<pe::LayerInfo> stack;
        for (jsize i = 0; i < n; ++i) {
            pe::LayerInfo info;
            if (idv[i] <= 0 || bl[i] < 0 || bl[i] >= pe::canvas::kBlendModeCount) return JNI_FALSE;
            info.id = static_cast<uint32_t>(idv[i]);
            auto js = static_cast<jstring>(env->GetObjectArrayElement(names, i));
            info.name = js ? toStdString(env, js) : std::string();
            if (js) env->DeleteLocalRef(js);
            info.visible = (fl[i] & 1) != 0;
            info.locked = (fl[i] & 2) != 0;
            info.opacity = op[i];
            info.blend = static_cast<pe::canvas::BlendMode>(bl[i]);
            stack.push_back(info);
        }
        return fromHandle(handle)->setLayerStack(stack, static_cast<uint32_t>(activeId)) ? JNI_TRUE : JNI_FALSE;
    });
}

JNIEXPORT jbyteArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeExportLayerTiles(
    JNIEnv* env, jclass, jlong handle, jint id, jboolean changedOnly, jint maxTiles) {
    return guard("nativeExportLayerTiles", static_cast<jbyteArray>(nullptr), [&]() -> jbyteArray {
        std::vector<uint8_t> out;
        fromHandle(handle)->exportLayerTiles(static_cast<uint32_t>(id), changedOnly == JNI_TRUE,
                                             static_cast<size_t>(std::max(0, maxTiles)), out);
        if (out.size() > static_cast<size_t>(INT32_MAX)) return nullptr;
        jbyteArray arr = env->NewByteArray(static_cast<jsize>(out.size()));
        if (!arr) return nullptr;
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(out.size()), reinterpret_cast<const jbyte*>(out.data()));
        return arr;
    });
}

JNIEXPORT jint JNICALL Java_com_paintengine_engine_NativeEngine_nativeImportLayerTiles(JNIEnv* env, jclass, jlong handle,
                                                                                      jint id, jbyteArray data) {
    return guard("nativeImportLayerTiles", jint{-1}, [&]() -> jint {
        if (!data) return -1;
        const jsize len = env->GetArrayLength(data);
        jbyte* bytes = env->GetByteArrayElements(data, nullptr);
        if (!bytes) return -1;
        const long n = fromHandle(handle)->importLayerTiles(static_cast<uint32_t>(id), reinterpret_cast<const uint8_t*>(bytes),
                                                            static_cast<size_t>(len));
        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
        return static_cast<jint>(n);
    });
}

// One layer's own content (visibility ignored): {width, height, pixels...}; null if unknown.
JNIEXPORT jintArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeLayerThumbnail(JNIEnv* env, jclass,
                                                                                         jlong handle, jint id, jint maxSide) {
    return guard("nativeLayerThumbnail", static_cast<jintArray>(nullptr), [&]() -> jintArray {
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> px;
        if (!fromHandle(handle)->layerThumbnail(static_cast<uint32_t>(id), static_cast<uint32_t>(std::max(1, maxSide)), w, h, px))
            return nullptr;
        jintArray out = env->NewIntArray(static_cast<jsize>(px.size() + 2));
        if (!out) return nullptr;
        const jint head[2] = {static_cast<jint>(w), static_cast<jint>(h)};
        env->SetIntArrayRegion(out, 0, 2, head);
        if (!px.empty()) env->SetIntArrayRegion(out, 2, static_cast<jsize>(px.size()), reinterpret_cast<const jint*>(px.data()));
        return out;
    });
}

// Straight ARGB thumbnail: {width, height, pixels...}; null if no canvas.
JNIEXPORT jintArray JNICALL Java_com_paintengine_engine_NativeEngine_nativeCanvasThumbnail(JNIEnv* env, jclass,
                                                                                          jlong handle, jint maxSide) {
    return guard("nativeCanvasThumbnail", static_cast<jintArray>(nullptr), [&]() -> jintArray {
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> px;
        if (!fromHandle(handle)->canvasThumbnail(static_cast<uint32_t>(std::max(1, maxSide)), w, h, px)) return nullptr;
        const size_t n = px.size() + 2;
        jintArray out = env->NewIntArray(static_cast<jsize>(n));
        if (!out) return nullptr;
        const jint head[2] = {static_cast<jint>(w), static_cast<jint>(h)};
        env->SetIntArrayRegion(out, 0, 2, head);
        if (!px.empty())
            env->SetIntArrayRegion(out, 2, static_cast<jsize>(px.size()), reinterpret_cast<const jint*>(px.data()));
        return out;
    });
}

JNIEXPORT jint JNICALL Java_com_paintengine_engine_NativeEngine_nativeLoadCanvasContent(JNIEnv* env, jclass,
                                                                                      jlong handle, jintArray data) {
    return guard("nativeLoadCanvasContent", jint{-1}, [&]() -> jint {
        const jsize len = data ? env->GetArrayLength(data) : 0;
        std::vector<jint> flat(static_cast<size_t>(len));
        if (len) env->GetIntArrayRegion(data, 0, len, flat.data());
        std::vector<pe::ContentOp> ops;
        ops.reserve(flat.size() / 5);
        for (size_t i = 0; i + 4 < flat.size(); i += 5)
            ops.push_back({flat[i], flat[i + 1], flat[i + 2], flat[i + 3], static_cast<uint32_t>(flat[i + 4])});
        return static_cast<jint>(fromHandle(handle)->loadCanvasContent(ops));
    });
}

JNIEXPORT jlong JNICALL Java_com_paintengine_engine_NativeEngine_nativeCanvasContentRevision(JNIEnv*, jclass,
                                                                                           jlong handle) {
    return guard("nativeCanvasContentRevision", jlong{0},
                 [&] { return static_cast<jlong>(fromHandle(handle)->canvasContentRevision()); });
}

JNIEXPORT void JNICALL Java_com_paintengine_engine_NativeEngine_nativeRunCanvasSelfTest(JNIEnv*, jclass,
                                                                                        jlong handle) {
    guard("nativeRunCanvasSelfTest", 0, [&] {
        fromHandle(handle)->runCanvasSelfTest();
        return 0;
    });
}

}  // extern "C"
