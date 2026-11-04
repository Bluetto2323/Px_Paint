#include "engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <system_error>

#include "core/Log.h"
#include "core/TestHooks.h"
#include "core/Time.h"

namespace pe {

namespace {
constexpr const char* kTag = "Engine";
constexpr uint32_t kMaxConsecutiveErrors = 5;
constexpr uint32_t kMaxConsecutiveExceptions = 3;
constexpr int64_t kPerfLogPeriodNs = 5'000'000'000;
// Two failures of the same backend within this window exclude it (if another
// backend remains). Failures further apart are treated as unrelated.
constexpr int64_t kFailureWindowNs = 60'000'000'000;
// More recoveries than this within kFailureWindowNs: stop retrying.
constexpr size_t kMaxRecoveriesPerWindow = 6;
}  // namespace

Engine::Engine(const EngineConfig& config) : config_(config), sceneMode_(config.scene), canvas_(doc_) {
    scheduler_.setMode(config.renderMode);
    canvas_.setMemoryBudget(config.canvasMemoryBudgetBytes);
    // Layer pixels live on the CPU: the same share of RAM bounds what layer
    // operations may allocate (duplicate, merge).
    doc_.setMemoryBudget(config.canvasMemoryBudgetBytes ? config.canvasMemoryBudgetBytes : (1024ull << 20));
}

Engine::~Engine() { shutdown(); }

bool Engine::onRenderThread() const { return std::this_thread::get_id() == renderThreadId_.load(); }

void Engine::start() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (threadState_ != ThreadState::NotStarted) return;
        threadState_ = ThreadState::Running;
    }
    quit_ = false;
    PE_LOGI(kTag, "Starting (backend=%s, fallback=%s, validation=%s, render=%s, diagnostics=%s)",
            gfx::toString(config_.backend), config_.allowFallback ? "on" : "off",
            config_.enableValidation ? "on" : "off", gfx::toString(config_.renderMode),
            config_.diagnostics ? "on" : "off");
    queue_.setExceptionHandler([this](const char* what) {
        // Runs on the render thread right after the failing task.
        pendingRecovery_ = std::string("exception in lifecycle task: ") + what;
    });
    try {
        thread_ = std::thread([this] { threadMain(); });
    } catch (const std::system_error& e) {
        PE_LOGE(kTag, "Could not start the render thread: %s", e.what());
        queue_.close();
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        threadState_ = ThreadState::Exited;
        lifecycleCv_.notify_all();
    }
}

void Engine::shutdown() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (threadState_ == ThreadState::NotStarted) {
            threadState_ = ThreadState::Exited;
            return;
        }
    }
    if (onRenderThread()) {
        PE_LOGE(kTag, "shutdown() called from the render thread: ignored (would self-join)");
        return;
    }
    quit_ = true;
    queue_.close();  // later lifecycle calls are refused
    queue_.wake();
    if (thread_.joinable()) {
        thread_.join();
        PE_LOGI(kTag, "Shut down");
    }
    queue_.clear();  // tasks refused/unrun: their windows are released here, never used
}

const char* toString(SceneMode mode) { return mode == SceneMode::Canvas ? "canvas" : "diagnostic"; }

// ------------------------------------------------------------ public (UI)
void Engine::surfaceCreated(std::unique_ptr<PlatformWindow> window) {
    if (!window) return;
    // std::function must be copyable: move the window into a shared holder.
    auto holder = std::make_shared<std::unique_ptr<PlatformWindow>>(std::move(window));
    const bool posted = queue_.post([this, holder] {
        if (window_) {
            // Android always destroys before re-creating; be defensive anyway:
            // the old window must be detached BEFORE its reference is dropped.
            PE_LOGW(kTag, "surfaceCreated while a window is still attached: releasing the old one first");
            detachAndReleaseWindow();
        }
        // surfaceRevoked_ is only cleared by the release task: a destroy that
        // raced with this create must keep blocking new frames.
        window_ = std::move(*holder);
        PE_LOGI(kTag, "surfaceCreated: %s %ux%u", toString(window_->kind()), window_->width(), window_->height());
        if (renderingDisabled_) {
            PE_LOGW(kTag, "Rendering disabled after repeated failures: window kept, not attached");
        } else if (!selection_) {
            selectDevice(failedBackends_);
        } else if (selection_.device->attachSurface(*window_)) {
            surfaceAttached_ = true;
            onSurfaceAvailable();
        } else {
            recoverDevice("attachSurface failed on existing device");
        }
        publishStatus(true);
    });
    if (!posted) PE_LOGW(kTag, "surfaceCreated after shutdown: window released without use");
}

void Engine::surfaceChanged(uint32_t width, uint32_t height) {
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        view_.setScreenSize(width, height);  // a fitted canvas is refitted
    }
    queue_.post([this, width, height] {
        PE_LOGI(kTag, "surfaceChanged: %ux%u", width, height);
        if (window_ && window_->kind() == WindowKind::Headless)
            static_cast<HeadlessWindow*>(window_.get())->resize(width, height);
        if (selection_ && surfaceAttached_) selection_.device->notifySurfaceResized({width, height});
        scheduler_.markWarmup();  // the rebuild frame must not count as steady state
        scheduler_.requestFrame();
        publishStatus(true);
    });
}

