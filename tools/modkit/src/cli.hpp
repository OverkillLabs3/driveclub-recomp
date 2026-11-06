// SPDX-License-Identifier: GPL-2.0-or-later
// Command line plumbing for dcmod: arguments, game folders, wildcards.
#pragma once

#include "archive.hpp"

#include <functional>
#include <map>

namespace dcmod {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> options;  // "--name" -> values (flags have an empty value)

    bool Has(const std::string& name) const { return options.count(name) != 0; }
    std::string Get(const std::string& name, const std::string& fallback = {}) const {
        const auto it = options.find(name);
        return it == options.end() || it->second.empty() ? fallback : it->second.back();
    }
    std::vector<std::string> All(const std::string& name) const {
        const auto it = options.find(name);
        return it == options.end() ? std::vector<std::string>{} : it->second;
    }
};

// `value_options` names the options that take a value; every other "--x" is a flag.
Args ParseArgs(const std::vector<std::string>& argv, const std::vector<std::string>& value_options);

// The game folders from --update/--base/--game, DRIVECLUB_UPDATE_DIR/DRIVECLUB_BASE_DIR, or the update= and base=
// lines of launcher.ini (in the working directory or next to the program).
std::vector<fs::path> GameFolders(const Args& args);
std::unique_ptr<Archive> OpenGame(const Args& args);

// '*' matches any run of characters, '?' one; any case.
bool WildMatch(std::string_view pattern, std::string_view text);

std::string HumanSize(uint64_t bytes);

using Command = std::function<int(const std::vector<std::string>& argv)>;
int RunArchive(const std::vector<std::string>& argv);
int RunRpk(const std::vector<std::string>& argv);
int RunLua(const std::vector<std::string>& argv);
int RunMod(const std::vector<std::string>& argv);
int RunTex(const std::vector<std::string>& argv);
int RunAudio(const std::vector<std::string>& argv);
int RunMesh(const std::vector<std::string>& argv);
int RunStudio(const std::vector<std::string>& argv);
int RunDash(const std::vector<std::string>& argv);
int RunCar(const std::vector<std::string>& argv);
int RunMcp(const std::vector<std::string>& argv);

}  // namespace dcmod
