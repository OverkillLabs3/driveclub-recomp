DriveClub for PC
================

You need your own copy of DriveClub for PS4 (CUSA00093). No game files are included.

What to prepare
---------------
Two folders from your console dump:
  1. The version 1.28 update (it contains eboot.bin).
  2. The base game (it contains game000.dat).
Nothing in these folders is changed or copied. They can stay where they are.
If your dump is already version 1.28 in a single folder (game000.dat next to the 1.28 eboot.bin), choose that folder as the base game folder and leave the update field empty.

How to play
-----------
1. Unzip this folder anywhere you can write to (not Program Files).
2. Run DriveClub.exe.
3. Press Browse next to each game folder and pick the update folder and the base game folder.
4. Choose the patches and settings you want (60 FPS is on by default; "Unlocked" removes the frame cap) and press Play.

Your choices are remembered.

If Windows shows "Windows protected your PC", press More info, then Run anyway. The program is not signed.

Anti-aliasing (DLAA and FSR)
----------------------------
The launcher can replace the game's own anti-aliasing with AMD FSR 3.1 (any graphics card) or NVIDIA DLAA (GeForce RTX): a sharper, steadier picture, especially on thin lines and foliage. In the game the overlay shows what is active. F9 switches it off and on (to compare), F10 switches between FSR and DLSS.
Cars have no motion data of their own yet, so fast cars can leave a faint trail.

Language
--------
The launcher has a Language choice (click it to go to the next one). The game has text for 21 languages. Japanese, Korean and Chinese may show missing characters.

Controls
--------
A gamepad works out of the box (Xbox, PlayStation, Switch Pro, 8BitDo over USB or Bluetooth).

Keyboard (racing layout; change it in data\user\input_config\default.ini):
  W / S          accelerate / brake and reverse
  A / D          steer
  Space          handbrake
  Q / E          shift down / up
  C              change camera
  R / T          touchpad left / right half (R is Reset to Track)
  Enter or Z     accept      Backspace  back      Esc or P  pause
  Arrow keys     d-pad       I J K L    look around (or move the mouse)
  F7             mouse look on / off

Requirements
------------
Windows 10 or 11 (64-bit), a processor from Intel Haswell (2013) or AMD Excavator or Ryzen onward, and a graphics card with a Vulkan 1.3 driver. About 40 GB of free disk space for the game dumps.

Online play (private, with friends)
-----------------------------------
In the launcher, Gameplay tab, ONLINE: choose Host a session to run a private server on your PC, or Join a session and type the IP address of the friend who hosts. Everyone uses a player name of 3 to 16 letters, digits, - or _.
Players reach each other over the home network or a VPN (Tailscale, Hamachi, Radmin). The host runs Allow-Firewall.bat once (and every player runs it once for the game) so Windows lets those connections in.
The host starts the game first and lets the others get in, because the game reads its friend list at start. With Online play Off, the game behaves as always and nothing online is touched.
The folder server\ holds the community server (GPL-2.0, see licenses\). More in online\README.md.

Known issues
------------
Some reflections (rain drops on the side window, paint on cars on the stage select screen) are brighter than they should be.
The "Static (faster)" mirror option stops reflections and rain on the glass from updating.

Files
-----
launcher.ini   the two folders you chose
data\          your patch choices and settings
mods\          files here replace the game's own files of the same path
logs\          one log per start, launcher.log and crash_*.dmp after a crash (see "Reporting a problem" below)

Reporting a problem
-------------------
Open an issue at https://github.com/OverkillLabs3/driveclub-recomp/issues and attach the files from the logs folder next to DriveClub.exe: the newest driveclub_<date>_<time>.log (one per start, take the one from the run that went wrong), launcher.log (it exists even when the game never started) and any crash_*.dmp. Without them an issue usually cannot be solved. Say what you were doing when it happened.
