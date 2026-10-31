using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using DriveClub.Launcher;

namespace LauncherParityTests;
public static class Program
{
    static readonly string Root = AppContext.BaseDirectory;
    static readonly string Out = Path.Combine(Root,"parity-review");
    static readonly List<string> Checks = [];
    static object? Invoke(object o,string n,params object[] a)=>o.GetType().GetMethod(n,BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(o,a);
    static T Field<T>(object o,string n)=>(T)o.GetType().GetField(n,BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic)!.GetValue(o)!;
    static void Set(object o,string n,object v)=>o.GetType().GetField(n,BindingFlags.Instance|BindingFlags.NonPublic)!.SetValue(o,v);
    static void Check(bool ok,string name){if(!ok)throw new Exception(name);Checks.Add(name);Console.WriteLine("PASS "+name);}
    static async Task Idle(){await Application.Current.Dispatcher.InvokeAsync(()=>{},DispatcherPriority.ApplicationIdle);await Task.Delay(100);}
    static IEnumerable<DependencyObject> Desc(DependencyObject o){yield return o;for(int i=0;i<VisualTreeHelper.GetChildrenCount(o);i++)foreach(var x in Desc(VisualTreeHelper.GetChild(o,i)))yield return x;}
    static T Id<T>(MainWindow w,string id) where T:DependencyObject=>Desc((FrameworkElement)w.FindName("Root")).OfType<T>().Single(x=>AutomationProperties.GetAutomationId(x)==id);
    static void HeaderToggle(MainWindow window, Setting setting, string title)
    {
        var root=(FrameworkElement)window.FindName("Root");
        var check=Id<CheckBox>(window,"Setting"+setting.Id);
        var canvas=(Canvas)VisualTreeHelper.GetParent(check);
        var heading=canvas.Children.OfType<Border>().Single(b=>b.Child is TextBlock t && t.Text==title);
        Check(AutomationProperties.GetName(check)==setting.Label && Math.Abs(Canvas.GetTop(check)-Canvas.GetTop(heading)-7)<.1, title+" has one accessible checkbox in its header");
        Check(Canvas.GetLeft(heading)+heading.Width<=Canvas.GetLeft(check), title+" header text does not overlap its checkbox");
        Check(!Desc(root).OfType<TextBlock>().Any(t=>AutomationProperties.GetAutomationId(t)=="SettingLabel"+setting.Id), title+" has no duplicate setting row");
        Check(Equals(heading.ToolTip,check.ToolTip), title+" header exposes the checkbox hint or unavailable reason");
        heading.RaiseEvent(new MouseEventArgs(Mouse.PrimaryDevice,0){RoutedEvent=Mouse.MouseEnterEvent});
        Check(((TextBlock)window.FindName("Status")).Text==(string?)check.ToolTip, title+" header hover displays the checkbox hint");
        heading.RaiseEvent(new MouseEventArgs(Mouse.PrimaryDevice,0){RoutedEvent=Mouse.MouseLeaveEvent});
    }
    static async Task Modal(MainWindow w,string title,Action<Window> check,Action open)
    {
        Exception? failure=null;
        _=w.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle,new Action(()=>
        {
            try{var dialog=Application.Current.Windows.OfType<Window>().Single(x=>x!=w&&x.Title==title);check(dialog);dialog.Close();}
            catch(Exception ex){failure=ex;foreach(var dialog in Application.Current.Windows.OfType<Window>().Where(x=>x!=w).ToArray())dialog.Close();}
        }));
        open();await Idle();if(failure!=null)throw failure;
    }
    static async Task PlayArgument(LauncherState state)
    {
        string child=Path.Combine(Root,"play-argument-fixture");Directory.CreateDirectory(child);
        foreach(string f in Directory.GetFiles(Root))
            if(new[]{".exe",".dll",".json"}.Contains(Path.GetExtension(f)))File.Copy(f,Path.Combine(child,Path.GetFileName(f)),true);
        // Rename the test apphost: its embedded managed assembly still points to
        // this harness, which handles IPC without starting a game.
        File.Copy(Environment.ProcessPath!,Path.Combine(child,"DriveClub.Backend.exe"),true);
        state.FoldersOk=true;state.HardwareProblem="";
        foreach(var c in state.Controls)if(c.Tag=="@updatecheck")c.Selected=0;
        File.WriteAllText(Path.Combine(child,"backend-state.json"),JsonSerializer.Serialize(state,Backend.JsonOptions));
        File.WriteAllText(Path.Combine(child,"launcher.ini"),"auto_play=0\ncheck_updates=0\n");
        async Task Run(bool preview)
        {
            string marker=Path.Combine(child,"play-requested.txt");File.Delete(marker);
            var start=new ProcessStartInfo(Path.Combine(child,"DriveClub.Launcher.exe")){UseShellExecute=false,WorkingDirectory=child};start.ArgumentList.Add("--play");if(preview)start.ArgumentList.Add("--preview");
            using var process=Process.Start(start)!;
            try
            {
                if(preview)await Task.Delay(2200);
                else for(int i=0;i<150&&!File.Exists(marker)&&!process.HasExited;i++)await Task.Delay(100);
                Check(File.Exists(marker)!=preview,preview?"--preview suppresses --play; no game request":"--play reaches native launch even when auto_play=0");
            }
            finally{if(!process.HasExited){process.Kill(entireProcessTree:true);await process.WaitForExitAsync();}}
        }
        await Run(false);await Run(true);
    }
    [STAThread] public static int Main(string[] args)
    {
        if(args.Length>0&&args[0].StartsWith("--ui-"))
        {
            string output=args[^1];
            if(args[0]=="--ui-run")
            {File.WriteAllText(Path.Combine(Root,"play-requested.txt"),"Launch intercepted by test fixture; no game started.");File.WriteAllText(output,"{\"Error\":\"Controlled test launch intercepted\"}");return 1;}
            File.Copy(Path.Combine(Root,"backend-state.json"),output,true);return 0;
        }
        Directory.CreateDirectory(Out);
        var app=new App();app.InitializeComponent();typeof(Application).GetField("_startupUri",BindingFlags.Instance|BindingFlags.NonPublic)!.SetValue(app,null);app.ShutdownMode=ShutdownMode.OnExplicitShutdown;
        var window=new MainWindow();
        window.ContentRendered+=async(_,_)=>
        {
            try
            {
                for(int i=0;i<150&&Field<LauncherState>(window,"state").Controls.Count==0;i++)await Task.Delay(100);
                var state=Field<LauncherState>(window,"state");Check(state.Controls.Count>0,"isolated native state loaded");await Idle();
                Check(Field<bool>(window,"preview"),"UI audit runs with --preview");
                var body=(FrameworkElement)window.FindName("Root");
                var baseRow=Id<TextBox>(window,"GameFolder1");var updateRow=Id<TextBox>(window,"GameFolder0");
                Check(baseRow.TranslatePoint(new Point(),body).Y<updateRow.TranslatePoint(new Point(),body).Y,"Game Files shows Base game above Update 1.28 with original IDs");
                var folderOrder=Desc(body).OfType<TextBox>().Select(AutomationProperties.GetAutomationId).Where(x=>x.StartsWith("GameFolder")).ToArray();
                Check(folderOrder.SequenceEqual(new[]{"GameFolder1","GameFolder0"}),"Game Files keyboard/tree order matches visual order");

                Check(state.Controls.Count(c=>c.Details.Length>0)>=19,"expanded native explanations available in frontend model");
                Invoke(window,"ShowPage",1,false);await Idle();
                HeaderToggle(window,state.Controls.Single(c=>c.Tag=="@nr"),"DLSS 5 NEURAL RENDERING");
                HeaderToggle(window,state.Controls.Single(c=>c.Tag=="@hdr"),"HDR");
                window.Width=window.MinWidth;await Idle();
                HeaderToggle(window,state.Controls.Single(c=>c.Tag=="@nr"),"DLSS 5 NEURAL RENDERING");
                HeaderToggle(window,state.Controls.Single(c=>c.Tag=="@hdr"),"HDR");
                var displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                displayScroll.ScrollToVerticalOffset(Math.Min(80,displayScroll.ScrollableHeight));await Idle();
                double offsetBeforeToggle=displayScroll.VerticalOffset;
                Check(offsetBeforeToggle>0,"display page can scroll before changing HDR");
                var hdr=state.Controls.Single(c=>c.Tag=="@hdr");int hdrBefore=hdr.Selected;
                Id<CheckBox>(window,"Setting"+hdr.Id).IsChecked=hdrBefore==0;
                for(int i=0;i<100&&ReferenceEquals(state,Field<LauncherState>(window,"state"));++i)await Task.Delay(50);
                state=Field<LauncherState>(window,"state");
                Check(state.Controls.Single(c=>c.Tag=="@hdr").Selected!=hdrBefore && state.Controls.Single(c=>c.Tag=="@hdr_paper").Hidden==(hdrBefore!=0),"HDR header checkbox validates and updates dependent controls through IPC");
                await Idle();
                displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                Check(Math.Abs(displayScroll.VerticalOffset-Math.Min(offsetBeforeToggle,displayScroll.ScrollableHeight))<1,"HDR toggle retains display scroll position after validation");
                displayScroll.ScrollToEnd();await Idle();
                double offsetBeforeRestore=displayScroll.VerticalOffset;
                hdr=state.Controls.Single(c=>c.Tag=="@hdr");var toggledState=state;
                Id<CheckBox>(window,"Setting"+hdr.Id).IsChecked=hdrBefore!=0;
                for(int i=0;i<100&&ReferenceEquals(toggledState,Field<LauncherState>(window,"state"));++i)await Task.Delay(50);
                state=Field<LauncherState>(window,"state");
                Check(state.Controls.Single(c=>c.Tag=="@hdr").Selected==hdrBefore,"HDR header checkbox can restore its original state");
                await Idle();
                displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                Check(Math.Abs(displayScroll.VerticalOffset-Math.Min(offsetBeforeRestore,displayScroll.ScrollableHeight))<1,"HDR toggle clamps scroll position when content shrinks");
                var simulated=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                simulated.Graphics=JsonSerializer.Deserialize<JsonElement>("{\"checked\":true,\"name\":\"GeForce RTX 5080\",\"dlss\":true,\"neural\":true,\"reflex\":true,\"fsr411\":true,\"dlss_runtime\":true,\"neural_runtime\":true,\"frame_multiplier_max\":6}");
                simulated.Controls.Single(c=>c.Tag=="@nr").Selected=1;
                var nrState=await new Backend().Call("validate",simulated);Invoke(window,"Adopt",nrState);await Idle();
                state=Field<LauncherState>(window,"state");
                displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                displayScroll.ScrollToEnd();await Idle();double nrOffset=displayScroll.VerticalOffset;
                Check(nrOffset>0,"display page can scroll before changing Neural Rendering");
                var beforeNr=state;Id<CheckBox>(window,"Setting"+state.Controls.Single(c=>c.Tag=="@nr").Id).IsChecked=false;
                for(int i=0;i<100&&ReferenceEquals(beforeNr,Field<LauncherState>(window,"state"));++i)await Task.Delay(50);
                await Idle();state=Field<LauncherState>(window,"state");
                displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                Check(state.Controls.Single(c=>c.Tag=="@nr").Selected==0 && Math.Abs(displayScroll.VerticalOffset-Math.Min(nrOffset,displayScroll.ScrollableHeight))<1,"Neural Rendering off retains or clamps scroll after validation");
                nrOffset=displayScroll.VerticalOffset;beforeNr=state;
                Id<CheckBox>(window,"Setting"+state.Controls.Single(c=>c.Tag=="@nr").Id).IsChecked=true;
                for(int i=0;i<100&&ReferenceEquals(beforeNr,Field<LauncherState>(window,"state"));++i)await Task.Delay(50);
                await Idle();state=Field<LauncherState>(window,"state");
                displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                Check(state.Controls.Single(c=>c.Tag=="@nr").Selected==1 && Math.Abs(displayScroll.VerticalOffset-Math.Min(nrOffset,displayScroll.ScrollableHeight))<1,"Neural Rendering on retains scroll after validation");
                displayScroll.ScrollToVerticalOffset(80);await Idle();double repeatedOffset=displayScroll.VerticalOffset;
                var copy1=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                var copy2=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                Invoke(window,"Adopt",copy1);Invoke(window,"Adopt",copy2);await Idle();
                state=Field<LauncherState>(window,"state");displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                Check(Math.Abs(displayScroll.VerticalOffset-Math.Min(repeatedOffset,displayScroll.ScrollableHeight))<1,"consecutive backend updates retain pending scroll position");
                displayScroll.ScrollToVerticalOffset(80);await Idle();double queuedOffset=displayScroll.VerticalOffset;
                var queued1=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                var queued2=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                Invoke(window,"Adopt",queued1);
                await window.Dispatcher.InvokeAsync(()=>Invoke(window,"Adopt",queued2),DispatcherPriority.Loaded);await Idle();
                state=Field<LauncherState>(window,"state");displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                Check(Math.Abs(displayScroll.VerticalOffset-Math.Min(queuedOffset,displayScroll.ScrollableHeight))<1,"queued backend updates retain scroll through layout");
                displayScroll.ScrollToVerticalOffset(80);await Idle();double gapOffset=displayScroll.VerticalOffset;
                var gap1=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                var gap2=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                var oldScroll=displayScroll;bool injected=false;
                DispatcherHookEventHandler? hook=null;
                hook=(_,e)=>
                {
                    var page=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().LastOrDefault();
                    if(!injected && e.Operation.Priority==DispatcherPriority.Loaded && page!=null && !ReferenceEquals(page,oldScroll) && page.VerticalOffset==0 && page.ScrollableHeight>0)
                    {
                        injected=true;
                        window.Dispatcher.BeginInvoke(DispatcherPriority.Normal,new Action(()=>Invoke(window,"Adopt",gap2)));
                    }
                };
                window.Dispatcher.Hooks.OperationCompleted+=hook;
                Invoke(window,"Adopt",gap1);await Idle();window.Dispatcher.Hooks.OperationCompleted-=hook;
                state=Field<LauncherState>(window,"state");displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                Check(injected && Math.Abs(displayScroll.VerticalOffset-Math.Min(gapOffset,displayScroll.ScrollableHeight))<1,"update arriving between restoration and layout retains scroll");
                displayScroll.ScrollToVerticalOffset(80);await Idle();
                var navigationState=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                Invoke(window,"Adopt",navigationState);Invoke(window,"ShowPage",2,false);Invoke(window,"ShowPage",1,false);await Idle();
                state=Field<LauncherState>(window,"state");displayScroll=((Panel)window.FindName("PageHolder")).Children.OfType<ScrollViewer>().Last();
                Check(displayScroll.VerticalOffset==0,"deferred scroll restoration does not override page navigation");
                beforeNr=state;Id<CheckBox>(window,"Setting"+state.Controls.Single(c=>c.Tag=="@nr").Id).IsChecked=false;
                Invoke(window,"ShowPage",2,false);
                for(int i=0;i<100&&ReferenceEquals(beforeNr,Field<LauncherState>(window,"state"));++i)await Task.Delay(50);
                await Idle();state=Field<LauncherState>(window,"state");
                Check(Field<int>(window,"currentPage")==2 && state.Controls.Single(c=>c.Tag=="@nr").Selected==0,"pending validation keeps a newly selected page active");
                var choices = new Setting { Id = 9999, Label = "Fixture", Kind = "Choice", Options = ["Available", "Unavailable"], OptionReasons = ["", "Unsupported GPU"] };
                var choiceView = (FrameworkElement)Invoke(window, "SettingView", choices, 300d, false)!;
                var combo = Desc(choiceView).OfType<ComboBox>().Single();
                var unavailable = (ComboBoxItem)combo.Items[1];
                Check(!unavailable.IsEnabled && (string)unavailable.ToolTip == "Unsupported GPU", "unavailable choices expose the backend reason and cannot be selected");
                var neural = new Setting { Id = 9997, Label = "Neural Rendering", Kind = "Choice", Tag = "@nr", Options = ["Off", "On"], OptionReasons = ["", ""] };
                var neuralCheck = Desc((FrameworkElement)Invoke(window, "SettingView", neural, 300d, false)!).OfType<CheckBox>().Single();
                Check(neuralCheck.IsEnabled && neuralCheck.IsChecked == false, "Neural Rendering off is an enabled unchecked checkbox");
                neuralCheck.IsChecked = true;
                Check(neural.Selected == 1, "checking Neural Rendering preserves on index");
                neural.OptionReasons = ["", "Unsupported GPU"];
                neuralCheck = Desc((FrameworkElement)Invoke(window, "SettingView", neural, 300d, false)!).OfType<CheckBox>().Single();
                Check(neuralCheck.IsEnabled && neuralCheck.IsChecked == true, "Neural Rendering can be turned off when on becomes unsupported");
                neuralCheck.IsChecked = false;
                Check(neural.Selected == 0, "unchecking Neural Rendering preserves off index");
                neuralCheck = Desc((FrameworkElement)Invoke(window, "SettingView", neural, 300d, false)!).OfType<CheckBox>().Single();
                Check(!neuralCheck.IsEnabled && (string)neuralCheck.ToolTip == "Unsupported GPU" && ToolTipService.GetShowOnDisabled(neuralCheck), "unavailable Neural Rendering shows its reason and cannot be turned on");
                var upstreamChoice = new Setting { Id = 9996, Label = "Other On/Off", Kind = "Choice", Tag = "@upstream", Options = ["Off", "On"] };
                Check(Desc((FrameworkElement)Invoke(window, "SettingView", upstreamChoice, 300d, false)!).OfType<ComboBox>().Any(), "upstream On/Off choices remain unchanged");
                foreach (string tag in new[] { "@nr_intensity", "@nr_tone", "@nr_structure" })
                {
                    var amount = new Setting { Id = 9998, Label = "Amount", Kind = "Choice", Tag = tag, Selected = 2, Options = ["0%", "25%", "50%", "75%", "100%"] };
                    var amountView = (FrameworkElement)Invoke(window, "SettingView", amount, 300d, false)!;
                    var slider = Desc(amountView).OfType<Slider>().Single();
                    var displayed = Desc(amountView).OfType<TextBlock>().Single(x => AutomationProperties.GetAutomationId(x) == "SettingValue9998");
                    Check(slider.Minimum == 0 && slider.Maximum == 100 && slider.TickFrequency == 25 && slider.SmallChange == 25 && slider.IsSnapToTickEnabled && slider.Value == 50 && displayed.Text == "50%", tag + " displays a percentage slider with 25-point steps");
                    foreach (int percent in new[] { 0, 25, 50, 75, 100 })
                    {
                        slider.Value = percent;
                        Check(amount.Selected == percent / 25 && displayed.Text == percent + "%", tag + " preserves the existing index at " + percent + "%");
                    }
                }
                state=JsonSerializer.Deserialize<LauncherState>(JsonSerializer.Serialize(state,Backend.JsonOptions),Backend.JsonOptions)!;
                var physics=state.Controls.Single(c=>c.Tag=="@ffb:wheel_ffb_model");physics.Selected=0;
                var response=await new Backend().Call("validate",state);Invoke(window,"Adopt",response);Invoke(window,"ShowPage",4,false);Set(window,"drivingSubpage",1);Invoke(window,"ShowPage",4,false);Invoke(window,"Adopt",response);await Idle();
                var setting=response.Controls.Single(c=>c.Label=="Force feedback");var help=Id<Button>(window,"SettingHelp"+setting.Id);
                await Modal(window,setting.Label,d=>{var text=Desc((FrameworkElement)d.Content).OfType<TextBox>().Single();Check(text.IsReadOnly&&text.Text.Contains(setting.Details),"details button opens full selectable native explanation");Check(text.ActualHeight>120&&!Desc(text).OfType<FilmGrainOverlay>().Any(),"reading pane expands and has no input-field grain");},()=>help.RaiseEvent(new RoutedEventArgs(Button.ClickEvent)));

                Invoke(window,"ShowPage",6,false);await Idle();
                var runDetails=Id<Button>(window,"BenchmarkRunDetails");
                await Modal(window,"WHAT THE GAME RAN WITH",d=>{var text=Desc((FrameworkElement)d.Content).OfType<TextBox>().Single();Check(text.Text.Contains("2560x1440")&&text.Text.Contains("3840x2160"),"benchmark details dialog displays actual render and output");},()=>runDetails.RaiseEvent(new RoutedEventArgs(Button.ClickEvent)));

                Set(window,"preview",false);response.HardwareProblem="Fixture: processor lacks a required instruction.";Invoke(window,"Adopt",response);Invoke(window,"ShowPage",0,false);await Idle();
                var play=(Button)window.FindName("FooterPlay");Check(!play.IsEnabled&&play.ToolTip?.ToString()==response.HardwareProblem,"disabled Play explains exact hardware block");
                Invoke(window,"OperationStatus","");Check(((TextBlock)window.FindName("Status")).Text==response.HardwareProblem,"hardware failure appears in persistent status");
                Set(window,"preview",true);response.HardwareProblem="";Invoke(window,"Adopt",response);
                Set(window,"availableUpdate","9.9.9");var updater=Field<Button>(window,"updateButton");updater.Content="Update to 9.9.9";updater.Visibility=Visibility.Visible;
                Invoke(window,"ShowUpdateNotice","9.9.9");await Idle();Check(updater.IsVisible&&updater.IsEnabled,"install-update action visible separately from release link");updater.Visibility=Visibility.Collapsed;
                var gain=response.Controls.Single(c=>c.Tag=="@ffbgainup");var captureButton=new Button{Content="Map"};
                string config=Path.Combine(Root,"data/user/input_config/default.ini");
                File.WriteAllText(config,"# DriveClub racing layout\nwheel_button = 4, ffb_gain_up\nwheel_button = 5, ffb_gain_down\n");string initial=File.ReadAllText(config);
                var capture=(Task)Invoke(window,"ActivateNativeButton",captureButton,gain,false)!;await Task.Delay(200);
                Check(Field<int>(window,"captureSettingId")==gain.Id,"FFB capture starts in frontend");
                window.RaiseEvent(new KeyEventArgs(Keyboard.PrimaryDevice,PresentationSource.FromVisual(window),0,Key.Escape){RoutedEvent=Keyboard.PreviewKeyDownEvent});await capture.WaitAsync(TimeSpan.FromSeconds(5));
                Check(File.ReadAllText(config)==initial&&Field<int>(window,"captureSettingId")==-1,"Esc cancels real IPC capture and keeps mapping");
                capture=(Task)Invoke(window,"ActivateNativeButton",captureButton,gain,false)!;await Task.Delay(200);
                await (Task)Invoke(window,"ActivateNativeButton",captureButton,gain,false)!;await capture.WaitAsync(TimeSpan.FromSeconds(5));
                Check(File.ReadAllText(config)==initial,"second click cancels without saving");
                capture=(Task)Invoke(window,"ActivateNativeButton",captureButton,gain,false)!;await Task.Delay(200);
                window.RaiseEvent(new KeyEventArgs(Keyboard.PrimaryDevice,PresentationSource.FromVisual(window),0,Key.Delete){RoutedEvent=Keyboard.PreviewKeyDownEvent});await capture.WaitAsync(TimeSpan.FromSeconds(5));
                Check(!File.ReadAllText(config).Contains("ffb_gain_up")&&File.ReadAllText(config).Contains("ffb_gain_down"),"Delete clears requested mapping through live IPC");
                await PlayArgument(response);
                window.Width=window.MinWidth;window.Height=window.MinHeight;Invoke(window,"ShowPage",0,false);await Idle();
                var discord=(Button)window.FindName("DiscordButton");Check(discord.IsVisible&&discord.TranslatePoint(new Point(),body).Y+discord.ActualHeight<=body.ActualHeight,"approved Discord button remains anchored at minimum window");
                File.WriteAllText(Path.Combine(Out,"checks.json"),JsonSerializer.Serialize(new{passed=Checks.Count,checks=Checks,game_launched=false},Backend.JsonOptions));app.Shutdown();
            }
            catch(Exception ex){File.WriteAllText(Path.Combine(Out,"checks.json"),JsonSerializer.Serialize(new{error=ex.ToString(),checks=Checks},Backend.JsonOptions));Console.Error.WriteLine(ex);app.Shutdown(1);}
        };
        return app.Run(window);
    }
}
