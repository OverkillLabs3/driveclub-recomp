using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;

namespace DriveClub.Launcher;

// Tune my wheel: the wheel type, which sets every force feedback setting to what suits that kind of wheelbase (the
// backend's ApplyWheelType), then one change for each thing a player may feel. Each change is written at once, as a
// change on the page is, so a running game and Live FFB Tuner follow it. The native window has a shorter version
// (OpenFfbGuide in driveclub_launcher.cpp).
internal sealed class FfbGuideDialog : Window
{
    private readonly MainWindow owner;
    private readonly StackPanel body = new() { Margin = new Thickness(24) };
    private readonly string detectedName;
    private readonly int detectedType;
    private static readonly string[] KindNames = ["direct drive", "belt", "gear"];

    // What a player feels, and the change that helps: a step on one setting, within its range. Damper's
    // "heavy and slow" lowers the gain once the damper is already at 0.
    private sealed record Fix(string Feel, string Key, int Step, int Min, int Max, string? Fallback = null, int FallbackStep = 0);
    private static readonly Fix[] Fixes =
    [
        new("Light or vague around the centre", "wheel_ffb_physics_damper", 5, 0, 30),
        new("Wobbles, or swings past the centre when you let go", "wheel_ffb_physics_damper", 5, 0, 30),
        new("Heavy and slow to turn", "wheel_ffb_physics_damper", -5, 0, 30, "wheel_ffb_physics_gain", -10),
        new("Too strong, or flat and dead in fast corners", "wheel_ffb_physics_gain", -10, 20, 300),
        new("Too weak overall", "wheel_ffb_physics_gain", 10, 20, 300),
        new("Busy, grainy or rattly", "wheel_ffb_physics_detail", -15, 0, 200),
        new("Dull: you can't feel the road or the grip", "wheel_ffb_physics_detail", 15, 0, 200),
        new("Kicks too hard after a slide or a hit", "wheel_ffb_physics_max_force", -10, 50, 100),
        new("Hard to catch slides", "wheel_ffb_physics_countersteer", 1, 0, 1),
    ];

    internal FfbGuideDialog(MainWindow owner, string detectedName, int detectedType)
    {
        this.owner = owner; this.detectedName = detectedName; this.detectedType = detectedType;
        Owner = owner; Title = "Tune my wheel"; Width = 640; MinWidth = 420;
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
        body.Children.Add(Text("1 of 2 · YOUR WHEEL", 12, true, "#8B93A1", 6));
        body.Children.Add(Text("What kind of wheel do you have?", 20, true));
        string found = detectedType is >= 0 and <= 2
            ? $"The game found {(detectedName.Length > 0 ? "“" + detectedName + "”" : "your wheel")}: {KindNames[detectedType]}. "
            : "";
        body.Children.Add(Text(found + "Each kind gets settings that suit its motor: gain 80, and its own detail, damper and steering help. " +
                               "Choosing one sets all of them again, so start here.", 14, color: "#B8BEC8", after: 16));
        body.Children.Add(Choice("Direct drive", "The motor turns the steering shaft itself: Fanatec DD and CSL DD, Moza, Simagic, Simucube, Logitech G PRO, Thrustmaster T818...", () => PickWheel(1)));
        body.Children.Add(Choice("Belt", "Thrustmaster T300, TX, T-GT or TS-PC, Fanatec CSL Elite or ClubSport V2...", () => PickWheel(2)));
        body.Children.Add(Choice("Gear", "Logitech G29, G920 or G923, Thrustmaster T150, TMX or T248...", () => PickWheel(3)));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 8, 0, 0) };
        actions.Children.Add(Plain("Keep my settings", () => ShowFeelStep(null)));
        actions.Children.Add(Plain("Close", Close, cancel: true));
        body.Children.Add(actions);
    }

    private async Task PickWheel(int type)
    {
        if (!await owner.ChangeFfbSetting("wheel_ffb_wheel_type", type)) { ShowFeelStep("Could not save the wheel type. The status line of the launcher says why."); return; }
        string Value(string key) => owner.FfbSetting(key) is { } s ? (s.Kind == "Toggle" ? (s.IsOn ? "on" : "off") : s.Selected.ToString()) : "?";
        ShowFeelStep($"Set for a {KindNames[type - 1]} wheel: gain {Value("wheel_ffb_physics_gain")}, detail {Value("wheel_ffb_physics_detail")}, " +
                     $"damper {Value("wheel_ffb_physics_damper")}, max force {Value("wheel_ffb_physics_max_force")}, countersteer assist {Value("wheel_ffb_physics_countersteer")}.");
    }

    private void ShowFeelStep(string? done)
    {
        body.Children.Clear();
        body.Children.Add(Text("2 of 2 · HOW DOES IT FEEL?", 12, true, "#8B93A1", 6));
        body.Children.Add(Text("Drive a few laps, then pick what you feel", 20, true));
        if (done != null) body.Children.Add(Text(done, 14, color: "#5AC878"));
        body.Children.Add(Text("Each pick makes one small change, and the game takes it within a second. Not enough? Pick it again. " +
                               "Too far? Pick the opposite.", 14, color: "#B8BEC8", after: 16));
        var result = Text("", 14, true, "#5AC878");
        foreach (var fix in Fixes) body.Children.Add(Choice(fix.Feel, "", async () => { result.Text = await Apply(fix); }));
        body.Children.Add(result);
        body.Children.Add(Text("One car stronger or weaker than the rest? The FFB gain + and − buttons on the Wheel page change only the car you're driving, " +
                               "5% a press, and each car keeps its own.", 13, color: "#8B93A1", after: 8));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
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
            return setting.Kind == "Toggle" ? $"{label} is already on." : $"{label} is already at {from}, as far as this goes.";
        }
        return await Change(setting, label, from, to);
    }

    private async Task<string> Change(Setting setting, string label, int from, int to)
    {
        string key = setting.Tag["@ffb:".Length..];
        if (!await owner.ChangeFfbSetting(key, to)) return $"Could not change {label}.";
        string Shown(int v) => setting.Kind == "Toggle" ? (v != 0 ? "on" : "off") : v.ToString();
        return $"{label}: {Shown(from)} → {Shown(to)}. Drive a few corners and see.";
    }
}
