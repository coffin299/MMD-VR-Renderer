# 技術構成まとめ

MMD-VR-Renderer が何をどう繋いで VR 動画を出すかの全体像です。実装の詳細な動機は [TECHNICAL_RESEARCH.md](TECHNICAL_RESEARCH.md)、短い構成表は [ARCHITECTURE.md](ARCHITECTURE.md) を参照してください。

## 1. 位置づけ

| 項目 | 内容 |
|------|------|
| 目的 | MMD + MME（ray-mmd 含む）の見た目を保ったまま、VR180／VR360 用の正距円筒映像をオフライン生成する |
| 非目的 | MMD 本体の改変、MMDX12 への移行、ray-mmd エフェクトファイルの改変 |
| 導入形態 | MME と同型の `d3d9.dll` プロキシ連鎖 |

```
MikuMikuDance.exe
    └─ d3d9.dll          … 本プロジェクト（MMD-VR-Renderer）
           └─ d3d9_mme.dll   … 改名した MME 本体
                  └─（必要なら）システムの d3d9
```

アンインストール用に、同じバイト列の別名 `d3d9_mmdvr.dll` も配置します。

## 2. ランタイム構成

ビルド成果の中心は共有ライブラリ `d3d9_mmdvr.dll`（導入時は `d3d9.dll` としてもコピー）です。論理レイヤは次のとおりです。

```
┌─────────────────────────────────────────────────────────┐
│  UI（MenuUI / RenderDialog / ProgressDialog / UiStrings）│
├─────────────────────────────────────────────────────────┤
│  PluginHost … ジョブ制御・フレームシーク・面キャプチャ要求 │
├──────────────┬──────────────────┬───────────────────────┤
│ DeviceHooks  │ D3D9Capture      │ MmdTimeline / WaveDetect│
│ (MinHook)    │ Backend          │ （MMD UI 操作）         │
├──────────────┴──────────────────┴───────────────────────┤
│  mmdvr_core（静的ライブラリ）                             │
│  RenderController / CameraController / ProjectionEngine │
│  StereoEngine / VideoEncoder / SphericalMp4Inject / …   │
└─────────────────────────────────────────────────────────┘
```

### 主要モジュール

| モジュール | 役割 |
|------------|------|
| `D3D9Proxy` | `d3d9_mme.dll` のロードとエクスポート転送 |
| `DeviceHooks` | `CreateDevice` / `Present` / `SetTransform` / `GetTransform` / `SetRenderTarget` / `BeginScene` / Draw* |
| `PluginHost` | デバイス寿命、キャプチャ同期、レンダーセッションの解放 |
| `MmdTimeline` | フレーム入力 Edit へのシーク（アニメ進行） |
| `MmdWaveDetect` | 読み込み済み音声パスの UI からの推定 |
| `CameraController` | VIEW／PROJECTION の観測と面カメラ上書き状態 |
| `D3D9CaptureBackend` | Present 直前のバックバッファ readback（RT プール） |
| `ProjectionEngine` | キューブマップ → equirect（360／180） |
| `StereoEngine` | IPD オフセットと左右並べ |
| `RenderController` | フレームループ、`job.json` 再開、エンコード起動 |
| `VideoEncoder` | 外部 FFmpeg（映像＋任意 AAC） |
| `SphericalMp4Inject` | YouTube 向け Spherical Video UUID 注入 |
| `Diagnostics` | `mmd_vr_renderer.log` |

オフライン単体ツール `mmdvr_offline.exe` は、既に揃った面画像から合成だけ行う（D3D／MMD 不要）。

## 3. 1 フレームの処理の流れ

キューブマップ ON（既定・真の VR 画角）の場合:

1. **シーク** — MMD のフレーム番号欄に目標フレームを書き、Enter 相当でポーズ評価  
2. **片目ごと**（ステレオなら左右）  
   - ジョブ開始時に MMD の出力サイズを正方形にする（4K なら一辺 4096）  
   - 各面は MMD のカメラ欄を確定するだけ。ビューポートも別 RT も変えない  
   - 保存するのは MMD が Present した画面。面の数は VR180 が 5、VR360 が 6（ステレオは左右）  
   - 終了後に出力サイズとカメラを戻し、従来どおりエンコードする  
