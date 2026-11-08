using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;

namespace DriveClub.Launcher;

// FFB Engineer: the wheel type, which sets every force feedback setting to what suits that kind of wheelbase (the
// backend's ApplyWheelType), then one change for each thing a player may feel, each saying what it changes. Each change
// is written at once, as a change on the page is, so a running game and Live FFB Tuner follow it; Reset to defaults
// sets the wheel type again. The native window has a shorter version (OpenFfbGuide in driveclub_launcher.cpp).
internal sealed class FfbGuideDialog : Window
{
    private readonly MainWindow owner;
    private readonly StackPanel body = new() { Margin = new Thickness(24) };
    private readonly string detectedName;
    private readonly int detectedType;
    private static readonly string[] KindNames = ["direct drive", "belt", "gear"];
    // What each wheel type sets (the backend's kWheelKind, with gain 70 for all)
    private static readonly string[] KindSets =
    [
        "Sets: Gain 70 · Detail 100 · Damper 0 · Countersteer assist off",
        "Sets: Gain 70 · Detail 100 · Damper 0 · Countersteer assist on · Power steering on",
        "Sets: Gain 70 · Detail 60 · Damper 0 · Minimum force 5 · Countersteer assist on · Power steering on",
    ];

    // What a player feels, and the change that helps: a step on one setting, within its range. Damper's
    // "heavy and slow" lowers the gain once the damper is already at 0.
    private sealed record Fix(string Feel, string Key, int Step, int Min, int Max, string? Fallback = null, int FallbackStep = 0);
    private static readonly Fix[] Fixes =
    [
        new("Light or vague around the centre", "wheel_ffb_physics_damper", 5, 0, 30),
        new("Wobbles or swings past the centre when you let go", "wheel_ffb_physics_damper", 5, 0, 30),
        new("Heavy and slow to turn", "wheel_ffb_physics_damper", -5, 0, 30, "wheel_ffb_physics_gain", -10),
        new("Too strong, or goes flat in fast corners", "wheel_ffb_physics_gain", -10, 20, 300),
        new("Too weak overall", "wheel_ffb_physics_gain", 10, 20, 300),
        new("Grainy, buzzy or rattly", "wheel_ffb_physics_detail", -15, 0, 200),
        new("Dull: you can't feel the road or the grip", "wheel_ffb_physics_detail", 15, 0, 200),
        new("Kicks too hard in slides or crashes", "wheel_ffb_physics_max_force", -10, 50, 100),
        new("Hard to catch slides", "wheel_ffb_physics_countersteer", 1, 0, 1),
    ];

    internal FfbGuideDialog(MainWindow owner, string detectedName, int detectedType)
    {
        this.owner = owner; this.detectedName = detectedName; this.detectedType = detectedType;
        Owner = owner; Title = "FFB Engineer"; Width = 640; MinWidth = 420;
        MaxWidth = Math.Max(420, SystemParameters.WorkArea.Width - 32);
        SizeToContent = SizeToContent.Height; ResizeMode = ResizeMode.NoResize;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = new SolidColorBrush(Color.FromRgb(0x17, 0x17, 0x19));
        Foreground = new SolidColorBrush(Color.FromRgb(0xE8, 0xEA, 0xED));
        SetResourceReference(FontFamilyProperty, "BodyFont"); FontSize = 14;
        Content = new ScrollViewer { Content = body, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, MaxHeight = SystemParameters.WorkArea.Height - 80 };
        ShowWheelStep();
    }

