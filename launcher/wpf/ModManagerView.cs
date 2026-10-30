using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.Win32;

namespace DriveClub.Launcher;

public partial class MainWindow
{
    private const int ModsPage = 7;
    private ModLibrary? modLibrary;
    private ModLibraryState modState = new();
    private string? selectedMod;
    private string modMessage = "";
    private bool modError;

    private FrameworkElement BuildModsPage()
    {
        modLibrary ??= new ModLibrary(root);
        try { modState = modLibrary.Load(); }
        catch (Exception ex) { modMessage = ex.Message; modError = true; }
        var canvas = new Canvas { HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
        var scroll = new ScrollViewer { Content = canvas, RenderTransform = new TranslateTransform(), VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, HorizontalContentAlignment = HorizontalAlignment.Left };
        double previousWidth = -1;
        void Arrange()
        {
            double width = Math.Max(700, scroll.ViewportWidth > 0 ? scroll.ViewportWidth : scroll.ActualWidth);
            if (Math.Abs(width - previousWidth) < .5) return;
            previousWidth = width; LayoutMods(canvas, width);
        }
        scroll.SizeChanged += (_, _) => Arrange();
        scroll.ScrollChanged += (_, e) => { if (e.ViewportWidthChange != 0) Arrange(); };
        return scroll;
    }

    private Button ModButton(string label, string id, Action action)
    {
        var button = new Button { Content = label, Height = 38, Padding = new Thickness(12, 0, 12, 0), Style = (Style)FindResource("FieldButton") };
        AutomationProperties.SetAutomationId(button, id);
        button.Click += (_, _) => action();
        return button;
    }

    private void LayoutMods(Canvas canvas, double width)
    {
        canvas.Children.Clear(); canvas.Width = width;
        double panelWidth = MeasureGameStatus(width - 72).Width, gap = 18;
        double groupWidth = panelWidth * 2 + gap, left = PanelGroupLeft(width, groupWidth);
        Caption(canvas, "MOD MANAGER", left, 46, groupWidth, 46, 32, true, "#F5F6F8", center: true);
        Caption(canvas, "Local loose-file mods. Higher in the list takes priority.", left, 99, groupWidth, 24, 15, false, "#8B93A1", center: true);
        RulePanel(canvas, left, 145, panelWidth, 38, "INSTALLED MODS");
        double inner = panelWidth - 44, x = left + 22, y = 195;
        double half = (inner - 12) / 2;
        Place(canvas, ModButton("Import ZIP", "ImportModZip", () => ImportMod(false)), x, y, half, 38);
        Place(canvas, ModButton("Import folder", "ImportModFolder", () => ImportMod(true)), x + half + 12, y, half, 38);
        y += 52;
        var count = Text($"{modState.Packages.Count} installed  /  {modState.Packages.Count(p => p.Enabled)} enabled", 12, color: "#8B93A1");
        Place(canvas, count, x, y, inner, 24); y += 32;
        if (modState.Packages.Count == 0)
        {
            var empty = Text("No mods installed.\n\nImport a ZIP or a folder containing files with the same paths as in the game. Imported mods start disabled.", 14, color: "#8B93A1");
            empty.Measure(new Size(inner, double.PositiveInfinity)); Place(canvas, empty, x, y, inner, empty.DesiredSize.Height); y += empty.DesiredSize.Height + 20;
        }
        if (!modState.Packages.Any(p => p.Id == selectedMod)) selectedMod = modState.Packages.FirstOrDefault()?.Id;
        var conflicts = ModLibrary.Conflicts(modState);
        for (int i = 0; i < modState.Packages.Count; i++)
        {
            var package = modState.Packages[i];
            var title = Text(package.Name, 15); title.Measure(new Size(inner - 45, double.PositiveInfinity));
            double rowHeight = Math.Max(52, title.DesiredSize.Height + 29);
            var row = new Grid(); row.ColumnDefinitions.Add(new() { Width = new GridLength(38) }); row.ColumnDefinitions.Add(new());
            var enabled = new CheckBox { IsChecked = package.Enabled, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center, ToolTip = package.Enabled ? "Disable mod" : "Enable mod" };
            AutomationProperties.SetAutomationId(enabled, "EnableMod-" + package.Id); AutomationProperties.SetName(enabled, "Enable " + package.Name);
            enabled.Checked += async (_, _) => await ChangeMods(() => modLibrary!.SetEnabled(package.Id, true), "Mod enabled.");
            enabled.Unchecked += async (_, _) => await ChangeMods(() => modLibrary!.SetEnabled(package.Id, false), "Mod disabled.");
            row.Children.Add(enabled);
            var content = new StackPanel { Margin = new Thickness(7, 7, 7, 7) }; content.Children.Add(title);
            int overridden = conflicts.Count(c => c.Value.Skip(1).Any(p => p.Id == package.Id));
            var note = Text($"{i + 1:00}   {(package.Enabled ? "Enabled" : "Disabled")}   ·   {package.Files.Count} files" + (overridden == 0 ? "" : $"   ·   {overridden} overridden"), 11, color: "#8B93A1");
            content.Children.Add(note);
            var select = new Button { Content = content, HorizontalContentAlignment = HorizontalAlignment.Stretch, Padding = new Thickness(0), Background = package.Id == selectedMod ? Brush("#252527") : Brushes.Transparent, BorderThickness = new Thickness(0) };
            AutomationProperties.SetAutomationId(select, "SelectMod-" + package.Id);
            select.Click += (_, _) => { selectedMod = package.Id; LayoutMods(canvas, width); };
            Grid.SetColumn(select, 1); row.Children.Add(select);
            Place(canvas, row, x, y, inner, rowHeight); y += rowHeight;
            Place(canvas, new Border { Background = Brush("#393940") }, x, y + 5, inner, 1); y += 16;
        }
        var tools = new Grid(); tools.ColumnDefinitions.Add(new()); tools.ColumnDefinitions.Add(new() { Width = new GridLength(12) }); tools.ColumnDefinitions.Add(new());
        tools.Children.Add(ModButton("Open mods folder", "OpenModsFolder", () => OpenModFolder(modLibrary!.OverlayDirectory)));
        var refresh = ModButton("Refresh", "RefreshMods", async () => await ChangeMods(() => modLibrary!.Load(), "Mod library refreshed."));
        Grid.SetColumn(refresh, 2); tools.Children.Add(refresh); Place(canvas, tools, x, y + 12, inner, 38); y += 66;

        double detailsX = left + panelWidth + gap, dy = 195;
        RulePanel(canvas, detailsX, 145, panelWidth, 38, "MOD DETAILS");
        detailsX += 22;
        void Paragraph(string message, double size = 14, string color = "#E8EAED", bool heading = false)
        {
            var text = Text(message, size, heading, color); text.Measure(new Size(inner, double.PositiveInfinity));
            Place(canvas, text, detailsX, dy, inner, text.DesiredSize.Height); dy += text.DesiredSize.Height + 14;
        }
        var selected = modState.Packages.FirstOrDefault(p => p.Id == selectedMod);
        if (selected != null)
        {
            Paragraph(selected.Name, 19, heading: true);
            if (!string.IsNullOrWhiteSpace(selected.Version + selected.Author)) Paragraph(string.Join("  ·  ", new[] { selected.Version, selected.Author }.Where(s => !string.IsNullOrWhiteSpace(s))), 12, "#8B93A1");
            if (!string.IsNullOrWhiteSpace(selected.Description)) Paragraph(selected.Description);
            var priority = new Grid(); priority.ColumnDefinitions.Add(new()); priority.ColumnDefinitions.Add(new() { Width = new GridLength(12) }); priority.ColumnDefinitions.Add(new());
            var higher = ModButton("Move up", "ModPriorityUp", async () => await ChangeMods(() => modLibrary!.Move(selected.Id, -1), "Mod priority updated."));
            var lower = ModButton("Move down", "ModPriorityDown", async () => await ChangeMods(() => modLibrary!.Move(selected.Id, 1), "Mod priority updated."));
            higher.IsEnabled = modState.Packages.IndexOf(selected) > 0; lower.IsEnabled = modState.Packages.IndexOf(selected) < modState.Packages.Count - 1;
            priority.Children.Add(higher); Grid.SetColumn(lower, 2); priority.Children.Add(lower);
            Place(canvas, priority, detailsX, dy, inner, 38); dy += 52;
            var affected = conflicts.Where(c => c.Value.Any(p => p.Id == selected.Id)).ToArray();
            Paragraph(affected.Length == 0 ? "No file conflicts with other enabled mods." : $"{affected.Length} shared file(s). The mod highest in the list wins.", 12, affected.Length == 0 ? "#8B93A1" : "#F0AA3C");
            foreach (var conflict in affected.Take(8)) Paragraph(conflict.Key + "\nUsed from: " + conflict.Value[0].Name, 12, "#8B93A1");
            if (affected.Length > 8) Paragraph($"+ {affected.Length - 8} more shared paths", 12, "#8B93A1");
            var files = new TextBox { Text = string.Join(Environment.NewLine, selected.Files.Keys.OrderBy(k => k)), IsReadOnly = true, TextWrapping = TextWrapping.Wrap, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Background = Brushes.Transparent, BorderThickness = new Thickness(0), FontSize = 12, Foreground = Brush("#8B93A1"), Padding = new Thickness(0) };
            AutomationProperties.SetName(files, "Mod files");
            Place(canvas, files, detailsX, dy, inner, Math.Min(132, selected.Files.Count * 20 + 8)); dy += files.Height + 16;
            Place(canvas, ModButton("Remove from manager", "RemoveMod", async () => await ChangeMods(() => modLibrary!.Remove(selected.Id), "Mod removed. Its copied files remain in the library for recovery.")), detailsX, dy, inner, 38); dy += 52;
        }
        else Paragraph("Select an installed mod to see its files, conflicts and priority.", color: "#8B93A1");
        Paragraph("Changes apply immediately to the next game launch. The original game folders stay untouched.\n\nBinary community patches remain under Gameplay.", 12, "#8B93A1");
        if (!string.IsNullOrEmpty(modMessage)) Paragraph(modMessage, 12, modError ? "#FF9A9F" : "#8B93A1");
        canvas.Height = Math.Max(y, dy) + 24;
    }

    private async void ImportMod(bool folder)
    {
        if (busy) return;
        string source;
        if (folder)
        {
            var dialog = new OpenFolderDialog { Title = "Import a mod folder (game-relative files)" };
            if (dialog.ShowDialog(this) != true) return;
            source = dialog.FolderName;
        }
        else
        {
            var dialog = new OpenFileDialog { Title = "Import a loose-file mod", Filter = "ZIP mod archive (*.zip)|*.zip", CheckFileExists = true };
            if (dialog.ShowDialog(this) != true) return;
            source = dialog.FileName;
        }
        await ChangeMods(() => modLibrary!.Import(source), "Mod imported. Enable it when ready.", selectLast: true);
    }

    private async Task ChangeMods(Func<ModLibraryState> operation, string message, bool selectLast = false)
    {
        if (busy) return;
        busy = true; PageHolder.IsEnabled = SidebarMenuHost.IsEnabled = FooterPlay.IsEnabled = CloseButton.IsEnabled = SaveButton.IsEnabled = ResetButton.IsEnabled = false;
        SetStatus("Updating mods…");
        try
        {
            modState = await Task.Run(operation);
            if (selectLast) selectedMod = modState.Packages.LastOrDefault()?.Id;
            modMessage = message; modError = false;
        }
        catch (Exception ex) { modMessage = ex.Message; modError = true; }
        finally
        {
            busy = false; PageHolder.IsEnabled = SidebarMenuHost.IsEnabled = CloseButton.IsEnabled = SaveButton.IsEnabled = ResetButton.IsEnabled = true;
            FooterPlay.IsEnabled = preview || (state.FoldersOk && string.IsNullOrEmpty(state.HardwareProblem));
            pages.Remove(ModsPage); ShowPage(ModsPage, false); SetStatus(modMessage, modError);
        }
    }

    private void OpenModFolder(string folder)
    {
        try { Directory.CreateDirectory(folder); Process.Start(new ProcessStartInfo(folder) { UseShellExecute = true }); }
        catch (Exception ex) { SetStatus(ex.Message, true); }
    }
}
