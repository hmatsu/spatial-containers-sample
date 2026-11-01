// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstring>

#include "graphics.h"

namespace {

class HeadlessGraphics : public Graphics {
public:
    std::vector<const char*> RequiredExtensions() const override { return {}; }
    bool Initialize(XrInstance, XrSystemId) override { return true; }
    const void* SessionBinding() const override { return nullptr; }
    bool CreateSwapchain(XrSession, int, int) override { return false; }
    XrSwapchain Swapchain() const override { return XR_NULL_HANDLE; }
    bool Upload(const Image&) override { return false; }
    void DestroySwapchain() override {}
};

}  // namespace

std::unique_ptr<Graphics> CreateHeadlessGraphics() {
    return std::make_unique<HeadlessGraphics>();
}

SwapchainFormat ChooseSwapchainFormat(const std::vector<int64_t>& available, const std::vector<int64_t>& rgbaPreferred,
                                      const std::vector<int64_t>& bgraPreferred) {
    // Preference lists alternate RGBA and BGRA at each quality level (sRGB, then UNORM).
    for (size_t i = 0; i < std::max(rgbaPreferred.size(), bgraPreferred.size()); ++i) {
        if (i < rgbaPreferred.size() && std::find(available.begin(), available.end(), rgbaPreferred[i]) != available.end()) {
            return {rgbaPreferred[i], false};
        }
        if (i < bgraPreferred.size() && std::find(available.begin(), available.end(), bgraPreferred[i]) != available.end()) {
            return {bgraPreferred[i], true};
        }
    }
    return {};
}

void CopyPixels(const Image& image, bool bgra, uint8_t* out, size_t outRowPitch) {
    for (int y = 0; y < image.height(); ++y) {
        const uint8_t* src = image.data() + static_cast<size_t>(y) * image.rowPitch();
        uint8_t* dst = out + static_cast<size_t>(y) * outRowPitch;
        if (!bgra) {
            std::memcpy(dst, src, image.rowPitch());
            continue;
        }
        for (int x = 0; x < image.width(); ++x) {
            dst[4 * x + 0] = src[4 * x + 2];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x + 0];
            dst[4 * x + 3] = src[4 * x + 3];
        }
    }
}
