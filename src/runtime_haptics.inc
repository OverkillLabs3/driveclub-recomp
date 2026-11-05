/* DualSense haptics (BB_ADAPTIVE_TRIGGERS=1, USB only). The controller's two voice-coil actuators are fed through its
 * USB sound card: the endpoint "Speakers (DualSense Wireless Controller)" has four channels at 48 kHz, the back left and
 * back right ones (3 and 4) drive the actuators. The sound is made from the car's data: engine note from the engine speed,
 * road and kerb bumps from the change of each wheel's load, tyre scrub from the slip, a thump at every gear change, a
 * pulse on the rev limiter. The game's own rumble is mixed in, and not sent to the motors: SDL turns the audio haptics
 * off while the rumble motors run. */
#define COBJMACROS
#include <mmdeviceapi.h>
#include <audioclient.h>

typedef struct {
    volatile float rpm, limiter, speed, throttle, brake;
    volatile float load[4], lat[4], lon[4], ratio[4], angle[4];   /* wheels: right front, left front, right rear, left rear */
    volatile int gear, shift_up;
    volatile uint32_t shifts;   /* counts the gear changes */
    volatile uint64_t us;       /* when the car was last read */
} HapIn;
static HapIn hap_in;
static volatile int hap_active;   /* the haptics stream runs: the game's rumble goes to the mix, not the motors */
static volatile uint8_t hap_rumble_large, hap_rumble_small;
static int hap_on = -1;
static float hap_gain = 1.0f;

static const GUID hap_clsid_enum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID hap_iid_enum = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID hap_iid_client = {0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID hap_iid_render = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const PROPERTYKEY hap_key_name = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

static float hap_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

typedef struct { float lp; } HapLow;
static uint32_t hap_seed = 2463534242u;
static float hap_noise(void) {
    hap_seed ^= hap_seed << 13; hap_seed ^= hap_seed >> 17; hap_seed ^= hap_seed << 5;
    return (float)(int32_t)hap_seed * (1.0f / 2147483648.0f);
}
static float hap_low(HapLow *f, float x, float a) { f->lp = f->lp * a + x * (1.0f - a); return f->lp; }

/* The endpoint of a DualSense: the render device whose name says so. */
static IMMDevice *hap_find_device(IMMDeviceEnumerator *en) {
    IMMDeviceCollection *col = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) return NULL;
    UINT n = 0;
    IMMDeviceCollection_GetCount(col, &n);
    IMMDevice *found = NULL;
    for (UINT i = 0; i < n && !found; ++i) {
        IMMDevice *d = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
        IPropertyStore *ps = NULL;
        bool match = false;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
            PROPVARIANT v;
            memset(&v, 0, sizeof v);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &hap_key_name, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                match = wcsstr(v.pwszVal, L"DualSense") != NULL;
            IPropertyStore_Release(ps);
        }
        if (match) found = d; else IMMDevice_Release(d);
    }
    IMMDeviceCollection_Release(col);
    return found;
}

