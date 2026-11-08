/* The runtime side of mod scripts. A Lua script of a mod calls into the runtime by opening a path under
 * /app0/dcmod/ with loadfile (the open fails, which the script ignores): the runtime sees every path the game
 * resolves, and acts on these:
 *
 *   dcmod/log/<text>    writes "Mod: <text>" to the log (the game's own print output does not reach the log)
 *   dcmod/ui/text/<id>/<x>/<y>/<size>/<rrggbbaa>/<ttl>/<text>   text on the screen
 *   dcmod/ui/rect/<id>/<x>/<y>/<w>/<h>/<rrggbbaa>/<ttl>          a filled rectangle
 *   dcmod/ui/image/<id>/<x>/<y>/<w>/<h>/<rrggbbaa tint>/<ttl>/<name>   the picture images/<name>.png of a mods folder
 *   dcmod/ui/clear/<id>                                          removes an item
 *       x, y, w, h in thousandths of the screen, size in pixels, ttl in milliseconds; an item with the same id is replaced
 *   dcmod/postfx/use/<name>           the post-processing filter postfx/<name>.spv of a mods folder (use/off: none)
 *   dcmod/postfx/slot<k>/use/<name>   the same for filter slot k (0 to 2; slots run in order, plain use/ is slot 0)
 *   dcmod/postfx/param/<i>/<value>    number i (0 to 15) the filter reads, value in thousandths (negative: n before the digits)
 *   dcmod/env/time/<hour*1000>        sets the hour of the day (the game's clock)
 *   dcmod/env/rain/<name>             sets the race config's precipitation preset (Dry, Light, Mid, Heavy, ...)
 *   dcmod/env/weather/<name>          sets the race config's weather name (Clear, Cloudy, Overcast, Stormy)
 *   dcmod/state/env.lua.bin          a Lua chunk with the hour of the day and the clock's compression
 *   dcmod/store/set/<name>/<value>    keeps a value between runs (dcmod.store); store/del/<name> removes it
 *   dcmod/state/store.lua.bin         a Lua chunk that returns name1, value1, name2, value2, ... of everything kept
 *   dcmod/state/input.lua.bin        a Lua chunk with the keys held and the mouse (only while the game window has the focus)
 *   dcmod/state/car.lua.bin  a Lua chunk that returns the live state of the player's car as numbers (see below)
 *
 * The game lower-cases the paths it opens, so a script writes each capital letter as '^' and the lower-case letter
 * ("^hello" is "Hello"); the log shows the capitals again. A script should replace characters other than letters,
 * digits and " .:=_,-()[]" by '?'. At most 20000 lines are written per run; a line repeated within 5 ms is one call
 * that the file system saw twice, and is written once. */
#include "runtime.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

void bbgpu_overlay_mod_item(const char *id, int kind, float x, float y, float w, float h, float size, unsigned color, int ttl_ms,
                            const char *text);

void bbgpu_postfx_use(int slot, const char *name);
void bbgpu_postfx_param(int slot, int index, float value);
void bbgpu_postfx_scale(int slot, float value);
int bbgpu_camera(float *out);
void bbgpu_sun_scale(float r, float g, float b);
void bbgpu_light_poke(int slot, int dword, float mul, float add);

#define MOD_PREFIX "/app0/dcmod/"
#define MAX_LINES 20000

/* A Lua 5.1 chunk (the game's own build: 4 byte int, 8 byte size_t, 8 byte numbers) that is `return n1, n2, ...`: one
 * LOADK per number and a RETURN. The script loads it with loadfile and calls it; the file is written again at each
 * open, so it always holds the values of that moment. Returns the length. */
