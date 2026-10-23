// SPDX-License-Identifier: GPL-2.0-or-later
// VR (experimental), see bbport_vr.h and packaging/vr/README.md.
//
// How it works:
// - The head camera hook (DriveClub 1.28, InstallHeadHook): the game's driver head camera update is patched at one
//   place to call bbvr_head_hook, which replaces the camera's world matrix the renderer draws from: the car body's
//   turn and the headset's (rigid: not the game's head on springs), at the seat point plus the headset's movement.
//   Every call is kept with its head pose and camera position (head_history).
// - Alternate frames: each call moves the camera to the next eye, ipd/2 along the head's own x axis.
// - The frame's camera: the G-buffer pass's constants hold the position of the camera the frame is drawn from
//   (NoteCameraConstants); matched with the hook's history it names the frame's eye and exact head pose, so no
//   frame goes to the wrong eye and none is stated at a guessed pose.
// - The picture: the game's display buffer at the output resolution (NoteFrame, vr_copy.comp; the window's picture
//   is scaled for the monitor). Each eye keeps its last picture; the headset's swapchain holds both side by side and
//   the projection layer states each at the pose it was drawn from, so the runtime turns it to the head's pose now.
// - Elsewhere (menus, loading, other cameras) the picture is a still screen in front of the player.
//
// vr.ini (next to DriveClubGame.exe), key = value lines, all optional:
//   afr = 1              stereo by alternate frames (each eye gets half the frame rate); 0: one picture for both eyes
//   ipd = 0.063          the distance between the eyes, metres
//   rigid = 1            the camera is fixed to the car body; 0: the game's simulated head (on springs) plus the headset
//   seat_x, seat_y, seat_z  where the eyes sit relative to the game's head point, metres along the car's axes (x left,
//                        y up, z forward); the seat keys move it 2 cm (with Ctrl 10 cm) while driving and the log
//                        prints the values to keep here
//   key_recentre = 0, key_up = 2, key_down = 1, key_left = 5, key_right = 6, key_forward = 4, key_back = 3
//                        the keys (one character, F1..F12, pageup, home, ...); the keypad works too (5, 8/2, 4/6,
//                        9/3)
//   smoothing = 1        steadies a head that holds still ("1 euro" filter); smooth_hz = 0.5 (lower: steadier),
//                        smooth_turn = 0.05 and smooth_move = 0.1 (how fast it lets go, per degree/s and per cm/s)
//   cockpit_vfov = 90    the driver's head camera's vertical field of view in the headset, degrees (VR sets the
//                        game's field of view for it while on: SetFieldOfView; F5/F6 do not apply then)
//   screen_vfov = 40     the height of the still screen, degrees
//   latency_ms = 33      only for a frame whose camera was not found: the head pose of that long ago is stated
// The recentre key (0, or keypad 5) makes the direction the head faces straight ahead.
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include "video_core/renderer_vulkan/vk_common.h"
#include "bbport_vr.h"
#include "video_core/host_shaders/vr_copy_comp.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" uintptr_t runtime_image_base; // src/probe.c: where the guest's image is

namespace BbVr {
std::mutex* g_queue_mutex = nullptr; // set by the presenter: every queue submission is made under it
}

namespace {

constexpr float Pi = 3.14159265358979f;
#define VKF VULKAN_HPP_DEFAULT_DISPATCHER

struct Functions {
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr;
    PFN_xrCreateInstance CreateInstance;
    PFN_xrGetSystem GetSystem;
    PFN_xrGetSystemProperties GetSystemProperties;
    PFN_xrCreateSession CreateSession;
    PFN_xrPollEvent PollEvent;
    PFN_xrBeginSession BeginSession;
    PFN_xrEndSession EndSession;
    PFN_xrWaitFrame WaitFrame;
    PFN_xrBeginFrame BeginFrame;
    PFN_xrEndFrame EndFrame;
    PFN_xrCreateReferenceSpace CreateReferenceSpace;
    PFN_xrLocateSpace LocateSpace;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats;
    PFN_xrCreateSwapchain CreateSwapchain;
    PFN_xrDestroySwapchain DestroySwapchain;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage;
    PFN_xrWaitSwapchainImage WaitSwapchainImage;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage;
    PFN_xrGetVulkanInstanceExtensionsKHR GetVulkanInstanceExtensionsKHR;
    PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensionsKHR;
    PFN_xrGetVulkanGraphicsDeviceKHR GetVulkanGraphicsDeviceKHR;
    PFN_xrGetVulkanGraphicsRequirementsKHR GetVulkanGraphicsRequirementsKHR;
} xr{};

struct Settings {
    bool afr = true, rigid = true, smoothing = true;
    bool auto_recentre = true; // recentre on getting into the car (after 3 s or more out of the cockpit view)
    bool diag = false; // diag = 1: the hook goes in without a headset too and logs the game's head point
    float ipd = 0.063f;
    float seat[3] = {0.0f, 0.07f, 0.0f}; // above the game's own head point (each car its own), which sits low
    float smooth_hz = 0.5f, smooth_turn = 0.05f, smooth_move = 0.1f;
    float cockpit_vfov = 90.0f, screen_vfov = 40.0f, latency_ms = 33.0f;
    // recentre, seat up, down, left, right, forward, back: the number row (on every keyboard); the keypad works too
    int keys[7] = {'0', '2', '1', '5', '6', '4', '3'};
} settings;

struct State {
    bool failed = false;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t swapchain_format = 0;
    uint32_t swapchain_width = 0, swapchain_height = 0;
    std::vector<XrSwapchainImageVulkanKHR> images;
    bool running = false;
    // the frame between RecordFrame and AfterSubmit
    bool frame_begun = false, image_acquired = false;
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    uint32_t width = 0, height = 0;   // one eye's picture
    bool afr_frame = false;           // the two eyes' last pictures side by side
    bool frame_tagged = false;        // one picture, its head pose known exactly
    XrPosef frame_pose{};
    bool cockpit_shown = false;
    long long cockpit_left_us = 0; // when the cockpit view was left last
    // the recentred head
    bool have_centre = false;
    float centre_yaw = 0.0f;
    XrVector3f centre_position{0, 0, 0};
} st;

std::atomic<bool> active{false};
std::string instance_extension_storage, device_extension_storage;
std::deque<std::string> extension_names;

long long NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool Ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) {
        return true;
    }
    std::printf("VR: %s failed (%d)\n", what, int(r));
    return false;
}

