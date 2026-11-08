// SPDX-License-Identifier: GPL-2.0-or-later
#include "cli.hpp"

#include <algorithm>
#include <cstdlib>

namespace dcmod {

Args ParseArgs(const std::vector<std::string>& argv, const std::vector<std::string>& value_options) {
    Args a;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        if (s.size() > 1 && s[0] == '-' && !(s.size() == 2 && s[1] >= '0' && s[1] <= '9')) {
            std::string name = s;
            std::string value;
            bool has_value = false;
            const size_t eq = s.find('=');
            if (StartsWith(s, "--") && eq != std::string::npos) {
                name = s.substr(0, eq);
                value = s.substr(eq + 1);
                has_value = true;
            }
            const bool takes = std::find(value_options.begin(), value_options.end(), name) != value_options.end();
            if (takes && !has_value) {
                if (i + 1 >= argv.size()) throw Error("the option " + name + " needs a value");
                value = argv[++i];
            }
            a.options[name].push_back(value);
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

namespace {

std::string IniValue(const fs::path& ini, const std::string& key) {
    std::error_code ec;
    if (!fs::exists(ini, ec)) return {};
    std::string text;
    try {
        text = ReadText(ini);
    } catch (const Error&) {
        return {};
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (StartsWith(line, key + "=")) return line.substr(key.size() + 1);
        pos = end + 1;
    }
    return {};
}

}  // namespace

std::vector<fs::path> GameFolders(const Args& args) {
    std::vector<fs::path> out;
    for (const std::string& s : args.All("--update")) out.emplace_back(s);
    for (const std::string& s : args.All("--game")) out.emplace_back(s);
    for (const std::string& s : args.All("--base")) out.emplace_back(s);
    if (!out.empty()) return out;
    if (const char* e = std::getenv("DRIVECLUB_UPDATE_DIR"); e && *e) out.emplace_back(e);
    if (const char* e = std::getenv("DRIVECLUB_BASE_DIR"); e && *e) out.emplace_back(e);
    if (!out.empty()) return out;
    for (const fs::path& ini : {fs::path("launcher.ini"), fs::path("..") / "launcher.ini"}) {
        const std::string u = IniValue(ini, "update"), b = IniValue(ini, "base");
        if (!u.empty()) out.emplace_back(u);
        if (!b.empty()) out.emplace_back(b);
        if (!out.empty()) return out;
    }
    return out;
}

std::unique_ptr<Archive> OpenGame(const Args& args) {
    const std::vector<fs::path> folders = GameFolders(args);
    if (folders.empty()) {
        throw Error("no game folders: use --update DIR and --base DIR, set DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR, "
                    "or run it where launcher.ini is");
    }
    return Archive::Open(folders);
}

bool WildMatch(std::string_view pattern, std::string_view text) {
    const std::string p = Lower(std::string(pattern)), t = Lower(std::string(text));
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::string HumanSize(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GB", double(bytes) / double(1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / double(1ull << 20));
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", double(bytes) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

}  // namespace dcmod
