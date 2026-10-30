@echo off
rem Starts the launcher with the online settings. Put this next to DriveClub.exe.
cd /d "%~dp0"
set BB_ONLINE=1
set BB_NP_LOCAL=Player
rem Only in a diagnostics build (BB_DIAGNOSTICS=ON):
rem set BB_GPU_LOG=info
rem set BB_GPU_LOG_ONLY=Lib.Np,Lib.User
start "" ".\DriveClub.exe"
