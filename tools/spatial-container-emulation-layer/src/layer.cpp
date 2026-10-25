// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// OpenXR API layer that emulates XR_EXT_spatial_container and
// XR_EXT_spatial_container_self_rendering on top of a runtime that only
// implements the classic OpenXR 1.x session and frame loop.
//
// The layer translates in both directions:
// - Toward the app it presents the spatial container session model: the session
//   stays in IDLE, per-container visibility / interactable / bounds events, and
//   per-container view location and layer submission.
// - Toward the runtime it drives the classic model: xrBeginSession on READY,
//   xrEndSession on STOPPING, xrLocateViews, and a single list of layers in
//   xrEndFrame composed from all visible containers.
//
// It is intended for trying spatial container apps on existing runtimes, not as a
// faithful compositor: see README.md for the limitations.

#if defined(_WIN32)
#define XR_USE_PLATFORM_WIN32
#define NOMINMAX
#include <windows.h>
#define LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define XR_USE_TIMESPEC
#include <time.h>
#define LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kLayerName = "XR_APILAYER_spatial_container_emulation";

// --- Configuration (environment variables) -------------------------------------

struct Config {
    XrExtent3Df bounds = {0.5f, 0.5f, 0.5f};
    bool boundsFromEnv = false;
    XrVector3f position = {0.0f, 0.0f, -1.0f};
    uint32_t maxContainers = 16;
    bool supportsBounded = true;
    bool supportsImmersive = true;
    bool denyVisibleRequests = false;
    bool denyBoundsModeRequests = false;
    bool tightFov = true;
    bool log = false;
};

