using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Interop;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;
using Path = System.IO.Path;

namespace DriveClub.Launcher;
public partial class MainWindow : Window
{
    private InputMonitorView? monitorView;

    private double pageScrollOffset;
    private readonly Backend backend = new();
    private LauncherState state = new();
    private readonly Dictionary<int, FrameworkElement> pages = [];
    private readonly Dictionary<string, BitmapSource> images = [];
    private readonly List<Button> navButtons = [];
    private readonly string[] names = ["Play", "Display", "Graphics", "General", "Driving", "Patches", "Benchmark", "Mods"];
    private readonly string[] hints = ["Your game, ready to drive.", "Frame rate, resolution, anti-aliasing and HDR.", "Scene detail, reflections and camera effects.", "Player profile, content, launcher behavior and advanced options.", "Driving assists, field of view and steering wheel forces.", "Compatibility workarounds and optional community patches.", "A repeatable event driven by the game's AI.", "Manage local replacement files."];
    // Home is shell-only; the remaining pages follow the native 0.3.1 tabs.
    private readonly int[] sourcePages = [-1, 0, 1, 2, 3, 5, 4, -2];
    private readonly string[] backgroundPaths = ["backgrounds/page-0.png", "backgrounds/page-2.png", "backgrounds/detail.png", "backgrounds/page-4.png", "backgrounds/driving.png", "backgrounds/patches.png", "backgrounds/page-5.png", "backgrounds/mods.png"];
    private int drivingSubpage;
    private bool foldersExpanded;
    private bool animateFoldersOpening;
    private bool foldersAnimating;
    private Action? arrangeHome;
    private readonly string root = AppContext.BaseDirectory;
    private int currentPage, revision;
    private int navigationSelection = -1;
    private bool suppress, busy, allowClose;
    private Window? launchSplash;
    private Panel? busyOverlayParent;
    private Button? switchToGame;
    private bool mayCloseSplash;
    private int launchedGameId;
    private LaunchSession? launchSession;
    private string baseStatus = "Loading your settings...";
    private readonly bool preview = Environment.GetCommandLineArgs().Contains("--preview");
    private DispatcherTimer? spinnerTimer;
    private int spinnerIndex;
    private readonly List<BitmapSource> spinnerFrames = [];
    private int refreshRate = 240;
    private string? releaseUrl;
    private string baseStatusColour = "#A0A0AA";
    private Task CheckForUpdates() => CheckNativeUpdates();

