# Runtime implementation checklist

A checklist of the normative requirements ("must" / "must not", plus selected "should") that `XR_EXT_spatial_container` (rev. 1) and `XR_EXT_spatial_container_self_rendering` place on **runtimes**. It is a reading aid for implementers, not a replacement for the specification. Wording is paraphrased; check the spec text before relying on any item.

Source: [KhronosGroup/OpenXR-Docs](https://github.com/KhronosGroup/OpenXR-Docs), `specification/sources/chapters/extensions/ext/ext_spatial_container.adoc` and `ext_spatial_container_self_rendering.adoc`.

Items marked **(Godot)** are not spec requirements, but things the Godot 4.8 implementation relies on.

## System properties

- [ ] Support at least one of bounded or immersive (`supportsBounded` / `supportsImmersive`).
- [ ] If the runtime renders graphics and supports `self_rendering`, set `supportsImmersive = XR_TRUE`.
- [ ] `xrEnumerateSupportedSpatialContainerGraphicsPresentationsEXT` returns identical contents for a given `systemId` for the lifetime of the instance.
- [ ] Should: allow apps to create many containers, limited only by memory. A separate limit on *displayed* containers is allowed.
- [ ] **(Godot)** `maxSpatialContainerCount` > 0. Godot does not create a container otherwise.
- [ ] **(Godot)** Expose both extensions. Godot uses spatial containers only when both are enabled.

## Session behavior when containers are enabled

Enabled by chaining `XrSessionCreateInfoSpatialContainersEXT` to `xrCreateSession`.

- [ ] Never queue READY, SYNCHRONIZED, VISIBLE, FOCUSED, STOPPING, or EXITING session state events.
- [ ] Treat the session as running while in IDLE.
- [ ] Treat the session as focused while in IDLE with at least one interactable container.
- [ ] `xrBeginSession`, `xrEndSession`, `xrRequestExitSession`, and `xrLocateViews` return `XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT`.
- [ ] `xrWaitFrame` always sets `XrFrameState::shouldRender` to false.
- [ ] `xrEndFrame` with a non-zero `XrFrameEndInfo::layerCount` returns `XR_ERROR_VALIDATION_FAILURE`.
- [ ] `xrCreateSession` no longer requires a graphics binding (other extensions such as `self_rendering` may require one).
- [ ] Before queueing LOSS_PENDING, queue `XrEventDataSpatialContainerClosedEXT` for every active container.
- [ ] Events that extensions normally queue when the session starts running (e.g. `XR_EXT_user_presence`) are queued after session creation.
- [ ] Do not enable `XR_MND_headless` behavior for such sessions.
- [ ] **(Godot)** Queue the session state change event for IDLE after session creation, as in core OpenXR. Godot starts its render loop when it receives this event.

## Container creation and destruction

- [ ] A new container is not visible, not interactable, and in BOUNDED mode.
- [ ] `suggestedBounds` with a negative, infinite, or NaN dimension returns `XR_ERROR_VALIDATION_FAILURE`.
- [ ] Creating more than `maxSpatialContainerCount` returns `XR_ERROR_LIMIT_REACHED`.
- [ ] `xrCreateSpatialContainerEXT` on a session without containers enabled returns `XR_ERROR_SPATIAL_CONTAINERS_NOT_ENABLED_EXT`.
- [ ] `suggestedBounds` may be ignored. A runtime that supports non-box shapes uses it as a bounding box around the shape.
- [ ] After `xrDestroySpatialContainerEXT`, never show the container's contents again and queue no further events for it.

## Closing

- [ ] When a container can no longer be displayed (user closes it, LOSS_PENDING), queue `XrEventDataSpatialContainerClosedEXT`.
- [ ] If the container was visible, first make it not visible and queue `VisibleChanged(false)`.
- [ ] After closing, every function on that container except `xrDestroySpatialContainerEXT` returns `XR_ERROR_SPATIAL_CONTAINER_CLOSED_EXT`.

## Container space

- [ ] The origin is the center of the container bounds. +X is right, +Y is top, +Z is the front face.
- [ ] A gravity-aligned container in front of a viewer at the identity pose of LOCAL has the identity orientation relative to LOCAL.
- [ ] The space handle stays valid after the container is destroyed.
- [ ] While the container is not visible or destroyed, the space is unlocatable (`locationFlags` and `viewStateFlags` are 0).
- [ ] When bounds and the local origin change together (e.g. dragging one edge), change both simultaneously.

## State events (general)

- [ ] Queue an event for every state change, both app-requested and runtime-initiated.
- [ ] Do not queue change events that do not indicate a change (e.g. two hide requests in a row).
- [ ] Deferred requests produce no event until they are applied or denied.

## Visibility

- [ ] Never show a container before the app has requested to show it at least once.
- [ ] Every change of visibility queues `XrEventDataSpatialContainerVisibleChangedEXT`.
- [ ] Every denied show request queues `XrEventDataSpatialContainerVisibleRequestDeniedEXT`. With multiple denied requests, queue one event per request.
- [ ] Deny show requests for a bounds mode the system does not support.
- [ ] Should: honor requests to hide.
- [ ] Should: after reporting visible, not delay rendering the container for more than 0.5 s (`visible` already reads true during the delay).

## Interactable

- [ ] Apps cannot request it. The runtime alone decides.
- [ ] An interactable container is always visible.
- [ ] Every change queues `XrEventDataSpatialContainerInteractableChangedEXT`.
- [ ] If at most one container can be interactable, queue the event for the container losing interactability before the one gaining it.

## Bounds and bounds mode

- [ ] Bounded containers always have finite bounds, and immersive containers always have infinite bounds.
- [ ] Infinite bounds are reported as `infiniteBounds = XR_TRUE` with width, height, and depth 0.0. Finite bounds are reported as `XR_FALSE` with positive finite values.
- [ ] Every change of bounds or bounds mode queues `XrEventDataSpatialContainerBoundsChangedEXT`.
- [ ] Coalesce unpolled bounds events. Only the latest change is reported.
- [ ] During interactive resizing, either queue events at frame rate or a single event at the end. In the latter case, `xrGetSpatialContainerBoundsEXT` keeps returning the old bounds until the event is queued.
- [ ] Always honor a bounds mode request for a container that is not visible. If the runtime would deny it, reject or defer the later show request instead.
- [ ] Every denied bounds mode request for a visible container queues `XrEventDataSpatialContainerBoundsModeRequestDeniedEXT`.
- [ ] Requesting an unsupported bounds mode returns `XR_ERROR_VALIDATION_FAILURE`.
- [ ] Treat immersive containers the same as a full-screen immersive app without this extension, except for the session changes above.
- [ ] If other containers are hidden while an immersive container is visible, queue `VisibleChanged(false)` for them.

## Self rendering: lifecycle

- [ ] `xrBeginSpatialContainerRenderingEXT` on a container not created with `XR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_SELF_RENDERING_EXT` returns `XR_ERROR_SPATIAL_CONTAINER_GRAPHICS_PRESENTATION_MISSING_EXT`.
- [ ] Beginning a container that is already in the active render set returns `XR_ERROR_SPATIAL_CONTAINER_IS_RENDERING_EXT`.
- [ ] Ending a container that is not in the active render set returns `XR_ERROR_SPATIAL_CONTAINER_NOT_RENDERING_EXT`.
- [ ] Beginning or ending between `xrBeginFrame` and `xrEndFrame` returns `XR_ERROR_CALL_ORDER_INVALID`.
- [ ] Destroying a container in the active render set behaves as if `xrEndSpatialContainerRenderingEXT` were called first.

## Self rendering: locating views

- [ ] Return views ordered by container index in `viewLocateInfos`, then by view index within the container.
- [ ] `viewCount` not equal to the sum of the view counts of the requested view configurations returns `XR_ERROR_VALIDATION_FAILURE`.
- [ ] `viewStateCount` not equal to `viewLocateInfoCount` returns `XR_ERROR_VALIDATION_FAILURE`.
- [ ] A container that is not in the active render set returns `XR_ERROR_SPATIAL_CONTAINER_NOT_RENDERING_EXT`.
- [ ] When decaying a view configuration, still return the original number of views, filling the extra views with duplicates.
- [ ] When no valid view can be produced (e.g. the container is off screen), set `shouldSubmitLayers = XR_FALSE`.
- [ ] After a container is closed, return `shouldSubmitLayers = XR_FALSE` and `viewStateFlags = 0` for it.
- [ ] `recommendedImageExtent` width and height are positive and no larger than `recommendedImageRectWidth` / `recommendedImageRectHeight` of the primary view configuration.
- [ ] Should: make each `XrView::fov` the smallest frustum that contains the whole container. Asymmetric FOVs are allowed.
- [ ] Should: account for container motion when filling views, to reduce reprojection artifacts.

## Self rendering: submitting layers

- [ ] A container referenced more than once in `containerLayers` returns `XR_ERROR_SPATIAL_CONTAINER_DUPLICATE_EXT`.
- [ ] A container in the active render set but missing from `containerLayers` (or the structure not chained at all) returns `XR_ERROR_SPATIAL_CONTAINER_MISSING_EXT`.
- [ ] A container in `containerLayers` that is not in the active render set returns `XR_ERROR_SPATIAL_CONTAINER_NOT_RENDERING_EXT`.
- [ ] In any of the three error cases above, render nothing for any container.
- [ ] `retainPreviousSubmission = XR_TRUE` with a non-zero `layerCount` returns `XR_ERROR_VALIDATION_FAILURE`.
- [ ] Retain: reuse the layers and swapchain contents latched at the last submission without retain. Later swapchain changes are not reflected.
- [ ] Clear (`layerCount = 0`, no retain): treat it as transparent black for every pixel of the container.
- [ ] Compose layers within a container in the submitted order (painter's algorithm).
- [ ] A swapchain submitted for several containers is rendered in all of them.
- [ ] `XrSpatialContainerCompositionLayerViewConfigurationEXT` chained to anything other than a projection layer of a container returns `XR_ERROR_VALIDATION_FAILURE`.

## Self rendering: clipping

- [ ] Discard pixels outside the 2D pixel bounds (the projection of the container volume onto each display).
- [ ] Do **not** discard pixels inside the 2D pixel bounds that are outside the 3D volume. Clipping to the 3D volume is up to the app, which reports it with `XrSpatialContainerLayerVolumeClippingEXT`.
- [ ] `volumeClipping` has no meaning for immersive containers.

## Conformance

- [ ] The spec says a separate, CTS-only test extension will let CTS control runtime policy. Implement it when targeting conformance.
