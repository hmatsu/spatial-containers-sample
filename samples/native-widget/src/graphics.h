// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// What the widget needs from a graphics API: a swapchain for the panel and a way to
// copy a CPU image into it. The panel is shown as a quad layer, so there is no 3D
// rendering.

#pragma once

#include <openxr/openxr.h>

#include <memory>
#include <vector>

#include "panel.h"

class Graphics {
public:
    virtual ~Graphics() = default;

    // Instance extensions the backend needs.
    virtual std::vector<const char*> RequiredExtensions() const = 0;

    // Called after the system is known. Returns false on failure.
    virtual bool Initialize(XrInstance instance, XrSystemId systemId) = 0;

    // Graphics binding to chain to XrSessionCreateInfo, or nullptr for none.
    virtual const void* SessionBinding() const = 0;

    // Creates the panel swapchain. Returns false if the backend cannot present.
    virtual bool CreateSwapchain(XrSession session, int width, int height) = 0;
    virtual XrSwapchain Swapchain() const = 0;

    // Copies `image` into the next swapchain image: acquire, wait, copy, release.
    virtual bool Upload(const Image& image) = 0;

    virtual void DestroySwapchain() = 0;
};

// Presents nothing. Used to exercise the container lifecycle without a GPU, e.g.
// against the emulation layer's mock runtime.
std::unique_ptr<Graphics> CreateHeadlessGraphics();

#if defined(WIDGET_HAS_VULKAN)
std::unique_ptr<Graphics> CreateVulkanGraphics();
#endif
#if defined(_WIN32)
std::unique_ptr<Graphics> CreateD3D11Graphics();
#endif

// Picks the 8-bit RGBA or BGRA format to use from the runtime's list (sRGB first),
// and whether the panel's RGBA bytes must be swizzled to BGRA. Returns 0 if none fits.
struct SwapchainFormat {
    int64_t format = 0;
    bool bgra = false;
};
SwapchainFormat ChooseSwapchainFormat(const std::vector<int64_t>& available, const std::vector<int64_t>& rgbaPreferred,
                                      const std::vector<int64_t>& bgraPreferred);

// Copies RGBA rows into `out`, swizzling to BGRA if asked.
void CopyPixels(const Image& image, bool bgra, uint8_t* out, size_t outRowPitch);