    [DllImport("dwmapi.dll")] private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [DllImport("gdi32.dll")] private static extern int GetDeviceCaps(IntPtr dc, int index);

    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { public int Size; public NativeRect Monitor, Work; public uint Flags; }
    [StructLayout(LayoutKind.Sequential)] private struct MinMaxInfo { public NativePoint Reserved, MaxSize, MaxPosition, MinTrackSize, MaxTrackSize; }
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out NativeRect rect);
    private IntPtr WindowHook(IntPtr hwnd, int message, IntPtr wparam, IntPtr lparam, ref bool handled)
    {
        if (message != 0x0024) return IntPtr.Zero;
        var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        if (!GetMonitorInfo(MonitorFromWindow(hwnd, 2), ref info)) return IntPtr.Zero;
        var limits = Marshal.PtrToStructure<MinMaxInfo>(lparam);
        limits.MaxPosition = new() { X = info.Work.Left - info.Monitor.Left, Y = info.Work.Top - info.Monitor.Top };
        limits.MaxSize = new() { X = info.Work.Right - info.Work.Left, Y = info.Work.Bottom - info.Work.Top };
        var dpi = VisualTreeHelper.GetDpi(this);
        limits.MinTrackSize = new() { X = (int)Math.Ceiling(MinWidth * dpi.DpiScaleX), Y = (int)Math.Ceiling(MinHeight * dpi.DpiScaleY) };
        Marshal.StructureToPtr(limits, lparam, false); handled = true; return IntPtr.Zero;
    }
    public MainWindow()
    {
        LoadFonts();
        InitializeComponent();
        InitializeParity();
        if (preview) Title = "DRIVECLUB Launcher - Preview";
        Status.MouseLeftButtonUp += (_, _) =>
        {
            if (releaseUrl == null || Status.Text != baseStatus) return;
            try { Process.Start(new ProcessStartInfo(releaseUrl) { UseShellExecute = true }); }
            catch (Exception ex) when (ex is Win32Exception or InvalidOperationException) { SetStatus("Could not open the release page in your browser.", true); }
        };
        StateChanged += (_, _) => MaximizeGlyph.Glyph = WindowState == WindowState.Maximized ? "Restore" : "Maximize";
        TitleLogo.Source = Image("brand/logo-red.png");
        LoadOptionBrushes();
        BusyArt.Source = Image("backgrounds/page-0.png", true);
        SwipeOne.Source = SwipeTwo.Source = Image("transitions/swipe.png");
        Stamp.Source = Image("transitions/stamp.png");
        for (int i = 0; i < 29; ++i) if (Image($"spinner/{i:00}.png") is { } frame) spinnerFrames.Add(frame);
        foreach (string background in backgroundPaths) Image(background, true);
        BuildNavigation();
        SourceInitialized += (_, _) =>
        {
            var handle = new WindowInteropHelper(this).Handle;
            HwndSource.FromHwnd(handle)?.AddHook(WindowHook);
            int square = 1; DwmSetWindowAttribute(handle, 33, ref square, sizeof(int));
            int dark = 1; DwmSetWindowAttribute(handle, 20, ref dark, sizeof(int));
            var dc = GetDC(handle); int hz = GetDeviceCaps(dc, 116); ReleaseDC(handle, dc);
            refreshRate = hz > 1 ? hz : 240;
        };
        Closing += (_, e) => { if (busy && !allowClose) { e.Cancel = true; SetStatus("Please wait for the current operation to finish."); } };
        Loaded += async (_, _) =>
        {
            try
            {
                Adopt(await backend.Call("state"));
                OperationStatus(state.FoldersOk ? "" : "Choose the game folders on Play.");

                if (Environment.GetCommandLineArgs().Contains("--mod-manager")) ShowPage(ModsPage, false);
                if (!preview && Environment.GetCommandLineArgs().Contains("--benchmark")) { ShowPage(6, false); await Launch(true); }
                else if (!preview && (Environment.GetCommandLineArgs().Contains("--play") || (File.Exists(Path.Combine(root, "launcher.ini")) && File.ReadLines(Path.Combine(root, "launcher.ini")).Any(x => x.Trim() == "auto_play=1")))) await Launch(false);
                if (!preview && !busy && IsVisible) _ = CheckForUpdates();
            }
            catch (Exception ex) { SetStatus(ex.Message, true); }
        };
        // One 16 ms timer at Render priority: a Background-priority timer is starved by the page's own
        // layout work, which made the monitor crawl while the rest of the window was idle. The lambda
        // reads the field, so it always drives the view of the page currently on screen (rebuilt pages
        // get a fresh view) and does nothing when that page is not visible.
        var monitorTimer = new System.Windows.Threading.DispatcherTimer(System.Windows.Threading.DispatcherPriority.Render)
        { Interval = TimeSpan.FromMilliseconds(16) };
        monitorTimer.Tick += (_, _) =>
        {
            var view = monitorView;
            if (view is { IsVisible: true }) view.ReadPad();
            PollControllers();   // a pad plugged in or removed shows up without reopening the page
        };
        monitorTimer.Start();

    }
    private void LoadFonts()
    {
        string directory = Path.Combine(root, "assets", "fonts");
        if (!Directory.Exists(directory)) return;
        var uri = new Uri(directory + Path.DirectorySeparatorChar);
        Application.Current.Resources["BodyFont"] = new FontFamily(uri, "./#001Futura");
        Application.Current.Resources["HeadingFont"] = new FontFamily(uri, "./#004Russian");
    }
    private BitmapSource? Image(string relative, bool monochrome = false)
    {
        string key = relative + (monochrome ? ":mono" : "");
        if (images.TryGetValue(key, out var cached)) return cached;
        string file = Path.Combine(root, "assets", relative.Replace('/', Path.DirectorySeparatorChar));
        if (!File.Exists(file)) return null;
        var bitmap = new BitmapImage(); bitmap.BeginInit(); bitmap.CacheOption = BitmapCacheOption.OnLoad;
        bitmap.UriSource = new Uri(file); bitmap.EndInit(); bitmap.Freeze();
        BitmapSource result = bitmap;
        // Runtime conversion happens once per art. Original files remain unchanged.
        if (monochrome) { var gray = new FormatConvertedBitmap(bitmap, PixelFormats.Gray8, null, 0); gray.Freeze(); result = gray; }
        images[key] = result; return result;
    }
    private TextBlock Text(string value, double size = 14, bool heading = false, string color = "#F2F2F4") => new()
    { Text = value, FontSize = size, FontFamily = (FontFamily)Application.Current.FindResource(heading ? "HeadingFont" : "BodyFont"), Foreground = Brush(color), TextWrapping = TextWrapping.Wrap };
    private static SolidColorBrush Brush(string color) { var b = (SolidColorBrush)new BrushConverter().ConvertFromString(color)!; b.Freeze(); return b; }
    private void SetStatus(string text, bool error = false)
    {
        baseStatus = text; baseStatusColour = error ? "#FF9A9F" : "#A0A0AA";
        releaseUrl = null; RestoreStatus();
    }
    private void RestoreStatus()
    {
        Status.Text = baseStatus; Status.Foreground = Brush(baseStatusColour);
        Status.Cursor = releaseUrl == null ? Cursors.Arrow : Cursors.Hand;
        Status.ToolTip = releaseUrl == null ? null : "Opens the release page. Use the Update button to download and install it.";
    }
    private void ShowStatusHint(string text)
    {
        // The footer line is one or two lines high: a hint with bullets shows its first line here, the rest is in the
        // hover tooltip.
        var newline = text.IndexOf('\n');
        if (newline > 0) text = text[..newline].Trim() + "  (hover for the full text)";
        Status.Text = text; Status.Foreground = Brush("#A0A0AA");
        Status.Cursor = Cursors.Arrow; Status.ToolTip = null;
    }
    private void ShowUpdateNotice(string tag)
    {
        SetStatus($"Version {tag} is available. Use Update to install it, or click here for release details.");
        releaseUrl = "https://github.com/OverkillLabs3/driveclub-recomp/releases/tag/" + Uri.EscapeDataString(tag);
        baseStatusColour = "#5AC878"; RestoreStatus();
    }
    private void DiscordClick(object sender, RoutedEventArgs e)
    {
        var result = CommunityLink.Open();
        if (result.NeedsNotice) new CommunityLinkDialog(result.Message) { Owner = this }.ShowDialog();
        else SetStatus("Opening Discord in your browser...");
    }
    private void CopyDiscordClick(object sender, RoutedEventArgs e)
    {
        bool copied = CommunityLink.TryCopy();
        if (copied) SetStatus("Discord link copied: " + CommunityLink.Url);
        else new CommunityLinkDialog("Could not copy the link. Select it below and press Ctrl+C, or try Copy link again.") { Owner = this }.ShowDialog();
    }
    private void LoadOptionBrushes()
    {
        Application.Current.Resources["OptionActive"] = FindResource("Accent");
        foreach (var pair in new[] { ("OptionInactive", "controls/option-inactive.png") })
        {
            var bitmap = Image(pair.Item2);
            if (bitmap is null) continue;
            // The first 192 source pixels fade to transparent black. Use only
            // the opaque portion; grain is rendered after scaling in the template.
            var brush = new ImageBrush(bitmap) { Stretch = Stretch.Fill, Opacity = .72, Viewbox = new Rect(192, 8, 476, 62), ViewboxUnits = BrushMappingMode.Absolute }; brush.Freeze();
            Application.Current.Resources[pair.Item1] = brush;
        }
    }
    private void BuildNavigation()
    {
        for (int i = 0; i < names.Length; ++i)
        {
            int page = i;
            var label = Text(names[i], 17, true, "#8B93A1"); label.VerticalAlignment = VerticalAlignment.Center;
            var content = new Grid(); content.ColumnDefinitions.Add(new() { Width = new(28) }); content.ColumnDefinitions.Add(new());
            var number = Text($"{i + 1:00}", 10, true, "#616B78"); number.VerticalAlignment = VerticalAlignment.Center;
            content.Children.Add(number); Grid.SetColumn(label, 1); content.Children.Add(label);
            var button = new Button { Content = content, Height = 38, Margin = new(0, 0, 0, i + 1 < names.Length ? 9 : 0), Padding = new(14, 0, 0, 0), BorderThickness = new(3, 0, 0, 0), Background = Brushes.Transparent, BorderBrush = Brushes.Transparent };
            button.Template = NavigationTemplate();
            AutomationProperties.SetAutomationId(button, "Nav" + names[i]); AutomationProperties.SetName(button, names[i]);
            button.Click += (_, _) => ShowPage(page);
            button.MouseEnter += (_, _) => { if (currentPage == page) NavigationSelection.Opacity = .85; };
            button.MouseLeave += (_, _) => { if (currentPage == page) NavigationSelection.Opacity = 1; };
            navButtons.Add(button); Navigation.Children.Add(button);
        }
    }
    private static ControlTemplate NavigationTemplate()
    {
        var border = new FrameworkElementFactory(typeof(Border)); border.Name = "Face";
        border.SetBinding(Border.BackgroundProperty, new Binding("Background") { RelativeSource = RelativeSource.TemplatedParent });
        border.SetBinding(Border.BorderBrushProperty, new Binding("BorderBrush") { RelativeSource = RelativeSource.TemplatedParent });
        border.SetBinding(Border.BorderThicknessProperty, new Binding("BorderThickness") { RelativeSource = RelativeSource.TemplatedParent });
        var content = new FrameworkElementFactory(typeof(ContentPresenter)); content.SetValue(FrameworkElement.MarginProperty, new Thickness(11, 0, 0, 0)); content.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Stretch);
        border.AppendChild(content);
        var template = new ControlTemplate(typeof(Button)) { VisualTree = border };
        var hover = new Trigger { Property = IsMouseOverProperty, Value = true }; hover.Setters.Add(new Setter(Border.OpacityProperty, .85, "Face")); template.Triggers.Add(hover);
        return template;
    }
    private void Adopt(LauncherState incoming)
    {
        bool initial = state.Controls.Count == 0;
        if (initial || !incoming.FoldersOk || (!state.FoldersOk && incoming.FoldersOk))
            foldersExpanded = !incoming.FoldersOk;
        suppress = true;
        foreach (var c in state.Controls) c.PropertyChanged -= SettingChanged;
        state = incoming;
        // Sidebar, window border, page margins, two panels, gap and scrollbar.
        MinWidth = Math.Ceiling(228 + 2 + 72 + 2 * MeasureGameStatus(double.PositiveInfinity).Width + 24 + 10);
        foreach (var c in state.Controls) c.PropertyChanged += SettingChanged;
        pages.Clear(); ShowPage(currentPage, initial); suppress = false;
        FooterPlay.IsEnabled = preview || (state.FoldersOk && string.IsNullOrEmpty(state.HardwareProblem));
        FooterPlay.ToolTip = string.IsNullOrEmpty(state.HardwareProblem) ? "Save and start the game." : state.HardwareProblem;
    }
    private async void SettingChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (suppress || sender is not Setting setting || args.PropertyName is not ("Selected" or "Text")) return;
        int changed = ++revision; SetStatus("Unsaved changes");
        state.ChangedId = setting.Id;
        if (args.PropertyName == "Selected")
        {
            string action = setting.Id == state.PresetId ? "preset" : setting.Tag == "@drive_preset" ? "drive-preset" : setting.Tag == "@wheelpreset" ? "wheel-preset" : "validate";
            try
            {
                var result = await backend.Call(action, state);
                if (changed == revision)
                {
                    Adopt(result); OperationStatus("Unsaved changes");
                    // A slot switch rewrites default.ini, so the chips have to show the new slot's bindings.
                    if (setting.Tag == "@profile") { pages.Remove(currentPage); ShowPage(currentPage, false); }
                }
            }
            catch (Exception ex) { SetStatus(ex.Message, true); }
        }
    }
    private DoubleAnimation Animation(double from, double to, double seconds)
    {
        var a = new DoubleAnimation(from, to, TimeSpan.FromSeconds(seconds)) { EasingFunction = new OriginalMenuEase(), FillBehavior = FillBehavior.HoldEnd };
        Timeline.SetDesiredFrameRate(a, refreshRate); return a;
    }
    private const double PageTransitionSeconds = .280;
    private const double NavigationSelectionSeconds = .220;
    private DateTime nextControllerPoll = DateTime.MinValue;
    private string controllerSignature = "";
    private bool controllerPolling;
    // Once a second: is the set of pads different from when the page was built? If so, re-read the state
    // so DEVICES shows the new pad (and the status line says so).
    private async void PollControllers()
    {
        if (controllerPolling || DateTime.UtcNow < nextControllerPoll) return;
        nextControllerPoll = DateTime.UtcNow.AddSeconds(1);
        string signature = InputMonitorView.SlotSignature();
        if (signature == controllerSignature) return;
        bool first = controllerSignature.Length == 0;
        controllerSignature = signature;
        if (first || busy || preview) return;
        controllerPolling = true;
        try
        {
            Adopt(await backend.Call("state"));
            SetStatus("Controller list updated.");
        }
        catch (Exception) { }
        finally { controllerPolling = false; }
    }
    private void MoveNavigationSelection(int index, bool animate)
    {
        NavigationSelection.Opacity = navButtons[index].IsMouseOver ? .85 : 1;
        if (navigationSelection == index) return;
        double target = navButtons[index].TranslatePoint(new Point(), Navigation).Y;
        double from = NavigationSelectionTransform.Y;
        NavigationSelectionTransform.BeginAnimation(TranslateTransform.YProperty, null);
        NavigationSelectionTransform.Y = target;
        NavigationSelection.Visibility = Visibility.Visible;
        if (animate && navigationSelection >= 0)
            NavigationSelectionTransform.BeginAnimation(TranslateTransform.YProperty, PageSlideAnimation(from, target, NavigationSelectionSeconds));
        navigationSelection = index;
    }
    private DoubleAnimation PageSlideAnimation(double from, double to, double seconds = PageTransitionSeconds)
    {
        var animation = new DoubleAnimation(from, to, TimeSpan.FromSeconds(seconds))
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut },
            FillBehavior = FillBehavior.HoldEnd
        };
        Timeline.SetDesiredFrameRate(animation, refreshRate);
        return animation;
    }
    private void ShowPage(int index, bool animate = true)
    {
        if (state.Controls.Count == 0 || (currentPage == index && PageHolder.Children.Count != 0 && animate)) return;
        currentPage = index;
        for (int i = 0; i < navButtons.Count; ++i)
        {
            var button = navButtons[i]; bool selected = i == index;
            button.Background = Brushes.Transparent;
            button.BorderBrush = Brushes.Transparent;
            var content = (Grid)button.Content;
            ((TextBlock)content.Children[0]).Foreground = Brush(selected ? "#F5F6F8" : "#616B78");
            ((TextBlock)content.Children[1]).Foreground = Brush(selected ? "#F5F6F8" : "#8B93A1");
        }
        MoveNavigationSelection(index, animate);
        FooterPlay.Visibility = index == 0 ? Visibility.Collapsed : Visibility.Visible;
        FooterPlayColumn.Width = new GridLength(index == 0 ? 2 : 184);
        FooterPlay.Content = preview ? "PREVIEW LAUNCH" : "PLAY";
        ResetButton.Visibility = SaveButton.Visibility = index == ModsPage ? Visibility.Collapsed : Visibility.Visible;
        if (!pages.TryGetValue(index, out var page)) { page = BuildPage(index); pages[index] = page; }
        var outgoing = PageHolder.Children.OfType<FrameworkElement>().LastOrDefault();
        PageHolder.Children.Clear();
        if (animate && outgoing != null && outgoing != page)
        {
            outgoing.IsHitTestVisible = false; PageHolder.Children.Add(outgoing);
            outgoing.BeginAnimation(OpacityProperty, Animation(outgoing.Opacity, 0, PageTransitionSeconds));
            var exitTransform = (TranslateTransform)outgoing.RenderTransform;
            exitTransform.BeginAnimation(TranslateTransform.YProperty, PageSlideAnimation(exitTransform.Y, -12));
        }
        page.IsHitTestVisible = true; page.BeginAnimation(OpacityProperty, null); page.Opacity = animate ? 0 : 1;
        var enterTransform = (TranslateTransform)page.RenderTransform;
        enterTransform.BeginAnimation(TranslateTransform.YProperty, null); enterTransform.Y = 0;
        PageHolder.Children.Add(page);
        if (animate)
        {
            var fade = Animation(0, 1, PageTransitionSeconds);
            fade.Completed += (_, _) => { if (PageHolder.Children.Count > 0 && ReferenceEquals(PageHolder.Children[^1], page)) foreach (var old in PageHolder.Children.OfType<FrameworkElement>().Where(x => x != page).ToArray()) PageHolder.Children.Remove(old); };
            page.BeginAnimation(OpacityProperty, fade);
            enterTransform.BeginAnimation(TranslateTransform.YProperty, PageSlideAnimation(32, 0));
        }
        // Reduced page motion: incoming +32 to 0, outgoing 0 to -12,
        // 280 ms cubic ease-in-out slide; opacity keeps the smoothstep curve.
        // Backgrounds only crossfade.
        ChangeBackground(index, animate);
    }
    private void ChangeBackground(int index, bool animate)
    {
        var art = new Grid { ClipToBounds = true };
        var image = new System.Windows.Controls.Image { Source = Image(backgroundPaths[index], true), Stretch = Stretch.UniformToFill };
        RenderOptions.SetBitmapScalingMode(image, BitmapScalingMode.LowQuality); art.Children.Add(image);
        art.Children.Add(new Border { Background = (Brush)FindResource("ArtworkDim") });
        if (!animate || BackgroundHolder.Children.Count == 0) { BackgroundHolder.Children.Clear(); BackgroundHolder.Children.Add(art); return; }
        // Keep the current composited layers beneath the new one, including during rapid navigation.
        BackgroundHolder.Children.Add(art);
        var fade = Animation(0, 1, PageTransitionSeconds);
        fade.Completed += (_, _) =>
        {
            if (BackgroundHolder.Children.Count > 0 && ReferenceEquals(BackgroundHolder.Children[^1], art))
                foreach (var old in BackgroundHolder.Children.OfType<FrameworkElement>().Where(x => x != art).ToArray()) BackgroundHolder.Children.Remove(old);
        };
        art.BeginAnimation(OpacityProperty, fade);
    }
    private static void Place(Canvas canvas, FrameworkElement element, double x, double y, double width, double height)
    {
        element.Width = Math.Max(0, width); element.Height = Math.Max(0, height); Canvas.SetLeft(element, x); Canvas.SetTop(element, y); canvas.Children.Add(element);
    }
    private Border Caption(Canvas canvas, string value, double x, double y, double width, double height, double size = 14, bool heading = false, string color = "#E8EAED", bool wrap = false, bool center = false)
    {
        var text = Text(value, size, heading, color); text.TextWrapping = wrap ? TextWrapping.Wrap : TextWrapping.NoWrap;
        text.VerticalAlignment = VerticalAlignment.Center; text.TextTrimming = TextTrimming.CharacterEllipsis;
        text.TextAlignment = center ? TextAlignment.Center : TextAlignment.Left;
        var box = new Border { Child = text };
        Place(canvas, box, x, y, width, height);
        return box;
    }
    private FrameworkElement BuildPage(int index)
    {
        if (index == ModsPage) return BuildModsPage();
        if (index == 0) return BuildHomePage();
        var canvas = new Canvas { VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Left };
        var scroll = new ScrollViewer { VerticalContentAlignment = VerticalAlignment.Top, HorizontalContentAlignment = HorizontalAlignment.Left, RenderTransform = new TranslateTransform(), Content = canvas, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, PanningMode = PanningMode.VerticalOnly };
        scroll.ScrollChanged += (_, e) => { if (Math.Abs(e.VerticalChange) > 0.5) pageScrollOffset = scroll.VerticalOffset; };
        scroll.Loaded += (_, _) => scroll.ScrollToVerticalOffset(pageScrollOffset);
        double previousWidth = -1;
        void ArrangePage()
        {
            double width = Math.Max(700, scroll.ViewportWidth > 0 ? scroll.ViewportWidth : scroll.ActualWidth);
            if (Math.Abs(width - previousWidth) < .5) return;
            previousWidth = width; LayoutPage(canvas, index, width);
        }
        scroll.SizeChanged += (_, _) => ArrangePage();
        scroll.ScrollChanged += (_, e) => { if (e.ViewportWidthChange != 0) ArrangePage(); };
        return scroll;
    }
    private FrameworkElement BuildHomePage()
    {
        // Bottom alignment while space is available; the upper section sets a minimum
        // row height and pushes Quick Settings below the viewport in a short window.
        var page = new Grid();
        page.RowDefinitions.Add(new() { MinHeight = 572 });
        page.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var upper = new Canvas { VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Left };
        page.Children.Add(upper);
        var quick = new Canvas();
        var dock = new Border { Child = quick, Margin = new(36, 0, 36, 24), Tag = "QuickSettingsDock" };
        AutomationProperties.SetAutomationId(dock, "QuickSettingsDock");
        Grid.SetRow(dock, 1); page.Children.Add(dock);
        var scroll = new ScrollViewer { Content = page, RenderTransform = new TranslateTransform(), VerticalContentAlignment = VerticalAlignment.Top, HorizontalContentAlignment = HorizontalAlignment.Left, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        scroll.ScrollChanged += (_, e) => { if (Math.Abs(e.VerticalChange) > 0.5) pageScrollOffset = scroll.VerticalOffset; };
        scroll.Loaded += (_, _) => scroll.ScrollToVerticalOffset(pageScrollOffset);
        double previousWidth = -1;
        void ArrangeHome()
        {
            if (scroll.ActualWidth <= 0 || scroll.ActualHeight <= 0) return;
            double width = Math.Max(700, scroll.ViewportWidth > 0 ? scroll.ViewportWidth : scroll.ActualWidth);
            // Scrollbar appearance must not replace the expander mid-animation.
            if (foldersAnimating && previousWidth > 0) width = previousWidth;
            if (Math.Abs(previousWidth - width) >= .5)
            {
                previousWidth = width; page.Width = width; LayoutPage(upper, 0, width);
                double available = width - 72; quick.Width = available; quick.Children.Clear();
                int columns = available >= 850 ? 4 : 2;
                double qw = Math.Min(MeasureGameStatus(available).Width, (available - (columns - 1) * 16) / columns);
                double start = PanelGroupLeft(width, columns * qw + (columns - 1) * 16) - 36;
                Caption(quick, "QUICK SETTINGS", start, 0, columns * qw + (columns - 1) * 16, 27, 16, true, "#F5F6F8", center: true);
                double qy = 36;
                var controls = state.QuickControls.Select(id => state.Controls.Single(c => c.Id == id)).ToArray();
                for (int k = 0; k < controls.Length; k += columns)
                {
                    var line = controls.Skip(k).Take(columns).ToArray();
                    double lineHeight = line.Max(c => RowHeight(c, qw, true));
                    for (int col = 0; col < line.Length; ++col)
                        Place(quick, SettingView(line[col], qw, true), start + col * (qw + 16), qy, qw, lineHeight);
                    qy += lineHeight + 6;
                }
                quick.Height = qy - 6;
            }
            double viewport = scroll.ViewportHeight > 0 ? scroll.ViewportHeight : scroll.ActualHeight;
            page.RowDefinitions[0].MinHeight = upper.Height + 24;
            page.Height = Math.Max(viewport, page.RowDefinitions[0].MinHeight + quick.Height + 24);
        }
        scroll.SizeChanged += (_, _) => ArrangeHome();
        arrangeHome = ArrangeHome;
        upper.SizeChanged += (_, _) => ArrangeHome();
        scroll.ScrollChanged += (_, e) => { if (e.ViewportWidthChange != 0 || e.ViewportHeightChange != 0) ArrangeHome(); };
        return scroll;
    }
    private const double SectionHeaderHeight = 38;
    private double PanelGroupLeft(double pageWidth, double groupWidth)
    {
        // Page coordinates begin after the 228-DIP sidebar. Keep the whole
        // window's center as the target, with safe margins in narrow windows.
        double target = Root.ActualWidth / 2 - 228 - groupWidth / 2;
        return Math.Clamp(target, 36, Math.Max(36, pageWidth - 36 - groupWidth));
    }
    private void RulePanel(Canvas canvas, double x, double y, double width, double height, string title)
    {
        Place(canvas, new Border { Background = Brush("#ED2B38") }, x, y, 3, SectionHeaderHeight);
        Caption(canvas, title, x + 20, y, width - 40, SectionHeaderHeight, 15, true, "#D7D7D9");
    }
    private double SliderValueWidth(Setting setting)
    {
        double Measure(int number)
        {
            var text = Text(number + setting.Unit, 14); text.TextWrapping = TextWrapping.NoWrap;
            text.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            return Math.Ceiling(text.DesiredSize.Width);
        }
        return Math.Max(75, Math.Max(Measure(setting.Minimum), Measure(setting.Maximum)));
    }

    private string PadDisplay(string token)
    {
        if (string.IsNullOrEmpty(token)) return "\u2014";
        var ps = new System.Collections.Generic.Dictionary<string, string> {
            ["cross"]="Cross", ["circle"]="Circle", ["square"]="Square", ["triangle"]="Triangle",
            ["l1"]="L1", ["r1"]="R1", ["l3"]="L3", ["r3"]="R3", ["options"]="OPTIONS",
            ["back"]="Back / Share", ["touchpad_center"]="Touchpad",
            ["pad_up"]="D-pad up", ["pad_down"]="D-pad down", ["pad_left"]="D-pad left", ["pad_right"]="D-pad right" };
        var xb = new System.Collections.Generic.Dictionary<string, string> {
            ["cross"]="A", ["circle"]="B", ["square"]="X", ["triangle"]="Y",
            ["l1"]="LB", ["r1"]="RB", ["l3"]="LS click", ["r3"]="RS click", ["options"]="Menu",
            ["back"]="View", ["touchpad_center"]="Touchpad",
            ["pad_up"]="D-pad up", ["pad_down"]="D-pad down", ["pad_left"]="D-pad left", ["pad_right"]="D-pad right" };
        var map = state.XboxNames ? xb : ps;
        return map.TryGetValue(token, out var shown) ? shown : token;
    }


    // Built once per page render; a 60 Hz timer feeds it straight from XInput (no backend call).
    private FrameworkElement BuildMonitorView(string text)
    {
        int[] dz = new int[8];
        foreach (var c in state.Controls)
        {
            if (!c.Tag.StartsWith("@dead:")) continue;
            var parts = c.Tag.Split(':');
            int dev = parts.Length > 1 ? parts[1] switch { "leftjoystick" => 0, "rightjoystick" => 1, "l2" => 2, "r2" => 3, _ => 0 } : 0;
            int part = parts.Length > 2 ? int.Parse(parts[2]) : 0;
            dz[dev * 2 + part] = c.Selected;
        }
        monitorView = new InputMonitorView(dz, state.ReadSlot);
        return monitorView;
    }

    private (double Width, double Height) SettingLabelSize(Setting setting, double width, bool quick)
    {
        double labelWidth = width - (setting.Kind == "Toggle" ? 42 : setting.Kind == "Slider" ? SliderValueWidth(setting) + 12 : 0) - (setting.Details.Length == 0 ? 0 : 26);
        var label = Text(setting.Label, quick ? 14 : 15);
        label.Measure(new Size(Math.Max(1, labelWidth), double.PositiveInfinity));
        return (labelWidth, Math.Max(quick ? 24 : 20, Math.Ceiling(label.DesiredSize.Height)));
    }
    // The input monitor keeps its proportions: taller when the card is wider (0.3.0's MonitorHeight),
    // so the stick boxes, dead-zone rings and buttons grow with the window instead of staying at 46 px.
    private const double MonitorDesignWidth = 240;
    private static double MonitorScale(double width) => Math.Clamp(width / MonitorDesignWidth, 0.85, 2.2);
    private static double MonitorHeight(double width) => 200 * MonitorScale(width) + 18;
    private double RowHeight(Setting setting, double width, bool quick = false)
    {
        if (setting.Kind == "Action") return state.BenchmarkRanDetails.Count > 0 ? 288 : 242;
        if (setting.Kind == "Info")
        {
            // The body *is* the row (the device list is several lines): measure the wrapped text at the
            // card width, otherwise the list is cut off after two lines and looks incomplete.
            var body = Text(setting.Options.FirstOrDefault() ?? "", 14);
            body.Measure(new Size(Math.Max(1, width), double.PositiveInfinity));
            return Math.Max(46, Math.Ceiling(body.DesiredSize.Height) + 14);
        }
        double labelHeight = SettingLabelSize(setting, width, quick).Height;
        if (setting.Kind == "Toggle") return Math.Max(52, labelHeight + 12);
        if (setting.Kind == "Monitor") return MonitorHeight(width);
        return Math.Max(quick ? 76 : 82, labelHeight + (quick ? 52 : 62));
    }
    private void LayoutPage(Canvas canvas, int index, double availableWidth)
    {
        canvas.Children.Clear(); canvas.Width = availableWidth;
        const double left = 36; double width = availableWidth - 72;
        if (index == 0) { BuildHome(canvas, left, width); return; }
        if (sourcePages[index] < 0) { canvas.Height = 180; return; }
        double y = 145;
        double maxPanelWidth = MeasureGameStatus(width).Width;
        var cards = state.Cards.Where(c => c.Page == sourcePages[index] && (names[index] != "Driving" || c.Subpage == drivingSubpage)).ToArray();
        if (names[index] == "Driving")
        {
            string[] sections = ["Assists and view", "Wheel and force feedback", "Controller and keyboard"];
            double sectionWidth = Math.Min(maxPanelWidth, (width - 18 * (sections.Length - 1)) / sections.Length);
            for (int i = 0; i < sections.Length; ++i)
            {
                int section = i;
                var button = new Button { Content = sections[i], Style = (Style)FindResource("FieldButton"), Padding = new(12,0,12,0), Background = drivingSubpage == i ? Brush("#ED2B38") : (Brush)FindResource("OptionInactive"), BorderThickness = new(drivingSubpage == i ? 3 : 0,0,0,0), BorderBrush = Brush("#ED2B38") };
                AutomationProperties.SetAutomationId(button, "DrivingSubpage" + i);
                button.Click += (_, _) => { drivingSubpage = section; pages.Remove(index); ShowPage(index, false); };
                Place(canvas, button, PanelGroupLeft(availableWidth, sections.Length * sectionWidth + (sections.Length - 1) * 18) + i * (sectionWidth + 18), y, sectionWidth, 38);
            }
            y += 56;
        }
        double cardWidth = maxPanelWidth;
        int columns = Math.Max(1, Math.Min(cards.Length, (int)Math.Floor((width + 18) / (cardWidth + 18))));
        double groupLeft = PanelGroupLeft(availableWidth, columns * cardWidth + (columns - 1) * 18);
        double groupWidth = columns * cardWidth + (columns - 1) * 18;
        Caption(canvas, names[index].ToUpperInvariant(), groupLeft, 46, groupWidth, 46, 32, true, "#F5F6F8", center: true);
        Caption(canvas, hints[index], groupLeft, 99, groupWidth, 24, 15, false, "#8B93A1", center: true);
        double[] cy = Enumerable.Repeat(y, columns).ToArray();
        foreach (var card in cards)
        {
            int col = Array.IndexOf(cy, cy.Min()); double x = groupLeft + col * (cardWidth + 18);
            var settings = card.Controls.Select(id => state.Controls.Single(c => c.Id == id)).Where(c => !c.Hidden).ToArray();
            double noteHeight = string.IsNullOrEmpty(card.Note) ? 0 : new FormattedText(card.Note, System.Globalization.CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, new Typeface((FontFamily)Application.Current.FindResource("BodyFont"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal), 12, Brushes.White, VisualTreeHelper.GetDpi(this).PixelsPerDip) { MaxTextWidth = cardWidth - 44 }.Height + 12;
            double height = SectionHeaderHeight + 12 + 17 + settings.Sum(s => RowHeight(s, cardWidth - 44)) + noteHeight;
            RulePanel(canvas, x, cy[col], cardWidth, height, card.Title);
            double ry = cy[col] + SectionHeaderHeight + 12;
            foreach (var setting in settings) { double rh = RowHeight(setting, cardWidth - 44); Place(canvas, SettingView(setting, cardWidth - 44), x + 22, ry, cardWidth - 44, rh); ry += rh; }
            if (noteHeight > 0) Caption(canvas, card.Note, x + 22, cy[col] + height - noteHeight - 4, cardWidth - 44, noteHeight, 12, false, "#8B93A1", true);
            cy[col] += height + 18;
        }
        canvas.Height = cy.Max() + 10;
    }
    private FrameworkElement SettingView(Setting setting, double width, bool quick = false)
    {
        var row = new Canvas(); double height = RowHeight(setting, width, quick);
        var labelSize = SettingLabelSize(setting, width, quick);
        double fieldTop = labelSize.Height + (quick ? 4 : 12);
        void Label(double top, double boxHeight)
        {
            var label = Text(setting.Label, quick ? 14 : 15, color: "#E8EAED");
            label.TextTrimming = TextTrimming.None;
            AutomationProperties.SetAutomationId(label, "SettingLabel" + setting.Id);
            Place(row, label, 0, top, labelSize.Width, boxHeight);
            if (setting.Details.Length > 0) Place(row, SettingHelp(setting), labelSize.Width + 4, top + Math.Max(0, (boxHeight - 20) / 2), 20, 20);
        }
        FrameworkElement field;
        if (setting.Kind == "Toggle")
        {
            Label((height - 4 - labelSize.Height) / 2, labelSize.Height);
            var check = new CheckBox { ToolTip = setting.Hint, MinHeight = 0, Padding = new(0) };
            check.SetBinding(ToggleButton.IsCheckedProperty, new Binding(nameof(Setting.IsOn)) { Source = setting, Mode = BindingMode.TwoWay }); field = check;
            Place(row, check, width - 31, (height - 4 - 24) / 2, 24, 24);
        }
        else if (setting.Kind == "Action")
        {
            var run = new Button { Content = "RUN BENCHMARK", Style = (Style)FindResource("PlayButton"), FontSize = 16, Padding = new(0) };
            run.Click += async (_, _) => await Launch(true); field = run; Place(row, run, 0, 3, width, 44);
            Caption(row, "LAST RESULT", 0, 65, width, 26, 14, true, "#8B93A1");
            if (!string.IsNullOrEmpty(state.BenchmarkFps))
            {
                Caption(row, state.BenchmarkFps + " FPS", 0, 96, width, 48, 32, true);
                Caption(row, state.BenchmarkLows, 0, 148, width, 26);
                Caption(row, state.BenchmarkAbout, 0, 178, width, 50, 13, false, "#8B93A1", true);
            }
            else Caption(row, "Run an event to measure average FPS and frame-time lows.", 0, 104, width, 70, 16, false, "#8B93A1", true);
            AddBenchmarkDetails(row, width);
        }
        else if (setting.Kind == "Button")
        {
            Label(quick ? 0 : 6, labelSize.Height);
            var button = new Button { Content = setting.Options.FirstOrDefault() ?? setting.Label, Padding = new(12,0,12,0) };
            ConfigureNativeButton(button, setting);
            field = button; Place(row, field, 0, fieldTop, width, 38);
        }
        else if (setting.Kind == "Slider")
        {
            Label(quick ? 0 : 6, labelSize.Height);
            double valueWidth = SliderValueWidth(setting);
            var value = Text(setting.Selected + setting.Unit, 14); value.TextAlignment = TextAlignment.Right; value.TextWrapping = TextWrapping.NoWrap;
            AutomationProperties.SetAutomationId(value, "SettingValue" + setting.Id);
            Place(row, value, width - valueWidth, quick ? 0 : 6, valueWidth, 20);
            var slider = new Slider { Minimum = setting.Minimum, Maximum = setting.Maximum, TickFrequency = 1, IsSnapToTickEnabled = true, Value = setting.Selected, Style = (Style)FindResource("SettingSlider") };
            slider.ValueChanged += (_, _) => { suppress = true; setting.Selected = (int)Math.Round(slider.Value); suppress = false; value.Text = setting.Selected + setting.Unit; ++revision; SetStatus("Unsaved changes"); };
            slider.LostMouseCapture += (_, _) => { if (!slider.IsMouseCaptureWithin && state.Controls.Contains(setting)) SettingChanged(setting, new(nameof(Setting.Selected))); };
            slider.KeyUp += (_, _) => { if (state.Controls.Contains(setting)) SettingChanged(setting, new(nameof(Setting.Selected))); };
            field = slider; Place(row, field, 0, fieldTop, width, 38);
        }
        else if (setting.Kind == "Binding")
        {
            Label(quick ? 0 : 6, labelSize.Height);
            var kbText = setting.Options.ElementAtOrDefault(0) ?? "";
            var padText = setting.Options.ElementAtOrDefault(1) ?? "";
            // An axis (and a trigger the runtime reads as an axis) has no remappable pad button, but its
            // keyboard key still is a binding: only the right chip is locked.
            bool padLocked = setting.Tag.EndsWith("@ro");
            var kb = new Button { Content = kbText.Length == 0 ? "\u2014" : kbText, Padding = new(10, 0, 10, 0), ToolTip = "Click, then press the key to use. Esc cancels." };
            var pad = new Button { Content = padLocked ? padText : PadDisplay(padText), Padding = new(10, 0, 10, 0) };
            kb.Click += async (_, _) => await ActivateBinding(kb, setting, 0);
            if (padLocked) { pad.IsEnabled = false; pad.ToolTip = "Read as an axis by the game: it cannot be remapped."; }
            else pad.ToolTip = "Click, then press the controller button to use. Esc cancels.";
            pad.Click += async (_, _) => await ActivateBinding(pad, setting, 1);
            var chips = new Grid();
            chips.ColumnDefinitions.Add(new ColumnDefinition());
            chips.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(8) });
            chips.ColumnDefinitions.Add(new ColumnDefinition());
            Grid.SetColumn(kb, 0); Grid.SetColumn(pad, 2);
            chips.Children.Add(kb); chips.Children.Add(pad);
            field = chips; Place(row, field, 0, fieldTop, width, 38);
        }
        else if (setting.Kind == "Info")
        {
            // A device row (@dev:N) is red while it is the pad the launcher reads, and a double-click
            // makes it that pad. Other Info rows are plain read-only text.
            bool device = setting.Tag.StartsWith("@dev:");
            bool red = setting.Tag == "@devhint" || (device && setting.Selected != 0);
            var body = Text(setting.Options.FirstOrDefault() ?? "", 14, color: red ? "#ED2B38" : "#B9C0CC");
            body.TextWrapping = TextWrapping.Wrap;
            if (device)
            {
                body.Cursor = System.Windows.Input.Cursors.Hand;
                body.ToolTip = "Double-click to read this pad.";
                body.MouseLeftButtonDown += async (_, e) => { if (e.ClickCount == 2) await ActivateDevice(setting); };
            }
            Place(row, body, 0, 2, width, height - 10);
            field = body;
        }
        else if (setting.Kind == "Monitor")
        {
            // Live: the window's own 16 ms timer reads the pad in this process, no backend call and no click.
            var view = BuildMonitorView(setting.Options.FirstOrDefault() ?? "");
            view.ToolTip = "Live from the pad in use. A stick, trigger or d-pad that never moves here is not reaching the game either.";
            Place(row, view, 0, 0, width, height - 8);
            field = view;
        }
        else
        {
            Label(quick ? 0 : 6, labelSize.Height);
            if (setting.Kind == "Text")
            {
                var box = new TextBox { MaxLength = 20 };
                box.SetBinding(TextBox.TextProperty, new Binding(nameof(Setting.Text)) { Source = setting, Mode = BindingMode.TwoWay, UpdateSourceTrigger = UpdateSourceTrigger.PropertyChanged }); field = box;
            }
            else
            {
                var combo = new ComboBox { ItemsSource = setting.Options };
                combo.SetBinding(Selector.SelectedIndexProperty, new Binding(nameof(Setting.Selected)) { Source = setting, Mode = BindingMode.TwoWay }); field = combo;
            }
            Place(row, field, 0, fieldTop, width, 38);
        }
        if (!quick) Place(row, new Border { Background = Brush("#2E2E31") }, 0, height - 2, width, 1);
        field.IsEnabled = setting.Enabled;
        AutomationProperties.SetName(field, setting.Label); AutomationProperties.SetAutomationId(field, (quick ? "Quick" : "Setting") + setting.Id);
        row.ToolTip = setting.Hint; row.MouseEnter += (_, _) => ShowStatusHint(setting.Hint); row.MouseLeave += (_, _) => RestoreStatus();
        return row;
    }
    private (string[] Labels, string[] Values, double LabelWidth, double ValueOffset, double Width) MeasureGameStatus(double availableWidth)
    {
        string[] labels = ["Game version", "Renderer", "Hardware", "Runtime"];
        string[] values = [state.FoldersOk ? "1.28" : "Select game folders", "Vulkan 1.3  /  native x64", string.IsNullOrEmpty(state.HardwareProblem) ? "No blocking issue" : "Needs attention", state.RuntimeAvailable ? "Available" : "Runtime not bundled"];
        double Measure(string value, double size, bool heading = false)
        {
            var text = Text(value, size, heading); text.TextWrapping = TextWrapping.NoWrap;
            text.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            return Math.Ceiling(text.DesiredSize.Width);
        }
        double labelWidth = Math.Max(125, labels.Max(value => Measure(value, 14)));
        double valueOffset = 22 + labelWidth + 15;
        double sw = Math.Min(availableWidth, Math.Max(valueOffset + values.Max(value => Measure(value, 14)) + 22,
            Measure(preview ? "PREVIEW LAUNCH" : "PLAY", 23, true) + 44));
        return (labels, values, labelWidth, valueOffset, sw);
    }
    private void BuildHome(Canvas canvas, double left, double width)
    {
        var (labels, values, labelWidth, valueOffset, sw) = MeasureGameStatus(width);
        double foldersWidth = width - sw - 24;
        bool besideStatus = !foldersExpanded || foldersWidth >= sw;
        double actualFoldersWidth = Math.Min(sw, besideStatus ? foldersWidth : width);
        double groupWidth = besideStatus ? sw + 24 + actualFoldersWidth : sw;
        left = PanelGroupLeft(canvas.Width, groupWidth);
        double logoLeft = left + (groupWidth - 500) / 2;
        Place(canvas, new Border { Background = Brush("#454547"), OpacityMask = new ImageBrush(Image("brand/logo-red.png")) { Stretch = Stretch.Fill } }, logoLeft, 62, 500, 73);
        // Match the visible logo edge, excluding its 8-pixel transparent inset.
        double logoInset = 500 * 8d / 880;
        Caption(canvas, "NEVER DRIVE ALONE", logoLeft + logoInset, 151, 500 - logoInset, 29, 22, true, "#F5F6F8");
        double statusShift = 48 - SectionHeaderHeight - 12;
        RulePanel(canvas, left, 230, sw, 198 - statusShift, "GAME STATUS");
        for (int k = 0; k < 4; ++k)
        {
            Caption(canvas, labels[k], left + 22, 278 - statusShift + k * 35, labelWidth, 29, 14, false, "#8B93A1");
            var statusValue = Caption(canvas, values[k], left + valueOffset, 278 - statusShift + k * 35, sw - valueOffset - 22, 29, 14, false, k == 0 && state.FoldersOk ? "#5AC878" : "#E8EAED");
            if (k == 2 && !string.IsNullOrEmpty(state.HardwareProblem))
            {
                statusValue.ToolTip = state.HardwareProblem; statusValue.Cursor = Cursors.Hand;
                statusValue.MouseLeftButtonUp += (_, _) => ShowDetails("HARDWARE CHECK", state.HardwareProblem);
                AutomationProperties.SetName(statusValue, state.HardwareProblem);
            }
            if (k == 0 && state.FoldersOk)
            {
                var verified = new DottedCaptionGlyph { Glyph = "Checkmark", Fill = Brush("#5AC878"), ToolTip = "Game files verified" };
                AutomationProperties.SetName(verified, "Game files verified");
                var version = Text(values[0], 14); version.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
                Place(canvas, verified, left + valueOffset + Math.Ceiling(version.DesiredSize.Width) + 10, 278 - statusShift + 6.5, 16, 16);
            }
            if (k < 3) Place(canvas, new Border { Background = Brush("#313134") }, left + 22, 310 - statusShift + k * 35, sw - 44, 1);
        }
        var play = new Button { Content = preview ? "PREVIEW LAUNCH" : "PLAY", Style = (Style)FindResource("PlayButton"), FontSize = 23, Padding = new(0), BorderThickness = new(0), Background = Brush("#ED2B38"), IsEnabled = preview || (state.FoldersOk && string.IsNullOrEmpty(state.HardwareProblem)), ToolTip = string.IsNullOrEmpty(state.HardwareProblem) ? "Save and start the game." : state.HardwareProblem };
        play.Click += PlayClick; AutomationProperties.SetAutomationId(play, "HomePlay"); Place(canvas, play, left, 452 - statusShift, sw, 44);
        Caption(canvas, preview ? "Interface preview only. No game will be started." : "Your settings are saved when you start the game.", left, 524 - statusShift, sw, 24, 13, false, "#8B93A1");
        // Paths share the status row on wide windows and flow below it on narrow ones.
        double foldersY = besideStatus ? 230 : 574;
        double foldersHeight = BuildFolders(canvas, besideStatus ? left + sw + 24 : left, foldersY, actualFoldersWidth);
        // Quick Settings stays bottom-docked and is pushed down by this upper section.
        canvas.Height = Math.Max(548, foldersY + foldersHeight);
    }
    private double BuildFolders(Canvas canvas, double left, double y, double width)
    {
        double bodyHeight = 108 + (state.FoldersOk ? 0 : 42);
        bool opening = animateFoldersOpening && foldersExpanded;
        animateFoldersOpening = false;
        var body = new Canvas { Width = width, Height = foldersExpanded && !opening ? bodyHeight : 0, Opacity = foldersExpanded && !opening ? 1 : 0, ClipToBounds = true, Visibility = foldersExpanded ? Visibility.Visible : Visibility.Collapsed };
        AutomationProperties.SetAutomationId(body, "GameFoldersBody");
        body.SizeChanged += (_, _) => canvas.Height = Math.Max(548, y + SectionHeaderHeight + body.ActualHeight);
        Place(canvas, body, left, y + SectionHeaderHeight, width, body.Height);
        var headerContent = new Grid { Width = width - 40, Height = SectionHeaderHeight };
        var title = Text("GAME FILES", 15, true, "#D7D7D9"); title.VerticalAlignment = VerticalAlignment.Center;
        headerContent.Children.Add(title);
        var arrowRotation = new RotateTransform(foldersExpanded ? 180 : 0);
        headerContent.Children.Add(new DottedCaptionGlyph { Glyph = "ChevronDown", Fill = Brush("#B5B5BE"), Width = 16, Height = 16, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center, RenderTransformOrigin = new(.5,.5), RenderTransform = arrowRotation });
        var toggle = new Button { Content = headerContent, Padding = new(20,0,20,0), Background = Brushes.Transparent, BorderThickness = new(0), ToolTip = foldersExpanded ? "Hide game folders" : "Show or change game folders" };
        AutomationProperties.SetName(toggle, foldersExpanded ? "Hide game folders" : "Show game folders");
        AutomationProperties.SetAutomationId(toggle, "GameFoldersToggle");
        int animationRevision = 0;
        void AnimateBody(bool expand)
        {
            int token = ++animationRevision;
            foldersAnimating = true;
            body.Visibility = Visibility.Visible; body.IsHitTestVisible = expand;
            toggle.ToolTip = expand ? "Hide game folders" : "Show or change game folders";
            AutomationProperties.SetName(toggle, expand ? "Hide game folders" : "Show game folders");
            if (expand) body.Height = bodyHeight;
            var fade = new DoubleAnimation(body.Opacity, expand ? 1 : 0, TimeSpan.FromSeconds(.12));
            Timeline.SetDesiredFrameRate(fade, refreshRate);
            fade.Completed += (_, _) =>
            {
                if (token != animationRevision) return;
                foldersAnimating = false;
                if (expand) { arrangeHome?.Invoke(); return; }
                body.Visibility = Visibility.Collapsed;
                pages.Remove(0); ShowPage(0, false);
            };
            body.BeginAnimation(OpacityProperty, fade);
            arrowRotation.Angle = expand ? 180 : 0;
        }
        toggle.Click += (_, _) =>
        {
            if (!foldersExpanded && body.Visibility == Visibility.Collapsed)
            {
                foldersExpanded = true; animateFoldersOpening = true; foldersAnimating = true;
                pages.Remove(0); ShowPage(0, false); return;
            }
            foldersExpanded = !foldersExpanded; AnimateBody(foldersExpanded);
        };
        Place(canvas, toggle, left, y, width, SectionHeaderHeight);
        Place(canvas, new Border { Background = Brush("#ED2B38"), IsHitTestVisible = false }, left, y, 3, SectionHeaderHeight);
        for (int visualRow = 0; visualRow < 2; ++visualRow)
        {
            int i = 1 - visualRow;
            int which = i; double fy = 12 + visualRow * 48, rw = width - 44;
            Caption(body, i == 0 ? "Update 1.28" : "Base game", 22, fy, 96, 38, 14);
            string selectedPath = i == 0 ? state.UpdatePath : state.BasePath;
            var path = new TextBox { Text = selectedPath, ToolTip = selectedPath, IsReadOnly = true, FontSize = 14, BorderThickness = new(0) };
            AutomationProperties.SetName(path, i == 0 ? "Update folder (version 1.28)" : "Base game folder");
            AutomationProperties.SetAutomationId(path, "GameFolder" + i);
            Place(body, path, 126, fy, rw - 154, 38);
            Place(body, new Border { Background = Brush(state.FoldersOk ? "#5AC878" : "#F0AA3C") }, 126, fy, 3, 38);
            var browse = new Button { Content = new DottedCaptionGlyph { Glyph = "Search", Fill = Brushes.White, Width = 20, Height = 20 }, Style = (Style)FindResource("FieldButton"), ToolTip = "Browse", Padding = new(0) };
            AutomationProperties.SetName(browse, i == 0 ? "Browse for update folder" : "Browse for base game folder");
            AutomationProperties.SetAutomationId(browse, "BrowseGameFolder" + i);
            Place(body, browse, 22 + rw - 38, fy, 38, 38);
            browse.Click += async (_, _) =>
            {
                var dialog = new OpenFolderDialog { Title = which == 0 ? "Choose the DRIVECLUB v1.28 update" : "Choose the DRIVECLUB base game" };
                if (dialog.ShowDialog(this) != true) return;
                if (which == 0) state.UpdatePath = dialog.FolderName; else state.BasePath = dialog.FolderName;
                try { Adopt(await backend.Call("validate", state)); SetStatus(state.FoldersOk ? "Game folders found. Unsaved changes." : state.FolderProblem, !state.FoldersOk); } catch (Exception ex) { SetStatus(ex.Message, true); }
            };
        }
        if (!state.FoldersOk) Caption(body, state.FolderProblem, 22, 108, width - 44, 42, 11, false, "#F0AA3C", true);
        if (opening) body.Loaded += (_, _) => AnimateBody(true);
        return SectionHeaderHeight + (foldersExpanded && !opening ? bodyHeight : 0);
    }
    private async void SaveClick(object? sender, RoutedEventArgs e)
    {
        if (busy) return;
        try { SaveButton.IsEnabled = false; Adopt(await backend.Call("apply", state)); OperationStatus("Saved."); }
        catch (Exception ex) { SetStatus(ex.Message, true); }
        finally { SaveButton.IsEnabled = true; }
    }
    private async void ResetClick(object? sender, RoutedEventArgs e)
    {
        if (busy || currentPage == ModsPage) return;
        ++revision;
        state.ResetPage = sourcePages[currentPage];
        state.ResetSubpage = names[currentPage] == "Driving" ? drivingSubpage : -1;
        string pageName = names[currentPage] == "Driving" ? (drivingSubpage == 0 ? "Assists and view" : drivingSubpage == 1 ? "Wheel and force feedback" : "Controller and keyboard") : names[currentPage];
        try { ResetButton.IsEnabled = false; Adopt(await backend.Call("reset-page", state)); SetStatus($"{pageName} defaults restored. Unsaved changes."); }
        catch (Exception ex) { SetStatus(ex.Message, true); }
        finally { ResetButton.IsEnabled = true; }
    }
    private async void PlayClick(object? sender, RoutedEventArgs e) => await Launch(false);
    private void MinimizeClick(object sender, RoutedEventArgs e) => SystemCommands.MinimizeWindow(this);
    private void MaximizeClick(object sender, RoutedEventArgs e) { if (WindowState == WindowState.Maximized) SystemCommands.RestoreWindow(this); else SystemCommands.MaximizeWindow(this); }
    private void CloseClick(object sender, RoutedEventArgs e) => Close();
    private void OpenLaunchSplash()
    {
        busyOverlayParent = (Panel)BusyOverlay.Parent;
        busyOverlayParent.Children.Remove(BusyOverlay);
        double width = Math.Min(960, ActualWidth), height = Math.Min(600, ActualHeight);
        mayCloseSplash = false;
        launchSplash = new Window
        {
            Title = "DRIVECLUB - Loading", Icon = Icon, Width = width, Height = height,
            Left = Left + (ActualWidth - width) / 2, Top = Top + (ActualHeight - height) / 2,
            WindowStartupLocation = WindowStartupLocation.Manual,
            WindowStyle = WindowStyle.None, ResizeMode = ResizeMode.NoResize,
            Background = Brush("#101014"), Topmost = true, ShowInTaskbar = true,
            FontFamily = FontFamily, UseLayoutRounding = true, SnapsToDevicePixels = true,
            Content = BusyOverlay
        };
        switchToGame = new Button
        {
            Content = "Switch to game", Style = (Style)FindResource("FieldButton"),
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top,
            Margin = new Thickness(24), Padding = new Thickness(16, 0, 16, 0),
            Height = 36, FontSize = 12, Visibility = Visibility.Collapsed
        };
        switchToGame.Click += (_, _) => launchSession?.SwitchToGame();
        BusyOverlay.Children.Add(switchToGame);
        launchSplash.Closing += (_, e) =>
        {
            if (mayCloseSplash) return;
            e.Cancel = true;
            if (launchedGameId > 0) launchSession?.SwitchToGame();
        };
        Hide();
        launchSplash.Show();
        launchSplash.UpdateLayout();
    }
    private void CloseLaunchSplash()
    {
        if (launchSplash == null) return;
        mayCloseSplash = true;
        launchSplash.Content = null;
        launchSplash.Close();
        launchSplash = null;
        if (switchToGame != null) BusyOverlay.Children.Remove(switchToGame);
        switchToGame = null;
        busyOverlayParent?.Children.Add(BusyOverlay);
        busyOverlayParent = null;
    }
    private async Task FadeLaunchSplash()
    {
        if (launchSplash == null) return;
        var finished = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(160));
        fade.Completed += (_, _) => finished.TrySetResult(true);
        launchSplash.BeginAnimation(OpacityProperty, fade);
        await finished.Task;
    }
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool AllowSetForegroundWindow(uint processId);
    private static void FocusLaunchedGame(int processId)
    {
        try
        {
            using var game = Process.GetProcessById(processId);
            game.Refresh();
            if (!game.HasExited && game.MainWindowHandle != IntPtr.Zero)
            {
                AllowSetForegroundWindow((uint)processId);
                SetForegroundWindow(game.MainWindowHandle);
            }
        }
        catch (ArgumentException) { } catch (InvalidOperationException) { }
    }
    private void StopLoadingAnimation()
    {
        spinnerTimer?.Stop();
        ((RotateTransform)Spinner.RenderTransform).BeginAnimation(RotateTransform.AngleProperty, null);
    }
    private async Task Launch(bool benchmark)
    {
        if (busy) return;
        busy = true; CloseButton.IsEnabled = SaveButton.IsEnabled = ResetButton.IsEnabled = false;
        launchedGameId = 0;
        BusyOverlay.Visibility = Visibility.Visible;
        Progress.IsIndeterminate = false; Progress.Value = 0; PercentText.Text = "0%";
        ProgressText.Text = preview ? "Interface preview. No game is being prepared or launched." : "Preparing your game files...";
        Task<LauncherState>? operation = null;
        try
        {
            OpenLaunchSplash();
            StartLoadingAnimation();
            if (preview)
            {
                for (int i = 0; i <= 100; i += 5) { await Task.Delay(160); Progress.Value = i; PercentText.Text = i + "%"; }
                CloseLaunchSplash(); Show(); Activate(); SetStatus("Preview finished. No game was started.");
            }
            else
            {
                launchSession = new LaunchSession();
                var started = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
                var gameStarted = new Progress<int>(pid =>
                {
                    launchedGameId = pid;
                    Progress.IsIndeterminate = true; PercentText.Text = "";
                    ProgressText.Text = state.StartupSignalSupported ? "Loading DRIVECLUB..." : "DRIVECLUB is starting. Switch to its window when ready.";
                    if (switchToGame != null) switchToGame.Visibility = Visibility.Visible;
                    started.TrySetResult(pid);
                });
                var progress = new Progress<(int Percent, string Text)>(p =>
                {
                    if (launchedGameId == 0) { Progress.Value = p.Percent; PercentText.Text = p.Percent + "%"; ProgressText.Text = p.Text; }
                });
                operation = backend.Call(benchmark ? "benchmark" : "run", state, progress, launchSession.EventName, gameStarted);
                if (benchmark)
                {
                    // The benchmark backend stays alive until the run finishes. Read its child PID
                    // from progress, so waiting for that backend cannot keep the splash over the race.
                    if (await Task.WhenAny(operation, started.Task) == started.Task)
                    {
                        using var stopWaiting = new CancellationTokenSource();
                        var handoff = launchSession.WaitForFrameAsync(await started.Task, TimeSpan.FromMinutes(3), stopWaiting.Token);
                        if (await Task.WhenAny(operation, handoff) == handoff)
                        {
                            await handoff;
                            await FadeLaunchSplash(); CloseLaunchSplash(); StopLoadingAnimation();
                            FocusLaunchedGame(launchedGameId);
                        }
                        else
                        {
                            stopWaiting.Cancel();
                            try { await handoff; } catch (OperationCanceledException) { } catch (IOException) { }
                        }
                    }
                    Adopt(await operation);
                    CloseLaunchSplash(); Show(); Activate(); SetStatus("Benchmark complete.");
                }
                else
                {
                    Adopt(await operation);
                    launchedGameId = state.GameProcessId;
                    if (launchedGameId <= 0) throw new IOException("The launcher backend did not return the game process. Update the complete launcher package.");
                    Progress.IsIndeterminate = true; PercentText.Text = "";
                    ProgressText.Text = state.StartupSignalSupported ? "Loading DRIVECLUB..." : "DRIVECLUB is starting. Switch to its window when ready.";
                    if (switchToGame != null) switchToGame.Visibility = Visibility.Visible;
                    await launchSession.WaitForFrameAsync(launchedGameId, TimeSpan.FromMinutes(3));
                    await FadeLaunchSplash(); CloseLaunchSplash();
                    FocusLaunchedGame(launchedGameId);
                    allowClose = true; Close(); return;
                }
            }
        }
        catch (Exception ex)
        {
            CloseLaunchSplash(); StopLoadingAnimation(); BusyOverlay.Visibility = Visibility.Collapsed;
            Show(); Activate(); SetStatus(ex.Message, true);
            // Keep the IPC operation owned/observed; a timeout never kills an active benchmark.
            if (operation != null && !operation.IsCompleted) try { await operation; } catch (Exception) { }
        }
        finally
        {
            launchSession?.Dispose(); launchSession = null;
            CloseLaunchSplash(); StopLoadingAnimation();
            BusyOverlay.Visibility = Visibility.Collapsed; Progress.IsIndeterminate = false;
            busy = false; CloseButton.IsEnabled = SaveButton.IsEnabled = ResetButton.IsEnabled = true;
        }
    }
    private Storyboard StartLoadingAnimation()
    {
        double width = Math.Max(960, BusyOverlay.ActualWidth), height = Math.Max(600, BusyOverlay.ActualHeight);
        SwipeLayer.Visibility = Visibility.Visible;
        var first = (TranslateTransform)SwipeOne.RenderTransform;
        var second = (TranslateTransform)((TransformGroup)SwipeTwo.RenderTransform).Children[1];
        first.X = -width; first.Y = height;
        second.X = width; second.Y = -height;
        const double movementStart = .3, movementSeconds = .3, landingSeconds = movementStart + movementSeconds;
        var intro = new Storyboard { Duration = TimeSpan.FromSeconds(1.6) };
        Timeline.SetDesiredFrameRate(intro, refreshRate);
        void Add(DependencyObject target, PropertyPath property, AnimationTimeline animation)
        {
            Storyboard.SetTarget(animation, target);
            Storyboard.SetTargetProperty(animation, property);
            intro.Children.Add(animation);
        }
        foreach (var tuple in new[] { (SwipeOne, -width, height, "RenderTransform.X", "RenderTransform.Y"), (SwipeTwo, width, -height, "RenderTransform.Children[1].X", "RenderTransform.Children[1].Y") })
        {
            var x = Animation(tuple.Item2, 0, movementSeconds); x.BeginTime = TimeSpan.FromSeconds(movementStart);
            var y = Animation(tuple.Item3, 0, movementSeconds); y.BeginTime = TimeSpan.FromSeconds(movementStart);
            Add(tuple.Item1, new PropertyPath(tuple.Item4), x);
            Add(tuple.Item1, new PropertyPath(tuple.Item5), y);
        }
        foreach (var swipe in new[] { SwipeOne, SwipeTwo })
        {
            var alpha = new DoubleAnimationUsingKeyFrames { Duration = TimeSpan.FromSeconds(1.6) };
            alpha.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)));
            alpha.KeyFrames.Add(new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(landingSeconds))));
            alpha.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(1.1))));
            Add(swipe, new PropertyPath(OpacityProperty), alpha);
        }
        var scale = (ScaleTransform)Stamp.RenderTransform;
        var scaleX = Animation(4, 1, movementSeconds); scaleX.BeginTime = TimeSpan.FromSeconds(movementStart);
        var scaleY = Animation(4, 1, movementSeconds); scaleY.BeginTime = TimeSpan.FromSeconds(movementStart);
        Add(Stamp, new PropertyPath("RenderTransform.ScaleX"), scaleX);
        Add(Stamp, new PropertyPath("RenderTransform.ScaleY"), scaleY);
        var stampAlpha = new DoubleAnimationUsingKeyFrames { Duration = TimeSpan.FromSeconds(1.6) };
        stampAlpha.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        stampAlpha.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(movementStart))));
        stampAlpha.KeyFrames.Add(new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(landingSeconds))));
        stampAlpha.KeyFrames.Add(new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(1.1))));
        stampAlpha.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(1.2))));
        Add(Stamp, new PropertyPath(OpacityProperty), stampAlpha);
        // Preserve the original plate travel (0.3 to 0.6 s); move the stamp landing to match it.
        intro.Begin(this, HandoffBehavior.SnapshotAndReplace, false);
        if (spinnerFrames.Count == 0) return intro;
        spinnerIndex = 0; Spinner.Source = spinnerFrames[0];
        spinnerTimer?.Stop(); spinnerTimer = new DispatcherTimer(DispatcherPriority.Render) { Interval = TimeSpan.FromMilliseconds(33) };
        spinnerTimer.Tick += (_, _) => { Spinner.Source = spinnerFrames[++spinnerIndex % spinnerFrames.Count]; }; spinnerTimer.Start();
        return intro;
    }
}

