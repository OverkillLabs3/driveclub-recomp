// Looks for a newer release on the project's Releases page and installs it over the program files.
#pragma once
#include <functional>
#include <string>

namespace Updater {

struct Release {
  std::wstring version;   // "0.3.1"
  std::wstring zip_url;   // the win64 zip
  std::wstring sums_url;  // SHA256SUMS.txt
};

// "0.3.1" newer than "0.3.0": compares the numbers of the dotted parts.
bool IsNewer(const std::wstring& candidate, const std::wstring& current);

// Reads the answer of the releases/latest call. False when it holds no release, or no zip and checksum file.
bool ParseRelease(const std::string& json, Release& out);

// The newest published release when it is newer than `current`. False when it is not, when the player is
// offline, or when anything else goes wrong (`why` says what, for the log).
bool CheckNewer(const std::wstring& current, Release& out, std::string& why);

// Downloads the zip, checks it against the checksum file, unpacks it into <home>\update_download,
// replaces program files and starts the new native launcher. The player's data, saves, mods,
// logs and launcher.ini are not touched. True: the caller must close. `progress` gets 0..100.
bool Install(const Release& release, const std::wstring& home, const std::function<void(int)>& progress,
             std::wstring& error);

// WPF maps its managed DLLs until it exits. Download/verify first, then let a
// native helper wait for that exact shell process before replacing the package.
bool InstallForShell(const Release& release, const std::wstring& home, unsigned long shell_pid,
                     const std::function<void(int)>& progress, std::wstring& error);
// Internal helper entry point. -1 means this is an ordinary launcher invocation.
int RunDeferred(int argc, wchar_t** argv);

// Removes the leftovers of an earlier update (the folder is in use while the script still runs). A folder that
// holds anything the updater did not write there (a game dump named "update", say) is left alone.
void CleanUp(const std::wstring& home);

}  // namespace Updater
