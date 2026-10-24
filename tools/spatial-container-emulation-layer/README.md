# Spatial container emulation layer

An OpenXR API layer that provides `XR_EXT_spatial_container` and `XR_EXT_spatial_container_self_rendering` on top of a runtime that only implements the classic OpenXR session and frame loop (for example, an OpenXR 1.0 runtime).

Use it to run spatial container apps, such as [samples/container-viewer](../../samples/container-viewer), on runtimes that do not support the extensions yet, or to try out runtime design decisions before implementing them.

It is a testing tool, not a compositor: see [Limitations](#limitations).

## How it works

Toward the app, the layer presents the spatial container session model. Toward the runtime, it drives the classic model.

| App sees | Layer does with the runtime |
|---|---|
| Session stays in IDLE (the first IDLE is passed through) | Swallows READY / SYNCHRONIZED / VISIBLE / FOCUSED / STOPPING / EXITING |
| `xrBeginSession`, `xrEndSession`, `xrRequestExitSession`, `xrLocateViews` return `XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT` | Calls `xrBeginSession` itself on READY and `xrEndSession` on STOPPING |
| A container becomes visible once the app requested it and the runtime session is VISIBLE or FOCUSED | Session state VISIBLE / FOCUSED |
| Visible containers are interactable while the runtime session is FOCUSED | Session state FOCUSED |
| `Closed` for every container | Session state EXITING or LOSS_PENDING |
| `xrWaitFrame` keeps pacing frames and `shouldRender` is always false | Forwards `xrWaitFrame` while the runtime session runs. Otherwise it sleeps for one display period |
| `xrLocateSpatialContainerViewsEXT` | `xrLocateViews` in the space the app passes |
| Layers per container through `XrSpatialContainerLayerFrameEndInfoEXT` | One `xrEndFrame` with the layers of all visible containers, in render-set order |
| Container space | A LOCAL reference space offset to the configured container position |

The layer also validates what the spec requires of runtimes: duplicate, missing, or not-rendering containers in `xrEndFrame`, retain with layers, bounds mode values, closed containers, and the begin / end rendering call order. Structures from the two extensions are removed from next chains before anything reaches the runtime.

## Building

Requires CMake 3.16+ and a C++17 compiler. The OpenXR-SDK (release 1.1.65) is downloaded automatically, or you can point `OPENXR_SDK_SOURCE_DIR` at a local checkout.

```
cmake -S . -B build
cmake --build build --config Release
```

The build directory then contains the layer library and its manifest, `XrApiLayer_spatial_container_emulation.json`.

## Enabling the layer

Point the loader at the directory that contains the manifest and enable the layer:

```
# Linux
export XR_API_LAYER_PATH=/path/to/build
export XR_ENABLE_API_LAYERS=XR_APILAYER_spatial_container_emulation

# Windows (PowerShell)
$env:XR_API_LAYER_PATH = "C:\path\to\build\Release"
$env:XR_ENABLE_API_LAYERS = "XR_APILAYER_spatial_container_emulation"
```

Then start the app from the same shell. With the layer enabled this way, the loader lists the two extensions in `xrEnumerateInstanceExtensionProperties`, so apps (including Godot 4.8) detect and enable them as usual. The layer does nothing unless the app enables `XR_EXT_spatial_container`.

On Windows you can instead register the manifest under `HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Explicit`, and still enable it with `XR_ENABLE_API_LAYERS`.

## Configuration

All settings are environment variables.

| Variable | Default | Meaning |
|---|---|---|
| `XR_SCEMU_BOUNDS` | `0.5,0.5,0.5` | Bounds of bounded containers in meters (width, height, depth). Overrides the app's `suggestedBounds`. Without it, a non-zero `suggestedBounds` is used. |
| `XR_SCEMU_POSITION` | `0,0,-1` | Center of the container in the runtime's LOCAL space, in meters. The container is gravity aligned and faces +Z. |
| `XR_SCEMU_SUPPORTED_MODES` | `bounded,immersive` | Bounds modes reported as supported (`bounded`, `immersive`, or both). |
| `XR_SCEMU_MAX_CONTAINERS` | `16` | `maxSpatialContainerCount` |
| `XR_SCEMU_DENY_VISIBLE_REQUESTS` | off | Deny every show request, to test the app's handling of `VisibleRequestDenied`. |
| `XR_SCEMU_DENY_BOUNDS_MODE_REQUESTS` | off | Deny bounds mode requests for visible containers, to test `BoundsModeRequestDenied`. Requests for hidden containers are still honored, as the spec requires. |
| `XR_SCEMU_LOG` | off | Log state changes to stderr. |

For a desktop display such as the SRD, set `XR_SCEMU_POSITION` to the center of the display volume and `XR_SCEMU_BOUNDS` to its size in the runtime's LOCAL space.

## Limitations

- **Graphics binding required**: The spec lets spatial container sessions omit the graphics binding, but the layer passes `xrCreateSession` through, so the runtime needs one.
- **No clipping and no tight FOV**: Views are the runtime's full views, and content outside a bounded container is not discarded. The spec requires runtimes to discard pixels outside the 2D pixel bounds.
- **Fixed placement**: The container is placed at `XR_SCEMU_POSITION` in both bounded and immersive modes, and never moves.
- **One view configuration**: Only the runtime's first view configuration is supported, and it is never decayed.
- **Retain is approximate**: Retained layers are resubmitted as structures, so later writes to their swapchains can show up. The spec says they must not. Next chains of retained layers are dropped.
- **Layer types**: Only projection and quad layers are forwarded. Others are dropped (logged with `XR_SCEMU_LOG`).
- **Input**: `xrSyncActions` is passed through, and every visible container is interactable while the runtime session is focused.
- **Single instance**: Space location filtering assumes one `XrInstance` per process.
- **Native support**: Do not enable the layer on a runtime that implements the extensions itself.
- **Windows**: The Windows code paths compile in principle but have not been built or run yet. The layer has only been tested on Linux, against the mock runtime below.

## Tests

`tests/` contains a minimal headless mock runtime and an end-to-end test that goes through the real OpenXR loader: app → loader → layer → mock runtime. The mock runtime rejects any spatial container structure that reaches it, and records what `xrEndFrame` received. The tests cover:

- Extension listing, system properties, and presentation enumeration
- Session translation, including stopping and restarting the runtime session and session loss
- Visibility, interactable, and bounds events, including their order
- View location, frame submission (normal, retain, and clear), and `xrEndFrame` validation
- Container space locatability
- The deny switches

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests are set up for Linux only.
