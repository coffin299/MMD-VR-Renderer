# 技術調査メモ

## 参照した知見

- [cchina33/MMD-Plugin-Loader-Injection-Hooking](https://github.com/cchina33/MMD-Plugin-Loader-Injection-Hooking): MME 互換の `d3d9` → `d3d9_mme.dll` 連鎖と Present キャプチャの型。  
- oigami MMDPlugin（BowlRoll）: マルチプラグインハブ — 本プロジェクトの対象導入は素の MME のため **必須ではない**。  
- MMDX12: 別系統の DX12 アプリ — **参考のみ**（ray-mmd／MME 経路と同一出力は未検証）。  
- コミュニティの VR 手順: EquirectangularX + 複数 AVI、DomeMaster 系キューブ補助 — 投影設計の参考。

## MMDExport（手元 9.32 の `Data/MMDExport.h`）

利用可: `ExpGetFrameTime`、ボーン／アクセサリ問い合わせ、`ExpSetRenderRepeatCount` など。  
**無いもの:** カメラ eye／target の setter。

## リスク

1. SetTransform 上書きは MMD／MME の各パスで上書きされうる。複数面品質はフックタイミングに依存。  
2. Soul-chan 系 `MMDMainData` RVA は 9.31 向け — 9.32 では **未使用**。  
3. `GetRenderTargetData` は重い。オフライン用途のみ。  
4. ステレオ + ray-mmd SSR は、真の単パス全方位 BRDF とは一致しない。

## 決定事項

| 決定 | 選択 | 理由 |
|------|------|------|
| 統合方式 | d3d9 プロキシ連鎖 | 導入済み MME と整合。MMDPlugin.lib 不要 |
| 投影 | キューブ縫合 | オフラインで検証しやすい。DomeMaster 系の実務に近い |
| エンコーダ | 外部 FFmpeg | プロセス内への GPL 再配布を避ける |
| レンダラ | MMD+MME を維持 | 要件の最優先 |
| 複数面待ち | 背景スレッド + Present 条件変数 | UI スレッドでの Present 待ちはデッドロック（実証済） |
| YouTube VR | Spherical UUID 箱の注入 | ffmpeg の `-metadata` 文字列だけでは認識されない |
| アニメ進行 | フレーム Edit への UI シーク | 内部 API が無いため |

## 実機結果の要約（MMD 9.32 + MME 0.37）

- Present 前 StretchRect → SYSTEMMEM キャプチャ: **動作**  
- Present 継続中の VIEW 上書き: **動作**（プローブで画素差あり）  
- UI スレッドでの Present 待ち: **失敗**（差 0／タイムアウト）  
- 9.31 Soul-chan RVA ヘルパ: **未使用**  
- 面カメラと deferred の行列不一致: 破片の主因になりうる → 面キャプチャ中は GetTransform も含め統一  

English: [en/TECHNICAL_RESEARCH.md](en/TECHNICAL_RESEARCH.md)
