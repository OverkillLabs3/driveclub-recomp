# DRIVECLUB WPF launcher

Windows .NET 8 WPF shell over the native launcher backend. `DriveClub.exe` is a
small native bootstrapper. It opens `DriveClub.Launcher.exe` on Windows, or
`DriveClub.Native.exe` under Wine/Proton. `DriveClub.Backend.exe` owns settings,
preparation, launch and benchmarks. The runtime provides the first-frame splash
signal and the root-menu exit confirmation; rendering algorithms are unchanged.

## Settings

- **Play:** game status and folders; Quick Settings in the order Performance
  preset, Frame rate, Output resolution, Anti-aliasing.
- **Display:** frame rate, resolution/window, anti-aliasing/filtering and HDR.
- **Graphics:** scene detail, reflections and camera effects, including the
  existing Slow/Fast motion-blur exposure control.
- **General:** player profile, content, startup/overlays and advanced launcher options.
- **Driving:** Assists and view / Wheel and force feedback subpages.
- **Patches:** compatibility workarounds, gameplay changes, photo mode, effect
  overrides. Each existing optional patch retains
  its own control.
- **Benchmark:** the native AI-driven repeatable event and results.
- **Mods:** local loose-file replacements; not a downloadable mod catalogue.

The native card model is the single source of category membership and reset
scopes. Controls omitted by the native release are not exposed by the shell.
Resolution presets and the existing settings/patch formats are retained.

The current main's pedal input overlay is available under General (F2 while
playing). Its per-car FFB gain button assignments remain under Driving / Wheel
and force feedback. These buttons use the native wheel capture and save only
their assignments; other pending settings are retained. Capture times out after
15 seconds without a new wheel button. Physical wheel capture needs hardware
validation; the prerequisite and settings-preservation paths are covered offline.

The native launcher features are also available in the WPF shell:

- **Update:** the installed version comes from `version.txt`, not a fixed frontend
  version. The native checker selects the release and its checksum. Update downloads
  and verifies the package, then a native helper waits for the WPF process to exit
  before replacing its mapped EXE/DLL files and restarting the entry point. The
  existing player-file exclusions apply. Development/unknown version identifiers
  do not trigger replacement with an older stable release. No .NET installer is
  involved in application updates.
- **Details:** the `i` buttons open the native expanded explanations as selectable
  text. Benchmark results expose the actual run's render/output sizes, AA, cap,
  presentation, reflection and detail settings, rather than the currently selected
  options for the next run.
- **Wheel buttons:** a second click or Esc cancels capture; Delete/Backspace or
  the context menu clears the assignment. Clearing does not require a connected
  wheel. All other pending launcher settings remain pending. Wheel Setup launch
  failures are returned to the frontend.
- **Hardware and direct launch:** disabled Play explains the exact CPU/GPU check
  failure. `DriveClub.exe --play` starts through the WPF launch path even when
  auto-play is off; `--preview` suppresses it. Game Files lists Base game before
  Update 1.28 while preserving their existing path IDs and saved meanings.

Offline coverage: `launcher/tests/backend/parity.py`,
`launcher/tests/setup/updater_shell_test.cpp` and
`launcher/tests/parity/LauncherParity.Tests.csproj`. The fake SDL provider under
tests/backend is only a test fixture and must never ship in a game package.

### PS4 preset

Quick Settings offers **PS4**: 30 FPS, native 1080p output/render, original game
anti-aliasing and texture filtering, live mirrors, original model/scenery/shadow
quality, dynamic resolution, motion blur with Slow exposure, lens dust, heat haze
and FXAA. It restores 100% field of view and disables optional gameplay/photo
mode/effect overrides. Player identity, language, game folders, driving assists,
wheel settings, content and startup preferences are preserved.

The PS4 preset disables the camera/replay workaround: it is unnecessary at
30 FPS. Explicitly selecting 60 FPS delta time or Unlocked/VRR enables it;
selecting 30 FPS or fixed tick selects its off default. It remains individually
switchable in Patches. A manual opt-out survives save/restart and other setting
changes, until the next explicit FPS-mode change. Old configurations without a
camera preference derive it from their saved frame mode; existing preferences
are retained.

The runtime contract matches PR #187: camera_patch=0/1 in launcher.ini and
BB_CAMERA_PATCH=0/1 for the game. That runtime implementation is now in main
through merged PR #187; this frontend configures it. Unsupported older runtimes
show an unavailable control with an explanation.
Selecting a profile does not install a runtime patch.

