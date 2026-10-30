# DriveClub online with friends (quick guide)

Sessions, private lobbies and races together, clubs, statistics, fame and levels, leaderboards and the friends list with
online status work. Public matchmaking and ghosts do not yet. Details: [README.md](README.md).

## What you need

- Your own copy of DriveClub (PS4, CUSA00093 or CUSA00003, version 1.28). No game files are included anywhere.
- The same release for every player.
- **The host only:** the community server (the `server` folder in the release).
- **Players not on the same home network:** a shared VPN. Tailscale is the quickest (free, direct connections);
  Hamachi and Radmin work too.

## Set up once (every player)

1. Extract the release.
2. Double-click `Allow-Firewall.bat` and answer Yes to the administrator question. The game and the server can then
   be reached from your home network and the VPN, but not from the internet.
3. Start `DriveClub.exe` once, choose the update folder (1.28, with `eboot.bin`) and the base game folder (with
   `game000.dat`) under **Browse**, then close it. Use the frame rate **60 FPS**, **Unlocked (VRR)** or **Unlimited**,
   not **60 FPS (fixed tick)**.
4. VPN: everyone in the same network. With Tailscale the host invites the others under *Users, Invite users*, and
   they pick the host's tailnet when they sign in.

## Play

1. **Host:** in the launcher, **Gameplay**, **ONLINE**, choose **Host a session**, then press Play. The server starts
   and the launcher shows your IP addresses (Tailscale `100.x`, Hamachi `25.x`, Radmin `26.x`, home network
   `192.168.x`). Give one of them to your friends.
2. **Friends:** **ONLINE**, **Join a session**, type the host's IP address under **Host's IP address**, then press Play.
3. Join the other player's session from the friends list, or invite them to a private lobby, then start an event.
   Promoting a leader and kicking a player work too.

What a friend is doing appears at the top of the picture as a short message when it changes. The game's own friends
list always shows just "Playing DRIVECLUB".

Club names are typed with the PC keyboard (Enter confirms, Esc cancels).

To change your name or the host's IP: change it in the launcher.

## If something goes wrong

- **"Server not reachable":** is the VPN on? Has the host started the game with Host a session? Is the IP right? Did the
  host run `Allow-Firewall.bat`?
- **"Cannot connect to players":** run `Allow-Firewall.bat` on both PCs. With Tailscale, check that both are in the same
  tailnet (the host's PC must show up in the other player's Tailscale menu).
- **Race times drift apart:** do not use **60 FPS (fixed tick)**. Use **60 FPS**, **Unlocked (VRR)** or **Unlimited**; the
  race clock then follows real time at any frame rate.
- **Something is missing for one player only** (level, club values): the game remembers profiles. Restart once.
