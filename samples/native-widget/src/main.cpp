// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// A resident widget in a bounded spatial container: a clock and a pomodoro timer on
// a flat panel, meant to sit next to other apps.
//
// Compared with samples/cpp-minimal, it shows the other side of self rendering:
// - The panel is a quad layer: no views to render, only an image to place.
// - The image changes about once a second, so it is uploaded only then. Every other
//   frame retains the previous submission (retainPreviousSubmission).
// - It stays bounded and uses input only while the runtime reports it interactable.
//
// Usage: native_widget [--headless] [--graphics vulkan|d3d11] [--seconds N]
//                      [--focus-minutes M] [--break-minutes M] [--start]

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <openxr/openxr.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "graphics.h"
#include "panel.h"
#include "pomodoro.h"

namespace {

#define CHECK_XR(call)                                                                           \
    do {                                                                                         \
        const XrResult result_ = (call);                                                         \
        if (XR_FAILED(result_)) {                                                                \
            std::fprintf(stderr, "%s failed: %d (%s:%d)\n", #call, result_, __FILE__, __LINE__); \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

struct Options {
    bool headless = false;
    std::string graphics;
    double seconds = 0.0;  // 0: run until the container is closed.
    double focusMinutes = 25.0;
    double breakMinutes = 5.0;
    bool start = false;
};

[[noreturn]] void Usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s [--headless] [--graphics vulkan|d3d11] [--seconds N] [--focus-minutes M]"
                 " [--break-minutes M] [--start]\n",
                 program);
    std::exit(2);
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--start") {
            options.start = true;
        } else if (arg == "--graphics" && hasValue) {
            options.graphics = argv[++i];
        } else if (arg == "--seconds" && hasValue) {
            options.seconds = std::atof(argv[++i]);
        } else if (arg == "--focus-minutes" && hasValue) {
            options.focusMinutes = std::atof(argv[++i]);
        } else if (arg == "--break-minutes" && hasValue) {
            options.breakMinutes = std::atof(argv[++i]);
        } else {
            Usage(argv[0]);
        }
    }
    return options;
}

// Copies a C string into a fixed-size OpenXR name field, truncating if needed.
template <size_t N>
void CopyName(char (&destination)[N], const char* source) {
    std::snprintf(destination, N, "%s", source);
}

Pomodoro::Duration Minutes(double minutes) {
    return std::chrono::duration_cast<Pomodoro::Duration>(std::chrono::duration<double, std::ratio<60>>(minutes));
}

std::tm LocalTime() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return local;
}

class Widget {
public:
    Widget(Graphics& graphics, const Options& options)
        : graphics_(graphics),
          options_(options),
          pomodoro_(Minutes(options.focusMinutes), Minutes(options.breakMinutes)),
          image_(panel::kWidth, panel::kHeight) {
        if (options.start) pomodoro_.toggle();
    }

    bool Initialize() {
        if (!CreateInstance()) return false;
        if (!graphics_.Initialize(instance_, systemId_)) {
            std::fprintf(stderr, "Graphics initialization failed\n");
            return false;
        }
        return CreateSession() && CreateActions() && CreateContainer();
    }