#ifdef _WIN32
std::wstring ExeDir() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}
#endif

// A key name of vr.ini: one character (a letter, a digit, [ ] ; and so on), F1..F12 or a few names. 0: unknown.
int KeyCode(const char* name) {
#ifdef _WIN32
    std::string n = name;
    for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (n.size() == 1) {
        const SHORT scan = VkKeyScanA(n[0]);
        return scan == -1 ? 0 : (scan & 0xff);
    }
    if (n[0] == 'f' && n.size() <= 3) {
        const int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    static const struct {
        const char* name;
        int code;
    } names[] = {{"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"home", VK_HOME}, {"end", VK_END},
                 {"insert", VK_INSERT}, {"delete", VK_DELETE}, {"up", VK_UP}, {"down", VK_DOWN},
                 {"left", VK_LEFT}, {"right", VK_RIGHT}, {"space", VK_SPACE}, {"tab", VK_TAB}};
    for (const auto& k : names) {
        if (n == k.name) return k.code;
    }
#endif
    return 0;
}

void ReadSettings() {
#ifdef _WIN32
    FILE* f = _wfopen((ExeDir() + L"vr.ini").c_str(), L"rb");
    if (!f) {
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char key[64] = "", name[32] = "";
        if (std::sscanf(line, " key_%63[a-z_] = %31s", key, name) == 2) {
            static const char* const actions[7] = {"recentre", "up", "down", "left", "right", "forward", "back"};
            for (int a = 0; a < 7; ++a) {
                if (std::string(key) == actions[a]) {
                    const int code = KeyCode(name);
                    if (code) settings.keys[a] = code;
                    else std::printf("VR: vr.ini: unknown key '%s'\n", name);
                }
            }
            continue;
        }
        float value = 0;
        if (std::sscanf(line, " %63[a-z_] = %f", key, &value) != 2) {
            continue;
        }
        const std::string k = key;
        if (k == "afr") settings.afr = value != 0;
        else if (k == "rigid") settings.rigid = value != 0;
        else if (k == "smoothing") settings.smoothing = value != 0;
        else if (k == "auto_recentre") settings.auto_recentre = value != 0;
        else if (k == "diag") settings.diag = value != 0;
        else if (k == "ipd" && value > 0.0f && value < 0.2f) settings.ipd = value;
        else if (k == "seat_x") settings.seat[0] = value;
        else if (k == "seat_y") settings.seat[1] = value;
        else if (k == "seat_z") settings.seat[2] = value;
        else if (k == "smooth_hz" && value > 0.01f) settings.smooth_hz = value;
        else if (k == "smooth_turn" && value >= 0) settings.smooth_turn = value;
        else if (k == "smooth_move" && value >= 0) settings.smooth_move = value;
        else if (k == "cockpit_vfov" && value > 10) settings.cockpit_vfov = value;
        else if (k == "screen_vfov" && value > 5) settings.screen_vfov = value;
        else if (k == "latency_ms" && value >= 0) settings.latency_ms = value;
    }
    std::fclose(f);
#endif
}

template <typename T>
void Load(T& out, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    xr.GetInstanceProcAddr(st.instance, name, &f);
    out = reinterpret_cast<T>(f);
}

// Starts OpenXR: the loader, the instance and the headset. False: VR stays off.
bool Start() {
#ifdef _WIN32
    HMODULE loader = LoadLibraryW(L"openxr_loader.dll");
    if (!loader) {
        std::printf("VR: openxr_loader.dll not found next to the game\n");
        return false;
    }
    xr.GetInstanceProcAddr =
        reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
#endif
    if (!xr.GetInstanceProcAddr) {
        return false;
    }
    Load(xr.CreateInstance, "xrCreateInstance");
    const char* const extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(info.applicationInfo.applicationName, "DriveClub");
    std::strcpy(info.applicationInfo.engineName, "DriveClub Recomp");
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    if (!xr.CreateInstance || !Ok(xr.CreateInstance(&info, &st.instance), "xrCreateInstance")) {
        return false;
    }
    Load(xr.GetSystem, "xrGetSystem");
    Load(xr.GetSystemProperties, "xrGetSystemProperties");
    Load(xr.CreateSession, "xrCreateSession");
    Load(xr.PollEvent, "xrPollEvent");
    Load(xr.BeginSession, "xrBeginSession");
    Load(xr.EndSession, "xrEndSession");
    Load(xr.WaitFrame, "xrWaitFrame");
    Load(xr.BeginFrame, "xrBeginFrame");
    Load(xr.EndFrame, "xrEndFrame");
    Load(xr.CreateReferenceSpace, "xrCreateReferenceSpace");
    Load(xr.LocateSpace, "xrLocateSpace");
    Load(xr.EnumerateSwapchainFormats, "xrEnumerateSwapchainFormats");
    Load(xr.CreateSwapchain, "xrCreateSwapchain");
    Load(xr.DestroySwapchain, "xrDestroySwapchain");
    Load(xr.EnumerateSwapchainImages, "xrEnumerateSwapchainImages");
    Load(xr.AcquireSwapchainImage, "xrAcquireSwapchainImage");
    Load(xr.WaitSwapchainImage, "xrWaitSwapchainImage");
    Load(xr.ReleaseSwapchainImage, "xrReleaseSwapchainImage");
    Load(xr.GetVulkanInstanceExtensionsKHR, "xrGetVulkanInstanceExtensionsKHR");
    Load(xr.GetVulkanDeviceExtensionsKHR, "xrGetVulkanDeviceExtensionsKHR");
    Load(xr.GetVulkanGraphicsDeviceKHR, "xrGetVulkanGraphicsDeviceKHR");
    Load(xr.GetVulkanGraphicsRequirementsKHR, "xrGetVulkanGraphicsRequirementsKHR");
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Ok(xr.GetSystem(st.instance, &system_info, &st.system), "xrGetSystem (is the headset connected?)")) {
        return false;
    }
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    xr.GetSystemProperties(st.instance, st.system, &props);
    std::printf("VR: headset %s\n", props.systemName);
    return true;
}

// "A B C" into separate names, added to `out` unless there; the names live in `extension_names` (a deque, so
// their storage never moves) for the Vulkan create infos.
void AddNames(const std::string& list, std::vector<const char*>& out) {
    size_t at = 0;
    while (at < list.size()) {
        const size_t end = list.find(' ', at);
        const std::string name = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? list.size() : end + 1;
        if (name.empty()) {
            continue;
        }
        bool have = false;
        for (const char* e : out) {
            have = have || (e && name == e);
        }
        if (!have) {
            extension_names.push_back(name);
            out.push_back(extension_names.back().c_str());
        }
    }
}

// ---- small maths ------------------------------------------------------------------------------------------------------
struct Quat {
    float x, y, z, w;
};
Quat Mul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat AxisAngle(float x, float y, float z, float radians) {
    const float s = std::sin(radians * 0.5f);
    return {x * s, y * s, z * s, std::cos(radians * 0.5f)};
}
Quat Conj(Quat q) {
    return {-q.x, -q.y, -q.z, q.w};
}
Quat ToQuat(const XrQuaternionf& q) {
    return {q.x, q.y, q.z, q.w};
}
XrVector3f Rotate(Quat q, XrVector3f v) {
    const Quat p = Mul(Mul(q, {v.x, v.y, v.z, 0}), Conj(q));
    return {p.x, p.y, p.z};
}
float AngleDeg(const XrQuaternionf& a, const XrQuaternionf& b) {
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0f * std::acos(std::fmin(1.0f, d)) * 180.0f / Pi;
}
XrQuaternionf Nlerp(XrQuaternionf a, XrQuaternionf b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    XrQuaternionf q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
// Yaw of where the head looks, degrees, right positive. OpenXR: -Z forward, +Y up.
float Yaw(const XrQuaternionf& q) {
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return std::atan2(fx, -fz) * 180.0f / Pi;
}

// ---- the session ------------------------------------------------------------------------------------------------------
void PollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xr.PollEvent(st.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& change = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            if (change.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                st.running = Ok(xr.BeginSession(st.session, &begin), "xrBeginSession");
            } else if (change.state == XR_SESSION_STATE_STOPPING) {
                xr.EndSession(st.session);
                st.running = false;
            } else if (change.state == XR_SESSION_STATE_EXITING || change.state == XR_SESSION_STATE_LOSS_PENDING) {
                st.running = false;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            st.running = false;
            st.failed = true;
            active = false;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool EnsureSwapchain(uint32_t width, uint32_t height) {
    if (st.swapchain && st.swapchain_width == width && st.swapchain_height == height) {
        return true;
    }
    if (st.swapchain) {
        xr.DestroySwapchain(st.swapchain);
        st.swapchain = XR_NULL_HANDLE;
    }
    if (!st.swapchain_format) {
        uint32_t count = 0;
        xr.EnumerateSwapchainFormats(st.session, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xr.EnumerateSwapchainFormats(st.session, count, &count, formats.data());
        // The pictures hold sRGB values in RGBA8: copied bit for bit into an sRGB image, the runtime reads them right.
        for (const int64_t f : formats) {
            if (f == VK_FORMAT_R8G8B8A8_SRGB) st.swapchain_format = f;
        }
        if (!st.swapchain_format) {
            std::printf("VR: the runtime has no R8G8B8A8_SRGB swapchain\n");
            st.failed = true;
            return false;
        }
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = st.swapchain_format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Ok(xr.CreateSwapchain(st.session, &info, &st.swapchain), "xrCreateSwapchain")) {
        st.failed = true;
        return false;
    }
    uint32_t count = 0;
    xr.EnumerateSwapchainImages(st.swapchain, 0, &count, nullptr);
    st.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xr.EnumerateSwapchainImages(st.swapchain, count, &count,
                                reinterpret_cast<XrSwapchainImageBaseHeader*>(st.images.data()));
    st.swapchain_width = width;
    st.swapchain_height = height;
    std::printf("VR: swapchain %ux%u\n", width, height);
    return true;
}

// ---- the driver's head camera, moved by the headset -------------------------------------------------------------------
// DriveClub 1.28, the driver's head camera's update (guest +0xb916d0, `this` in r15). At the hook (guest +0xb92420)
// +0x270..+0x2a0 hold the camera's world matrix the renderer draws from (just written by +0xb93510) and +0x2d0..+0x300
// the car body's: rows of four floats, the x, y and z axis and the position, in metres; the axes point left, up and
// forward.
bool hook_installed = false;
std::mutex head_mutex;
XrPosef head_now{{0, 0, 0, 1}, {0, 0, 0}}; // the headset, LOCAL space (from the present thread)
bool head_now_valid = false;
std::atomic<long long> hook_time_us{0};    // the hook's last run: the driver's head camera is in use

// Each call of the hook: the head pose it used and the camera it left for the game.
struct HookCall {
    long long time_us = 0;
    XrPosef head{};      // the middle of the head, LOCAL space
    float position[3]{}; // the camera's world position the game draws from
    int eye = -1;        // alternate frames: 0 the left eye, 1 the right, -1 the middle
    const float* camera = nullptr; // which driver's head camera
};
HookCall head_history[64];
unsigned head_next = 0;

// The head for the game, steadied: a "1 euro" filter (Casiez et al.), whose cut-off frequency rises with the speed:
// a head holding still is smoothed hard, a turning one passes almost as it is.
struct HeadFilter {
    bool init = false;
    long long time_us = 0;
    XrPosef pose{};
    float turn_speed = 0.0f, move_speed = 0.0f; // degrees/s, cm/s (smoothed)
} head_filter;
float Alpha(float hz, float dt) {
    return 1.0f / (1.0f + 1.0f / (2.0f * Pi * hz * dt));
}
XrPosef Steady(const XrPosef& raw) {
    HeadFilter& f = head_filter;
    const long long now = NowUs();
    if (!settings.smoothing || !f.init || now - f.time_us > 200000) {
        f = {true, now, raw, 0.0f, 0.0f};
        return raw;
    }
    const float dt = std::fmax(0.001f, float(now - f.time_us) / 1e6f);
    f.time_us = now;
    const float dx = raw.position.x - f.pose.position.x, dy = raw.position.y - f.pose.position.y,
                dz = raw.position.z - f.pose.position.z;
    const float turn = AngleDeg(f.pose.orientation, raw.orientation) / dt;
    const float move = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0f / dt;
    const float speed_alpha = Alpha(2.0f, dt);
    f.turn_speed += (turn - f.turn_speed) * speed_alpha;
    f.move_speed += (move - f.move_speed) * speed_alpha;
    const float a_turn = Alpha(settings.smooth_hz + settings.smooth_turn * f.turn_speed, dt);
    const float a_move = Alpha(settings.smooth_hz + settings.smooth_move * f.move_speed, dt);
    f.pose.orientation = Nlerp(f.pose.orientation, raw.orientation, a_turn);
    f.pose.position = {f.pose.position.x + dx * a_move, f.pose.position.y + dy * a_move,
                       f.pose.position.z + dz * a_move};
    return f.pose;
}

// The game updates more than one driver's head camera (two cars' or two cameras' of one car, each frame, one after
// the other): each keeps its own seat point and its own eye for alternate frames; the frame's camera match tells
// which one is drawn.
struct CameraState {
    const float* camera = nullptr;
    long long last_us = 0;
    float seat_point[3]{};
    long long seat_time_us = 0;
    int next_eye = 0;
    float diag_eye[3]{};
    long long diag_us = 0;
};
std::mutex camera_state_mutex;
CameraState camera_states[64];
// The camera the frames are drawn from (found by NoteCameraConstants), and when: the others (each opponent's car has
// one too) are left as the game has them.
std::atomic<const float*> drawn_camera{nullptr};
std::atomic<long long> drawn_camera_us{0};
CameraState& StateOf(const float* camera) {
    CameraState* oldest = &camera_states[0];
    for (auto& c : camera_states) {
        if (c.camera == camera) return c;
        if (c.last_us < oldest->last_us) oldest = &c;
    }
    *oldest = {};
    oldest->camera = camera;
    if (settings.diag) {
        int live = 0;
        for (const auto& c : camera_states) live += c.camera && NowUs() - c.last_us < 1000000;
        std::printf("VR diag: driver's head camera %p updated (%d updated lately)\n", (const void*)camera, live + 1);
    }
    return *oldest;
}

// diag = 1: where the game puts the driver's eyes, in the car's axes (x left, y up, z forward), each time it changes
// (another car, a restart): the head pose it builds (+0x310..+0x340, from the car's POSITION_OFFSET, the neck and the
// head-to-eyes offset) and the point of the camera the renderer draws from (+0x270).
void DiagHeadPoint(const float* camera, CameraState& state) {
    float* const last = state.diag_eye;
    long long& last_us = state.diag_us;
    const long long now = NowUs();
    const float* car = camera + 0x2d0 / 4;
    const float* drawn = camera + 0x270 / 4;
    const float* pose = camera + 0x310 / 4;
    const float eye[3] = {pose[12], pose[13], pose[14]};
    const float dx = eye[0] - last[0], dy = eye[1] - last[1], dz = eye[2] - last[2];
    if (last_us && dx * dx + dy * dy + dz * dz < 0.05f * 0.05f) {
        return;
    }
    last[0] = eye[0], last[1] = eye[1], last[2] = eye[2];
    last_us = now;
    float point[3];
    for (int i = 0; i < 3; ++i) {
        point[i] = 0.0f;
        for (int k = 0; k < 3; ++k) point[i] += (drawn[12 + k] - car[12 + k]) * car[i * 4 + k];
    }
    std::printf("VR diag: camera %p: the game's head pose puts the eyes at x %.3f y %.3f z %.3f; the drawn camera is at "
                "x %.3f y %.3f z %.3f (car axes, metres; the car's axis lengths %.3f %.3f %.3f)\n",
                (const void*)camera, eye[0], eye[1], eye[2], point[0], point[1], point[2],
                std::sqrt(car[0] * car[0] + car[1] * car[1] + car[2] * car[2]),
                std::sqrt(car[4] * car[4] + car[5] * car[5] + car[6] * car[6]),
                std::sqrt(car[8] * car[8] + car[9] * car[9] + car[10] * car[10]));
}

extern "C" __attribute__((sysv_abi)) void bbvr_head_hook(float* camera) {
    if (drawn_camera.load() && drawn_camera.load() != camera && NowUs() - drawn_camera_us.load() < 1000000) {
        return; // another car's camera, not drawn
    }
    std::unique_lock state_lock{camera_state_mutex};
    CameraState& state = StateOf(camera);
    state.last_us = NowUs();
    if (settings.diag) {
        DiagHeadPoint(camera, state);
    }
    XrPosef head;
    float centre_yaw, seat[3];
    XrVector3f centre_position;
    {
        std::scoped_lock lock{head_mutex};
        if (!head_now_valid || !st.have_centre) {
            return;
        }
        head = Steady(head_now);
        centre_yaw = st.centre_yaw;
        centre_position = st.centre_position;
        seat[0] = settings.seat[0], seat[1] = settings.seat[1], seat[2] = settings.seat[2];
    }
    // The head relative to the recentred pose (yaw only: up stays up), in the headset's axes.
    const Quat uncentre = AxisAngle(0, 1, 0, centre_yaw * Pi / 180.0f);
    const Quat q = Mul(uncentre, ToQuat(head.orientation));
    const XrVector3f d = Rotate(uncentre, {head.position.x - centre_position.x, head.position.y - centre_position.y,
                                           head.position.z - centre_position.z});
    // To the car's axes: OpenXR is X right, Y up, Z back; the car X left, Y up, Z forward (a half turn about Y).
    const Quat qe{-q.x, q.y, -q.z, q.w};
    const XrVector3f de{-d.x, d.y, -d.z};
    float r[3][3]; // the head's turn as rows (row vectors: row i is where axis i goes)
    for (int i = 0; i < 3; ++i) {
        const XrVector3f axis = Rotate(qe, {i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f});
        r[i][0] = axis.x, r[i][1] = axis.y, r[i][2] = axis.z;
    }
    float* const cam = camera + 0x270 / 4;
    float rest[3][3]; // the seat's frame the head turns in
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) rest[i][k] = cam[i * 4 + k];
    if (settings.rigid) {
        // The car body's frame, at the game's head point averaged over 2 s in the car's own axes: the game's head
        // sways on springs and lags the car's turns, which the player's head does not.
        const float* car = camera + 0x2d0 / 4;
        float offset[3];
        for (int i = 0; i < 3; ++i) {
            offset[i] = 0.0f;
            for (int k = 0; k < 3; ++k) offset[i] += (cam[12 + k] - car[12 + k]) * car[i * 4 + k];
        }
        float* const seat_point = state.seat_point;
        long long& seat_time_us = state.seat_time_us;
        const long long now = NowUs();
        if (!seat_time_us || now - seat_time_us > 1000000) {
            seat_point[0] = offset[0], seat_point[1] = offset[1], seat_point[2] = offset[2];
        } else {
            const float dt = float(now - seat_time_us) / 1e6f;
            for (int i = 0; i < 3; ++i) seat_point[i] += (offset[i] - seat_point[i]) * dt / (2.0f + dt);
        }
        seat_time_us = now;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) rest[i][k] = car[i * 4 + k];
        for (int k = 0; k < 3; ++k) {
            cam[12 + k] = car[12 + k] + seat_point[0] * car[k] + seat_point[1] * car[4 + k] + seat_point[2] * car[8 + k];
        }
    }
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) cam[i * 4 + k] = r[i][0] * rest[0][k] + r[i][1] * rest[1][k] + r[i][2] * rest[2][k];
    for (int k = 0; k < 3; ++k)
        cam[12 + k] += (seat[0] + de.x) * rest[0][k] + (seat[1] + de.y) * rest[1][k] + (seat[2] + de.z) * rest[2][k];
    // Alternate frames: each call is the next eye, ipd/2 along the head's own x axis (the camera's x row points left:
    // the left eye is on its + side).
    int eye = -1;
    if (settings.afr) {
        eye = state.next_eye;
        state.next_eye ^= 1;
        const float side = eye == 0 ? settings.ipd * 0.5f : -settings.ipd * 0.5f;
        for (int k = 0; k < 3; ++k) cam[12 + k] += side * cam[k];
    }
    state_lock.unlock();
    std::scoped_lock lock{head_mutex};
    HookCall& call = head_history[head_next++ % 64];
    call = {NowUs(), head, {cam[12], cam[13], cam[14]}, eye, camera};
    hook_time_us = call.time_us;
}

// The head pose of the call made `time_us` ago or before (frames whose camera was not found).
bool HeadAt(long long time_us, XrPosef& out) {
    std::scoped_lock lock{head_mutex};
    long long best = -1;
    for (const HookCall& c : head_history) {
        if (c.time_us && c.time_us <= time_us && c.time_us > best) {
            best = c.time_us;
            out = c.head;
        }
    }
    return best >= 0;
}

void InstallHeadHook() {
#ifdef _WIN32
    static const uint8_t expected[14] = {0xc4, 0xc1, 0x79, 0x6f, 0x87, 0x10, 0x03, 0x00, 0x00,  // vmovdqa xmm0,[r15+0x310]
                                         0xc5, 0xf9, 0x70, 0xc8, 0x00};                       // vpshufd xmm1,xmm0,0
    uint8_t* site = reinterpret_cast<uint8_t*>(runtime_image_base + 0xb92420);
    if (hook_installed || !runtime_image_base || std::memcmp(site, expected, sizeof expected) != 0) {
        if (!hook_installed) {
            std::printf("VR: the head camera hook does not fit this game version: no head tracking\n");
        }
        return;
    }
    // The trampoline: bbvr_head_hook(r15), the two instructions the jump replaces, back after them.
    uint8_t* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) {
        return;
    }
    size_t n = 0;
    const uint8_t mov_rdi_r15[] = {0x4c, 0x89, 0xff};
    std::memcpy(t + n, mov_rdi_r15, 3), n += 3;
    t[n++] = 0x48, t[n++] = 0xb8; // mov rax, imm64
    const uint64_t hook = reinterpret_cast<uint64_t>(&bbvr_head_hook);
    std::memcpy(t + n, &hook, 8), n += 8;
    t[n++] = 0xff, t[n++] = 0xd0; // call rax
    std::memcpy(t + n, expected, sizeof expected), n += sizeof expected;
    t[n++] = 0xff, t[n++] = 0x25, t[n++] = 0, t[n++] = 0, t[n++] = 0, t[n++] = 0; // jmp [rip]
    const uint64_t back = reinterpret_cast<uint64_t>(site + sizeof expected);
    std::memcpy(t + n, &back, 8), n += 8;
    FlushInstructionCache(GetCurrentProcess(), t, n);
    // At the site: jmp [rip] to the trampoline (14 bytes).
    uint8_t jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    const uint64_t to = reinterpret_cast<uint64_t>(t);
    std::memcpy(jump + 6, &to, 8);
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof jump, PAGE_EXECUTE_READWRITE, &old)) {
        return;
    }
    std::memcpy(site, jump, sizeof jump);
    VirtualProtect(site, sizeof jump, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof jump);
    hook_installed = true;
    std::printf("VR: head camera hook in\n");
