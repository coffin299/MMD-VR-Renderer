#include "UiStrings.hpp"
#include <fstream>

namespace mmdvr::ui {
namespace {

Lang g_lang = Lang::Ja;

std::wstring prefPath() {
  wchar_t appdata[MAX_PATH]{};
  if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH))
    return L"";
  return std::wstring(appdata) + L"\\MMD-VR-Renderer\\ui_lang.txt";
}

const Bundle kJa{
  L"MMD-VR-Renderer",
  L"VRレンダラー",
  L"VRレンダラーを開く...",
  L"現在フレームをキャプチャ",
  L"カメラ上書きプローブ",
  L"バージョン情報",
  L"バージョン情報",
  L"MMD-VR-Renderer\n"
  L"MMD+MME向け VR180/VR360 オフラインレンダラーです。\n\n"
  L"MMDX12は使いません。d3d9プロキシ連鎖でMMEを維持します。\n\n"
  L"不具合は GitHub Issues へ。プルリクエスト歓迎します。\n"
  L"https://github.com/coffin299/MMD-VR-Renderer/issues\n\n"
  L"推奨スペック(開発環境):\n"
  L"Ryzen 9 7950X3D / RAM 64GB / RTX 4080 Super",
  L"MMD VRレンダラー",
  L"VRモード",
  L"ステレオ",
  L"画質",
  L"解像度",
  L"2K  3840×1920",
  L"3K  5760×2880",
  L"4K  7680×3840",
  L"8K  15360×7680 (YouTube・容量超大)",
  L"サイズ",
  L"FPS",
  L"IPD単位",
  L"CPUスレッド",
  L"面解像度",
  L"※ キューブ1面の辺(px)。主にVRAM（面RT）＋キャプチャ時はRAMも消費。最終2K〜8Kとは別",
  L"連番書込",
  L"キュー(RAM)",
  L"連番形式",
  L"自動（大解像度→JPEG）",
  L"PNG（可逆・大容量）",
  L"JPEG q92（小容量）",
  L"開始フレーム",
  L"終了フレーム",
  L"プロジェクト名",
  L"出力ルート",
  L"...",
  L"FFmpegでMP4出力 → ルート\\プロジェクト名\\プロジェクト名.mp4",
  L"エンコーダ",
  L"音声(WAV)",
  L"※ 空欄可。MMD読込WAVを自動検出／手動指定。開始フレーム位置からmux",
  L"音声ファイルを選ぶ",
  L"VRキューブマップ面カメラ（90°×複数面→正距円筒）",
  L"※ OFFだと通常画角のまま拡大のみ。VR180/360にはON必須。キャンセル後はカメラ復元",
  L"CPU（libx265→libx264）",
  L"自動（既定・av1_nvenc→libx265）",
  L"NVENC（hevc_nvenc優先）",
  L"QSV（hevc_qsv優先）",
  L"AMF（hevc_amf優先）",
  L"AV1（av1_nvenc優先 → HEVCフォールバック）",
  L"開始",
  L"キャプチャ",
  L"キャンセル",
  L"※ 0=自動（空きRAMで上限）。大きい値は自己責任。",
  L"※ サイズは片目分。ステレオは幅2倍。連番は PNG/JPEG 選択可（自動=大解像度JPEG q92・約50MB/枚）。空き不足で自動一時停止。",
  L"容量が超大きい（PNG/MP4）",
  L"不具合・要望:",
  L"<a href=\"https://github.com/coffin299/MMD-VR-Renderer/issues\">GitHub Issues</a>",
  L"推奨スペック(開発環境): Ryzen 9 7950X3D / 64GB / RTX 4080 Super",
  L"プルリクエスト歓迎します",
  L"★ このPC向けの最適値を入れる",
  L"検出: CPU %d / RAM %uGB / VRAM空き約%uGB · %s",
  L"高スペック寄り → 8K / 高画質 / 面8192（VRAM16GB級）",
  L"中〜高 → 8K / 高画質 / 面4096（VRAM8GB級）",
  L"中スペック → 3K / 標準 / 面1536",
  L"軽め → 2K / 高速 / 面1024",
  L"0=全コア",
  L"0=自動",
  L"レンダリング中",
  L"VRレンダリング進行状況",
  L"進捗 %d / %d フレーム (%d%%)",
  L"一時停止中",
  L"一時停止中（ディスク空き不足）— 空きを確保して「再開」",
  L"PNG書き込み完了待ち…",
  L"動画エンコード中（FFmpeg）",
  L"VRメタデータ書き込み中",
  L"MMD表示を復元中…",
  L"完了",
  L"キャンセル済み",
  L"キャンセルしています…",
  L"現在フレーム: %d\n経過: %s\n1Fあたり: %.2f 秒\n残り推定: %s",
  L"経過: %s\n%s\n%s",
  L"キューに残ったPNGをディスクへ書き出しています。\n完了するまで少々お待ちください。",
  L"PNG連番からMP4を生成しています。\n解像度・長さによっては数分かかることがあります。",
  L"YouTube VR認識用のSphericalメタデータを埋め込んでいます。",
  L"カメラ・出力サイズ・UI表示を元に戻しています。",
  L"一時停止",
  L"再開",
  L"キャンセル",
  L"モノ",
  L"ステレオ",
  L"高画質",
  L"標準",
  L"高速",
  L"自動（解像度に合わせる）",
  L"自動(RAM)",
  L"出力フォルダを選ぶ",
  L"片目 %d×%d  →  最終(左右並べ) %d×%d",
  L"最終 %d×%d（モノ）",
  L"保存しました:\nmmdvr_captures\\frame_000000_preview.png",
  L"キャプチャに失敗しました。3Dビューは表示されていますか？",
  L"別の処理が実行中です。",
  L"カメラプローブ",
  L"基準キャプチャに失敗しました。",
  L"レンダリング実行中です。",
  L"レンダリングが完了しました。",
  L"レンダリングがエラーまたはキャンセルで終了しました。\nmmd_vr_renderer.log を確認してください。",
  L"出力フォルダを開きますか？",
  L"日本語",
  L"English",
  L"レンダリング実行前に必ずプロジェクトを保存してください",
  L"エンコーダ: %s\n（入力がエンコード上限超過時のみ縮小。連番はフル解像度のまま）",
  L"未完了のジョブがあります。\n\n"
  L"次フレーム: %d（完了済 %d / 予定 %d）\n"
  L"フォルダ:\n%s\n\n"
  L"はい = 途中から再開\n"
  L"いいえ = 最初からやり直し\n"
  L"キャンセル = 中止",
  L"レンダー",
  L"MP4再エンコード",
  L"連番(PNG/JPEG)が残っているプロジェクトから MP4 だけ作り直します。\n"
  L"出力ルート\\プロジェクト名 または …\\frames を指定してください。",
  L"プロジェクト / frames",
  L"プロジェクトフォルダを選ぶ",
  L"再エンコード開始",
  L"YouTube VR用 Spherical メタデータを埋め込む",
  L"frame_*.png / .jpg が見つかりません。",
  L"エンコード中…（完了までこのダイアログを閉じないでください）",
  L"MP4を書き出しました:\n%s",
  L"再エンコードに失敗しました。\n%s",
};

