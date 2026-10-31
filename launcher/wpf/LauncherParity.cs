using System.ComponentModel;
using System.Diagnostics;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace DriveClub.Launcher;

public partial class MainWindow
{
    private Button updateButton = null!;
    private string availableUpdate = "";
    private int captureSettingId = -1;
    private string? wheelCaptureCommand;
    private bool bindingCapture;

    private void InitializeParity()
    {
        var footer = (Grid)FooterFrame.Child;
        footer.ColumnDefinitions.Insert(3, new ColumnDefinition { Width = GridLength.Auto });
        foreach (UIElement child in footer.Children)
            if (Grid.GetColumn(child) >= 3) Grid.SetColumn(child, Grid.GetColumn(child) + 1);
        updateButton = new Button
        {
            Content = "Update", Style = (Style)FindResource("FieldButton"), Height = 40,
            Margin = new Thickness(18, 0, 0, 0), Padding = new Thickness(18, 0, 18, 0),
            Foreground = Brush("#5AC878"), Visibility = Visibility.Collapsed,
            ToolTip = "Download, verify and install the new release. Saves, settings and mods are kept."
        };
        AutomationProperties.SetAutomationId(updateButton, "InstallUpdate");
        Grid.SetColumn(updateButton, 3); footer.Children.Add(updateButton);
        updateButton.Click += async (_, _) => await InstallAvailableUpdate();
        PreviewKeyDown += (_, e) =>
        {
            if (captureSettingId < 0) return;
            if (e.Key == Key.Escape) { wheelCaptureCommand = "cancel"; e.Handled = true; }
            else if (!bindingCapture && e.Key is Key.Delete or Key.Back) { wheelCaptureCommand = "clear"; e.Handled = true; }
        };
    }

    private void OperationStatus(string success)
    {
        if (!string.IsNullOrEmpty(state.HardwareProblem)) SetStatus(state.HardwareProblem, true);
        else SetStatus(success);
    }

    private async Task CheckNativeUpdates()
    {
        if (!state.Controls.Any(c => c.Tag == "@updatecheck" && c.IsOn)) return;
        try
        {
            var response = await backend.Call("check-update", state);
            availableUpdate = response.UpdateVersion;
            if (availableUpdate.Length != 0)
            {
                updateButton.Content = "Update to " + availableUpdate;
                updateButton.Visibility = Visibility.Visible;
                if (state.HardwareProblem.Length == 0) ShowUpdateNotice(availableUpdate);
                else OperationStatus("");
            }
            else
            {
                updateButton.Visibility = Visibility.Collapsed;
                // Development identifiers are shown honestly; never compare
                // main-<hash> against an invented release number and downgrade it.
                if (response.UpdateNote.StartsWith("Development build") || response.InstalledVersion.Length == 0)
                    OperationStatus(response.UpdateNote);
            }
        }
        catch (Exception ex) when (ex is System.IO.IOException or InvalidOperationException)
        { SetStatus("Update check unavailable: " + ex.Message, true); }
    }