// The exact smoothstep timing curve from the first launcher visual layer.
public sealed class OriginalMenuEase : EasingFunctionBase
{
    public OriginalMenuEase() { EasingMode = EasingMode.EaseIn; }
    protected override double EaseInCore(double t) => t * t * (3 - 2 * t);
    protected override Freezable CreateInstanceCore() => new OriginalMenuEase();
}

public sealed class InputMonitorView : System.Windows.Controls.Canvas
{
    [StructLayout(LayoutKind.Sequential)]
    private struct PadState
    {
        public uint Packet; public ushort Buttons;
        public byte LeftTrigger, RightTrigger;
        public short Lx, Ly, Rx, Ry;
    }
    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")] private static extern uint GetState(uint pad, out PadState state);

    private static SolidColorBrush B(byte r, byte g, byte b) => new(System.Windows.Media.Color.FromRgb(r, g, b));
    private static readonly SolidColorBrush Rail = B(35, 37, 43);
    private static readonly SolidColorBrush Accent = B(237, 43, 56);
    private static readonly SolidColorBrush Muted = B(139, 147, 161);
    private static readonly SolidColorBrush Bright = B(245, 246, 248);
    private static readonly SolidColorBrush Ring = B(120, 128, 142);

    // The dead zones of the rows above, in the order the runtime keeps them:
    // 0/1 left stick inner/outer, 2/3 right stick, 4/5 L2, 6/7 R2.
    private readonly int[] dz;
    private readonly Border[] boxes = new Border[4];
    private readonly TextBlock[] axisLabels = new TextBlock[4];
    private readonly System.Windows.Shapes.Ellipse[][] rings = new System.Windows.Shapes.Ellipse[4][];
    private readonly System.Windows.Shapes.Ellipse[] dots = new System.Windows.Shapes.Ellipse[4];
    private readonly TextBlock[] padLabels = new TextBlock[2];
    private readonly Border[] tracks = new Border[2];
    private readonly Border[] bars = new Border[2];
    private readonly Border[] ticks = new Border[4];
    private readonly Border[] dpad = new Border[4];
    private readonly Border padFrame;
    private readonly Border[] pills = new Border[12];
    private readonly TextBlock[] pillLabels = new TextBlock[12];
    private readonly TextBlock empty;