#endif
}

// ---- the field of view ------------------------------------------------------------------------------------------------
// The game builds every camera's projection from half its angle: the angle times the float at +0x12efac4 (0.5; the
// launcher's field of view slider and F5/F6 change it, see camera_fov_apply in src/runtime_pad.c). The driver's head
// camera's angle is 60.18 degrees (FOCAL_LENGTH 14.01 mm, HALF_SENSOR_HEIGHT 8.12 mm in its settings): while VR is
// on, the float makes it cockpit_vfov (the other cameras widen alike; they are on the still screen).
constexpr float kCockpitVfov = 60.18f;
void SetFieldOfView() {
#ifdef _WIN32
    if (!hook_installed) {
        return; // another game version: its address is not known to be this
    }
    float* const k = reinterpret_cast<float*>(runtime_image_base + 0x12efac4);
    const float wanted = 0.5f * settings.cockpit_vfov / kCockpitVfov;
    if (std::fabs(*k - wanted) < 1e-6f) {
        return;
    }
    DWORD old = 0;
    if (VirtualProtect(k, 4, PAGE_READWRITE, &old)) {
        *k = wanted;
        VirtualProtect(k, 4, old, &old);
    }
#endif
}

// ---- the driver's head camera or a still screen ---------------------------------------------------------------------
// The projections the game draws with come from its anti-aliasing constants; the reflection cube map's faces (exactly
// square 90 x 90 degrees) come in between, so the cockpit's is kept apart: the driver's head camera is in use while
// its field of view was seen in the last 2 s and the hook ran in the last 300 ms.
std::atomic<float> cockpit_x{0.0f}, cockpit_y{0.0f};
std::atomic<long long> cockpit_time_us{0};
bool Cockpit() {
    const long long now = NowUs();
    return hook_installed && now - cockpit_time_us.load() <= 2000000 && now - hook_time_us.load() <= 300000;
}

