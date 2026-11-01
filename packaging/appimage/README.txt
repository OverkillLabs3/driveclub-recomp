DriveClub Recomp for Linux (AppImage)
=====================================

DriveClub (PS4, version 1.28) running natively on Linux: no Wine, no emulator. You need your
own, legally obtained, decrypted game dump:

  * the version 1.28 update folder (it contains eboot.bin and sce_sys/param.sfo)
  * the base game folder (it contains game000.dat)

Nothing in your game folders is modified or copied.

Start
-----
Make the AppImage executable and run it:

  chmod +x driveclub-recomp-<version>-x86_64.AppImage
  ./driveclub-recomp-<version>-x86_64.AppImage

This opens the Linux launcher, the native equivalent of the Windows launcher. On the GAME page
choose the update and base folders (or put them next to the AppImage as update/ and base/),
then press PLAY. The launcher covers the recomp's settings:

  GAME       game folders, player name, startup options, display/refresh and memory tier
  DISPLAY    frame rate, output/render size, anti-aliasing, upscaling, HDR, filtering, mirror
  DRIVING    assists, gearbox, camera, field of view, picture and wheel options
  PATCHES    the resolution, widescreen and camera patches, with the patch list
  LOGS       the last run's output and the launcher log

PLAY prepares the game files and starts the runtime. SAVE keeps the settings for next time;
they are also written to launcher.ini and launch.env in the data folder (see below), so a
runtime restart (for example after changing the resolution in the game) keeps them. The
settings map to the runtime's BB_* environment variables and to camera.ini, picture.ini,
driveclub.ini and patches_enabled.txt.

The first start asks for the two game folders. You can also save the folders without a
dialog and still get the launcher:

  ./driveclub-recomp-<version>-x86_64.AppImage --setup /path/to/update /path/to/base

Everything it writes (settings, prepared game files, saves, logs) lives in a `data/` folder
next to the AppImage whenever that folder is writable, similar to the Windows build's data
folder. `update/` and `base/` next to the AppImage are found automatically. The newest run's
output is in `data/logs/last-run.log`; the launcher's LOGS page shows it.

If the AppImage is in a read-only folder, the data goes in
~/.local/share/driveclub-recomp instead. An existing folder from an older build is kept so
saves are not lost.

Data folder
-----------
Use DATA FOLDER > Change... in the launcher to pick another folder. Tick "Move the current
data here" to bring the settings, prepared game files and saves along; on another drive they
are copied and the old folder is kept. The choice is remembered in a `data-path` file next to
the AppImage (or in ~/.config/driveclub-recomp/data-path when the AppImage's folder is
read-only). To go back beside the AppImage, use DATA FOLDER > Use AppImage folder.

For scripts or a headless run, set DRIVECLUB_DATA_DIR (it wins over the saved choice), or
create an empty `portable` file next to the AppImage to use data/ there when no folder has been
selected. DRIVECLUB_PORTABLE=1 does the same without the marker.

Options
-------
  --play                Prepare the game if needed and play without opening the launcher.
  --setup UPDATE BASE   Save the game folders and open the launcher.
  --repair              Prepare the game files again (after replacing a dump) and play.
  --selftest            Print the paths and check the bundled libraries.
  --version             Print the packaged version.
  --help                Show this text.

Environment
-----------
  DRIVECLUB_UPDATE_DIR, DRIVECLUB_BASE_DIR
      Use these game folders instead of the saved settings (for --play and --repair).
  DRIVECLUB_DATA_DIR
      Where settings, prepared files, saves and logs live. Default: data/ next to the
      AppImage when writable, otherwise ~/.local/share/driveclub-recomp.
  DRIVECLUB_PORTABLE=1
      Use data/ next to the AppImage when no data folder has been selected.
  BB_*
      Runtime options; the launcher writes the ones it manages. See the project README for
      the full list, including diagnostic and troubleshooting options.

Requirements
------------
x86_64 Linux with a Vulkan 1.3 driver (radv, anv or the NVIDIA proprietary driver). The AppImage
uses the host's glibc, Vulkan loader/driver, X11 stack (XWayland on Wayland desktops) and audio
backends; everything else is bundled. Without libfuse2, run it with --appimage-extract-and-run
(or APPIMAGE_EXTRACT_AND_RUN=1). The launcher uses SDL3's own window and renderer, so it needs no
GTK or Qt installation. The Steam Deck's Gaming Mode needs the AppImage added as a non-Steam
game; the launcher can also be used there in Desktop Mode.
