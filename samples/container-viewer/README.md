# SRD Container Viewer

A 3D model viewer sample for `XR_EXT_spatial_container`, built with Godot.

| Mode | Behavior |
|---|---|
| bounded | Scales the model to fit the container bounds reported by the runtime and spins it slowly. The container bounds are drawn as a wireframe box. |
| immersive | Shows the model at real scale (1 unit = 1 m), 1 m in front of the user. |

To exercise the real extension code path on such runtimes, run the viewer with the [emulation layer](../../tools/spatial-container-emulation-layer) enabled.

On runtimes without the extension (such as existing OpenXR 1.0 runtimes), the viewer runs in **emulation mode**, using the bounds from the project setting `xr/openxr/extensions/spatial_container/bounds` as a virtual container.

## Requirements

- Godot **4.8 or later** (`OpenXRSpatialContainerExtension` confirmed in 4.8-dev7)
- An OpenXR runtime (the SRD runtime when using an SRD on Windows)

## Running

```sh
godot --path . -- --model=C:/path/to/model.glb
```

- Without `--model=`, a simple built-in model is shown.
- `--offset=x,y,z` sets where the content is placed (meters, in the play space) in immersive mode and in emulation mode. The default is `0,0,-1`. On a desktop display such as the SRD, the default is likely outside the display volume, so adjust it with the keys below and pass the printed value next time.
- `--bounds=x,y,z` sets the size of the virtual container in emulation mode (meters). It has no effect when the runtime supports the extension.
- Drag and drop a `.glb` / `.gltf` file onto the Godot window while running to replace the model.

## Controls

| Key / button | Action |
|---|---|
| Space / Enter, controller A/X or trigger | Request a switch between bounded and immersive |
| Arrow keys, thumbstick | Rotate the model (stops auto-spin) |
| `+` / `-` | Zoom in / out (immersive) |
| R | Reset the view |
| B | Toggle the bounds box |
| J / L, U / O, I / K | Move the content 1 cm left / right, down / up, forward / back (immersive and emulation). The new value is printed as `--offset=...` |

While the extension is active, input is accepted only when the runtime reports the container as interactable, as the spec leaves input routing to the runtime. In emulation mode input is always accepted.

## Implementation notes

- **Startup**: Godot automatically creates one main container when the session is created, using `bounds_mode` and `bounds` from project.godot.
- **Coordinate system**: While bounded, Godot uses the container space as the play space, so the `XROrigin3D` origin is the container center. In immersive mode Godot switches back to the regular play space, so the content is moved to `immersive_offset`.
- **Fitting**: The diameter of the model's bounding sphere is matched to the shortest side of the container times `fill_ratio`. The model never leaves the box at any rotation, at the cost of appearing somewhat smaller.
- **Asynchronous requests**: Bounds mode requests are asynchronous. The result arrives as the `spatial_container_bounds_changed` signal (success) or `spatial_container_bounds_mode_request_denied` (denied), and the layout is updated only after the signal arrives.
- **While hidden**: `_process` is stopped while the container is not visible. The spec also expects apps to stop CPU and GPU work while hidden.
- **Input routing**: With the extension, keyboard and controller input is ignored unless the container is interactable.
- **Status display**: A `Label3D` shows the extension state (active or emulated), the mode, the bounds size, visible / interactable, and the content offset when it applies. This is useful for visually checking a runtime while debugging it.

## Tests

[tests/run_tests.gd](tests/run_tests.gd) unit-tests `main.gd` without an XR runtime. It covers argument parsing, fitting at any rotation, scale, centering, placement with and without the extension, the bounds box, glTF loading, bounds events, input gating, offset nudging, pausing while hidden, and the status label.

```sh
godot --headless --path . --import
godot --headless --path . -s res://tests/run_tests.gd
```

The exit code is the number of failed tests.

## Verifying on Windows

See [docs/godot-viewer-verification.md](../../docs/godot-viewer-verification.md) for step-by-step checks: first with the viewer's own emulation, then through the emulation layer, then on a runtime with native support.

## Notes for the SRD runtime

See [docs/srd-runtime-notes.md](../../docs/srd-runtime-notes.md).