bool ParseFloat3(const char* text, float out[3]) {
    if (text == nullptr) return false;
    float v[3];
    if (std::sscanf(text, "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) return false;
    for (float f : v) {
        if (!std::isfinite(f)) return false;
    }
    std::memcpy(out, v, sizeof(v));
    return true;
}

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

Config LoadConfig() {
    Config config;
    float v[3];
    if (ParseFloat3(std::getenv("XR_SCEMU_BOUNDS"), v) && v[0] > 0 && v[1] > 0 && v[2] > 0) {
        config.bounds = {v[0], v[1], v[2]};
        config.boundsFromEnv = true;
    }
    if (ParseFloat3(std::getenv("XR_SCEMU_POSITION"), v)) {
        config.position = {v[0], v[1], v[2]};
    }
    if (const char* modes = std::getenv("XR_SCEMU_SUPPORTED_MODES")) {
        config.supportsBounded = std::strstr(modes, "bounded") != nullptr;
        config.supportsImmersive = std::strstr(modes, "immersive") != nullptr;
        if (!config.supportsBounded && !config.supportsImmersive) {
            config.supportsBounded = config.supportsImmersive = true;
        }
    }
    if (const char* max = std::getenv("XR_SCEMU_MAX_CONTAINERS")) {
        config.maxContainers = static_cast<uint32_t>(std::strtoul(max, nullptr, 10));
    }
    config.denyVisibleRequests = EnvFlag("XR_SCEMU_DENY_VISIBLE_REQUESTS");
    config.denyBoundsModeRequests = EnvFlag("XR_SCEMU_DENY_BOUNDS_MODE_REQUESTS");
    if (std::getenv("XR_SCEMU_TIGHT_FOV") != nullptr) config.tightFov = EnvFlag("XR_SCEMU_TIGHT_FOV");
    config.log = EnvFlag("XR_SCEMU_LOG");
    return config;
}

const Config& GetConfig() {
    static const Config config = LoadConfig();
    return config;
}

void Log(const char* format, ...) {
    if (!GetConfig().log) return;
    std::va_list args;
    va_start(args, format);
    std::fprintf(stderr, "[%s] ", kLayerName);
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

// --- Next-chain helpers ----------------------------------------------------------

template <typename T>
const T* FindInChain(const void* next, XrStructureType type) {
    for (auto* it = static_cast<const XrBaseInStructure*>(next); it != nullptr; it = it->next) {
        if (it->type == type) return reinterpret_cast<const T*>(it);
    }
    return nullptr;
}

// Temporarily removes structures of the given types from a next chain owned by the
// app, restoring the chain when destroyed. Runtimes that do not know a structure
// would otherwise see it.
class ScopedChainUnlink {
public:
    ScopedChainUnlink(const void* head, std::initializer_list<XrStructureType> types) {
        auto* prev = const_cast<XrBaseInStructure*>(static_cast<const XrBaseInStructure*>(head));
        while (prev != nullptr && prev->next != nullptr) {
            auto* current = const_cast<XrBaseInStructure*>(prev->next);
            if (std::find(types.begin(), types.end(), current->type) != types.end()) {
                removed_.push_back({prev, current});
                prev->next = current->next;
            } else {
                prev = current;
            }
        }
    }
    ~ScopedChainUnlink() {
        for (auto it = removed_.rbegin(); it != removed_.rend(); ++it) {
            it->first->next = it->second;
        }
    }
    ScopedChainUnlink(const ScopedChainUnlink&) = delete;
    ScopedChainUnlink& operator=(const ScopedChainUnlink&) = delete;

private:
    std::vector<std::pair<XrBaseInStructure*, XrBaseInStructure*>> removed_;
};

// --- Next dispatch -----------------------------------------------------------------

struct Dispatch {
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrGetSystemProperties GetSystemProperties = nullptr;
    PFN_xrEnumerateViewConfigurations EnumerateViewConfigurations = nullptr;
    PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrBeginSession BeginSession = nullptr;
    PFN_xrEndSession EndSession = nullptr;
    PFN_xrRequestExitSession RequestExitSession = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrBeginFrame BeginFrame = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrLocateViews LocateViews = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
    PFN_xrLocateSpace LocateSpace = nullptr;
    PFN_xrLocateSpaces LocateSpaces = nullptr;
    PFN_xrLocateSpacesKHR LocateSpacesKHR = nullptr;
#if defined(_WIN32)
    PFN_xrConvertWin32PerformanceCounterToTimeKHR ConvertTime = nullptr;
#else
    PFN_xrConvertTimespecTimeToTimeKHR ConvertTime = nullptr;
#endif
};

// --- State -------------------------------------------------------------------------

// Deep copy of submitted composition layers, so that "retain" can resubmit them
// after the app's structures are gone.
struct LayerCopy {
    XrStructureType type = XR_TYPE_UNKNOWN;
    XrCompositionLayerProjection projection{};
    std::vector<XrCompositionLayerProjectionView> projectionViews;
    XrCompositionLayerQuad quad{};

    const XrCompositionLayerBaseHeader* Header() const {
        if (type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        }
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
};

struct Container {
    XrSpatialContainerEXT handle = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpatialContainerGraphicsPresentationEXT presentation{};
    XrExtent3Df bounds{};
    XrSpatialContainerBoundsModeEXT boundsMode = XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT;
    bool wantVisible = false;
    bool visible = false;
    bool interactable = false;
    bool closed = false;
    bool rendering = false;
    std::vector<LayerCopy> retained;
    XrSpace boundsSpace = XR_NULL_HANDLE;  // Internal space at the container center, for FOV fitting.
};

struct Session {
    XrSession handle = XR_NULL_HANDLE;
    XrInstance instance = XR_NULL_HANDLE;
    bool emulated = false;
    bool idleReported = false;
    bool runtimeRunning = false;
    bool runtimeVisible = false;
    bool runtimeFocused = false;
    bool waitedDown = false;     // The last xrWaitFrame was forwarded to the runtime.
    bool frameDown = false;      // The current frame was begun on the runtime.
    bool inFrame = false;        // Between the app's xrBeginFrame and xrEndFrame.
    bool runtimeShouldRender = false;
    XrViewConfigurationType primaryViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> viewConfigViews;
    XrTime lastDisplayTime = 0;
    XrDuration displayPeriod = 11111111;  // 90 Hz until the runtime reports otherwise.
    std::chrono::steady_clock::time_point lastEmulatedWait{};
    std::vector<XrSpatialContainerEXT> renderOrder;  // Active render set, in begin order.
};

struct Instance {
    XrInstance handle = XR_NULL_HANDLE;
    Dispatch next;
    bool containersEnabled = false;
    bool selfRenderingEnabled = false;
    std::deque<XrEventDataBuffer> events;
};

std::mutex g_mutex;
std::map<XrInstance, std::unique_ptr<Instance>> g_instances;
std::map<XrSession, std::unique_ptr<Session>> g_sessions;
std::map<XrSpatialContainerEXT, std::unique_ptr<Container>> g_containers;
std::map<XrSpace, XrSpatialContainerEXT> g_containerSpaces;  // Kept after container destruction.
uint64_t g_nextContainerId = 1;

Instance* GetInstance(XrInstance instance) {
    auto it = g_instances.find(instance);
    return it == g_instances.end() ? nullptr : it->second.get();
}

Session* GetSession(XrSession session) {
    auto it = g_sessions.find(session);
    return it == g_sessions.end() ? nullptr : it->second.get();
}

Container* GetContainer(XrSpatialContainerEXT container) {
    auto it = g_containers.find(container);
    return it == g_containers.end() ? nullptr : it->second.get();
}

Instance* GetInstanceForSession(const Session* session) {
    return session ? GetInstance(session->instance) : nullptr;
}

// --- Events --------------------------------------------------------------------------

template <typename T>
void QueueEvent(Instance* instance, const T& event) {
    static_assert(sizeof(T) <= sizeof(XrEventDataBuffer), "event too large");
    XrEventDataBuffer buffer{};
    std::memcpy(&buffer, &event, sizeof(T));
    instance->events.push_back(buffer);
}

void QueueVisible(Instance* instance, Container* c) {
    XrEventDataSpatialContainerVisibleChangedEXT e{XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_CHANGED_EXT};
    e.spatialContainer = c->handle;
    e.visible = c->visible;
    QueueEvent(instance, e);
    Log("container %p visible=%d", (void*)c->handle, c->visible);
}

void QueueInteractable(Instance* instance, Container* c) {
    XrEventDataSpatialContainerInteractableChangedEXT e{XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_INTERACTABLE_CHANGED_EXT};
    e.spatialContainer = c->handle;
    e.interactable = c->interactable;
    QueueEvent(instance, e);
    Log("container %p interactable=%d", (void*)c->handle, c->interactable);
}

void FillBounds(const Container* c, XrExtent3Df* bounds, XrBool32* infinite) {
    if (c->boundsMode == XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT) {
        *bounds = {0.0f, 0.0f, 0.0f};
        *infinite = XR_TRUE;
    } else {
        *bounds = c->bounds;
        *infinite = XR_FALSE;
    }
}

void QueueBoundsChanged(Instance* instance, Container* c) {
    // Coalesce with a still-queued bounds event for the same container.
    for (auto& buffer : instance->events) {
        if (buffer.type != XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_CHANGED_EXT) continue;
        auto* e = reinterpret_cast<XrEventDataSpatialContainerBoundsChangedEXT*>(&buffer);
        if (e->spatialContainer == c->handle) {
            FillBounds(c, &e->bounds, &e->infiniteBounds);
            e->boundsMode = c->boundsMode;
            return;
        }
    }
    XrEventDataSpatialContainerBoundsChangedEXT e{XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_CHANGED_EXT};
    e.spatialContainer = c->handle;
    FillBounds(c, &e.bounds, &e.infiniteBounds);
    e.boundsMode = c->boundsMode;
    QueueEvent(instance, e);
    Log("container %p bounds mode=%d", (void*)c->handle, c->boundsMode);
}

template <typename T>
void QueueContainerEvent(Instance* instance, XrStructureType type, XrSpatialContainerEXT handle) {
    T e{type};
    e.spatialContainer = handle;
    QueueEvent(instance, e);
}

bool IsSupportedMode(XrSpatialContainerBoundsModeEXT mode) {
    const Config& config = GetConfig();
    return (mode == XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT && config.supportsBounded) ||
           (mode == XR_SPATIAL_CONTAINER_BOUNDS_MODE_IMMERSIVE_EXT && config.supportsImmersive);
}

// Applies the runtime's session state and the app's requests to every container of
// the session, queueing an event for each change.
void UpdateContainers(Instance* instance, Session* session) {
    for (auto& [handle, c] : g_containers) {
        if (c->session != session->handle || c->closed) continue;
        const bool visible = c->wantVisible && session->runtimeVisible;
        const bool interactable = visible && session->runtimeFocused;
        // Losing interactability is reported before hiding, and gaining after showing.
        if (!interactable && c->interactable) {
            c->interactable = false;
            QueueInteractable(instance, c.get());
        }
        if (visible != c->visible) {
            c->visible = visible;
            QueueVisible(instance, c.get());
        }
        if (interactable && !c->interactable) {
            c->interactable = true;
            QueueInteractable(instance, c.get());
        }
    }
}

void CloseContainers(Instance* instance, Session* session) {
    for (auto& [handle, c] : g_containers) {
        if (c->session != session->handle || c->closed) continue;
        if (c->interactable) {
            c->interactable = false;
            QueueInteractable(instance, c.get());
        }
        if (c->visible) {
            c->visible = false;
            QueueVisible(instance, c.get());
        }
        c->closed = true;
        QueueContainerEvent<XrEventDataSpatialContainerClosedEXT>(instance, XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_CLOSED_EXT,
                                                                   c->handle);
        Log("container %p closed", (void*)c->handle);
    }
}

// Handles a runtime session state change for an emulated session. Returns true if
// the event should be passed to the app.
bool HandleRuntimeSessionState(Instance* instance, Session* session, const XrEventDataSessionStateChanged& event) {
    Log("runtime session state %d", event.state);
    session->lastDisplayTime = std::max(session->lastDisplayTime, event.time);
    switch (event.state) {
        case XR_SESSION_STATE_IDLE:
            // The app sees IDLE once, after creation. Later IDLEs are not a change.
            if (session->idleReported) return false;
            session->idleReported = true;
            return true;
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = session->primaryViewConfig;
            XrResult result = instance->next.BeginSession(session->handle, &beginInfo);
            Log("xrBeginSession on runtime: %d", result);
            session->runtimeRunning = XR_SUCCEEDED(result);
            return false;
        }
        case XR_SESSION_STATE_SYNCHRONIZED:
            session->runtimeVisible = false;
            session->runtimeFocused = false;
            break;
        case XR_SESSION_STATE_VISIBLE:
            session->runtimeVisible = true;
            session->runtimeFocused = false;
            break;
        case XR_SESSION_STATE_FOCUSED:
            session->runtimeVisible = true;
            session->runtimeFocused = true;
            break;
        case XR_SESSION_STATE_STOPPING: {
            session->runtimeVisible = false;
            session->runtimeFocused = false;
            UpdateContainers(instance, session);
            XrResult result = instance->next.EndSession(session->handle);
            Log("xrEndSession on runtime: %d", result);
            session->runtimeRunning = false;
            return false;
        }
        case XR_SESSION_STATE_EXITING:
            CloseContainers(instance, session);
            return false;
        case XR_SESSION_STATE_LOSS_PENDING:
            CloseContainers(instance, session);
            return true;
        default:
            return true;
    }
    UpdateContainers(instance, session);
    return false;
}

// --- Time ----------------------------------------------------------------------------

XrTime CurrentTime(Instance* instance, Session* session) {
    if (instance->next.ConvertTime != nullptr) {
        XrTime time = 0;
#if defined(_WIN32)
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        if (XR_SUCCEEDED(instance->next.ConvertTime(instance->handle, &counter, &time))) return time;
#else
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (XR_SUCCEEDED(instance->next.ConvertTime(instance->handle, &now, &time))) return time;
#endif
    }
    return session->lastDisplayTime + session->displayPeriod;
}

// --- Layer copies ----------------------------------------------------------------------

// Copies a layer for submission. With forRetain, pointers into app memory that may
// not outlive this frame (next chains) are dropped.
bool CopyLayer(const XrCompositionLayerBaseHeader* layer, bool forRetain, LayerCopy* out) {
    out->type = layer->type;
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const auto* src = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        out->projection = *src;
        out->projectionViews.assign(src->views, src->views + src->viewCount);
        if (forRetain) {
            out->projection.next = nullptr;
            for (auto& view : out->projectionViews) view.next = nullptr;
        }
        out->projection.views = out->projectionViews.data();
        return true;
    }
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        out->quad = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
        if (forRetain) out->quad.next = nullptr;
        return true;
    }
    return false;
}

// --- View fitting -----------------------------------------------------------------------

XrVector3f Cross(const XrVector3f& a, const XrVector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f c = Cross(u, v);
    const XrVector3f t{2 * c.x, 2 * c.y, 2 * c.z};
    const XrVector3f c2 = Cross(u, t);
    return {v.x + q.w * t.x + c2.x, v.y + q.w * t.y + c2.y, v.z + q.w * t.z + c2.z};
}

XrPosef Inverse(const XrPosef& p) {
    const XrQuaternionf q{-p.orientation.x, -p.orientation.y, -p.orientation.z, p.orientation.w};
    const XrVector3f t = Rotate(q, {-p.position.x, -p.position.y, -p.position.z});
    return {q, t};
}

// a * b: applies b first, then a.
XrPosef Multiply(const XrPosef& a, const XrPosef& b) {
    const XrQuaternionf& p = a.orientation;
    const XrQuaternionf& q = b.orientation;
    const XrQuaternionf r{p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y, p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x,
                          p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w, p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z};
    const XrVector3f t = Rotate(p, b.position);
    return {r, {a.position.x + t.x, a.position.y + t.y, a.position.z + t.z}};
}

enum class FitResult { Fitted, Unchanged, Outside };

// Narrows a view's FOV to the smallest frustum that contains the container's bounding
// box, as the spec recommends. This also limits the app's rendering to the box's 2D
// pixel bounds (as a rectangle). `scale` returns the fitted size relative to the full
// FOV, per axis.
FitResult FitFov(const XrPosef& containerInView, const XrExtent3Df& bounds, XrFovf* fov, float scale[2]) {
    constexpr float kNear = 0.01f;
    float minX = INFINITY, maxX = -INFINITY, minY = INFINITY, maxY = -INFINITY;
    for (int i = 0; i < 8; ++i) {
        const XrVector3f corner{(i & 1 ? 0.5f : -0.5f) * bounds.width, (i & 2 ? 0.5f : -0.5f) * bounds.height,
                                (i & 4 ? 0.5f : -0.5f) * bounds.depth};
        const XrVector3f r = Rotate(containerInView.orientation, corner);
        const XrVector3f c{r.x + containerInView.position.x, r.y + containerInView.position.y, r.z + containerInView.position.z};
        // A corner at or behind the eye: no finite frustum contains the box.
        if (c.z > -kNear) return FitResult::Unchanged;
        minX = std::min(minX, c.x / -c.z);
        maxX = std::max(maxX, c.x / -c.z);
        minY = std::min(minY, c.y / -c.z);
        maxY = std::max(maxY, c.y / -c.z);
    }
    const float left = std::tan(fov->angleLeft), right = std::tan(fov->angleRight);
    const float down = std::tan(fov->angleDown), up = std::tan(fov->angleUp);
    const float l = std::max(minX, left), r = std::min(maxX, right);
    const float d = std::max(minY, down), u = std::min(maxY, up);
    if (l >= r || d >= u) return FitResult::Outside;
    *fov = {std::atan(l), std::atan(r), std::atan(u), std::atan(d)};
    scale[0] = (r - l) / (right - left);
    scale[1] = (u - d) / (up - down);
    return FitResult::Fitted;
}

// Fits the views of one bounded container. Returns false if the container is
// entirely outside every view.
bool FitContainerViews(Instance* inst, Session* s, Container* c, XrSpace locateSpace, XrTime time, XrView* views,
                       uint32_t viewCount, XrExtent2Di* recommended) {
    if (c->boundsSpace == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        spaceInfo.poseInReferenceSpace.position = GetConfig().position;
        if (XR_FAILED(inst->next.CreateReferenceSpace(s->handle, &spaceInfo, &c->boundsSpace))) return true;
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(inst->next.LocateSpace(c->boundsSpace, locateSpace, time, &location))) return true;
    const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((location.locationFlags & valid) != valid) return true;

    bool anyInside = false;
    float maxScale[2] = {0.0f, 0.0f};
    for (uint32_t v = 0; v < viewCount; ++v) {
        float scale[2] = {1.0f, 1.0f};
        const FitResult result = FitFov(Multiply(Inverse(views[v].pose), location.pose), c->bounds, &views[v].fov, scale);
        if (result == FitResult::Outside) continue;
        anyInside = true;
        maxScale[0] = std::max(maxScale[0], scale[0]);
        maxScale[1] = std::max(maxScale[1], scale[1]);
    }
    if (!anyInside) return false;
    // Keep the angular resolution: the image shrinks with the FOV.
    recommended->width = std::max(1, static_cast<int32_t>(std::ceil(recommended->width * maxScale[0])));
    recommended->height = std::max(1, static_cast<int32_t>(std::ceil(recommended->height * maxScale[1])));
    return true;
}

// --- Intercepted core functions -----------------------------------------------------

XrResult XRAPI_CALL Layer_xrDestroyInstance(XrInstance instance) {
    PFN_xrDestroyInstance next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Instance* inst = GetInstance(instance);
        if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = inst->next.DestroyInstance;
        for (auto it = g_sessions.begin(); it != g_sessions.end();) {
            it = it->second->instance == instance ? g_sessions.erase(it) : std::next(it);
        }
        g_instances.erase(instance);
    }
    return next(instance);
}

