// First-run preparation for the native Linux build: the Windows launcher prepares the game
// dumps through launcher/prepare.cpp; on Linux the AppImage's AppRun calls this CLI instead.
//
//   driveclub-prepare --update DIR --base DIR --work DIR [--patches FILE] [--patches-only]
//
// Writes <work>/prep/{boot-linked.bin,content.bin,patches.bin,eboot.elf,image.meta}, which the
// runtime takes as its boot image, content profile and patch file. Nothing in the game folders
// is modified.
#include "../launcher/prepare.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

void Usage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s --update DIR --base DIR --work DIR [--patches FILE] [--patches-only]\n"
                 "\n"
                 "  --update DIR    the version 1.28 update folder (eboot.bin, sce_sys/param.sfo)\n"
                 "  --base DIR      the base game folder (game000.dat)\n"
                 "  --work DIR      where the prepared files go (created when missing)\n"
                 "  --patches FILE  the patch XML (default: patches/DriveClub.xml)\n"
                 "  --patches-only  rebuild patches.bin from an already prepared image\n",
                 program);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path update, base, work, patches = "patches/DriveClub.xml";
    bool patches_only = false;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!std::strcmp(arg, "--help") || !std::strcmp(arg, "-h")) {
            Usage(argv[0]);
            return 0;
        }
        if (!std::strcmp(arg, "--patches-only")) {
            patches_only = true;
            continue;
        }
        const bool has_value = i + 1 < argc;
        if (!std::strcmp(arg, "--update") && has_value) {
            update = argv[++i];
        } else if (!std::strcmp(arg, "--base") && has_value) {
            base = argv[++i];
        } else if (!std::strcmp(arg, "--work") && has_value) {
            work = argv[++i];
        } else if (!std::strcmp(arg, "--patches") && has_value) {
            patches = argv[++i];
        } else {
            std::fprintf(stderr, "driveclub-prepare: unknown or incomplete option '%s'\n", arg);
            Usage(argv[0]);
            return 2;
        }
    }
    if (work.empty()) {
        std::fprintf(stderr, "driveclub-prepare: --work is required\n");
        Usage(argv[0]);
        return 2;
    }
    if (!patches_only && (update.empty() || base.empty())) {
        std::fprintf(stderr, "driveclub-prepare: --update and --base are required\n");
        Usage(argv[0]);
        return 2;
    }
    std::error_code ec;
    if (!patches_only) {
        if (!fs::is_directory(update, ec)) {
            std::fprintf(stderr, "driveclub-prepare: the update folder does not exist: %s\n",
                         update.string().c_str());
            return 1;
        }
        if (!fs::is_directory(base, ec)) {
            std::fprintf(stderr, "driveclub-prepare: the base game folder does not exist: %s\n",
                         base.string().c_str());
            return 1;
        }
        if (!FolderIsVersion128(update)) {
            std::fprintf(stderr,
                         "driveclub-prepare: the update folder must be DriveClub version 1.28 "
                         "(CUSA00093 or CUSA00003), with sce_sys/param.sfo and eboot.bin\n");
            return 1;
        }
    }
    if (!fs::is_regular_file(patches, ec)) {
        std::fprintf(stderr, "driveclub-prepare: the patch file does not exist: %s\n",
                     patches.string().c_str());
        return 1;
    }

    int last_step = -1;
    std::string message;
    try {
        message = PrepareGame(update, base, work, patches, patches_only, [&](int step, const std::string& text) {
            if (step != last_step) {
                std::printf("prepare: [%d/6] %s\n", step, text.c_str());
                std::fflush(stdout);
                last_step = step;
            }
        });
    } catch (const std::exception& error) {
        std::fprintf(stderr, "driveclub-prepare: %s\n", error.what());
        return 1;
    }
    if (!message.empty()) {
        std::fprintf(stderr, "driveclub-prepare: %s\n", message.c_str());
        return 1;
    }
    std::printf("prepare: ready in %s\n", (work / "prep").string().c_str());
    return 0;
}
