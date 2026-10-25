// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// End-to-end test of the emulation layer: app -> OpenXR loader -> layer -> mock runtime.
// Usage: layer_test <path to mock runtime library> [basic|deny]

#include <openxr/openxr.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_XR(expr, expected) CHECK((expr) == (expected))
#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-4)

struct Mock {
    void (*queueState)(XrSessionState);
    bool (*isRunning)();
    uint32_t (*endFrameCount)();
    uint32_t (*lastFrameLayerCount)();
    const char* (*lastError)();
};

Mock LoadMock(const char* path) {
    void* lib = dlopen(path, RTLD_NOW);
    if (lib == nullptr) {
        std::fprintf(stderr, "cannot open %s: %s\n", path, dlerror());
        std::exit(2);
    }
    Mock mock;
    mock.queueState = reinterpret_cast<void (*)(XrSessionState)>(dlsym(lib, "mock_queue_session_state"));
    mock.isRunning = reinterpret_cast<bool (*)()>(dlsym(lib, "mock_is_running"));
    mock.endFrameCount = reinterpret_cast<uint32_t (*)()>(dlsym(lib, "mock_end_frame_count"));
    mock.lastFrameLayerCount = reinterpret_cast<uint32_t (*)()>(dlsym(lib, "mock_last_frame_layer_count"));
    mock.lastError = reinterpret_cast<const char* (*)()>(dlsym(lib, "mock_last_error"));
    return mock;
}

struct Api {
    PFN_xrCreateSpatialContainerEXT CreateContainer;
    PFN_xrDestroySpatialContainerEXT DestroyContainer;
    PFN_xrCreateSpatialContainerSpaceEXT CreateContainerSpace;
    PFN_xrRequestSpatialContainerVisibleEXT RequestVisible;
    PFN_xrRequestSpatialContainerBoundsModeEXT RequestBoundsMode;
    PFN_xrGetSpatialContainerBoundsEXT GetBounds;
    PFN_xrGetSpatialContainerStateEXT GetState;
    PFN_xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT EnumeratePresentations;
    PFN_xrBeginSpatialContainerRenderingEXT BeginRendering;
    PFN_xrEndSpatialContainerRenderingEXT EndRendering;
    PFN_xrLocateSpatialContainerViewsEXT LocateViews;
};

template <typename T>
void Load(XrInstance instance, const char* name, T* out) {
    PFN_xrVoidFunction fn = nullptr;
    CHECK_XR(xrGetInstanceProcAddr(instance, name, &fn), XR_SUCCESS);
    *out = reinterpret_cast<T>(fn);
}

// Drains all pending events and returns them.
std::vector<XrEventDataBuffer> Poll(XrInstance instance) {
    std::vector<XrEventDataBuffer> events;
    for (;;) {
        XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
        if (xrPollEvent(instance, &buffer) != XR_SUCCESS) return events;
        events.push_back(buffer);
    }
}

const char* TypeName(XrStructureType type) {
    switch (type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            return "SessionStateChanged";
        case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_CHANGED_EXT:
            return "VisibleChanged";
        case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_INTERACTABLE_CHANGED_EXT:
            return "InteractableChanged";
        case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_CHANGED_EXT:
            return "BoundsChanged";
        case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_CLOSED_EXT:
            return "Closed";
        case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_REQUEST_DENIED_EXT:
            return "VisibleRequestDenied";
        case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_DENIED_EXT:
            return "BoundsModeRequestDenied";
        default:
            return "other";
    }
}