XrResult XRAPI_CALL Layer_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* properties) {
    Instance* inst;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        inst = GetInstance(instance);
    }
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    auto* containerProps = const_cast<XrSystemSpatialContainerPropertiesEXT*>(
        FindInChain<XrSystemSpatialContainerPropertiesEXT>(properties->next, XR_TYPE_SYSTEM_SPATIAL_CONTAINER_PROPERTIES_EXT));
    XrResult result;
    {
        ScopedChainUnlink unlink(properties, {XR_TYPE_SYSTEM_SPATIAL_CONTAINER_PROPERTIES_EXT});
        result = inst->next.GetSystemProperties(instance, systemId, properties);
    }
    if (XR_SUCCEEDED(result) && containerProps != nullptr && inst->containersEnabled) {
        const Config& config = GetConfig();
        containerProps->maxSpatialContainerCount = config.maxContainers;
        containerProps->supportsBounded = config.supportsBounded;
        containerProps->supportsImmersive = config.supportsImmersive;
    }
    return result;
}

XrResult XRAPI_CALL Layer_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    Instance* inst;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        inst = GetInstance(instance);
    }
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    const bool emulated = inst->containersEnabled &&
                          FindInChain<XrBaseInStructure>(createInfo->next, XR_TYPE_SESSION_CREATE_INFO_SPATIAL_CONTAINERS_EXT);
    XrResult result;
    {
        ScopedChainUnlink unlink(createInfo, {XR_TYPE_SESSION_CREATE_INFO_SPATIAL_CONTAINERS_EXT});
        result = inst->next.CreateSession(instance, createInfo, session);
    }
    if (XR_FAILED(result)) return result;

    auto state = std::make_unique<Session>();
    state->handle = *session;
    state->instance = instance;
    state->emulated = emulated;
    if (emulated) {
        uint32_t count = 0;
        inst->next.EnumerateViewConfigurations(instance, createInfo->systemId, 0, &count, nullptr);
        std::vector<XrViewConfigurationType> types(count);
        if (count > 0 &&
            XR_SUCCEEDED(inst->next.EnumerateViewConfigurations(instance, createInfo->systemId, count, &count, types.data()))) {
            state->primaryViewConfig = types[0];
        }
        inst->next.EnumerateViewConfigurationViews(instance, createInfo->systemId, state->primaryViewConfig, 0, &count, nullptr);
        state->viewConfigViews.resize(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        inst->next.EnumerateViewConfigurationViews(instance, createInfo->systemId, state->primaryViewConfig, count, &count,
                                                   state->viewConfigViews.data());
        Log("emulated session created, view configuration %d with %u views", state->primaryViewConfig, count);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sessions[*session] = std::move(state);
    return result;
}

XrResult XRAPI_CALL Layer_xrDestroySession(XrSession session) {
    PFN_xrDestroySession next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Session* s = GetSession(session);
        Instance* inst = GetInstanceForSession(s);
        if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = inst->next.DestroySession;
        for (auto it = g_containers.begin(); it != g_containers.end();) {
            it = it->second->session == session ? g_containers.erase(it) : std::next(it);
        }
        g_sessions.erase(session);
    }
    return next(session);
}

