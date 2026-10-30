using DriveClub.Launcher;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

string root = Path.Combine(AppContext.BaseDirectory, "fixtures-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
int count = 0;
void Check(bool condition, string name) { if (!condition) throw new Exception(name); Console.WriteLine("PASS " + name); count++; }
void Fails(Action action, string name) { try { action(); } catch (Exception ex) when (ex is IOException or InvalidDataException or InvalidOperationException or ArgumentException) { Check(true, name); return; } throw new Exception("Expected rejection: " + name); }
string Folder(string name, params (string File, string Text)[] files)
{
    string directory = Path.Combine(root, name); Directory.CreateDirectory(directory);
    foreach (var file in files) { string destination = Path.Combine(directory, file.File); Directory.CreateDirectory(Path.GetDirectoryName(destination)!); File.WriteAllText(destination, file.Text); }
    return directory;
}
string Zip(string name, params (string File, string Text)[] files)
{
    string path = Path.Combine(root, name + ".zip");
    using var archive = ZipFile.Open(path, ZipArchiveMode.Create);
    foreach (var file in files) { using var writer = new StreamWriter(archive.CreateEntry(file.File).Open()); writer.Write(file.Text); }
    return path;
}
string home = Folder("launcher"); var library = new ModLibrary(home);
var a = library.Import(Folder("Scenery", ("data/scenery.rpk", "first"), ("mod.json", "{\"name\":\"Scenery\",\"version\":\"1.0\"}"))).Packages.Single();
Check(!a.Enabled && a.Files.Keys.Single() == "data/scenery.rpk", "folder import preserves game-relative paths and starts disabled");
Check(!File.Exists(Path.Combine(home, "mods/data/scenery.rpk")), "import does not activate files");
library.SetEnabled(a.Id, true);
Check(File.ReadAllText(Path.Combine(home, "mods/data/scenery.rpk")) == "first", "enable deploys mod");
Check(new ModLibrary(home).Load().Packages.Single().Enabled, "enabled state persists across restarts");
var b = library.Import(Zip("other", ("data/scenery.rpk", "second"), ("data/cars.rpk", "cars"))).Packages.Last();
Check(b.Files.ContainsKey("data/scenery.rpk"), "ZIP with single game directory keeps that directory");
var state = library.SetEnabled(b.Id, true);
Check(File.ReadAllText(Path.Combine(home, "mods/data/scenery.rpk")) == "first" && ModLibrary.Conflicts(state).Count == 1, "higher mod wins and reports conflict");
library.Move(b.Id, -1);
Check(File.ReadAllText(Path.Combine(home, "mods/data/scenery.rpk")) == "second", "priority reorder updates winner");
library.SetEnabled(b.Id, false);
Check(File.ReadAllText(Path.Combine(home, "mods/data/scenery.rpk")) == "first" && !File.Exists(Path.Combine(home, "mods/data/cars.rpk")), "disable restores lower winner and removes only its exclusive files");
File.WriteAllText(Path.Combine(home, "mods/manual.txt"), "manual");
library.Remove(a.Id);
Check(File.ReadAllText(Path.Combine(home, "mods/manual.txt")) == "manual" && !File.Exists(Path.Combine(home, "mods/data/scenery.rpk")), "remove enabled mod preserves unmanaged files");
var c = library.Import(Zip("collision", ("manual.txt", "overwrite"))).Packages.Last();
Fails(() => library.SetEnabled(c.Id, true), "unmanaged collision rejected");
Check(!library.Load().Packages.Single(p => p.Id == c.Id).Enabled && File.ReadAllText(Path.Combine(home, "mods/manual.txt")) == "manual", "failed activation leaves state and manual files unchanged");
library.SetEnabled(b.Id, true); File.WriteAllText(Path.Combine(home, "mods/data/cars.rpk"), "external edit");
Fails(() => library.SetEnabled(b.Id, false), "external edit to managed file protected");
File.WriteAllText(Path.Combine(home, "mods/data/cars.rpk"), "cars"); library.SetEnabled(b.Id, false);
Fails(() => library.Import(Zip("traversal", ("../escape.txt", "bad"))), "ZIP traversal rejected");
Check(!File.Exists(Path.Combine(home, "data/mod-manager/escape.txt")), "traversal writes nothing outside staging");
Fails(() => library.Import(Zip("device", ("CON.txt", "bad"))), "Windows device filename rejected");
Fails(() => library.Import(Zip("duplicate", ("A.txt", "one"), ("a.txt", "two"))), "case-insensitive duplicate rejected");
string symlinkZip = Path.Combine(root, "symlink.zip");
using (var archive = ZipFile.Open(symlinkZip, ZipArchiveMode.Create))
{
    var entry = archive.CreateEntry("linked-file"); entry.ExternalAttributes = unchecked((int)0xA1FF0000);
    using var writer = new StreamWriter(entry.Open()); writer.Write("outside");
}
Fails(() => library.Import(symlinkZip), "ZIP symlink rejected");
var packaged = library.Import(Zip("packaged", ("Bundle/mod.json", "{\"name\":\"Wrapped\",\"author\":\"Author\"}"), ("Bundle/files/data/example.rpk", "wrapped"))).Packages.Last();
Check(packaged.Name == "Wrapped" && packaged.Author == "Author" && packaged.Files.Keys.Single() == "data/example.rpk", "manifest and explicit wrapper supported");
// A blocked second destination forces rollback after the first operation has succeeded.
var rollback = library.Import(Folder("rollback", ("a.txt", "new a"), ("z/b.txt", "new b"))).Packages.Last();
File.WriteAllText(Path.Combine(home, "mods/z"), "blocks directory");
Fails(() => library.SetEnabled(rollback.Id, true), "I/O failure while applying rejected");
Check(!File.Exists(Path.Combine(home, "mods/a.txt")) && !library.Load().Packages.Single(p => p.Id == rollback.Id).Enabled && File.ReadAllText(Path.Combine(home, "mods/z")) == "blocks directory", "partial deployment rolls back without removing blocking user file");
// Recovery after an interrupted deployment (new file installed, state not committed).
string transaction = Path.Combine(home, "data/mod-manager/transaction"); Directory.CreateDirectory(transaction);
File.Copy(Path.Combine(home, "data/mod-manager/library.json"), Path.Combine(transaction, "old-state.json"));
File.WriteAllText(Path.Combine(home, "mods/interrupted.txt"), "partial");
string hash = Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes("partial")));
File.WriteAllText(Path.Combine(transaction, "journal.json"), JsonSerializer.Serialize(new[] { new { Path = "interrupted.txt", OldHash = (string?)null, NewHash = hash } }));
new ModLibrary(home).Load(); Check(!File.Exists(Path.Combine(home, "mods/interrupted.txt")) && !Directory.Exists(transaction), "restart recovers interrupted deployment");
string link = Path.Combine(root, "linked-mod");
try
{
    Directory.CreateSymbolicLink(link, Path.Combine(root, "Scenery"));
    Fails(() => library.Import(link), "symlink import rejected");
}
catch (Exception ex) when (ex is UnauthorizedAccessException || (ex is IOException && (ex.HResult & 0xffff) == 1314)) { Console.WriteLine("SKIP folder symlink fixture requires OS privilege; ZIP symlink check passed"); }
Console.WriteLine($"{count} checks passed. Fixtures: {root}");
if (args.Length == 2 && args[0] == "--ui-fixture")
{
    var ui = new ModLibrary(args[1]);
    var first = ui.Import(Folder("ui-one", ("mod.json", "{\"name\":\"Scenery detail example\",\"version\":\"1.0\",\"author\":\"Test fixture\",\"description\":\"Sample data for interface verification only. These are text fixtures, not playable mods.\"}"), ("data/scenery.rpk", "example one"))).Packages.Last();
    ui.SetEnabled(first.Id, true);
    var second = ui.Import(Folder("ui-two", ("mod.json", "{\"name\":\"Alternative scenery with a long descriptive name\",\"version\":\"2.0\"}"), ("data/scenery.rpk", "example two"), ("data/textures.rpk", "example"))).Packages.Last();
    ui.SetEnabled(second.Id, true);
    ui.Import(Folder("ui-three", ("mod.json", "{\"name\":\"Disabled texture example\"}"), ("data/other.rpk", "example")));
    Console.WriteLine("UI test fixtures created in " + args[1]);
}
