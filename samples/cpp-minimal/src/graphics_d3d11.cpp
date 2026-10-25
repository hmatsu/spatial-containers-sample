// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Direct3D 11 backend: a spinning colored cube and a wireframe of the container bounds.

#if defined(_WIN32)

#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "graphics.h"

using Microsoft::WRL::ComPtr;

namespace {

const char kShaders[] = R"(
cbuffer Constants : register(b0) {
    float4x4 ModelViewProjection;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};
VSOutput VSMain(float3 position : POSITION, float4 color : COLOR) {
    VSOutput output;
    output.position = mul(ModelViewProjection, float4(position, 1.0));
    output.color = color;
    return output;
}
float4 PSMain(VSOutput input) : SV_TARGET {
    return input.color;
}
)";

struct Vertex {
    float position[3];
    float color[4];
};

// 36 vertices for the solid cube, followed by 24 for the wireframe box. Both unit size.
std::vector<Vertex> BuildVertices() {
    std::vector<Vertex> vertices;
    const float h = 0.5f;
    struct Face {
        float normal[3], u[3], v[3], color[4];
    } faces[] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0.90f, 0.35f, 0.30f, 1}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0.55f, 0.20f, 0.18f, 1}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, {0.35f, 0.80f, 0.40f, 1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {0.20f, 0.45f, 0.22f, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0.30f, 0.50f, 0.90f, 1}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {0.18f, 0.28f, 0.55f, 1}},
    };
    for (const Face& f : faces) {
        auto corner = [&](float a, float b) {
            Vertex vertex{};
            for (int i = 0; i < 3; ++i) vertex.position[i] = (f.normal[i] + a * f.u[i] + b * f.v[i]) * h;
            std::memcpy(vertex.color, f.color, sizeof(f.color));
            return vertex;
        };
        // Counter-clockwise seen from outside.
        Vertex quad[4] = {corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1)};
        for (int index : {0, 1, 2, 0, 2, 3}) vertices.push_back(quad[index]);
    }
    const float cyan[4] = {0.3f, 0.8f, 1.0f, 1.0f};
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < 4; ++i) {
            for (float end : {-h, h}) {
                Vertex vertex{};
                vertex.position[axis] = end;
                vertex.position[(axis + 1) % 3] = (i & 1) ? h : -h;
                vertex.position[(axis + 2) % 3] = (i & 2) ? h : -h;
                std::memcpy(vertex.color, cyan, sizeof(cyan));
                vertices.push_back(vertex);
            }
        }
    }
    return vertices;
}

constexpr UINT kCubeVertexCount = 36;
constexpr UINT kBoxVertexCount = 24;

class D3D11Graphics : public Graphics {
public:
    std::vector<const char*> RequiredExtensions() const override { return {XR_KHR_D3D11_ENABLE_EXTENSION_NAME}; }

    bool Initialize(XrInstance instance, XrSystemId systemId) override {
        instance_ = instance;
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
        return CreatePipeline();
    }

    const void* SessionBinding() const override { return &binding_; }

    bool CreateSwapchains(XrSession session, const std::vector<XrViewConfigurationView>& views) override {
        uint32_t count = 0;
        xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
        format_ = DXGI_FORMAT_UNKNOWN;
        for (int64_t candidate : preferred) {
            if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
                format_ = static_cast<DXGI_FORMAT>(candidate);
                break;
            }
        }
        if (format_ == DXGI_FORMAT_UNKNOWN) return false;

