// Verify the real launcher compiler regenerates the exact player-validated
// variant.
#include "../launcher/prepare.cpp"
#include <iostream>
static void Check(bool ok, const char *s) {
  if (!ok)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << '\n';
}
int main(int argc, char **argv) {
  try {
    Check(argc == 4 || argc == 6, "usage: test eboot.elf patches.xml "
                                  "isolated.bin [selection validated.bin]");
    SelfFile self;
    self.elf = ReadFile(fs::path(argv[1]));
    auto off = Rd<uint64_t>(self.elf, 0x20);
    auto stride = Rd<uint16_t>(self.elf, 0x36),
         n = Rd<uint16_t>(self.elf, 0x38);
    uint64_t size = 0;
    for (unsigned i = 0; i < n; ++i) {
      auto p = off + uint64_t(i) * stride;
      Ph h{};
      h.type = Rd<uint32_t>(self.elf, p);
      h.flags = Rd<uint32_t>(self.elf, p + 4);
      h.offset = Rd<uint64_t>(self.elf, p + 8);
      h.vaddr = Rd<uint64_t>(self.elf, p + 16);
      h.filesz = Rd<uint64_t>(self.elf, p + 32);
      h.memsz = Rd<uint64_t>(self.elf, p + 40);
      self.ph.push_back(h);
      if (h.type == 1)
        size = std::max(size, h.vaddr + h.memsz);
    }
    auto xml = ReadFile(fs::path(argv[2]));
    auto entries = ParsePatches(std::string(xml.begin(), xml.end()));
    auto found =
        std::find_if(entries.begin(), entries.end(), [](const auto &e) {
          return e.name == "One-second collision penalties";
        });
    Check(found != entries.end() && found->lines.size() == 4,
          "four guarded one-second writes");
    std::vector<PatchEntry> one{*found};
    std::set<std::string> on{found->name}, off_selection;
    auto output = CompilePatches(one, &on, self, size);
    Check(Rd<uint64_t>(output, 16) == 4,
          "enabled option compiles all four writes");
    WriteFile(fs::path(argv[3]), output);
    if (argc == 6) {
      auto chosen = ReadLines(fs::path(argv[4]));
      std::set<std::string> selected(chosen.begin(), chosen.end());
      Check(CompilePatches(entries, &selected, self, size) ==
                ReadFile(fs::path(argv[5])),
            "public patch matches the player-validated production writes");
    }
    Check(Rd<uint64_t>(CompilePatches(one, &off_selection, self, size), 16) ==
              0,
          "unchecked option restores original native policy");
    for (const auto &line : found->lines) {
      auto address =
          std::strtoull(line.address.c_str(), nullptr, 16) - kPatchBase;
      SelfFile wrong = self;
      bool changed = false;
      for (const auto &h : wrong.ph)
        if (h.type == 1 && address >= h.vaddr && address < h.vaddr + h.filesz) {
          wrong.elf[size_t(h.offset + address - h.vaddr)] ^= 1;
          changed = true;
          break;
        }
      Check(changed &&
                Rd<uint64_t>(CompilePatches(one, &on, wrong, size), 16) == 0,
            "signature mismatch rejects all four writes atomically");
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
