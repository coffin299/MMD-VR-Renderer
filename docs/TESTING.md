# テスト

## 自動

```powershell
cmake --build build --config Release
.\build\Release\mmdvr_tests.exe
.\build\Release\mmdvr_offline.exe --vr180 --stereo --out dist\sample_vr180
.\build\Release\mmdvr_offline.exe --vr360 --mono --out dist\sample_vr360
```

| スイート | 結果（参考） |
|----------|----------------|
| 投影／ステレオ／job／数学（7 件） | Pass |
| オフライン合成 VR180 SBS + VR360 モノ | Pass |

## 手動（実機 MMD）

1. `tools\install_to_mmd.ps1`  
2. MMD 起動 → **MMEffect** と **VRレンダラー** メニューが出ること  
3. **現在フレームをキャプチャ** → `mmdvr_captures` に PNG  
4. **カメラ上書きプローブ** → `*_probe_base.png` と `*_probe_yaw35.png` が異なること  
5. **VRレンダラーを開く...** → 短区間（1〜2 フレーム）で PNG／任意 MP4  
6. 必要なら PMM のハッシュを前後で比較（本ツールは PMM を書かない設計）

| 試験 | 結果（参考） |
|------|----------------|
| 起動 + MME 連鎖 | Pass |
| メニュー + キャプチャ | Pass |
| カメラプローブ | Pass（差分あり） |
| 複数フレーム UI ジョブ | 利用可。長時間 ray-mmd は手動確認 |

English: [en/TESTING.md](en/TESTING.md)