        for (const XrViewConfigurationView& view : views) {
            ViewTarget target;
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            info.format = format_;
            info.sampleCount = 1;
            info.width = view.recommendedImageRectWidth;
            info.height = view.recommendedImageRectHeight;
            info.faceCount = 1;
            info.arraySize = 1;
            info.mipCount = 1;
            if (XR_FAILED(xrCreateSwapchain(session, &info, &target.swapchain))) return false;
            target.width = info.width;
            target.height = info.height;
            xrEnumerateSwapchainImages(target.swapchain, 0, &count, nullptr);
            target.images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            xrEnumerateSwapchainImages(target.swapchain, count, &count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(target.images.data()));

            D3D11_TEXTURE2D_DESC depthDesc{};
            depthDesc.Width = info.width;
            depthDesc.Height = info.height;
            depthDesc.MipLevels = 1;
            depthDesc.ArraySize = 1;
            depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            depthDesc.SampleDesc.Count = 1;
            depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            ComPtr<ID3D11Texture2D> depth;
            if (FAILED(device_->CreateTexture2D(&depthDesc, nullptr, &depth))) return false;
            if (FAILED(device_->CreateDepthStencilView(depth.Get(), nullptr, &target.depthView))) return false;
            targets_.push_back(std::move(target));
        }
        return true;
    }

    bool RenderView(uint32_t viewIndex, const XrView& view, XrExtent2Di extent, const Scene& scene,
                    XrCompositionLayerProjectionView* projectionView) override {
        if (viewIndex >= targets_.size()) return false;
        ViewTarget& target = targets_[viewIndex];
        extent.width = std::min<int32_t>(extent.width, static_cast<int32_t>(target.width));
        extent.height = std::min<int32_t>(extent.height, static_cast<int32_t>(target.height));

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(target.swapchain, &acquireInfo, &imageIndex))) return false;
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrWaitSwapchainImage(target.swapchain, &waitInfo))) return false;

        ID3D11RenderTargetView* colorView = ColorView(target.images[imageIndex].texture);
        const float transparent[4] = {0, 0, 0, 0};
        context_->ClearRenderTargetView(colorView, transparent);
        context_->ClearDepthStencilView(target.depthView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->OMSetRenderTargets(1, &colorView, target.depthView.Get());

        const Mat4 viewProjection = Multiply(ProjectionFov(view.fov, 0.05f, 100.0f), InvertRigid(FromPose(view.pose)));
        const UINT stride = sizeof(Vertex), offset = 0;
        context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->OMSetDepthStencilState(depthState_.Get(), 0);
        context_->RSSetState(rasterizer_.Get());

        Draw(Multiply(viewProjection, scene.cubeModel), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, kCubeVertexCount, 0);
        if (scene.drawBox) {
            Draw(Multiply(viewProjection, scene.boxModel), D3D11_PRIMITIVE_TOPOLOGY_LINELIST, kBoxVertexCount,
                 kCubeVertexCount);
        }

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(target.swapchain, &releaseInfo);

        projectionView->pose = view.pose;
        projectionView->fov = view.fov;
        projectionView->subImage.swapchain = target.swapchain;
        projectionView->subImage.imageRect = {{0, 0}, extent};
        projectionView->subImage.imageArrayIndex = 0;
        return true;
    }

    void DestroySwapchains() override {
        for (ViewTarget& target : targets_) xrDestroySwapchain(target.swapchain);
        targets_.clear();
        colorViews_.clear();
    }

private:
    struct ViewTarget {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        std::vector<XrSwapchainImageD3D11KHR> images;
        ComPtr<ID3D11DepthStencilView> depthView;
    };

    bool CreatePipeline() {
        ComPtr<ID3DBlob> vsBlob, psBlob, errors;
        if (FAILED(D3DCompile(kShaders, sizeof(kShaders), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob,
                              &errors)) ||
            FAILED(D3DCompile(kShaders, sizeof(kShaders), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob,
                              &errors))) {
            if (errors) std::fprintf(stderr, "D3D11: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            return false;
        }
        device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader_);
        device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader_);
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        device_->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_);

        const std::vector<Vertex> vertices = BuildVertices();
        D3D11_BUFFER_DESC vbDesc{
            static_cast<UINT>(vertices.size() * sizeof(Vertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA vbData{vertices.data(), 0, 0};
        device_->CreateBuffer(&vbDesc, &vbData, &vertexBuffer_);
        D3D11_BUFFER_DESC cbDesc{sizeof(Mat4), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        device_->CreateBuffer(&cbDesc, nullptr, &constants_);

        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
        device_->CreateDepthStencilState(&depthDesc, &depthState_);
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        // No culling: the cube is closed and depth tested, and this avoids depending on
        // how the winding order maps through the projection and viewport.
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        device_->CreateRasterizerState(&rasterDesc, &rasterizer_);
        return vertexShader_ && pixelShader_ && inputLayout_ && vertexBuffer_ && constants_ && depthState_ && rasterizer_;
    }

    ID3D11RenderTargetView* ColorView(ID3D11Texture2D* texture) {
        ComPtr<ID3D11RenderTargetView>& view = colorViews_[texture];
        if (!view) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = format_;
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            device_->CreateRenderTargetView(texture, &desc, &view);
        }
        return view.Get();
    }

    void Draw(const Mat4& modelViewProjection, D3D11_PRIMITIVE_TOPOLOGY topology, UINT count, UINT first) {
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &modelViewProjection, 0, 0);
        context_->IASetPrimitiveTopology(topology);
        context_->Draw(count, first);
    }

    XrInstance instance_ = XR_NULL_HANDLE;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    XrGraphicsBindingD3D11KHR binding_{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    std::vector<ViewTarget> targets_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11RenderTargetView>> colorViews_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11DepthStencilState> depthState_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
};

}  // namespace

std::unique_ptr<Graphics> CreateD3D11Graphics() {
    return std::make_unique<D3D11Graphics>();
}

#endif  // _WIN32
