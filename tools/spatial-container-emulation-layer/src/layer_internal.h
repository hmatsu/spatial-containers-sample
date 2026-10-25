// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Stateless parts of the emulation layer, kept separate so they can be unit tested:
// configuration parsing, next-chain handling, layer copies and view fitting.

#pragma once

#include <openxr/openxr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

namespace scemu {

// --- Configuration ---------------------------------------------------------------

struct Config {
    XrExtent3Df bounds = {0.5f, 0.5f, 0.5f};
    bool boundsFromEnv = false;
    XrVector3f position = {0.0f, 0.0f, -1.0f};
    uint32_t maxContainers = 16;
    bool supportsBounded = true;
    bool supportsImmersive = true;
    bool denyVisibleRequests = false;
    bool denyBoundsModeRequests = false;
    bool tightFov = true;
    bool log = false;
};

inline bool ParseFloat3(const char* text, float out[3]) {
    if (text == nullptr) return false;
    float v[3];
    if (std::sscanf(text, "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) return false;
    for (float f : v) {
        if (!std::isfinite(f)) return false;
    }
    std::memcpy(out, v, sizeof(v));
    return true;
}

// Looks up a setting by name; returns nullptr when unset.
using SettingLookup = const char* (*)(const char* name);

inline bool Flag(SettingLookup lookup, const char* name) {
    const char* value = lookup(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

inline Config ParseConfig(SettingLookup lookup) {
    Config config;
    float v[3];
    if (ParseFloat3(lookup("XR_SCEMU_BOUNDS"), v) && v[0] > 0 && v[1] > 0 && v[2] > 0) {
        config.bounds = {v[0], v[1], v[2]};
        config.boundsFromEnv = true;
    }
    if (ParseFloat3(lookup("XR_SCEMU_POSITION"), v)) {
        config.position = {v[0], v[1], v[2]};
    }
    if (const char* modes = lookup("XR_SCEMU_SUPPORTED_MODES")) {
        config.supportsBounded = std::strstr(modes, "bounded") != nullptr;
        config.supportsImmersive = std::strstr(modes, "immersive") != nullptr;
        if (!config.supportsBounded && !config.supportsImmersive) {
            config.supportsBounded = config.supportsImmersive = true;
        }
    }
    if (const char* max = lookup("XR_SCEMU_MAX_CONTAINERS")) {
        config.maxContainers = static_cast<uint32_t>(std::strtoul(max, nullptr, 10));
    }
    config.denyVisibleRequests = Flag(lookup, "XR_SCEMU_DENY_VISIBLE_REQUESTS");
    config.denyBoundsModeRequests = Flag(lookup, "XR_SCEMU_DENY_BOUNDS_MODE_REQUESTS");
    if (lookup("XR_SCEMU_TIGHT_FOV") != nullptr) config.tightFov = Flag(lookup, "XR_SCEMU_TIGHT_FOV");
    config.log = Flag(lookup, "XR_SCEMU_LOG");
    return config;
}

// --- Next chains -----------------------------------------------------------------

template <typename T>
const T* FindInChain(const void* next, XrStructureType type) {
    for (auto* it = static_cast<const XrBaseInStructure*>(next); it != nullptr; it = it->next) {
        if (it->type == type) return reinterpret_cast<const T*>(it);
    }
    return nullptr;
}

// Temporarily removes structures of the given types from a next chain owned by the
// app, restoring the chain when destroyed. Runtimes that do not know a structure
// would otherwise see it.
class ScopedChainUnlink {
public:
    ScopedChainUnlink(const void* head, std::initializer_list<XrStructureType> types) {
        auto* prev = const_cast<XrBaseInStructure*>(static_cast<const XrBaseInStructure*>(head));
        while (prev != nullptr && prev->next != nullptr) {
            auto* current = const_cast<XrBaseInStructure*>(prev->next);
            if (std::find(types.begin(), types.end(), current->type) != types.end()) {
                removed_.emplace_back(prev, current);
                prev->next = current->next;
            } else {
                prev = current;
            }
        }
    }
    ~ScopedChainUnlink() {
        for (auto it = removed_.rbegin(); it != removed_.rend(); ++it) {
            it->first->next = it->second;
        }
    }
    ScopedChainUnlink(const ScopedChainUnlink&) = delete;
    ScopedChainUnlink& operator=(const ScopedChainUnlink&) = delete;

private:
    std::vector<std::pair<XrBaseInStructure*, XrBaseInStructure*>> removed_;
};

// --- Layer copies ----------------------------------------------------------------

// Deep copy of submitted composition layers, so that "retain" can resubmit them
// after the app's structures are gone.
struct LayerCopy {
    LayerCopy() = default;
    // projection.views points into projectionViews, which a copy would not update.
    LayerCopy(const LayerCopy&) = delete;
    LayerCopy& operator=(const LayerCopy&) = delete;
    LayerCopy(LayerCopy&&) noexcept = default;
    LayerCopy& operator=(LayerCopy&&) noexcept = default;

    XrStructureType type = XR_TYPE_UNKNOWN;
    XrCompositionLayerProjection projection{};
    std::vector<XrCompositionLayerProjectionView> projectionViews;
    XrCompositionLayerQuad quad{};

    const XrCompositionLayerBaseHeader* Header() const {
        if (type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        }
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
};

// Copies a layer for submission. With forRetain, pointers into app memory that may
// not outlive this frame (next chains) are dropped.
inline bool CopyLayer(const XrCompositionLayerBaseHeader* layer, bool forRetain, LayerCopy* out) {
    out->type = layer->type;
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const auto* src = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        out->projection = *src;
        out->projectionViews.assign(src->views, src->views + src->viewCount);
        if (forRetain) {
            out->projection.next = nullptr;
            for (auto& view : out->projectionViews) view.next = nullptr;
        }
        out->projection.views = out->projectionViews.data();
        return true;
    }
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        out->quad = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
        if (forRetain) out->quad.next = nullptr;
        return true;
    }
    return false;
}

// --- View fitting ----------------------------------------------------------------

inline XrVector3f Cross(const XrVector3f& a, const XrVector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f c = Cross(u, v);
    const XrVector3f t{2 * c.x, 2 * c.y, 2 * c.z};
    const XrVector3f c2 = Cross(u, t);
    return {v.x + q.w * t.x + c2.x, v.y + q.w * t.y + c2.y, v.z + q.w * t.z + c2.z};
}

inline XrPosef Inverse(const XrPosef& p) {
    const XrQuaternionf q{-p.orientation.x, -p.orientation.y, -p.orientation.z, p.orientation.w};
    const XrVector3f t = Rotate(q, {-p.position.x, -p.position.y, -p.position.z});
    return {q, t};
}

// a * b: applies b first, then a.
inline XrPosef Multiply(const XrPosef& a, const XrPosef& b) {
    const XrQuaternionf& p = a.orientation;
    const XrQuaternionf& q = b.orientation;
    const XrQuaternionf r{p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y, p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x,
                          p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w, p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z};
    const XrVector3f t = Rotate(p, b.position);
    return {r, {a.position.x + t.x, a.position.y + t.y, a.position.z + t.z}};
}

enum class FitResult : uint8_t { Fitted, Unchanged, Outside };

// Narrows a view's FOV to the smallest frustum that contains the container's bounding
// box, as the spec recommends. This also limits the app's rendering to the box's 2D
// pixel bounds (as a rectangle). `scale` returns the fitted size relative to the full
// FOV, per axis.
inline FitResult FitFov(const XrPosef& containerInView, const XrExtent3Df& bounds, XrFovf* fov, float scale[2]) {
    constexpr float kNear = 0.01f;
    float minX = INFINITY, maxX = -INFINITY, minY = INFINITY, maxY = -INFINITY;
    for (int i = 0; i < 8; ++i) {
        const XrVector3f corner{(i & 1 ? 0.5f : -0.5f) * bounds.width, (i & 2 ? 0.5f : -0.5f) * bounds.height,
                                (i & 4 ? 0.5f : -0.5f) * bounds.depth};
        const XrVector3f r = Rotate(containerInView.orientation, corner);
        const XrVector3f c{r.x + containerInView.position.x, r.y + containerInView.position.y,
                           r.z + containerInView.position.z};
        // A corner at or behind the eye: no finite frustum contains the box.
        if (c.z > -kNear) return FitResult::Unchanged;
        minX = std::min(minX, c.x / -c.z);
        maxX = std::max(maxX, c.x / -c.z);
        minY = std::min(minY, c.y / -c.z);
        maxY = std::max(maxY, c.y / -c.z);
    }
    const float left = std::tan(fov->angleLeft), right = std::tan(fov->angleRight);
    const float down = std::tan(fov->angleDown), up = std::tan(fov->angleUp);
    const float l = std::max(minX, left), r = std::min(maxX, right);
    const float d = std::max(minY, down), u = std::min(maxY, up);
    if (l >= r || d >= u) return FitResult::Outside;
    *fov = {std::atan(l), std::atan(r), std::atan(u), std::atan(d)};
    scale[0] = (r - l) / (right - left);
    scale[1] = (u - d) / (up - down);
    return FitResult::Fitted;
}

}  // namespace scemu
