# Online (experimental)

This branch lets the game play online against a community server: sessions and private lobbies with friends,
**races between players** (peer to peer, over a LAN or a VPN such as Tailscale, Hamachi or Radmin), clubs,
driver and club statistics, fame and levels, leaderboards, challenges and the friends list with live presence.
It is a work in progress.


## What works

**Playing together**
- Join a friend's multiplayer session from the friends list, or invite them into a private lobby.
- Promote another player to session leader, kick players, leave and rejoin.
- Race together: the host starts an event, everyone races, and afterwards everyone is back in the lobby.
- Race times match between players, also when their frame rates differ (see "Frame rate and race times").
- Players on different PCs: one LAN or a VPN (tested with Tailscale and Hamachi).

**Friends and presence**
- Every other player on the server is a friend.
- Online and offline show within seconds: a friend who starts the game later appears, one who quits goes away.
- What a friend is doing ("Playing a Tour Event", "In the menus", ...) shows as a short note at the top of the
  screen whenever it changes. The game's own friend list only ever says "Playing DRIVECLUB"; on a console the
  status was shown by the system, not by the game.
- Avatars (a figure on a colour made from the name) instead of an endless loading ring.

**Clubs**
- Create a club: the club name is typed with the PC keyboard (Enter accepts, Esc cancels). The name field starts
  with your player name; type over it.
- Find clubs, join and leave, invitations, members list, badge and livery customization.
- Club level, club fame (the members' fame summed), the global club leaderboard, club statistics and club
  accolades (the members' summed).

**Statistics and progression**
- Driver profile for yourself and your friends: total fame, driver level, global ranking, favourite track and
  manufacturer, accolades ("Nearly leveled up", "Recently leveled up").
- Statistics per track, car, game mode, solo challenge, overdrive and face-off.
- The global driver leaderboard by fame.
- Leaderboards (read and score posting), event history, challenges and challenges sent to friends.

## What does not work yet

- Public matchmaking (the "Multiplayer" events), tours with others and ghosts.
- Fame is worked out from the events the game reports (it does not send its own total), so fame from accolades is
  missing and a level can be one lower than the game shows for itself.
- Club fame is the members' fame, not fame earned while in the club.
- The add-ons store answers with an empty container.
- "View Profile" on a friend opens the console's PlayStation profile, which the port does not have: use "View
  Driver Profile" instead.

## Play with friends

You need: your own copy of the game (PS4, CUSA00093 or CUSA00003, version 1.28), a build of this branch (or a release
package), the community server (only the host), and for players outside your home network a VPN that all of you
join (Tailscale is the quickest: direct connections, free for 3 users).

1. Unpack the release package (or build the runtime, the launcher and the server, see below). Only the host needs
   the `server` folder.
2. Every player runs `Allow-Firewall.bat` once. It allows the game (and on the host the server, TCP 8080) from
   the home network and the VPN address ranges only, not from the internet.
3. Every player starts `DriveClub.exe` once to pick the game folders (the 1.28 update with `eboot.bin`, and the base
   game with `game000.dat`), then closes it.
4. The host runs `Play-Online.bat` with `host=self`. It starts the server and shows the host's IP addresses.
5. The others run `Play-Online.bat` with the host's IP (the VPN address, e.g. `100.x.x.x` for Tailscale).
6. Join the host's session from the friends list, or let the host invite you into a private lobby, and race.

`online.ini` keeps the name and the host; delete it to change them.

Every player needs the same build: the runtime, the launcher and the server of one release belong together.

## Frame rate and race times

Each game runs its own simulation, timed by its own clock. With "60 FPS (deltatime)" or "Unlocked (VRR)" the game
times itself by the wall clock, so race times match between players at any frame rate. A frame longer than 33 ms is
counted as 33 ms so a hitch cannot throw the car or camera; the rest of it is made up over the next frames, so the
race clock still keeps to the wall clock (a stall over 250 ms, such as loading, is dropped). With "60 FPS (fixed
tick)" the game runs slower whenever it misses 60 FPS, and times drift apart: don't use it online.

## How it fits together

1. `BB_ONLINE=1` sends the `sceNet`, `sceNp`, `sceHttp`, `sceSsl` and `sceRudp` calls to the network libraries in
   `gpu/shadps4` instead of the offline stubs.
2. `BB_NP_LOCAL=<name>` makes the game see a signed-in account called `<name>` that lives on this PC. The account ID
   is derived from the name. `BB_COOP_SERVER=<host:port>` is the community server for the runtime's own requests
   (default `127.0.0.1:8080`).
3. `data/user/host_overrides.json` sends the game's old hostnames to the community server over plain http
   (see `host_overrides.json` here; `Play-Online.ps1` writes it).
4. The game reads its server settings from a "TSS" file. `mods/override-0.tss` replaces it (see `override-0.tss`
   here). The `OTG_*` keys switch on the leaderboard service, `fa_egsConnection` the server connection, and
   `egs_timeout 3600` stops the game from dropping its session every 30 seconds. Slots 1 to 6 are files with the
   single line `<< Config >>`, so the game does not wait for a storage server that no longer exists.
5. Invitations go through the server as PSN "game custom data": the runtime polls `dccoop/pending/<name>` and raises
   the system event the game waits for. The server then pushes the inviter's lobby to both games (see
   `lobby-notes.md`). Joining from the friends list pushes the lobby to the joiner the same way.
