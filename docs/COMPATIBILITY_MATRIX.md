# 互換性マトリクス

| コンポーネント | 版／パス | 状態 | メモ |
|----------------|----------|------|------|
| MikuMikuDance | 9.32 x64 | **検証済** | プロキシで起動可 |
| MikuMikuEffect | 0.37 を `d3d9_mme.dll` | **検証済** | 連鎖ロードをログ確認 |
| ray-mmd | 1.5.0（UserFile 配下など） | Present／**部分** | 単フレームキャプチャ可。複数面 SSR 継ぎ目はシーン依存 |
| MMDPlugin | 未導入 | N/A | 不要 |
| MMDX12 | — | 未使用 | 参考のみ |
| Windows | 10／11 x64 | **検証済** | 開発機 |
| VS ビルド | 2019 BuildTools／VS2022 | **検証済** | CMake VS 16 2019 ジェネレータ等 |

## ランタイム確認（参考）

| 確認 | 結果 |
|------|------|
| プロキシ読込／`InitD3D9Proxy` | Pass |
| `Loaded d3d9_mme.dll` | Pass |
| Present／SetTransform フック | Pass |
| VRレンダラーメニュー | Pass |
| 現在フレームキャプチャ PNG | Pass |
| カメラ上書きプローブ | Pass（チャンネル平均差あり） |
| 単体テスト（7） | Pass |
| オフライン VR180／VR360 合成 | Pass |
| 長時間 ray-mmd ステレオ本番 | 自動ループでは未完走（UI で手動） |
| PMM 非改変 | 設計上 PMM を書かない。必要なら手動でハッシュ確認 |

English: [en/COMPATIBILITY_MATRIX.md](en/COMPATIBILITY_MATRIX.md)
