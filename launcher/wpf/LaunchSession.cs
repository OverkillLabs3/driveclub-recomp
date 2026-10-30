using System.Diagnostics;
using System.IO;

namespace DriveClub.Launcher;

// A unique, non-persistent event per launch. Window creation is intentionally not readiness.
public sealed class LaunchSession : IDisposable
{
    public string EventName { get; } = @"Local\DriveClub.FirstFrame." + Guid.NewGuid().ToString("N");
    private readonly EventWaitHandle ready;
    private readonly TaskCompletionSource<bool> manual = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public LaunchSession() => ready = new EventWaitHandle(false, EventResetMode.ManualReset, EventName);
    public void SwitchToGame() => manual.TrySetResult(true);
    public async Task<bool> WaitForFrameAsync(int processId, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        Process process;
        try { process = Process.GetProcessById(processId); }
        catch (ArgumentException ex) { throw new IOException("DRIVECLUB closed before the loading screen finished. See the game log.", ex); }
        using var running = process;
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        var signalled = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var registration = ThreadPool.RegisterWaitForSingleObject(ready,
            (_, _) => signalled.TrySetResult(true), null, Timeout.Infinite, true);
        try
        {
            var exited = process.WaitForExitAsync(cancellation.Token);
            var expired = Task.Delay(timeout, cancellation.Token);
            var completed = await Task.WhenAny(signalled.Task, manual.Task, exited, expired);
            cancellationToken.ThrowIfCancellationRequested();
            if (process.HasExited)
                throw new IOException($"DRIVECLUB closed before the loading screen finished (exit {process.ExitCode}). See the game log.");
            if (completed == expired)
                throw new TimeoutException("DRIVECLUB is still starting. Its window has been left running; check the game window or log.");
            return completed == signalled.Task;
        }
        finally
        {
            registration.Unregister(null);
            cancellation.Cancel();
        }
    }
    public void Dispose() => ready.Dispose();
}
