# Native widget

A resident widget in a bounded spatial container, written in C++ with only the OpenXR SDK. It shows a clock and a pomodoro timer on a flat panel, and is meant to sit next to other apps, such as the [Godot viewer](../container-viewer).

```text
╭──────────────────────────────╮
│           14 : 05            │   clock (local time)
│                              │
│  FOCUS               24:13   │   phase and countdown
│  PAUSED                      │   (only while paused)
│  ███████████░░░░░░░░░░░░░░   │   progress of the phase
╰──────────────────────────────╯
```

## What it shows

Where [samples/cpp-minimal](../cpp-minimal) renders 3D views every frame, this sample shows the other side of self rendering:

- **A quad layer, not a projection layer**: The panel is drawn on the CPU into an image and placed as an `XrCompositionLayerQuad` at the container's center, facing the user. There are no views to render. It locates views only to learn whether the container is on screen (`shouldSubmitLayers`).
- **Updates only when something changes**: The image changes at most once a second, when the countdown, the clock or the progress bar's end pixel changes. Only then is it uploaded and submitted. Every other frame sets `retainPreviousSubmission`, so the runtime keeps showing the last panel. In a test on Monado, 5 of 83 frames uploaded the panel and 77 were retained.
- **Bounded by design**: The container asks for a thin box the size of the panel (0.48 × 0.288 × 0.02 m, 1 mm per pixel). The panel is fitted into whatever bounds the runtime gives, keeping its aspect ratio, and follows `BoundsChanged`. It requests immersive mode only if the system cannot do bounded containers. Corners are transparent (`BLEND_TEXTURE_SOURCE_ALPHA`), and the layer reports strict volume clipping, since the quad lies inside the box.
- **Input only while interactable**: Select on a controller (`khr/simple_controller`) starts or pauses the timer, and menu resets it. Actions are synced only while the runtime reports the container as interactable.
- **The lifecycle**: Show on request, begin rendering on `VisibleChanged(true)`, end rendering when hidden, request hidden and wait for it before destroying, and tear down on `Closed`.

| File | Contents |
|---|---|
| [src/main.cpp](src/main.cpp) | The OpenXR logic |
| [src/pomodoro.h](src/pomodoro.h) | The timer: focus and break phases, independent of any clock |
| [src/panel.h](src/panel.h), [src/panel.cpp](src/panel.cpp) | Drawing the panel: seven-segment digits, a 5×7 font, rounded rectangles |
| [src/graphics_vulkan.cpp](src/graphics_vulkan.cpp) | Vulkan backend (`XR_KHR_vulkan_enable2`): copies the image through a staging buffer |
| [src/graphics_d3d11.cpp](src/graphics_d3d11.cpp) | Direct3D 11 backend (`XR_KHR_D3D11_enable`), Windows only |
| [src/graphics_common.cpp](src/graphics_common.cpp) | Headless backend, swapchain format choice, RGBA/BGRA copy |

## Building

Requires CMake 3.16+ and a C++17 compiler. On Linux, also the X11 headers (needed by the OpenXR-SDK) and the Vulkan headers and loader: `libx11-dev` and `libvulkan-dev` on Debian and Ubuntu. Without Vulkan the widget builds with only the headless backend on Linux, and with Direct3D 11 on Windows. The OpenXR-SDK is downloaded automatically, or you can point `OPENXR_SDK_SOURCE_DIR` at a local checkout.

```sh
cmake -S . -B build
cmake --build build --config Release
```

## Running

```text
native_widget [--headless] [--graphics vulkan|d3d11] [--seconds N]
              [--focus-minutes M] [--break-minutes M] [--start]
```

| Option | Default | Meaning |
|---|---|---|
| `--graphics` | `d3d11` on Windows, `vulkan` elsewhere | Graphics backend. Use the one the runtime supports |
| `--headless` | off | No graphics binding and no panel: for tests |
| `--seconds N` | run until closed | Request hidden after N seconds and exit cleanly |
| `--focus-minutes M`, `--break-minutes M` | 25, 5 | Phase lengths |
| `--start` | paused | Start the timer right away (also useful without a controller) |

On a runtime without spatial container support, enable the [emulation layer](../../tools/spatial-container-emulation-layer). To place the widget next to another app there, give each process its own `XR_SCEMU_POSITION`. A 1.x runtime usually shows only one app at a time; if it has `XR_EXTX_overlay` (Monado does), also set `XR_SCEMU_OVERLAY_PLACEMENT=1` for the widget, so it is shown on top of the other app instead of replacing it. The [end-to-end tests](../../tests/e2e) run it this way next to the Godot viewer.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

- **`native_widget_unit`** (doctest, every platform): the timer (pausing, rounding, phase changes, long steps, reset, degenerate lengths), premultiplied colors, clipping, rounded corners, every seven-segment digit, the font, the panel layout and colors, change detection, fitting, the swapchain format choice, and the BGRA copy.
- **`native_widget_headless`** (Linux): runs the widget through the emulation layer and its mock runtime for 2.5 seconds with the timer running. The log must show the container shown, the countdown changing, the hide request, the container hidden, and a clean shutdown.
- The emulation layer's own tests also run.
