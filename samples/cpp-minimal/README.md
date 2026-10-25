# C++ minimal sample

A minimal spatial container app that uses only the OpenXR SDK. It is meant as a reference for the call sequence of `XR_EXT_spatial_container` and `XR_EXT_spatial_container_self_rendering`, without an engine in between.

It draws a spinning cube inside a wireframe of the container bounds, and switches between bounded and immersive modes on a timer. In immersive mode the cube is 30 cm and the wireframe is hidden.

## What it shows

All of the OpenXR logic is in [src/main.cpp](src/main.cpp):

1. **Instance**: Check that both extensions are available, then create the instance. `XrSystemSpatialContainerPropertiesEXT` tells which bounds modes are supported.
2. **Session**: Create the session with `XrSessionCreateInfoSpatialContainersEXT` chained. The session is running once it reports IDLE. `xrBeginSession` is never called.
3. **Container**: Create the container and its space, then request it to be shown. A new container is hidden and bounded. If the system does not support bounded, the sample requests immersive before showing it.
4. **On `VisibleChanged(true)`**: Begin rendering (`xrBeginSpatialContainerRenderingEXT`). On `VisibleChanged(false)`, end rendering.
5. **Each frame**:
   - Locate the views with `xrLocateSpatialContainerViewsEXT`.
   - If `shouldSubmitLayers` is true, render into `recommendedImageExtent` with a transparent background. Submit a projection layer with `XrSpatialContainerCompositionLayerViewConfigurationEXT` and a volume clipping hint, through `XrSpatialContainerLayerFrameEndInfoEXT`.
   - Otherwise, retain the previous submission.
   - Every container in the active render set is listed every frame, even without layers.
6. **Bounds mode**: Requests are asynchronous. The sample only changes its scene when `BoundsChanged` arrives.
7. **Shutdown**: Request hidden, wait for `VisibleChanged(false)`, end rendering, then destroy the container. On `Closed`, the sample tears down right away.

The graphics side is behind a small interface ([src/graphics.h](src/graphics.h)):

- **Direct3D 11** ([src/graphics_d3d11.cpp](src/graphics_d3d11.cpp)), on Windows.
- **Headless** ([src/graphics_headless.cpp](src/graphics_headless.cpp)), which renders nothing and submits no layers. It exists to test the lifecycle without a GPU.

## Building

Requires CMake 3.16+ and a C++17 compiler (Visual Studio 2019 or later on Windows). On Linux, the OpenXR-SDK also needs the X11 development headers (`libx11-dev` on Debian and Ubuntu). The OpenXR-SDK (release 1.1.65) is downloaded and its loader built automatically, or you can point `OPENXR_SDK_SOURCE_DIR` at a local checkout.

```
cmake -S . -B build
cmake --build build --config Release
```

## Running

```
cpp_minimal [--headless] [--seconds N] [--toggle-seconds N]
```

| Option | Default | Meaning |
|---|---|---|
| `--headless` | off (always on outside Windows) | No graphics binding and no rendering |
| `--seconds N` | run until closed | Request hidden after N seconds and exit cleanly |
| `--toggle-seconds N` | 8 | Switch bounds mode every N seconds. 0 disables switching |

On a runtime without spatial container support, enable the [emulation layer](../../tools/spatial-container-emulation-layer) first. For a desktop display such as the SRD, also set `XR_SCEMU_POSITION` and `XR_SCEMU_BOUNDS` to match the display volume.

The sample uses `XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY`. If your runtime reports a different form factor, change it in `CreateInstance()`.

## Tests

On Linux, `ctest` runs the sample headless through the emulation layer and its mock runtime. The test passes when the log shows the full lifecycle in order: shown, both mode switches, hidden on request, and a clean shutdown. The layer's own tests run as well.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Verification status

- **Linux**: The headless path runs end to end through the emulation layer and the mock runtime.
- **Windows**: The Direct3D 11 path cross-compiles and links for Windows with MinGW-w64 without warnings, but it has not been run yet. Rendering has not been seen on a real display. The view and projection math was checked numerically.
- **MSVC**: The MSVC build has not been tried.