// ---- pictures ---------------------------------------------------------------------------------------------------------
struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
};
struct Gpu {
    bool ready = false;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDescriptorSetLayout copy_set = VK_NULL_HANDLE;
    VkPipelineLayout copy_layout = VK_NULL_HANDLE;
    VkPipeline copy_pipeline = VK_NULL_HANDLE;
    std::mutex mutex;              // the pictures: made on the renderer's thread, read on the present thread
    std::vector<GpuImage> retired; // replaced by a resize (rare), kept: the GPU may still read them
    // The display buffer of each presenter frame, keyed by the frame's image, with the frame's camera.
    struct FramePicture {
        VkImage frame = VK_NULL_HANDLE;
        GpuImage picture; // RGBA8 holding sRGB values, TRANSFER_SRC_OPTIMAL
        uint32_t width = 0, height = 0;
        bool valid = false;
        unsigned last_use = 0;
        bool tagged = false; // its camera was found: drawn for `eye` with `head`
        int eye = -1;
        XrPosef head{};
    } pictures[4];
    unsigned picture_uses = 0;
    // Alternate frames: each eye's last picture and the head it was drawn for.
    GpuImage eye_picture[2];
    XrPosef eye_head[2]{};
    bool eye_valid[2]{};
} gpu;

struct CopyPush {
    float size[2];
    float encode;
};

