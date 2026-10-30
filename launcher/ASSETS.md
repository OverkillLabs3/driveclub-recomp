# Bundled launcher artwork

The WPF launcher includes its theme under `launcher/assets/`: the eight page
backgrounds, red DRIVECLUB logo, two interface fonts, inactive option texture,
swipe/stamp graphics and all 29 spinner frames. These are the same files used by
the reviewed local interface, with no separate asset download or extraction step.

The WPF project copies the complete theme into `assets/` beside `DriveClub.exe`
for both build and publish. Preserve that directory when packaging the launcher.
`assets/manifest.json` records each file's size and SHA-256 for verification.

## Theme layout

- `backgrounds/`: `page-0.png` (Play), `page-2.png` (Display),
  `detail.png` (Graphics), `page-4.png` (General), `driving.png`,
  `patches.png`, `page-5.png` (Benchmark), `mods.png`.
- `brand/logo-red.png`: title and Play-page logo.
- `fonts/001futura-webfont.ttf`, `fonts/004russian-webfont.ttf`.
- `controls/option-inactive.png`: option-field texture.
- `transitions/swipe.png`, `transitions/stamp.png`: startup graphics.
- `spinner/00.png` through `28.png`: startup spinner at approximately 30 Hz.

Images and fonts were extracted/converted from DRIVECLUB interface assets.
Raw game archives, runtime binaries, user preferences and review captures are
not included in this theme.

Backgrounds are converted to monochrome once on load and receive a uniform dim
layer; source images are unchanged. Stationary device-pixel grain is generated
procedurally. Artwork crossfades; foreground transitions take 280 ms and sidebar
selection movement 220 ms. Startup retains its swipe/stamp timeline and spinner.

The shared PC app/window icon is `launcher/wpf/Assets/app.ico`, embedded into
the WPF executable and Windows game resource. It does not replace logos inside
the interface. Read `launcher/wpf/README.md` for build, preview and capture usage.