const Bundle kEn{
  L"MMD-VR-Renderer",
  L"VR Renderer",
  L"Open VR Renderer...",
  L"Capture Current Frame",
  L"Camera Override Probe",
  L"About",
  L"About",
  L"MMD-VR-Renderer\n"
  L"Offline VR180/VR360 renderer for MMD+MME.\n\n"
  L"Does not use MMDX12; keeps MME via d3d9 proxy chain.\n\n"
  L"Report bugs on GitHub Issues. Pull requests welcome.\n"
  L"https://github.com/coffin299/MMD-VR-Renderer/issues\n\n"
  L"Recommended (dev machine):\n"
  L"Ryzen 9 7950X3D / RAM 64GB / RTX 4080 Super",
  L"MMD VR Renderer",
  L"VR Mode",
  L"Stereo",
  L"Quality",
  L"Resolution",
  L"2K  3840×1920",
  L"3K  5760×2880",
  L"4K  7680×3840",
  L"8K  15360×7680 (YouTube · huge files)",
  L"Size",
  L"FPS",
  L"IPD",
  L"CPU threads",
  L"Face res",
  L"Note: cubemap face edge (px). Uses VRAM (face RTs) + RAM on capture. Not the final 2K–8K",
  L"Seq writers",
  L"Queue (RAM)",
  L"Seq format",
  L"Auto (JPEG if large)",
  L"PNG (lossless, large)",
  L"JPEG q92 (smaller)",
  L"Start frame",
  L"End frame",
  L"Project name",
  L"Output root",
  L"...",
  L"Encode MP4 with FFmpeg → root\\project\\project.mp4",
  L"Encoder",
  L"Audio (WAV)",
  L"Optional. Auto-detect MMD-loaded WAV or browse. Muxed from start-frame offset",
  L"Choose audio file",
  L"VR cubemap face cameras (90° faces → equirect)",
  L"OFF = stretch live camera only. ON required for real VR180/360 FOV; camera restored after cancel",
  L"CPU (libx265→libx264)",
  L"Auto (default · av1_nvenc→libx265)",
  L"NVENC (prefer hevc_nvenc)",
  L"QSV (prefer hevc_qsv)",
  L"AMF (prefer hevc_amf)",
  L"AV1 (prefer av1_nvenc → HEVC fallback)",
  L"Start",
  L"Capture",
  L"Cancel",
  L"Note: 0 = auto (capped by free RAM). High values use a lot of RAM.",
  L"Note: size is per eye; stereo doubles width. Choose PNG/JPEG sequence (Auto = JPEG q92 ~50MB/frame when large). Auto-pauses if disk fills.",
  L"extremely large PNG/MP4 files",
  L"Bugs / requests:",
  L"<a href=\"https://github.com/coffin299/MMD-VR-Renderer/issues\">GitHub Issues</a>",
  L"Recommended (dev): Ryzen 9 7950X3D / 64GB / RTX 4080 Super",
  L"Pull requests welcome",
  L"★ Fill optimal values for this PC",
  L"Detected: %d CPU / %uGB RAM / ~%uGB free VRAM · %s",
  L"High-end → 8K / High / face 8192 (16GB-class VRAM)",
  L"Mid-high → 8K / High / face 4096 (8GB-class VRAM)",
  L"Mid → 3K / Balanced / face 1536",
  L"Light → 2K / Fast / face 1024",
  L"0=all cores",
  L"0=auto",
  L"Rendering",
  L"VR render progress",
  L"Progress %d / %d frames (%d%%)",
  L"Paused",
  L"Paused (disk nearly full) — free space, then Resume",
  L"Flushing remaining PNGs…",
  L"Encoding video (FFmpeg)",
  L"Writing VR metadata",
  L"Restoring MMD view…",
  L"Done",
  L"Cancelled",
  L"Cancelling…",
  L"Frame: %d\nElapsed: %s\nPer frame: %.2f sec\nETA: %s",
  L"Elapsed: %s\n%s\n%s",
  L"Writing queued PNGs to disk.\nPlease wait until this finishes.",
  L"Building MP4 from the PNG sequence.\nHigh resolutions can take several minutes.",
  L"Embedding Spherical metadata for YouTube VR.",
  L"Restoring camera, output size, and viewport UI.",
  L"Pause",
  L"Resume",
  L"Cancel",
  L"Mono",
  L"Stereo",
  L"High",
  L"Balanced",
  L"Fast",
  L"Auto (from resolution)",
  L"Auto (RAM)",
  L"Choose output folder",
  L"Per eye %d×%d  →  Final (SBS) %d×%d",
  L"Final %d×%d (mono)",
  L"Saved:\nmmdvr_captures\\frame_000000_preview.png",
  L"Capture failed. Is the 3D view visible?",
  L"Another job is already running.",
  L"Camera Probe",
  L"Baseline capture failed.",
  L"Render already running.",
  L"Render finished.",
  L"Render ended with errors or cancel.\nSee mmd_vr_renderer.log",
  L"Open the output folder?",
  L"日本語",
  L"English",
  L"Always save your project before rendering",
  L"Encoder: %s\n(Scaled only if input exceeds encoder limits; sequence stays full-res)",
  L"An unfinished job was found.\n\n"
  L"Next frame: %d (done %d / planned %d)\n"
  L"Folder:\n%s\n\n"
  L"Yes = resume from there\n"
  L"No = restart from the beginning\n"
  L"Cancel = abort",
  L"Render",
  L"Re-encode MP4",
  L"Rebuild MP4 from an existing PNG/JPEG sequence.\n"
  L"Pick the job folder (output\\project) or its frames\\ subfolder.",
  L"Project / frames",
  L"Choose project folder",
  L"Start re-encode",
  L"Embed Spherical metadata for YouTube VR",
  L"No frame_*.png / .jpg found.",
  L"Encoding… (do not close this dialog until it finishes)",
  L"MP4 written:\n%s",
  L"Re-encode failed.\n%s",
};

} // namespace

void setLang(Lang lang) {
  g_lang = lang;
  saveLangPreference();
}

Lang lang() { return g_lang; }
bool isJa() { return g_lang == Lang::Ja; }

const Bundle& t() { return g_lang == Lang::Ja ? kJa : kEn; }

void loadLangPreference() {
  auto path = prefPath();
  if (path.empty()) return;
  std::ifstream f(path);
  if (!f) return;
  std::string s;
  f >> s;
  if (s == "en" || s == "EN") g_lang = Lang::En;
  else g_lang = Lang::Ja;
}

void saveLangPreference() {
  auto path = prefPath();
  if (path.empty()) return;
  auto dir = path.substr(0, path.find_last_of(L"\\/"));
  CreateDirectoryW(dir.c_str(), nullptr);
  std::ofstream f(path);
  if (!f) return;
  f << (g_lang == Lang::En ? "en" : "ja");
}

} // namespace mmdvr::ui