void GpuInit(VkPhysicalDevice physical, VkDevice device) {
    gpu.device = device;
    gpu.physical = physical;
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    si.bindingCount = 2;
    si.pBindings = b;
    VKF.vkCreateDescriptorSetLayout(device, &si, nullptr, &gpu.copy_set);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CopyPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &gpu.copy_set;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    VKF.vkCreatePipelineLayout(device, &li, nullptr, &gpu.copy_layout);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(VR_COPY_COMP);
    mi.pCode = VR_COPY_COMP;
    VkShaderModule module = VK_NULL_HANDLE;
    if (VKF.vkCreateShaderModule(device, &mi, nullptr, &module) != VK_SUCCESS) {
        return;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = gpu.copy_layout;
    VKF.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &gpu.copy_pipeline);
    VKF.vkDestroyShaderModule(device, module, nullptr);
    gpu.ready = gpu.copy_pipeline != VK_NULL_HANDLE;
}

bool MakeImage(GpuImage& img, uint32_t width, uint32_t height, VkImageUsageFlags usage) {
    if (img.image && img.width == width && img.height == height) {
        return true;
    }
    if (img.image) {
        gpu.retired.push_back(img);
        img = {};
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {width, height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VKF.vkCreateImage(gpu.device, &ii, nullptr, &img.image) != VK_SUCCESS) {
        img = {};
        return false;
    }
    VkMemoryRequirements req;
    VKF.vkGetImageMemoryRequirements(gpu.device, img.image, &req);
    VkPhysicalDeviceMemoryProperties props;
    VKF.vkGetPhysicalDeviceMemoryProperties(gpu.physical, &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i) {
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
        }
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || VKF.vkAllocateMemory(gpu.device, &ai, nullptr, &img.memory) != VK_SUCCESS) {
        VKF.vkDestroyImage(gpu.device, img.image, nullptr);
        img = {};
        return false;
    }
    VKF.vkBindImageMemory(gpu.device, img.image, img.memory, 0);
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKF.vkCreateImageView(gpu.device, &vi, nullptr, &img.view);
    }
    img.width = width;
    img.height = height;
    return true;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKF.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
}

