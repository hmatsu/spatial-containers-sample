# End-to-end tests

[run_e2e.sh](run_e2e.sh) runs the samples on a real OpenXR runtime without a GPU:

- **Runtime**: [Monado](https://monado.freedesktop.org/), with its null compositor and a simulated HMD.
- **Vulkan**: Mesa's software Vulkan driver (lavapipe).
- **Display**: Xvfb.

The apps render every frame for real; the null compositor just does not show them.

| Scenario | What runs | Checks |
|---|---|---|
| `emulation` | The viewer on the plain runtime | Runs on Monado, uses the viewer's emulation, and the session reaches FOCUSED |
| `layer` | The viewer through the [emulation layer](../../tools/spatial-container-emulation-layer), with `--toggle-every=1` | Both extensions are enabled, the container is shown and interactable, the mode switches both ways with the right bounds, frames with container layers reach the runtime, and every frame ends |
| `widget` | The [native widget](../../samples/native-widget) through the layer, rendering with Vulkan, for 4 seconds | Shown and interactable, the panel is uploaded on changes and retained in between, and frames with container layers reach the runtime |
| `coexistence` | The viewer (left) and the widget (right) at the same time, the widget's runtime session as an overlay | The viewer is shown before the widget starts and is never hidden while the widget runs; the widget passes the same checks as above; both apps' frames reach the runtime |

The viewer scenarios also check for script errors, and the widget scenarios for failed OpenXR calls.

## Why the widget is an overlay

Monado is a 1.x runtime: it shows one regular app at a time, and switches to the newest. Without the overlay, starting the widget hides the viewer (its session goes from VISIBLE to SYNCHRONIZED), and the viewer comes back when the widget exits. With `XR_SCEMU_OVERLAY_PLACEMENT`, the layer creates the widget's runtime session as an `XR_EXTX_overlay` session, so Monado composites it on top of the viewer and both stay visible and interactable. A runtime with native spatial container support would place both containers itself.

## Running

On Ubuntu 24.04:

```sh
sudo apt-get install monado-service libopenxr1-monado mesa-vulkan-drivers xvfb xauth
cmake -S tools/spatial-container-emulation-layer -B build-layer -DSCEMU_BUILD_TESTS=OFF
cmake --build build-layer
cmake -S samples/native-widget -B build-widget -DWIDGET_BUILD_TESTS=OFF  # needs libvulkan-dev
cmake --build build-widget
tests/e2e/run_e2e.sh path/to/godot build-layer build-widget/native_widget
```

Leave out the widget to run only the viewer scenarios. Set `E2E_LOG_DIR` to keep the logs in a known place. If `TMPDIR`-style paths are long in your environment, set `E2E_TMP` to a short directory: Monado's IPC socket path must fit in 108 bytes.

## Godot version

The `layer` scenario needs a Godot build that includes the fix for frames that Godot skips while a spatial container is active. Godot 4.8-dev7 calls `xrEndFrame` without `XrSpatialContainerLayerFrameEndInfoEXT` on those frames. The spec requires the runtime to reject that with `XR_ERROR_SPATIAL_CONTAINER_MISSING_EXT`, so on dev7 no frame ever ends, and the runtime never makes the session visible. Godot master has the fix. The [E2E workflow](../../.github/workflows/e2e.yml) builds Godot from a pinned master commit (`GODOT_E2E_COMMIT`) and caches the build, until a release with the fix is available. See the repository README for when the workflow runs.