    private TextBlock Text(string text, double size = 14, bool bold = false, string color = "#E8EAED", double after = 12) =>
        new() { Text = text, FontSize = size, FontWeight = bold ? FontWeights.SemiBold : FontWeights.Normal, TextWrapping = TextWrapping.Wrap,
                Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString(color)), Margin = new Thickness(0, 0, 0, after) };

    private Button Choice(string title, string detail, Func<Task> click)
    {
        // the full width of the button, so the text starts at its left whatever its length (the button's template
        // centres its content)
        var content = new StackPanel { Width = detail.Length > 0 ? 560 : double.NaN };
        content.Children.Add(Text(title, 15, true, after: detail.Length > 0 ? 2 : 0));
        if (detail.Length > 0) content.Children.Add(Text(detail, 13, color: "#8B93A1", after: 0));
        var button = new Button { Content = content, HorizontalContentAlignment = HorizontalAlignment.Left, Padding = new Thickness(14, 10, 14, 10), Margin = new Thickness(0, 0, 0, 8) };
        AutomationProperties.SetName(button, title);
        button.Click += async (_, _) => { if (!button.IsEnabled) return; button.IsEnabled = false; try { await click(); } finally { button.IsEnabled = true; } };
        return button;
    }

    private Button Plain(string text, Action click, bool cancel = false)
    {
        var button = new Button { Content = text, Padding = new Thickness(16, 6, 16, 6), Margin = new Thickness(12, 0, 0, 0), IsCancel = cancel };
        button.Click += (_, _) => click();
        return button;
    }

    private void ShowWheelStep()
    {
        body.Children.Clear();
        body.Children.Add(Text("STEP 1 OF 2 · YOUR WHEEL", 12, true, "#8B93A1", 6));
        body.Children.Add(Text("What kind of wheel do you have?", 20, true));
        string found = detectedType is >= 0 and <= 2
            ? $"The game found {(detectedName.Length > 0 ? "“" + detectedName + "”" : "your wheel")}: {KindNames[detectedType]}. "
            : "";
        body.Children.Add(Text(found + "Each kind starts from settings that suit its motor. Picking one resets all your force feedback settings to suit it; each car's own FFB gain stays.",
                               14, color: "#B8BEC8", after: 16));
        body.Children.Add(Choice("Direct drive", "The motor turns the steering shaft directly: Fanatec DD and CSL DD, Moza, Simagic, Simucube, Logitech G PRO, Thrustmaster T818...\n" + KindSets[0], () => PickWheel(1)));
        body.Children.Add(Choice("Belt", "Thrustmaster T300, TX, T-GT or TS-PC, Fanatec CSL Elite or ClubSport V2...\n" + KindSets[1], () => PickWheel(2)));
        body.Children.Add(Choice("Gear", "Logitech G29, G920 or G923, Thrustmaster T150, TMX or T248...\n" + KindSets[2], () => PickWheel(3)));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 8, 0, 0) };
        actions.Children.Add(Plain("Skip, keep my settings", () => ShowFeelStep(null)));
        actions.Children.Add(Plain("Close", Close, cancel: true));
        body.Children.Add(actions);
    }

    private string Value(string key) => owner.FfbSetting(key) is { } s ? (s.Kind == "Toggle" ? (s.IsOn ? "on" : "off") : s.Selected.ToString()) : "?";

    private string SetFor(string kind) =>
        $"Set for a {kind} wheel: Gain {Value("wheel_ffb_physics_gain")} · Detail {Value("wheel_ffb_physics_detail")} · " +
        $"Damper {Value("wheel_ffb_physics_damper")} · Max force {Value("wheel_ffb_physics_max_force")} · Countersteer assist {Value("wheel_ffb_physics_countersteer")}";

    private async Task PickWheel(int type)
    {
        if (!await owner.ChangeFfbSetting("wheel_ffb_wheel_type", type)) { ShowFeelStep("Couldn't save the wheel type. The launcher's status line says why."); return; }
        ShowFeelStep(SetFor(KindNames[type - 1]));
    }

    // Asked first: every force feedback setting back to what suits the wheel type in use (Auto: the kind the game found),
    // and each car's own FFB gain back to 100%.
    private void ConfirmReset()
    {
        body.Children.Clear();
        body.Children.Add(Text("RESET TO DEFAULTS", 12, true, "#8B93A1", 6));
        body.Children.Add(Text("Reset all force feedback settings?", 20, true));
        body.Children.Add(Text("Every setting goes back to what suits your wheel, and every car's own FFB gain goes back to 100%. " +
                               "Your wheel setup (buttons, pedals, shifter) stays as it is.", 14, color: "#B8BEC8", after: 16));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        actions.Children.Add(Plain("Reset settings", async () => await ResetDefaults()));
        actions.Children.Add(Plain("Cancel", () => ShowFeelStep(null), cancel: true));
        body.Children.Add(actions);
    }

    private async Task ResetDefaults()
    {
        if (!await owner.ResetFfb()) { ShowFeelStep("Couldn't reset the settings. The launcher's status line says why."); return; }
        int type = owner.FfbSetting("wheel_ffb_wheel_type")?.Selected ?? 0;
        int kind = type is >= 1 and <= 3 ? type - 1 : detectedType is >= 0 and <= 2 ? detectedType : 1;
        ShowFeelStep("Reset to defaults, and every car back to 100%. " + SetFor(KindNames[kind]));
    }

    // What a pick changes, as "Damper +5"
    private string Does(Fix fix)
    {
        static string Step(int s) => s > 0 ? $"+{s}" : $"−{-s}";
        if (owner.FfbSetting(fix.Key) is not { } setting) return "";
        if (setting.Kind == "Toggle") return $"{setting.Label}: {(fix.Step > 0 ? "on" : "off")}";
        string does = $"{setting.Label} {Step(fix.Step)}";
        if (fix.Fallback != null && owner.FfbSetting(fix.Fallback) is { } other)
            does += $" (or {other.Label} {Step(fix.FallbackStep)} if {setting.Label} is already {fix.Min})";
        return does;
    }

    private void ShowFeelStep(string? done)
    {
        body.Children.Clear();
        body.Children.Add(Text("STEP 2 OF 2 · HOW DOES IT FEEL?", 12, true, "#8B93A1", 6));
        body.Children.Add(Text("Drive a few laps, then pick what you feel", 20, true));
        if (done != null) body.Children.Add(Text(done, 14, color: "#5AC878"));
        body.Children.Add(Text("Each pick makes the change shown under it, and the game uses it within a second. Not enough? Pick it again. " +
                               "Too much? Pick the opposite, or reset to defaults.", 14, color: "#B8BEC8", after: 16));
        var result = Text("", 14, true, "#5AC878");
        foreach (var fix in Fixes) body.Children.Add(Choice(fix.Feel, Does(fix), async () => { result.Text = await Apply(fix); }));
        body.Children.Add(result);
        body.Children.Add(Text("One car stronger or weaker than the rest? Use the FFB gain + and − buttons on the Wheel page. They change only the car you're driving, " +
                               "5% per press, and each car remembers its own.", 13, color: "#8B93A1", after: 8));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        actions.Children.Add(Plain("Reset to defaults", ConfirmReset));
        actions.Children.Add(Plain("Back", ShowWheelStep));
        actions.Children.Add(Plain("Done", Close, cancel: true));
        body.Children.Add(actions);
    }

    private async Task<string> Apply(Fix fix)
    {
        if (owner.FfbSetting(fix.Key) is not { } setting) return "This setting is not available.";
        string label = setting.Label;
        int from = setting.Selected, to = Math.Clamp(from + fix.Step, fix.Min, fix.Max);
        if (setting.Kind == "Toggle") to = fix.Step > 0 ? 1 : 0;
        if (to == from)
        {
            if (fix.Fallback != null && owner.FfbSetting(fix.Fallback) is { } other)
                return await Change(other, other.Label, other.Selected, Math.Clamp(other.Selected + fix.FallbackStep, 20, 300));
            return setting.Kind == "Toggle" ? $"{label} is already on." : $"{label} is already at {from}, as far as this pick goes.";
        }
        return await Change(setting, label, from, to);
    }

    private async Task<string> Change(Setting setting, string label, int from, int to)
    {
        string key = setting.Tag["@ffb:".Length..];
        if (!await owner.ChangeFfbSetting(key, to)) return $"Could not change {label}.";
        string Shown(int v) => setting.Kind == "Toggle" ? (v != 0 ? "on" : "off") : v.ToString();
        return $"{label}: {Shown(from)} → {Shown(to)}. Drive a few corners to feel it.";
    }
}
