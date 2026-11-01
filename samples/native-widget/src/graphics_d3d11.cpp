// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Direct3D 11 backend (XR_KHR_D3D11_enable): copies the panel image into the
// swapchain texture with UpdateSubresource.

#if defined(_WIN32)

#define NOMINMAX
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>

#include "graphics.h"

using Microsoft::WRL::ComPtr;

namespace {

class D3D11Graphics : public Graphics {
public:
    std::vector<const char*> RequiredExtensions() const override { return {XR_KHR_D3D11_ENABLE_EXTENSION_NAME}; }

    bool Initialize(XrInstance instance, XrSystemId systemId) override {
        PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
        if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
                                            reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)))) {
            return false;
        }
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (XR_FAILED(getRequirements(instance, systemId, &requirements))) return false;

        // Use the adapter the runtime asks for.
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (std::memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0) break;
            adapter.Reset();
        }
        if (!adapter) {
            std::fprintf(stderr, "D3D11: adapter requested by the runtime not found\n");
            return false;
        }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        UINT levelCount = 0;
        while (levelCount < 4 && levels[levelCount] >= requirements.minFeatureLevel) ++levelCount;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     levels, levelCount, D3D11_SDK_VERSION, &device_, nullptr, &context_))) {
            return false;
        }
        binding_.device = device_.Get();
        return true;
    }

    const void* SessionBinding() const override { return &binding_; }

    bool CreateSwapchain(XrSession session, int width, int height) override {
        uint32_t count = 0;
        xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        const SwapchainFormat chosen =
            ChooseSwapchainFormat(formats, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM},
                                  {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM});
        if (chosen.format == 0) {
            std::fprintf(stderr, "D3D11: no 8-bit RGBA or BGRA swapchain format\n");
            return false;
        }
        bgra_ = chosen.bgra;

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags =
            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = chosen.format;
        info.sampleCount = 1;
        info.width = static_cast<uint32_t>(width);
        info.height = static_cast<uint32_t>(height);
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(session, &info, &swapchain_))) return false;
        xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr);
        images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(swapchain_, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));
        width_ = width;
        height_ = height;
        pixels_.resize(static_cast<size_t>(width) * height * 4);
        return true;
    }

    XrSwapchain Swapchain() const override { return swapchain_; }

    bool Upload(const Image& image) override {
        if (swapchain_ == XR_NULL_HANDLE || image.width() != width_ || image.height() != height_) return false;
        CopyPixels(image, bgra_, pixels_.data(), image.rowPitch());

        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &acquireInfo, &index))) return false;
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrWaitSwapchainImage(swapchain_, &waitInfo))) return false;
        context_->UpdateSubresource(images_[index].texture, 0, nullptr, pixels_.data(), static_cast<UINT>(image.rowPitch()),
                                    0);
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        return XR_SUCCEEDED(xrReleaseSwapchainImage(swapchain_, &releaseInfo));
    }

    void DestroySwapchain() override {
        if (swapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(swapchain_);
        swapchain_ = XR_NULL_HANDLE;
        images_.clear();
    }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    XrGraphicsBindingD3D11KHR binding_{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    std::vector<uint8_t> pixels_;
    int width_ = 0, height_ = 0;
    bool bgra_ = false;
};

}  // namespace

std::unique_ptr<Graphics> CreateD3D11Graphics() {
    return std::make_unique<D3D11Graphics>();
}

#endif  // _WIN32