bool Engine::surfaceDestroyed() {
    if (onRenderThread()) {  // defensive: never wait for ourselves
        releaseSurfaceForUi();
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (threadState_ != ThreadState::Running) return false;  // no thread can use the window
    }

    // (1) Stop starting new frames on this surface immediately.
    surfaceRevoked_ = true;

    // (2) Post the release; completion is signalled even if it throws.
    auto done = std::make_shared<bool>(false);
    struct CompletionSignal {
        Engine* engine;
        std::shared_ptr<bool> done;
        ~CompletionSignal() {
            std::lock_guard<std::mutex> lock(engine->lifecycleMutex_);
            *done = true;
            engine->lifecycleCv_.notify_all();
        }
    };
    const bool posted = queue_.post([this, done] {
        CompletionSignal signal{this, done};
        releaseSurfaceForUi();
    });
    queue_.wake();
    if (!posted) PE_LOGI(kTag, "surfaceDestroyed during shutdown: waiting for render thread teardown");

    // (4) Wait without timeout: for the release, or for the thread to exit
    // (teardown releases the window in the same order).
    const int64_t start = nowNs();
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    while (!*done && threadState_ != ThreadState::Exited) {
        if (lifecycleCv_.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout && !*done &&
            threadState_ != ThreadState::Exited) {
            lock.unlock();
            PE_LOGW(kTag, "surfaceDestroyed: still waiting for the render thread (%.1f s) - GPU call blocked?",
                    nsToMs(nowNs() - start) / 1000.0);
            lock.lock();
        }
    }
    const bool releasedByTask = *done;
    lock.unlock();
    const double waitedMs = nsToMs(nowNs() - start);
    if (waitedMs > 100.0) PE_LOGW(kTag, "surfaceDestroyed took %.1f ms", waitedMs);
    if (!releasedByTask) PE_LOGI(kTag, "surfaceDestroyed: window released by render thread teardown");
    return true;
}

void Engine::pause() {
    queue_.post([this] {
        paused_ = true;
        PE_LOGI(kTag, "Paused");
        publishStatus(true);
    });
}

void Engine::resume() {
    queue_.post([this] {
        paused_ = false;
        scheduler_.resetIntervalTracking();
        scheduler_.requestFrame();
        PE_LOGI(kTag, "Resumed");
        publishStatus(true);
    });
}

void Engine::setRenderMode(gfx::RenderMode mode) {
    queue_.post([this, mode] {
        config_.renderMode = mode;
        scheduler_.setMode(mode);
        scheduler_.resetIntervalTracking();
        scheduler_.requestFrame();
        PE_LOGI(kTag, "Render mode: %s", gfx::toString(mode));
        publishStatus(true);
    });
}

void Engine::setDiagnosticsEnabled(bool enabled) {
    queue_.post([this, enabled] {
        config_.diagnostics = enabled;
        lastPerfLogNs_ = 0;
        PE_LOGI(kTag, "Diagnostics: %s (render mode unchanged: %s)", enabled ? "on" : "off",
                gfx::toString(scheduler_.mode()));
        publishStatus(true);
    });
}

void Engine::setSceneMode(SceneMode mode) {
    queue_.post([this, mode] {
        if (sceneMode_ == mode) return;
        if (sceneMode_ == SceneMode::Canvas) canvas_.abortSelfTest("scene changed");
        sceneMode_ = mode;
        PE_LOGI(kTag, "Scene: %s", toString(mode));
        scheduler_.requestFrame();
        publishStatus(true);
    });
}

std::string Engine::createCanvas(const CanvasSpec& spec) {
    const uint32_t maxSide = canvas::StoragePolicy::kMaxCanvasSide;
    if (spec.width == 0 || spec.height == 0) return "width and height must be at least 1 px";
    if (spec.width > maxSide || spec.height > maxSide)
        return "the largest supported side is " + std::to_string(maxSide) + " px";
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        view_.setCanvasSize(spec.width, spec.height);
        view_.setRotationAt(0.0, {view_.screenWidth() * 0.5, view_.screenHeight() * 0.5});
        view_.fitToScreen();
    }
    doc_.open(spec.width, spec.height);  // synchronous: imports may follow at once
    queue_.post([this, spec] {
        canvas_.open(spec);
        scheduler_.requestFrame();
        publishStatus(true);
    });
    return {};
}

canvas::StoragePlan Engine::checkCanvasSize(uint32_t width, uint32_t height) const {
    const CanvasStatus c = status().canvas;
    if (c.maxTextureSize == 0) {
        canvas::StoragePlan p;
        p.reason = "graphics device not ready yet";
        return p;
    }
    canvas::StorageLimits l;
    l.maxTextureSize = c.maxTextureSize;
    l.memoryBudgetBytes = c.memoryBudgetBytes;
    return canvas::StoragePolicy::choose(width, height, l);
}

void Engine::closeCanvas() {
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        view_.setCanvasSize(0, 0);
    }
    doc_.close();
    queue_.post([this] {
        canvas_.close();
        scheduler_.requestFrame();
        publishStatus(true);
    });
}

void Engine::setCanvasColors(uint32_t workspaceArgb, uint32_t paperArgb, bool transparentBackground) {
    doc_.setBackground(paperArgb, transparentBackground);  // layers blend with the paper
    queue_.post([this, workspaceArgb, paperArgb, transparentBackground] {
        canvas_.setWorkspaceColor(workspaceArgb);
        canvas_.setPaper(paperArgb, transparentBackground);
        scheduler_.requestFrame();
    });
}

void Engine::clearCanvasContent() {
    doc_.clear();  // the active layer
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(true);
    });
}

bool Engine::stampAt(double screenX, double screenY, uint32_t sizePx, uint32_t argb, canvas::Vec2* canvasPoint) {
    canvas::Vec2 c;
    canvas::RectI rect;
    sizePx = std::clamp<uint32_t>(sizePx, 1, 512);
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        if (!view_.valid()) return false;
        c = view_.toCanvas({screenX, screenY});
        if (c.x < 0 || c.y < 0 || c.x >= view_.canvasWidth() || c.y >= view_.canvasHeight()) return false;
        rect = CanvasController::stampRect(c.x, c.y, sizePx, view_.canvasWidth(), view_.canvasHeight());
    }
    if (canvasPoint) *canvasPoint = c;
    if (rect.empty()) return false;
    doc_.fill(rect, argb);
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(false);
    });
    return true;
}