void Copy(VkCommandBuffer cmd, VkImage from, VkImage to, uint32_t width, uint32_t height, int32_t to_x) {
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstOffset = {to_x, 0, 0};
    copy.extent = {width, height, 1};
    VKF.vkCmdCopyImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

// ---- the camera of the frame being drawn ------------------------------------------------------------------------------
struct FrameCamera {
    bool found = false;
    int eye = -1;
    XrPosef head{};
};
std::mutex camera_mutex;
FrameCamera camera_now; // the frame being drawn

void ReadKeys() {
#ifdef _WIN32
    // Recentre, and the seat up/down, left/right and forward/back by 2 cm (Ctrl: 10 cm): the keys of vr.ini (the
    // number row 0, 2/1, 5/6, 4/3 by default) or the keypad (5, 8/2, 4/6, 9/3).
    static bool was[7];
    static const int keypad[7] = {VK_NUMPAD5, VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD9, VK_NUMPAD3};
    for (int k = 0; k < 7; ++k) {
        const bool down = (GetAsyncKeyState(settings.keys[k]) & 0x8000) != 0 || (GetAsyncKeyState(keypad[k]) & 0x8000) != 0;
        if (down && !was[k]) {
            std::scoped_lock lock{head_mutex};
            if (k == 0) {
                st.have_centre = false;
            } else {
                const int axis = k <= 2 ? 1 : k <= 4 ? 0 : 2;
                const float size = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 0.10f : 0.02f;
                settings.seat[axis] += (k % 2 == 1) ? size : -size;
                std::printf("VR: seat_x = %.2f, seat_y = %.2f, seat_z = %.2f (copy into vr.ini to keep)\n",
                            settings.seat[0], settings.seat[1], settings.seat[2]);
            }
        }
        was[k] = down;
    }
#endif
}

} // namespace

