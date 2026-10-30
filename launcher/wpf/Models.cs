using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace DriveClub.Launcher;
public sealed class Setting : INotifyPropertyChanged
{
    public int Id { get; set; }
    public string Label { get; set; } = "";
    public string Hint { get; set; } = "";
    public string Details { get; set; } = "";
    public string Kind { get; set; } = "";
    public string Tag { get; set; } = "";
    public int Minimum { get; set; }
    public int Maximum { get; set; }
    public string Unit { get; set; } = "";
    public bool Hidden { get; set; }
    public bool Enabled { get; set; } = true;
    public List<string> Options { get; set; } = [];
    private int selected;
    public int Selected { get => selected; set { if (selected == value) return; selected = value; Notify(); Notify(nameof(IsOn)); } }
    private string text = "";
    public string Text { get => text; set { if (text == value) return; text = value; Notify(); } }
    [JsonIgnore] public bool IsOn { get => Selected != 0; set => Selected = value ? 1 : 0; }
    public event PropertyChangedEventHandler? PropertyChanged;
    private void Notify([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new(name));
}
public sealed class CardModel
{
    public string Title { get; set; } = "";
    public int Page { get; set; }
    public int Subpage { get; set; }
    public List<int> Controls { get; set; } = [];
    public string Note { get; set; } = "";
}
public sealed class LauncherState
{
    public int Version { get; set; }
    public int GameProcessId { get; set; }
    public int ChangedId { get; set; } = -1;
    public int ResetPage { get; set; } = -2;
    public int ResetSubpage { get; set; } = -1;
    public List<Setting> Controls { get; set; } = [];
    public List<CardModel> Cards { get; set; } = [];
    public string UpdatePath { get; set; } = "";
    public string BasePath { get; set; } = "";
    public bool FoldersOk { get; set; }
    public string FolderProblem { get; set; } = "";
    public string HardwareProblem { get; set; } = "";
    public bool RuntimeAvailable { get; set; }
    public bool StartupSignalSupported { get; set; }
    public List<int> QuickControls { get; set; } = [];
    public int PresetId { get; set; }
    public string Error { get; set; } = "";
    public string BenchmarkFps { get; set; } = "";
    public string BenchmarkLows { get; set; } = "";
    public string BenchmarkAbout { get; set; } = "";
    public string BenchmarkRanSummary { get; set; } = "";
    public List<string> BenchmarkRanDetails { get; set; } = [];
    public string Message { get; set; } = "";
    public bool CaptureCancelled { get; set; }
    public bool CaptureClear { get; set; }
    public string InstalledVersion { get; set; } = "";
    public string UpdateVersion { get; set; } = "";
    public string UpdateNote { get; set; } = "";
    public uint ShellProcessId { get; set; }
    public bool RestartRequired { get; set; }
}
public sealed class Backend
{
    private readonly SemaphoreSlim gate = new(1, 1);
    public static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true, PropertyNameCaseInsensitive = true };
    public async Task<LauncherState> Call(string action, LauncherState? request = null, IProgress<(int Percent, string Text)>? progress = null, string? launchReadyEvent = null, IProgress<int>? gameStarted = null, Func<string?>? captureCommand = null)
    {
        await gate.WaitAsync();
        string folder = Path.Combine(AppContext.BaseDirectory, "data", "ui");
        Directory.CreateDirectory(folder);
        string id = Guid.NewGuid().ToString("N"), input = Path.Combine(folder, id + ".request.json"), output = Path.Combine(folder, id + ".response.json");
        try
        {
            var start = new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "DriveClub.Backend.exe"))
            { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = AppContext.BaseDirectory };
            // Scope the event name to this backend/game process tree, never the user's environment.
            if (launchReadyEvent != null) start.Environment["BB_LAUNCH_READY_EVENT"] = launchReadyEvent;
            start.ArgumentList.Add("--ui-" + action);
            if (request != null) { await File.WriteAllTextAsync(input, JsonSerializer.Serialize(request, JsonOptions)); start.ArgumentList.Add(input); }
            start.ArgumentList.Add(output);
            using var process = Process.Start(start) ?? throw new IOException("The native backend could not be started.");
            var completion = process.WaitForExitAsync();
            int reportedGameId = 0;
            string? sentCommand = null;
            while (!completion.IsCompleted)
            {
                string? command = captureCommand?.Invoke();
                if (command is "cancel" or "clear" && command != sentCommand)
                {
                    await File.WriteAllTextAsync(output + ".control", command + "\n");
                    sentCommand = command;
                }
                if (progress != null && File.Exists(output + ".progress"))
                {
                    try
                    {
                        using var json = JsonDocument.Parse(await File.ReadAllTextAsync(output + ".progress"));
                        progress.Report((json.RootElement.GetProperty("Percent").GetInt32(), json.RootElement.GetProperty("Text").GetString() ?? ""));
                        if (json.RootElement.TryGetProperty("GameProcessId", out var pid) && pid.GetInt32() > 0 && pid.GetInt32() != reportedGameId)
                        { reportedGameId = pid.GetInt32(); gameStarted?.Report(reportedGameId); }
                    }
                    catch (IOException) { } catch (JsonException) { }
                }
                await Task.WhenAny(completion, Task.Delay(100));
            }
            await completion;
            if (!File.Exists(output)) throw new IOException($"The backend stopped without a response (exit {process.ExitCode}).");
            var response = JsonSerializer.Deserialize<LauncherState>(await File.ReadAllTextAsync(output), JsonOptions) ?? throw new IOException("Empty backend response.");
            if (!string.IsNullOrWhiteSpace(response.Error)) throw new InvalidOperationException(response.Error);
            return response;
        }
        finally
        {
            foreach (var file in new[] { input, output, output + ".progress", output + ".control" }) try { File.Delete(file); } catch (IOException) { }
            gate.Release();
        }
    }
}