6. Races are peer to peer. The game looks up the other player's profile (`/v1/profiles`, which must carry `npId`),
   opens a connection with `sceNpSignalingActivateConnection` and then talks over `sceRudp`:
   - Without a matchmaking server, each runtime registers the UDP port of its P2P transport on the community server
     (`dccoop/p2p/register/<name>?port=N`) and resolves peers there (`dccoop/p2p/<name>`). The server answers with
     the address it sees, or its own address towards the asker for a game on the server's PC.
   - `gpu/shadps4/core/libraries/network/rudp.cpp` is a small `libSceRudp`: both ends call `sceRudpInitiate` (a
     simultaneous open), reliable contexts get acks, retransmission and in-order delivery, the others are plain
     datagrams. It runs on its own virtual port of the P2P transport. Both players need this runtime; it does not
     talk to a console.
   - The game owns two signaling contexts. An incoming connection goes to the newest one, also when the game creates
     it after the peer connected, and `PEER_ACTIVATED` reaches it; the game answers with its own activation.
7. Presence: on a console PSN pushes every change of a friend to the game. The runtime asks the server every 5 s
   (`dccoop/presence/<name>`, one `name<TAB>online<TAB>status` line per friend) and raises the
   `np:service:presence:*` push events for what changed. That poll is also each game's sign of life: the server
   counts a player online while their game keeps asking (15 s).
8. The on-screen keyboard is the game's plugin `oskGNMm.prx`: its `module_start` registers an "OnScreenKeyboard"
   service with the service list the game passes to `sceKernelLoadStartModule`, and the service opens the system
   text dialog (`libSceImeDialog`), which the runtime answers from the PC keyboard. The launcher links the plugin
   with a "start when loaded" flag (boot image format `BBPROBE6`) and the runtime starts it when the game loads it.
   A game prepared by an older launcher is prepared again once.
9. Statistics: after each event the game uploads counters (`stats/V06/player/<id>` gamestate, keys such as
   `au_r8/vehicle/distance` or `Drift/overdrive/fame`). The server answers `statsService.getAccountStats` and the
   profile pages from them and takes each player's fame from the event fame there. `libScePngDec` (on libpng)
   decodes the avatars.

## The server

[shad-net/driveclub-server](https://github.com/shad-net/driveclub-server) (GPL-2.0) with the online changes on
[Judaswick/driveclub-server](https://github.com/Judaswick/driveclub-server), branch `coop`: lobbies and sessions
(join, leave, host change, kick), invitations, messaging push, matching stubs, profiles with `npId`, avatars, the P2P
directory, presence, clubs, statistics and fame. The patches in `server-patches` are an older state of the same work.

Build (Qt 6.10 or newer and protobuf; on Windows with MSYS2 CLANG64 the packages are
`mingw-w64-clang-x86_64-{qt6-base,protobuf}`):

```
git clone -b coop https://github.com/Judaswick/driveclub-server
cd driveclub-server
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_SERVER_DEPLOY_QT=OFF
ninja -C out
```

Run `driveclubsrv.exe` from a folder that holds `server-config.json` renamed to `config.json`, with the Qt DLLs and
`sqldrivers/qsqlite.dll` next to the exe (or run `windeployqt`). With `shadnet.url` empty and `allow_unverified` true
it accepts any login. It listens on all interfaces (port 8080): anyone who reaches it can sign in under any name, so
keep it to your home network and VPN (`Allow-Firewall.bat` does that) and don't forward the port on your router.

## Building the runtime and the launcher

See [BUILDING.md](../../BUILDING.md). `bash build.sh` builds the runtime; `packaging/package.sh` builds the runtime and
the launcher and packs a release. The launcher must come from this branch too: it links the on-screen keyboard.

## Tools for continuing

Run with `BB_GPU_LOG=info` and `BB_GPU_LOG_ONLY=NpHandler,Lib.NpSignaling,Lib.NpWebApi,Lib.Net` for the network side.
These are for reverse engineering and do nothing unless set:

- `BB_GUEST_TRACE="0xADDR,..."` logs the registers whenever the game reaches one of those guest addresses
  (`BB_GUEST_TRACE_MAX` hits each, default 20); `BB_GUEST_TRACE_DUMP="reg:len"` also dumps memory at a register.
- `BB_GUEST_PATCH="0xADDR=HEXBYTES,..."` writes bytes into guest code before the game starts.
- `BB_GUEST_SAMPLE=1` samples where the game's threads are, 20 times a second.

`lobby-notes.md` has what is known about the lobby and messaging code, with guest addresses
(0x800000000 + ELF vaddr).

## Notes

- `host_overrides.json` ends with a catch-all (`*`) so nothing from the game goes to the original hosts. Remove it
  if you want other traffic to leave the PC.
- The game files are not part of anything here; every player needs their own copy.