static DWORD WINAPI hap_thread(void *arg) {
    (void)arg;
    HMODULE ole = LoadLibraryA("ole32.dll");
    if (!ole) return 0;
    HRESULT (WINAPI *co_init)(void *, DWORD) = (HRESULT (WINAPI *)(void *, DWORD))GetProcAddress(ole, "CoInitializeEx");
    HRESULT (WINAPI *co_create)(const GUID *, void *, DWORD, const GUID *, void **) =
        (HRESULT (WINAPI *)(const GUID *, void *, DWORD, const GUID *, void **))GetProcAddress(ole, "CoCreateInstance");
    if (!co_init || !co_create) return 0;
    co_init(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    WAVEFORMATEX *fmt = NULL;
    HRESULT hr = co_create(&hap_clsid_enum, NULL, CLSCTX_ALL, &hap_iid_enum, (void **)&en);
    if (SUCCEEDED(hr)) dev = hap_find_device(en);
    if (!dev) { printf("Runtime: haptics: no DualSense sound device (USB cable needed), triggers only\n"); return 0; }
    hr = IMMDevice_Activate(dev, &hap_iid_client, CLSCTX_ALL, NULL, (void **)&ac);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetMixFormat(ac, &fmt);
    if (FAILED(hr) || !fmt || fmt->nChannels < 4 || fmt->wBitsPerSample != 32) {
        printf("Runtime: haptics: unusable format (hr 0x%lx, %u channels), triggers only\n", (unsigned long)hr, fmt ? fmt->nChannels : 0);
        return 0;
    }
    const unsigned channels = fmt->nChannels, rate = fmt->nSamplesPerSec;
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 300000, 0, fmt, NULL);   /* 30 ms buffer */
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(ac, &hap_iid_render, (void **)&rc);
    UINT32 frames = 0;
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(ac, &frames);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(ac);
    if (FAILED(hr)) { printf("Runtime: haptics: stream failed (hr 0x%lx), triggers only\n", (unsigned long)hr); return 0; }
    printf("Runtime: haptics on: DualSense speakers, %u channels at %u Hz, buffer %u frames\n", channels, rate, frames);
    hap_active = 1;

    const float dt = 1.0f / (float)rate;
    float engine_p1 = 0, engine_p2 = 0, rumble_p1 = 0, rumble_p2 = 0, limit_ph = 0, shift_t = 9.0f, shift_amp = 0, limit_p = 0;
    float engine_amp = 0, side_bump[2] = {0, 0}, side_squeal[2] = {0, 0}, rev_smooth = 800, road_amp = 0;
    float load_lp[4] = {0, 0, 0, 0};
    uint32_t seen_shifts = hap_in.shifts;
    HapLow road_lp[2] = {{0}, {0}}, squeal_lp[2] = {{0}, {0}}, bump_lp[2] = {{0}, {0}};
    const float a_road = expf(-6.2831853f * 160.0f * dt), a_bump = expf(-6.2831853f * 90.0f * dt);
    const float a_squeal_hi = expf(-6.2831853f * 260.0f * dt);
    while (hap_on) {
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) break;
        UINT32 room = frames - pad;
        if (room < rate / 200) { Sleep(2); continue; }   /* wait for at least 5 ms of room */
        if (room > rate / 100) room = rate / 100;        /* control step: 10 ms */
        BYTE *buf = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, room, &buf))) break;
        float *out = (float *)buf;
        const uint64_t now = now_us();
        const bool fresh = hap_in.us && now - hap_in.us < 300000;
        const float step = (float)room * dt;
        /* control rate: targets from the car */
        float rpm = hap_in.rpm, limiter = hap_in.limiter, speed = hap_in.speed, thr = hap_in.throttle, brk = hap_in.brake;
        float target_engine = 0, target_road = 0, target_bump[2] = {0, 0}, target_sq[2] = {0, 0}, weight = 0, a_lat = 0;
        if (fresh) {
            for (int i = 0; i < 4; ++i) weight += hap_in.load[i];
            const float mass = weight > 1000.0f ? weight / 9.81f : 1000.0f;
            for (int i = 0; i < 4; ++i) a_lat += hap_in.lat[i];
            a_lat = hap_clampf(-a_lat / mass / 12.0f, -1.0f, 1.0f);   /* to the right is positive */
            target_engine = 0.10f + 0.34f * thr + 0.10f * hap_clampf((rpm - 800.0f) / 7000.0f, 0, 1);
            target_road = 0.05f + 0.26f * hap_clampf(speed / 70.0f, 0, 1);
            const float hi = weight > 1000.0f ? weight : 1000.0f;
            for (int s = 0; s < 2; ++s) {   /* s 0 right, 1 left */
                float hp = 0, slip = 0;
                for (int k = 0; k < 2; ++k) {
                    const int w = s == 0 ? (k == 0 ? 0 : 2) : (k == 0 ? 1 : 3);
                    load_lp[w] += (hap_in.load[w] - load_lp[w]) * hap_clampf(step * 4.0f, 0, 1);
                    hp += hap_in.load[w] - load_lp[w];
                    const float r = fabsf(hap_in.ratio[w]), an = fabsf(hap_in.angle[w]) * 2.0f;
                    slip = fmaxf(slip, fmaxf(r, an));
                }
                target_bump[s] = hap_clampf(fabsf(hp) / (hi * 0.10f), 0, 1);
                if (speed > 8.0f) target_sq[s] = hap_clampf((slip - 0.14f) * 1.6f, 0, 1);
            }
            if (hap_in.shifts != seen_shifts) {
                seen_shifts = hap_in.shifts;
                shift_t = 0;
                shift_amp = hap_in.shift_up ? 0.95f : 0.75f;
            }
        } else {
            seen_shifts = hap_in.shifts;
        }
        const float rum_l = hap_rumble_large * (1.0f / 255.0f), rum_s = hap_rumble_small * (1.0f / 255.0f);
        const bool limiting = fresh && limiter > 1000.0f && rpm >= limiter * 0.985f && thr > 0.3f;
        for (UINT32 f = 0; f < room; ++f) {
            /* smooth the controls a little each sample (about 25 ms) */
            const float k = 0.0009f;
            engine_amp += (target_engine - engine_amp) * k;
            road_amp += (target_road - road_amp) * k;
            rev_smooth += (rpm - rev_smooth) * 0.0008f;
            for (int s = 0; s < 2; ++s) {
                const float up = target_bump[s] > side_bump[s] ? 0.02f : 0.0012f;   /* quick attack, slower release */
                side_bump[s] += (target_bump[s] - side_bump[s]) * up;
                side_squeal[s] += (target_sq[s] - side_squeal[s]) * 0.0015f;
            }
            const float hz = hap_clampf(rev_smooth / 60.0f, 14.0f, 140.0f);
            engine_p1 += hz * dt; engine_p1 -= floorf(engine_p1);
            engine_p2 += 2.0f * hz * dt; engine_p2 -= floorf(engine_p2);
            float eng = sinf(6.2831853f * engine_p1) + 0.55f * sinf(6.2831853f * engine_p2 + 0.7f);
            eng *= engine_amp * (hz < 24.0f ? 0.6f + (hz - 14.0f) * 0.04f : 1.0f);
            if (limiting) {
                limit_p += 24.0f * dt; limit_p -= floorf(limit_p);
                limit_ph += 85.0f * dt; limit_ph -= floorf(limit_ph);
                eng += (limit_p < 0.5f ? 1.0f : 0.0f) * 0.45f * sinf(6.2831853f * limit_ph);
            }
            float thump = 0;
            if (shift_t < 0.2f) {
                const float t = shift_t;
                thump = shift_amp * expf(-t / 0.045f) * sinf(6.2831853f * 58.0f * t);
                if (t > 0.03f) thump += 0.45f * shift_amp * expf(-(t - 0.03f) / 0.03f) * sinf(6.2831853f * 118.0f * (t - 0.03f));
                shift_t += dt;
            }
            /* the game's own rumble: the large motor as a low beat, the small one as a high buzz */
            float game = 0;
            if (rum_l > 0.01f || rum_s > 0.01f) {
                rumble_p1 += 42.0f * dt; rumble_p1 -= floorf(rumble_p1);
                rumble_p2 += 130.0f * dt; rumble_p2 -= floorf(rumble_p2);
                game = rum_l * 0.9f * sinf(6.2831853f * rumble_p1) + rum_s * 0.6f * sinf(6.2831853f * rumble_p2);
            }
            float side[2];
            const float n = hap_noise();
            for (int s = 0; s < 2; ++s) {
                const float rough = hap_low(&road_lp[s], hap_noise(), a_road);
                const float bump = hap_low(&bump_lp[s], n * (0.6f + 0.4f * hap_noise()), a_bump);
                const float hiss = hap_noise() - hap_low(&squeal_lp[s], hap_noise(), a_squeal_hi);
                side[s] = rough * road_amp * 2.2f + bump * side_bump[s] * 3.2f + hiss * side_squeal[s] * 0.8f;
            }
            const float pan_r = 1.0f - 0.45f * a_lat, pan_l = 1.0f + 0.45f * a_lat;   /* the outside of a corner is heavier */
            const float common = eng + thump + game;
            float right = (common + side[0]) * pan_r * hap_gain, left = (common + side[1]) * pan_l * hap_gain;
            right = tanhf(right * 1.1f); left = tanhf(left * 1.1f);
            float *frame = out + (size_t)f * channels;
            for (unsigned c = 0; c < channels; ++c) frame[c] = 0;
            frame[2] = left; frame[3] = right;   /* back left and back right: the actuators */
        }
        (void)brk;
        IAudioRenderClient_ReleaseBuffer(rc, room, 0);
        Sleep(4);
    }
    hap_active = 0;
    IAudioClient_Stop(ac);
    return 0;
}