    void Run() {
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        auto last = start;
        while (!done_) {
            PollEvents();
            const auto now = Clock::now();
            // Advance by whole milliseconds and keep the remainder for the next turn:
            // a fast loop would otherwise truncate every step to zero.
            const auto elapsed = std::chrono::duration_cast<Pomodoro::Duration>(now - last);
            last += elapsed;
            if (pomodoro_.advance(elapsed)) {
                std::printf("Pomodoro: %s\n", pomodoro_.phase() == Pomodoro::Phase::Focus ? "focus" : "break");
            }
            if (options_.seconds > 0 && now - start > std::chrono::duration<double>(options_.seconds) && !quitting_) {
                RequestQuit();
            }
            if (sessionRunning_ && !done_) {
                if (!RunFrame()) break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    void Shutdown() {
        if (rendering_) EndRendering();
        if (container_ != XR_NULL_HANDLE) ext_.DestroyContainer(container_);
        graphics_.DestroySwapchain();
        if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
        if (containerSpace_ != XR_NULL_HANDLE) xrDestroySpace(containerSpace_);
        if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
        if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
        std::printf("Shut down after %u frames, %u panel changes, %u uploads, %u retained\n", frames_, changes_, uploads_,
                    retained_);
    }

private:
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
            bool found = false;
            for (const auto& p : available) found = found || std::strcmp(p.extensionName, name) == 0;
            if (!found) {
                std::fprintf(stderr,
                             "Required extension %s is not available. On runtimes without spatial container support, "
                             "enable XR_APILAYER_spatial_container_emulation.\n",
                             name);
                return false;
            }
        }

        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        CopyName(info.applicationInfo.applicationName, "spatial-container-native-widget");
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
        std::printf("System: %s, max containers %u, bounded %d\n", systemProps.systemName,
                    containerProps.maxSpatialContainerCount, supportsBounded_);
        if (containerProps.maxSpatialContainerCount == 0) return false;

        CHECK_XR(xrEnumerateViewConfigurations(instance_, systemId_, 0, &count, nullptr));
        std::vector<XrViewConfigurationType> configs(count);
        CHECK_XR(xrEnumerateViewConfigurations(instance_, systemId_, count, &count, configs.data()));
        if (configs.empty()) return false;
        viewConfig_ = configs[0];
        CHECK_XR(xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfig_, 0, &count, nullptr));
        views_.assign(count, {XR_TYPE_VIEW});

        XrEnvironmentBlendMode modes[8];
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance_, systemId_, viewConfig_, 8, &count, modes)) &&
            count > 0) {
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
        canPresent_ = graphics_.CreateSwapchain(session_, panel::kWidth, panel::kHeight);
        if (!canPresent_) std::printf("No swapchain: frames are submitted without layers\n");
        return true;
    }

    // Select toggles the timer and menu resets it, on any controller. Input is optional:
    // the widget still works if the runtime has none of these bindings.
    bool CreateActions() {
        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        CopyName(setInfo.actionSetName, "widget");
        CopyName(setInfo.localizedActionSetName, "Widget");
        CHECK_XR(xrCreateActionSet(instance_, &setInfo, &actionSet_));
        auto create = [&](const char* name, const char* localized, XrAction* action) {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            CopyName(info.actionName, name);
            CopyName(info.localizedActionName, localized);
            return XR_SUCCEEDED(xrCreateAction(actionSet_, &info, action));
        };
        if (!create("toggle", "Start or pause", &toggleAction_) || !create("reset", "Reset", &resetAction_)) return false;

        auto path = [&](const char* text) {
            XrPath p = XR_NULL_PATH;
            xrStringToPath(instance_, text, &p);
            return p;
        };
        const XrActionSuggestedBinding bindings[] = {
            {toggleAction_, path("/user/hand/left/input/select/click")},
            {toggleAction_, path("/user/hand/right/input/select/click")},
            {resetAction_, path("/user/hand/left/input/menu/click")},
            {resetAction_, path("/user/hand/right/input/menu/click")},
        };
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = path("/interaction_profiles/khr/simple_controller");
        suggested.countSuggestedBindings = 4;
        suggested.suggestedBindings = bindings;
        if (XR_FAILED(xrSuggestInteractionProfileBindings(instance_, &suggested))) {
            std::printf("Controller bindings not accepted; input disabled\n");
        }
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &actionSet_;
        CHECK_XR(xrAttachSessionActionSets(session_, &attach));
        return true;
    }

    bool CreateContainer() {
        XrSpatialContainerCreateInfoEXT info{XR_TYPE_SPATIAL_CONTAINER_CREATE_INFO_EXT};
        info.graphicsPresentation = XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT;
        // A thin box the size of the panel at 1 mm per pixel.
        info.suggestedBounds = {panel::kWidth / 1000.0f, panel::kHeight / 1000.0f, 0.02f};
        CHECK_XR(ext_.CreateContainer(session_, &info, &container_));
        XrSpatialContainerSpaceCreateInfoEXT spaceInfo{XR_TYPE_SPATIAL_CONTAINER_SPACE_CREATE_INFO_EXT};
        spaceInfo.spatialContainer = container_;
        CHECK_XR(ext_.CreateContainerSpace(session_, &spaceInfo, &containerSpace_));

        // A widget belongs next to other apps. Only if the system cannot do that, go immersive.
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
        if (XR_FAILED(ext_.GetBounds(container_, nullptr, &bounds))) return;
        SetBounds(bounds.bounds, bounds.infiniteBounds);
    }

    void SetBounds(const XrExtent3Df& bounds, XrBool32 infinite) {
        if (infinite) {
            // Immersive: no box to fit in; show the panel at 1 mm per pixel.
            panelSize_ = {panel::kWidth / 1000.0f, panel::kHeight / 1000.0f};
        } else {
            const auto fitted = FitPanel(bounds.width, bounds.height);
            panelSize_ = {fitted.first, fitted.second};
        }
        layoutChanged_ = true;
    }

    void BeginRendering() {
        XrSpatialContainerBeginInfoEXT info{XR_TYPE_SPATIAL_CONTAINER_BEGIN_INFO_EXT};
        info.spatialContainer = container_;
        info.primaryViewConfigurationType = viewConfig_;
        rendering_ = XR_SUCCEEDED(ext_.BeginRendering(session_, &info));
        layoutChanged_ = true;  // Nothing has been submitted for this container yet.
        submitted_ = false;
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
        XrSpatialContainerVisibleRequestInfoEXT hide{XR_TYPE_SPATIAL_CONTAINER_VISIBLE_REQUEST_INFO_EXT};
        hide.visible = XR_FALSE;
        if (XR_FAILED(ext_.RequestVisible(container_, &hide))) done_ = true;
        std::printf("Hide requested\n");
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
                if (e.state == XR_SESSION_STATE_IDLE) sessionRunning_ = true;
                if (e.state == XR_SESSION_STATE_LOSS_PENDING) done_ = true;
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                done_ = true;
                break;
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_CHANGED_EXT: {
                visible_ = reinterpret_cast<const XrEventDataSpatialContainerVisibleChangedEXT&>(event).visible;
                std::printf("Visible: %d\n", visible_);
                if (visible_ && !rendering_) BeginRendering();
                if (!visible_ && rendering_) EndRendering();
                if (!visible_ && quitting_) done_ = true;
                break;
            }
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_INTERACTABLE_CHANGED_EXT:
                interactable_ =
                    reinterpret_cast<const XrEventDataSpatialContainerInteractableChangedEXT&>(event).interactable;
                std::printf("Interactable: %d\n", interactable_);
                break;
            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_CHANGED_EXT: {
                const auto& e = reinterpret_cast<const XrEventDataSpatialContainerBoundsChangedEXT&>(event);
                SetBounds(e.bounds, e.infiniteBounds);
                std::printf("Bounds changed: %.3f x %.3f m panel\n", panelSize_.width, panelSize_.height);
                break;
            }
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

    // Returns true when the action was pressed since the last sync.
    bool Pressed(XrAction action) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &info, &state)) && state.isActive &&
               state.changedSinceLastSync && state.currentState;
    }

    void HandleInput() {
        // The runtime decides which containers receive input.
        if (!interactable_) return;
        const XrActiveActionSet active{actionSet_, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_FAILED(xrSyncActions(session_, &sync))) return;
        if (Pressed(toggleAction_)) {
            pomodoro_.toggle();
            std::printf("Pomodoro: %s\n", pomodoro_.running() ? "running" : "paused");
        }
        if (Pressed(resetAction_)) {
            pomodoro_.reset();
            std::printf("Pomodoro: reset\n");
        }
    }

    PanelState CurrentState() const {
        const std::tm local = LocalTime();
        PanelState state;
        state.hour = local.tm_hour;
        state.minute = local.tm_min;
        state.focus = pomodoro_.phase() == Pomodoro::Phase::Focus;
        state.running = pomodoro_.running();
        state.remainingSeconds = pomodoro_.displaySeconds();
        state.progress = pomodoro_.progress();
        return state;
    }

    bool RunFrame() {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        CHECK_XR(xrWaitFrame(session_, &waitInfo, &frameState));
        HandleInput();
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        CHECK_XR(xrBeginFrame(session_, &beginInfo));
        ++frames_;

        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
        XrSpatialContainerLayerVolumeClippingEXT clipping{XR_TYPE_SPATIAL_CONTAINER_LAYER_VOLUME_CLIPPING_EXT};
        XrSpatialContainerLayerEXT containerLayer{XR_TYPE_SPATIAL_CONTAINER_LAYER_EXT};
        XrSpatialContainerLayerFrameEndInfoEXT containerInfo{XR_TYPE_SPATIAL_CONTAINER_LAYER_FRAME_END_INFO_EXT};
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = blendMode_;

        if (rendering_) {
            containerLayer.spatialContainer = container_;
            containerInfo.containerLayerCount = 1;
            containerInfo.containerLayers = &containerLayer;
            endInfo.next = &containerInfo;

            // The views are only needed to learn whether the container is on screen.
            XrSpatialContainerViewLocateInfoEXT locate{XR_TYPE_SPATIAL_CONTAINER_VIEW_LOCATE_INFO_EXT};
            locate.viewConfigurationType = viewConfig_;
            locate.space = containerSpace_;
            locate.spatialContainer = container_;
            XrSpatialContainerViewsLocateInfoEXT locateInfo{XR_TYPE_SPATIAL_CONTAINER_VIEWS_LOCATE_INFO_EXT};
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.viewLocateInfoCount = 1;
            locateInfo.viewLocateInfos = &locate;
            XrSpatialContainerViewStateEXT viewState{XR_TYPE_SPATIAL_CONTAINER_VIEW_STATE_EXT};
            CHECK_XR(
                ext_.LocateViews(session_, &locateInfo, 1, &viewState, static_cast<uint32_t>(views_.size()), views_.data()));

            const PanelState state = CurrentState();
            const bool changed = !shownValid_ || !SameDisplay(state, shown_);
            if (changed) {
                shown_ = state;
                shownValid_ = true;
                ++changes_;
            }
            if ((changed || layoutChanged_ || !submitted_) && viewState.shouldSubmitLayers && canPresent_) {
                if (changed || !submitted_) {
                    RenderPanel(shown_, image_);
                    if (graphics_.Upload(image_)) ++uploads_;
                }
                quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // Rounded corners.
                quad.space = containerSpace_;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = graphics_.Swapchain();
                quad.subImage.imageRect = {{0, 0}, {panel::kWidth, panel::kHeight}};
                quad.pose = {{0, 0, 0, 1}, {0, 0, 0}};  // The container's center, facing the user (+Z).
                quad.size = panelSize_;
                containerLayer.layerCount = 1;
                containerLayer.layers = layers;
                clipping.volumeClipping = XR_SPATIAL_CONTAINER_VOLUME_CLIPPING_STRICT_EXT;  // Flat, inside the box.
                containerLayer.next = &clipping;
                submitted_ = true;
                layoutChanged_ = false;
            } else if (submitted_) {
                // Nothing changed: keep showing the last panel.
                containerLayer.retainPreviousSubmission = XR_TRUE;
                ++retained_;
            }
        }
        CHECK_XR(xrEndFrame(session_, &endInfo));
        return true;
    }

    Graphics& graphics_;
    Options options_;
    Pomodoro pomodoro_;
    Image image_;
    PanelState shown_;
    bool shownValid_ = false;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    Extensions ext_;
    XrViewConfigurationType viewConfig_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrView> views_;
    XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool supportsBounded_ = false;
    bool canPresent_ = false;
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction toggleAction_ = XR_NULL_HANDLE;
    XrAction resetAction_ = XR_NULL_HANDLE;

    XrSpatialContainerEXT container_ = XR_NULL_HANDLE;
    XrSpace containerSpace_ = XR_NULL_HANDLE;
    XrExtent2Df panelSize_{panel::kWidth / 1000.0f, panel::kHeight / 1000.0f};

    bool sessionRunning_ = false;
    bool visible_ = false;
    bool interactable_ = false;
    bool rendering_ = false;
    bool submitted_ = false;
    bool layoutChanged_ = true;
    bool quitting_ = false;
    bool done_ = false;
    uint32_t frames_ = 0, changes_ = 0, uploads_ = 0, retained_ = 0;
};

std::unique_ptr<Graphics> CreateGraphics(const Options& options) {
    if (options.headless) return CreateHeadlessGraphics();
    std::string name = options.graphics;
#if defined(_WIN32)
    if (name.empty()) name = "d3d11";
    if (name == "d3d11") return CreateD3D11Graphics();
#endif
#if defined(WIDGET_HAS_VULKAN)
    if (name.empty()) name = "vulkan";
    if (name == "vulkan") return CreateVulkanGraphics();
#endif
    std::fprintf(stderr, "Graphics backend '%s' is not available in this build; use --headless\n", name.c_str());
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv);
    std::unique_ptr<Graphics> graphics = CreateGraphics(options);
    if (!graphics) return 2;
    bool ok = false;
    {
        Widget widget(*graphics, options);
        ok = widget.Initialize();
        if (ok) widget.Run();
        widget.Shutdown();
    }
    return ok ? 0 : 1;
}
