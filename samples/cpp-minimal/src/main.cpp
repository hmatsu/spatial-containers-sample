// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Minimal spatial container app using only the OpenXR SDK.
//
// It follows the lifecycle described in XR_EXT_spatial_container:
//   create container -> request visible -> on VisibleChanged(true) begin rendering
//   -> render per container -> request hidden -> on VisibleChanged(false) end
//   rendering -> destroy container.
// A spinning cube is drawn inside a wireframe of the container bounds, and the app
// switches between bounded and immersive modes on a timer.
//
// Usage: cpp_minimal [--headless] [--seconds N] [--toggle-seconds N]

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <openxr/openxr.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "graphics.h"

namespace {

#define CHECK_XR(call)                                                              \
    do {                                                                            \
        const XrResult result_ = (call);                                            \
        if (XR_FAILED(result_)) {                                                   \
            std::fprintf(stderr, "%s failed: %d (%s:%d)\n", #call, result_, __FILE__, __LINE__); \
            return false;                                                           \
        }                                                                           \
    } while (0)

struct Options {
    bool headless = false;
    double seconds = 0.0;  // 0: run until the container is closed.
    double toggleSeconds = 8.0;
};

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--seconds" && i + 1 < argc) {
            options.seconds = std::atof(argv[++i]);
        } else if (arg == "--toggle-seconds" && i + 1 < argc) {
            options.toggleSeconds = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: %s [--headless] [--seconds N] [--toggle-seconds N]\n", argv[0]);
            std::exit(2);
        }
    }
    return options;
}

XrQuaternionf AxisAngle(float x, float y, float z, float angle) {
    const float s = std::sin(angle * 0.5f);
    return {x * s, y * s, z * s, std::cos(angle * 0.5f)};
}

