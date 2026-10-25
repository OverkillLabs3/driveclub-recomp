// Requires the player's own 1.28 eboot.elf. No game bytes are distributed here.
// Tests the real patch compiler and executes only three small isolated setters,
// never the game, SDL, a controller, or any force-feedback device.
#include "../launcher/prepare.cpp"
#include <windows.h>
#include <array>
#include <iostream>

static void Check(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
  std::cout << "PASS " << what << '\n';
}

int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("usage: collision_penalty_test eboot.elf DriveClub.xml");
    SelfFile self;
    self.elf = ReadFile(fs::path(argv[1]));
    // Read real program headers, rather than assuming a file/image offset.
    const auto phoff = Rd<uint64_t>(self.elf, 0x20);
    const auto phsize = Rd<uint16_t>(self.elf, 0x36);
    const auto phnum = Rd<uint16_t>(self.elf, 0x38);
    uint64_t size = 0;
    for (unsigned i = 0; i < phnum; ++i) {
      const auto p = phoff + uint64_t(i) * phsize;
      Ph ph{};
      ph.type=Rd<uint32_t>(self.elf,p); ph.flags=Rd<uint32_t>(self.elf,p+4);
      ph.offset=Rd<uint64_t>(self.elf,p+8); ph.vaddr=Rd<uint64_t>(self.elf,p+16);
      ph.filesz=Rd<uint64_t>(self.elf,p+32); ph.memsz=Rd<uint64_t>(self.elf,p+40);
      self.ph.push_back(ph);
      if(ph.type==1)size=std::max(size,ph.vaddr+ph.memsz);
    }
    const auto xml = ReadFile(fs::path(argv[2]));
    const auto entries = ParsePatches(std::string(xml.begin(),xml.end()));
    const std::string name="Disable collision slowdown penalties";
    std::vector<PatchEntry> only;
    for(const auto& e:entries)if(e.name==name)only.push_back(e);
    Check(only.size()==1 && only[0].lines.size()==2,"two guarded collision patches found");
    const std::set<std::string> off, on{name};
    auto disabled=CompilePatches(only,&off,self,size);
    Check(Rd<uint64_t>(disabled,16)==0,"disabled option produces no writes");
    auto enabled=CompilePatches(only,&on,self,size);
    Check(Rd<uint64_t>(enabled,16)==2,"enabled option matches original 1.28 instructions");
    // A mismatch at either site must reject the whole option, not half-apply it.
    for (const auto& line:only[0].lines) {
      const auto address=std::strtoull(line.address.c_str(),nullptr,16)-kPatchBase;
      SelfFile wrong=self;
      for(const auto& ph:self.ph)if(ph.type==1 && address>=ph.vaddr && address<ph.vaddr+ph.filesz)
        wrong.elf[size_t(ph.offset+address-ph.vaddr)]^=1;
      Check(Rd<uint64_t>(CompilePatches(only,&on,wrong,size),16)==0,"unexpected instructions reject both writes");
    }
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,size_t(size),MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!image)throw std::runtime_error("VirtualAlloc failed");
    for(const auto& ph:self.ph)if(ph.type==1)std::memcpy(image+ph.vaddr,self.elf.data()+ph.offset,size_t(ph.filesz));
    using Wall=void(__attribute__((sysv_abi)) *)(void*,float);
    using Contact=void(__attribute__((sysv_abi)) *)(void*,float,float,float);
    auto wall=reinterpret_cast<Wall>(image+0x644a20);
    auto contact=reinterpret_cast<Contact>(image+0x644a70);
    auto corner=reinterpret_cast<Contact>(image+0x644a30);
    alignas(16) std::array<uint8_t,80> state{};
    auto f=[&](size_t at)->float& {return *reinterpret_cast<float*>(state.data()+at);};
    wall(state.data(),25.0f);
    Check(state[0xc]==1 && f(0x10)==25.0f,"original wall setter requests extra slowdown");
    state.fill(0);f(0x2c)=-999.0f;
    contact(state.data(),1000000.0f,100.0f,0.0f);
    Check(state[0x3f]==1 && f(0x24)==1.0f && f(0x28)==3.0f,"original car contact requests three-second penalty");
    size_t at=24;size_t changed=0;
    for(unsigned i=0;i<2;i++) {
      const auto offset=Rd<uint64_t>(enabled,at);const auto n=Rd<uint64_t>(enabled,at+8);at+=16;
      for(size_t j=0;j<n;j++)if(image[offset+j]!=enabled[at+j])++changed;
      std::memcpy(image+offset,enabled.data()+at,size_t(n));at+=size_t(n);
    }
    FlushInstructionCache(GetCurrentProcess(),image,size_t(size));
    Check(changed==2,"only two function-entry bytes change in the game image");
    state.fill(0);const auto blank=state;wall(state.data(),25.0f);
    Check(state==blank,"patched wall setter leaves slowdown state untouched");
    state.fill(0);f(0x2c)=-999.0f;const auto before=state;
    contact(state.data(),1000000.0f,100.0f,0.0f);
    Check(state==before,"patched contact setter starts no penalty timer");
    state.fill(0);f(0x2c)=-999.0f;
    corner(state.data(),1000000.0f,100.0f,0.0f);
    Check(state[0x3d]==1 && f(0x1c)==1.0f && f(0x20)==3.0f,"corner-cut penalty remains functional");
    VirtualFree(image,0,MEM_RELEASE);
    return 0;
  } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
