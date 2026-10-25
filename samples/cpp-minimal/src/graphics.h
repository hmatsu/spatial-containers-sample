// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// The graphics side of the sample: everything the OpenXR logic in main.cpp needs
// from a graphics API, so that logic stays the same for every backend.

#pragma once

#include <openxr/openxr.h>

#include <memory>
#include <string>
#include <vector>

#include "xr_math.h"

// What to draw for one view, in the space the views were located in.
struct Scene {
    Mat4 boxModel;   // Unit cube scaled to the container bounds (wireframe).
    Mat4 cubeModel;  // Unit cube for the spinning solid cube.
    bool drawBox = false;
};

class Graphics {
public:
    virtual ~Graphics() = default;

    // Instance extensions the backend needs.
    virtual std::vector<const char*> RequiredExtensions() const = 0;

    // Called after the system is known. Returns false on failure.
    virtual bool Initialize(XrInstance instance, XrSystemId systemId) = 0;

    // Graphics binding to chain to XrSessionCreateInfo, or nullptr for none.
    virtual const void* SessionBinding() const = 0;

    // Creates one swapchain per view. Returns false if the backend cannot render.
    virtual bool CreateSwapchains(XrSession session, const std::vector<XrViewConfigurationView>& views) = 0;

    // Renders one view into the top-left `extent` of its swapchain image and fills the
    // projection view's subImage. The background is transparent.
    virtual bool RenderView(uint32_t viewIndex, const XrView& view, XrExtent2Di extent, const Scene& scene,
                            XrCompositionLayerProjectionView* projectionView) = 0;

    virtual void DestroySwapchains() = 0;
};

// Renders nothing and submits no layers. Used to exercise the container lifecycle
// without a GPU, e.g. against the emulation layer's mock runtime.
std::unique_ptr<Graphics> CreateHeadlessGraphics();

#if defined(_WIN32)
std::unique_ptr<Graphics> CreateD3D11Graphics();
#endif