// Returns the session if it is emulated, so callers can reject classic calls.
bool IsEmulated(XrSession session, Instance** instance) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = GetSession(session);
    *instance = GetInstanceForSession(s);
    return s != nullptr && s->emulated;
}

XrResult XRAPI_CALL Layer_xrBeginSession(XrSession session, const XrSessionBeginInfo* info) {
    Instance* inst;
    if (IsEmulated(session, &inst)) return XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT;
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    return inst->next.BeginSession(session, info);
}

XrResult XRAPI_CALL Layer_xrEndSession(XrSession session) {
    Instance* inst;
    if (IsEmulated(session, &inst)) return XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT;
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    return inst->next.EndSession(session);
}

XrResult XRAPI_CALL Layer_xrRequestExitSession(XrSession session) {
    Instance* inst;
    if (IsEmulated(session, &inst)) return XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT;
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    return inst->next.RequestExitSession(session);
}

XrResult XRAPI_CALL Layer_xrLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* viewState,
                                        uint32_t capacity, uint32_t* count, XrView* views) {
    Instance* inst;
    if (IsEmulated(session, &inst)) return XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT;
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    return inst->next.LocateViews(session, info, viewState, capacity, count, views);
}

XrResult XRAPI_CALL Layer_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Instance* inst = GetInstance(instance);
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    for (;;) {
        if (!inst->events.empty()) {
            *eventData = inst->events.front();
            inst->events.pop_front();
            return XR_SUCCESS;
        }
        XrResult result = inst->next.PollEvent(instance, eventData);
        if (result != XR_SUCCESS) return result;
        if (eventData->type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) return result;
        const auto event = *reinterpret_cast<XrEventDataSessionStateChanged*>(eventData);
        Session* s = GetSession(event.session);
        if (s == nullptr || !s->emulated) return result;
        if (HandleRuntimeSessionState(inst, s, event)) {
            // Pass the runtime's event through after anything it caused us to queue.
            XrEventDataBuffer copy;
            std::memcpy(&copy, &event, sizeof(event));
            inst->events.push_back(copy);
        }
    }
}

XrResult XRAPI_CALL Layer_xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* frameState) {
    Instance* inst;
    bool running;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Session* s = GetSession(session);
        inst = GetInstanceForSession(s);
        if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
        if (!s->emulated) return inst->next.WaitFrame(session, info, frameState);
        running = s->runtimeRunning;
    }
    if (running) {
        // xrWaitFrame blocks, so it is called without holding the lock.
        XrResult result = inst->next.WaitFrame(session, info, frameState);
        std::lock_guard<std::mutex> lock(g_mutex);
        Session* s = GetSession(session);
        if (s == nullptr) return XR_ERROR_HANDLE_INVALID;
        s->waitedDown = XR_SUCCEEDED(result);
        if (XR_SUCCEEDED(result)) {
            s->runtimeShouldRender = frameState->shouldRender;
            s->lastDisplayTime = frameState->predictedDisplayTime;
            if (frameState->predictedDisplayPeriod > 0) s->displayPeriod = frameState->predictedDisplayPeriod;
            frameState->shouldRender = XR_FALSE;
        }
        return result;
    }

    // The runtime session is not running, but for the app it is: pace frames ourselves.
    std::chrono::steady_clock::time_point wakeUp;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Session* s = GetSession(session);
        if (s == nullptr) return XR_ERROR_HANDLE_INVALID;
        wakeUp = s->lastEmulatedWait + std::chrono::nanoseconds(s->displayPeriod);
    }
    std::this_thread::sleep_until(wakeUp);
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = GetSession(session);
    if (s == nullptr) return XR_ERROR_HANDLE_INVALID;
    s->lastEmulatedWait = std::chrono::steady_clock::now();
    s->waitedDown = false;
    s->runtimeShouldRender = false;
    s->lastDisplayTime = std::max(s->lastDisplayTime + 1, CurrentTime(inst, s) + s->displayPeriod);
    frameState->predictedDisplayTime = s->lastDisplayTime;
    frameState->predictedDisplayPeriod = s->displayPeriod;
    frameState->shouldRender = XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrBeginFrame(XrSession session, const XrFrameBeginInfo* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = GetSession(session);
    Instance* inst = GetInstanceForSession(s);
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    if (!s->emulated) return inst->next.BeginFrame(session, info);
    XrResult result = XR_SUCCESS;
    s->frameDown = false;
    if (s->waitedDown && s->runtimeRunning) {
        result = inst->next.BeginFrame(session, info);
        s->frameDown = XR_SUCCEEDED(result);
    }
    s->waitedDown = false;
    if (XR_SUCCEEDED(result)) s->inFrame = true;
    return result;
}

XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = GetSession(session);
    Instance* inst = GetInstanceForSession(s);
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    if (!s->emulated) return inst->next.EndFrame(session, info);
    if (!s->inFrame) return XR_ERROR_CALL_ORDER_INVALID;
    if (info->layerCount != 0) return XR_ERROR_VALIDATION_FAILURE;

    const auto* containerInfo = FindInChain<XrSpatialContainerLayerFrameEndInfoEXT>(
        info->next, XR_TYPE_SPATIAL_CONTAINER_LAYER_FRAME_END_INFO_EXT);

    // Validate the submission against the active render set.
    std::set<XrSpatialContainerEXT> seen;
    const uint32_t entryCount = containerInfo ? containerInfo->containerLayerCount : 0;
    for (uint32_t i = 0; i < entryCount; ++i) {
        const XrSpatialContainerLayerEXT& entry = containerInfo->containerLayers[i];
        Container* c = GetContainer(entry.spatialContainer);
        if (c == nullptr || c->session != session) return XR_ERROR_HANDLE_INVALID;
        if (!seen.insert(entry.spatialContainer).second) return XR_ERROR_SPATIAL_CONTAINER_DUPLICATE_EXT;
        if (!c->rendering) return XR_ERROR_SPATIAL_CONTAINER_NOT_RENDERING_EXT;
        if (entry.retainPreviousSubmission && entry.layerCount != 0) return XR_ERROR_VALIDATION_FAILURE;
        for (uint32_t j = 0; j < entry.layerCount; ++j) {
            const XrCompositionLayerBaseHeader* layer = entry.layers[j];
            const auto* viewConfig = FindInChain<XrSpatialContainerCompositionLayerViewConfigurationEXT>(
                layer->next, XR_TYPE_SPATIAL_CONTAINER_COMPOSITION_LAYER_VIEW_CONFIGURATION_EXT);
            if (viewConfig != nullptr && layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            if (layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION && layer->type != XR_TYPE_COMPOSITION_LAYER_QUAD) {
                Log("unsupported layer type %d is dropped", layer->type);
            }
        }
    }
    for (XrSpatialContainerEXT handle : s->renderOrder) {
        if (seen.count(handle) == 0) return XR_ERROR_SPATIAL_CONTAINER_MISSING_EXT;
    }

    s->inFrame = false;

    // Update each container's submission, then compose the visible ones in render-set order.
    std::map<XrSpatialContainerEXT, std::vector<LayerCopy>> current;
    std::vector<const XrBaseInStructure*> appLayerChains;
    for (uint32_t i = 0; i < entryCount; ++i) {
        const XrSpatialContainerLayerEXT& entry = containerInfo->containerLayers[i];
        Container* c = GetContainer(entry.spatialContainer);
        if (entry.retainPreviousSubmission) continue;
        c->retained.clear();
        for (uint32_t j = 0; j < entry.layerCount; ++j) {
            LayerCopy frameCopy, retainCopy;
            if (!CopyLayer(entry.layers[j], false, &frameCopy)) continue;
            CopyLayer(entry.layers[j], true, &retainCopy);
            current[c->handle].push_back(std::move(frameCopy));
            c->retained.push_back(std::move(retainCopy));
            appLayerChains.push_back(reinterpret_cast<const XrBaseInStructure*>(entry.layers[j]));
        }
    }
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    for (XrSpatialContainerEXT handle : s->renderOrder) {
        Container* c = GetContainer(handle);
        if (!c->visible || c->closed) continue;
        const auto it = current.find(handle);
        const std::vector<LayerCopy>& source = it != current.end() ? it->second : c->retained;
        for (const LayerCopy& copy : source) layers.push_back(copy.Header());
    }

    if (!s->frameDown) return XR_SUCCESS;
    s->frameDown = false;

    // The copied layers still point at the app's next chains for this frame; hide the
    // structures the runtime does not know about while it reads them.
    std::vector<std::unique_ptr<ScopedChainUnlink>> unlinks;
    for (const XrBaseInStructure* chain : appLayerChains) {
        unlinks.push_back(std::make_unique<ScopedChainUnlink>(
            chain, std::initializer_list<XrStructureType>{XR_TYPE_SPATIAL_CONTAINER_COMPOSITION_LAYER_VIEW_CONFIGURATION_EXT}));
    }
    for (auto& [handle, copies] : current) {
        for (LayerCopy& copy : copies) {
            // The copy's own next pointer may still start with the stripped structure.
            if (copy.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && copy.projection.next != nullptr &&
                static_cast<const XrBaseInStructure*>(copy.projection.next)->type ==
                    XR_TYPE_SPATIAL_CONTAINER_COMPOSITION_LAYER_VIEW_CONFIGURATION_EXT) {
                copy.projection.next = static_cast<const XrBaseInStructure*>(copy.projection.next)->next;
            }
        }
    }
    ScopedChainUnlink unlinkFrameEnd(info, {XR_TYPE_SPATIAL_CONTAINER_LAYER_FRAME_END_INFO_EXT});
    XrFrameEndInfo down = *info;
    down.layerCount = static_cast<uint32_t>(layers.size());
    down.layers = layers.empty() ? nullptr : layers.data();
    return inst->next.EndFrame(session, &down);
}

bool IsUnlocatableContainerSpace(XrSpace space) {
    auto it = g_containerSpaces.find(space);
    if (it == g_containerSpaces.end()) return false;
    Container* c = GetContainer(it->second);
    return c == nullptr || !c->visible;
}

void ClearSpaceLocation(XrSpaceLocation* location) {
    location->locationFlags = 0;
    if (auto* velocity = const_cast<XrSpaceVelocity*>(FindInChain<XrSpaceVelocity>(location->next, XR_TYPE_SPACE_VELOCITY))) {
        velocity->velocityFlags = 0;
    }
}

XrResult XRAPI_CALL Layer_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
    PFN_xrLocateSpace next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_instances.empty()) return XR_ERROR_HANDLE_INVALID;
        next = g_instances.begin()->second->next.LocateSpace;
    }
    XrResult result = next(space, baseSpace, time, location);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (XR_SUCCEEDED(result) && (IsUnlocatableContainerSpace(space) || IsUnlocatableContainerSpace(baseSpace))) {
        ClearSpaceLocation(location);
    }
    return result;
}

void ClearSpaceLocations(const XrSpacesLocateInfo* info, XrSpaceLocations* locations) {
    const bool baseHidden = IsUnlocatableContainerSpace(info->baseSpace);
    for (uint32_t i = 0; i < info->spaceCount && i < locations->locationCount; ++i) {
        if (baseHidden || IsUnlocatableContainerSpace(info->spaces[i])) {
            locations->locations[i].locationFlags = 0;
        }
    }
}

XrResult XRAPI_CALL Layer_xrLocateSpaces(XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* locations) {
    PFN_xrLocateSpaces next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Instance* inst = GetInstanceForSession(GetSession(session));
        if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = inst->next.LocateSpaces;
    }
    XrResult result = next(session, info, locations);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (XR_SUCCEEDED(result)) ClearSpaceLocations(info, locations);
    return result;
}

