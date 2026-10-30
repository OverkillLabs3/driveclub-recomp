using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace DriveClub.Launcher;

// Keep a selectable invite visible even when both the shell and clipboard fail.
internal sealed class CommunityLinkDialog : Window
{
    internal CommunityLinkDialog(string message)
    {
        Title = "Discord community"; Width = 560; MinWidth = 380;
        MaxWidth = Math.Max(380, SystemParameters.WorkArea.Width - 32);
        SizeToContent = SizeToContent.Height; ResizeMode = ResizeMode.NoResize;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = new SolidColorBrush(Color.FromRgb(0x17, 0x17, 0x19));
        SetResourceReference(FontFamilyProperty, "BodyFont"); FontSize = 14;
        var body = new StackPanel { Margin = new Thickness(24) };
        var explanation = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 16) };
        var link = new TextBox { Text = CommunityLink.Url, IsReadOnly = true, Margin = new Thickness(0, 0, 0, 16) };
        System.Windows.Automation.AutomationProperties.SetName(link, "Discord invite link");
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        var buttonStyle = new Style(typeof(Button), (Style)FindResource(typeof(Button)));
        var focus = new Trigger { Property = IsKeyboardFocusedProperty, Value = true };
        focus.Setters.Add(new Setter(BorderBrushProperty, Brushes.LightGray));
        focus.Setters.Add(new Setter(BorderThicknessProperty, new Thickness(1)));
        buttonStyle.Triggers.Add(focus);
        var copy = new Button { Content = "Copy link", Style = buttonStyle, Margin = new Thickness(0, 0, 12, 0) };
        var close = new Button { Content = "Close", Style = buttonStyle, IsCancel = true };
        copy.Click += (_, _) => explanation.Text = CommunityLink.TryCopy()
            ? "Discord link copied. Paste it into your browser."
            : "The clipboard is still unavailable. Select the link and press Ctrl+C, or type it into your browser.";
        actions.Children.Add(copy); actions.Children.Add(close);
        body.Children.Add(explanation); body.Children.Add(link); body.Children.Add(actions); Content = body;
        Loaded += (_, _) => { link.Focus(); link.SelectAll(); };
    }
}
