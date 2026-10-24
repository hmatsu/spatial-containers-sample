# spatial-containers-sample

OpenXR の Spatial Containers（`XR_EXT_spatial_container` / `XR_EXT_spatial_container_self_rendering`）を試すための、サンプルと調査メモを置くリポジトリです。個人の活動として運営しています。

## 中身

| パス | 内容 |
|---|---|
| [samples/container-viewer](samples/container-viewer) | Godot 4.8 製の 3D モデルビューア（bounded ⇔ immersive の切り替え、拡張がないときのエミュレーション付き） |
| [docs/srd-runtime-notes.md](docs/srd-runtime-notes.md) | Spatial Reality Display のランタイムを Spatial Containers に対応させるための検討メモ |

## 参考資料

- 仕様のソース：[KhronosGroup/OpenXR-Docs](https://github.com/KhronosGroup/OpenXR-Docs)（`specification/sources/chapters/extensions/ext/ext_spatial_container*.adoc`）
- Godot の実装：[godotengine/godot](https://github.com/godotengine/godot)（`modules/openxr/extensions/spatial_container/`）
- Godot の公式サンプル：[GodotVR/godot_openxr_vendors](https://github.com/GodotVR/godot_openxr_vendors)（`samples/spatial-container-sample`）
- [Khronos Blog: OpenXR Spatial Containers](https://www.khronos.org/blog/openxr-spatial-containers)

## ライセンス

未定です（オーナーが決めます）。