uint64_t Engine::canvasContentRevision() const { return doc_.revision(); }

size_t Engine::loadCanvasContent(const std::vector<ContentOp>& ops) {
    const size_t kept = doc_.loadLegacy(ops);
    PE_LOGI(kTag, "Legacy canvas content loaded: %zu of %zu operations", kept, ops.size());
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(true);
    });
    return kept;
}

namespace {
// Screen sample -> canvas sample (viewport locked by the caller).
brush::StrokePoint toCanvas(const canvas::Viewport& view, const float* f) {
    const canvas::Vec2 c = view.toCanvas({f[0], f[1]});
    brush::StrokePoint p{c.x, c.y, f[2]};
    p.axes.pressure = std::isfinite(f[3]) ? f[3] : brush::kNoValue;
    p.axes.tilt = std::isfinite(f[4]) ? f[4] : brush::kNoValue;
    // Screen direction = canvas direction + view rotation.
    p.axes.orientation =
        std::isfinite(f[5]) ? static_cast<float>(canvas::normalizeAngle(f[5] - view.rotation())) : brush::kNoValue;
    return p;
}
}  // namespace

bool Engine::strokeBegin(const brush::BrushSettings& settings, const float* sample) {
    if (!sample) return false;
    brush::StrokePoint p;
    brush::BrushSettings s = settings;
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        if (!view_.valid()) return false;
        p = toCanvas(view_, sample);
        // A fixed nib angle is held in the hand: screen space, like the
        // stylus orientation, so it does not turn with the canvas view.
        // (An angle that follows the stroke is relative to its direction.)
        if (!s.angleFollowsStroke) s.angle = static_cast<float>(s.angle - view_.rotation());
    }
    if (!doc_.strokeBegin(s, p)) return false;
    queue_.wake();
    return true;
}

void Engine::strokeSamples(const float* samples, size_t count) {
    if (!samples || count == 0) return;
    // Converted in chunks on the stack: no allocation per touch event.
    brush::StrokePoint chunk[64];
    size_t done = 0;
    while (done < count) {
        const size_t n = std::min<size_t>(64, count - done);
        {
            std::lock_guard<std::mutex> lock(viewMutex_);
            if (!view_.valid()) return;
            for (size_t i = 0; i < n; ++i) chunk[i] = toCanvas(view_, samples + (done + i) * kSampleFloats);
        }
        doc_.strokeSamples(chunk, n);
        done += n;
    }
    queue_.wake();
}

void Engine::strokeEnd() {
    doc_.strokeEnd();
    queue_.wake();
}

void Engine::strokeCancel() {
    doc_.strokeCancel();
    queue_.wake();
}

size_t Engine::exportCanvasTiles(bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out) {
    return doc_.exportTiles(changedOnly, maxTiles, out);
}

LayerError Engine::layerOp(LayerOp op, uint32_t id, int32_t arg, float value, const std::string& text, uint32_t& outId) {
    LayerError e = LayerError::BadValue;
    switch (op) {
        case LayerOp::Add: e = doc_.addLayer(id, text, outId); break;
        case LayerOp::Delete: e = doc_.deleteLayer(id); break;
        case LayerOp::Duplicate: e = doc_.duplicateLayer(id, text, outId); break;
        case LayerOp::Rename: e = doc_.renameLayer(id, text); break;
        case LayerOp::Move: e = doc_.moveLayer(id, arg); break;
        case LayerOp::Visible: e = doc_.setVisible(id, arg != 0); break;
        case LayerOp::Locked: e = doc_.setLocked(id, arg != 0); break;
        case LayerOp::Opacity: e = doc_.setOpacity(id, value); break;
        case LayerOp::Blend:
            e = arg >= 0 && arg < canvas::kBlendModeCount ? doc_.setBlend(id, static_cast<canvas::BlendMode>(arg))
                                                          : LayerError::BadValue;
            break;
        case LayerOp::Active: e = doc_.setActive(id); break;
        case LayerOp::MergeDown: e = doc_.mergeDown(id); break;
        case LayerOp::Flatten: e = doc_.flatten(text); break;
        case LayerOp::Clear: e = doc_.clear(); break;
    }
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(true);
    });
    return e;
}

int Engine::historyOp(HistoryOp op, int32_t count) {
    int done = 0;
    if (op == HistoryOp::Clear) {
        doc_.clearHistory();
    } else {
        for (int32_t i = 0; i < std::max<int32_t>(1, count); ++i) {
            const int r = op == HistoryOp::Undo ? doc_.undo() : doc_.redo();
            if (r < 0) {
                done = -1;
                break;
            }
            if (r == 0) break;
            done++;
        }
    }
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(true);
    });
    return done;
}

bool Engine::setLayerStack(const std::vector<LayerInfo>& stack, uint32_t activeId) {
    const bool ok = doc_.setLayerStack(stack, activeId);
    queue_.post([this] { scheduler_.requestFrame(); });
    return ok;
}

long Engine::importLayerTiles(uint32_t id, const uint8_t* data, size_t size) {
    const long n = doc_.importLayerTiles(id, data, size);
    queue_.wake();
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(true);
    });
    return n;
}

long Engine::importCanvasTiles(const uint8_t* data, size_t size) {
    const long n = doc_.importTiles(data, size);
    queue_.wake();
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(true);
    });
    return n;
}

bool Engine::captureFrame(gfx::CaptureResult& out, std::chrono::milliseconds timeout) {
    uint64_t serial;
    {
        std::lock_guard<std::mutex> lock(debugCaptureMutex_);
        serial = debugCaptureSerial_;
    }
    if (!queue_.post([this] {
            debugCaptureRequested_ = true;
            scheduler_.requestFrame();
        }))
        return false;
    std::unique_lock<std::mutex> lock(debugCaptureMutex_);
    if (!debugCaptureCv_.wait_for(lock, timeout, [&] { return debugCaptureSerial_ != serial; })) return false;
    if (!debugCaptureOk_) return false;
    out = debugCapture_;
    return true;
}

