// Builds the data files that make the game's add-on content playable (cars, bikes, tours, livery decals) on Linux.
// A port of launcher/unlock.cpp and the archive reader in launcher/datx.cpp: the game folders are only read, the
// changed copies are written to `out_dir` and the runtime serves them as an extra mods folder.
#pragma once
#include <string>

// all_cars: every car and bike is also free of its driver level and club level.
std::string BuildUnlockFiles(const std::string& update_dir, const std::string& base_dir, const std::string& out_dir,
                             bool all_cars);