// Compact description such as "VisibleChanged(1) InteractableChanged(1)".
std::string Describe(const std::vector<XrEventDataBuffer>& events) {
    std::string out;
    for (const auto& e : events) {
        if (!out.empty()) out += " ";
        out += TypeName(e.type);
        if (e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            out += "(" + std::to_string(reinterpret_cast<const XrEventDataSessionStateChanged&>(e).state) + ")";
        } else if (e.type == XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_CHANGED_EXT) {
            out +=
                "(" + std::to_string(reinterpret_cast<const XrEventDataSpatialContainerVisibleChangedEXT&>(e).visible) + ")";
        } else if (e.type == XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_INTERACTABLE_CHANGED_EXT) {
            out +=
                "(" +
                std::to_string(reinterpret_cast<const XrEventDataSpatialContainerInteractableChangedEXT&>(e).interactable) +
                ")";
        } else if (e.type == XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_CHANGED_EXT) {
            out += "(" + std::to_string(reinterpret_cast<const XrEventDataSpatialContainerBoundsChangedEXT&>(e).boundsMode) +
                   ")";
        }
    }
    return out;
}

#define CHECK_EVENTS(events, expected)                                                                             \
    do {                                                                                                           \
        const std::string actual = Describe(events);                                                               \
        if (actual != (expected)) {                                                                                \
            std::fprintf(stderr, "FAILED %s:%d: events [%s], expected [%s]\n", __FILE__, __LINE__, actual.c_str(), \
                         (expected));                                                                              \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

struct Frame {
    XrCompositionLayerProjectionView views[2]{};
    XrSpatialContainerCompositionLayerViewConfigurationEXT viewConfig{
        XR_TYPE_SPATIAL_CONTAINER_COMPOSITION_LAYER_VIEW_CONFIGURATION_EXT};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1]{};
    XrSpatialContainerLayerEXT containerLayer{XR_TYPE_SPATIAL_CONTAINER_LAYER_EXT};
    XrSpatialContainerLayerFrameEndInfoEXT containerInfo{XR_TYPE_SPATIAL_CONTAINER_LAYER_FRAME_END_INFO_EXT};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};

    Frame(XrSpatialContainerEXT container, XrSpace space, XrTime time) {
        viewConfig.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        for (auto& view : views) view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projection.next = &viewConfig;
        projection.space = space;
        projection.viewCount = 2;
        projection.views = views;
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        containerLayer.spatialContainer = container;
        containerLayer.layerCount = 1;
        containerLayer.layers = layers;
        containerInfo.containerLayerCount = 1;
        containerInfo.containerLayers = &containerLayer;
        endInfo.next = &containerInfo;
        endInfo.displayTime = time;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }
};

XrTime WaitAndBegin(XrSession session) {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    CHECK_XR(xrWaitFrame(session, &waitInfo, &state), XR_SUCCESS);
    CHECK(state.shouldRender == XR_FALSE);  // Always false with spatial containers.
    CHECK(state.predictedDisplayTime > 0);
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    CHECK_XR(xrBeginFrame(session, &beginInfo), XR_SUCCESS);
    return state.predictedDisplayTime;
}

