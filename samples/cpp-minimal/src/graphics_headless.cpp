// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT

#include "graphics.h"

namespace {

class HeadlessGraphics : public Graphics {
public:
    std::vector<const char*> RequiredExtensions() const override { return {}; }
    bool Initialize(XrInstance, XrSystemId) override { return true; }
    const void* SessionBinding() const override { return nullptr; }
    bool CreateSwapchains(XrSession, const std::vector<XrViewConfigurationView>&) override { return false; }
    bool RenderView(uint32_t, const XrView&, XrExtent2Di, const Scene&, XrCompositionLayerProjectionView*) override {
        return false;
    }
    void DestroySwapchains() override {}
};

}  // namespace

std::unique_ptr<Graphics> CreateHeadlessGraphics() {
    return std::make_unique<HeadlessGraphics>();
}
