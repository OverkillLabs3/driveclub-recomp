/* The runtime side of mod scripts. A Lua script of a mod calls into the runtime by opening a path under
 * /app0/dcmod/ with loadfile (the open fails, which the script ignores): the runtime sees every path the game
 * resolves, and acts on these:
 *
 *   dcmod/log/<text>    writes "Mod: <text>" to the log (the game's own print output does not reach the log)
 *   dcmod/ui/text/<id>/<x>/<y>/<size>/<rrggbbaa>/<ttl>/<text>   text on the screen
 *   dcmod/ui/rect/<id>/<x>/<y>/<w>/<h>/<rrggbbaa>/<ttl>          a filled rectangle
 *   dcmod/ui/clear/<id>                                          removes an item
 *       x, y, w, h in thousandths of the screen, size in pixels, ttl in milliseconds; an item with the same id is replaced
 *   dcmod/state/car.lua.bin  a Lua chunk that returns the live state of the player's car as numbers (see below)
 *
 * The game lower-cases the paths it opens, so a script writes each capital letter as '^' and the lower-case letter
 * ("^hello" is "Hello"); the log shows the capitals again. A script should replace characters other than letters,
 * digits and " .:=_,-()[]" by '?'. At most 20000 lines are written per run; a line repeated within 5 ms is one call
 * that the file system saw twice, and is written once. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

void bbgpu_overlay_mod_item(const char *id, int kind, float x, float y, float w, float h, float size, unsigned color, int ttl_ms,
                            const char *text);

#define MOD_PREFIX "/app0/dcmod/"
#define MAX_LINES 20000

/* A Lua 5.1 chunk (the game's own build: 4 byte int, 8 byte size_t, 8 byte numbers) that is `return n1, n2, ...`: one
 * LOADK per number and a RETURN. The script loads it with loadfile and calls it; the file is written again at each
 * open, so it always holds the values of that moment. Returns the length. */
static size_t numbers_chunk(uint8_t *buf, size_t cap, const double *v, int n) {
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
        const uint8_t type = 3;  /* number */
        PUT(&type, 1);
        PUT(&v[i], 8);
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

/* Called with every guest path before it is resolved: 0 when the path is not a mod call, 1 when it is a call without a
 * file (the open fails), 2 when the runtime made a file for the script (its host path is written to `out`). */
int runtime_mod_marker(const char *guest, char *out, size_t size) {
    static int lines;
    static char last[512];
    static double last_ms;
    if (!guest || strncasecmp(guest, MOD_PREFIX, sizeof(MOD_PREFIX) - 1)) return 0;
    const char *call = guest + sizeof(MOD_PREFIX) - 1;
    if (!strcasecmp(call, "state/car.lua.bin")) {
        /* values: 1 layout version, 2 car found (1/0), 3 speed m/s, 4 rpm, 5 maximum rpm, 6 rev limiter rpm, 7 gear (-1
         * reverse, 0 neutral), 8 throttle 0..1, 9 brake 0..1, 10 steering -1..1, then four wheels (right front, left front,
         * right rear, left rear) of five numbers: load N, lateral force N, longitudinal force N, slip ratio, slip angle */
        double values[48];
        int n = runtime_mod_car_state(values + 2, 46);
        values[0] = 1;
        values[1] = n > 0;
        if (n < 0) n = 0;
        static const char *dir_name = "/dcmod_state_car.luac";
        const char *user = runtime_file_user_dir();
        uint8_t chunk[1024];
        const size_t length = numbers_chunk(chunk, sizeof(chunk), values, n + 2);
        if (!user || !*user || !length || (size_t)snprintf(out, size, "%s%s", user, dir_name) >= size) return 1;
        FILE *f = fopen(out, "wb");
        if (!f) return 1;
        const size_t written = fwrite(chunk, 1, length, f);
        fclose(f);
        return written == length ? 2 : 1;
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
        } else if (sscanf(text, "rect/%63[^/]/%d/%d/%d/%d/%8x/%d", id, &x, &y, &w, &h, &rgba, &ttl) == 7) {
            const unsigned abgr = (rgba >> 24 & 0xff) | ((rgba >> 16 & 0xff) << 8) | ((rgba >> 8 & 0xff) << 16) | ((rgba & 0xff) << 24);
            bbgpu_overlay_mod_item(id, 1, x / 1000.0f, y / 1000.0f, w / 1000.0f, h / 1000.0f, 0, abgr, ttl, "");
        }
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
