# SRD Container Viewer

`XR_EXT_spatial_container` を使った Godot 製の 3D モデルビューアのサンプルです。

| モード | 表示 |
|---|---|
| bounded | ランタイムが返すコンテナの大きさに合わせてモデルを自動縮小し、ゆっくり回転させる。コンテナの境界をワイヤーフレームで表示する |
| immersive | モデルを実寸（1 unit = 1 m）で、ユーザーの 1 m 前に表示する |

拡張に対応していないランタイム（OpenXR 1.0 の既存ランタイムなど）では、プロジェクト設定の境界（`xr/openxr/extensions/spatial_container/bounds`）を仮想コンテナとして使う**エミュレーションモード**で動きます。

## 必要なもの

- Godot **4.8 以降**（`OpenXRSpatialContainerExtension` は 4.8-dev7 で確認済み）
- OpenXR ランタイム（Windows で SRD を使う場合は SRD のランタイム）

## 実行

```
godot --path . -- --model=C:/path/to/model.glb
```

- `--model=` を省略すると、組み込みの簡単なモデルを表示します。
- 実行中に `.glb` / `.gltf` を Godot のウィンドウにドラッグ＆ドロップすると、そのモデルに差し替えます。

## 操作

| キー / ボタン | 動作 |
|---|---|
| Space / Enter、コントローラの A/X またはトリガー | bounded ⇔ immersive の切り替えを要求 |
| 矢印キー、スティック | モデルを回転（触ると自動回転が止まる） |
| `+` / `-` | 拡大・縮小（immersive 時） |
| R | 表示をリセット |
| B | 境界ボックスの表示を切り替え |

## 実装のポイント

- **起動の流れ**：Godot はセッション作成時にメインのコンテナを1つ自動で作ります。そのときの設定は project.godot の `bounds_mode` と `bounds` です。
- **座標系**：bounded の間は、Godot がコンテナの空間をプレイスペースとして使います。そのため `XROrigin3D` の原点がコンテナの中心になります。immersive になると通常のプレイスペースに戻るので、コンテンツを `immersive_offset` の位置に置き直しています。
- **フィットの計算**：モデルを囲む球の直径を、コンテナの一番短い辺 × `fill_ratio` に合わせます。どの向きに回しても箱からはみ出しませんが、そのぶん小さめに表示されます。
- **非同期の要求**：モード切り替えの要求は非同期です。結果は `spatial_container_bounds_changed`（成功）か `spatial_container_bounds_mode_request_denied`（拒否）のシグナルで届きます。表示の切り替えは、このシグナルを受けてから行います。
- **非表示中**：コンテナが見えていない間は `_process` を止めます。仕様でも、非表示中は CPU と GPU の負荷を止めることが期待されています。
- **状態の表示**：`Label3D` に、拡張の状態（有効かエミュレーションか）、モード、境界の大きさ、visible / interactable を表示します。ランタイム側をデバッグするときの目視確認に使えます。

## SRD ランタイム向けのメモ

[docs/srd-runtime-notes.md](../../docs/srd-runtime-notes.md) を参照してください。