static void hap_start(void) {
    if (hap_on >= 0) return;
    const char *e = getenv("BB_HAPTICS");
    hap_on = !(e && *e == '0');
    const char *g = getenv("BB_HAPTICS_GAIN");
    if (g && *g) hap_gain = hap_clampf((float)atof(g) / 100.0f, 0.0f, 2.0f);
    if (!hap_on) return;
    HANDLE t = CreateThread(NULL, 0, hap_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* Called with each car read from the telemetry hook. */
static void hap_feed(const uint8_t *state, float speed, float rpm, float limiter, int gear, float throttle, float brake) {
    static int last_gear;
    if (hap_on < 0) hap_start();
    if (hap_on <= 0) return;
    const float *load = (const float *)(state + 0xa50), *lat = (const float *)(state + 0xa60),
                *lon = (const float *)(state + 0xa70), *ratio = (const float *)(state + 0xb30),
                *angle = (const float *)(state + 0xb50);
    for (int i = 0; i < 4; ++i) {
        hap_in.load[i] = load[i]; hap_in.lat[i] = lat[i]; hap_in.lon[i] = lon[i];
        hap_in.ratio[i] = ratio[i]; hap_in.angle[i] = angle[i];
    }
    hap_in.rpm = rpm; hap_in.limiter = limiter; hap_in.speed = speed; hap_in.throttle = throttle; hap_in.brake = brake;
    if (last_gear != 0 && gear != last_gear && gear != 0) { hap_in.shift_up = gear > last_gear && gear > 1; hap_in.shifts++; }
    last_gear = gear;
    hap_in.gear = gear;
    hap_in.us = now_us();
}
