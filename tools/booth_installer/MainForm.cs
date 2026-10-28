namespace MmdVrBoothInstaller;

internal sealed class MainForm : Form
{
    private readonly TextBox _mmdPath = new();
    private readonly Button _browse = new();
    private readonly Label _latestLabel = new();
    private readonly Label _installedLabel = new();
    private readonly ProgressBar _progress = new();
    private readonly Label _status = new();
    private readonly Button _check = new();
    private readonly Button _install = new();
    private readonly Button _openReleases = new();
    private readonly LinkLabel _issues = new();

    private ReleaseInfo? _latest;
    private CancellationTokenSource? _cts;
    private bool _busy;

    public MainForm()
    {
        Text = "MMD-VR-Renderer インストーラー (BOOTH)";
        Font = new Font("Yu Gothic UI", 9F);
        MinimumSize = new Size(520, 360);
        Size = new Size(560, 400);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;

        var title = new Label
        {
            Text = "GitHub Releases の最新版を取得して MMD フォルダへ導入します。",
            AutoSize = false,
            Location = new Point(16, 14),
            Size = new Size(510, 22)
        };

        var mmdCaption = new Label
        {
            Text = "MMD フォルダ（MikuMikuDance.exe がある場所）",
            AutoSize = true,
            Location = new Point(16, 48)
        };

        _mmdPath.Location = new Point(16, 72);
        _mmdPath.Size = new Size(400, 27);
        _mmdPath.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        _mmdPath.TextChanged += (_, _) => RefreshInstalledLabel();

        _browse.Text = "参照…";
        _browse.Location = new Point(426, 70);
        _browse.Size = new Size(100, 28);
        _browse.Click += BrowseClick;

        _latestLabel.Text = "最新版: （未確認）";
        _latestLabel.AutoSize = false;
        _latestLabel.Location = new Point(16, 112);
        _latestLabel.Size = new Size(510, 22);

        _installedLabel.Text = "導入済み: （フォルダを選択）";
        _installedLabel.AutoSize = false;
        _installedLabel.Location = new Point(16, 136);
        _installedLabel.Size = new Size(510, 22);

        _progress.Location = new Point(16, 170);
        _progress.Size = new Size(510, 22);
        _progress.Minimum = 0;
        _progress.Maximum = 100;

        _status.Text = "「最新を確認」→「導入 / 更新」の順で実行してください。";
        _status.AutoSize = false;
        _status.Location = new Point(16, 200);
        _status.Size = new Size(510, 44);

        _check.Text = "最新を確認";
        _check.Location = new Point(16, 256);
        _check.Size = new Size(120, 32);
        _check.Click += async (_, _) => await CheckLatestAsync();

        _install.Text = "導入 / 更新";
        _install.Location = new Point(146, 256);
        _install.Size = new Size(140, 32);
        _install.Enabled = false;
        _install.Click += async (_, _) => await InstallOrUpdateAsync();

        _openReleases.Text = "Release ページ";
        _openReleases.Location = new Point(296, 256);
        _openReleases.Size = new Size(120, 32);
        _openReleases.Click += (_, _) =>
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = GitHubReleaseClient.ReleasesPageUrl,
                UseShellExecute = true
            });

        _issues.Text = "不具合報告 (Issues)";
        _issues.AutoSize = true;
        _issues.Location = new Point(16, 304);
        _issues.LinkClicked += (_, _) =>
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = $"https://github.com/{GitHubReleaseClient.Owner}/{GitHubReleaseClient.Repo}/issues",
                UseShellExecute = true
            });

        Controls.AddRange(new Control[]
        {
            title, mmdCaption, _mmdPath, _browse,
            _latestLabel, _installedLabel, _progress, _status,
            _check, _install, _openReleases, _issues
        });

        TryLoadSavedPath();
        Shown += async (_, _) =>
        {
            try { await CheckLatestAsync(); } catch { /* status already set */ }
        };
    }

    private void TryLoadSavedPath()
    {
        try
        {
            var cfg = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "MMD-VR-Renderer", "booth_installer.json");
            if (!File.Exists(cfg)) return;
            var text = File.ReadAllText(cfg).Trim().Trim('"');
            if (Directory.Exists(text))
            {
                _mmdPath.Text = text;
                RefreshInstalledLabel();
            }
        }
        catch { /* ignore */ }
    }

    private void SavePath(string path)
    {
        try
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "MMD-VR-Renderer");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "booth_installer.json"),
                System.Text.Json.JsonSerializer.Serialize(path));
        }
        catch { /* ignore */ }
    }

    private void BrowseClick(object? sender, EventArgs e)
    {
        using var dlg = new FolderBrowserDialog
        {
            Description = "MikuMikuDance.exe があるフォルダを選択",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = false
        };
        if (!string.IsNullOrWhiteSpace(_mmdPath.Text) && Directory.Exists(_mmdPath.Text))
            dlg.SelectedPath = _mmdPath.Text;

        if (dlg.ShowDialog(this) == DialogResult.OK)
        {
            _mmdPath.Text = dlg.SelectedPath;
            SavePath(dlg.SelectedPath);
            RefreshInstalledLabel();
        }
    }

    private void RefreshInstalledLabel()
    {
        var dir = _mmdPath.Text.Trim();
        if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
        {
            _installedLabel.Text = "導入済み: （フォルダを選択）";
            return;
        }

        if (!InstallService.LooksLikeMmdFolder(dir))
        {
            _installedLabel.Text = "導入済み: ⚠ MikuMikuDance.exe が見つかりません";
            return;
        }

        var state = InstallService.ReadState(dir);
        var proxy = File.Exists(Path.Combine(dir, "d3d9_mme.dll")) &&
                    (File.Exists(Path.Combine(dir, "d3d9_mmdvr.dll")) ||
                     File.Exists(Path.Combine(dir, "d3d9.dll")));
        if (state?.InstalledTag is { Length: > 0 } tag)
        {
            var hint = _latest is not null && InstallService.NeedsUpdate(tag, _latest.TagName)
                ? " → 更新あり"
                : proxy ? " （導入済み）" : " （記録あり・DLL要確認）";
            _installedLabel.Text = $"導入済み: {tag}{hint}";
        }
        else if (proxy)
        {
            _installedLabel.Text = "導入済み: バージョン不明（更新で最新を導入できます）";
        }
        else
        {
            _installedLabel.Text = "導入済み: なし（新規導入）";
        }
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        _check.Enabled = !busy;
        _browse.Enabled = !busy;
        _mmdPath.Enabled = !busy;
        _install.Enabled = !busy && _latest is not null;
        UseWaitCursor = busy;
    }

    private async Task CheckLatestAsync()
    {
        if (_busy) return;
        SetBusy(true);
        _status.Text = "GitHub から最新 Release を確認中…";
        _progress.Value = 0;
        try
        {
            _cts?.Cancel();
            _cts = new CancellationTokenSource();
            _latest = await GitHubReleaseClient.GetLatestAsync(_cts.Token);
            var when = _latest.PublishedAt?.ToLocalTime().ToString("yyyy-MM-dd HH:mm") ?? "-";
            var sizeMb = _latest.ZipSize > 0
                ? $" / {_latest.ZipSize / (1024.0 * 1024.0):0.0} MB"
                : "";
            _latestLabel.Text =
                $"最新版: {_latest.TagName}（{_latest.ZipName}{sizeMb}） 公開: {when}";
            _status.Text = "最新版を取得しました。MMD フォルダを指定して「導入 / 更新」を押してください。";
            RefreshInstalledLabel();
        }
        catch (Exception ex)
        {
            _latest = null;
            _latestLabel.Text = "最新版: 取得失敗";
            _status.Text = "エラー: " + ex.Message;
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async Task InstallOrUpdateAsync()
    {
        if (_busy || _latest is null) return;

        var mmd = _mmdPath.Text.Trim();
        if (string.IsNullOrEmpty(mmd) || !Directory.Exists(mmd))
        {
            MessageBox.Show(this, "MMD フォルダを指定してください。", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        if (!InstallService.LooksLikeMmdFolder(mmd))
        {
            var r = MessageBox.Show(this,
                "MikuMikuDance.exe が見つかりません。このフォルダで続行しますか？",
                Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (r != DialogResult.Yes) return;
        }

        var state = InstallService.ReadState(mmd);
        if (state?.InstalledTag is not null &&
            !InstallService.NeedsUpdate(state.InstalledTag, _latest.TagName))
        {
            var r = MessageBox.Show(this,
                $"すでに {_latest.TagName} が導入されています。再インストールしますか？",
                Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (r != DialogResult.Yes) return;
        }

        SetBusy(true);
        SavePath(mmd);
        var zipPath = Path.Combine(Path.GetTempPath(), _latest.ZipName);
        try
        {
            _cts?.Cancel();
            _cts = new CancellationTokenSource();
            var ct = _cts.Token;

            _status.Text = $"ダウンロード中: {_latest.ZipName}";
            var progress = new Progress<double>(p =>
            {
                var v = (int)Math.Clamp(p, 0, 100);
                if (InvokeRequired) BeginInvoke(() => _progress.Value = v);
                else _progress.Value = v;
            });
            await GitHubReleaseClient.DownloadAsync(_latest.ZipUrl, zipPath, progress, ct);

            _status.Text = "インストール中…";
            _progress.Value = 100;
            await Task.Run(() => InstallService.InstallFromZip(zipPath, mmd, _latest.TagName), ct);

            RefreshInstalledLabel();
            _status.Text =
                $"{_latest.TagName} を導入しました。MikuMikuDance.exe を起動し、" +
                "メニュー「VRレンダラー (VR Renderer)」を開いてください。";
            MessageBox.Show(this, _status.Text, Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (OperationCanceledException)
        {
            _status.Text = "キャンセルしました。";
        }
        catch (Exception ex)
        {
            _status.Text = "エラー: " + ex.Message;
            MessageBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            try { if (File.Exists(zipPath)) File.Delete(zipPath); } catch { /* ignore */ }
            SetBusy(false);
        }
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        _cts?.Cancel();
        base.OnFormClosing(e);
    }
}
