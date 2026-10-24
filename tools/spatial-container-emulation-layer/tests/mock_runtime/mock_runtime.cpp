// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Minimal headless OpenXR 1.0-style runtime for testing the emulation layer.
// It implements just enough of the classic session and frame loop, rejects any
// spatial container structure that reaches it, and records what xrEndFrame
// received. Tests drive it through the exported mock_* functions.

#define XR_USE_TIMESPEC
#include <time.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#define MOCK_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

std::mutex g_mutex;
std::deque<XrEventDataBuffer> g_events;
XrSession g_session = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
XrTime g_time = 1000000000;
uint64_t g_nextHandle = 1;
std::string g_lastError;

// What the last xrEndFrame received.
uint32_t g_frameLayerCount = 0;
std::vector<XrStructureType> g_frameLayerTypes;
uint32_t g_endFrameCount = 0;

void Fail(const std::string& message) {
    std::fprintf(stderr, "[mock runtime] %s\n", message.c_str());
    g_lastError = message;
}

// Any structure from the spatial container extensions reaching the runtime is a
// layer bug: a real 1.0 runtime would not know it.
bool HasContainerStruct(const void* next) {
    for (auto* it = static_cast<const XrBaseInStructure*>(next); it != nullptr; it = it->next) {
        if (it->type >= 1000810000 && it->type < 1000814000) return true;
    }
    return false;
}

template <typename T>
T MakeHandle() {
    return reinterpret_cast<T>(static_cast<uintptr_t>(g_nextHandle++));
}

void QueueState(XrSessionState state) {
    XrEventDataSessionStateChanged e{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    e.session = g_session;
    e.state = state;
    e.time = g_time;
    XrEventDataBuffer buffer{};
    std::memcpy(&buffer, &e, sizeof(e));
    g_events.push_back(buffer);
    g_state = state;
}

XrResult XRAPI_CALL EnumerateInstanceExtensionProperties(const char*, uint32_t capacity, uint32_t* count,
                                                         XrExtensionProperties* properties) {
    *count = 1;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 1) return XR_ERROR_SIZE_INSUFFICIENT;
    std::strcpy(properties[0].extensionName, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
    properties[0].extensionVersion = 1;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* info, XrInstance* instance) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        if (std::strcmp(info->enabledExtensionNames[i], XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME) != 0) {
            Fail(std::string("unknown extension requested: ") + info->enabledExtensionNames[i]);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    *instance = MakeHandle<XrInstance>();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroyInstance(XrInstance) { return XR_SUCCESS; }

XrResult XRAPI_CALL GetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
    properties->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    std::strcpy(properties->runtimeName, "Spatial container emulation mock runtime");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* systemId) {
    *systemId = 1;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HasContainerStruct(properties->next)) {
        Fail("container struct in xrGetSystemProperties");
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::strcpy(properties->systemName, "Mock system");
    properties->graphicsProperties = {2048, 2048, 1};
    properties->trackingProperties = {XR_TRUE, XR_TRUE};
    return XR_SUCCESS;
}

XrResult XRAPI_CALL EnumerateViewConfigurations(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count,
                                                XrViewConfigurationType* types) {
    *count = 1;
    if (capacity == 0) return XR_SUCCESS;
    types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL EnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity,
                                                    uint32_t* count, XrViewConfigurationView* views) {
    *count = 2;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < 2; ++i) {
        views[i].recommendedImageRectWidth = 1280;
        views[i].recommendedImageRectHeight = 720;
        views[i].maxImageRectWidth = 2048;
        views[i].maxImageRectHeight = 2048;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo* info, XrSession* session) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HasContainerStruct(info->next)) {
        Fail("container struct in xrCreateSession");
        return XR_ERROR_VALIDATION_FAILURE;
    }
    g_session = MakeHandle<XrSession>();
    *session = g_session;
    QueueState(XR_SESSION_STATE_IDLE);
    QueueState(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroySession(XrSession) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session = XR_NULL_HANDLE;
    g_running = false;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL BeginSession(XrSession, const XrSessionBeginInfo* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running) return XR_ERROR_SESSION_RUNNING;
    if (g_state != XR_SESSION_STATE_READY) return XR_ERROR_SESSION_NOT_READY;
    if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    g_running = true;
    QueueState(XR_SESSION_STATE_SYNCHRONIZED);
    QueueState(XR_SESSION_STATE_VISIBLE);
    QueueState(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL EndSession(XrSession) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (g_state != XR_SESSION_STATE_STOPPING) return XR_ERROR_SESSION_NOT_STOPPING;
    g_running = false;
    QueueState(XR_SESSION_STATE_IDLE);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RequestExitSession(XrSession) { return XR_SUCCESS; }

XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer* event) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_events.empty()) return XR_EVENT_UNAVAILABLE;
    *event = g_events.front();
    g_events.pop_front();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL WaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) {
        Fail("xrWaitFrame while not running");
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    g_time += 11111111;
    state->predictedDisplayTime = g_time;
    state->predictedDisplayPeriod = 11111111;
    state->shouldRender = g_state == XR_SESSION_STATE_VISIBLE || g_state == XR_SESSION_STATE_FOCUSED;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_running ? XR_SUCCESS : XR_ERROR_SESSION_NOT_RUNNING;
}

XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (HasContainerStruct(info->next)) {
        Fail("container struct in XrFrameEndInfo");
        return XR_ERROR_VALIDATION_FAILURE;
    }
    g_frameLayerTypes.clear();
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (HasContainerStruct(info->layers[i]->next)) {
            Fail("container struct in a composition layer");
            return XR_ERROR_VALIDATION_FAILURE;
        }
        g_frameLayerTypes.push_back(info->layers[i]->type);
    }
    g_frameLayerCount = info->layerCount;
    ++g_endFrameCount;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL LocateViews(XrSession, const XrViewLocateInfo*, XrViewState* state, uint32_t capacity, uint32_t* count,
                                XrView* views) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) return XR_ERROR_SESSION_NOT_RUNNING;
    *count = 2;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    for (uint32_t i = 0; i < 2; ++i) {
        views[i].pose = {{0, 0, 0, 1}, {i == 0 ? -0.032f : 0.032f, 0, 0}};
        views[i].fov = {-0.8f, 0.8f, 0.7f, -0.7f};
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space) {
    std::lock_guard<std::mutex> lock(g_mutex);
    *space = MakeHandle<XrSpace>();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroySpace(XrSpace) { return XR_SUCCESS; }

XrResult XRAPI_CALL LocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    location->pose = {{0, 0, 0, 1}, {0, 0, 0}};
    return XR_SUCCESS;
}

XrResult XRAPI_CALL ConvertTimespecTimeToTimeKHR(XrInstance, const timespec* ts, XrTime* time) {
    *time = static_cast<XrTime>(ts->tv_sec) * 1000000000 + ts->tv_nsec;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
#define ENTRY(fn)                                              \
    if (std::strcmp(name, "xr" #fn) == 0) {                    \
        *function = reinterpret_cast<PFN_xrVoidFunction>(fn); \
        return XR_SUCCESS;                                     \
    }
    ENTRY(EnumerateInstanceExtensionProperties)
    ENTRY(CreateInstance)
    ENTRY(DestroyInstance)
    ENTRY(GetInstanceProperties)
    ENTRY(GetSystem)
    ENTRY(GetSystemProperties)
    ENTRY(EnumerateViewConfigurations)
    ENTRY(EnumerateViewConfigurationViews)
    ENTRY(CreateSession)
    ENTRY(DestroySession)
    ENTRY(BeginSession)
    ENTRY(EndSession)
    ENTRY(RequestExitSession)
    ENTRY(PollEvent)
    ENTRY(WaitFrame)
    ENTRY(BeginFrame)
    ENTRY(EndFrame)
    ENTRY(LocateViews)
    ENTRY(CreateReferenceSpace)
    ENTRY(DestroySpace)
    ENTRY(LocateSpace)
    ENTRY(ConvertTimespecTimeToTimeKHR)
    ENTRY(GetInstanceProcAddr)
#undef ENTRY
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

}  // namespace

MOCK_EXPORT XrResult xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest* request) {
    request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    request->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    request->getInstanceProcAddr = GetInstanceProcAddr;
    return XR_SUCCESS;
}

// --- Test controls -------------------------------------------------------------------

MOCK_EXPORT void mock_queue_session_state(XrSessionState state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    QueueState(state);
}

MOCK_EXPORT bool mock_is_running() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_running;
}

MOCK_EXPORT uint32_t mock_end_frame_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_endFrameCount;
}

MOCK_EXPORT uint32_t mock_last_frame_layer_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_frameLayerCount;
}

MOCK_EXPORT const char* mock_last_error() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_lastError.c_str();
}