3. **合成** — 面 → equirect（VR180 は前方半球）。ステレオなら SBS  
4. **PNG 書き出し**（非同期キュー可）と `job.json` 更新  
5. 全フレーム後 — FFmpeg で MP4、任意で WAV を AAC mux、Spherical メタデータ注入  

キューブマップ OFF（安全モード）は、現在の MMD カメラをそのままキャプチャして解像度へスケールするだけです（VR 画角にはなりません）。

## 4. フック方針（ray-mmd 向け）

| 処理 | 方針 |
|------|------|
| 通常視聴中 | SetTransform は無改変パススルー（上書きフラグ OFF） |
| 面キャプチャ中 | メイン相当 RT 上の透視 VIEW／PROJ のみ面カメラに置換。フレーム全体で維持 |
| GetTransform | 上書き中は面カメラを返す |
| キャプチャ | Present **前**、Present スレッド上のみ D3D を呼ぶ |
| キャンセル／ジョブ終了 | `releaseRenderSession`: キャプチャ要求解除、上書き OFF、観測行列の書き戻し、RT プール解放 |
| デバイス状態 | StretchRect／GetRenderTargetData の前後で RT／DS／Viewport／Scissor が変わっていれば戻す（盲目の全状態セーブはしない） |

診断用: 環境変数 `MMDVR_ISOLATE=1..3` でキャプチャ段階を切って切り分け可能。

## 5. 入出力成果物

| 成果物 | 説明 |
|--------|------|
| `frame_%06d.png` | 連番（再開時は `job.json` の続きから） |
| `job.json` | 設定・最終完了フレーム・完了フラグ |
| `mmdvr_job.mp4`（既定名） | FFmpeg 出力。音声パスがあれば AAC 同梱 |
| Spherical UUID | VR180／VR360 とステレオ情報。YouTube が VR として処理するための箱 |
| `mmd_vr_renderer.log` | 診断ログ（MMD 実行ファイル横など） |

## 6. 制約・既知の限界

1. 公式のカメラ API が無いため、面方向は D3D 行列フックに依存する（MMD 版差に注意）。  
2. SSR／SSAO などスクリーン空間効果は面の継ぎ目で破綻しうる（エフェクト本体は改変しない）。  
3. VR180 モノは YouTube 上でステレオ想定の制約がある場合がある。ステレオ SBS を推奨。  
4. 物理演算は「再生」ではなく「シーク評価」に依存する。事前の焼き込みが必要な場合がある。  
5. キャプチャは GPU→CPU readback のため重い。オフライン用途前提。

## 7. リポジトリ上の対応関係（ざっくり）

| パス | 内容 |
|------|------|
| `src/plugin/` | DLL 入口、プロキシ、フック、UI、MMD UI 連携 |
| `src/core/` | レンダー制御・投影・エンコード・メタデータ |
| `src/camera/` | カメラ数学・面カメラ生成 |
| `src/capture/` | D3D9 キャプチャ |
| `src/include/mmdvr/` | 公開ヘッダ |
| `tools/` | 依存取得、MMD への導入／削除、BOOTH インストーラ |
| `tests/` | 投影・ステレオ・job・数学の単体テスト |

## 8. 関連ドキュメント

- [ARCHITECTURE.md](ARCHITECTURE.md) — 短表  
- [TECHNICAL_RESEARCH.md](TECHNICAL_RESEARCH.md) — 調査と決定理由  
- [COMPATIBILITY_MATRIX.md](COMPATIBILITY_MATRIX.md) — 検証環境  
- [BUILD.md](BUILD.md) / [TESTING.md](TESTING.md) / [TROUBLESHOOTING.md](TROUBLESHOOTING.md)  
- English: [en/TECHNICAL_OVERVIEW.md](en/TECHNICAL_OVERVIEW.md)