bool Engine::canvasThumbnail(uint32_t maxSide, uint32_t& width, uint32_t& height, std::vector<uint32_t>& argb) {
    return doc_.thumbnail(maxSide, width, height, argb);
}

void Engine::runCanvasSelfTest() {
    queue_.post([this] {
        if (sceneMode_ != SceneMode::Canvas) {
            PE_LOGW(kTag, "Canvas self-test needs the canvas scene");
            return;
        }
        canvas_.requestSelfTest();
        scheduler_.requestFrame();
        publishStatus(true);
    });
}

ViewportInfo Engine::viewportOp(ViewOp op, const double* a, size_t n) {
    static constexpr size_t kArgs[] = {0, 0, 0, 2, 3, 3, 3, 3, 6};
    const size_t need = kArgs[static_cast<size_t>(op) < 9 ? static_cast<size_t>(op) : 0];
    ViewportInfo info;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        if (op != ViewOp::Query && n >= need && view_.valid()) {
            const uint64_t before = view_.revision();
            switch (op) {
                case ViewOp::Query: break;
                case ViewOp::Fit: view_.fitToScreen(); break;
                case ViewOp::Center: view_.centerCanvas(); break;
                case ViewOp::PanBy: view_.panBy(a[0], a[1]); break;
                case ViewOp::ZoomAt: view_.zoomAt(a[0], {a[1], a[2]}); break;
                case ViewOp::SetZoomAt: view_.setZoomAt(a[0], {a[1], a[2]}); break;
                case ViewOp::RotateAt: view_.rotateAt(a[0], {a[1], a[2]}); break;
                case ViewOp::SetRotationAt: view_.setRotationAt(a[0], {a[1], a[2]}); break;
                case ViewOp::Gesture: view_.applyGesture({a[0], a[1]}, {a[2], a[3]}, a[4], a[5]); break;
            }
            changed = view_.revision() != before;
        }
        info.state = view_.state();
        info.minZoom = view_.valid() ? view_.minZoom() : 0.0;
        info.maxZoom = canvas::Viewport::kMaxZoom;
        info.canvasWidth = view_.canvasWidth();
        info.canvasHeight = view_.canvasHeight();
        info.screenWidth = view_.screenWidth();
        info.screenHeight = view_.screenHeight();
        info.revision = view_.revision();
    }
    if (changed) requestRender();
    return info;
}

bool Engine::sampleColor(double screenX, double screenY, uint32_t& argb) const {
    canvas::Vec2 c;
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        if (!view_.valid()) return false;
        c = view_.toCanvas({screenX, screenY});
    }
    if (!std::isfinite(c.x) || !std::isfinite(c.y)) return false;
    return doc_.sampleColor(static_cast<int32_t>(std::floor(c.x)), static_cast<int32_t>(std::floor(c.y)), argb);
}

// ------------------------------------------------------------------ selection (Block 10)
void Engine::shapePolygon(SelectShape shape, double x0, double y0, double x1, double y1, std::vector<float>& out) {
    canvas::SelectionMask::shapePolygon(shape == SelectShape::Ellipse, x0, y0, x1, y1, out);
}

LayerError Engine::selectShapeCanvas(SelectShape shape, int32_t mode, bool antialias, const float* xy, size_t points) {
    if (mode < 0 || mode > 3 || !xy) return LayerError::BadValue;
    const auto m = static_cast<canvas::SelectionMask::Mode>(mode);
    LayerError e;
    if (shape == SelectShape::Polygon) {
        e = doc_.select(xy, points, m, antialias);
    } else {
        if (points < 2) return LayerError::BadValue;
        std::vector<float> poly;
        shapePolygon(shape, xy[0], xy[1], xy[2], xy[3], poly);
        e = doc_.select(poly.data(), poly.size() / 2, m, antialias && shape == SelectShape::Ellipse);
    }
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(true);
    });
    return e;
}

LayerError Engine::selectShape(SelectShape shape, int32_t mode, bool antialias, const float* screenXY, size_t points) {
    if (!screenXY || points == 0) return LayerError::BadValue;
    canvas::Affine2 m;
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        if (!view_.valid()) return LayerError::NoCanvas;
        m = view_.screenToCanvas();
    }
    std::vector<float> c(points * 2);
    for (size_t i = 0; i < points; ++i) {
        const canvas::Vec2 p = m.apply({screenXY[i * 2], screenXY[i * 2 + 1]});
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return LayerError::BadValue;
        c[i * 2] = static_cast<float>(p.x);
        c[i * 2 + 1] = static_cast<float>(p.y);
    }
    return selectShapeCanvas(shape, mode, antialias, c.data(), points);
}

LayerError Engine::selectionOp(SelectionOp op, int32_t mode, uint32_t& outId) {
    LayerError e = LayerError::BadValue;
    const auto m = static_cast<canvas::SelectionMask::Mode>(std::clamp(mode, 0, 3));
    switch (op) {
        case SelectionOp::SelectAll: e = doc_.selectAll(); break;
        case SelectionOp::Deselect: e = doc_.deselect(); break;
        case SelectionOp::Invert: e = doc_.invertSelection(); break;
        case SelectionOp::FromContent: e = doc_.selectFromContent(m); break;
        case SelectionOp::Clear: e = doc_.clearSelected(); break;
        case SelectionOp::Copy: e = doc_.copySelection(false); break;
        case SelectionOp::CopyMerged: e = doc_.copySelection(true); break;
        case SelectionOp::Cut: e = doc_.cutSelection(); break;
        case SelectionOp::Paste: e = doc_.paste(outId); break;
        case SelectionOp::Duplicate: e = doc_.duplicateSelection(outId); break;
        case SelectionOp::MoveBegin: e = doc_.moveBegin(); break;
        case SelectionOp::MoveCommit: e = doc_.moveCommit(); break;
        case SelectionOp::MoveCancel: doc_.moveCancel(); e = LayerError::None; break;
    }
    queue_.post([this] {
        scheduler_.requestFrame();
        publishStatus(true);
    });
    return e;
}

