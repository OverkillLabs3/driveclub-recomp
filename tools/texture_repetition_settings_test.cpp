#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "bbport_settings.h"


// Run each case in a fresh process: Load() is the startup settings loader.
// BB_CONFIG selects an isolated test file; BB_TEXTURE_REPETITION can override it.
int main(int argc, char** argv) {
    assert(argc == 2);
    const bool expected = std::strcmp(argv[1], "1") == 0;
    BbSettings::Load();
    auto& settings = BbSettings::Get();
    assert(settings.texture_repetition == expected);
    assert(settings.startup_texture_repetition == expected);
    assert(!BbSettings::ResolutionNeedsRestart());
    settings.texture_repetition = !expected;
    assert(BbSettings::ResolutionNeedsRestart());
    // A live edit must not change the code or cache folder of existing pipelines.
    assert(settings.startup_texture_repetition == expected);
    BbSettings::Save();
    FILE* file = std::fopen(std::getenv("BB_CONFIG"), "r");
    assert(file);
    char line[256];
    bool found = false;
    while (std::fgets(line, sizeof(line), file))
        if (std::strncmp(line, "texture_repetition=", 19) == 0)
            found = line[19] == (expected ? '0' : '1');
    std::fclose(file);
    assert(found);
    std::printf("PASS: startup=%d; override, persistence and immutable startup/restart boundary\n", int(expected));
}
