#include "engine/CanvasController.h"

#include <algorithm>
#include <cmath>

#include "core/Log.h"
#include "core/Time.h"

namespace pe {

namespace {
constexpr const char* kTag = "Canvas";
constexpr uint64_t kMiB = 1024ull * 1024ull;

gfx::ClearColor clearFromArgb(uint32_t argb) {
    return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, 1.0f};
}
}  // namespace

canvas::StorageLimits CanvasController::limitsFor(const gfx::GraphicsCapabilities& caps, uint64_t budgetOverride) {
    canvas::StorageLimits l;
    l.maxTextureSize = caps.limits.maxTextureSize2D;
    // Budget for canvas pixels: a quarter of device-local memory (on phones
    // this is shared system RAM), clamped; GLES cannot report memory, so a
    // conservative fixed budget is used there.
    if (budgetOverride > 0)
        l.memoryBudgetBytes = budgetOverride;
    else if (caps.memory.known && caps.memory.deviceLocalBytes > 0)
        l.memoryBudgetBytes = std::clamp<uint64_t>(caps.memory.deviceLocalBytes / 4, 128 * kMiB, 1024 * kMiB);
    else
        l.memoryBudgetBytes = 512 * kMiB;
    // Vulkan: one VkDeviceMemory per texture in this version, so the tile
    // count must stay below maxMemoryAllocationCount (with a reserve).
    if (caps.limits.maxMemoryAllocationCount > 512) {
        const uint64_t tex = canvas::StoragePolicy::kTileSize + 2u * canvas::TileGrid::kGutter;
        const uint64_t byCount = static_cast<uint64_t>(caps.limits.maxMemoryAllocationCount - 256) * tex * tex * 4u;
        l.memoryBudgetBytes = std::min(l.memoryBudgetBytes, byCount);
    }
    return l;
}

void CanvasController::open(const CanvasSpec& spec) {
    surface_.destroy();
    open_ = true;
    spec_ = spec;
    journal_.clear();
    pending_.clear();
    gpuFailed_ = false;
    error_.clear();
    storageText_.clear();
    restores_ = 0;
    frame_ = {};
    setPaper(spec.paperArgb, spec.transparentBackground);
    PE_LOGI(kTag, "Canvas opened: %ux%u, paper #%08X%s", spec.width, spec.height, spec.paperArgb,
            spec.transparentBackground ? " (transparent background)" : "");
}

void CanvasController::close() {
    if (!open_) return;
    surface_.destroy();
    open_ = false;
    journal_.clear();
    pending_.clear();
    error_.clear();
    storageText_.clear();
    frame_ = {};
    PE_LOGI(kTag, "Canvas closed");
}

void CanvasController::setWorkspaceColor(uint32_t argb) { look_.workspace = clearFromArgb(argb); }

void CanvasController::setPaper(uint32_t argb, bool transparentBackground) {
    look_.paper = canvas::Rgba8::fromArgb(argb | 0xFF000000u);  // paper itself is opaque
    look_.transparentBackground = transparentBackground;
    spec_.paperArgb = argb;
    spec_.transparentBackground = transparentBackground;
}

canvas::RectI CanvasController::stampRect(double cx, double cy, uint32_t size, uint32_t canvasWidth,
                                          uint32_t canvasHeight) {
    const canvas::RectI canvasRect{0, 0, static_cast<int32_t>(canvasWidth), static_cast<int32_t>(canvasHeight)};
    const int32_t half = static_cast<int32_t>(size / 2);
    return canvas::RectI{static_cast<int32_t>(std::floor(cx)) - half, static_cast<int32_t>(std::floor(cy)) - half,
                         static_cast<int32_t>(size), static_cast<int32_t>(size)}
        .intersect(canvasRect);
}

bool CanvasController::stamp(double cx, double cy, uint32_t size, uint32_t argb) {
    if (!open_) return false;
    return fill(stampRect(cx, cy, size, spec_.width, spec_.height), argb);
}

bool CanvasController::fill(const canvas::RectI& rect, uint32_t argb) {
    if (!open_) return false;
    const canvas::RectI r =
        rect.intersect({0, 0, static_cast<int32_t>(spec_.width), static_cast<int32_t>(spec_.height)});
    if (r.empty()) return false;
    if (journal_.size() >= kMaxJournalOps) {
        PE_LOGW(kTag, "Content journal full (%zu operations): stamp ignored", journal_.size());
        return false;
    }
    const FillOp op{r, canvas::Rgba8::fromArgb(argb)};
    journal_.push_back(op);
    pending_.push_back(op);
    return true;
}

