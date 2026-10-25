# Notes on adding Spatial Containers support to the SRD runtime

Personal notes based only on the public specification (OpenXR-Docs, `XR_EXT_spatial_container` rev. 1 and `XR_EXT_spatial_container_self_rendering`) and the Godot 4.8 implementation. Nothing here assumes anything about the internal implementation of the SRD.

## 1. Moving to OpenXR 1.1 is not a prerequisite

- In the registry (`xr.xml`), `XR_EXT_spatial_container` has no `depends` attribute, so it does not depend on a core version. `self_rendering` depends only on `XR_EXT_spatial_container`.
- In other words, **the extension can be implemented on a 1.0 runtime as is**. Moving to 1.1 and supporting spatial containers can be done as separate steps.

## 2. Minimal configuration: immersive only

The key points of the spec are:

- A runtime only needs to support **at least one** of bounded and immersive.
- A rendering runtime that supports `self_rendering` must set `supportsImmersive = true`.
- Immersive containers are treated "the same as a full-screen and immersive app created without spatial container usage".
- According to a note in the spec, the existing 1.x path is equivalent to "implicitly creating one immersive container and showing it".

From this, a first step could be the following configuration:

```text
supportsImmersive = true
supportsBounded   = false
maxSpatialContainerCount = 1
```

This approach reuses the current rendering path almost unchanged and only adapts the API surface to the new spec. Bounded support can be added afterwards.

## 3. Representing bounded containers on the SRD

- **Bounds size**: Match the bounding box to the volume in which the display can comfortably present stereoscopic content. `suggestedBounds` may be ignored (the spec says "may").
- **Origin and orientation**: The origin of the container space is the center of the bounding box. +X points to the right face, +Y to the top face, and +Z to the front face. The spec gives the example that a gravity-aligned container in front of the viewer has the identity orientation relative to LOCAL. When **the panel is tilted**, as on the SRD, which of the following the box is aligned to is a design decision:

  | Alignment | Pros | Cons |
  |---|---|---|
  | Gravity (level floor) | Matches the feel of placing objects | Does not use the full display volume |
  | Panel plane | Uses the full display volume | The "floor" appears tilted to apps |

- **Impact on Godot apps**: While bounded, Godot uses the container space as the play space, so content placed at the origin appears at the center of the container. Where the center is placed directly affects how apps look.
- **Bounds changes**: Bounds may change at any time while bounded. When they do, queue a `BoundsChanged` event (either a single event at the end, or events at frame rate).

## 4. Rendering (self_rendering)

- **Session state**: The session stays in IDLE and is treated as running. `xrBeginSession`, `xrEndSession`, `xrRequestExitSession`, and `xrLocateViews` return `XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT`. `shouldRender` from `xrWaitFrame` is always false.
- **Locating views**: Views are returned per container by `xrLocateSpatialContainerViewsEXT`.
  - **FOV**: The spec recommends "the smallest frustum that still contains the entire bounds of the container" and **explicitly allows asymmetric FOVs**. This should fit well with the SRD, which uses eye tracking to build oblique frustums through the display.
  - **`recommendedImageExtent`**: Resolution can be reduced to match how much of the screen the container occupies.
  - **`shouldSubmitLayers = false`**: Returned when no valid view can be produced on screen (for example, when the container is off screen). Whether to return this when eye tracking is lost, or to keep using the last pose, is a runtime policy decision.
  - **Decaying view configurations**: The spec allows decaying stereo to mono (the number of views stays the same, with the extra views filled with duplicates).
- **Submitting layers**: Layers are submitted per container by chaining `XrSpatialContainerLayerFrameEndInfoEXT` to `xrEndFrame`. A non-zero `layerCount` in the frame end info itself is an error.
- **Clipping outside the bounds**: The runtime must discard pixels outside the *2D pixel bounds* (the projection of the container volume onto the display), but must not discard pixels that are inside the 2D pixel bounds yet outside the *3D volume*. Clipping to the 3D volume is the app's choice. `XrSpatialContainerLayerVolumeClippingEXT` (NONE / STRICT) is a hint in which the app reports what it did. With STRICT, the runtime may, for example, draw the rear faces of the volume. Depth paradoxes cause eye strain, which matters on a stereoscopic desktop display, so deciding how the SRD presents NONE content is worth doing early.

## 5. Mapping visibility and focus to Windows

| Spec event | Candidate mapping |
|---|---|
| `VisibleChanged` | SRD output shown / hidden, minimized |
| `InteractableChanged` | Windows focus (keyboard and mouse input target) |
| `Closed` | The user closes the app. If the container was visible, queue `VisibleChanged(false)` first |
| `VisibleRequestDenied` | For example, refusing to show again after the user minimized the container |

- The runtime must not show a container that the app has never requested to show.
- When starting to show a container, the runtime may delay rendering until the first frame arrives, but should not delay for more than 0.5 seconds.

## 6. Future: multiple containers

Placing containers from multiple apps in the display volume of a single SRD requires a compositor that composites across processes. Starting with `maxSpatialContainerCount = 1` and showing only one container at a time is realistic. The spec also mentions that highly constrained devices may limit simultaneous display to one container.

## 7. Conformance testing (CTS)

The Issues section of the spec says that a separate, CTS-only test extension will be defined so that CTS can control behavior that depends on runtime policy. Check it when working on CTS support.

## See also

- [runtime-checklist.md](runtime-checklist.md): normative requirements of both extensions, as a checklist for runtime implementers.

## What this sample lets you check

- A `Label3D` shows the bounds size, mode, and visible / interactable state reported by the runtime.
- The B key toggles a wireframe of the bounds, so it can be compared with the actual display volume.
- The Space key requests a bounds mode switch, so both the success and denial flows can be exercised.
