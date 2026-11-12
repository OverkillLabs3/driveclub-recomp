# Club Badge compatibility

Club Badge uploads require a server that advertises EGS `BobObjectType = 5`
and persists both preview references. The companion server changes target
the [Judaswick coop branch](https://github.com/Judaswick/driveclub-server/tree/coop).

The PNG encoder is adapted from
[shadPS4 commit 4f11a8c](https://github.com/shadps4-emu/shadPS4/commit/4f11a8c979de05cc97a236c2346c0f3343dc4034).
The imported files retain their shadPS4 copyright and GPL-2.0-or-later notices.

The Vulkan readback hook checks the DriveClub 1.28 conversion call-site
signature before installing. The patched runtime and server passed an
isolated Windows game test, and the reporter verified the deployed fix in
Windows DriveClub Recomp v0.4.2. The separate build from the upstream v0.4.2
tag passed compilation and a startup smoke test. Its in-game save test is
pending. Test a visibly nonempty badge in an isolated installation before
adopting a different build. Check both preview sizes and repeat after restart.
