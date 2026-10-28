# アーキテクチャ

詳細な技術構成は [TECHNICAL_OVERVIEW.md](TECHNICAL_OVERVIEW.md) を参照してください。

## 概要

MMD-VR-Renderer は、既存の MMD + MME 導入に対し `d3d9.dll` プロキシとして入り、`d3d9_mme.dll`（改名した MME）へ連鎖します。MMD のレンダラは置き換えず、MMDX12 も使いません。

## コンポーネント

| コンポーネント | 役割 |
|----------------|------|
| `d3d9` プロキシ | MME／システム D3D9 のロード、CreateDevice／Present／SetTransform 等のフック |
| `PluginHost` | メニュー UI、ジョブ制御、描画要求 |
| `CameraController` | VIEW／PROJECTION の観測と複数面キャプチャ時の上書き |
| `D3D9CaptureBackend` | Present 前のバックバッファ取得 |
| `ProjectionEngine` | キューブマップ → 正距円筒（VR360／VR180） |
| `StereoEngine` | IPD オフセットと SBS 合成 |
| `RenderController` | フレームループ、`job.json` 再開 |
| `ImageSequenceWriter` | PNG 連番 |
| `VideoEncoder` | 外部 FFmpeg（映像＋任意音声） |
| `SphericalMp4Inject` | YouTube 向け Spherical メタデータ |
| `Diagnostics` | `mmd_vr_renderer.log` |

## データフロー

1. ユーザーが **VRレンダラー** メニューから設定ダイアログを開く。  
2. 各アニメーションフレーム: MMD のフレーム欄へシーク → ポーズ評価 → ライブ視点またはキューブ各面をキャプチャ。  
3. 面を equirect に縫合（ステレオなら SBS）。  
4. `frame_XXXXXX.png` を書き、`job.json` を更新。  
5. 任意で FFmpeg により MP4 化し、音声 mux と Spherical メタデータを付与。

## 制約

- 公式 MMD プラグイン API が無いためフックが必要。  
- `MMDExport.h` にカメラ setter は無く、カメラ制御は変換行列ベース（MMD 版ごとの検証が必要）。  
- スクリーン空間効果（SSR／SSAO）は面をまたぐと継ぎ目が出うる。エフェクト本体は改変しない。

English: [en/ARCHITECTURE.md](en/ARCHITECTURE.md)