void CanvasController::clearContent() {
    journal_.clear();
    pending_.clear();
    surface_.destroy();  // recreated empty (and with no texture allocated) at the next frame
}

void CanvasController::releaseGpu() {
    if (testStep_ >= 0) abortSelfTest("graphics device released");
    surface_.destroy();
    testTiled_.destroy();
    testSingle_.destroy();
    renderer_.destroy();
    gpuFailed_ = false;  // a different device may succeed
}

bool CanvasController::ensureGpu(gfx::GraphicsDevice& device, gfx::TextureFormat format) {
    if (!renderer_.isReadyFor(format) && !renderer_.create(device, format)) return false;
    if (!open_ || surface_.valid() || gpuFailed_) return true;

    const canvas::StoragePlan plan =
        canvas::StoragePolicy::choose(spec_.width, spec_.height, limitsFor(device.capabilities(), budgetOverride_));
    storageText_ = plan.reason;
    if (!plan.ok || !surface_.create(device, spec_.width, spec_.height, plan, "canvas")) {
        gpuFailed_ = true;
        error_ = plan.ok ? "GPU storage allocation failed" : "canvas not supported on this device: " + plan.reason;
        // A refused size is a user-facing outcome, not an engine failure.
        if (plan.ok)
            PE_LOGE(kTag, "%s", error_.c_str());
        else
            PE_LOGW(kTag, "%s", error_.c_str());
        return true;  // the frame still renders (workspace + paper)
    }
    error_.clear();
    if (!journal_.empty()) {
        // New device / backend: rebuild the content from the journal.
        const int64_t t0 = nowNs();
        for (const auto& op : journal_) surface_.fill(op.rect, op.color);
        restores_++;
        PE_LOGI(kTag, "Canvas content restored from %zu journal operations in %.1f ms", journal_.size(),
                static_cast<double>(nowNs() - t0) / 1e6);
    }
    pending_.clear();
    return true;
}

bool CanvasController::render(gfx::GraphicsDevice& device, gfx::RenderingContext& ctx, gfx::RenderTarget& target,
                              const canvas::Viewport& viewport) {
    if (!ensureGpu(device, target.colorFormat())) {
        gfx::RenderPassDesc pass;
        pass.target = &target;
        pass.clearColor = look_.workspace;
        ctx.beginRenderPass(pass);
        ctx.endRenderPass();
        return false;
    }
    // Uploads happen before the render pass is recorded.
    if (surface_.valid()) {
        for (const auto& op : pending_) surface_.fill(op.rect, op.color);
    }
    pending_.clear();

    if (testStep_ >= 0) {
        using canvas::CanvasSelfTest;
        if (testStep_ == 0 && !testTiled_.valid()) {
            canvas::StorageLimits forced = limitsFor(device.capabilities(), budgetOverride_);
            forced.forceTiled = true;
            const auto tiledPlan = canvas::StoragePolicy::choose(CanvasSelfTest::kWidth, CanvasSelfTest::kHeight, forced);
            const auto singlePlan = canvas::StoragePolicy::choose(CanvasSelfTest::kWidth, CanvasSelfTest::kHeight,
                                                                  limitsFor(device.capabilities(), budgetOverride_));
            const bool ok = tiledPlan.ok && singlePlan.ok && singlePlan.mode == canvas::StorageMode::SingleTexture &&
                            testTiled_.create(device, CanvasSelfTest::kWidth, CanvasSelfTest::kHeight, tiledPlan,
                                              "selftest.tiled") &&
                            testSingle_.create(device, CanvasSelfTest::kWidth, CanvasSelfTest::kHeight, singlePlan,
                                               "selftest.single") &&
                            CanvasSelfTest::paintBase(testTiled_) && CanvasSelfTest::paintBase(testSingle_);
            if (!ok) {
                testDetail_ = "could not create the calibration canvas";
                testFailed_ = true;
                finishSelfTest();
            }
        }
        if (testStep_ == 2 && !testLateApplied_) {
            // Partial update of textures that earlier frames already sampled.
            if (!CanvasSelfTest::paintLate(testTiled_) || !CanvasSelfTest::paintLate(testSingle_)) {
                testDetail_ += "late partial update failed; ";
                testFailed_ = true;
            }
            testLateApplied_ = true;
        }
    }
    if (testStep_ >= 0) {
        using canvas::CanvasSelfTest;
        testViewport_ = CanvasSelfTest::viewportFor(testStep_, target.extent().width, target.extent().height);
        canvas::CanvasFrameStats st;
        renderer_.render(ctx, target, testViewport_, CanvasSelfTest::appearance(),
                         CanvasSelfTest::tiledStep(testStep_) ? &testTiled_ : &testSingle_, st);
        testRendered_ = true;
        return true;
    }

    zoom_ = viewport.zoom();
    rotation_ = viewport.rotation();
    if (open_ && viewport.valid()) {
        renderer_.render(ctx, target, viewport, look_, surface_.valid() ? &surface_ : nullptr, frame_);
    } else {
        frame_ = {};
        renderer_.renderEmpty(ctx, target, look_);
    }
    return true;
}