    private static readonly int[] PillMasks = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0040, 0x0080, 0x0010, 0x0020, 0, 0 };
    private static readonly string[] PillNames = { "A", "B", "X", "Y", "L1", "R1", "L3", "R3", "Menu", "View", "LT", "RT" };
    private static readonly int[] DpadMasks = { 0x0001, 0x0002, 0x0004, 0x0008 };

    // 0.3.0 lays the picture out in units of 240 wide and scales it to the card, so it follows the window.
    private const double DesignWidth = 240;
    private double u = 1;
    private bool laidOut;
    private bool xinputChecked, xinputMissing;
    // Starts false so the first SetLive() (from the constructor) really applies the whole state: with true
    // here, SetLive(true) returned early and the "no pad" line was never hidden — it then sat on top of
    // the axis labels while the live picture was drawn underneath.
    private bool live;

    private readonly int padSlot;   // -1: the first connected pad
    public InputMonitorView(int[] deadzones, int padSlot = -1)
    {
        this.padSlot = padSlot;
        dz = deadzones;
        Background = System.Windows.Media.Brushes.Transparent;
        ClipToBounds = false;
        string[] axisNames = { "LX", "LY", "RX", "RY" };
        for (int i = 0; i < 4; ++i)
        {
            axisLabels[i] = Caption(axisNames[i], 0, 0);
            Children.Add(axisLabels[i]);
            boxes[i] = new System.Windows.Controls.Border { Background = Rail, CornerRadius = new CornerRadius(6) };
            Children.Add(boxes[i]);
            rings[i] = new System.Windows.Shapes.Ellipse[2];
            for (int k = 0; k < 2; ++k)
            {
                rings[i][k] = new System.Windows.Shapes.Ellipse { Stroke = Ring, StrokeThickness = 1, Width = 0, Height = 0, Visibility = Visibility.Collapsed };
                Children.Add(rings[i][k]);
            }
            dots[i] = new System.Windows.Shapes.Ellipse { Fill = Accent, Width = 7, Height = 7 };
            Children.Add(dots[i]);
        }
        for (int i = 0; i < 2; ++i)
        {
            padLabels[i] = Caption(i == 0 ? "L2" : "R2", 0, 0);
            Children.Add(padLabels[i]);
            tracks[i] = new System.Windows.Controls.Border { Background = Rail, CornerRadius = new CornerRadius(4.5) };
            Children.Add(tracks[i]);
            bars[i] = new System.Windows.Controls.Border { Background = Accent, CornerRadius = new CornerRadius(4.5), Width = 0 };
            Children.Add(bars[i]);
            for (int k = 0; k < 2; ++k)
            {
                ticks[i * 2 + k] = new System.Windows.Controls.Border { Background = Ring, Width = 1, Height = 13 };
                Children.Add(ticks[i * 2 + k]);
            }
        }
        padFrame = new System.Windows.Controls.Border { Background = Rail, CornerRadius = new CornerRadius(4) };
        Children.Add(padFrame);
        for (int i = 0; i < 4; ++i)
        {
            // order: up, down, left, right (the masks of DpadMasks)
            dpad[i] = new System.Windows.Controls.Border { Background = Rail, CornerRadius = new CornerRadius(3) };
            Children.Add(dpad[i]);
        }
        for (int i = 0; i < 12; ++i)
        {
            pills[i] = new System.Windows.Controls.Border { Background = Rail, CornerRadius = new CornerRadius(9) };
            Children.Add(pills[i]);
            pillLabels[i] = Caption(PillNames[i], 0, 0);
            Children.Add(pillLabels[i]);
        }
        empty = Caption("No pad is sending data right now.", 0, 0);
        empty.Foreground = Muted;
        Children.Add(empty);
        SetLive(true);   // live: hide the "no pad" line before the first read
        SizeChanged += (_, _) => ApplyLayout(true);
        Loaded += (_, _) => { ApplyLayout(true); ReadPad(); };
    }

    private static TextBlock Caption(string text, double x, double y)
    {
        var t = new System.Windows.Controls.TextBlock { Text = text, FontSize = 11, Foreground = Muted };
        System.Windows.Controls.Canvas.SetLeft(t, x);
        System.Windows.Controls.Canvas.SetTop(t, y);
        return t;
    }

    private double Width_ => ActualWidth > 0 ? ActualWidth : (double.IsNaN(Width) ? DesignWidth : Width);

    // One design unit follows the card width (0.3.0: clamp(width / 240, 0.85, 2.2)).
    private void ApplyLayout(bool force)
    {
        double k = Math.Clamp(Width_ / DesignWidth, 0.85, 2.2);
        if (!force && laidOut && Math.Abs(k - u) < 0.001) return;
        u = k; laidOut = true;
        double side = 46 * u, reach = side / 2 - 5 * u;
        for (int i = 0; i < 4; ++i)
        {
            double x = i * 58 * u;
            axisLabels[i].FontSize = 11 * u;
            Canvas.SetLeft(axisLabels[i], x); Canvas.SetTop(axisLabels[i], 0);
            boxes[i].Width = side; boxes[i].Height = side; boxes[i].CornerRadius = new CornerRadius(6 * u);
            Canvas.SetLeft(boxes[i], x); Canvas.SetTop(boxes[i], 16 * u);
            for (int k2 = 0; k2 < 2; ++k2) rings[i][k2].StrokeThickness = 1 * u;
            dots[i].Width = 7 * u; dots[i].Height = 7 * u;
        }
        for (int i = 0; i < 2; ++i)
        {
            double y = (i == 0 ? 78 : 100) * u;
            padLabels[i].FontSize = 11 * u;
            Canvas.SetLeft(padLabels[i], 0); Canvas.SetTop(padLabels[i], y - 2 * u);
            tracks[i].Width = 120 * u; tracks[i].Height = 9 * u; tracks[i].CornerRadius = new CornerRadius(4.5 * u);
            Canvas.SetLeft(tracks[i], 30 * u); Canvas.SetTop(tracks[i], y);
            bars[i].Height = 9 * u; bars[i].CornerRadius = new CornerRadius(4.5 * u);
            // The fill grows from the left edge of its track, not from the origin of the view: without this
            // it started at x=0 and covered the "L2" label.
            Canvas.SetLeft(bars[i], 30 * u); Canvas.SetTop(bars[i], y);
            for (int k2 = 0; k2 < 2; ++k2)
            {
                ticks[i * 2 + k2].Width = 1 * u; ticks[i * 2 + k2].Height = 13 * u;
                Canvas.SetTop(ticks[i * 2 + k2], y - 2 * u);
            }
        }
        double cell = 15 * u;
        padFrame.Width = cell * 3; padFrame.Height = cell * 3; padFrame.CornerRadius = new CornerRadius(4 * u);
        Canvas.SetLeft(padFrame, 0); Canvas.SetTop(padFrame, 126 * u);
        for (int i = 0; i < 4; ++i)
        {
            int col = i == 2 ? 0 : i == 3 ? 2 : 1, row = i == 0 ? 0 : i == 1 ? 2 : 1;
            dpad[i].Width = cell - 2 * u; dpad[i].Height = cell - 2 * u; dpad[i].CornerRadius = new CornerRadius(3 * u);
            Canvas.SetLeft(dpad[i], col * cell + 1 * u); Canvas.SetTop(dpad[i], 126 * u + row * cell + 1 * u);
        }
        for (int i = 0; i < 12; ++i)
        {
            double x = (60 + (i % 4) * 40) * u, y = (126 + (i / 4) * 22) * u;
            pills[i].Width = 34 * u; pills[i].Height = 18 * u; pills[i].CornerRadius = new CornerRadius(9 * u);
            Canvas.SetLeft(pills[i], x); Canvas.SetTop(pills[i], y);
            pillLabels[i].FontSize = 11 * u;
            Canvas.SetLeft(pillLabels[i], x + 6 * u); Canvas.SetTop(pillLabels[i], y + 2 * u);
        }
        empty.FontSize = 12 * u;
        Canvas.SetLeft(empty, 0); Canvas.SetTop(empty, 20 * u);   // 0.3.0 puts it below the axis label row
    }

    private void SetLive(bool on)
    {
        if (live == on) return;
        live = on;
        foreach (var e in boxes) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in axisLabels) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in dots) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in padLabels) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in tracks) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in bars) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in ticks) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in pills) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in pillLabels) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        padFrame.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in dpad) e.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        foreach (var e in rings) foreach (var r in e) r.Visibility = on && r.Width > 0 ? Visibility.Visible : Visibility.Collapsed;
        empty.Visibility = on ? Visibility.Collapsed : Visibility.Visible;
    }

    // The dead-zone rows are in the runtime's units (0..127), the pad reports -32768..32767 / 0..255.
    // Feeding the raw value straight into a 0..127 scale made the dot jump to the rim at the slightest
    // deflection (a 9% push already looked like full lock).
    private const double AxisScale = 127.0 / 32768.0;
    private const double TriggerScale = 127.0 / 255.0;

    private static double MapThrough(double value, int inner, int outer)
    {
        if (outer <= inner) return value >= inner ? 1 : 0;
        double t = (value - inner) / (double)(outer - inner);
        return Math.Clamp(t, 0, 1);
    }

    // Which XInput slots have a pad right now, as a string: "1010". Used to spot a pad being plugged in.
    public static string SlotSignature()
    {
        try
        {
            var chars = new char[4];
            for (uint slot = 0; slot < 4; ++slot) chars[slot] = GetState(slot, out _) == 0 ? '1' : '0';
            return new string(chars);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        { return "0000"; }
    }

    private bool QueryPad(out PadState state)
    {
        state = default;
        if (xinputMissing) return false;
        if (!xinputChecked)
        {
            xinputChecked = true;
            try { GetState(0, out _); }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { xinputMissing = true; return false; }
        }
        if (padSlot >= 0) return GetState((uint)padSlot, out state) == 0;   // the pad chosen in the launcher
        for (uint slot = 0; slot < 4; ++slot)
            if (GetState(slot, out state) == 0) return true;
        return false;
    }

    public void ReadPad()
    {
        ApplyLayout(false);
        if (!QueryPad(out var st)) { SetLive(false); return; }
        SetLive(true);
        const double Reach = 18;   // 46u / 2 - 5u, in design units
        int[] values = { st.Lx, st.Ly, st.Rx, st.Ry };
        for (int i = 0; i < 4; ++i)
        {
            int inner = dz[(i / 2) * 2], outer = dz[(i / 2) * 2 + 1];
            double cxBox = i * 58 * u + 23 * u, cyBox = 16 * u + 23 * u;
            double reach = Reach * u;
            double ri = reach * Math.Min(Math.Max(inner, 0), 127) / 127.0, ro = reach * Math.Min(Math.Max(outer, 0), 127) / 127.0;
            for (int k = 0; k < 2; ++k)
            {
                double rad = k == 0 ? ri : ro;
                bool show = k == 0 ? ri > 1 : outer < 127;
                rings[i][k].Visibility = show ? Visibility.Visible : Visibility.Collapsed;
                if (!show) continue;
                rings[i][k].Width = rad * 2; rings[i][k].Height = rad * 2;
                Canvas.SetLeft(rings[i][k], cxBox - rad);
                Canvas.SetTop(rings[i][k], cyBox - rad);
            }
            double mag = MapThrough(Math.Abs(values[i]) * AxisScale, inner, outer);
            double signed = values[i] < 0 ? -mag : mag;
            double cx = cxBox + (i % 2 == 0 ? signed * ro : 0);
            double cy = cyBox - (i % 2 == 1 ? signed * ro : 0);
            Canvas.SetLeft(dots[i], cx - 3.5 * u);
            Canvas.SetTop(dots[i], cy - 3.5 * u);
        }
        int[] trig = { st.LeftTrigger, st.RightTrigger };
        for (int i = 0; i < 2; ++i)
        {
            int inner = dz[4 + i * 2], outer = dz[5 + i * 2];
            for (int k = 0; k < 2; ++k)
            {
                int edge = k == 0 ? inner : outer;
                bool show = edge > 0 && edge < 127;
                ticks[i * 2 + k].Visibility = show ? Visibility.Visible : Visibility.Collapsed;
                if (show) Canvas.SetLeft(ticks[i * 2 + k], 30 * u + 120 * u * edge / 127.0);
            }
            double t = MapThrough(trig[i] * TriggerScale, inner, outer);
            bars[i].Width = Math.Max(2 * u, 120 * u * Math.Min(Math.Max(outer, 0), 127) / 127.0 * t);
        }
        for (int i = 0; i < 4; ++i)
            dpad[i].Background = (st.Buttons & DpadMasks[i]) != 0 ? Accent : Rail;
        for (int i = 0; i < 12; ++i)
        {
            bool on = i == 10 ? MapThrough(st.LeftTrigger * TriggerScale, dz[4], dz[5]) > 0.4
                    : i == 11 ? MapThrough(st.RightTrigger * TriggerScale, dz[6], dz[7]) > 0.4
                    : (st.Buttons & PillMasks[i]) != 0;
            pills[i].Background = on ? Accent : Rail;
            pillLabels[i].Foreground = on ? Bright : Muted;
        }
    }
}