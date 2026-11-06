using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace DriveClub.Launcher;

/// <summary>A choice a mod offers (mod.json "settings"); the player's value goes to dcmod build --settings.</summary>
public sealed class ModSetting
{
    public string Label { get; set; } = "";
    public string Type { get; set; } = "string";
    public JsonElement? Default { get; set; }
    public double? Min { get; set; }
    public double? Max { get; set; }
    public List<string> Choices { get; set; } = [];
}

public sealed class ModPackage
{
    /// <summary>What the mod is for (Car, Track, Livery, Graphics, Physics, Script, Audio, Tour, ...), shown in the list.</summary>
    public string Category { get; set; } = "";
    /// <summary>The id written in mod.json; the settings file is keyed by it.</summary>
    public string ManifestId { get; set; } = "";
    [JsonPropertyName("settings")] public Dictionary<string, ModSetting> SettingDefs { get; set; } = new();
    /// <summary>The player's values for the settings, as JSON text by name.</summary>
    public Dictionary<string, string> Choices { get; set; } = new();
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string Version { get; set; } = "";
    public string Author { get; set; } = "";
    public string Description { get; set; } = "";
    public bool Enabled { get; set; }
    /// <summary>The mod has patches (mod.json "patches"); its source is kept in .dcmod-src and built by dcmod.</summary>
    public bool Patched { get; set; }
    public Dictionary<string, string> Files { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}

public sealed class ModLibraryState
{
    public List<ModPackage> Packages { get; set; } = [];
    public Dictionary<string, string> Deployed { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}

/// <summary>Local loose-file overlays. The first enabled package wins each path.</summary>
public sealed class ModLibrary
{
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true, PropertyNameCaseInsensitive = true };
    private readonly string home, store, stateFile, overlay, transaction;
    public string LibraryDirectory => Path.Combine(store, "packages");
    public string OverlayDirectory => overlay;

    public ModLibrary(string directory)
    {
        home = Path.GetFullPath(directory);
        store = Under(home, "data/mod-manager");
        stateFile = Path.Combine(store, "library.json");
        overlay = Under(home, "mods");
        transaction = Path.Combine(store, "transaction");
    }

    public ModLibraryState Load()
    {
        using var gate = Lock(); Recover(); return Read();
    }

    public ModLibraryState SetEnabled(string id, bool enabled) => Change(s => Find(s, id).Enabled = enabled);
    public ModLibraryState Move(string id, int offset) => Change(s =>
    {
        var package = Find(s, id); int index = s.Packages.IndexOf(package);
        int target = Math.Clamp(index + offset, 0, s.Packages.Count - 1);
        s.Packages.RemoveAt(index); s.Packages.Insert(target, package);
    });
    public ModLibraryState SetChoice(string id, string name, string json) => Change(s =>
    {
        var package = Find(s, id);
        if (!package.SettingDefs.ContainsKey(name)) throw new InvalidDataException("The mod has no setting " + name + ".");
        using (JsonDocument.Parse(json)) { }
        package.Choices[name] = json;
    });
    public ModLibraryState Remove(string id) => Change(s => s.Packages.Remove(Find(s, id)));

    public string PackageDirectory(ModPackage package)
    {
        if (!Guid.TryParseExact(package.Id, "N", out _)) throw new InvalidDataException("Invalid mod ID.");
        return Under(LibraryDirectory, package.Id);
    }

