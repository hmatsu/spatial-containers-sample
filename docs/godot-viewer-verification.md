# Verifying the Godot viewer on Windows

Step-by-step checks for [samples/container-viewer](../samples/container-viewer) on Windows, for example with a Spatial Reality Display. The checks go in three stages, each adding one moving part:

| Stage | Runtime | Spatial container code path | What it verifies |
|---|---|---|---|
| A | Existing runtime as is | Viewer's own emulation | Rendering, input, placement on the display |
| B | Existing runtime + [emulation layer](../tools/spatial-container-emulation-layer) | Real extension path in Godot | Godot's container lifecycle, events, mode switches |
| C | Runtime with native support | Real extension path | The runtime implementation |

Stage C is the goal. A and B separate app problems from runtime problems before you get there.

## Before you start

- **Godot**: Godot 4.8 (4.8-dev7 or later) for Windows. Use the `_console.exe` variant, so that the log is visible.
- **Build tools** (stage B only): Visual Studio 2022 with the C++ workload, and CMake 3.16 or later.
- **OpenXR runtime**: The runtime to test, set as the active OpenXR runtime.
- **Graphics API**: Godot does not support Direct3D 11 for OpenXR. Check which graphics extension the runtime offers (step A2 shows it) and configure Godot to match:

  | Runtime offers | Godot setting |
  |---|---|
  | `XR_KHR_vulkan_enable2` or `XR_KHR_vulkan_enable` | Default (Forward+ renderer, Vulkan) |
  | `XR_KHR_D3D12_enable` | Project Settings → Rendering → Rendering Device → Driver (Windows): `d3d12` |
  | `XR_KHR_opengl_enable` | Project Settings → Rendering → Renderer → Rendering Method: `gl_compatibility` |

  If the runtime only offers `XR_KHR_D3D11_enable`, Godot cannot use it. Use [samples/cpp-minimal](../samples/cpp-minimal) (Direct3D 11) instead, and the stage B and C checks still apply.