Reset this page affects only the current page or active Driving subpage. Play
resets its four Quick Settings. Identity/language retain their exclusions.
Reset and profile selection do not save; Save settings and Play persist changes.

## Build and review

Building requires Windows, MSYS2 CLANG64 and .NET SDK 8+ with Windows desktop
targeting. The default package starts with a native `DriveClub.exe`, which checks
for .NET 8 Desktop x64 and offers to install it from Microsoft only when missing.
There is no separate Setup.exe. The native interface is included automatically.

```powershell
./launcher/build-wpf.ps1 -OutputDirectory C:\path\to\separate\preview
python launcher/tests/backend/verify.py C:\path\to\preview\DriveClub.Backend.exe C:\path\to\test-output
python launcher/tests/backend/preservation.py C:\path\to\preview\DriveClub.Backend.exe C:\path\to\existing-install C:\path\to\private-test-output
dotnet run --project launcher/tests/mod-manager/ModManager.Tests.csproj -c Release
```

Build outputs go into a separate directory. To run normally, provide the release
runtime, native helper applications/licenses and game folders beside the shell.
The theme is copied automatically from `launcher/assets/` during both build
and publish. This script does not replace an installed game.
The normal upstream build path remains available; this is a separate Windows
frontend build until packaging is explicitly adopted upstream.

For an offline package that carries .NET, pass `-SelfContained` to
`build-wpf.ps1`. It publishes the x64 runtime beside the application and includes
its runtime license/notices under `licenses/dotnet/`; it does not install .NET
system-wide. The default compact package is framework-dependent.

`DriveClub.exe --preview` prevents auto-play and previews the loading sequence
when Play/Benchmark is pressed; no game is prepared or launched. Settings remain
editable in this executable's own directory.

## Startup splash

Play (including auto-play) and Benchmark hide the main launcher and open a separate centered
960 x 600 loading window with the existing startup artwork and animation. File
preparation retains its real progress; runtime initialization shows an animated
spinner without a fabricated percentage. Creating the game window does not dismiss
the splash. It fades away after the first successfully submitted game presentation,
then gives focus to the game. During a benchmark the launcher stays hidden,
stops its loading animation, and returns with results when the game exits. The
benchmark child PID is delivered through progress while its backend keeps waiting
for completion.

The WPF shell creates a unique manual-reset event named
`Local\DriveClub.FirstFrame.<32 hex digits>` before starting the backend. Its name
is passed through `BB_LAUNCH_READY_EVENT` only to the backend/game process tree.
The backend returns the child PID. The normal runtime signals once after a
successful real (not generated/reused) game presentation; no captures or GPU
readbacks are involved. The launcher detects support from the runtime marker.

A **Switch to game** button permits manual handoff, including with older runtimes
without the notification. Early process exit restores the launcher with an error.
A three-minute safety timeout also restores the launcher and leaves the game
running. Neither path terminates the user's game or changes saved preferences.

Startup lifecycle checks (real named events and child processes):

```powershell
dotnet run --project launcher/tests/startup/Startup.Tests.csproj -c Release
clang++ -std=c++20 tools/launcher_ready_test.cpp -o C:\path\to\launcher_ready_test.exe
C:\path\to\launcher_ready_test.exe
```

`DriveClub.exe --benchmark` opens the Benchmark page and starts the configured
run; it does not select or overwrite a performance preset.

## Startup and .NET prerequisites

Run `DriveClub.exe`. On Windows it checks compatible x64 Core and Desktop 8
runtimes directly, without PowerShell or a download when they are present. It
also recognizes a self-contained package. The WPF shell is
`DriveClub.Launcher.exe`; keep its matching DLL/deps/runtimeconfig files beside it.
The bootstrapper forwards arguments, working directory and child exit status.

If .NET is missing, the supplied `Install-Launcher.ps1` asks permission before
using Microsoft's HTTPS release metadata to download the Desktop x64 installer.
It validates SHA-512 and the trusted Microsoft signature, then requests elevation
for the official installer. Downloads are temporary, cancellation stops startup,
and Windows is never restarted automatically. After installation the bootstrapper
rechecks availability and starts WPF with the original arguments. No preferences
or game data are rewritten by the prerequisite check/installation.

Under Wine/Proton the bootstrapper selects `DriveClub.Native.exe` immediately,
without .NET detection, PowerShell or installation. On Windows, use
`DriveClub.exe --native` or open `DriveClub.Native.exe` directly for the same
fallback. Actual Wine/Proton execution still requires testing; the route and
argument forwarding are covered by Windows tests.

