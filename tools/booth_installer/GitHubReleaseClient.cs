using System.Net.Http.Headers;
using System.Text.Json;

namespace MmdVrBoothInstaller;

internal sealed class ReleaseInfo
{
    public required string TagName { get; init; }
    public required string Name { get; init; }
    public required string ZipUrl { get; init; }
    public required string ZipName { get; init; }
    public long ZipSize { get; init; }
    public DateTimeOffset? PublishedAt { get; init; }
}

internal static class GitHubReleaseClient
{
    public const string Owner = "coffin299";
    public const string Repo = "MMD-VR-Renderer";
    public static string LatestApiUrl =>
        $"https://api.github.com/repos/{Owner}/{Repo}/releases/latest";
    public static string ReleasesPageUrl =>
        $"https://github.com/{Owner}/{Repo}/releases";

    private static readonly HttpClient Http = CreateClient();

    private static HttpClient CreateClient()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("MMD-VR-Renderer-BoothInstaller/1.0");
        c.DefaultRequestHeaders.Accept.Add(
            new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return c;
    }

    public static async Task<ReleaseInfo> GetLatestAsync(CancellationToken ct)
    {
        using var resp = await Http.GetAsync(LatestApiUrl, ct).ConfigureAwait(false);
        var body = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        if (!resp.IsSuccessStatusCode)
        {
            throw new InvalidOperationException(
                $"GitHub API error {(int)resp.StatusCode}: {TrimBody(body)}");
        }

        using var doc = JsonDocument.Parse(body);
        var root = doc.RootElement;
        var tag = root.GetProperty("tag_name").GetString()
                  ?? throw new InvalidOperationException("tag_name missing");
        var name = root.TryGetProperty("name", out var n) ? n.GetString() ?? tag : tag;
        DateTimeOffset? published = null;
        if (root.TryGetProperty("published_at", out var p) &&
            p.ValueKind == JsonValueKind.String &&
            DateTimeOffset.TryParse(p.GetString(), out var dt))
        {
            published = dt;
        }

        if (!root.TryGetProperty("assets", out var assets) ||
            assets.ValueKind != JsonValueKind.Array)
        {
            throw new InvalidOperationException("Release has no assets.");
        }

        JsonElement? best = null;
        foreach (var a in assets.EnumerateArray())
        {
            var an = a.GetProperty("name").GetString() ?? "";
            if (!an.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) continue;
            if (an.Contains("win64", StringComparison.OrdinalIgnoreCase) ||
                an.Contains("MMD-VR-Renderer", StringComparison.OrdinalIgnoreCase))
            {
                best = a;
                if (an.Contains("win64", StringComparison.OrdinalIgnoreCase)) break;
            }
        }

        if (best is null)
        {
            throw new InvalidOperationException(
                "win64 zip asset not found on latest release. " + ReleasesPageUrl);
        }

        var asset = best.Value;
        var zipName = asset.GetProperty("name").GetString() ?? "release.zip";
        var zipUrl = asset.GetProperty("browser_download_url").GetString()
                     ?? throw new InvalidOperationException("download url missing");
        var size = asset.TryGetProperty("size", out var s) && s.TryGetInt64(out var sz) ? sz : 0;

        return new ReleaseInfo
        {
            TagName = tag,
            Name = name,
            ZipUrl = zipUrl,
            ZipName = zipName,
            ZipSize = size,
            PublishedAt = published
        };
    }

    public static async Task DownloadAsync(
        string url,
        string destPath,
        IProgress<double>? progress,
        CancellationToken ct)
    {
        using var resp = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct)
            .ConfigureAwait(false);
        resp.EnsureSuccessStatusCode();
        var total = resp.Content.Headers.ContentLength ?? -1L;
        await using var src = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
        await using var dst = new FileStream(
            destPath, FileMode.Create, FileAccess.Write, FileShare.None, 81920, true);
        var buffer = new byte[81920];
        long readTotal = 0;
        int n;
        while ((n = await src.ReadAsync(buffer.AsMemory(0, buffer.Length), ct)
                   .ConfigureAwait(false)) > 0)
        {
            await dst.WriteAsync(buffer.AsMemory(0, n), ct).ConfigureAwait(false);
            readTotal += n;
            if (total > 0 && progress is not null)
            {
                progress.Report(100.0 * readTotal / total);
            }
        }
        progress?.Report(100);
    }

    private static string TrimBody(string body) =>
        body.Length <= 200 ? body : body[..200] + "…";
}
