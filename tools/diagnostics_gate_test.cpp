#include "../gpu/shim/bbport_diagnostics.h"
#include "../gpu/shim/bbport_recent.h"
#include "../gpu/shim/bbport_toggles.h"
#include "../gpu/shim/core/emulator_settings.h"
#include "../gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

int main() {
    SetEnv("BB_QUARANTINE", "0");
    const auto trigger = std::filesystem::temp_directory_path() /
        ("driveclub-diagnostics-gate-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    { std::ofstream file(trigger); assert(file); file << "capture"; }
    SetEnv("BB_CAPTURE_TRIGGER", trigger.string().c_str());
    SetEnv("BB_DUMP_SHADERS", "1");
    SetEnv("BB_VK_MARKERS", "1");
    SetEnv("BB_VK_VALIDATION", "1");
    assert(std::getenv("BB_CAPTURE_TRIGGER"));
    assert(bool(BB_DIAGNOSTIC_ENV("BB_CAPTURE_TRIGGER")) == bool(BB_DIAGNOSTICS));
    auto* settings = EmulatorSettingsImpl::GetInstance();
    assert(settings->IsDumpShaders() == bool(BB_DIAGNOSTICS));
    assert(settings->IsVkGuestMarkersEnabled() == bool(BB_DIAGNOSTICS));
    assert(settings->IsVkHostMarkersEnabled() == bool(BB_DIAGNOSTICS));
    assert(settings->IsVkValidationEnabled() == bool(BB_DIAGNOSTICS));
    BbRecent::Draw(1, 2, 3, 4);
    BbRecent::Dispatch(1, 2, 3, 4);
    assert(BbRecent::next.load() == (BB_DIAGNOSTICS ? 2u : 0u));
    // Mirror feedback is functional state; disabling statistics must not disable it.
    BbStats::NoteMirrorFeedback(0x12345678);
    assert(BbStats::IsMirrorFeedback(0x12345678));
    assert(!BbStats::IsMirrorFeedback(0x12345679));
    using Vulkan::FrameCapture;
    for (unsigned i = 0; i < 24; ++i) {
        FrameCapture::AddDisplayBuffer(0x12345000 + i * 0x1000);
        FrameCapture::AddDisplayBuffer(0x12345000 + i * 0x1000);
        assert(FrameCapture::IsDisplayBuffer(0x12345000 + i * 0x1000));
    }
    assert(!FrameCapture::IsDisplayBuffer(0x12345001));
    const auto flips = FrameCapture::Flips();
    FrameCapture::OnFlip(0x12345000);
    assert(FrameCapture::Flips() == flips + 1);
    assert(FrameCapture::Active() == bool(BB_DIAGNOSTICS));
    assert(std::filesystem::exists(trigger) == !bool(BB_DIAGNOSTICS));
    if (std::filesystem::exists(trigger)) assert(std::filesystem::remove(trigger));
    std::printf("PASS: %s build diagnostic flags, capture trigger, breadcrumbs; functional flips, display registry overflow and mirror feedback preserved.\n",
                BB_DIAGNOSTICS ? "diagnostic" : "normal");
}
