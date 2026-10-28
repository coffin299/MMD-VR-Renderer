# トラブルシューティング

## 導入後に MMD が起動しない

- `tools\uninstall_from_mmd.ps1` を実行する。  
- `d3d9_mme.dll`（元 MME）が残っているか確認する。  
- `mmd_vr_renderer.log` を確認する。

## MMEffect メニューが出ない

- プロキシが `d3d9_mme.dll` を読めていること。ログに `Loaded d3d9_mme.dll`。  
- MME を入れ直し、`d3d9.dll` → `d3d9_mme.dll` に改名してから本プロキシを再導入する。

## GetRenderTargetData 失敗

- 現行は非 MSAA の RT へ StretchRect してから SYSTEMMEM へ読む。  
- 最新の `d3d9.dll` ビルドに更新する。

## カメラプローブ／複数面で変化が小さい

- Present 待ちを MMD の UI スレッドでブロックしない（デッドロックする）。  
- **カメラ上書きプローブ**（背景スレッド）かレンダージョブのワーカーを使う。  
- 非同期プローブでも差がほぼ 0 なら、MME が後からカメラを書き戻している可能性がある。ログのタイミングを確認し、高速プリセットも試す。

## パノラマの継ぎ目・SSR／SSAO

- キューブ面をまたぐスクリーン空間効果では起きうる。ray-mmd 本体は改変しない。SSR を弱めるか継ぎ目を許容する。

## レンダ開始後に描画が壊れ、キャンセルしても戻らない

- 最新ビルドではキャンセル／終了時にカメラ復元とフック解除を行う。  
- それでも壊れる場合は UI で「VRキューブマップ面カメラ」を OFF にして切り分け、`mmd_vr_renderer.log` と Issues へ。  
- 診断: 環境変数 `MMDVR_ISOLATE=1`（D3D 読取なし）など。

## 出力がずっと同じポーズ（フレーム 0）

- フレームシークが効いていない。ログに `MmdTimeline` の成否を確認。  
- MMD のフレーム番号欄が見える状態で再試行する。

## MP4 に音が無い／YouTube が VR にならない

- ダイアログの「音声(WAV)」が空なら mux しない。自動検出失敗時は手動指定。  
- VR メタデータはキューブマップ ON 時に Spherical UUID を注入。単なるタグではない。アップロード後の YouTube 処理完了を待つ。

## FFmpeg エンコード失敗

- `dist\ffmpeg\ffmpeg.exe` か PATH、またはダイアログでエンコードを外して PNG のみ。  
- 途中再開した PNG は `-start_number` 対応済みの最新ビルドを使う。

## ジョブ再開

- 出力先の `job.json` を消すと最初からやり直せる。

English: [en/TROUBLESHOOTING.md](en/TROUBLESHOOTING.md)
