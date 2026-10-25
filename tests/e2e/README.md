# End-to-end tests

[run_viewer_e2e.sh](run_viewer_e2e.sh) runs the Godot viewer on a real OpenXR runtime without a GPU:

- **Runtime**: [Monado](https://monado.freedesktop.org/), with its null compositor and a simulated HMD.
- **Vulkan**: Mesa's software Vulkan driver (lavapipe).
- **Display**: Xvfb.

The viewer renders every frame for real; the null compositor just does not show them.

| Scenario | What runs | Checks |
|---|---|---|
| `emulation` | The viewer on the plain runtime | Runs on Monado, uses the viewer's emulation, and the session reaches FOCUSED |
| `layer` | The viewer through the [emulation layer](../../tools/spatial-container-emulation-layer), with `--toggle-every=1` | Both extensions are enabled, the container is shown and interactable, the mode switches both ways with the right bounds, frames with container layers reach the runtime, and every frame ends |

Both scenarios also check for script errors.

## Running

On Ubuntu 24.04:

```sh
sudo apt-get install monado-service libopenxr1-monado mesa-vulkan-drivers xvfb xauth
cmake -S tools/spatial-container-emulation-layer -B build-layer -DSCEMU_BUILD_TESTS=OFF
cmake --build build-layer
tests/e2e/run_viewer_e2e.sh path/to/godot build-layer
```

Set `E2E_LOG_DIR` to keep the logs in a known place. If `TMPDIR`-style paths are long in your environment, set `E2E_TMP` to a short directory: Monado's IPC socket path must fit in 108 bytes.

## Godot version

The `layer` scenario needs a Godot build that includes the fix for frames that Godot skips while a spatial container is active. Godot 4.8-dev7 calls `xrEndFrame` without `XrSpatialContainerLayerFrameEndInfoEXT` on those frames. The spec requires the runtime to reject that with `XR_ERROR_SPATIAL_CONTAINER_MISSING_EXT`, so on dev7 no frame ever ends, and the runtime never makes the session visible. Godot master has the fix. The [E2E workflow](../../.github/workflows/e2e.yml) builds Godot from a pinned master commit (`GODOT_E2E_COMMIT`) and caches the build, until a release with the fix is available. See the repository README for when the workflow runs.