static size_t values_chunk(uint8_t *buf, size_t cap, const double *v, const char *const *text, int n) {
    size_t at = 0;
#define PUT(p, len) do { if (at + (len) > cap) return 0; memcpy(buf + at, (p), (len)); at += (len); } while (0)
#define PUT32(x) do { const uint32_t t_ = (uint32_t)(x); PUT(&t_, 4); } while (0)
    static const uint8_t header[12] = {0x1b, 'L', 'u', 'a', 0x51, 0, 1, 4, 8, 4, 8, 0};
    PUT(header, 12);
    static const char name[] = "@dcmod/state";   /* the game's own scripts have a source name and line numbers: so does this one */
    const uint64_t name_size = sizeof(name);
    PUT(&name_size, 8);
    PUT(name, sizeof(name));
    PUT32(0);                    /* line defined */
    PUT32(0);                    /* last line defined */
    const uint8_t function_info[4] = {0, 0, 2, (uint8_t)(n + 1)};   /* upvalues, parameters, vararg main, stack slots */
    PUT(function_info, 4);
    PUT32(n + 1);                /* instructions */
    for (int i = 0; i < n; ++i) PUT32(1u | ((uint32_t)i << 6) | ((uint32_t)i << 14));   /* LOADK R(i) K(i) */
    PUT32(30u | ((uint32_t)(n + 1) << 23));                                               /* RETURN R(0) with n values */
    PUT32(n);                    /* constants */
    for (int i = 0; i < n; ++i) {
        if (text && text[i]) {
            const uint8_t type = 4;  /* string: size_t length with the closing zero, then the bytes */
            const uint64_t length = strlen(text[i]) + 1;
            PUT(&type, 1);
            PUT(&length, 8);
            PUT(text[i], length);
        } else {
            const uint8_t type = 3;  /* number */
            PUT(&type, 1);
            PUT(&v[i], 8);
        }
    }
    PUT32(0);                    /* functions */
    PUT32(n + 1);                /* line info: line 1 for every instruction */
    for (int i = 0; i < n + 1; ++i) PUT32(1);
    PUT32(0);                    /* local variables */
    PUT32(0);                    /* upvalue names */
#undef PUT32
#undef PUT
    return at;
}

static size_t numbers_chunk(uint8_t *buf, size_t cap, const double *v, int n) {
    return values_chunk(buf, cap, v, NULL, n);
}

/* Values a mod keeps between runs (dcmod.store): lines "name<TAB>value" in <user>/dcmod_store.tsv, at most 256 entries of
 * names up to 63 and values up to 255 characters. */
#define STORE_MAX 256
typedef struct { char name[64], value[256]; } StoreEntry;
static int store_load(StoreEntry *entries) {
    const char *user = runtime_file_user_dir();
    char path[1024];
    if (!user || !*user || (size_t)snprintf(path, sizeof path, "%s/dcmod_store.tsv", user) >= sizeof path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = 0;
    char line[400];
    while (n < STORE_MAX && fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *nl = strpbrk(tab + 1, "\r\n");
        if (nl) *nl = 0;
        snprintf(entries[n].name, sizeof entries[n].name, "%s", line);
        snprintf(entries[n].value, sizeof entries[n].value, "%s", tab + 1);
        ++n;
    }
    fclose(f);
    return n;
}
static void store_save(const StoreEntry *entries, int n) {
    const char *user = runtime_file_user_dir();
    char path[1024];
    if (!user || !*user || (size_t)snprintf(path, sizeof path, "%s/dcmod_store.tsv", user) >= sizeof path) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < n; ++i) fprintf(f, "%s\t%s\n", entries[i].name, entries[i].value);
    fclose(f);
}
static void store_set(const char *name, const char *value) {   /* value NULL: remove */
    static StoreEntry entries[STORE_MAX];
    int n = store_load(entries), at = -1;
    for (int i = 0; i < n; ++i) if (!strcmp(entries[i].name, name)) at = i;
    if (!value) {
        if (at < 0) return;
        entries[at] = entries[--n];
    } else {
        if (at < 0) {
            if (n >= STORE_MAX) return;
            at = n++;
        }
        snprintf(entries[at].name, sizeof entries[at].name, "%s", name);
        /* a value never holds a tab or a line break, they are the file's separators */
        snprintf(entries[at].value, sizeof entries[at].value, "%s", value);
        for (char *c = entries[at].value; *c; ++c) if (*c == 0x09 || *c == 0x0d || *c == 0x0a) *c = 0x20;
    }
    store_save(entries, n);
}

/* The text of a call: the game lower-cases paths, so a capital letter is sent as '^' and the lower-case letter, and any
 * other character as '~' and two hex digits ("~2f" is '/', and the bytes of UTF-8 text work the same way). */