LayerError Engine::selectionMoveScreen(double sdx, double sdy) {
    canvas::Affine2 m;
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        if (!view_.valid()) return LayerError::NoCanvas;
        m = view_.screenToCanvas();
    }
    // Linear part only: an offset, not a point.
    const double dx = m.a * sdx + m.c * sdy, dy = m.b * sdx + m.d * sdy;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return LayerError::BadValue;
    const LayerError e = doc_.moveTo(static_cast<int32_t>(std::lround(dx)), static_cast<int32_t>(std::lround(dy)));
    if (e == LayerError::None) queue_.post([this] { scheduler_.requestFrame(); });
    return e;
}

LayerError Engine::selectionMoveCanvas(int32_t dx, int32_t dy) {
    const LayerError e = doc_.moveTo(dx, dy);
    if (e == LayerError::None) queue_.post([this] { scheduler_.requestFrame(); });
    return e;
}

bool Engine::viewMatrix(double out[6]) const {
    std::lock_guard<std::mutex> lock(viewMutex_);
    if (!view_.valid()) return false;
    const canvas::Affine2 m = view_.canvasToScreen();
    out[0] = m.a; out[1] = m.b; out[2] = m.c; out[3] = m.d; out[4] = m.tx; out[5] = m.ty;
    return true;
}

canvas::Vec2 Engine::screenToCanvas(canvas::Vec2 p) const {
    std::lock_guard<std::mutex> lock(viewMutex_);
    return view_.valid() ? view_.toCanvas(p) : canvas::Vec2{};
}

canvas::Vec2 Engine::canvasToScreen(canvas::Vec2 p) const {
    std::lock_guard<std::mutex> lock(viewMutex_);
    return view_.valid() ? view_.toScreen(p) : canvas::Vec2{};
}

void Engine::requestRender() {
    queue_.post([this] { scheduler_.requestFrame(); });
}

void Engine::runSelfTest() {
    queue_.post([this] {
        if (sceneMode_ != SceneMode::Diagnostic) {
            selfTest_.status = SelfTestStatus::Unsupported;
            selfTest_.detail = "the triangle self-test runs in the diagnostic scene";
            publishStatus(true);
            return;
        }
        selfTestRequested_ = true;
        scheduler_.requestFrame();
    });
}

EngineStatus Engine::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

std::string Engine::statusText() const { return diagnostics::statusSummary(status()); }

std::string Engine::statusJson() const { return diagnostics::statusJson(status()); }

std::string Engine::capabilityReport() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return publishedReport_;
}

bool Engine::waitForStatus(const std::function<bool(const EngineStatus&)>& predicate,
                           std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(statusMutex_);
    return statusCv_.wait_for(lock, timeout, [&] { return predicate(status_); });
}

// ------------------------------------------------------------ render thread
void Engine::threadMain() {
    renderThreadId_.store(std::this_thread::get_id());
    PE_LOGI(kTag, "Render thread started");
    state_ = "Waiting for surface";
    publishStatus(true);

    while (!quit_) {
        try {
            const bool block = !canRender() && pendingRecovery_.empty();
            if (block && statusDirty_) publishStatus(true);  // never go idle with a stale snapshot
            queue_.waitAndRun(block);
            if (quit_) break;
            if (!pendingRecovery_.empty()) {
                const std::string reason = std::move(pendingRecovery_);
                pendingRecovery_.clear();
                recoverDevice(reason.c_str());
            }
            if (canRender()) renderFrame();
        } catch (const std::exception& e) {
            onRenderThreadException(e.what());
        } catch (...) {
            onRenderThreadException("non-standard exception");
        }
    }

    teardown();
    // Only now may UI-thread waiters assume the window is no longer used.
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    threadState_ = ThreadState::Exited;
    lifecycleCv_.notify_all();
}

void Engine::teardown() {
    // Orderly teardown on the thread that owns the GPU objects:
    // swapchain/EGLSurface -> window reference -> scene resources -> device
    // -> backend (instance / EGLDisplay).
    try {
        detachAndReleaseWindow();
        scene_.destroy();
        destroyDevice();
    } catch (const std::exception& e) {
        PE_LOGE(kTag, "Exception during teardown: %s", e.what());
    } catch (...) {
        PE_LOGE(kTag, "Unknown exception during teardown");
    }
    state_ = "Shut down";
    publishStatus(true);
    PE_LOGI(kTag, "Render thread exiting");
}

bool Engine::canRender() const {
    return selection_ && surfaceAttached_ && !surfaceRevoked_.load() && !paused_ && !renderingDisabled_ &&
           (scheduler_.wantsFrame() || selection_.device->needsFrame() ||
            (sceneMode_ == SceneMode::Canvas &&
             (canvas_.selfTestActive() || doc_.hasPendingInput() || doc_.hasPendingComposite())));
}

void Engine::detachAndReleaseWindow() {
    bool detached = true;
    if (selection_) {
        try {
            selection_.device->detachSurface();  // waits for the GPU, destroys swapchain / EGLSurface
        } catch (const std::exception& e) {
            PE_LOGE(kTag, "detachSurface threw: %s", e.what());
            detached = false;
        }
        if (!detached) {
            // Last resort: destroying the device also destroys the surface.
            try {
                destroyDevice();
                detached = true;
            } catch (...) {
                PE_LOGE(kTag, "destroyDevice threw while recovering from a failed detach");
            }
        }
        if (detached && selection_ && selection_.device->surfaceState().attached) {
            PE_LOGE(kTag, "Invariant violated: device still reports an attached surface after detach");
            detached = false;
        }
    }
    surfaceAttached_ = false;
    if (!window_) return;
    if (detached) {
        window_.reset();  // ANativeWindow_release happens here, after the surface is gone
    } else {
        PE_LOGE(kTag, "Window reference deliberately leaked: its surface could not be destroyed safely");
        (void)window_.release();
    }
}

