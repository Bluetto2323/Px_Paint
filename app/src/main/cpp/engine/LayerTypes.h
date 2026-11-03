// PxPaint - layer types shared by the document and its history (Block 6-7).
#pragma once

#include <cstdint>
#include <string>

#include "canvas/Compositor.h"
#include "canvas/RasterStore.h"

namespace pe {

struct LayerInfo {
    uint32_t id = 0;
    std::string name;
    bool visible = true;
    bool locked = false;
    float opacity = 1.0f;
    canvas::BlendMode blend = canvas::BlendMode::Normal;
};

struct LayerSummary {
    LayerInfo info;
    size_t tiles = 0;        // allocated tiles (content)
    uint64_t version = 0;    // changes with the layer's pixels
};

enum class LayerError : int32_t {
    None = 0,
    NoCanvas = 1,
    NotFound = 2,
    LastLayer = 3,     // the only layer cannot be deleted
    NoLayerBelow = 4,  // merge down of the bottom layer
    Locked = 5,
    Hidden = 6,
    TooMany = 7,
    Memory = 8,
    BadValue = 9,
};
const char* layerErrorName(LayerError e);

// A layer: properties + its own pixels.
struct LayerData {
    LayerInfo info;
    canvas::RasterStore store;
};

}  // namespace pe
