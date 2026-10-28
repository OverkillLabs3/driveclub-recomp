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
                var choices = new Setting { Id = 9999, Label = "Fixture", Kind = "Choice", Options = ["Available", "Unavailable"], OptionReasons = ["", "Unsupported GPU"] };
                var choiceView = (FrameworkElement)Invoke(window, "SettingView", choices, 300d, false)!;
                var combo = Desc(choiceView).OfType<ComboBox>().Single();
                var unavailable = (ComboBoxItem)combo.Items[1];
                Check(!unavailable.IsEnabled && (string)unavailable.ToolTip == "Unsupported GPU", "unavailable choices expose the backend reason and cannot be selected");
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
