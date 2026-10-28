// The renderer needs Vulkan 1.3 and a few extensions and features (the same list as the code that creates the
// device). Without them the game would stop right after the start; this finds out first and says why. It only
// refuses when it is sure: a driver that cannot be queried, that takes too long or that crashes is never a reason
// to keep a player from starting.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <windows.h>
#include <cpuid.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include "gpu_check.h"

#include <versionhelpers.h>
#include <wbemidl.h>
#include <comdef.h>
#pragma comment(lib, "wbemuuid.lib")

namespace {

GpuCapabilities capabilities;
std::wstring ExeDir();

std::wstring Widen(const char* text) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  if (n <= 1) return L"";
  std::wstring w(size_t(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text, -1, w.data(), n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

struct Candidate {
  VkPhysicalDevice handle{};
  VkPhysicalDeviceProperties props{};
  unsigned long long local_memory = 0;
  int index = 0;  // position in the driver's list: what BB_GPU_ID counts
  std::wstring problem;  // "" when the card has everything
  std::wstring name;
};

struct Outcome {
  std::wstring message;  // "" fine, or when the check could not be completed
  int use_index = -1;    // >= 0: the renderer must use this card
  GpuCapabilities capabilities;
};

GpuCapabilities FeatureSupport(const Candidate& card) {
  auto support = CapabilitiesForGpu(card.name);
  const auto runtime = ExeDir() + L"\\runtime\\";
  support.dlss_runtime = GetFileAttributesW((runtime + L"bbport_dlss.dll").c_str()) != INVALID_FILE_ATTRIBUTES &&
                         GetFileAttributesW((runtime + L"nvngx_dlss.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
  auto neural_runtime = std::filesystem::path(runtime);
  if (const wchar_t* env = _wgetenv(L"BB_NR_RUNTIME"); env && *env) {
    neural_runtime = env;
    if (_wcsicmp(neural_runtime.extension().c_str(), L".dll") == 0) neural_runtime = neural_runtime.parent_path();
  }
  support.neural_runtime = GetFileAttributesW((neural_runtime / L"nvngx_dlssnr.dll").c_str()) != INVALID_FILE_ATTRIBUTES &&
                           GetFileAttributesW((runtime + L"bbport_nr_nvngx.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
  if (!support.dlss_runtime) {
    support.dlss = false;
    support.frame_multiplier_max = 0;
  }
  if (GetFileAttributesW((runtime + L"nvngx_dlssg.dll").c_str()) == INVALID_FILE_ATTRIBUTES) support.frame_multiplier_max = 0;
  if (!support.neural_runtime) support.neural = false;
  return support;
}
const wchar_t* kTail = L" Update the graphics driver; if it still does not run, this card is not supported."
                       L" (To start anyway, turn on 'Skip the graphics card check' in the launcher.)";

// What one card lacks, or "" when it has everything the renderer needs.
std::wstring Problem(const Candidate& c, PFN_vkEnumerateDeviceExtensionProperties get_extensions,
                     PFN_vkGetPhysicalDeviceFeatures2 get_features2, bool* certain) {
  *certain = true;
  if (c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) return L"This is a software renderer, not a graphics card.";
  if (c.props.apiVersion < VK_API_VERSION_1_3) {
    const std::wstring have = std::to_wstring(VK_VERSION_MAJOR(c.props.apiVersion)) + L"." + std::to_wstring(VK_VERSION_MINOR(c.props.apiVersion));
    return L"Your graphics card (" + c.name + L") offers Vulkan " + have + L", the game needs 1.3." + kTail;
  }
  uint32_t count = 0;
  if (get_extensions(c.handle, nullptr, &count, nullptr) != VK_SUCCESS) { *certain = false; return L""; }
  std::vector<VkExtensionProperties> extensions(count);
  if (get_extensions(c.handle, nullptr, &count, extensions.data()) != VK_SUCCESS) { *certain = false; return L""; }
  std::wstring missing;
  const auto need = [&](const char* wanted) {
    const bool has = std::any_of(extensions.begin(), extensions.end(),
                                 [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, wanted) == 0; });
    if (!has) missing += (missing.empty() ? L"" : L", ") + Widen(wanted);
  };
  need("VK_KHR_swapchain");
  need("VK_KHR_push_descriptor");
  need("VK_EXT_vertex_attribute_divisor");
  need("VK_EXT_robustness2");
  if (!missing.empty()) {
    return L"Your graphics card (" + c.name + L") lacks Vulkan extensions the game needs (" + missing + L")." + kTail;
  }
  VkPhysicalDeviceRobustness2FeaturesEXT robustness{};
  robustness.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
  VkPhysicalDeviceFeatures2 features{};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = &robustness;
  get_features2(c.handle, &features);
  if (!robustness.robustBufferAccess2) missing += L"robustBufferAccess2";
  if (!robustness.robustImageAccess2) missing += (missing.empty() ? L"" : L", ") + std::wstring(L"robustImageAccess2");
  if (!robustness.nullDescriptor) missing += (missing.empty() ? L"" : L", ") + std::wstring(L"nullDescriptor");
  if (!missing.empty()) {
    return L"Your graphics card (" + c.name + L") lacks Vulkan features the game needs (" + missing + L")." + kTail;
  }
  return L"";
}

Outcome Query() {
  // To see how a refusal looks: set DRIVECLUB_FAKE_GPU_PROBLEM=1.
  if (GetEnvironmentVariableW(L"DRIVECLUB_FAKE_GPU_PROBLEM", nullptr, 0) > 0) {
    return {L"Your graphics card (Example GPU) lacks Vulkan features the game needs (robustBufferAccess2, nullDescriptor)." + std::wstring(kTail), -1};
  }
  HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
  if (!lib) {
    return {L"No Vulkan graphics driver was found. Install or update your graphics driver (NVIDIA, AMD or Intel), then start the game again.", -1};
  }
  const auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
  if (!get_instance_proc) return {};
  const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
  if (!create_instance) return {};

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "DriveClub";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = nullptr;
  if (create_instance(&info, nullptr, &instance) != VK_SUCCESS || !instance) return {};

  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
  const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
  const auto get_memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
  const auto get_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get_instance_proc(instance, "vkEnumerateDeviceExtensionProperties"));
  const auto get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get_instance_proc(instance, "vkGetPhysicalDeviceFeatures2"));
  if (!enumerate || !get_props || !get_memory || !get_extensions || !get_features2) return {};

  uint32_t count = 0;
  if (enumerate(instance, &count, nullptr) != VK_SUCCESS) return {};
  if (count == 0) {
    return {L"No graphics card with Vulkan support was found. Install or update your graphics driver, then start the game again.", -1};
  }
  std::vector<VkPhysicalDevice> handles(count);
  if (enumerate(instance, &count, handles.data()) != VK_SUCCESS) return {};
  handles.resize(count);

  std::vector<Candidate> devices;
  bool all_certain = true;
  for (uint32_t i = 0; i < handles.size(); ++i) {
    Candidate c;
    c.handle = handles[i];
    c.index = int(i);
    get_props(c.handle, &c.props);
    c.name = Widen(c.props.deviceName);
    VkPhysicalDeviceMemoryProperties memory{};
    get_memory(c.handle, &memory);
    for (uint32_t h = 0; h < memory.memoryHeapCount; ++h) {
      if (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        c.local_memory = std::max<unsigned long long>(c.local_memory, memory.memoryHeaps[h].size);
      }
    }
    bool certain = true;
    c.problem = Problem(c, get_extensions, get_features2, &certain);
    all_certain = all_certain && certain;
    devices.push_back(c);
  }
  // A card whose answer could not be read: do not refuse anybody on a guess.
  if (!all_certain) return {};

  // The renderer's order: a driver new enough first, then a discrete card, no software renderer, most video memory.
  std::stable_sort(devices.begin(), devices.end(), [](const Candidate& a, const Candidate& b) {
    const bool a_api = a.props.apiVersion >= VK_API_VERSION_1_3, b_api = b.props.apiVersion >= VK_API_VERSION_1_3;
    if (a_api != b_api) return a_api;
    const bool a_disc = a.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const bool b_disc = b.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    if (a_disc != b_disc) return a_disc;
    const bool a_cpu = a.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU, b_cpu = b.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (a_cpu != b_cpu) return b_cpu;
    return a.local_memory > b.local_memory;
  });

  wchar_t override_index[32];
  if (GetEnvironmentVariableW(L"BB_GPU_ID", override_index, 32) > 0) {
    const int index = _wtoi(override_index);
    if (index >= 0) {
      for (const auto& card : devices) {
        if (card.index == index) return {card.problem, -1, FeatureSupport(card)};
      }
      return {};
    }
  }
  if (devices.front().problem.empty()) return {L"", -1, FeatureSupport(devices.front())};
  // The renderer would pick a card that cannot run it: take the first one that can.
  for (const Candidate& c : devices) {
    if (c.problem.empty() && c.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) return {L"", c.index, FeatureSupport(c)};
  }
  // Only software renderers: say so rather than blaming one of them.
  const bool only_software = std::all_of(devices.begin(), devices.end(), [](const Candidate& c) {
    return c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
  });
  if (only_software) {
    return {L"Only a software renderer was found, no real graphics card. Install or update your graphics driver, then start the game again.", -1};
  }
  for (const Candidate& c : devices) {
    if (c.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) return {c.problem, -1};
  }
  return {};
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring ExeDir() {
  wchar_t path[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  std::wstring dir(path, n);
  const size_t slash = dir.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

}  // namespace

bool GpuCompatibilityCheckBypassed() {
  return FileExists(ExeDir() + L"\\skip_gpu_check.txt") || GetEnvironmentVariableW(L"DRIVECLUB_SKIP_GPU_CHECK", nullptr, 0) > 0;
}

int GpuCheckChildMain(const wchar_t* result_path) {
  const Outcome outcome = Query();
  const auto& c = outcome.capabilities;
  std::string text = outcome.message.empty() ? "ok\n" : "bad\n";
  text += "index=" + std::to_string(outcome.use_index) + "\nname=" + Narrow(c.name) +
          "\nchecked=" + std::to_string(c.checked) + "\ndlss=" + std::to_string(c.dlss) +
          "\nframe_max=" + std::to_string(c.frame_multiplier_max) + "\nneural=" + std::to_string(c.neural) +
          "\nreflex=" + std::to_string(c.reflex) + "\nfsr411=" + std::to_string(c.fsr411) +
          "\ndlss_runtime=" + std::to_string(c.dlss_runtime) + "\nneural_runtime=" + std::to_string(c.neural_runtime) +
          "\nmessage=" + Narrow(outcome.message) + "\n";
  FILE* f = _wfopen(result_path, L"wb");
  if (f) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
  return outcome.message.empty() ? 0 : 1;
}

// Forward declaration
std::wstring CheckAncientIgpuDriver();

std::wstring CheckGpu() {
  capabilities = {};
  if (GpuCompatibilityCheckBypassed()) {
    capabilities = DisplayGraphicsCapabilities();
    return L"";
  }
  wchar_t temp[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, temp)) return L"";
  const std::wstring result = std::wstring(temp) + L"driveclub_gpucheck_" + std::to_wstring(GetCurrentProcessId()) + L".txt";
  DeleteFileW(result.c_str());

  wchar_t self[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
  if (n == 0) return L"";
  std::wstring command = L"\"" + std::wstring(self, n) + L"\" --gpu-check \"" + result + L"\"";
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return L"";
  // A driver that hangs or crashes must not keep anyone from playing: no answer in time is no objection.
  const DWORD waited = WaitForSingleObject(pi.hProcess, 8000);
  DWORD code = 0xFFFFFFFF;
  if (waited == WAIT_OBJECT_0) {
    GetExitCodeProcess(pi.hProcess, &code);
  } else {
    TerminateProcess(pi.hProcess, 2);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (waited != WAIT_OBJECT_0 || (code != 0 && code != 1)) {
    DeleteFileW(result.c_str());
    return L"";
  }

  std::string text;
  if (FILE* f = _wfopen(result.c_str(), L"rb")) {
    char chunk[512];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) text.append(chunk, got);
    std::fclose(f);
  }
  DeleteFileW(result.c_str());
  const auto value = [&](const char* key) {
    const std::string prefix = std::string("\n") + key + "=";
    const auto start = text.find(prefix);
    if (start == std::string::npos) return std::string{};
    const auto begin = start + prefix.size();
    return text.substr(begin, text.find('\n', begin) - begin);
  };
  capabilities.checked = value("checked") == "1";
  capabilities.name = Widen(value("name").c_str());
  capabilities.dlss = value("dlss") == "1";
  capabilities.frame_multiplier_max = std::atoi(value("frame_max").c_str());
  capabilities.neural = value("neural") == "1";
  capabilities.reflex = value("reflex") == "1";
  capabilities.fsr411 = value("fsr411") == "1";
  capabilities.dlss_runtime = value("dlss_runtime") == "1";
  capabilities.neural_runtime = value("neural_runtime") == "1";
  if (text.rfind("bad\n", 0) == 0 && code == 1) return Widen(value("message").c_str());
  if (text.rfind("ok\n", 0) == 0) {
    const size_t at = text.find("index=");
    if (at != std::string::npos && std::atoi(text.c_str() + at + 6) >= 0 && GetEnvironmentVariableW(L"BB_GPU_ID", nullptr, 0) == 0) {
      SetEnvironmentVariableW(L"BB_GPU_ID", Widen(std::to_string(std::atoi(text.c_str() + at + 6)).c_str()).c_str());
    }
  }

  std::wstring igpu_warning = CheckAncientIgpuDriver();
  if (!igpu_warning.empty()) return igpu_warning;

  return L"";
}

const GpuCapabilities& GraphicsCapabilities() { return capabilities; }

GpuCapabilities DisplayGraphicsCapabilities() {
  // An explicit Vulkan device index cannot safely be mapped to a Windows display index.
  if (GetEnvironmentVariableW(L"BB_GPU_ID", nullptr, 0) > 0) return FeatureSupport(Candidate{});
  std::wstring name;
  for (DWORD index = 0;; ++index) {
    DISPLAY_DEVICEW display{};
    display.cb = sizeof(display);
    if (!EnumDisplayDevicesW(nullptr, index, &display, 0)) break;
    // A detached adapter can still be the renderer's preferred discrete GPU.
    if (display.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
    if (!name.empty() && _wcsicmp(name.c_str(), display.DeviceString) != 0) return FeatureSupport(Candidate{});
    name = display.DeviceString;
  }
  if (name.empty()) return FeatureSupport(Candidate{});
  Candidate card;
  card.name = name;
  return FeatureSupport(card);
}

GpuCapabilities CapabilitiesForGpu(const std::wstring& name) {
  GpuCapabilities support;
  support.checked = !name.empty();
  support.name = name;
  std::wstring model = name;
  std::transform(model.begin(), model.end(), model.begin(), [](wchar_t c) { return std::towupper(c); });
  const auto rtx = model.find(L"RTX");
  if (rtx != std::wstring::npos) {
    const int number = _wtoi(model.c_str() + rtx + 3);
    int series = number % 100 >= 50 ? number / 100 : 0;
    if (model.find(L"ADA") != std::wstring::npos) series = 40;
    if (model.find(L"BLACKWELL") != std::wstring::npos) series = 50;
    support.dlss = support.reflex = support.fsr411 = true;
    support.frame_multiplier_max = series == 50 ? 6 : series == 40 ? 2 : 0;
    support.neural = series == 50;
  } else if (const auto gtx = model.find(L"GTX"); gtx != std::wstring::npos) {
    const int number = _wtoi(model.c_str() + gtx + 3);
    support.reflex = number >= 900;
    support.fsr411 = number >= 1000;
  } else if (const auto rx = model.find(L"RADEON RX"); rx != std::wstring::npos) {
    const int number = _wtoi(model.c_str() + rx + 9);
    support.fsr411 = number >= 5000;
  } else if (model.find(L"INTEL") != std::wstring::npos && model.find(L"ARC") != std::wstring::npos) {
    support.fsr411 = true;
  }
  return support;
}

std::wstring CheckCpu() {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return L"";
  std::wstring missing;
  const auto need = [&](bool have, const wchar_t* name) {
    if (have) return;
    if (!missing.empty()) missing += L", ";
    missing += name;
  };
  bool avx = (c & (1u << 28)) != 0 && (c & (1u << 27)) != 0;
  if (avx) {
    unsigned lo = 0, hi = 0;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    avx = (lo & 6) == 6;
  }
  need(avx, L"AVX");
  need((c & (1u << 23)) != 0, L"POPCNT");
  need((c & (1u << 22)) != 0, L"MOVBE");
  unsigned b7 = 0, d7 = 0, c7 = 0, a7 = 0;
  need(__get_cpuid_count(7, 0, &a7, &b7, &c7, &d7) && (b7 & (1u << 3)) != 0, L"BMI1");
  unsigned ae = 0, be = 0, ce = 0, de = 0;
  need(__get_cpuid(0x80000001u, &ae, &be, &ce, &de) && (ce & (1u << 5)) != 0, L"LZCNT");
  if (missing.empty()) return L"";
  return L"Your processor does not support " + missing +
         L", which the game needs. Intel Haswell (2013) or newer, or AMD Excavator or Ryzen or newer, is required. If you are sure your processor has these, turn on \"Skip the hardware check\".";
}

std::wstring DescribeCpu() {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char brand[64] = {0};
  for (unsigned i = 0; i < 3; ++i) {
    if (!__get_cpuid(0x80000002u + i, &a, &b, &c, &d)) return L"unknown processor";
    memcpy(brand + i * 16 + 0, &a, 4);
    memcpy(brand + i * 16 + 4, &b, 4);
    memcpy(brand + i * 16 + 8, &c, 4);
    memcpy(brand + i * 16 + 12, &d, 4);
  }
  const char* name = brand;
  while (*name == ' ') ++name;
  unsigned c1 = 0;
  __get_cpuid(1, &a, &b, &c1, &d);
  unsigned b7 = 0, c7 = 0, d7 = 0, a7 = 0;
  __get_cpuid_count(7, 0, &a7, &b7, &c7, &d7);
  unsigned c81 = 0;
  __get_cpuid(0x80000001u, &a, &b, &c81, &d);
  char features[160];
  snprintf(features, sizeof features, "; avx %d avx2 %d bmi1 %d movbe %d lzcnt %d sse4a %d", int((c1 >> 28) & 1), int((b7 >> 5) & 1),
           int((b7 >> 3) & 1), int((c1 >> 22) & 1), int((c81 >> 5) & 1), int((c81 >> 6) & 1));
  std::string text(name);
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return Widen((text + features).c_str());
}

std::wstring CheckAncientIgpuDriver() {
  std::wstring warning;
  HRESULT hres = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool com_init = SUCCEEDED(hres);
  IWbemLocator* locator = nullptr;
  IWbemServices* services = nullptr;
  IEnumWbemClassObject* enumerator = nullptr;
  
  if (com_init) {
    hres = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                            IID_IWbemLocator, reinterpret_cast<void**>(&locator));
  }
  if (com_init && locator) {
    hres = locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr,
                                  0, nullptr, nullptr, &services);
  }
  if (com_init && services) {
    hres = CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                             RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  }
  if (com_init && services) {
    hres = services->ExecQuery(_bstr_t(L"WQL"),
                               _bstr_t(L"SELECT DriverVersion, DriverDate, Name FROM Win32_VideoController WHERE Name LIKE '%AMD%' OR Name LIKE '%Radeon%' OR Name LIKE '%Intel%' OR Name LIKE '%UHD%' OR Name LIKE '%Iris%' OR Name LIKE '%HD Graphics%'"),
                               WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
  }
  if (com_init && enumerator) {
    IWbemClassObject* obj = nullptr;
    ULONG returned = 0;
    while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == WBEM_S_NO_ERROR && returned == 1) {
      VARIANT v_name, v_version, v_date;
      VariantInit(&v_name); VariantInit(&v_version); VariantInit(&v_date);
      obj->Get(L"Name", 0, &v_name, nullptr, nullptr);
      obj->Get(L"DriverVersion", 0, &v_version, nullptr, nullptr);
      obj->Get(L"DriverDate", 0, &v_date, nullptr, nullptr);
      
      if (v_name.vt == VT_BSTR && v_version.vt == VT_BSTR && v_date.vt == VT_BSTR) {
        std::wstring name(v_name.bstrVal, SysStringLen(v_name.bstrVal));
        std::wstring version(v_version.bstrVal, SysStringLen(v_version.bstrVal));
        std::wstring date_str(v_date.bstrVal, SysStringLen(v_date.bstrVal));
        
        // Parse driver date: WMI format is YYYYMMDDHHMMSS.mmmmmm+UUU
        // e.g., 20210825053000.000000+000
        if (date_str.size() >= 8) {
          int year = _wtoi(date_str.substr(0, 4).c_str());
          int month = _wtoi(date_str.substr(4, 2).c_str());
          // Warn if driver is older than 2 years
          SYSTEMTIME now;
          GetLocalTime(&now);
          int current_year = now.wYear;
          int current_month = now.wMonth;
          int months_old = (current_year - year) * 12 + (current_month - month);
          
          if (months_old > 24) { // Older than 2 years
            if (!warning.empty()) warning += L"\n";
            warning += L"\u26A0 Ancient AMD GPU driver detected: " + name;
            warning += L"\n   Driver version: " + version;
            warning += L"\n   Driver date: " + std::to_wstring(year) + L"-" + std::to_wstring(month);
            warning += L" (" + std::to_wstring(months_old) + L" months old)";
            warning += L"\n   This will cause Vulkan initialization to hang on hybrid laptops (iGPU + dGPU).";
            warning += L"\n   Please update your GPU driver (AMD Adrenalin / Intel Arc Control / laptop OEM support page).";
          }
        }
      }
      VariantClear(&v_name); VariantClear(&v_version); VariantClear(&v_date);
      obj->Release();
    }
  }
  
  if (enumerator) enumerator->Release();
  if (services) services->Release();
  if (locator) locator->Release();
  if (com_init) CoUninitialize();
  
  return warning;
}

std::wstring CheckVulkanLayers() {
  // Placeholder for future Vulkan layer checks
  return L"";
}

std::wstring DescribeSystem() {
  unsigned long major = 0, minor = 0, build = 0;
  typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  RtlGetVersionFn get_version = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  if (get_version) {
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    if (get_version(&info) == 0) {
      major = info.dwMajorVersion;
      minor = info.dwMinorVersion;
      build = info.dwBuildNumber;
    }
  }
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof memory;
  unsigned long long total = 0, avail = 0;
  if (GlobalMemoryStatusEx(&memory)) {
    total = memory.ullTotalPhys >> 20;
    avail = memory.ullAvailPhys >> 20;
  }
  wchar_t text[200];
  swprintf(text, 200, L"Windows %lu.%lu build %lu, %llu MB memory (%llu MB free)%ls", major, minor, build, total, avail,
           ntdll && GetProcAddress(ntdll, "wine_get_version") ? L", running under Wine/Proton" : L"");
  return text;
}