To try the new interface under Wine, run `DriveClub.Launcher.exe` directly with
.NET 8 Desktop x64 installed in the same Wine prefix. This bypasses automatic
native-interface selection; WPF compatibility under Wine remains unverified.

`DriveClub.exe --check-only` launches nothing: exit 0 means the selected interface
is available; exit 2 means .NET is missing on the WPF route. A missing program
file returns 1. `DriveClub.exe --native --check-only` checks only the native route.
`Install-Launcher.ps1 -CheckOnly` prints the runtime state without installation.

Native runtime-detection/routing tests are in `launcher/tests/setup/`:
`bootstrap_test.cpp`, `bootstrap_fixture.cpp`, `bootstrap_integration.py`, and
`updater_fallback_test.cpp`. The existing offline installer checks remain:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File launcher/tests/setup/verify.ps1 -OutputDirectory C:\path\to\fixtures
```

## In-game exit confirmation

Esc or mapped Back opens a modal confirmation only on the root main-menu screen.
Cancel is selected by default. Mouse, keyboard and mapped controller actions are
supported; held input cannot leak through into the menu. Exit waits for writable
save mounts and uses the game's normal Quit/CRT shutdown. The dialog is rendered
inside the game and intentionally has no logo. See
[implementation and regression checks](../../docs/in-game-exit.md).

## Visual design and assets

Opaque WindowChrome window, 228-DIP neutral sidebar, accent #ED2B38, integrated
caption/footer, original game logos and bundled fonts/artwork. Cards
use the measured Game Status width, centered within safe margins, with an 18-DIP
column gap. Minimum width accommodates two cards. Long captions wrap. Quick
Settings stays at the bottom when space permits; the whole page scrolls when
upper content needs more space. Valid game folders collapse; invalid ones expand.

Page transitions use 280 ms vertical slide/fade; the sidebar selection moves over
220 ms while labels stay still. Artwork only crossfades. The original startup
animation and 29-frame spinner retain their independent timelines. Uniform,
stationary grain is generated at device-pixel resolution with no tiling.

The complete reviewed theme is tracked in `launcher/assets/` and copied to
`assets/` beside the executable by the WPF project, including on publish. No
manual theme extraction is needed. Keep this directory in the release package.
See `launcher/ASSETS.md` for the theme file layout.
`wpf/Assets/app.ico` is the shared PC app/game icon. Captures, user settings,
runtime binaries and example mods remain outside source commits.

## Local Mod Manager

Imports folders or ZIP archives of loose files, with optional mod.json metadata.
Containers files/, mods/ and app0/ are accepted; arbitrary game-relative paths
are preserved. Imports start disabled. The highest enabled entry wins conflicts.
Enable/disable/reorder apply immediately to the launcher's own mods/ directory.
Save/reset do not apply to this page.

Original game archives and import sources are not modified. The library and
journal live in data/mod-manager/; packages are retained for recovery after
removal. Traversal, symlink/reparse, Windows device-name and case-collision
checks protect imports. Deployment protects unmanaged/externally edited files,
uses staging/rollback and recovers an interrupted journal on restart. No actual
in-game mods were verified by the launcher filesystem tests.

## Existing user configurations

The shell and backend use the existing launcher.ini, patch selection and user
sidecars beside the launcher. Opening the shell does not select the PS4 preset.
Saving retains unknown launcher/FFB keys, camera/HDR/overlay sidecar keys and
unlisted custom patch selections. The two-thread control owns only bit 37 of
an existing optimization mask and preserves all unrelated bits. Invalid saved
masks reject Save instead of replacing the file with defaults. Text updates use
a temporary file and atomic replacement so a failed replacement retains the
previous file. These guarantees are tested against copies, never by modifying
the supplied existing installation. Native pending wheel-setting migration is
retained and affects only recognized wheel keys.

## Temporary workaround lifecycle

Temporary patches are explicitly grouped under **Temporary workarounds**, named
as workarounds, described as experimental where appropriate and individually
toggleable. They must not be presented as permanent native fixes. The current
camera workaround still follows the explicit FPS-mode defaults described above;
a saved manual opt-out remains respected.

When the underlying runtime/core issue is fixed, verify the affected scenarios
with the workaround disabled. Once that native correction is established, retire
the workaround's automatic enabling and UI entry instead of stacking both paths.
Keep old config keys tolerated for compatibility. Update the source condition,
capability handling, descriptions and tests together. A runtime version number
alone does not prove the issue is fixed; this launcher does not automatically
claim to detect an unverified native correction.