void CanvasController::requestSelfTest() {
    if (testStep_ >= 0) return;  // already running
    testStep_ = 0;
    testRendered_ = false;
    testLateApplied_ = false;
    testFailed_ = false;
    testDetail_.clear();
    selfTest_.status = SelfTestStatus::Pending;
    selfTest_.detail.clear();
    selfTest_.runs++;
    PE_LOGI(kTag, "Canvas self-test started (%d steps)", canvas::CanvasSelfTest::kSteps);
}

void CanvasController::onSelfTestCapture(bool produced, const gfx::CaptureResult& capture) {
    if (testStep_ < 0) return;
    testRendered_ = false;
    if (!produced) {
        testDetail_ += "step " + std::to_string(testStep_) + ": capture was not produced; ";
        testFailed_ = true;
    } else {
        const auto r = canvas::CanvasSelfTest::verify(testStep_, capture, testViewport_);
        testDetail_ += r.detail + "; ";
        if (!r.passed) testFailed_ = true;
        (r.passed ? PE_LOGI(kTag, "Canvas self-test %s", r.detail.c_str())
                  : PE_LOGE(kTag, "Canvas self-test %s", r.detail.c_str()));
    }
    if (++testStep_ >= canvas::CanvasSelfTest::kSteps) finishSelfTest();
}

void CanvasController::onSelfTestUnsupported(const char* why) {
    if (testStep_ < 0) return;
    selfTest_.status = SelfTestStatus::Unsupported;
    selfTest_.detail = why;
    testStep_ = -1;
    testRendered_ = false;
    testTiled_.destroy();
    testSingle_.destroy();
    PE_LOGW(kTag, "Canvas self-test unsupported: %s", why);
}

void CanvasController::abortSelfTest(const char* why) {
    if (testStep_ < 0) return;
    testDetail_ += std::string("interrupted at step ") + std::to_string(testStep_) + " (" + why + "), run it again";
    testFailed_ = true;
    finishSelfTest();
}

void CanvasController::finishSelfTest() {
    selfTest_.status = testFailed_ ? SelfTestStatus::Failed : SelfTestStatus::Passed;
    if (!testDetail_.empty() && testDetail_.size() >= 2 && testDetail_.compare(testDetail_.size() - 2, 2, "; ") == 0)
        testDetail_.resize(testDetail_.size() - 2);
    selfTest_.detail = testDetail_;
    testStep_ = -1;
    testRendered_ = false;
    testTiled_.destroy();
    testSingle_.destroy();
    if (testFailed_)
        PE_LOGE(kTag, "Canvas self-test FAILED: %s", selfTest_.detail.c_str());
    else
        PE_LOGI(kTag, "Canvas self-test PASSED");
}

void CanvasController::fillStatus(CanvasStatus& c) const {
    c.open = open_;
    c.gpuReady = surface_.valid();
    c.width = open_ ? spec_.width : 0;
    c.height = open_ ? spec_.height : 0;
    c.storage = storageText_;
    c.error = error_;
    const auto& s = surface_.stats();
    c.tileSize = s.tileSize;
    c.tilesTotal = s.tilesTotal;
    c.tilesAllocated = s.tilesAllocated;
    c.bytesAllocated = s.bytesAllocated;
    c.writes = s.writes;
    c.textureUploads = s.textureUploads;
    c.bytesUploaded = s.bytesUploaded;
    c.lastWriteMs = s.lastWriteMs;
    c.maxWriteMs = s.maxWriteMs;
    c.journalOps = journal_.size();
    c.restores = restores_;
    c.tilesVisible = frame_.tilesVisible;
    c.piecesDrawn = frame_.piecesDrawn;
    c.drawCalls = frame_.drawCalls;
    c.zoom = zoom_;
    c.rotationDeg = rotation_ * 180.0 / canvas::kPi;
    c.selfTest = selfTest_;
}

}  // namespace pe
