using System.IO;
using System.Windows;
using System.Windows.Media.Animation;

namespace DriveClub.Launcher;
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // No 60 Hz dispatcher timer drives animations. WPF schedules the retained visuals.
        Timeline.DesiredFrameRateProperty.OverrideMetadata(typeof(Timeline), new PropertyMetadata(240));
        DispatcherUnhandledException += (_, args) =>
        {
            Directory.CreateDirectory(Path.Combine(AppContext.BaseDirectory, "logs"));
            File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "logs", "wpf-error.txt"), args.Exception.ToString());
            MessageBox.Show(args.Exception.Message, "DRIVECLUB launcher");
            args.Handled = true;
        };
        base.OnStartup(e);
    }
}