void Engine::releaseSurfaceForUi() {
    detachAndReleaseWindow();
    surfaceRevoked_ = false;
    PE_LOGI(kTag, "surfaceDestroyed: surface detached, window released");
    publishStatus(true);
}

void Engine::onRenderThreadException(const char* what) {
    consecutiveExceptions_++;
    canvas_.invalidateGpuContent();  // an abandoned frame may have lost recorded uploads
    PE_LOGE(kTag, "Render thread exception #%u: %s", consecutiveExceptions_, what);
    if (consecutiveExceptions_ >= kMaxConsecutiveExceptions) {
        PE_LOGE(kTag, "Too many consecutive exceptions: graphics disabled for this session");
        try {
            scene_.destroy();
            destroyDevice();
        } catch (...) {
            PE_LOGE(kTag, "Exception while disabling graphics");
        }
        renderingDisabled_ = true;
        state_ = "Error: graphics disabled after repeated exceptions";
        publishStatus(true);
        return;
    }
    pendingRecovery_ = std::string("render thread exception: ") + what;
}

bool Engine::selectDevice(std::vector<gfx::BackendType> excluded) {
    if (!window_) return false;
    gfx::SelectionOptions options;
    options.preference = config_.backend;
    options.allowFallback = config_.allowFallback;
    options.enableValidation = config_.enableValidation;
    options.excluded = std::move(excluded);

    selection_ = gfx::BackendSelector::select(options, *window_);
    lastAttempts_ = selection_.attempts;
    if (!selection_) {
        state_ = "Error: no usable graphics backend";
        surfaceAttached_ = false;
        std::string report = "No usable backend.\n";
        for (const auto& a : selection_.attempts)
            report += std::string(gfx::toString(a.type)) + " failed at '" + a.failedStage + "': " + a.reason + "\n";
        capabilityReport_ = report;
        diagnostics::logMultiline("Diagnostics", report);
        return false;
    }
    surfaceAttached_ = true;
    state_ = "Starting";
    capabilityReport_ = diagnostics::capabilityReport(selection_.device->capabilities(),
                                                      selection_.backend->describe(), selection_.attempts);
    diagnostics::logMultiline("Diagnostics", capabilityReport_);
    onSurfaceAvailable();
    return true;
}

void Engine::destroyDevice() {
    if (!selection_) return;
    scene_.destroy();  // release Refs before the device's ResourceManager shuts down
    canvas_.releaseGpu();
    canvasCaptureInFlight_ = false;
    selection_.device->waitIdle();
    selection_.reset();  // device (swapchain/EGLSurface first) then backend
    surfaceAttached_ = false;
}

void Engine::recordBackendFailure(gfx::BackendType failed) {
    const size_t i = static_cast<size_t>(failed);
    const int64_t now = nowNs();
    if (now - lastBackendFailureNs_[i] > kFailureWindowNs) backendFailures_[i] = 0;
    lastBackendFailureNs_[i] = now;
    if (++backendFailures_[i] < 2) return;  // first failure: retry the same backend
    if (std::find(failedBackends_.begin(), failedBackends_.end(), failed) != failedBackends_.end()) return;

    // Exclude it only if something else remains to fall back to: excluding
    // the last candidate would leave the app with no graphics at all.
    gfx::SelectionOptions probe;
    probe.preference = config_.backend;
    probe.allowFallback = config_.allowFallback;
    probe.excluded = failedBackends_;
    probe.excluded.push_back(failed);
    if (gfx::BackendSelector::candidateOrder(probe).empty()) {
        PE_LOGW(kTag, "Backend %s failed repeatedly but is the only one left: retrying it", gfx::toString(failed));
        return;
    }
    failedBackends_.push_back(failed);
    PE_LOGW(kTag, "Backend %s excluded for this session (falling back)", gfx::toString(failed));
}

