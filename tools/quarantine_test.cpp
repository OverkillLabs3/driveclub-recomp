#include "../gpu/shim/bbport_recent.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const bool on = std::string(argv[1]) == "on";
    assert(on || std::string(argv[1]) == "off");
    const auto file = std::filesystem::temp_directory_path() /
        ("driveclub-quarantine-test-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    const std::string original = "11111111\n";
    { std::ofstream stream(file); assert(stream); stream << original; }
    SetEnv("BB_QUARANTINE", on ? "1" : "0");
    SetEnv("BB_QUARANTINE_FILE", file.string().c_str());
    SetEnv("BB_COMPUTE_SKIP", "22222222");
    assert(BbQuarantine::Enabled() == on);
    SetEnv("BB_QUARANTINE", on ? "0" : "1");
    assert(BbQuarantine::Enabled() == on); // startup decision remains cached
    SetEnv("BB_QUARANTINE", on ? "1" : "0");
    assert(BbQuarantine::Contains(0x11111111) == on);
    assert(BbQuarantine::Contains(0x22222222) == on);
    // Exercise ring wraparound and the draw boundary before the suspect dispatches.
    for (unsigned i = 0; i < BbRecent::Size + 3; ++i) BbRecent::Draw(0x55555555, 0x66666666, 3, 1);
    BbRecent::Dispatch(0x33333333, 1, 1, 1);
    BbRecent::Dispatch(0x44444444, 1, 1, 1);
    BbRecent::Dispatch(0x33333333, 1, 1, 1);
    assert(BbRecent::next.load() == ((BB_DIAGNOSTICS || on) ? BbRecent::Size + 6 : 0));
    BbQuarantine::NoteDeviceLost();
    if (on) {
        assert(BbQuarantine::count.load() == 4);
        // Reset loaded state and use the real loader to verify persisted entries.
        BbQuarantine::count.store(0);
        BbQuarantine::state.store(0);
        for (auto hash : {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u})
            assert(BbQuarantine::Contains(hash));
        assert(BbQuarantine::count.load() == 4);
        assert(!BbQuarantine::Contains(0x55555555));
        assert(!BbQuarantine::Contains(0x66666666));
    } else {
        std::ifstream stream(file);
        std::ostringstream contents; contents << stream.rdbuf();
        assert(contents.str() == original); // disabled quarantine must not rewrite its file
    }
    assert(std::filesystem::remove(file));
    std::printf("PASS: diagnostics=%d quarantine=%s; history policy, wraparound, save/reload, deduplication and disabled-file preservation.\n",
                BB_DIAGNOSTICS, on ? "on" : "off");
}