    public ModLibraryState Import(string source)
    {
        using var gate = Lock(); Recover(); var state = Read();
        string temp = Path.Combine(store, "import-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try
        {
            if (Directory.Exists(source)) CopyFolder(source, temp);
            else
            {
                using var archive = ZipFile.OpenRead(source);
                var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var entry in archive.Entries)
                {
                    string name = entry.FullName.Replace('\\', '/').TrimEnd('/');
                    if (name.Length == 0) continue;
                    ValidateRelative(name);
                    // Reject Unix symlinks; Windows reparse attributes are not supported either.
                    if (((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000 || (entry.ExternalAttributes & 0x400) != 0)
                        throw new InvalidDataException("Mod archives cannot contain links.");
                    string destination = Under(temp, name);
                    if (entry.FullName.EndsWith('/') || entry.FullName.EndsWith('\\')) { Directory.CreateDirectory(destination); continue; }
                    if (!seen.Add(name)) throw new InvalidDataException("Duplicate mod path: " + name);
                    Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                    entry.ExtractToFile(destination);
                }
            }
            string content = temp;
            // A ZIP may contain one enclosing folder. Recognized payload containers are optional.
            while (!Directory.EnumerateFiles(content).Any() && Directory.GetDirectories(content) is { Length: 1 } children)
            {
                string next = children[0]; string leaf = Path.GetFileName(next);
                if (leaf.Equals("mods", StringComparison.OrdinalIgnoreCase) || leaf.Equals("app0", StringComparison.OrdinalIgnoreCase) || leaf.Equals("files", StringComparison.OrdinalIgnoreCase)) break;
                // Never guess that a game-relative directory is merely packaging.
                if (Directory.Exists(source) || content != temp || (!File.Exists(Path.Combine(next, "mod.json")) && !new[] { "mods", "app0", "files" }.Any(n => Directory.Exists(Path.Combine(next, n))))) break;
                content = next;
            }
            string manifest = Path.Combine(content, "mod.json");
            var package = File.Exists(manifest) ? JsonSerializer.Deserialize<ModPackage>(File.ReadAllText(manifest), Json) ?? new() : new ModPackage();
            package.Id = Guid.NewGuid().ToString("N"); package.Enabled = false;
            package.Patched = HasPatches(manifest);
            package.ManifestId = ManifestIdOf(manifest) ?? package.Id;
            if (string.IsNullOrWhiteSpace(package.Name)) package.Name = Path.GetFileNameWithoutExtension(source.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            string payload = content;
            foreach (string container in new[] { "files", "mods", "app0" })
                if (Directory.Exists(Path.Combine(content, container)) && (File.Exists(manifest) || !Directory.EnumerateFiles(content).Any()) && Directory.GetDirectories(content).Length == 1)
                { payload = Path.Combine(content, container); break; }
            package.Files = new(StringComparer.OrdinalIgnoreCase);
            // A mod with patches keeps its whole folder for dcmod; only its "files" folder counts as loose game files.
            if (package.Patched) payload = Path.Combine(content, "files");
            foreach (string file in package.Patched && !Directory.Exists(payload) ? [] : Files(payload))
            {
                if (file.Equals(manifest, StringComparison.OrdinalIgnoreCase)) continue;
                string relative = Path.GetRelativePath(payload, file).Replace('\\', '/');
                if (!package.Files.TryAdd(relative, Hash(file))) throw new InvalidDataException("Duplicate mod path: " + relative);
            }
            if (package.Files.Count == 0 && !package.Patched) throw new InvalidDataException("The mod contains no game files.");
            string destinationPackage = PackageDirectory(package);
            Directory.CreateDirectory(destinationPackage);
            foreach (string relative in package.Files.Keys) Copy(Under(payload, relative), Under(destinationPackage, relative));
            if (package.Patched)
                foreach (string file in Files(content)) Copy(file, Under(destinationPackage, SourceFolder + "/" + Path.GetRelativePath(content, file).Replace('\\', '/')));
            state.Packages.Add(package);
            WriteState(state); // Imported mods start disabled; applying them is an explicit action.
            return state;
        }
        finally { Directory.Delete(temp, true); }
    }

    public static Dictionary<string, List<ModPackage>> Conflicts(ModLibraryState state)
    {
        var paths = new Dictionary<string, List<ModPackage>>(StringComparer.OrdinalIgnoreCase);
        foreach (var package in state.Packages.Where(p => p.Enabled))
            foreach (string file in package.Files.Keys)
            {
                if (!paths.TryGetValue(file, out var owners)) paths[file] = owners = [];
                owners.Add(package);
            }
        return paths.Where(p => p.Value.Count > 1).ToDictionary(p => p.Key, p => p.Value, StringComparer.OrdinalIgnoreCase);
    }

    private ModLibraryState Change(Action<ModLibraryState> edit)
    {
        using var gate = Lock(); Recover(); var state = Read(); edit(state); Deploy(state); return state;
    }

    private ModLibraryState Read()
    {
        var state = File.Exists(stateFile) ? JsonSerializer.Deserialize<ModLibraryState>(File.ReadAllText(stateFile), Json) ?? throw new InvalidDataException("Invalid mod library.") : new();
        state.Deployed = new(state.Deployed, StringComparer.OrdinalIgnoreCase);
        var ids = new HashSet<string>();
        foreach (var package in state.Packages)
        {
            _ = PackageDirectory(package);
            if (!ids.Add(package.Id)) throw new InvalidDataException("Duplicate mod ID.");
            package.Files = new(package.Files, StringComparer.OrdinalIgnoreCase);
            foreach (var file in package.Files) { ValidateRelative(file.Key); ValidateHash(file.Value); }
        }
        foreach (var file in state.Deployed) { ValidateRelative(file.Key); ValidateHash(file.Value); }
        return state;
    }

    private const string SourceFolder = ".dcmod-src";
    private const string GeneratedId = "00000000000000000000000000000001";

    private static bool HasPatches(string manifest)
    {
        if (!File.Exists(manifest)) return false;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(manifest), new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
            return doc.RootElement.TryGetProperty("patches", out var patches) && patches.ValueKind == JsonValueKind.Array && patches.GetArrayLength() > 0;
        }
        catch (JsonException) { return false; }
    }

    private static string? ManifestIdOf(string manifest)
    {
        try
        {
            if (!File.Exists(manifest)) return null;
            using var doc = JsonDocument.Parse(File.ReadAllText(manifest), new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
            return doc.RootElement.TryGetProperty("id", out var id) && id.ValueKind == JsonValueKind.String ? id.GetString() : null;
        }
        catch (JsonException) { return null; }
    }

    /// <summary>With patched mods every enabled mod goes through dcmod, so patches see the files of the mods below them.</summary>
    private Dictionary<string, (ModPackage Package, string Hash)> Generate(ModLibraryState next)
    {
        string exe = Environment.GetEnvironmentVariable("DCMOD_EXE") ?? Path.Combine(AppContext.BaseDirectory, "dcmod.exe");
        if (!File.Exists(exe)) throw new IOException("dcmod.exe is missing, so mods with patches cannot be applied.");
        var generated = new ModPackage { Id = GeneratedId, Name = "Generated", Enabled = true };
        string output = PackageDirectory(generated);
        if (Directory.Exists(output)) Directory.Delete(output, true);
        var start = new ProcessStartInfo(exe) { WorkingDirectory = home, RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add("mod"); start.ArgumentList.Add("build");
        foreach (var package in next.Packages.Where(p => p.Enabled).Reverse()) // lowest priority first: dcmod lets later mods win
        {
            string folder = PackageDirectory(package);
            start.ArgumentList.Add(package.Id + "=" + (package.Patched ? Under(folder, SourceFolder) : folder));
        }
        start.ArgumentList.Add("-o"); start.ArgumentList.Add(output);
        var chosen = new Dictionary<string, Dictionary<string, JsonElement>>();
        foreach (var package in next.Packages.Where(p => p.Enabled && p.Choices.Count > 0))
            chosen[package.ManifestId.Length > 0 ? package.ManifestId : package.Id] = package.Choices.ToDictionary(c => c.Key, c => { using var doc = JsonDocument.Parse(c.Value); return doc.RootElement.Clone(); });
        if (chosen.Count > 0)
        {
            Directory.CreateDirectory(store);
            string settingsFile = Path.Combine(store, "settings.json");
            File.WriteAllText(settingsFile, JsonSerializer.Serialize(chosen));
            start.ArgumentList.Add("--settings"); start.ArgumentList.Add(settingsFile);
        }
        using var process = Process.Start(start) ?? throw new IOException("dcmod could not be started.");
        var errors = process.StandardError.ReadToEndAsync();
        string text = process.StandardOutput.ReadToEnd();
        process.WaitForExit();
        text += errors.Result;
        Directory.CreateDirectory(store);
        File.WriteAllText(Path.Combine(store, "last-build.txt"), text);
        if (process.ExitCode != 0) throw new IOException("Applying the mods failed: " + text.Trim());
        File.Delete(Path.Combine(output, "dcmod-report.json"));
        var result = new Dictionary<string, (ModPackage Package, string Hash)>(StringComparer.OrdinalIgnoreCase);
        if (Directory.Exists(output))
            foreach (string file in Files(output))
            {
                string relative = Path.GetRelativePath(output, file).Replace('\\', '/');
                ValidateRelative(relative);
                string hash = Hash(file);
                generated.Files[relative] = hash;
                result[relative] = (generated, hash);
            }
        return result;
    }

    private sealed class Operation
    {
        public string Path { get; set; } = "";
        public string? OldHash { get; set; }
        public string? NewHash { get; set; }
    }

    private void Deploy(ModLibraryState next)
    {
        var old = Read();
        var winners = new Dictionary<string, (ModPackage Package, string Hash)>(StringComparer.OrdinalIgnoreCase);
        if (next.Packages.Any(p => p.Enabled && p.Patched)) winners = Generate(next);
        else
            foreach (var package in next.Packages.Where(p => p.Enabled).Reverse())
                foreach (var file in package.Files) winners[file.Key] = (package, file.Value);
        var operations = new List<Operation>();
        foreach (string relative in old.Deployed.Keys.Union(winners.Keys, StringComparer.OrdinalIgnoreCase))
        {
            string target = Under(overlay, relative);
            old.Deployed.TryGetValue(relative, out string? previous);
            string? desired = winners.TryGetValue(relative, out var winner) ? winner.Hash : null;
            string? actual = File.Exists(target) ? Hash(target) : null;
            if (Directory.Exists(target)) throw new IOException("A folder blocks mod file: " + relative);
            if (actual != null && previous == null) throw new IOException("An unmanaged file already exists in mods: " + relative + ". Move it out before enabling this mod.");
            if (actual != null && actual != previous) throw new IOException("A deployed mod file was changed outside the manager: " + relative + ". Restore or move it out first.");
            if (actual == desired) continue;
            if (desired != null && Hash(Under(PackageDirectory(winner.Package), relative)) != desired)
                throw new IOException("A library file is missing or changed: " + relative + ". Re-import the mod.");
            operations.Add(new() { Path = relative, OldHash = actual, NewHash = desired });
        }
        next.Deployed = winners.ToDictionary(p => p.Key, p => p.Value.Hash, StringComparer.OrdinalIgnoreCase);
        Directory.CreateDirectory(transaction);
        // Prepare all sources and backups before touching the live overlay.
        try
        {
            if (File.Exists(stateFile)) File.Copy(stateFile, Path.Combine(transaction, "old-state.json"));
            foreach (var op in operations)
            {
                if (op.OldHash != null) Copy(Under(overlay, op.Path), Under(transaction, "old/" + op.Path));
                if (op.NewHash != null) Copy(Under(PackageDirectory(winners[op.Path].Package), op.Path), Under(transaction, "new/" + op.Path));
            }
            File.WriteAllText(Path.Combine(transaction, "prepared.json"), JsonSerializer.Serialize(operations, Json));
            File.Move(Path.Combine(transaction, "prepared.json"), Path.Combine(transaction, "journal.json"));
            foreach (var op in operations)
            {
                string target = Under(overlay, op.Path);
                // Re-check immediately before changing a file, including changes during staging.
                string? actual = File.Exists(target) ? Hash(target) : null;
                if (actual != op.OldHash) throw new IOException("A mod file changed while applying: " + op.Path);
                if (op.NewHash == null) File.Delete(target);
                else Copy(Under(transaction, "new/" + op.Path), target);
            }
            WriteState(next);
            File.Delete(Path.Combine(transaction, "journal.json")); // Commit point; recovery no longer rolls back.
        }
        catch
        {
            Recover(); throw;
        }
        finally
        {
            if (Directory.Exists(transaction) && !File.Exists(Path.Combine(transaction, "journal.json"))) Directory.Delete(transaction, true);
        }
        // Keep removed package payloads in the library as recoverable files; never delete user content.
    }

    private void Recover()
    {
        string journal = Under(store, "transaction/journal.json");
        if (!File.Exists(journal))
        {
            if (Directory.Exists(transaction)) Directory.Delete(transaction, true);
            return;
        }
        var operations = JsonSerializer.Deserialize<List<Operation>>(File.ReadAllText(journal), Json) ?? throw new InvalidDataException("Invalid mod recovery journal.");
        foreach (var op in operations.AsEnumerable().Reverse())
        {
            ValidateRelative(op.Path);
            string target = Under(overlay, op.Path);
            string? actual = File.Exists(target) ? Hash(target) : null;
            if (actual == op.OldHash) continue;
            if (actual != op.NewHash && actual != null) throw new IOException("Mod recovery paused to preserve an externally changed file: " + op.Path);
            if (op.OldHash == null) File.Delete(target);
            else
            {
                string backup = Under(transaction, "old/" + op.Path);
                if (Hash(backup) != op.OldHash) throw new IOException("A mod recovery backup is damaged: " + op.Path);
                Copy(backup, target);
            }
        }
        string oldState = Path.Combine(transaction, "old-state.json");
        if (File.Exists(oldState)) File.Copy(oldState, stateFile, true); else File.Delete(stateFile);
        File.Delete(journal); Directory.Delete(transaction, true);
    }

    private FileStream Lock()
    {
        _ = Under(home, "data/mod-manager/manager.lock");
        Directory.CreateDirectory(store);
        try { return new FileStream(Path.Combine(store, "manager.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException ex) { throw new IOException("The mod library is busy in another launcher window.", ex); }
    }
    private void WriteState(ModLibraryState state)
    {
        string temporary = stateFile + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(state, Json));
        File.Move(temporary, stateFile, true);
    }
    private static ModPackage Find(ModLibraryState state, string id) => state.Packages.SingleOrDefault(p => p.Id == id) ?? throw new InvalidDataException("The mod was removed. Refresh the library.");
    private static void ValidateHash(string hash)
    {
        if (hash.Length != 64 || !hash.All(Uri.IsHexDigit)) throw new InvalidDataException("Invalid mod checksum.");
    }
    private static string Hash(string path)
    {
        using var stream = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(stream));
    }
    private static void Copy(string source, string destination)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        // Same-directory rename avoids exposing a partly written game file.
        string temporary = destination + ".mod-manager-" + Guid.NewGuid().ToString("N");
        try { File.Copy(source, temporary); File.Move(temporary, destination, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    private static IEnumerable<string> Files(string directory)
    {
        CheckLink(directory);
        foreach (string entry in Directory.EnumerateFileSystemEntries(directory))
        {
            CheckLink(entry);
            if (Directory.Exists(entry)) { foreach (string file in Files(entry)) yield return file; }
            else yield return entry;
        }
    }
    private static void CopyFolder(string source, string destination)
    {
        source = Path.GetFullPath(source);
        if (destination.StartsWith(source.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Choose a mod folder outside the manager's library.");
        foreach (string file in Files(source)) Copy(file, Under(destination, Path.GetRelativePath(source, file).Replace('\\', '/')));
    }
    private static void ValidateRelative(string relative)
    {
        string[] parts = relative.Replace('\\', '/').Split('/');
        foreach (string part in parts)
        {
            string stem = part.Split('.')[0].ToUpperInvariant();
            if (part.Length == 0 || part is "." or ".." || part.EndsWith(' ') || part.EndsWith('.') || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || part.Any(c => c < 32) || stem is "CON" or "PRN" or "AUX" or "NUL" || (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) && char.IsDigit(stem[3])))
                throw new InvalidDataException("Unsafe mod path: " + relative);
        }
    }
    private static string Under(string directory, string relative)
    {
        ValidateRelative(relative); directory = Path.GetFullPath(directory);
        string destination = Path.GetFullPath(Path.Combine(directory, relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!destination.StartsWith(directory.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Mod path escapes its folder.");
        // Check all ancestors as well as the leaf, including a linked mods/data directory.
        for (string? path = destination; path != null; path = Path.GetDirectoryName(path)) CheckLink(path);
        return destination;
    }
    private static void CheckLink(string path)
    {
        if (File.Exists(path) || Directory.Exists(path))
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("Linked folders or files are not supported: " + path);
    }
}