    private async Task InstallAvailableUpdate()
    {
        if (busy || availableUpdate.Length == 0) return;
        busy = true; updateButton.IsEnabled = false;
        var dialog = NewDetailsWindow("UPDATE DRIVECLUB", 520);
        var stack = new StackPanel { Margin = new Thickness(26) };
        stack.Children.Add(Text("Downloading and verifying " + availableUpdate + "…", 18, true));
        stack.Children.Add(new TextBlock { Text = "The launcher restarts after installation. Saves, settings and mods are kept.", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 14, 0, 20), Foreground = Brush("#A0A0AA") });
        var bar = new ProgressBar { Minimum = 0, Maximum = 100, Height = 5, IsIndeterminate = true };
        var status = Text("Connecting…", 14); status.Margin = new Thickness(0, 14, 0, 0);
        stack.Children.Add(bar); stack.Children.Add(status); dialog.Content = stack;
        bool done = false;
        dialog.Closing += (_, e) => { if (!done) e.Cancel = true; };
        dialog.Loaded += async (_, _) =>
        {
            try
            {
                state.UpdateVersion = availableUpdate; state.ShellProcessId = (uint)Environment.ProcessId;
                var response = await backend.Call("install-update", state,
                    new Progress<(int Percent, string Text)>(p =>
                    { bar.IsIndeterminate = p.Percent == 0; bar.Value = p.Percent; status.Text = p.Text + " " + p.Percent + "%"; }));
                if (!response.RestartRequired) throw new InvalidOperationException("The update did not request a restart.");
                done = true; dialog.Close(); allowClose = true; Close();
            }
            catch (Exception ex) { done = true; dialog.Close(); SetStatus("Update failed: " + ex.Message, true); }
            finally { busy = false; updateButton.IsEnabled = true; }
        };
        dialog.ShowDialog();
    }

    private Window NewDetailsWindow(string title, double width = 620) => new()
    {
        Owner = this, Title = title, Width = width, SizeToContent = SizeToContent.Height,
        MaxHeight = Math.Max(300, SystemParameters.WorkArea.Height - 60),
        WindowStartupLocation = WindowStartupLocation.CenterOwner, ResizeMode = ResizeMode.NoResize,
        ShowInTaskbar = false, Background = Brush("#111114"), Foreground = Brush("#F2F2F4"),
        FontFamily = (FontFamily)FindResource("BodyFont"), FontSize = 14,
        Icon = Icon
    };

    private void ShowDetails(string title, string content)
    {
        var dialog = NewDetailsWindow(title);
        var panel = new DockPanel { Margin = new Thickness(26), Background = Brush("#111114") };
        var close = new Button { Content = "Close", IsCancel = true, IsDefault = true, HorizontalAlignment = HorizontalAlignment.Right, Padding = new Thickness(25, 9, 25, 9), Margin = new Thickness(0, 18, 0, 0) };
        DockPanel.SetDock(close, Dock.Bottom); panel.Children.Add(close);
        close.Click += (_, _) => dialog.Close();
        var heading = Text(title, 19, true); heading.Margin = new Thickness(0, 0, 0, 18);
        DockPanel.SetDock(heading, Dock.Top); panel.Children.Add(heading);
        panel.Children.Add(new TextBox
        {
            // These are reading panes, not the grain-textured 38px input fields.
            Style = new Style(typeof(TextBox)), Height = double.NaN,
            Text = content, IsReadOnly = true, TextWrapping = TextWrapping.Wrap,
            Background = Brushes.Transparent, Foreground = Brush("#E8EAED"), BorderThickness = new Thickness(0),
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto, MaxHeight = Math.Max(170, SystemParameters.WorkArea.Height - 240),
            Padding = new Thickness(0), AcceptsReturn = true, VerticalContentAlignment = VerticalAlignment.Top
        });
        dialog.Content = panel; dialog.ShowDialog();
    }

    private Button SettingHelp(Setting setting)
    {
        var button = new Button
        {
            Content = "i", Padding = new Thickness(0), FontSize = 12, Foreground = Brush("#A0A0AA"),
            Background = Brushes.Transparent, ToolTip = "Details: " + setting.Label,
            BorderThickness = new Thickness(0)
        };
        AutomationProperties.SetName(button, "Details for " + setting.Label);
        AutomationProperties.SetAutomationId(button, "SettingHelp" + setting.Id);
        button.Click += (_, _) => ShowDetails(setting.Label, setting.Hint + "\n\n" + setting.Details);
        return button;
    }

    private void AddBenchmarkDetails(Canvas row, double width)
    {
        if (state.BenchmarkRanDetails.Count == 0) return;
        var button = new Button
        {
            Content = "View actual run settings", HorizontalContentAlignment = HorizontalAlignment.Left,
            ToolTip = state.BenchmarkRanSummary, Padding = new Thickness(12, 0, 12, 0)
        };
        AutomationProperties.SetAutomationId(button, "BenchmarkRunDetails");
        button.Click += (_, _) => ShowDetails("WHAT THE GAME RAN WITH", string.Join("\n\n", state.BenchmarkRanDetails));
        Place(row, button, 0, 238, width, 36);
    }

    private void ConfigureNativeButton(Button button, Setting setting)
    {
        button.Click += async (_, _) => await ActivateNativeButton(button, setting);
        if (setting.Tag is not ("@ffbgainup" or "@ffbgaindown")) return;
        var menu = new ContextMenu(); var clear = new MenuItem { Header = "Clear assignment" };
        clear.Click += async (_, _) => await ActivateNativeButton(button, setting, clear: true);
        menu.Items.Add(clear); button.ContextMenu = menu;
    }

    // Double-click on a device name: read that pad for the binding buttons and the monitor.
    private async Task ActivateDevice(Setting setting)
    {
        if (busy || !setting.Tag.StartsWith("@dev:")) return;
        busy = true;
        try
        {
            state.DeviceSlot = int.Parse(setting.Tag.Substring(5));
            var response = await backend.Call("usedevice", state);
            Adopt(response);   // the red name and the monitor follow the new pad
            SetStatus(response.Message.Length == 0 ? "Input device switched." : response.Message);
        }
        catch (Exception ex) { SetStatus(ex.Message, true); }
        finally { busy = false; state.DeviceSlot = -1; }
    }


    private async Task ActivateNativeButton(Button button, Setting setting, bool clear = false)
    {
        bool gain = setting.Tag is "@ffbgainup" or "@ffbgaindown";
        if (captureSettingId >= 0)
        {
            if (captureSettingId == setting.Id) wheelCaptureCommand = clear ? "clear" : "cancel";
            return;
        }
        if (busy) return;
        var originalContent = button.Content;
        try
        {
            if (gain)
            {
                busy = true; captureSettingId = setting.Id; wheelCaptureCommand = null;
                state.CaptureClear = clear;
                button.Content = clear ? "Clearing…" : "Press wheel button · click to cancel";
                SetStatus("Press a wheel button. Esc: cancel · Delete / Backspace: clear assignment.");
            }
            else button.IsEnabled = false;
            bool saves = setting.Tag is "@savebackup" or "@saverestore";
            var response = await backend.Call(gain || saves ? setting.Tag[1..] : "wheelsetup", state,
                captureCommand: gain ? () => wheelCaptureCommand : null);
            if (gain)
            {
                foreach (var changed in response.Controls.Where(c => c.Tag is "@ffbgainup" or "@ffbgaindown"))
                    state.Controls.Single(c => c.Tag == changed.Tag).Options = changed.Options;
                pages.Remove(currentPage); ShowPage(currentPage, false);
            }
            SetStatus(response.Message.Length == 0 ? (saves ? "Done." : "Wheel Setup opened.") : response.Message);
        }
        catch (Exception ex) { SetStatus(ex.Message, true); }
        finally
        {
            if (gain) { captureSettingId = -1; wheelCaptureCommand = null; state.CaptureClear = false; busy = false; }
            button.Content = originalContent; button.IsEnabled = true;
        }
    }

    private async Task ActivateBinding(Button button, Setting setting, int side = 0)
    {
        if (busy) return;
        var original = button.Content;
        try
        {
            busy = true;
            state.CaptureId = setting.Id;
            state.CaptureSide = side;
            captureSettingId = setting.Id;   // makes Esc write the cancel file at once
            bindingCapture = true;
            button.Content = side == 1 ? "Press a button…" : "Press a key…";
            SetStatus(side == 1
                ? "Press the controller button you want to use. Esc: cancel."
                : "Press the key you want to use. Esc: cancel.");
            var response = await backend.Call("capture", state, captureCommand: () => wheelCaptureCommand);
            var changed = response.Controls.FirstOrDefault(c => c.Id == setting.Id);
            if (changed != null) setting.Options = changed.Options;
            SetStatus(response.Message.Length == 0 ? "Binding saved." : response.Message);
            pages.Remove(currentPage); ShowPage(currentPage, false);
        }
        catch (Exception ex) { SetStatus(ex.Message, true); }
        finally
        {
            state.CaptureId = 0;
            captureSettingId = -1;
            bindingCapture = false;
            busy = false;
            button.Content = original;
            button.IsEnabled = true;
        }
    }
}
