# SRD ランタイムを Spatial Containers に対応させるためのメモ

公開されている仕様（OpenXR-Docs、`XR_EXT_spatial_container` rev.1 と `XR_EXT_spatial_container_self_rendering`）と、Godot 4.8 の実装だけをもとにした個人的なメモです。SRD の内部実装については何も前提にしていません。

## 1. OpenXR 1.1 への移行は前提条件ではない

- レジストリ（`xr.xml`）を見ると、`XR_EXT_spatial_container` には `depends` 属性がなく、コアのバージョンに依存していません。`self_rendering` が依存しているのも `XR_EXT_spatial_container` だけです。
- つまり、**1.0 のランタイムのままでもこの拡張は実装できます**。1.1 への移行と spatial container の対応は、別々に段階を踏んで進められます。

## 2. 最小構成：immersive だけ対応する

仕様の要点は次の4つです。

- ランタイムは bounded と immersive の**少なくとも一方**に対応していればよい。
- 描画するランタイムが `self_rendering` に対応する場合は、`supportsImmersive = true` が必須。
- immersive のコンテナは「拡張を使わない従来のフルスクリーンアプリと同じように扱う」。
- 仕様の注記によれば、従来の 1.x の経路は「暗黙のうちに immersive のコンテナを1つ作って表示する」ことと等価。

ここから考えると、最初の一歩は次の構成になります。

```
supportsImmersive = true
supportsBounded   = false
maxSpatialContainerCount = 1
```

今の描画経路をほぼそのまま使い、API の形だけを新しい仕様に合わせる進め方です。bounded への対応はその後に足せます。

## 3. bounded を SRD でどう表現するか

- **境界の大きさ**：境界ボックスを、ディスプレイで無理なく立体に見える範囲に合わせます。`suggestedBounds` は無視してかまいません（仕様上 may）。
- **原点と向き**：コンテナ空間の原点は境界ボックスの中心です。+X は右、+Y は上、+Z は手前の面を向きます。仕様には「重力方向に揃っていて正面にある場合は LOCAL に対して回転なし」という例があります。SRD のように**パネルが傾いている**場合、ボックスを次のどちらに揃えるかは設計判断になります。

  | 揃え方 | 利点 | 欠点 |
  |---|---|---|
  | 重力方向（床が水平） | 物を置く感覚に合う | 表示空間を使い切れない |
  | パネル面 | 表示空間を使い切れる | アプリから見て「床」が傾く |

- **Godot アプリへの影響**：bounded の間、Godot はコンテナ空間をプレイスペースとして使います。そのため、原点に置いたコンテンツはコンテナの中心に表示されます。中心をどこに置くかは、アプリの見え方に直接影響します。
- **境界の変更**：境界は bounded の間いつでも変えられます。変えた場合は `BoundsChanged` イベントを出します（まとめて1回でも、フレームごとでも可）。

## 4. 描画まわり（self_rendering）

- **セッションの状態**：セッションは IDLE のまま、running として扱われます。`xrBeginSession`、`xrEndSession`、`xrRequestExitSession`、`xrLocateViews` は `XR_ERROR_SPATIAL_CONTAINERS_ENABLED_EXT` を返します。`xrWaitFrame` の `shouldRender` は常に false になります。
- **視点の取得**：視点は `xrLocateSpatialContainerViewsEXT` でコンテナごとに返します。
  - **FOV**：仕様は「コンテナの境界を含む最小の視錐台」にすることを推奨していて、**非対称の FOV もはっきり認めています**。視線追跡で、ディスプレイ越しに見る斜めの視錐台を作る SRD とは相性が良いはずです。
  - **`recommendedImageExtent`**：コンテナが画面に占める大きさに合わせて解像度を下げられます。
  - **`shouldSubmitLayers = false`**：画面上に正しい視点を出せない場合（コンテナが画面外にあるときなど）に返します。視線追跡を見失ったときにこれを返すのか、最後の姿勢を使い続けるのかは、ランタイムの方針として決める必要があります。
  - **視点数の削減**：ステレオをモノラルに落とすことが仕様上できます（その場合、view の数はそのままで中身を複製します）。
- **レイヤーの提出**：`xrEndFrame` に `XrSpatialContainerLayerFrameEndInfoEXT` を付けて、コンテナごとに提出します。本体の `layerCount` が 0 以外ならエラーです。
- **境界外の切り取り**：`XrSpatialContainerLayerVolumeClippingEXT`（NONE / STRICT）を指定できます。表示範囲の外にある内容は見づらくなりやすいので、STRICT の実装を優先するとよさそうです。

## 5. 表示とフォーカスを Windows の仕組みに対応させる

| 仕様のイベント | 対応させる候補 |
|---|---|
| `VisibleChanged` | SRD 出力の表示・非表示、最小化 |
| `InteractableChanged` | Windows のフォーカス（キーボードやマウスの入力先） |
| `Closed` | ユーザーがアプリを閉じたとき。visible だった場合は、先に `VisibleChanged(false)` を出す |
| `VisibleRequestDenied` | 最小化された状態からの再表示を拒否する場合など |

- アプリが一度も表示を要求していないコンテナを、ランタイムが表示してはいけません（must not）。
- 表示を始めるときは、最初のフレームが届くまで描画を遅らせてもかまいません。ただし 0.5 秒以内が推奨です。

## 6. 将来：複数のコンテナ

1台の SRD の表示空間に複数のアプリのコンテナを並べるには、プロセスをまたいで合成するコンポジタが必要になります。まず `maxSpatialContainerCount = 1` から始め、同時に表示するのは1つだけにするのが現実的です。仕様にも、制約の多いデバイスでは同時表示を1つに限る例が書かれています。

## 7. 準拠テスト（CTS）

仕様の Issues に、ランタイムの方針で決まる挙動を CTS から操作するための「CTS 専用のテスト拡張」を別に定義すると書かれています。CTS に対応するときに確認してください。

## このサンプルで確認できること

- `Label3D` に、ランタイムが返す境界の大きさ、モード、visible / interactable を表示します。
- B キーで、境界のワイヤーフレームを実際の表示空間と見比べられます。
- Space キーでモード切り替えを要求し、成功と拒否の両方の流れを試せます。
