// Builds the data files that make all the game's add-on content playable (cars, bikes, tours, livery decals).
#pragma once
#include <string>

// Reads the vehicle list, the tour list and the livery list from the player's own game archive and writes changed
// copies to `out_dir`. The runtime serves them in place of the originals; the game folders are never touched.
// Skips the work when the copies in `out_dir` already match the archive. Returns "" or a short description of a problem.
// all_cars: every car and bike is also free of its driver level and club level.
std::wstring BuildUnlock(const std::wstring& update_dir, const std::wstring& base_dir, const std::wstring& out_dir,
                         bool all_cars = false);
