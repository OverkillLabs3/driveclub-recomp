using System.Diagnostics;
using DriveClub.Launcher;

if (args.Length > 0 && args[0] == "--child")
{
    using var windowCreated = EventWaitHandle.OpenExisting(args[1]);
    windowCreated.Set();
    if (args[2] == "ready")
    {
        await Task.Delay(350);
        using var ready = EventWaitHandle.OpenExisting(args[3]);
        ready.Set();
    }
    await Task.Delay(args[2] == "crash" ? 100 : 2000);
    return args[2] == "crash" ? 23 : 0;
}
int passed = 0;
void Check(bool ok, string name) { if (!ok) throw new Exception(name); passed++; Console.WriteLine("PASS " + name); }
Process Child(string mode, LaunchSession session, EventWaitHandle created, string name)
{
    var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true };
    if (Path.GetFileNameWithoutExtension(Environment.ProcessPath!).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        start.ArgumentList.Add(typeof(LaunchSession).Assembly.Location);
    foreach (var item in new[] {"--child",name,mode,session.EventName}) start.ArgumentList.Add(item);
    return Process.Start(start)!;
}
async Task Scenario(string mode)
{
    using var session = new LaunchSession();
    string name = @"Local\DriveClub.TestWindow." + Guid.NewGuid().ToString("N");
    using var created = new EventWaitHandle(false, EventResetMode.ManualReset, name);
    using var process = Child(mode, session, created, name);
    try
    {
        Check(created.WaitOne(5000), mode + ": child/window-created phase reached");
        var waiting = session.WaitForFrameAsync(process.Id, mode == "timeout" ? TimeSpan.FromMilliseconds(120) : TimeSpan.FromSeconds(5));
        Check(!waiting.IsCompleted, mode + ": process/window appearance does not end splash wait");
        if (mode == "ready") Check(await waiting, "real readiness event completes handoff");
        if (mode == "manual") { session.SwitchToGame(); Check(!await waiting, "manual switch completes without pretending a frame was ready"); }
        if (mode == "timeout")
        {
            try { await waiting; throw new Exception("timeout not detected"); } catch (TimeoutException) { Check(true, "hung startup returns a timeout instead of an endless splash"); }
            Check(!process.HasExited, "timeout does not terminate the game");
        }
        if (mode == "crash")
        {
            try { await waiting; throw new Exception("crash not detected"); } catch (IOException) { Check(true, "early game failure is reported"); }
        }
    }
    finally { if (!process.HasExited) { process.Kill(); await process.WaitForExitAsync(); } }
}
foreach (var mode in new[] {"ready","manual","timeout","crash"}) await Scenario(mode);
using (var session = new LaunchSession())
using (var current = Process.GetCurrentProcess())
using (var cancellation = new CancellationTokenSource())
{
    var waiting = session.WaitForFrameAsync(current.Id, TimeSpan.FromSeconds(5), cancellation.Token);
    cancellation.Cancel();
    try { await waiting; throw new Exception("cancellation not detected"); }
    catch (OperationCanceledException) { Check(true, "benchmark completion can cancel its outstanding splash wait"); }
    Check(!current.HasExited, "cancelling the splash wait does not terminate the process");
}

using (var first = new LaunchSession())
using (var second = new LaunchSession())
{
    Check(first.EventName != second.EventName, "successive launches have isolated event names");
}
Console.WriteLine($"{passed} startup lifecycle checks passed.");
return 0;
