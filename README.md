# spatial-containers-sample

Samples and research notes for experimenting with OpenXR Spatial Containers (`XR_EXT_spatial_container` / `XR_EXT_spatial_container_self_rendering`). This repository is maintained as a personal project.

## Contents

| Path | Description |
|---|---|
| [samples/container-viewer](samples/container-viewer) | 3D model viewer built with Godot 4.8 (switches between bounded and immersive, with emulation when the extension is unavailable) |
| [samples/cpp-minimal](samples/cpp-minimal) | Minimal C++ app using only the OpenXR SDK, with a Direct3D 11 backend and a headless mode for tests |
| [tools/spatial-container-emulation-layer](tools/spatial-container-emulation-layer) | OpenXR API layer that emulates both extensions on runtimes without them, with a mock runtime and end-to-end tests |
| [docs/srd-runtime-notes.md](docs/srd-runtime-notes.md) | Notes on adding Spatial Containers support to a Spatial Reality Display runtime |
| [docs/runtime-checklist.md](docs/runtime-checklist.md) | Runtime requirements of both extensions as a checklist |

## References

- Specification source: [KhronosGroup/OpenXR-Docs](https://github.com/KhronosGroup/OpenXR-Docs) (`specification/sources/chapters/extensions/ext/ext_spatial_container*.adoc`)
- Godot implementation: [godotengine/godot](https://github.com/godotengine/godot) (`modules/openxr/extensions/spatial_container/`)
- Official Godot sample: [GodotVR/godot_openxr_vendors](https://github.com/GodotVR/godot_openxr_vendors) (`samples/spatial-container-sample`)
- [Khronos Blog: OpenXR Spatial Containers](https://www.khronos.org/blog/openxr-spatial-containers)

## License

[MIT License](LICENSE)

## Checks

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) builds and tests the C++ code on Linux (also with ASan and UBSan) and with MSVC on Windows, checks and unit-tests the Godot viewer's scripts, and runs the linters.

| Tests | Where | Runs in CI on |
|---|---|---|
| Emulation layer unit tests (doctest) | [tools/spatial-container-emulation-layer/tests/unit_tests.cpp](tools/spatial-container-emulation-layer/tests/unit_tests.cpp) | Linux, Windows |
| C++ sample math unit tests (doctest) | [samples/cpp-minimal/tests/math_tests.cpp](samples/cpp-minimal/tests/math_tests.cpp) | Linux, Windows |
| Godot viewer unit tests | [samples/container-viewer/tests/run_tests.gd](samples/container-viewer/tests/run_tests.gd) | Linux (Godot headless) |
| Emulation layer end-to-end tests (loader + mock runtime) | [tools/spatial-container-emulation-layer/tests/layer_test.cpp](tools/spatial-container-emulation-layer/tests/layer_test.cpp) | Linux |
| C++ sample lifecycle test (headless, through the layer) | [samples/cpp-minimal/CMakeLists.txt](samples/cpp-minimal/CMakeLists.txt) | Linux |

To run the linters locally:

```sh
scripts/lint.sh        # check
scripts/lint.sh --fix  # apply formatting, then check
```

| Tool | Files | Configuration |
|---|---|---|
| clang-format 18 | C++ | [.clang-format](.clang-format) |
| clang-tidy 18 | C++ (static analysis) | [.clang-tidy](.clang-tidy) |
| gdformat, gdlint (gdtoolkit 4.5) | GDScript | [gdlintrc](gdlintrc) |
| markdownlint-cli2 | Markdown | [.markdownlint-cli2.yaml](.markdownlint-cli2.yaml) |
| actionlint, shellcheck | Workflows, shell scripts | |
| editorconfig-checker | All tracked files | [.editorconfig](.editorconfig) |