XrQuaternionf MultiplyQuat(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

const char* ModeName(XrSpatialContainerBoundsModeEXT mode) {
    return mode == XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT ? "immersive" : "bounded";
}

class App {
public:
    App(Graphics& graphics, const Options& options) : graphics_(graphics), options_(options) {}

    bool Initialize() {
        if (!CreateInstance()) return false;
        if (!graphics_.Initialize(instance_, systemId_)) {
            std::fprintf(stderr, "Graphics initialization failed\n");
            return false;
        }
        return CreateSession() && CreateContainer();
    }

    void Run() {
        const auto start = std::chrono::steady_clock::now();
        auto nextToggle = start + Seconds(options_.toggleSeconds);
        while (!done_) {
            PollEvents();
            const auto now = std::chrono::steady_clock::now();
            if (options_.seconds > 0 && now - start > Seconds(options_.seconds) && !quitting_) RequestQuit();
            if (options_.toggleSeconds > 0 && now > nextToggle && visible_ && !quitting_) {
                ToggleBoundsMode();
                nextToggle = now + Seconds(options_.toggleSeconds);
            }
            if (sessionRunning_ && !done_) {
                if (!RenderFrame()) break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    void Shutdown() {
        if (rendering_) EndRendering();
        if (container_ != XR_NULL_HANDLE) ext_.DestroyContainer(container_);
        graphics_.DestroySwapchains();
        if (containerSpace_ != XR_NULL_HANDLE) xrDestroySpace(containerSpace_);
        if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
        if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
        std::printf("Shut down after %u frames (%u with layers submitted)\n", frameCount_, submittedFrameCount_);
    }

private:
    static std::chrono::steady_clock::duration Seconds(double s) {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s));
    }

    struct Extensions {
        PFN_xrCreateSpatialContainerEXT CreateContainer = nullptr;
        PFN_xrDestroySpatialContainerEXT DestroyContainer = nullptr;
        PFN_xrCreateSpatialContainerSpaceEXT CreateContainerSpace = nullptr;
        PFN_xrRequestSpatialContainerVisibleEXT RequestVisible = nullptr;
        PFN_xrRequestSpatialContainerBoundsModeEXT RequestBoundsMode = nullptr;
        PFN_xrGetSpatialContainerBoundsEXT GetBounds = nullptr;
        PFN_xrBeginSpatialContainerRenderingEXT BeginRendering = nullptr;
        PFN_xrEndSpatialContainerRenderingEXT EndRendering = nullptr;
        PFN_xrLocateSpatialContainerViewsEXT LocateViews = nullptr;
    };

    template <typename T>
    bool Load(const char* name, T* out) {
        CHECK_XR(xrGetInstanceProcAddr(instance_, name, reinterpret_cast<PFN_xrVoidFunction*>(out)));
        return true;
    }

    bool CreateInstance() {
        std::vector<const char*> extensions = graphics_.RequiredExtensions();
        extensions.push_back(XR_EXT_SPATIAL_CONTAINER_EXTENSION_NAME);
        extensions.push_back(XR_EXT_SPATIAL_CONTAINER_SELF_RENDERING_EXTENSION_NAME);

        uint32_t count = 0;
        CHECK_XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
        std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
        CHECK_XR(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()));
        for (const char* name : extensions) {
            const bool found = std::any_of(available.begin(), available.end(),
                                           [&](const XrExtensionProperties& p) { return std::strcmp(p.extensionName, name) == 0; });
            if (!found) {
                std::fprintf(stderr,
                             "Required extension %s is not available. On runtimes without spatial container support, "
                             "enable XR_APILAYER_spatial_container_emulation.\n",
                             name);
                return false;
            }
        }

        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(info.applicationInfo.applicationName, "spatial-container-cpp-minimal");
        info.applicationInfo.applicationVersion = 1;
        info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.enabledExtensionNames = extensions.data();
        CHECK_XR(xrCreateInstance(&info, &instance_));

        if (!Load("xrCreateSpatialContainerEXT", &ext_.CreateContainer) ||
            !Load("xrDestroySpatialContainerEXT", &ext_.DestroyContainer) ||
            !Load("xrCreateSpatialContainerSpaceEXT", &ext_.CreateContainerSpace) ||
            !Load("xrRequestSpatialContainerVisibleEXT", &ext_.RequestVisible) ||
            !Load("xrRequestSpatialContainerBoundsModeEXT", &ext_.RequestBoundsMode) ||
            !Load("xrGetSpatialContainerBoundsEXT", &ext_.GetBounds) ||
            !Load("xrBeginSpatialContainerRenderingEXT", &ext_.BeginRendering) ||
            !Load("xrEndSpatialContainerRenderingEXT", &ext_.EndRendering) ||
            !Load("xrLocateSpatialContainerViewsEXT", &ext_.LocateViews)) {
            return false;
        }

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        CHECK_XR(xrGetSystem(instance_, &systemInfo, &systemId_));

        XrSystemSpatialContainerPropertiesEXT containerProps{XR_TYPE_SYSTEM_SPATIAL_CONTAINER_PROPERTIES_EXT};
        XrSystemProperties systemProps{XR_TYPE_SYSTEM_PROPERTIES, &containerProps};
        CHECK_XR(xrGetSystemProperties(instance_, systemId_, &systemProps));
        supportsBounded_ = containerProps.supportsBounded;
        supportsImmersive_ = containerProps.supportsImmersive;
        std::printf("System: %s, max containers %u, bounded %d, immersive %d\n", systemProps.systemName,
                    containerProps.maxSpatialContainerCount, supportsBounded_, supportsImmersive_);
        if (containerProps.maxSpatialContainerCount == 0) {
            std::fprintf(stderr, "The runtime cannot create spatial containers\n");
            return false;
        }

        CHECK_XR(xrEnumerateViewConfigurations(instance_, systemId_, 0, &count, nullptr));
        std::vector<XrViewConfigurationType> configs(count);
        CHECK_XR(xrEnumerateViewConfigurations(instance_, systemId_, count, &count, configs.data()));
        if (configs.empty()) return false;
        viewConfig_ = configs[0];
        CHECK_XR(xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfig_, 0, &count, nullptr));
        configViews_.assign(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CHECK_XR(xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfig_, count, &count, configViews_.data()));
        views_.assign(count, {XR_TYPE_VIEW});
        projectionViews_.assign(count, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrEnvironmentBlendMode modes[8];
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance_, systemId_, viewConfig_, 8, &count, modes)) && count > 0) {
            blendMode_ = modes[0];
        }
        return true;
    }

    bool CreateSession() {
        XrSessionCreateInfoSpatialContainersEXT enableContainers{XR_TYPE_SESSION_CREATE_INFO_SPATIAL_CONTAINERS_EXT};
        enableContainers.next = graphics_.SessionBinding();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO, &enableContainers};
        info.systemId = systemId_;
        CHECK_XR(xrCreateSession(instance_, &info, &session_));
        canRender_ = graphics_.CreateSwapchains(session_, configViews_);
        if (!canRender_) std::printf("No swapchains: frames are submitted without layers\n");
        return true;
    }

    bool CreateContainer() {
        XrSpatialContainerCreateInfoEXT info{XR_TYPE_SPATIAL_CONTAINER_CREATE_INFO_EXT};
        info.graphicsPresentation = XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT;
        info.suggestedBounds = {0.3f, 0.2f, 0.2f};
        CHECK_XR(ext_.CreateContainer(session_, &info, &container_));

        XrSpatialContainerSpaceCreateInfoEXT spaceInfo{XR_TYPE_SPATIAL_CONTAINER_SPACE_CREATE_INFO_EXT};
        spaceInfo.spatialContainer = container_;
        CHECK_XR(ext_.CreateContainerSpace(session_, &spaceInfo, &containerSpace_));

        // A new container is hidden and bounded. Start immersive if bounded is unsupported.
        if (!supportsBounded_) {
            XrSpatialContainerBoundsModeRequestInfoEXT mode{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_INFO_EXT};
            mode.boundsMode = XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT;
            CHECK_XR(ext_.RequestBoundsMode(container_, &mode));
        }
        UpdateBounds();

        XrSpatialContainerVisibleRequestInfoEXT show{XR_TYPE_SPATIAL_CONTAINER_VISIBLE_REQUEST_INFO_EXT};
        show.visible = XR_TRUE;
        CHECK_XR(ext_.RequestVisible(container_, &show));
        std::printf("Container created and show requested\n");
        return true;
    }

    void UpdateBounds() {
        XrSpatialContainerBoundsEXT bounds{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_EXT};
        if (XR_SUCCEEDED(ext_.GetBounds(container_, nullptr, &bounds))) {
            bounds_ = bounds.bounds;
            boundsMode_ = bounds.infiniteBounds ? XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT
                                                : XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT;
        }
    }

    void BeginRendering() {
        XrSpatialContainerBeginInfoEXT info{XR_TYPE_SPATIAL_CONTAINER_BEGIN_INFO_EXT};
        info.spatialContainer = container_;
        info.primaryViewConfigurationType = viewConfig_;
        rendering_ = XR_SUCCEEDED(ext_.BeginRendering(session_, &info));
    }

    void EndRendering() {
        XrSpatialContainerEndInfoEXT info{XR_TYPE_SPATIAL_CONTAINER_END_INFO_EXT};
        info.spatialContainer = container_;
        ext_.EndRendering(session_, &info);
        rendering_ = false;
    }

    void RequestQuit() {
        quitting_ = true;
        if (!visible_) {
            done_ = true;
            return;
        }
        // Hide first so the runtime can transition smoothly, then tear down on the event.
        XrSpatialContainerVisibleRequestInfoEXT hide{XR_TYPE_SPATIAL_CONTAINER_VISIBLE_REQUEST_INFO_EXT};
        hide.visible = XR_FALSE;
        if (XR_FAILED(ext_.RequestVisible(container_, &hide))) done_ = true;
        std::printf("Hide requested\n");
    }

    void ToggleBoundsMode() {
        const bool toImmersive = boundsMode_ == XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT;
        if (toImmersive ? !supportsImmersive_ : !supportsBounded_) return;
        XrSpatialContainerBoundsModeRequestInfoEXT mode{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_INFO_EXT};
        mode.boundsMode = toImmersive ? XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT : XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT;
        // Asynchronous: the result arrives as BoundsChanged or BoundsModeRequestDenied.
        if (XR_SUCCEEDED(ext_.RequestBoundsMode(container_, &mode))) std::printf("Requested %s\n", ModeName(mode.boundsMode));
    }

    void PollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
            HandleEvent(event);
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void HandleEvent(const XrEventDataBuffer& event) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& e = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
                std::printf("Session state %d\n", e.state);
                // With spatial containers the session is running while IDLE.
                if (e.state == XR_SESSION_STATE_IDLE) sessionRunning_ = true;
                if (e.state == XR_SESSION_STATE_LOSS_PENDING) done_ = true;
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                done_ = true;
                break;
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_CHANGED_EXT: {
                const auto& e = reinterpret_cast<const XrEventDataSpatialContainerVisibleChangedEXT&>(event);
                visible_ = e.visible;
                std::printf("Visible: %d\n", visible_);
                if (visible_ && !rendering_) BeginRendering();
                if (!visible_ && rendering_) EndRendering();
                if (!visible_ && quitting_) done_ = true;
                break;
            }
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_INTERACTABLE_CHANGED_EXT: {
                const auto& e = reinterpret_cast<const XrEventDataSpatialContainerInteractableChangedEXT&>(event);
                std::printf("Interactable: %d\n", e.interactable);
                break;
            }
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_CHANGED_EXT: {
                const auto& e = reinterpret_cast<const XrEventDataSpatialContainerBoundsChangedEXT&>(event);
                boundsMode_ = e.boundsMode;
                bounds_ = e.bounds;
                std::printf("Bounds changed: %s %.2f x %.2f x %.2f\n", ModeName(boundsMode_), bounds_.width, bounds_.height,
                            bounds_.depth);
                break;
            }
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_REQUEST_DENIED_EXT:
                std::printf("Show request denied\n");
                break;
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_DENIED_EXT:
                std::printf("Bounds mode request denied\n");
                break;
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_CLOSED_EXT:
                std::printf("Container closed by the runtime\n");
                if (rendering_) EndRendering();
                ext_.DestroyContainer(container_);
                container_ = XR_NULL_HANDLE;
                done_ = true;
                break;
            default:
                break;
        }
    }

    Scene BuildScene(XrTime time) const {
        const float angle = static_cast<float>(static_cast<double>(time % 20000000000LL) / 1e9 * 2.0 * 3.14159265 / 20.0);
        const XrQuaternionf spin = MultiplyQuat(AxisAngle(0, 1, 0, angle), AxisAngle(1, 0, 0, 0.45f));
        Scene scene;
        if (boundsMode_ == XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT) {
            // The cube's diagonal fits the smallest side, so it stays inside at any rotation.
            const float smallest = std::min({bounds_.width, bounds_.height, bounds_.depth});
            scene.cubeModel = FromPose({spin, {0, 0, 0}}, smallest * 0.9f / std::sqrt(3.0f));
            scene.boxModel = Scale(bounds_.width, bounds_.height, bounds_.depth);
            scene.drawBox = true;
        } else {
            scene.cubeModel = FromPose({spin, {0, 0, 0}}, 0.3f);
        }
        return scene;
    }

    bool RenderFrame() {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        CHECK_XR(xrWaitFrame(session_, &waitInfo, &frameState));
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        CHECK_XR(xrBeginFrame(session_, &beginInfo));
        ++frameCount_;

        XrSpatialContainerLayerEXT containerLayer{XR_TYPE_SPATIAL_CONTAINER_LAYER_EXT};
        XrSpatialContainerLayerVolumeClippingEXT clipping{XR_TYPE_SPATIAL_CONTAINER_LAYER_VOLUME_CLIPPING_EXT};
        XrSpatialContainerCompositionLayerViewConfigurationEXT layerViewConfig{
            XR_TYPE_SPATIAL_CONTAINER_COMPOSITION_LAYER_VIEW_CONFIGURATION_EXT};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
        XrSpatialContainerLayerFrameEndInfoEXT containerInfo{XR_TYPE_SPATIAL_CONTAINER_LAYER_FRAME_END_INFO_EXT};
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = blendMode_;

        // Every container in the active render set must be listed, even with no layers.
        if (rendering_) {
            containerLayer.spatialContainer = container_;
            containerInfo.containerLayerCount = 1;
            containerInfo.containerLayers = &containerLayer;
            endInfo.next = &containerInfo;

            XrSpatialContainerViewLocateInfoEXT locate{XR_TYPE_SPATIAL_CONTAINER_VIEW_LOCATE_INFO_EXT};
            locate.viewConfigurationType = viewConfig_;
            locate.space = containerSpace_;
            locate.spatialContainer = container_;
            XrSpatialContainerViewsLocateInfoEXT locateInfo{XR_TYPE_SPATIAL_CONTAINER_VIEWS_LOCATE_INFO_EXT};
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.viewLocateInfoCount = 1;
            locateInfo.viewLocateInfos = &locate;
            XrSpatialContainerViewStateEXT viewState{XR_TYPE_SPATIAL_CONTAINER_VIEW_STATE_EXT};
            CHECK_XR(ext_.LocateViews(session_, &locateInfo, 1, &viewState, static_cast<uint32_t>(views_.size()), views_.data()));

            const bool posesValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                                    (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
            bool rendered = false;
            if (viewState.shouldSubmitLayers && posesValid && canRender_) {
                const Scene scene = BuildScene(frameState.predictedDisplayTime);
                rendered = true;
                for (uint32_t i = 0; i < views_.size() && rendered; ++i) {
                    rendered = graphics_.RenderView(i, views_[i], viewState.recommendedImageExtent, scene, &projectionViews_[i]);
                }
            }
            if (rendered) {
                layerViewConfig.viewConfigurationType = viewState.viewConfigurationType;
                projection.next = &layerViewConfig;
                projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                projection.space = containerSpace_;
                projection.viewCount = static_cast<uint32_t>(projectionViews_.size());
                projection.views = projectionViews_.data();
                containerLayer.layerCount = 1;
                containerLayer.layers = layers;
                // Everything drawn is inside the container volume.
                clipping.volumeClipping = boundsMode_ == XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT
                                              ? XR_SPATIAL_CONTAINER_VOLUME_CLIPPING_STRICT_EXT
                                              : XR_SPATIAL_CONTAINER_VOLUME_CLIPPING_NONE_EXT;
                containerLayer.next = &clipping;
                ++submittedFrameCount_;
            } else {
                // Nothing new to show: keep the previous submission, as the spec recommends.
                containerLayer.retainPreviousSubmission = canRender_ ? XR_TRUE : XR_FALSE;
            }
        }
        CHECK_XR(xrEndFrame(session_, &endInfo));
        return true;
    }

    Graphics& graphics_;
    Options options_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    Extensions ext_;
    XrViewConfigurationType viewConfig_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews_;
    std::vector<XrView> views_;
    std::vector<XrCompositionLayerProjectionView> projectionViews_;
    XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool supportsBounded_ = false;
    bool supportsImmersive_ = false;
    bool canRender_ = false;

    XrSpatialContainerEXT container_ = XR_NULL_HANDLE;
    XrSpace containerSpace_ = XR_NULL_HANDLE;
    XrSpatialContainerBoundsModeEXT boundsMode_ = XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT;
    XrExtent3Df bounds_{};

    bool sessionRunning_ = false;
    bool visible_ = false;
    bool rendering_ = false;
    bool quitting_ = false;
    bool done_ = false;
    uint32_t frameCount_ = 0;
    uint32_t submittedFrameCount_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv);
    std::unique_ptr<Graphics> graphics;
#if defined(_WIN32)
    graphics = options.headless ? CreateHeadlessGraphics() : CreateD3D11Graphics();
#else
    if (!options.headless) std::printf("Only --headless is available on this platform; using it\n");
    graphics = CreateHeadlessGraphics();
#endif
    App app(*graphics, options);
    const bool ok = app.Initialize();
    if (ok) app.Run();
    app.Shutdown();
    return ok ? 0 : 1;
}