void Engine::recoverDevice(const char* reason) {
    const gfx::BackendType failed = selection_ ? selection_.device->backendType() : gfx::BackendType::None;
    PE_LOGE(kTag, "Recovering graphics device (%s, backend %s)", reason, gfx::toString(failed));
    deviceRecoveries_++;
    selfTestCaptureInFlight_ = false;
    destroyDevice();

    const int64_t now = nowNs();
    recentRecoveriesNs_.push_back(now);
    recentRecoveriesNs_.erase(std::remove_if(recentRecoveriesNs_.begin(), recentRecoveriesNs_.end(),
                                             [&](int64_t t) { return now - t > kFailureWindowNs; }),
                              recentRecoveriesNs_.end());
    if (recentRecoveriesNs_.size() > kMaxRecoveriesPerWindow) {
        PE_LOGE(kTag, "%zu device recoveries within %lld s: graphics disabled for this session",
                recentRecoveriesNs_.size(), static_cast<long long>(kFailureWindowNs / 1'000'000'000));
        renderingDisabled_ = true;
        state_ = "Error: graphics disabled after repeated device failures";
        publishStatus(true);
        return;
    }

    if (failed != gfx::BackendType::None) recordBackendFailure(failed);
    consecutiveErrors_ = 0;
    if (window_ && !surfaceRevoked_) selectDevice(failedBackends_);
    publishStatus(true);
}

void Engine::onSurfaceAvailable() {
    scheduler_.markAttach(nowNs());
    scheduler_.requestFrame();
    consecutiveOutOfDate_ = 0;
    if (config_.diagnostics && config_.autoSelfTest && sceneMode_ == SceneMode::Diagnostic) selfTestRequested_ = true;
}

void Engine::renderFrame() {
    gfx::GraphicsDevice& device = *selection_.device;
    const int64_t tBegin = nowNs();
    const uint64_t serial = scheduler_.beginFrame(tBegin);
    gfx::FrameContext frame;
    gfx::FrameStatus status = device.beginFrame(serial, frame);
    const int64_t tAcquired = nowNs();  // fence wait + image acquire (GPU / vsync)
    if (status != gfx::FrameStatus::Ok) {
        scheduler_.endFrameSkipped(status);
        handleFrameStatus(status);
        publishStatus(false);
        return;
    }
    consecutiveOutOfDate_ = 0;

    const gfx::TextureFormat fmt = frame.backbuffer->colorFormat();
    bool sceneOk = false;
    if (sceneMode_ == SceneMode::Canvas) {
        canvas::Viewport view;
        {
            std::lock_guard<std::mutex> lock(viewMutex_);
            view = view_;
        }
        sceneOk = canvas_.render(device, *frame.context, *frame.backbuffer, view);
        if (debugCaptureRequested_ && !canvas_.selfTestActive() && !canvasCaptureInFlight_) {
            debugCaptureRequested_ = false;
            debugCaptureInFlight_ = device.requestCapture();
            if (!debugCaptureInFlight_) {
                std::lock_guard<std::mutex> lock(debugCaptureMutex_);
                debugCaptureOk_ = false;
                debugCaptureSerial_++;
                debugCaptureCv_.notify_all();
            }
        }
        if (canvas_.selfTestWantsCapture() && !canvasCaptureInFlight_ && !debugCaptureInFlight_) {
            if (device.requestCapture())
                canvasCaptureInFlight_ = true;
            else
                canvas_.onSelfTestUnsupported("backbuffer readback not supported by this surface");
        }
    } else if ((sceneOk = scene_.isReadyFor(fmt) || scene_.create(device, fmt))) {
        scene_.render(*frame.context, *frame.backbuffer);
    } else {
        // Still close the frame properly (acquired image must be presented).
        gfx::RenderPassDesc pass;
        pass.target = frame.backbuffer;
        pass.clearColor = {0.5f, 0.0f, 0.0f, 1.0f};
        frame.context->beginRenderPass(pass);
        frame.context->endRenderPass();
    }

    if (selfTestRequested_ && sceneOk && sceneMode_ == SceneMode::Diagnostic) {
        selfTestRequested_ = false;
        selfTest_.runs++;
        if (device.requestCapture()) {
            selfTestCaptureInFlight_ = true;
            selfTest_.status = SelfTestStatus::Pending;
            selfTest_.detail.clear();
        } else {
            selfTest_.status = SelfTestStatus::Unsupported;
            selfTest_.detail = "backbuffer readback not supported by this surface";
        }
    }

    (void)PE_TEST_HOOK("engine.frame.recorded");  // timing hook only
    if (PE_TEST_HOOK("engine.frame.throw")) throw std::runtime_error("injected render-thread exception");

    const int64_t tRecorded = nowNs();
    status = device.endFrame();
    const int64_t tPresented = nowNs();
    if (status == gfx::FrameStatus::Ok && PE_TEST_HOOK("engine.frame.device_lost")) {
        PE_LOGE(kTag, "Injected device loss (test hook)");
        status = gfx::FrameStatus::DeviceLost;
    }
    if (status == gfx::FrameStatus::Ok) {
        if (sceneMode_ == SceneMode::Canvas) canvas_.onFramePresented(tPresented);
        scheduler_.endFramePresented(tPresented, nsToMs(tAcquired - tBegin), nsToMs(tRecorded - tAcquired),
                                     nsToMs(tPresented - tRecorded));
        consecutiveErrors_ = 0;
        consecutiveExceptions_ = 0;
        state_ = sceneOk ? "Running"
                         : sceneMode_ == SceneMode::Canvas ? "Error: canvas pipeline creation failed"
                                                           : "Error: test scene creation failed";
        if (debugCaptureInFlight_) {
            debugCaptureInFlight_ = false;
            std::lock_guard<std::mutex> lock(debugCaptureMutex_);
            debugCaptureOk_ = device.takeCapture(debugCapture_);
            debugCaptureSerial_++;
            debugCaptureCv_.notify_all();
        }
        if (canvasCaptureInFlight_) {
            canvasCaptureInFlight_ = false;
            gfx::CaptureResult capture;
            const bool produced = device.takeCapture(capture);
            canvas_.onSelfTestCapture(produced, capture);
            scheduler_.requestFrame();  // next step, or redraw the user's canvas
            publishStatus(true);
        }
        if (selfTestCaptureInFlight_) {
            selfTestCaptureInFlight_ = false;
            gfx::CaptureResult capture;
            const uint32_t runs = selfTest_.runs;
            if (device.takeCapture(capture)) {
                selfTest_ = TestScene::verify(capture);
            } else {
                selfTest_.status = SelfTestStatus::Failed;
                selfTest_.detail = "capture was not produced";
            }
            selfTest_.runs = runs;
            if (selfTest_.status == SelfTestStatus::Passed)
                PE_LOGI(kTag, "Self-test PASSED: %s", selfTest_.detail.c_str());
            else
                PE_LOGE(kTag, "Self-test %s: %s", toString(selfTest_.status), selfTest_.detail.c_str());
            publishStatus(true);
        }
        logPerfIfDue();
    } else {
        selfTestCaptureInFlight_ = false;
        if (debugCaptureInFlight_) {
            debugCaptureInFlight_ = false;
            debugCaptureRequested_ = true;  // try again on the next frame
        }
        // The frame's uploads may not have executed: rebuild the GPU copy.
        if (status == gfx::FrameStatus::Error || status == gfx::FrameStatus::DeviceLost)
            canvas_.invalidateGpuContent();
        if (canvasCaptureInFlight_) {
            canvasCaptureInFlight_ = false;
            canvas_.retrySelfTestStep();  // frame not presented: render this step again
        }
        scheduler_.endFrameSkipped(status);
        handleFrameStatus(status);
    }
    publishStatus(false);
}

void Engine::handleFrameStatus(gfx::FrameStatus status) {
    switch (status) {
        case gfx::FrameStatus::Ok:
        case gfx::FrameStatus::NoSurface: return;
        case gfx::FrameStatus::SurfaceOutOfDate:
            // Rebuild happens at the next beginFrame. A zero-sized or busy
            // window must not turn the render loop into a busy spin.
            if (++consecutiveOutOfDate_ > 2) std::this_thread::sleep_for(std::chrono::milliseconds(8));
            return;
        case gfx::FrameStatus::SurfaceLost:
            PE_LOGW(kTag, "Surface lost: detaching and trying to re-attach once");
            selection_.device->detachSurface();
            surfaceAttached_ = window_ && !surfaceRevoked_ && selection_.device->attachSurface(*window_);
            if (surfaceAttached_)
                onSurfaceAvailable();
            else
                state_ = "Surface lost (waiting for a new one)";
            publishStatus(true);
            return;
        case gfx::FrameStatus::DeviceLost: recoverDevice("device lost"); return;
        case gfx::FrameStatus::Error:
            if (++consecutiveErrors_ >= kMaxConsecutiveErrors) recoverDevice("repeated frame errors");
            return;
    }
}

void Engine::logPerfIfDue() {
    if (!config_.diagnostics) return;
    const int64_t now = nowNs();
    if (lastPerfLogNs_ == 0) {
        lastPerfLogNs_ = now;
        lastPerfLogFrames_ = scheduler_.stats().framesPresented;
        return;
    }
    if (now - lastPerfLogNs_ < kPerfLogPeriodNs) return;
    const auto& st = scheduler_.stats();
    const double seconds = nsToMs(now - lastPerfLogNs_) / 1000.0;
    const uint64_t rendered = st.framesPresented - lastPerfLogFrames_;
    char rate[64];
    if (scheduler_.mode() == gfx::RenderMode::Continuous && st.rateValid)
        std::snprintf(rate, sizeof(rate), "%.1f fps", st.fps);
    else
        std::snprintf(rate, sizeof(rate), "rate n/a (%s)", gfx::toString(scheduler_.mode()));
    PE_LOGI("Perf",
            "%s: %llu frames in %.1fs | CPU work avg %.2f ms (record %.2f, submit/present %.2f), max %.2f | "
            "wait GPU/vsync avg %.2f ms | total rendered %llu, skipped %llu",
            rate, static_cast<unsigned long long>(rendered), seconds, st.steadyAvgCpuMs, st.steadyAvgRecordMs,
            st.steadyAvgSubmitMs, st.steadyMaxCpuMs, st.steadyAvgWaitMs,
            static_cast<unsigned long long>(st.framesPresented), static_cast<unsigned long long>(st.framesSkipped));
    lastPerfLogNs_ = now;
    lastPerfLogFrames_ = st.framesPresented;
}

void Engine::publishStatus(bool force) {
    const int64_t now = nowNs();
    if (!force && now - lastPublishNs_ < 250'000'000) {
        statusDirty_ = true;
        return;
    }
    lastPublishNs_ = now;
    statusDirty_ = false;

    EngineStatus s;
    const bool terminal = state_.rfind("Error", 0) == 0 || state_ == "Shut down";
    if (terminal)
        s.state = state_;
    else if (!selection_)
        s.state = window_ ? "Initializing" : "Waiting for surface";
    else if (!surfaceAttached_)
        s.state = "No surface (device kept)";
    else if (paused_)
        s.state = "Paused";
    else
        s.state = state_;
    s.paused = paused_;
    if (selection_) {
        const auto& caps = selection_.device->capabilities();
        s.backend = selection_.device->backendType();
        s.backendDescription = selection_.backend->describe();
        s.gpu = caps.deviceName + " (" + caps.vendorName + ")";
        s.api = caps.apiVersion();
        s.driver = caps.driverVersion;
        s.surface = selection_.device->surfaceState();
        s.resources = selection_.device->resources().stats();
        s.validationLayerActive = caps.featureUsable("Validation layer");
        for (const auto& a : lastAttempts_) s.backendInitMs += a.durationMs;
    }
    s.attempts = lastAttempts_;
    s.windowKind = window_ ? toString(window_->kind()) : "none";
    s.frames = scheduler_.stats();
    s.renderMode = scheduler_.mode();
    s.diagnostics = config_.diagnostics;
    s.selfTest = selfTest_;
    s.deviceRecoveries = deviceRecoveries_;
    s.warnings = log::warningCount(LogSource::Engine);
    s.errors = log::errorCount(LogSource::Engine);
    s.validationErrors = log::errorCount(LogSource::ValidationLayer);
    s.validationWarnings = log::warningCount(LogSource::ValidationLayer);
    s.driverDebugMessages =
        log::errorCount(LogSource::DriverDebug) + log::warningCount(LogSource::DriverDebug);
    s.recentProblems = log::recentProblems(4);
    canvas_.fillStatus(s.canvas);
    s.canvas.scene = toString(sceneMode_);
    if (selection_) {
        const auto limits =
            CanvasController::limitsFor(selection_.device->capabilities(), config_.canvasMemoryBudgetBytes);
        s.canvas.maxTextureSize = limits.maxTextureSize;
        s.canvas.memoryBudgetBytes = limits.memoryBudgetBytes;
    }
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = std::move(s);
        publishedReport_ = capabilityReport_;
    }
    statusCv_.notify_all();
}

}  // namespace pe