static size_t unescape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in; *p && n < cap - 1; ++p) {
        unsigned hex;
        if (*p == '^' && p[1]) {
            out[n++] = (char)toupper((unsigned char)p[1]);
            ++p;
        } else if (*p == '~' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && sscanf(p + 1, "%2x", &hex) == 1) {
            out[n++] = (char)hex;
            p += 2;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = 0;
    return n;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* The keyboard and the mouse for scripts (dcmod/state/input.lua.bin): values 3 the game window has the focus (1/0),
 * 4 and 5 the mouse in the game window, 0..1 from the left and from the top, 6 mouse buttons (1 left, 2 right,
 * 4 middle), 7 and 8 the size of the game window in pixels, 9 the number of keys held, then their Windows virtual key
 * codes. Nothing is reported (0 keys, buttons 0)
 * while another window has the focus, so a script does not read what the player types elsewhere. */
static int input_state(double *v, int cap) {
    /* DC_INPUT_FILE=<file>: a test can set what scripts read, for example "mx 0.1 my 0.3 left 1 w 1902 h 1033 keys 118 65"
     * (the file is read on every call; the window focus is not needed) */
    const char *test = getenv("DC_INPUT_FILE");
    if (test && *test && cap >= 8) {
        FILE *f = fopen(test, "rb");
        if (f) {
            char text[512];
            const size_t got = fread(text, 1, sizeof text - 1, f);
            fclose(f);
            text[got] = 0;
            double mx = 0, my = 0, left = 0, width = 1920, height = 1080;
            int n = 0;
            for (char *tok = strtok(text, " \r\n\t"); tok;) {
                if (!strcmp(tok, "mx")) { tok = strtok(NULL, " \r\n\t"); if (tok) mx = atof(tok); }
                else if (!strcmp(tok, "my")) { tok = strtok(NULL, " \r\n\t"); if (tok) my = atof(tok); }
                else if (!strcmp(tok, "left")) { tok = strtok(NULL, " \r\n\t"); if (tok) left = atof(tok); }
                else if (!strcmp(tok, "w")) { tok = strtok(NULL, " \r\n\t"); if (tok) width = atof(tok); }
                else if (!strcmp(tok, "h")) { tok = strtok(NULL, " \r\n\t"); if (tok) height = atof(tok); }
                else if (!strcmp(tok, "keys")) {
                    while ((tok = strtok(NULL, " \r\n\t")) && 7 + n < cap && atoi(tok) > 0) v[7 + n++] = atoi(tok);
                    continue;
                }
                tok = strtok(NULL, " \r\n\t");
            }
            v[0] = 1; v[1] = mx; v[2] = my; v[3] = left ? 1 : 0; v[4] = width; v[5] = height; v[6] = n;
            return 7 + n;
        }
    }
#ifdef _WIN32
    if (cap < 8) return 0;
    HWND game = GetForegroundWindow();
    DWORD owner = 0;
    if (game) GetWindowThreadProcessId(game, &owner);
    const int focus = game && owner == GetCurrentProcessId();
    v[0] = focus; v[1] = 0; v[2] = 0; v[3] = 0; v[4] = 0; v[5] = 0; v[6] = 0;
    if (!focus) return 7;
    POINT p; RECT r;
    if (GetCursorPos(&p) && ScreenToClient(game, &p) && GetClientRect(game, &r) && r.right > 0 && r.bottom > 0) {
        v[1] = (double)p.x / r.right; v[2] = (double)p.y / r.bottom;
        v[4] = r.right; v[5] = r.bottom;
    }
    v[3] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000 ? 1 : 0) | (GetAsyncKeyState(VK_RBUTTON) & 0x8000 ? 2 : 0) |
           (GetAsyncKeyState(VK_MBUTTON) & 0x8000 ? 4 : 0);
    int n = 0;
    for (int k = 8; k < 255 && 7 + n < cap; ++k) {
        if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
        if (GetAsyncKeyState(k) & 0x8000) v[7 + n++] = k;
    }
    v[6] = n;
    return 7 + n;
#else
    (void)v; (void)cap;
    return 0;
#endif
}

/* Called with every guest path before it is resolved: 0 when the path is not a mod call, 1 when it is a call without a
 * file (the open fails), 2 when the runtime made a file for the script (its host path is written to `out`). */
int runtime_mod_marker(const char *guest, char *out, size_t size) {
    static int lines;
    static char last[512];
    static double last_ms;
    if (!guest || strncasecmp(guest, MOD_PREFIX, sizeof(MOD_PREFIX) - 1)) return 0;
    const char *call = guest + sizeof(MOD_PREFIX) - 1;
    const int is_car = !strcasecmp(call, "state/car.lua.bin"), is_env = !strcasecmp(call, "state/env.lua.bin");
    const int is_input = !strcasecmp(call, "state/input.lua.bin"), is_cam = !strcasecmp(call, "state/camera.lua.bin");
    if (is_car || is_env || is_input || is_cam) {
        /* car: values 1 layout version, 2 car found (1/0), 3 speed m/s, 4 rpm, 5 maximum rpm, 6 rev limiter rpm, 7 gear (-1
         * reverse, 0 neutral), 8 throttle 0..1, 9 brake 0..1, 10 steering -1..1, then four wheels (right front, left front,
         * right rear, left rear) of five numbers: load N, lateral force N, longitudinal force N, slip ratio, slip angle
         * env: values 1 layout version, 2 clock running (1/0), 3 hour of the day 0..24, 4 the game's time compression */
        double values[48];
        float cam[45];
        int n;
        if (is_cam) {   /* camera: 43 numbers, the view block (12), the transposed block (12), the position (3), the sun (16), the projection scales x and y (2) */
            n = bbgpu_camera(cam) ? 45 : 0;
            for (int i = 0; i < n; ++i) values[2 + i] = cam[i];
        } else
        n = is_car ? runtime_mod_car_state(values + 2, 46) : is_input ? input_state(values + 2, 46) : runtime_mod_env_state(values + 2, 46);
        values[0] = 1;
        values[1] = n > 0;
        if (n < 0) n = 0;
        const char *file_name = is_cam ? "/dcmod_state_cam.luac" : is_car ? "/dcmod_state_car.luac" : is_input ? "/dcmod_state_input.luac" : "/dcmod_state_env.luac";
        const char *user = runtime_file_user_dir();
        uint8_t chunk[1024];
        const size_t length = numbers_chunk(chunk, sizeof(chunk), values, n + 2);
        if (!user || !*user || !length || (size_t)snprintf(out, size, "%s%s", user, file_name) >= size) return 1;
        FILE *f = fopen(out, "wb");
        if (!f) return 1;
        const size_t written = fwrite(chunk, 1, length, f);
        fclose(f);
        return written == length ? 2 : 1;
    }
    if (!strcasecmp(call, "state/store.lua.bin")) {
        /* a chunk that returns name1, value1, name2, value2, ... (all text) */
        static StoreEntry entries[STORE_MAX];
        static const char *text[STORE_MAX * 2];
        static double unused[STORE_MAX * 2];
        static uint8_t chunk[STORE_MAX * 330];
        const int n = store_load(entries);
        for (int i = 0; i < n; ++i) { text[i * 2] = entries[i].name; text[i * 2 + 1] = entries[i].value; }
        const size_t length = values_chunk(chunk, sizeof chunk, unused, text, n * 2);
        const char *user = runtime_file_user_dir();
        if (!user || !*user || !length || (size_t)snprintf(out, size, "%s/dcmod_state_store.luac", user) >= size) return 1;
        FILE *f = fopen(out, "wb");
        if (!f) return 1;
        const size_t written = fwrite(chunk, 1, length, f);
        fclose(f);
        return written == length ? 2 : 1;
    }
    if (!strncasecmp(call, "store/", 6)) {
        char text[512], name[64];
        unescape(call + 6, text, sizeof text);
        int skip = 0;
        if (sscanf(text, "set/%63[^/]/%n", name, &skip) == 1 && skip > 0) store_set(name, text + skip);
        else if (sscanf(text, "del/%63[^/]", name) == 1) store_set(name, NULL);
        return 1;
    }
    if (!strncasecmp(call, "env/rain/", 9) || !strncasecmp(call, "env/weather/", 12)) {
        const int weather = call[4] == 'w' || call[4] == 'W';
        char name[64];
        unescape(call + (weather ? 12 : 9), name, sizeof(name));
        runtime_wish_race_config(weather, name);   /* kept: applied again whenever a race loads */
        return 1;
    }
    if (!strncasecmp(call, "env/time/", 9)) {
        const long thousandths = atol(call + 9);   /* hour of the day times 1000 */
        runtime_mod_set_hours((float)thousandths / 1000.0f);
        return 1;
    }
    if (!strncasecmp(call, "ui/", 3)) {
        char text[512];
        unescape(call + 3, text, sizeof(text));
        char id[64];
        int x, y, w, h, size, ttl, consumed = 0;
        unsigned rgba;
        if (sscanf(text, "clear/%63[^/]", id) == 1) {
            bbgpu_overlay_mod_item(id, -1, 0, 0, 0, 0, 0, 0, 0, "");
        } else if (sscanf(text, "text/%63[^/]/%d/%d/%d/%8x/%d/%n", id, &x, &y, &size, &rgba, &ttl, &consumed) == 6 && consumed > 0) {
            const unsigned abgr = (rgba >> 24 & 0xff) | ((rgba >> 16 & 0xff) << 8) | ((rgba >> 8 & 0xff) << 16) | ((rgba & 0xff) << 24);
            bbgpu_overlay_mod_item(id, 0, x / 1000.0f, y / 1000.0f, 0, 0, (float)size, abgr, ttl, text + consumed);
        } else if (sscanf(text, "image/%63[^/]/%d/%d/%d/%d/%8x/%d/%n", id, &x, &y, &w, &h, &rgba, &ttl, &consumed) == 7 && consumed > 0) {
            const unsigned abgr = (rgba >> 24 & 0xff) | ((rgba >> 16 & 0xff) << 8) | ((rgba >> 8 & 0xff) << 16) | ((rgba & 0xff) << 24);
            bbgpu_overlay_mod_item(id, 2, x / 1000.0f, y / 1000.0f, w / 1000.0f, h / 1000.0f, 0, abgr, ttl, text + consumed);
        } else if (sscanf(text, "rect/%63[^/]/%d/%d/%d/%d/%8x/%d", id, &x, &y, &w, &h, &rgba, &ttl) == 7) {
            const unsigned abgr = (rgba >> 24 & 0xff) | ((rgba >> 16 & 0xff) << 8) | ((rgba >> 8 & 0xff) << 16) | ((rgba & 0xff) << 24);
            bbgpu_overlay_mod_item(id, 1, x / 1000.0f, y / 1000.0f, w / 1000.0f, h / 1000.0f, 0, abgr, ttl, "");
        }
        return 1;
    }
    if (!strncasecmp(call, "postfx/", 7)) {
        char text[256];
        unescape(call + 7, text, sizeof(text));
        char name[96];
        int index, slot = 0, skip = 0;
        long thousandths;
        const char *rest = text;
        if (sscanf(text, "slot%d/%n", &slot, &skip) == 1 && skip > 0) rest = text + skip;   /* slot<k>/use/..., slot<k>/param/... */
        if (sscanf(rest, "use/%95[^/]", name) == 1) {
            bbgpu_postfx_use(slot, strcmp(name, "off") ? name : "");
        } else if (sscanf(rest, "scale/%ld", &thousandths) == 1) {
            bbgpu_postfx_scale(slot, (float)thousandths / 1000.0f);
        } else if (sscanf(rest, "param/%d/%ld", &index, &thousandths) == 2) {
            bbgpu_postfx_param(slot, index, (float)thousandths / 1000.0f);
        } else if (sscanf(rest, "param/%d/n%ld", &index, &thousandths) == 2) {
            bbgpu_postfx_param(slot, index, -(float)thousandths / 1000.0f);
        }
        return 1;
    }
    if (!strncasecmp(call, "light/", 6)) {   /* light/sun/<r>/<g>/<b>: multipliers of the sun's colour in thousandths (1000 = the game's own) */
        long r, g, b;
        int slot, dword;
        long mul, add;   /* light/poke/<slot>/<dword>/<mul>/<add>: number <dword> of the lighting becomes value * mul + add (thousandths) */
        if (sscanf(call + 6, "sun/%ld/%ld/%ld", &r, &g, &b) == 3) bbgpu_sun_scale(r / 1000.0f, g / 1000.0f, b / 1000.0f);
        else if (sscanf(call + 6, "poke/%d/%d/%ld/%ld", &slot, &dword, &mul, &add) == 4) bbgpu_light_poke(slot, dword, mul / 1000.0f, add / 1000.0f);
        else if (sscanf(call + 6, "poke/%d/off", &slot) == 1) bbgpu_light_poke(slot, -1, 1.0f, 0.0f);
        return 1;
    }
    if (!strncasecmp(call, "log/", 4)) {
        char text[512];
        unescape(call + 4, text, sizeof(text));
        const double t = now_ms();
        if (!strcmp(text, last) && t - last_ms < 5.0) return 1;
        snprintf(last, sizeof(last), "%s", text);
        last_ms = t;
        const int count = __atomic_add_fetch(&lines, 1, __ATOMIC_RELAXED);
        if (count <= MAX_LINES) {
            printf("Mod: %.400s\n", text);
            fflush(stdout);
        } else if (count == MAX_LINES + 1) {
            printf("Mod: the log call limit was reached, later messages are dropped\n");
            fflush(stdout);
        }
    }
    return 1;
}
