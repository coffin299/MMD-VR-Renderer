using System.IO.Compression;
using System.Text.Json;

namespace MmdVrBoothInstaller;

internal sealed class InstallState
{
    public string? InstalledTag { get; set; }
    public DateTimeOffset? InstalledAt { get; set; }
}

internal static class InstallService
{
    public const string StateFileName = ".mmdvr_install.json";
    public const string VersionMarkerName = ".mmdvr_version";

    public static string StatePath(string mmdDir) => Path.Combine(mmdDir, StateFileName);
    public static string VersionPath(string mmdDir) => Path.Combine(mmdDir, VersionMarkerName);

    public static InstallState? ReadState(string mmdDir)
    {
        try
        {
            var path = StatePath(mmdDir);
            if (File.Exists(path))
            {
                var json = File.ReadAllText(path);
                return JsonSerializer.Deserialize<InstallState>(json);
            }
            var marker = VersionPath(mmdDir);
            if (File.Exists(marker))
            {
                return new InstallState
                {
                    InstalledTag = File.ReadAllText(marker).Trim(),
                    InstalledAt = File.GetLastWriteTimeUtc(marker)
                };
            }
        }
        catch
        {
            // ignore corrupt state
        }
        return null;
    }

    public static void WriteState(string mmdDir, string tag)
    {
        var state = new InstallState
        {
            InstalledTag = tag,
            InstalledAt = DateTimeOffset.UtcNow
        };
        File.WriteAllText(
            StatePath(mmdDir),
            JsonSerializer.Serialize(state, new JsonSerializerOptions { WriteIndented = true }));
        File.WriteAllText(VersionPath(mmdDir), tag + Environment.NewLine);
    }

    public static bool LooksLikeMmdFolder(string dir)
    {
        if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir)) return false;
        return File.Exists(Path.Combine(dir, "MikuMikuDance.exe"));
    }

    /// <summary>
    /// Extract release zip; install proxy as d3d9.dll + named twin d3d9_mmdvr.dll.
    /// </summary>
    public static string InstallFromZip(string zipPath, string mmdDir, string tag)
    {
        if (!Directory.Exists(mmdDir))
            throw new DirectoryNotFoundException($"MMD folder not found: {mmdDir}");
        if (!File.Exists(zipPath))
            throw new FileNotFoundException("Downloaded zip missing", zipPath);

        var extractRoot = Path.Combine(
            Path.GetTempPath(),
            "mmdvr_booth_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(extractRoot);
        try
        {
            ZipFile.ExtractToDirectory(zipPath, extractRoot);
            var plugin = Directory.EnumerateFiles(extractRoot, "d3d9_mmdvr.dll", SearchOption.AllDirectories)
                .FirstOrDefault()
                ?? Directory.EnumerateFiles(extractRoot, "d3d9.dll", SearchOption.AllDirectories)
                    .FirstOrDefault()
                ?? throw new FileNotFoundException("d3d9_mmdvr.dll / d3d9.dll not found inside release zip.");

            InstallDll(plugin, mmdDir);
            WriteState(mmdDir, tag);
            return plugin;
        }
        finally
        {
            try { Directory.Delete(extractRoot, recursive: true); } catch { /* ignore */ }
        }
    }

    public static void InstallDll(string dllPath, string mmdDir)
    {
        var backup = Path.Combine(mmdDir, ".mmdvr_backup");
        Directory.CreateDirectory(backup);

        var mmeD3d9 = Path.Combine(mmdDir, "d3d9.dll");
        var mmeRenamed = Path.Combine(mmdDir, "d3d9_mme.dll");
        var named = Path.Combine(mmdDir, "d3d9_mmdvr.dll");
        var legacy = Path.Combine(mmdDir, "mmdvr_d3d9.dll");
        var marker = Path.Combine(mmdDir, "MMD-VR-Renderer.txt");
        var stamp = DateTime.Now.ToString("yyyyMMdd_HHmmss");

        if (File.Exists(mmeD3d9) && !File.Exists(mmeRenamed))
        {
            File.Copy(mmeD3d9, Path.Combine(backup, $"d3d9_mme_{stamp}.dll"), overwrite: true);
            File.Move(mmeD3d9, mmeRenamed);
        }
        else if (File.Exists(mmeD3d9) && File.Exists(mmeRenamed))
        {
            File.Copy(mmeD3d9, Path.Combine(backup, $"d3d9_proxy_{stamp}.dll"), overwrite: true);
        }

        if (File.Exists(legacy)) File.Delete(legacy);

        File.Copy(dllPath, mmeD3d9, overwrite: true);
        File.Copy(dllPath, named, overwrite: true);
        File.WriteAllText(marker,
            "MMD-VR-Renderer がこのフォルダに導入されています。\n" +
            "  d3d9.dll / d3d9_mmdvr.dll（同じ本体） / d3d9_mme.dll（MME）\n" +
            "Uninstall: tools\\uninstall_from_mmd.ps1 or BOOTH installer.\n");

        if (!File.Exists(mmeRenamed))
        {
            throw new InvalidOperationException(
                "d3d9_mme.dll がありません。先に MME を導入し、" +
                "本来の d3d9.dll が d3d9_mme.dll として残るようにしてください。");
        }
    }

    public static void Uninstall(string mmdDir)
    {
        var mmeRenamed = Path.Combine(mmdDir, "d3d9_mme.dll");
        var proxy = Path.Combine(mmdDir, "d3d9.dll");
        var named = Path.Combine(mmdDir, "d3d9_mmdvr.dll");
        var legacy = Path.Combine(mmdDir, "mmdvr_d3d9.dll");
        var marker = Path.Combine(mmdDir, "MMD-VR-Renderer.txt");

        if (File.Exists(named)) File.Delete(named);
        if (File.Exists(legacy)) File.Delete(legacy);
        if (File.Exists(marker)) File.Delete(marker);
        if (File.Exists(StatePath(mmdDir))) File.Delete(StatePath(mmdDir));
        if (File.Exists(VersionPath(mmdDir))) File.Delete(VersionPath(mmdDir));

        if (File.Exists(mmeRenamed))
        {
            if (File.Exists(proxy)) File.Delete(proxy);
            File.Move(mmeRenamed, proxy);
        }
    }

    public static bool NeedsUpdate(string? installedTag, string latestTag)
    {
        if (string.IsNullOrWhiteSpace(installedTag)) return true;
        return !string.Equals(
            NormalizeTag(installedTag),
            NormalizeTag(latestTag),
            StringComparison.OrdinalIgnoreCase);
    }

    private static string NormalizeTag(string tag) => tag.Trim().TrimStart('v', 'V');
}
