using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;

namespace DriveClub.Launcher;

internal readonly record struct CommunityOpenResult(bool NeedsNotice, bool Copied, string Message);

internal static class CommunityLink
{
    internal const string Url = "https://discord.gg/62b8jceumR";

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    internal static CommunityOpenResult Open() => Open(
        () => { Process.Start(new ProcessStartInfo(Url) { UseShellExecute = true }); },
        () => Clipboard.SetDataObject(Url, true),
        GetProcAddress(GetModuleHandle("ntdll.dll"), "wine_get_version") != IntPtr.Zero);

    // Wine/Proton may report shell success without opening a host browser.
    // Keep both a clipboard copy and a selectable URL available in that case.
    internal static CommunityOpenResult Open(Action openBrowser, Action copyLink, bool wine)
    {
        bool opened;
        try { openBrowser(); opened = true; }
        catch (Exception) { opened = false; }
        if (opened && !wine) return new(false, false, "");
        bool copied = TryCopy(copyLink);
        string reason = opened
            ? "The browser request was sent, but Wine/Proton may not open a browser."
            : "Your browser could not be opened.";
        string next = copied ? "The Discord link has been copied. Paste it into your browser."
            : "The clipboard is unavailable. Select the link below and press Ctrl+C, or try Copy link again.";
        return new(true, copied, reason + "\n\n" + next);
    }

    internal static bool TryCopy() => TryCopy(() => Clipboard.SetDataObject(Url, true));
    internal static bool TryCopy(Action copyLink)
    {
        try { copyLink(); return true; }
        catch (Exception) { return false; } // A busy/unavailable clipboard is recoverable.
    }
}