XrResult XRAPI_CALL Layer_xrLocateSpacesKHR(XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* locations) {
    PFN_xrLocateSpacesKHR next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Instance* inst = GetInstanceForSession(GetSession(session));
        if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = inst->next.LocateSpacesKHR;
    }
    XrResult result = next(session, info, locations);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (XR_SUCCEEDED(result)) ClearSpaceLocations(info, locations);
    return result;
}

XrResult XRAPI_CALL Layer_xrDestroySpace(XrSpace space) {
    PFN_xrDestroySpace next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_instances.empty()) return XR_ERROR_HANDLE_INVALID;
        next = g_instances.begin()->second->next.DestroySpace;
        g_containerSpaces.erase(space);
    }
    return next(space);
}

// --- XR_EXT_spatial_container ------------------------------------------------------

// Looks up a container the app may operate on. Closed containers are reported as such.
XrResult LookupOpenContainer(XrSpatialContainerEXT handle, Container** container, Instance** instance) {
    Container* c = GetContainer(handle);
    if (c == nullptr) return XR_ERROR_HANDLE_INVALID;
    Instance* inst = GetInstanceForSession(GetSession(c->session));
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    if (c->closed) return XR_ERROR_SPATIAL_CONTAINER_CLOSED_EXT;
    *container = c;
    *instance = inst;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrCreateSpatialContainerEXT(XrSession session, const XrSpatialContainerCreateInfoEXT* createInfo,
                                                      XrSpatialContainerEXT* spatialContainer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = GetSession(session);
    if (s == nullptr) return XR_ERROR_HANDLE_INVALID;
    if (!s->emulated) return XR_ERROR_SPATIAL_CONTAINERS_NOT_ENABLED_EXT;
    if (createInfo == nullptr || createInfo->type != XR_TYPE_SPATIAL_CONTAINER_CREATE_INFO_EXT) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const XrExtent3Df& suggested = createInfo->suggestedBounds;
    for (float v : {suggested.width, suggested.height, suggested.depth}) {
        if (!std::isfinite(v) || v < 0.0f) return XR_ERROR_VALIDATION_FAILURE;
    }
    uint32_t count = 0;
    for (auto& [handle, c] : g_containers) count += c->session == session ? 1 : 0;
    if (count >= GetConfig().maxContainers) return XR_ERROR_LIMIT_REACHED;

    auto c = std::make_unique<Container>();
    c->handle = reinterpret_cast<XrSpatialContainerEXT>(static_cast<uintptr_t>(g_nextContainerId++));
    c->session = session;
    c->presentation = createInfo->graphicsPresentation;
    const bool hasSuggestion = suggested.width > 0 && suggested.height > 0 && suggested.depth > 0;
    c->bounds = GetConfig().boundsFromEnv || !hasSuggestion ? GetConfig().bounds : suggested;
    *spatialContainer = c->handle;
    Log("container %p created, bounds %.3f x %.3f x %.3f", (void*)c->handle, c->bounds.width, c->bounds.height,
        c->bounds.depth);
    g_containers[c->handle] = std::move(c);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrDestroySpatialContainerEXT(XrSpatialContainerEXT spatialContainer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Container* c = GetContainer(spatialContainer);
    if (c == nullptr) return XR_ERROR_HANDLE_INVALID;
    Session* s = GetSession(c->session);
    if (s != nullptr) {
        auto& order = s->renderOrder;
        order.erase(std::remove(order.begin(), order.end(), spatialContainer), order.end());
    }
    // No events may be delivered for a destroyed container.
    if (Instance* inst = GetInstanceForSession(s)) {
        auto& events = inst->events;
        events.erase(std::remove_if(events.begin(), events.end(),
                                    [&](const XrEventDataBuffer& buffer) {
                                        switch (buffer.type) {
                                            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_CLOSED_EXT:
                                            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_CHANGED_EXT:
                                            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_CHANGED_EXT:
                                            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_REQUEST_DENIED_EXT:
                                            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_INTERACTABLE_CHANGED_EXT:
                                            case XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_DENIED_EXT:
                                                // All of these have the container right after type and next.
                                                return reinterpret_cast<const XrEventDataSpatialContainerClosedEXT*>(&buffer)
                                                           ->spatialContainer == spatialContainer;
                                            default:
                                                return false;
                                        }
                                    }),
                     events.end());
    }
    if (Instance* inst = GetInstanceForSession(s); inst != nullptr && c->boundsSpace != XR_NULL_HANDLE) {
        inst->next.DestroySpace(c->boundsSpace);
    }
    g_containers.erase(spatialContainer);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrCreateSpatialContainerSpaceEXT(XrSession session, const XrSpatialContainerSpaceCreateInfoEXT* createInfo,
                                                           XrSpace* space) {
    Instance* inst;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Container* c;
        XrResult result = LookupOpenContainer(createInfo->spatialContainer, &c, &inst);
        if (XR_FAILED(result)) return result;
        if (c->session != session) return XR_ERROR_VALIDATION_FAILURE;
    }
    // The container stays where it was configured, gravity aligned and facing the user.
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    spaceInfo.poseInReferenceSpace.position = GetConfig().position;
    XrResult result = inst->next.CreateReferenceSpace(session, &spaceInfo, space);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_containerSpaces[*space] = createInfo->spatialContainer;
    }
    return result;
}

XrResult XRAPI_CALL Layer_xrRequestSpatialContainerVisibleEXT(XrSpatialContainerEXT spatialContainer,
                                                              const XrSpatialContainerVisibleRequestInfoEXT* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Container* c;
    Instance* inst;
    XrResult result = LookupOpenContainer(spatialContainer, &c, &inst);
    if (XR_FAILED(result)) return result;
    if (info->visible && (GetConfig().denyVisibleRequests || !IsSupportedMode(c->boundsMode))) {
        QueueContainerEvent<XrEventDataSpatialContainerVisibleRequestDeniedEXT>(
            inst, XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_VISIBLE_REQUEST_DENIED_EXT, spatialContainer);
        Log("container %p show request denied", (void*)spatialContainer);
        return XR_SUCCESS;
    }
    c->wantVisible = info->visible;
    UpdateContainers(inst, GetSession(c->session));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrRequestSpatialContainerBoundsModeEXT(XrSpatialContainerEXT spatialContainer,
                                                                 const XrSpatialContainerBoundsModeRequestInfoEXT* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Container* c;
    Instance* inst;
    XrResult result = LookupOpenContainer(spatialContainer, &c, &inst);
    if (XR_FAILED(result)) return result;
    if (!IsSupportedMode(info->boundsMode)) return XR_ERROR_VALIDATION_FAILURE;
    // Requests for a hidden container must be honored.
    if (c->visible && GetConfig().denyBoundsModeRequests) {
        QueueContainerEvent<XrEventDataSpatialContainerBoundsModeRequestDeniedEXT>(
            inst, XR_TYPE_EVENT_DATA_SPATIAL_CONTAINER_BOUNDS_MODE_REQUEST_DENIED_EXT, spatialContainer);
        Log("container %p bounds mode request denied", (void*)spatialContainer);
        return XR_SUCCESS;
    }
    if (c->boundsMode != info->boundsMode) {
        c->boundsMode = info->boundsMode;
        QueueBoundsChanged(inst, c);
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrGetSpatialContainerBoundsEXT(XrSpatialContainerEXT spatialContainer,
                                                         const XrSpatialContainerBoundsGetInfoEXT*,
                                                         XrSpatialContainerBoundsEXT* bounds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Container* c;
    Instance* inst;
    XrResult result = LookupOpenContainer(spatialContainer, &c, &inst);
    if (XR_FAILED(result)) return result;
    FillBounds(c, &bounds->bounds, &bounds->infiniteBounds);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrGetSpatialContainerStateEXT(XrSpatialContainerEXT spatialContainer,
                                                        const XrSpatialContainerStateGetInfoEXT*,
                                                        XrSpatialContainerStateEXT* state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Container* c;
    Instance* inst;
    XrResult result = LookupOpenContainer(spatialContainer, &c, &inst);
    if (XR_FAILED(result)) return result;
    state->visible = c->visible;
    state->interactable = c->interactable;
    state->boundsMode = c->boundsMode;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT(
    XrInstance instance, XrSystemId, uint32_t capacity, uint32_t* count, XrSpatialContainerGraphicsPresentationEXT* presentations) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Instance* inst = GetInstance(instance);
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    const uint32_t available = inst->selfRenderingEnabled ? 1 : 0;
    *count = available;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < available) return XR_ERROR_SIZE_INSUFFICIENT;
    if (available > 0) presentations[0] = XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT;
    return XR_SUCCESS;
}

// --- XR_EXT_spatial_container_self_rendering --------------------------------------------

XrResult XRAPI_CALL Layer_xrBeginSpatialContainerRenderingEXT(XrSession session, const XrSpatialContainerBeginInfoEXT* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = GetSession(session);
    Container* c = GetContainer(info->spatialContainer);
    if (s == nullptr || c == nullptr || c->session != session) return XR_ERROR_HANDLE_INVALID;
    if (c->presentation != XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT) {
        return XR_ERROR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_MISSING_EXT;
    }
    if (c->rendering) return XR_ERROR_SPATIAL_CONTAINER_IS_RENDERING_EXT;
    if (s->inFrame) return XR_ERROR_CALL_ORDER_INVALID;
    if (info->primaryViewConfigurationType != s->primaryViewConfig) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    c->rendering = true;
    s->renderOrder.push_back(c->handle);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrEndSpatialContainerRenderingEXT(XrSession session, const XrSpatialContainerEndInfoEXT* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = GetSession(session);
    Container* c = GetContainer(info->spatialContainer);
    if (s == nullptr || c == nullptr || c->session != session) return XR_ERROR_HANDLE_INVALID;
    if (!c->rendering) return XR_ERROR_SPATIAL_CONTAINER_NOT_RENDERING_EXT;
    if (s->inFrame) return XR_ERROR_CALL_ORDER_INVALID;
    c->rendering = false;
    c->retained.clear();
    auto& order = s->renderOrder;
    order.erase(std::remove(order.begin(), order.end(), c->handle), order.end());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrLocateSpatialContainerViewsEXT(XrSession session, const XrSpatialContainerViewsLocateInfoEXT* info,
                                                           uint32_t viewStateCount, XrSpatialContainerViewStateEXT* viewStates,
                                                           uint32_t viewCount, XrView* views) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* s = GetSession(session);
    Instance* inst = GetInstanceForSession(s);
    if (inst == nullptr) return XR_ERROR_HANDLE_INVALID;
    if (viewStateCount != info->viewLocateInfoCount) return XR_ERROR_VALIDATION_FAILURE;
    const uint32_t perContainer = static_cast<uint32_t>(s->viewConfigViews.size());
    for (uint32_t i = 0; i < info->viewLocateInfoCount; ++i) {
        const XrSpatialContainerViewLocateInfoEXT& locate = info->viewLocateInfos[i];
        if (locate.viewConfigurationType != s->primaryViewConfig) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
        Container* c = GetContainer(locate.spatialContainer);
        if (c == nullptr || c->session != session) return XR_ERROR_HANDLE_INVALID;
        if (!c->rendering) return XR_ERROR_SPATIAL_CONTAINER_NOT_RENDERING_EXT;
    }
    if (viewCount != perContainer * info->viewLocateInfoCount) return XR_ERROR_VALIDATION_FAILURE;

    XrExtent2Di recommended{1, 1};
    if (!s->viewConfigViews.empty()) {
        recommended.width = static_cast<int32_t>(s->viewConfigViews[0].recommendedImageRectWidth);
        recommended.height = static_cast<int32_t>(s->viewConfigViews[0].recommendedImageRectHeight);
    }
    for (uint32_t i = 0; i < info->viewLocateInfoCount; ++i) {
        const XrSpatialContainerViewLocateInfoEXT& locate = info->viewLocateInfos[i];
        Container* c = GetContainer(locate.spatialContainer);
        XrSpatialContainerViewStateEXT& state = viewStates[i];
        XrView* containerViews = views + i * perContainer;
        state.viewConfigurationType = locate.viewConfigurationType;
        state.recommendedImageExtent = recommended;
        state.viewStateFlags = 0;
        state.shouldSubmitLayers = XR_FALSE;
        if (!s->runtimeRunning || !c->visible || c->closed) {
            for (uint32_t v = 0; v < perContainer; ++v) {
                containerViews[v].pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
                containerViews[v].fov = {-0.785f, 0.785f, 0.785f, -0.785f};
            }
            continue;
        }
        XrViewLocateInfo viewLocate{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocate.viewConfigurationType = locate.viewConfigurationType;
        viewLocate.displayTime = info->displayTime;
        viewLocate.space = locate.space;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t located = 0;
        XrResult result = inst->next.LocateViews(session, &viewLocate, &viewState, perContainer, &located, containerViews);
        if (XR_FAILED(result)) return result;
        state.viewStateFlags = viewState.viewStateFlags;
        bool onScreen = true;
        if (GetConfig().tightFov && c->boundsMode == XR_SPATIAL_CONTAINER_BOUNDS_MODE_BOUNDED_EXT &&
            (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
            onScreen = FitContainerViews(inst, s, c, locate.space, info->displayTime, containerViews, perContainer,
                                         &state.recommendedImageExtent);
        }
        state.shouldSubmitLayers = s->runtimeShouldRender && onScreen &&
                                   (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    }
    return XR_SUCCESS;
}

// --- Dispatch table --------------------------------------------------------------------

struct Intercept {
    const char* name;
    PFN_xrVoidFunction function;
    int extension;  // 0: core, 1: XR_EXT_spatial_container, 2: self rendering
};

#define INTERCEPT(name, ext) {#name, reinterpret_cast<PFN_xrVoidFunction>(Layer_##name), ext}
const Intercept kIntercepts[] = {
    INTERCEPT(xrDestroyInstance, 0),
    INTERCEPT(xrGetSystemProperties, 0),
    INTERCEPT(xrCreateSession, 0),
    INTERCEPT(xrDestroySession, 0),
    INTERCEPT(xrBeginSession, 0),
    INTERCEPT(xrEndSession, 0),
    INTERCEPT(xrRequestExitSession, 0),
    INTERCEPT(xrLocateViews, 0),
    INTERCEPT(xrPollEvent, 0),
    INTERCEPT(xrWaitFrame, 0),
    INTERCEPT(xrBeginFrame, 0),
    INTERCEPT(xrEndFrame, 0),
    INTERCEPT(xrLocateSpace, 0),
    INTERCEPT(xrLocateSpaces, 0),
    INTERCEPT(xrLocateSpacesKHR, 0),
    INTERCEPT(xrDestroySpace, 0),
    INTERCEPT(xrCreateSpatialContainerEXT, 1),
    INTERCEPT(xrDestroySpatialContainerEXT, 1),
    INTERCEPT(xrCreateSpatialContainerSpaceEXT, 1),
    INTERCEPT(xrRequestSpatialContainerVisibleEXT, 1),
    INTERCEPT(xrRequestSpatialContainerBoundsModeEXT, 1),
    INTERCEPT(xrGetSpatialContainerBoundsEXT, 1),
    INTERCEPT(xrGetSpatialContainerStateEXT, 1),
    INTERCEPT(xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT, 1),
    INTERCEPT(xrBeginSpatialContainerRenderingEXT, 2),
    INTERCEPT(xrEndSpatialContainerRenderingEXT, 2),
    INTERCEPT(xrLocateSpatialContainerViewsEXT, 2),
};
#undef INTERCEPT

XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    Instance* inst;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        inst = GetInstance(instance);
    }
    if (inst == nullptr) {
        *function = nullptr;
        return XR_ERROR_HANDLE_INVALID;
    }
    if (inst->containersEnabled) {
        for (const Intercept& intercept : kIntercepts) {
            if (std::strcmp(name, intercept.name) != 0) continue;
            if (intercept.extension == 2 && !inst->selfRenderingEnabled) break;
            // Space location wrappers are only useful if the runtime has the function.
            if (intercept.function == reinterpret_cast<PFN_xrVoidFunction>(Layer_xrLocateSpaces) && !inst->next.LocateSpaces) break;
            if (intercept.function == reinterpret_cast<PFN_xrVoidFunction>(Layer_xrLocateSpacesKHR) &&
                !inst->next.LocateSpacesKHR) {
                break;
            }
            *function = intercept.function;
            return XR_SUCCESS;
        }
        for (const Intercept& intercept : kIntercepts) {
            if (intercept.extension != 0 && std::strcmp(name, intercept.name) == 0) {
                *function = nullptr;
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            }
        }
    }
    return inst->next.GetInstanceProcAddr(instance, name, function);
}

template <typename T>
void LoadNext(PFN_xrGetInstanceProcAddr gipa, XrInstance instance, const char* name, T* out) {
    PFN_xrVoidFunction function = nullptr;
    if (XR_SUCCEEDED(gipa(instance, name, &function))) *out = reinterpret_cast<T>(function);
}

XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo,
                                                   XrInstance* instance) {
    if (layerInfo == nullptr || layerInfo->nextInfo == nullptr ||
        layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        std::strcmp(layerInfo->nextInfo->layerName, kLayerName) != 0) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    const XrApiLayerNextInfo* nextInfo = layerInfo->nextInfo;

    // Hide our extensions from the runtime, and ask for a time conversion extension so
    // frames can be paced while the runtime session is not running.
    bool containers = false, selfRendering = false;
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const char* ext = info->enabledExtensionNames[i];
        if (std::strcmp(ext, XR_EXT_SPATIAL_CONTAINER_EXTENSION_NAME) == 0) {
            containers = true;
        } else if (std::strcmp(ext, XR_EXT_SPATIAL_CONTAINER_SELF_RENDERING_EXTENSION_NAME) == 0) {
            selfRendering = true;
        } else {
            extensions.push_back(ext);
        }
    }
#if defined(_WIN32)
    const char* timeExtension = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
    const char* timeFunction = "xrConvertWin32PerformanceCounterToTimeKHR";
#else
    const char* timeExtension = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
    const char* timeFunction = "xrConvertTimespecTimeToTimeKHR";
#endif
    const bool appHasTime = std::any_of(extensions.begin(), extensions.end(),
                                        [&](const char* ext) { return std::strcmp(ext, timeExtension) == 0; });
    bool addedTime = containers && !appHasTime;
    if (addedTime) extensions.push_back(timeExtension);

    XrInstanceCreateInfo downInfo = *info;
    XrApiLayerCreateInfo downLayerInfo = *layerInfo;
    downLayerInfo.nextInfo = nextInfo->next;
    downInfo.enabledExtensionNames = extensions.data();
    downInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    XrResult result = nextInfo->nextCreateApiLayerInstance(&downInfo, &downLayerInfo, instance);
    if (result == XR_ERROR_EXTENSION_NOT_PRESENT && addedTime) {
        extensions.pop_back();
        addedTime = false;
        downInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        result = nextInfo->nextCreateApiLayerInstance(&downInfo, &downLayerInfo, instance);
    }
    if (XR_FAILED(result)) return result;

    auto inst = std::make_unique<Instance>();
    inst->handle = *instance;
    inst->containersEnabled = containers;
    inst->selfRenderingEnabled = containers && selfRendering;
    Dispatch& next = inst->next;
    PFN_xrGetInstanceProcAddr gipa = nextInfo->nextGetInstanceProcAddr;
    next.GetInstanceProcAddr = gipa;
    LoadNext(gipa, *instance, "xrDestroyInstance", &next.DestroyInstance);
    LoadNext(gipa, *instance, "xrGetSystemProperties", &next.GetSystemProperties);
    LoadNext(gipa, *instance, "xrEnumerateViewConfigurations", &next.EnumerateViewConfigurations);
    LoadNext(gipa, *instance, "xrEnumerateViewConfigurationViews", &next.EnumerateViewConfigurationViews);
    LoadNext(gipa, *instance, "xrCreateSession", &next.CreateSession);
    LoadNext(gipa, *instance, "xrDestroySession", &next.DestroySession);
    LoadNext(gipa, *instance, "xrBeginSession", &next.BeginSession);
    LoadNext(gipa, *instance, "xrEndSession", &next.EndSession);
    LoadNext(gipa, *instance, "xrRequestExitSession", &next.RequestExitSession);
    LoadNext(gipa, *instance, "xrPollEvent", &next.PollEvent);
    LoadNext(gipa, *instance, "xrWaitFrame", &next.WaitFrame);
    LoadNext(gipa, *instance, "xrBeginFrame", &next.BeginFrame);
    LoadNext(gipa, *instance, "xrEndFrame", &next.EndFrame);
    LoadNext(gipa, *instance, "xrLocateViews", &next.LocateViews);
    LoadNext(gipa, *instance, "xrCreateReferenceSpace", &next.CreateReferenceSpace);
    LoadNext(gipa, *instance, "xrDestroySpace", &next.DestroySpace);
    LoadNext(gipa, *instance, "xrLocateSpace", &next.LocateSpace);
    LoadNext(gipa, *instance, "xrLocateSpaces", &next.LocateSpaces);
    LoadNext(gipa, *instance, "xrLocateSpacesKHR", &next.LocateSpacesKHR);
    if (addedTime || appHasTime) LoadNext(gipa, *instance, timeFunction, &next.ConvertTime);
    Log("instance created: spatial_container=%d self_rendering=%d time_conversion=%d", containers, selfRendering,
        next.ConvertTime != nullptr);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances[*instance] = std::move(inst);
    return XR_SUCCESS;
}

}  // namespace

LAYER_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                    const char* layerName,
                                                                    XrNegotiateApiLayerRequest* request) {
    if (loaderInfo == nullptr || request == nullptr || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        (layerName != nullptr && std::strcmp(layerName, kLayerName) != 0)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = Layer_xrGetInstanceProcAddr;
    request->createApiLayerInstance = Layer_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