int Run(const Mock& mock, const std::string& mode) {
    // The layer adds both extensions to the list the loader reports.
    uint32_t count = 0;
    CHECK_XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), XR_SUCCESS);
    std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
    CHECK_XR(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data()), XR_SUCCESS);
    int found = 0;
    for (const auto& p : props) {
        found += std::strcmp(p.extensionName, XR_EXT_SPATIAL_CONTAINER_EXTENSION_NAME) == 0;
        found += std::strcmp(p.extensionName, XR_EXT_SPATIAL_CONTAINER_SELF_RENDERING_EXTENSION_NAME) == 0;
    }
    CHECK(found == 2);

    const char* extensions[] = {XR_EXT_SPATIAL_CONTAINER_EXTENSION_NAME,
                                XR_EXT_SPATIAL_CONTAINER_SELF_RENDERING_EXTENSION_NAME};
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(instanceInfo.applicationInfo.applicationName, "layer_test");
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.enabledExtensionNames = extensions;
    XrInstance instance;
    CHECK_XR(xrCreateInstance(&instanceInfo, &instance), XR_SUCCESS);
    if (g_failures) return 1;

    Api api{};
    Load(instance, "xrCreateSpatialContainerEXT", &api.CreateContainer);
    Load(instance, "xrDestroySpatialContainerEXT", &api.DestroyContainer);
    Load(instance, "xrCreateSpatialContainerSpaceEXT", &api.CreateContainerSpace);
    Load(instance, "xrRequestSpatialContainerVisibleEXT", &api.RequestVisible);
    Load(instance, "xrRequestSpatialContainerBoundsModeEXT", &api.RequestBoundsMode);
    Load(instance, "xrGetSpatialContainerBoundsEXT", &api.GetBounds);
    Load(instance, "xrGetSpatialContainerStateEXT", &api.GetState);
    Load(instance, "xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT", &api.EnumeratePresentations);
    Load(instance, "xrBeginSpatialContainerRenderingEXT", &api.BeginRendering);
    Load(instance, "xrEndSpatialContainerRenderingEXT", &api.EndRendering);
    Load(instance, "xrLocateSpatialContainerViewsEXT", &api.LocateViews);

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId;
    CHECK_XR(xrGetSystem(instance, &systemInfo, &systemId), XR_SUCCESS);

    XrSystemSpatialContainerPropertiesEXT containerProps{XR_TYPE_SYSTEM_SPATIAL_CONTAINER_PROPERTIES_EXT};
    XrSystemProperties systemProps{XR_TYPE_SYSTEM_PROPERTIES, &containerProps};
    CHECK_XR(xrGetSystemProperties(instance, systemId, &systemProps), XR_SUCCESS);
    CHECK(containerProps.maxSpatialContainerCount > 0);
    CHECK(containerProps.supportsBounded && containerProps.supportsImmersive);

    XrSpatialContainerGraphicsPresentationEXT presentation{};
    CHECK_XR(api.EnumeratePresentations(instance, systemId, 1, &count, &presentation), XR_SUCCESS);
    CHECK(count == 1 && presentation == XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT);

    XrSessionCreateInfoSpatialContainersEXT enableContainers{XR_TYPE_SESSION_CREATE_INFO_SPATIAL_CONTAINERS_EXT};
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &enableContainers};
    sessionInfo.systemId = systemId;
    XrSession session;
    CHECK_XR(xrCreateSession(instance, &sessionInfo, &session), XR_SUCCESS);

    // Classic session control is replaced by containers.
    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    CHECK_XR(xrBeginSession(session, &beginInfo), XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT);
    CHECK_XR(xrEndSession(session), XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT);
    CHECK_XR(xrRequestExitSession(session), XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT);

    XrSpatialContainerCreateInfoEXT createInfo{XR_TYPE_SPATIAL_CONTAINER_CREATE_INFO_EXT};
    createInfo.graphicsPresentation = XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT;
    createInfo.suggestedBounds = {-1.0f, 0.2f, 0.2f};
    XrSpatialContainerEXT container;
    CHECK_XR(api.CreateContainer(session, &createInfo, &container), XR_ERROR_VALIDATION_FAILURE);
    createInfo.suggestedBounds = {0.3f, 0.2f, 0.25f};
    CHECK_XR(api.CreateContainer(session, &createInfo, &container), XR_SUCCESS);

    XrSpatialContainerStateEXT state{XR_TYPE_SPATIAL_CONTAINER_STATE_EXT};
    CHECK_XR(api.GetState(container, nullptr, &state), XR_SUCCESS);
    CHECK(!state.visible && !state.interactable && state.boundsMode == XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT);
    XrSpatialContainerBoundsEXT bounds{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_EXT};
    CHECK_XR(api.GetBounds(container, nullptr, &bounds), XR_SUCCESS);
    CHECK(!bounds.infiniteBounds && bounds.bounds.width == 0.3f && bounds.bounds.depth == 0.25f);

    XrSpatialContainerSpaceCreateInfoEXT spaceInfo{XR_TYPE_SPATIAL_CONTAINER_SPACE_CREATE_INFO_EXT};
    spaceInfo.spatialContainer = container;
    XrSpace containerSpace;
    CHECK_XR(api.CreateContainerSpace(session, &spaceInfo, &containerSpace), XR_SUCCESS);
    XrReferenceSpaceCreateInfo localInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace local;
    CHECK_XR(xrCreateReferenceSpace(session, &localInfo, &local), XR_SUCCESS);

    // The app only ever sees IDLE: READY and the rest are handled by the layer.
    CHECK_EVENTS(Poll(instance), "SessionStateChanged(1)");
    CHECK(mock.isRunning());

    // Not visible yet: the container space is unlocatable.
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    CHECK_XR(xrLocateSpace(containerSpace, local, 1, &location), XR_SUCCESS);
    CHECK(location.locationFlags == 0);

    XrSpatialContainerBeginInfoEXT renderBegin{XR_TYPE_SPATIAL_CONTAINER_BEGIN_INFO_EXT};
    renderBegin.spatialContainer = container;
    renderBegin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    CHECK_XR(api.BeginRendering(session, &renderBegin), XR_SUCCESS);
    CHECK_XR(api.BeginRendering(session, &renderBegin), XR_ERROR_SPATIAL_CONTAINER_IS_RENDERING_EXT);

    XrSpatialContainerVisibleRequestInfoEXT show{XR_TYPE_SPATIAL_CONTAINER_VISIBLE_REQUEST_INFO_EXT};
    show.visible = XR_TRUE;
    CHECK_XR(api.RequestVisible(container, &show), XR_SUCCESS);

    if (mode == "deny") {
        // XR_SCEMU_DENY_VISIBLE_REQUESTS and XR_SCEMU_DENY_BOUNDS_MODE_REQUESTS are set.
        CHECK_EVENTS(Poll(instance), "VisibleRequestDenied");
        XrSpatialContainerBoundsModeRequestInfoEXT immersive{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_INFO_EXT};
        immersive.boundsMode = XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT;
        // Requests for a hidden container are always honored.
        CHECK_XR(api.RequestBoundsMode(container, &immersive), XR_SUCCESS);
        CHECK_EVENTS(Poll(instance), "BoundsChanged(2)");
        CHECK_XR(xrDestroySession(session), XR_SUCCESS);
        CHECK_XR(xrDestroyInstance(instance), XR_SUCCESS);
        return g_failures;
    }

    CHECK_EVENTS(Poll(instance), "VisibleChanged(1) InteractableChanged(1)");
    CHECK_XR(api.GetState(container, nullptr, &state), XR_SUCCESS);
    CHECK(state.visible && state.interactable);
    location = {XR_TYPE_SPACE_LOCATION};
    CHECK_XR(xrLocateSpace(containerSpace, local, 1, &location), XR_SUCCESS);
    CHECK(location.locationFlags != 0);

    // A normal frame: one projection layer reaches the runtime, without container structs.
    XrTime time = WaitAndBegin(session);
    XrSpatialContainerViewLocateInfoEXT viewLocate{XR_TYPE_SPATIAL_CONTAINER_VIEW_LOCATE_INFO_EXT};
    viewLocate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocate.space = containerSpace;
    viewLocate.spatialContainer = container;
    XrSpatialContainerViewsLocateInfoEXT viewsLocate{XR_TYPE_SPATIAL_CONTAINER_VIEWS_LOCATE_INFO_EXT};
    viewsLocate.displayTime = time;
    viewsLocate.viewLocateInfoCount = 1;
    viewsLocate.viewLocateInfos = &viewLocate;
    XrSpatialContainerViewStateEXT viewState{XR_TYPE_SPATIAL_CONTAINER_VIEW_STATE_EXT};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    CHECK_XR(api.LocateViews(session, &viewsLocate, 1, &viewState, 1, views), XR_ERROR_VALIDATION_FAILURE);
    CHECK_XR(api.LocateViews(session, &viewsLocate, 1, &viewState, 2, views), XR_SUCCESS);
    CHECK(viewState.shouldSubmitLayers);
    CHECK((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0);
    // The mock's eyes are at (+-0.032, 0, 0) in LOCAL and the container is 1 m ahead, so
    // in container space they are at (+-0.032, 0, 1).
    CHECK_NEAR(views[0].pose.position.x, -0.032f);
    CHECK_NEAR(views[0].pose.position.z, 1.0f);
    // FOV fitted to the 0.3 x 0.2 x 0.25 box: its front face is 0.875 m from the eyes.
    CHECK_NEAR(views[0].fov.angleLeft, std::atan(-0.118f / 0.875f));
    CHECK_NEAR(views[0].fov.angleRight, std::atan(0.182f / 0.875f));
    CHECK_NEAR(views[0].fov.angleUp, std::atan(0.1f / 0.875f));
    CHECK_NEAR(views[0].fov.angleDown, std::atan(-0.1f / 0.875f));
    CHECK_NEAR(views[1].fov.angleLeft, -views[0].fov.angleRight);
    // The image shrinks with the FOV (mock FOV: +-0.8 horizontal, +-0.7 vertical).
    CHECK(viewState.recommendedImageExtent.width ==
          static_cast<int32_t>(std::ceil(1280 * (0.3f / 0.875f) / (2 * std::tan(0.8f)))));
    CHECK(viewState.recommendedImageExtent.height ==
          static_cast<int32_t>(std::ceil(720 * (0.2f / 0.875f) / (2 * std::tan(0.7f)))));
    CHECK_XR(xrLocateViews(session, nullptr, nullptr, 0, &count, nullptr), XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT);

    // Validation of the submission, each of which must not end the frame.
    {
        Frame frame(container, containerSpace, time);
        frame.endInfo.layerCount = 1;
        frame.endInfo.layers = frame.layers;
        CHECK_XR(xrEndFrame(session, &frame.endInfo), XR_ERROR_VALIDATION_FAILURE);
    }
    {
        Frame frame(container, containerSpace, time);
        frame.endInfo.next = nullptr;
        CHECK_XR(xrEndFrame(session, &frame.endInfo), XR_ERROR_SPATIAL_CONTAINER_MISSING_EXT);
    }
    {
        Frame frame(container, containerSpace, time);
        XrSpatialContainerLayerEXT twice[2] = {frame.containerLayer, frame.containerLayer};
        frame.containerInfo.containerLayerCount = 2;
        frame.containerInfo.containerLayers = twice;
        CHECK_XR(xrEndFrame(session, &frame.endInfo), XR_ERROR_SPATIAL_CONTAINER_DUPLICATE_EXT);
    }
    {
        Frame frame(container, containerSpace, time);
        frame.containerLayer.retainPreviousSubmission = XR_TRUE;
        CHECK_XR(xrEndFrame(session, &frame.endInfo), XR_ERROR_VALIDATION_FAILURE);
    }
    const uint32_t framesBefore = mock.endFrameCount();
    {
        Frame frame(container, containerSpace, time);
        CHECK_XR(xrEndFrame(session, &frame.endInfo), XR_SUCCESS);
        CHECK(mock.endFrameCount() == framesBefore + 1);
        CHECK(mock.lastFrameLayerCount() == 1);
        // The app's chain is restored after the call.
        CHECK(frame.projection.next == &frame.viewConfig);
    }

    // Retain resubmits the last layers; clear submits none.
    {
        time = WaitAndBegin(session);
        Frame frame(container, containerSpace, time);
        frame.containerLayer.retainPreviousSubmission = XR_TRUE;
        frame.containerLayer.layerCount = 0;
        frame.containerLayer.layers = nullptr;
        CHECK_XR(xrEndFrame(session, &frame.endInfo), XR_SUCCESS);
        CHECK(mock.lastFrameLayerCount() == 1);
    }
    {
        time = WaitAndBegin(session);
        Frame frame(container, containerSpace, time);
        frame.containerLayer.layerCount = 0;
        frame.containerLayer.layers = nullptr;
        CHECK_XR(xrEndFrame(session, &frame.endInfo), XR_SUCCESS);
        CHECK(mock.lastFrameLayerCount() == 0);
    }

    // Bounds mode changes.
    XrSpatialContainerBoundsModeRequestInfoEXT modeRequest{XR_TYPE_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_INFO_EXT};
    // An invalid value on purpose.
    modeRequest.boundsMode =
        static_cast<XrSpatialContainerBoundsModeEXT>(7);  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    CHECK_XR(api.RequestBoundsMode(container, &modeRequest), XR_ERROR_VALIDATION_FAILURE);
    modeRequest.boundsMode = XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT;
    CHECK_XR(api.RequestBoundsMode(container, &modeRequest), XR_SUCCESS);
    CHECK_XR(api.RequestBoundsMode(container, &modeRequest), XR_SUCCESS);  // No change, no event.
    CHECK_EVENTS(Poll(instance), "BoundsChanged(2)");
    CHECK_XR(api.GetBounds(container, nullptr, &bounds), XR_SUCCESS);
    CHECK(bounds.infiniteBounds && bounds.bounds.width == 0.0f);
    // Immersive containers get the runtime's full views.
    CHECK_XR(api.LocateViews(session, &viewsLocate, 1, &viewState, 2, views), XR_SUCCESS);
    CHECK_NEAR(views[0].fov.angleRight, 0.8f);
    CHECK(viewState.recommendedImageExtent.width == 1280);

    // Runtime loses focus, then stops: interactable, then visible, are lost.
    mock.queueState(XR_SESSION_STATE_VISIBLE);
    CHECK_EVENTS(Poll(instance), "InteractableChanged(0)");
    mock.queueState(XR_SESSION_STATE_STOPPING);
    CHECK_EVENTS(Poll(instance), "VisibleChanged(0)");
    CHECK(!mock.isRunning());

    // Frames keep working for the app while the runtime session is stopped.
    {
        time = WaitAndBegin(session);
        CHECK_XR(api.LocateViews(session, &viewsLocate, 1, &viewState, 2, views), XR_SUCCESS);
        CHECK(!viewState.shouldSubmitLayers && viewState.viewStateFlags == 0);
        Frame frame(container, containerSpace, time);
        CHECK_XR(xrEndFrame(session, &frame.endInfo), XR_SUCCESS);
    }
    CHECK(std::strlen(mock.lastError()) == 0);

    // The runtime becomes ready again: the container comes back.
    mock.queueState(XR_SESSION_STATE_READY);
    CHECK_EVENTS(Poll(instance), "VisibleChanged(1) InteractableChanged(1)");
    CHECK(mock.isRunning());

    // Losing the session closes the container first.
    mock.queueState(XR_SESSION_STATE_LOSS_PENDING);
    CHECK_EVENTS(Poll(instance), "InteractableChanged(0) VisibleChanged(0) Closed SessionStateChanged(7)");
    CHECK_XR(api.RequestVisible(container, &show), XR_ERROR_SPATIAL_CONTAINER_CLOSED_EXT);
    CHECK_XR(api.GetState(container, nullptr, &state), XR_ERROR_SPATIAL_CONTAINER_CLOSED_EXT);
    CHECK_XR(api.DestroyContainer(container), XR_SUCCESS);
    CHECK_XR(api.GetState(container, nullptr, &state), XR_ERROR_HANDLE_INVALID);
    // The space stays valid but unlocatable.
    location = {XR_TYPE_SPACE_LOCATION};
    CHECK_XR(xrLocateSpace(containerSpace, local, 1, &location), XR_SUCCESS);
    CHECK(location.locationFlags == 0);

    CHECK_XR(xrDestroySession(session), XR_SUCCESS);
    CHECK_XR(xrDestroyInstance(instance), XR_SUCCESS);
    CHECK(std::strlen(mock.lastError()) == 0);
    return g_failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <mock runtime library> [basic|deny]\n", argv[0]);
        return 2;
    }
    const Mock mock = LoadMock(argv[1]);
    const int failures = Run(mock, argc > 2 ? argv[2] : "basic");
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