namespace BbVr {

bool Wanted() {
    static const bool wanted = [] {
        const char* env = std::getenv("BB_VR");
        if (env && *env) {
            return *env != '0';
        }
#ifdef _WIN32
        return GetFileAttributesW((ExeDir() + L"vr.ini").c_str()) != INVALID_FILE_ATTRIBUTES;
#else
        return false;
#endif
    }();
    return wanted;
}

bool Active() {
    return active.load(std::memory_order_relaxed);
}

void AppendInstanceExtensions(std::vector<const char*>& extensions) {
    if (!Wanted() || st.instance || st.failed) {
        return;
    }
    ReadSettings();
    if (!Start() || !xr.GetVulkanInstanceExtensionsKHR) {
        st.failed = true;
        std::printf("VR: off\n");
        return;
    }
    uint32_t size = 0;
    xr.GetVulkanInstanceExtensionsKHR(st.instance, st.system, 0, &size, nullptr);
    instance_extension_storage.assign(size, '\0');
    xr.GetVulkanInstanceExtensionsKHR(st.instance, st.system, size, &size, instance_extension_storage.data());
    instance_extension_storage.resize(std::strlen(instance_extension_storage.c_str()));
    AddNames(instance_extension_storage, extensions);
}

void AppendDeviceExtensions(VkInstance instance, VkPhysicalDevice physical_device, std::vector<const char*>& out) {
    if (!st.instance || st.failed) {
        return;
    }
    VkPhysicalDevice wanted = VK_NULL_HANDLE;
    if (Ok(xr.GetVulkanGraphicsDeviceKHR(st.instance, st.system, instance, &wanted), "xrGetVulkanGraphicsDeviceKHR") &&
        wanted != physical_device) {
        std::printf("VR: the headset wants another graphics card than the game uses\n");
    }
    uint32_t size = 0;
    xr.GetVulkanDeviceExtensionsKHR(st.instance, st.system, 0, &size, nullptr);
    device_extension_storage.assign(size, '\0');
    xr.GetVulkanDeviceExtensionsKHR(st.instance, st.system, size, &size, device_extension_storage.data());
    device_extension_storage.resize(std::strlen(device_extension_storage.c_str()));
    AddNames(device_extension_storage, out);
}

void OnDevice(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family,
              uint32_t queue_index) {
    if (Wanted() && settings.diag) {
        InstallHeadHook(); // without a headset too: the hook only logs then
    }
    if (!st.instance || st.failed) {
        return;
    }
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    xr.GetVulkanGraphicsRequirementsKHR(st.instance, st.system, &requirements);
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = instance;
    binding.physicalDevice = physical_device;
    binding.device = device;
    binding.queueFamilyIndex = queue_family;
    binding.queueIndex = queue_index;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = st.system;
    if (!Ok(xr.CreateSession(st.instance, &info, &st.session), "xrCreateSession")) {
        st.failed = true;
        return;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    xr.CreateReferenceSpace(st.session, &space, &st.local);
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xr.CreateReferenceSpace(st.session, &space, &st.view);
    GpuInit(physical_device, device);
    if (!gpu.ready) {
        std::printf("VR: the picture copy could not be set up\n");
        st.failed = true;
        return;
    }
    InstallHeadHook();
    active = true;
    std::printf("VR: on (%s, ipd %.1f mm, %s camera); keypad 5 recentres\n",
                settings.afr ? "stereo by alternate frames" : "one picture for both eyes", settings.ipd * 1000.0f,
                settings.rigid ? "car-fixed" : "the game's head");
}

void NoteProjection(float x_scale, float y_scale) {
    if (!Active() || x_scale <= 0.1f || y_scale <= 0.1f) {
        return;
    }
    const float vfov = 2.0f * std::atan(1.0f / y_scale) * 180.0f / Pi;
    if (std::fabs(vfov - settings.cockpit_vfov) < 3.0f && std::fabs(x_scale / y_scale - 1.0f) > 0.02f) {
        cockpit_x = x_scale;
        cockpit_y = y_scale;
        cockpit_time_us = NowUs();
    }
}

void NoteCameraConstants(const float* data) {
    // Many draws bind 512 bytes (the mirror's and the reflections' cameras too): each is tried until one is a camera
    // the hook gave the game (2 mm).
    if (!Active()) {
        return;
    }
    std::scoped_lock lock{camera_mutex};
    if (camera_now.found) {
        return;
    }
    std::scoped_lock head_lock{head_mutex};
    for (const HookCall& c : head_history) {
        const float dx = data[108] - c.position[0], dy = data[109] - c.position[1], dz = data[110] - c.position[2];
        if (c.time_us && dx * dx + dy * dy + dz * dz < 0.002f * 0.002f) {
            camera_now = {true, c.eye, c.head};
            drawn_camera = c.camera;
            drawn_camera_us = NowUs();
            return;
        }
    }
}

void NoteFrame(VkCommandBuffer cmd, VkImage frame, VkImageView display_view, VkFormat display_format,
               uint32_t display_width, uint32_t display_height) {
    if (!Active()) {
        return;
    }
    FrameCamera camera;
    {
        std::scoped_lock lock{camera_mutex};
        camera = camera_now;
        camera_now = {};
    }
    std::scoped_lock lock{gpu.mutex};
    Gpu::FramePicture* entry = nullptr;
    for (auto& e : gpu.pictures) {
        if (e.frame == frame) entry = &e;
    }
    if (!st.running || !display_view || !display_width || !display_height) {
        if (entry) entry->valid = false;
        return;
    }
    if (!entry) {
        entry = &gpu.pictures[0];
        for (auto& e : gpu.pictures) {
            if (e.last_use < entry->last_use) entry = &e;
        }
        entry->frame = frame;
    }
    entry->last_use = ++gpu.picture_uses;
    entry->valid = false;
    if (!MakeImage(entry->picture, display_width, display_height,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return;
    }
    const VkFormat f = display_format;
    const bool encode = f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    Barrier(cmd, entry->picture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_WRITE_BIT);
    VkDescriptorImageInfo infos[2] = {{VK_NULL_HANDLE, display_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {VK_NULL_HANDLE, entry->picture.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    const CopyPush push{{float(display_width), float(display_height)}, encode ? 1.0f : 0.0f};
    VKF.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.copy_pipeline);
    VKF.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.copy_layout, 0, 2, writes);
    VKF.vkCmdPushConstants(cmd, gpu.copy_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    VKF.vkCmdDispatch(cmd, (display_width + 7) / 8, (display_height + 7) / 8, 1);
    Barrier(cmd, entry->picture.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    entry->width = display_width, entry->height = display_height;
    entry->tagged = camera.found;
    entry->eye = camera.eye;
    entry->head = camera.head;
    entry->valid = true;
}

void RecordFrame(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height) {
    if (!Active() || st.failed) {
        return;
    }
    PollEvents();
    if (!st.running) {
        return;
    }
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    st.frame_state = {XR_TYPE_FRAME_STATE};
    if (!Ok(xr.WaitFrame(st.session, &wait, &st.frame_state), "xrWaitFrame")) {
        return;
    }
    {
        std::scoped_lock lock{*g_queue_mutex};
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        if (!Ok(xr.BeginFrame(st.session, &begin), "xrBeginFrame")) {
            return;
        }
    }
    st.frame_begun = true;

    SetFieldOfView(); // each frame: the launcher's own setting is applied once the game reads the pad
    // The head now: for the game's camera (the hook) and the still screen's place.
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xr.LocateSpace(st.view, st.local, st.frame_state.predictedDisplayTime, &location)) &&
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        ReadKeys();
        std::scoped_lock lock{head_mutex};
        if (!st.have_centre) {
            st.centre_yaw = Yaw(location.pose.orientation);
            st.centre_position = location.pose.position;
            st.have_centre = true;
            std::printf("VR: recentred (the head at %.2f, %.2f, %.2f m in the room)\n", location.pose.position.x,
                        location.pose.position.y, location.pose.position.z);
        }
        head_now = location.pose;
        head_now_valid = true;
    }

    // The frame's picture at the output resolution (the window's picture when there is none, or when the display
    // buffer is smaller than the window: loading screens).
    VkImage source = image;
    const Gpu::FramePicture* made = nullptr;
    {
        std::scoped_lock lock{gpu.mutex}; // the entry stays until this frame is presented and made again
        for (const auto& e : gpu.pictures) {
            if (e.frame == image && e.valid && e.width >= width) {
                made = &e;
                source = e.picture.image;
                width = e.width, height = e.height;
            }
        }
    }
    const bool cockpit = Cockpit();
    const int eye = settings.afr && cockpit && made && made->tagged ? made->eye : -1;
    if (!st.frame_state.shouldRender || !EnsureSwapchain(settings.afr ? width * 2 : width, height)) {
        return;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Ok(xr.AcquireSwapchainImage(st.swapchain, &acquire, &index), "xrAcquireSwapchainImage")) {
        return;
    }
    XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    image_wait.timeout = 100000000; // 100 ms
    if (!Ok(xr.WaitSwapchainImage(st.swapchain, &image_wait), "xrWaitSwapchainImage")) {
        return;
    }
    st.image_acquired = true;
    st.width = width;
    st.height = height;
    st.frame_tagged = cockpit && made && made->tagged;
    if (st.frame_tagged) {
        st.frame_pose = made->head;
    }
    if (eye >= 0) {
        // the frame's picture becomes its eye's
        GpuImage& pic = gpu.eye_picture[eye];
        if (pic.width != width || pic.height != height) {
            gpu.eye_valid[0] = gpu.eye_valid[1] = false;
        }
        if (MakeImage(pic, width, height, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            Barrier(cmd, pic.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            Copy(cmd, source, pic.image, width, height, 0);
            Barrier(cmd, pic.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            gpu.eye_head[eye] = made->head;
            gpu.eye_valid[eye] = true;
        }
    }
    const VkImage target = st.images[index].image;
    Barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    st.afr_frame = settings.afr && cockpit && (gpu.eye_valid[0] || gpu.eye_valid[1]);
    if (st.afr_frame) {
        // each eye's last picture in its half (the other eye's until both have one)
        for (int e = 0; e < 2; ++e) {
            Copy(cmd, gpu.eye_picture[gpu.eye_valid[e] ? e : 1 - e].image, target, width, height, int32_t(e * width));
        }
    } else {
        Copy(cmd, source, target, width, height, 0);
    }
    Barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
}

void AfterSubmit() {
    if (!st.frame_begun) {
        return;
    }
    st.frame_begun = false;
    XrCompositionLayerProjectionView views[2]{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = st.frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (st.image_acquired) {
        st.image_acquired = false;
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xr.ReleaseSwapchainImage(st.swapchain, &release);
        const bool cockpit = Cockpit();
        if (cockpit != st.cockpit_shown) {
            st.cockpit_shown = cockpit;
            // Getting into the car (a race starts, a restart, another car): the player sits as they drive now, so the
            // head is recentred there, unless the cockpit view was left only for a moment.
            if (cockpit && settings.auto_recentre && (!st.cockpit_left_us || NowUs() - st.cockpit_left_us > 3000000)) {
                std::scoped_lock lock{head_mutex};
                st.have_centre = false;
            }
            if (!cockpit) {
                st.cockpit_left_us = NowUs();
            }
            std::printf("VR: %s\n", cockpit ? "driver's head camera: the head moves the view" : "still screen");
        }
        // The pose each picture is stated at: the runtime turns it from there to the head's pose now.
        XrPosef pose{};
        float tan_y = std::tan(settings.screen_vfov * 0.5f * Pi / 180.0f);
        float tan_x = tan_y * float(st.width) / float(st.height); // a still screen: the picture's own shape
        if (cockpit) {
            // the scene's own field of view: the picture may be wider than the scene (the scene in the shape of an
            // eye, stretched into a 16:9 output for the menus)
            tan_y = 1.0f / cockpit_y.load();
            tan_x = cockpit_x.load() > 0.1f ? 1.0f / cockpit_x.load() : tan_y * float(st.width) / float(st.height);
            if (st.frame_tagged) {
                pose = st.frame_pose; // the head this frame was drawn for
            } else if (!HeadAt(NowUs() - (long long)(settings.latency_ms * 1000.0f), pose)) {
                pose = head_now;
            }
        } else {
            // a still screen straight ahead of the recentred head
            const Quat q = AxisAngle(0, 1, 0, -st.centre_yaw * Pi / 180.0f);
            pose.orientation = {q.x, q.y, q.z, q.w};
            pose.position = st.centre_position;
        }
        for (int e = 0; e < 2; ++e) {
            auto& v = views[e];
            v.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            v.pose = pose;
            v.subImage.imageRect = {{0, 0}, {int32_t(st.width), int32_t(st.height)}};
            if (st.afr_frame) {
                // this eye's last picture, at the head it was drawn for, ipd/2 to the side (OpenXR's x points right)
                const int from = gpu.eye_valid[e] ? e : 1 - e;
                const XrPosef& h = gpu.eye_head[from];
                const XrVector3f side = Rotate(ToQuat(h.orientation),
                                               {from == 0 ? -settings.ipd * 0.5f : settings.ipd * 0.5f, 0, 0});
                v.pose.orientation = h.orientation;
                v.pose.position = {h.position.x + side.x, h.position.y + side.y, h.position.z + side.z};
                v.subImage.imageRect.offset.x = int32_t(e * st.width);
            }
            v.fov = {-std::atan(tan_x), std::atan(tan_x), std::atan(tan_y), -std::atan(tan_y)};
            v.subImage.swapchain = st.swapchain;
        }
        layer.space = st.local;
        layer.viewCount = 2;
        layer.views = views;
        end.layerCount = 1;
        end.layers = layers;
    }
    Ok(xr.EndFrame(st.session, &end), "xrEndFrame");
}

} // namespace BbVr
