#include "canvas/CanvasSurface.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/Log.h"
#include "core/Time.h"

namespace pe::canvas {

namespace {
constexpr const char* kTag = "Canvas";

double msSince(int64_t t0) { return static_cast<double>(nowNs() - t0) / 1e6; }
}  // namespace

Rgba8 Rgba8::fromArgb(uint32_t argb) {
    const uint32_t a = (argb >> 24) & 0xFF;
    auto pm = [a](uint32_t c) { return static_cast<uint8_t>((c * a + 127) / 255); };
    return {pm((argb >> 16) & 0xFF), pm((argb >> 8) & 0xFF), pm(argb & 0xFF), static_cast<uint8_t>(a)};
}

gfx::Ref<gfx::TextureResource> CanvasSurface::createTexture(uint32_t w, uint32_t h, const std::string& name) {
    gfx::TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = gfx::TextureFormat::RGBA8_UNORM;
    d.usage = gfx::TextureUsage::Sampled;
    d.linearFilter = true;
    d.debugName = name;
    auto t = device_->createTexture(d);
    if (t) {
        stats_.tilesAllocated++;
        stats_.bytesAllocated += static_cast<uint64_t>(w) * h * 4u;
    }
    return t;
}

bool CanvasSurface::create(gfx::GraphicsDevice& device, uint32_t width, uint32_t height, const StoragePlan& plan,
                           std::string debugName) {
    destroy();
    if (!plan.ok) {
        PE_LOGE(kTag, "Surface '%s' %ux%u: storage plan rejected (%s)", debugName.c_str(), width, height,
                plan.reason.c_str());
        return false;
    }
    device_ = &device;
    name_ = std::move(debugName);
    stats_ = {};
    stats_.mode = plan.mode;
    if (plan.mode == StorageMode::SingleTexture) {
        // The whole canvas is one tile without gutter (CLAMP_TO_EDGE handles the border).
        grid_ = TileGrid(width, height, std::max(width, height));
        stats_.tilesTotal = 1;
        textures_.resize(1);
        textures_[0] = createTexture(width, height, name_ + ".single");
        if (!textures_[0]) {
            PE_LOGE(kTag, "Surface '%s': %ux%u texture allocation failed", name_.c_str(), width, height);
            destroy();
            return false;
        }
    } else {
        grid_ = TileGrid(width, height, plan.tileSize);
        stats_.tileSize = plan.tileSize;
        stats_.tilesTotal = static_cast<uint32_t>(grid_.tileCount());
        textures_.resize(grid_.tileCount());  // allocated on first write
    }
    PE_LOGI(kTag, "Surface '%s' %ux%u: %s, %u tile(s), %llu KiB allocated now", name_.c_str(), width, height,
            toString(plan.mode), stats_.tilesTotal, static_cast<unsigned long long>(stats_.bytesAllocated >> 10));
    return true;
}

void CanvasSurface::destroy() {
    textures_.clear();  // Refs: the device's ResourceManager defers the actual destruction
    scratch_.clear();
    scratch_.shrink_to_fit();
    device_ = nullptr;
    grid_ = TileGrid();
    stats_.tilesAllocated = 0;
    stats_.bytesAllocated = 0;
}

bool CanvasSurface::tileAllocated(TileCoord t) const {
    if (!valid()) return false;
    if (stats_.mode == StorageMode::SingleTexture) return static_cast<bool>(textures_[0]);
    return t.tx >= 0 && t.ty >= 0 && t.tx < grid_.columns() && t.ty < grid_.rows() &&
           static_cast<bool>(textures_[grid_.index(t)]);
}

bool CanvasSurface::fill(const RectI& rect, Rgba8 color) {
    const RectI c = rect.intersect(grid_.canvasRect());
    if (c.empty()) return true;
    std::vector<uint8_t> px(static_cast<size_t>(c.w) * c.h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = color.r;
        px[i + 1] = color.g;
        px[i + 2] = color.b;
        px[i + 3] = color.a;
    }
    return write(c, px.data());
}

bool CanvasSurface::write(const RectI& rect, const uint8_t* rgba) {
    if (!valid() || !rgba) return false;
    const RectI clipped = rect.intersect(grid_.canvasRect());
    if (clipped.empty()) return true;
    const int64_t t0 = nowNs();
    bool ok = true;
    const uint64_t uploadsBefore = stats_.textureUploads;

    if (stats_.mode == StorageMode::SingleTexture) {
        const uint8_t* src = rgba;
        if (!(clipped == rect)) {
            scratch_.resize(static_cast<size_t>(clipped.w) * clipped.h * 4);
            for (int32_t y = 0; y < clipped.h; ++y) {
                const size_t srcOff =
                    (static_cast<size_t>(clipped.y - rect.y + y) * rect.w + (clipped.x - rect.x)) * 4;
                std::memcpy(&scratch_[static_cast<size_t>(y) * clipped.w * 4], rgba + srcOff,
                            static_cast<size_t>(clipped.w) * 4);
            }
            src = scratch_.data();
        }
        ok = device_->updateTexture(*textures_[0], static_cast<uint32_t>(clipped.x), static_cast<uint32_t>(clipped.y),
                                    static_cast<uint32_t>(clipped.w), static_cast<uint32_t>(clipped.h), src);
        if (ok) {
            stats_.textureUploads++;
            stats_.bytesUploaded += static_cast<uint64_t>(clipped.area()) * 4u;
        }
    } else {
        // Texels to write: the clipped rect, plus the outer gutter where it
        // touches the canvas border (clamp-to-edge emulation).
        const RectI canvas = grid_.canvasRect();
        RectI grown = clipped;
        if (clipped.x == 0) { grown.x -= TileGrid::kGutter; grown.w += TileGrid::kGutter; }
        if (clipped.y == 0) { grown.y -= TileGrid::kGutter; grown.h += TileGrid::kGutter; }
        if (clipped.right() == canvas.right()) grown.w += TileGrid::kGutter;
        if (clipped.bottom() == canvas.bottom()) grown.h += TileGrid::kGutter;
        for (const TileCoord t : grid_.tilesStoring(grown)) {
            if (!writeTile(t, grown, rect, rgba)) ok = false;
        }
    }
    if (stats_.textureUploads != uploadsBefore) stats_.writes++;
    stats_.lastWriteMs = msSince(t0);
    stats_.maxWriteMs = std::max(stats_.maxWriteMs, stats_.lastWriteMs);
    return ok;
}

bool CanvasSurface::writeTile(TileCoord t, const RectI& grown, const RectI& src, const uint8_t* rgba) {
    const RectI storage = grid_.tileStorageRect(t);
    const RectI region = storage.intersect(grown);  // texels of this tile, in canvas coordinates
    if (region.empty()) return true;
    auto& tex = textures_[grid_.index(t)];
    if (!tex) {
        char name[64];
        std::snprintf(name, sizeof(name), ".tile[%d,%d]", t.tx, t.ty);
        tex = createTexture(grid_.textureSize(), grid_.textureSize(), name_ + name);
        if (!tex) {
            PE_LOGE(kTag, "Surface '%s': tile [%d,%d] allocation failed", name_.c_str(), t.tx, t.ty);
            return false;
        }
    }
    const int32_t maxX = static_cast<int32_t>(grid_.canvasWidth()) - 1;
    const int32_t maxY = static_cast<int32_t>(grid_.canvasHeight()) - 1;
    scratch_.resize(static_cast<size_t>(region.w) * region.h * 4);
    for (int32_t y = 0; y < region.h; ++y) {
        const int32_t cy = std::clamp(region.y + y, 0, maxY);  // gutter outside the canvas repeats the border
        const uint8_t* srcRow = rgba + static_cast<size_t>(cy - src.y) * src.w * 4;
        uint8_t* dst = &scratch_[static_cast<size_t>(y) * region.w * 4];
        const int32_t x0 = region.x, x1 = region.right();
        int32_t x = x0;
        // Left outer gutter (only at the canvas border).
        for (; x < x1 && x < 0; ++x, dst += 4) std::memcpy(dst, srcRow + (0 - src.x) * 4, 4);
        const int32_t inner = std::min(x1, maxX + 1) - x;
        if (inner > 0) {
            std::memcpy(dst, srcRow + static_cast<size_t>(x - src.x) * 4, static_cast<size_t>(inner) * 4);
            dst += static_cast<size_t>(inner) * 4;
            x += inner;
        }
        for (; x < x1; ++x, dst += 4) std::memcpy(dst, srcRow + static_cast<size_t>(maxX - src.x) * 4, 4);
    }
    const bool ok = device_->updateTexture(*tex, static_cast<uint32_t>(region.x - storage.x),
                                           static_cast<uint32_t>(region.y - storage.y),
                                           static_cast<uint32_t>(region.w), static_cast<uint32_t>(region.h),
                                           scratch_.data());
    if (ok) {
        stats_.textureUploads++;
        stats_.bytesUploaded += static_cast<uint64_t>(region.area()) * 4u;
    }
    return ok;
}

uint32_t CanvasSurface::collectVisible(const RectI& visible, std::vector<Piece>& out) const {
    if (!valid()) return 0;
    const RectI v = visible.intersect(grid_.canvasRect());
    if (v.empty()) return 0;
    if (stats_.mode == StorageMode::SingleTexture) {
        if (textures_[0]) out.push_back({grid_.canvasRect(), 0.0f, 0.0f, 1.0f, 1.0f, textures_[0].get()});
        return 1;
    }
    const auto tiles = grid_.tilesIntersecting(v);
    const float inv = 1.0f / static_cast<float>(grid_.textureSize());
    for (const TileCoord t : tiles) {
        const auto& tex = textures_[grid_.index(t)];
        if (!tex) continue;
        const RectI r = grid_.tileRect(t);
        const RectI s = grid_.tileStorageRect(t);
        Piece p;
        p.canvasRect = r;
        p.u0 = static_cast<float>(r.x - s.x) * inv;
        p.v0 = static_cast<float>(r.y - s.y) * inv;
        p.u1 = static_cast<float>(r.right() - s.x) * inv;
        p.v1 = static_cast<float>(r.bottom() - s.y) * inv;
        p.texture = tex.get();
        out.push_back(p);
    }
    return static_cast<uint32_t>(tiles.size());
}

}  // namespace pe::canvas
