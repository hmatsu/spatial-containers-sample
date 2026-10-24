# spatial-containers-sample

Samples and research notes for experimenting with OpenXR Spatial Containers (`XR_EXT_spatial_container` / `XR_EXT_spatial_container_self_rendering`). This repository is maintained as a personal project.

## Contents

| Path | Description |
|---|---|
| [samples/container-viewer](samples/container-viewer) | 3D model viewer built with Godot 4.8 (switches between bounded and immersive, with emulation when the extension is unavailable) |
| [docs/srd-runtime-notes.md](docs/srd-runtime-notes.md) | Notes on adding Spatial Containers support to a Spatial Reality Display runtime |
| [docs/runtime-checklist.md](docs/runtime-checklist.md) | Runtime requirements of both extensions as a checklist |

## References

- Specification source: [KhronosGroup/OpenXR-Docs](https://github.com/KhronosGroup/OpenXR-Docs) (`specification/sources/chapters/extensions/ext/ext_spatial_container*.adoc`)
- Godot implementation: [godotengine/godot](https://github.com/godotengine/godot) (`modules/openxr/extensions/spatial_container/`)
- Official Godot sample: [GodotVR/godot_openxr_vendors](https://github.com/GodotVR/godot_openxr_vendors) (`samples/spatial-container-sample`)
- [Khronos Blog: OpenXR Spatial Containers](https://www.khronos.org/blog/openxr-spatial-containers)

## License

[MIT License](LICENSE)