- **Form factor**: The viewer asks for a head-mounted display with a stereo view configuration (Godot's defaults). If the runtime reports something else, change Project Settings → XR → OpenXR → Form Factor / View Configuration.

## Stage A: viewer emulation on the existing runtime

A1. Open a terminal in `samples/container-viewer` and start the viewer:

```bat
Godot_v4.8-dev7_win64_console.exe --path .
```

A2. Check the log:

- `OpenXR: Running on OpenXR runtime: ...` names the runtime under test.
- `OpenXR: Found OpenXR extension ...` lines list everything the runtime offers. Note the graphics extension (see the table above). `XR_EXT_spatial_container` should be absent at this stage.
- `Spatial container extension not available, emulating a bounded container.` confirms the emulation path.

A3. On the display:

- The status label reads `ext: emulated`, `mode: bounded`, and the bounds from project.godot.
- The built-in model spins inside a cyan wireframe box.

If nothing is visible, the content is probably outside the display volume. Move it with J / L, U / O, I / K (1 cm per press), and watch the log for `Offset: --offset=x,y,z`. When the box sits where you want it, note the printed value.

A4. Interaction:

- Space toggles bounded and immersive. Immersive shows the model at real scale, without the box.
- The arrow keys rotate the model, and B toggles the box.
- Drag a `.glb` file onto the Godot window to replace the model.

A5. Restart with the offset you found, to confirm it is applied:

```bat
Godot_v4.8-dev7_win64_console.exe --path . -- --offset=0,-0.1,-0.5
```

Pass criteria: the model is visible and stays inside the box at any rotation, both modes work, and the label is readable.

## Stage B: the real extension path through the emulation layer

B1. Build the layer (once):

```bat
cd tools\spatial-container-emulation-layer
cmake -S . -B build -DSCEMU_BUILD_TESTS=OFF
cmake --build build --config Release
```

B2. In the terminal that will start Godot, enable the layer. Configure the container from the offset found in stage A3, and size the bounds to your display volume:

```bat
set XR_API_LAYER_PATH=%CD%\build\Release
set XR_ENABLE_API_LAYERS=XR_APILAYER_spatial_container_emulation
set XR_SCEMU_POSITION=0,-0.1,-0.5
set XR_SCEMU_BOUNDS=0.3,0.2,0.2
set XR_SCEMU_LOG=1
```

(In PowerShell, use `$env:NAME = "value"` instead of `set`.)

`XR_SCEMU_POSITION` is in the runtime's LOCAL space. The viewer uses Local as its play space, so the offset from stage A carries over.

B3. Start the viewer from the same terminal (`cd ..\..\samples\container-viewer` first) and check the log:

- `[XR_APILAYER_spatial_container_emulation] instance created: spatial_container=1 self_rendering=1 ...` confirms that the layer is loaded and that Godot enabled both extensions.
- `OpenXR: Enabled extension XR_EXT_spatial_container` and `... XR_EXT_spatial_container_self_rendering` come from Godot.
- `OpenXR: Max spatial container count: 16` means Godot is creating its container.
- The layer log shows `xrBeginSession on runtime: 0`, then `container ... visible=1` and `interactable=1`.

If the layer is not mentioned at all, check `XR_API_LAYER_PATH`. It must be the directory that contains `XrApiLayer_spatial_container_emulation.json`.

B4. On the display:

- The status label reads `ext: active`, `mode: bounded`, `bounds: 0.30 x 0.20 x 0.20 m` (your `XR_SCEMU_BOUNDS`), and `visible: true  interactable: true`.
- The box is at `XR_SCEMU_POSITION`. The viewer's own offset does not apply in bounded mode, because Godot uses the container space.

B5. Mode switches:

- Space requests immersive. The log shows `Bounds changed: mode=1 infinite=true ...`, and the label shows `mode: immersive`.
- Space again returns to bounded, with the same bounds as before.

B6. Focus and input:

- Make the runtime session lose focus. How depends on the runtime; for a desktop runtime, try clicking another window. The label shows `interactable: false`, and the keys stop acting.
- Return focus, and both come back. If the runtime never reports a focus change, this check does not apply.

B7. Denial handling. Restart with one of these set, then press Space:

```bat
set XR_SCEMU_DENY_BOUNDS_MODE_REQUESTS=1
```

The log shows `Bounds mode request denied by the runtime.`, and the mode does not change. Unset it afterwards (`set XR_SCEMU_DENY_BOUNDS_MODE_REQUESTS=`).

B8. Close the viewer window. The layer log shows `container ... visible=0` before shutdown, and nothing should be reported as an error.

Pass criteria: B3 to B8 behave as described. In particular, the content appears only after `visible=1`, the bounds and mode on the label follow the layer, and input stops while the container is not interactable.

## Stage C: a runtime with native support

C1. Unset `XR_ENABLE_API_LAYERS`, so the layer is not used.

C2. Repeat stage B from B3. These items now come from the runtime:

- `XR_EXT_spatial_container` and `XR_EXT_spatial_container_self_rendering` in the `Found OpenXR extension` lines.
- The bounds, placement, and focus behavior.
- Whether denials happen, according to the runtime's policy.

C3. Compare with stage B. Differences point at runtime behavior to check against [runtime-checklist.md](runtime-checklist.md).

Things worth checking on a stereoscopic desktop display in particular:

- Whether the reported bounds match the comfortable display volume, and where the container center is.
- Whether the box is aligned to gravity or to the panel. With a tilted panel, a gravity-aligned box looks level, and a panel-aligned one is tilted.
- Whether content is kept within the container's 2D pixel bounds.
- What happens to `interactable` and `visible` when eye tracking is lost, the app window loses focus, or the display is minimized.

## Reporting results

For each stage, these make a failure easy to analyze:

- The full console log (with `XR_SCEMU_LOG=1` in stage B).
- A photo of the display showing the status label.
- The Godot version, the runtime name and version (from the log), and the project settings changed from the repository.
