# MMD-VR-Renderer

既存の **MikuMikuDance + MikuMikuEffect（+ ray-mmd）** 環境に、**VR180 / VR360** のオフラインレンダリング（モノ／ステレオ）を追加する Windows 拡張です。

`MikuMikuDance.exe` は改変しません。MMDX12 にも切り替えません。導入は MME と同型の **`d3d9.dll` プロキシ**で、改名した `d3d9_mme.dll` へ連鎖します。

[English docs](docs/en/README.md) · [English project summary](docs/en/PROJECT.md)

## 機能

- VR180 / VR360 × モノ／ステレオ（左右並べ SBS）
- キューブマップ複数面キャプチャ → 正距円筒（equirect）合成
- IPD オフセットによるステレオ
- PNG 連番出力 + 任意で FFmpeg による MP4 化
- MP4 への音声 mux（MMD 読込 WAV の自動検出／手動指定）
- YouTube 向け Spherical Video メタデータ注入
- 進捗表示・キャンセル・一時停止・`job.json` による再開
- 既定 60 FPS、画質プリセット（高／標準／高速）

## 動作要件

- MikuMikuDance 9.32 x64
- MikuMikuEffect 0.37（`d3d9_mme.dll` として配置）
- Windows 10 / 11 x64
- MP4 化する場合: FFmpeg（任意・自動検出あり）

## 推奨スペック（開発環境）

重い解像度（4K／8K・ステレオ）を想定した目安です。これ未満でも動きますが時間がかかります。

| 項目 | スペック |
|------|----------|
| CPU | Ryzen 9 7950X3D |
| RAM | 64GB |
| GPU | RTX 4080 Super |

## ソースからの導入

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\fetch_deps.ps1
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File .\tools\install_to_mmd.ps1
```

配布 zip を使う場合は展開後に `tools\install_to_mmd.ps1` を実行してください。

MMD 上: **VRレンダラー → VRレンダラーを開く...** ／ **現在フレームをキャプチャ** ／ **カメラ上書きプローブ**

## 現状（正直な一覧）

| 領域 | 状態 |
|------|------|
| MME を維持する d3d9 プロキシ + メニュー UI | MMD 9.32 で動作 |
| MME Present 後のフレームキャプチャ | 動作 |
| 面カメラ（SetTransform／GetTransform 統一） | 動作（ray-mmd は環境により要確認） |
| キューブマップ → VR180/360 + SBS + ジョブ再開 | 動作 |
| FFmpeg エンコード + 音声 mux + Spherical メタデータ | FFmpeg 利用時に動作 |
| 長時間 ray-mmd ステレオ本番 | UI から実行。SSR の継ぎ目は既知の制限 |
| MMDX12 | 未使用 |

## リリース（タグで自動配布）

```powershell
git tag v0.1.0
git push origin v0.1.0
```

成果物は GitHub Releases の `MMD-VR-Renderer-<tag>-win64.zip` です。  
不具合報告: https://github.com/coffin299/MMD-VR-Renderer/issues

## BOOTH インストーラー

BOOTH 向けには zip 本体ではなく、GitHub Releases から最新版を取得する小型 exe を置けます。

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_booth_installer.ps1
```

出力: `dist\booth\MMD-VR-Renderer-Installer.exe`  
使い方: exe 起動 → MMD フォルダ選択 → **導入／更新**

## ドキュメント

日本語が主、英語は `docs/en/` にあります。

- [ドキュメント索引](docs/README.md)
- [技術構成（まとめ）](docs/TECHNICAL_OVERVIEW.md)
- [アーキテクチャ](docs/ARCHITECTURE.md)
- [技術調査メモ](docs/TECHNICAL_RESEARCH.md)
- [互換性マトリクス](docs/COMPATIBILITY_MATRIX.md)
- [ビルド](docs/BUILD.md)
- [テスト](docs/TESTING.md)
- [トラブルシューティング](docs/TROUBLESHOOTING.md)
- 変更履歴: [CHANGELOG.txt](CHANGELOG.txt)

## コントリビュート

プルリクエストを歓迎します（バグ修正・互換性・ドキュメントなど）。  
不具合は [Issues](https://github.com/coffin299/MMD-VR-Renderer/issues) へ。

## ライセンス

MIT（[LICENSE](LICENSE)）。第三者表記は [NOTICE](NOTICE)。
