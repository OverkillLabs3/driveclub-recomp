// First-run preparation of the user's game dumps (see prepare.cpp).
#pragma once

#include <filesystem>
#include <functional>
#include <string>

// The format of work/prep/boot-linked.bin: a game prepared in another format is prepared again.
constexpr char kBootFormat[] = "BBPROBE6";

// Converts the update dump (and the base dump's modules) into the runtime image under `work`:
//   work/prep/boot-linked.bin, content.bin, patches.bin, eboot.elf, image.meta
// With patches_only the image is reused and only patches.bin is rebuilt.
// Returns an empty string on success, otherwise a message for the user.
// progress(step, text) is called with step 0..6.
// Whether a param.sfo says the game is version 1.28 (its APP_VER or VERSION field).
bool SfoIsVersion128(const std::filesystem::path& param_sfo);

// Whether a folder holds the version 1.28 update: its param.sfo says so, or (when a repacked dump has another
// version in the file) its eboot.bin is the very file of the 1.28 update.
bool FolderIsVersion128(const std::filesystem::path& folder);

// One text or number field of a param.sfo ("" when it is missing or the file cannot be read).
std::string SfoField(const std::filesystem::path& param_sfo, const char* key);

std::string PrepareGame(const std::filesystem::path& update, const std::filesystem::path& base,
                        const std::filesystem::path& work, const std::filesystem::path& patch_xml, bool patches_only,
                        const std::function<void(int, const std::string&)>& progress);
