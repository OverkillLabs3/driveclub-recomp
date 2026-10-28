"""Exercise the production DriveAA provider decision without a Vulkan device."""
from pathlib import Path
import subprocess
import sys

source = (Path(__file__).resolve().parents[1] / 'gpu/shadps4/video_core/renderer_vulkan/vk_drive_aa.cpp').read_text(encoding='utf-8')
start = source.index('int DriveAA::EffectiveMode() const {')
opening = source.index('{', start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
decision = source[start:end]

fixture = r'''
#include <cassert>
#include <cstdio>
namespace BbSettings {
enum { AaOff, AaFsr3, AaDlss, AaFsr411 };
struct Settings { int aa_mode = AaOff; };
Settings& Get() { static Settings settings; return settings; }
}
struct Instance { bool supported = false; bool IsFsr411PortableSupported() const { return supported; } };
struct Dlss { bool available = false; bool Available() const { return available; }
    static Dlss* current; static Dlss* Get() { return current; } };
Dlss* Dlss::current = nullptr;
struct DriveAA { Instance instance; bool dlss_failed = false; int EffectiveMode() const; };
''' + decision + r'''
int main() {
    DriveAA aa; Dlss dlss; Dlss::current = &dlss;
    BbSettings::Get().aa_mode = BbSettings::AaFsr411;
    assert(aa.EffectiveMode() == BbSettings::AaFsr3);
    assert(BbSettings::Get().aa_mode == BbSettings::AaFsr411);
    aa.instance.supported = true;
    assert(aa.EffectiveMode() == BbSettings::AaFsr411);
    BbSettings::Get().aa_mode = BbSettings::AaFsr3;
    assert(aa.EffectiveMode() == BbSettings::AaFsr3);
    BbSettings::Get().aa_mode = BbSettings::AaOff;
    assert(aa.EffectiveMode() == BbSettings::AaOff);
    BbSettings::Get().aa_mode = BbSettings::AaDlss;
    assert(aa.EffectiveMode() == BbSettings::AaFsr3);
    dlss.available = true;
    assert(aa.EffectiveMode() == BbSettings::AaDlss);
    aa.dlss_failed = true;
    assert(aa.EffectiveMode() == BbSettings::AaFsr3);
    std::puts("PASS: unsupported FSR4 selects FSR3; supported FSR4, original AA and DLSS fallbacks remain correct.");
}
'''
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
cpp, exe = output / 'drive-aa-mode-test.cpp', output / 'drive-aa-mode-test.exe'
cpp.write_text(fixture, encoding='utf-8')
subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
