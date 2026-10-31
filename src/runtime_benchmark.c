/* Benchmark mode (BB_BENCHMARK, off by default): boot straight into an event, let the game's AI drive the
 * player's car for a fixed time, write a frame-time summary to the log and exit.
 *
 * The game ships its own Lua autotest framework (scripts/lua/testing, scripts/autotest/lua) and starts a test
 * script given on its command line as -luaautotest=<path>. Here the runtime writes a small test script of its
 * own (the template below, nothing taken from the game) to <user>/benchmark, serves that folder like a mods
 * folder, and gives the game the -luaautotest argument. The script skips the intro, starts the event with the
 * game's Automation.Go, skips the pre-race cameras, hands the car to the AI and then opens the path
 * "bb_benchmark/racing", which this file sees in runtime_benchmark_marker (the file does not exist; the open
 * fails and the game carries on). From then on each flip interval is kept; after the warm-up and the run time
 * the summary is printed and the process exits.
 *
 * The launcher's Benchmark card sets all of this for the player; DriveClub.exe --play passes it on as well:
 *   BB_BENCHMARK=1                       all defaults
 *   BB_BENCHMARK=track=chile_race_track_v1,car=fe_f12,tod=18,seconds=60
 * key=value pairs, separated by ',' (values: letters, digits and '_' only; they go into Lua source):
 *   track    world name, "Track Name" in newdata/tracks.csv (default canada_road_circuit_02_n, Fraser Valley).
 *            The launcher lists the circuits of the base game by name (kBenchTracks in
 *            launcher/driveclub_launcher.cpp). Run to a result: canada_, chile_, india_, norway_, uk_ and
 *            japan_road_circuit_02_n; the same with _01_n except uk_; canada_, india_ and japan_dlc_road_circuit_03_n;
 *            canada_, chile_, uk_race_track_v1; norway_race_track_01_v1. From game data, not run: uk_road_circuit_01_n,
 *            norway_dlc_road_circuit_03_n, india_race_track_v1, japan_race_track_01_v1, the reverse routes (_r for
 *            _n), the other race track layouts (v2, v3) and the point-to-point routes (_road_point_), which can end
 *            before the measurement does.
 *   car      "Vehicle Name" in newdata/vehicles.csv (default fe_458, Ferrari 458 Italia). The launcher lists base
 *            game cars by name (kBenchCars). Run to a result: mc_p1 pa_hu ko_ag he_ve_gt pa_zo fe_458 fe_f12 au_r8
 *            mc_mp_12 as_ma_177 bm_m4 me_gt_am ma_mc_st au_rs5 bm_m2 ren_dez vw_golf_gti mi_jcw. From game data,
 *            not run: me_sl_am au_rs6 lo_ex ren_cl, and the other cars of the list.
 *   mode     TimeTrial (default, the player's car alone) or Race (with the 7-car AI grid of the game's tests;
 *            from the game's test helpers, not run).
 *   tod      hour 0-23 (default 12; 8, 12, 18 and 22 run).   weather  Clear (default, run), Cloudy, Overcast, Stormy.
 *   rain     empty = dry (default); Heavy (run); Light, Mid, VeryLight, Wet (wet road, no rain), Damp, DryToWet, or
 *            any of the game's precipitation presets with '_' for each space (Wet_To_Dry, ...): from game data, not run.
 *   laps     1-99 (default 5).               drive    ai (default) or none (the player drives).
 *   warmup   s after the race starts that are not measured (default 5).
 *   seconds  s measured (default 60).        view     camera: chase, driverhead, attach (empty = game default).
 *   timeout  s from start until the race must run (default 240), else exit code 3.
 * The result line ("Benchmark: result mode=... avg_fps= low1_fps= low01_fps= p99_ms= max_ms= load_s=") goes to the
 * log and to <user>/benchmark/result.txt, or to BB_BENCHMARK_OUT=<file>, followed by a "ran ..." line: the settings
 * the game ran with, read by the game itself (ran_with()). The lows are the average frame rate of
 * the slowest 1% and 0.1% of frames. Exit code 0 = done, 3 = the race did not start or the frames stopped.
 * BB_BENCHMARK_LUA=<host file>: Lua run inside the script (in a pcall) with `stage` set to "frontend", "loaded",
 * "racing" and "tick" (every 5 s while racing); say(text) there goes to the log. A debugging aid: the game's
 * own print output does not reach the log, so script messages go through a marker open (say:<text>). */
#define _GNU_SOURCE
#include "runtime.h"
#include "guest_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define SCRIPT_GUEST "scripts/autotest/lua/bb_benchmark.lua"
#define MARKER_PREFIX "/app0/bb_benchmark/"

static struct {
    char track[64], car[64], mode[16], weather[16], view[24], drive[8], rain[48];
    int tod, laps, warmup, seconds, timeout;
} cfg = {"canada_road_circuit_02_n", "fe_458", "TimeTrial", "Clear", "", "ai", "", 12, 5, 5, 60, 240};

enum { OFF, WAITING, WARMUP, MEASURING, DONE };
static volatile int phase = OFF;
static double t_config, t_race, t_measure, t_last_flip;
static double *intervals;
static size_t interval_count, interval_cap, frames;
static char out_path[700], user_root[600];

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* Values end up in Lua source: letters, digits and '_' only. */
static int set_word(char *dst, size_t size, const char *v, size_t n) {
    if (n == 0 || n >= size) return 0;
    for (size_t i = 0; i < n; ++i) if (!isalnum((unsigned char)v[i]) && v[i] != '_') return 0;
    memcpy(dst, v, n); dst[n] = 0;
    return 1;
}
static int set_int(int *dst, const char *v, size_t n, int lo, int hi) {
    char buffer[16];
    if (n == 0 || n >= sizeof(buffer)) return 0;
    memcpy(buffer, v, n); buffer[n] = 0;
    char *end; long x = strtol(buffer, &end, 10);
    if (*end || x < lo || x > hi) return 0;
    *dst = (int)x;
    return 1;
}
/* The precipitation preset: a short name, or the game's preset name with '_' for each space. The game's names
 * (eboot strings next to the precipitation icons): Light Precipitation, Heavy Precipitation, Mid Precipitation,
 * Very Light Precipitation, Dry To Wet, Wet To Dry, Wet Track, Damp Track, Light Constant, ... */
static int set_rain(const char *v, size_t n) {
    static const char *const alias[][2] = {
        {"Heavy", "Heavy Precipitation"}, {"Mid", "Mid Precipitation"}, {"Light", "Light Precipitation"},
        {"VeryLight", "Very Light Precipitation"}, {"Wet", "Wet Track"}, {"Damp", "Damp Track"},
        {"DryToWet", "Dry To Wet"}};
    if (!set_word(cfg.rain, sizeof(cfg.rain), v, n)) return 0;
    for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); ++i) {
        if (!strcmp(cfg.rain, alias[i][0])) {
            snprintf(cfg.rain, sizeof(cfg.rain), "%s", alias[i][1]);
            return 1;
        }
    }
    for (char *c = cfg.rain; *c; ++c) if (*c == '_') *c = ' ';
    return 1;
}
static char *probe_lua; /* BB_BENCHMARK_LUA contents */

static int parse(const char *s) {
    if (!strcmp(s, "1") || !strcmp(s, "on")) return 1;
    while (*s) {
        while (*s == ',' || *s == ';' || *s == ' ') ++s;
        if (!*s) break;
        const char *eq = strchr(s, '='), *end = s + strcspn(s, ",; ");
        if (!eq || eq > end) { fprintf(stderr, "Benchmark: '%.*s' is not key=value\n", (int)(end - s), s); return 0; }
        size_t k = (size_t)(eq - s), n = (size_t)(end - eq - 1);
        const char *v = eq + 1;
        int ok = 0;
#define KEY(name) (k == sizeof(name) - 1 && !strncmp(s, name, k))
        if (KEY("track")) ok = set_word(cfg.track, sizeof(cfg.track), v, n);
        else if (KEY("car")) ok = set_word(cfg.car, sizeof(cfg.car), v, n);
        else if (KEY("mode")) ok = set_word(cfg.mode, sizeof(cfg.mode), v, n);
        else if (KEY("weather")) ok = set_word(cfg.weather, sizeof(cfg.weather), v, n);
        else if (KEY("rain")) ok = n == 0 ? (cfg.rain[0] = 0, 1) : set_rain(v, n);
        else if (KEY("view")) ok = n == 0 ? (cfg.view[0] = 0, 1) : set_word(cfg.view, sizeof(cfg.view), v, n);
        else if (KEY("drive")) ok = set_word(cfg.drive, sizeof(cfg.drive), v, n) && (!strcmp(cfg.drive, "ai") || !strcmp(cfg.drive, "none"));
        else if (KEY("tod")) ok = set_int(&cfg.tod, v, n, 0, 23);
        else if (KEY("laps")) ok = set_int(&cfg.laps, v, n, 1, 99);
        else if (KEY("warmup")) ok = set_int(&cfg.warmup, v, n, 0, 600);
        else if (KEY("seconds")) ok = set_int(&cfg.seconds, v, n, 1, 3600);
        else if (KEY("timeout")) ok = set_int(&cfg.timeout, v, n, 10, 3600);
#undef KEY
        if (!ok) { fprintf(stderr, "Benchmark: bad setting '%.*s'\n", (int)(end - s), s); return 0; }
        s = end;
    }
    return 1;
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *p = buffer + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') continue;
        char c = *p; *p = 0;
        mkdir(buffer, 0755); /* parents; failures show up in the last mkdir */
        *p = c;
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

/* The test script. It runs as a coroutine of the game's autotest component: the AutoTest:Wait* helpers yield
 * once per frame. Messages go to the game's log with a [BB_BENCHMARK] prefix. */
static const char script_template[] =
    "-- Written by DriveClubGame for BB_BENCHMARK (src/runtime_benchmark.c). Not game data.\n"
    "require(\"class\")\n"
    "require(\"autotesthelpers\")\n"
    "require(\"driveclub_testhelpers\")\n"
    "local track, car, mode, weather, view, rain = \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"\n"
    "local tod, laps, ai = %d, %d, %s\n"
    "local function mark(name) loadfile(\"bb_benchmark/\" .. name) end\n"
    "-- The game's print does not reach the log: messages also go through a marker open the runtime prints.\n"
    "local function say(text)\n"
    "  text = tostring(text)\n"
    "  print(\"[BB_BENCHMARK] \" .. text .. \"\\n\")\n"
    "  mark(\"say:\" .. (string.gsub(string.sub(text, 1, 400), \"[^%%w%%s%%.:=_,%%-%%(%%)%%[%%]]\", \"?\")))\n"
    "end\n"
    "local function probe(stage)\n"
    "  local ok, err = pcall(function()\n"
    "%s\n"
    "  end)\n"
    "  if not ok then say(\"probe error at \" .. stage .. \": \" .. tostring(err)) end\n"
    "end\n"
    "say(\"script started\")\n"
    "mark(\"started\")\n"
    "-- Intro: skip the video, confirm the title page.\n"
    "DriveClubTestHelpers:AddAutoPageResponse(function(page)\n"
    "  say(\"page \" .. tostring(page))\n"
    "  if page == \"boot.introvideo\" then Event.SendEvent(global, \"GUI\", \"SkipVideo\") end\n"
    "  if page == \"boot.start\" then Event.SendEvent(global, \"GUI\", \"BOOT_SEQ_DONE\") end\n"
    "end)\n"
    "AutoTest:WaitForEvent(global, \"IsFrontEnd\")\n"
    "say(\"front end reached\")\n"
    "mark(\"frontend\")\n"
    "probe(\"frontend\")\n"
    "AutoTest:WaitForSeconds(1)\n"
    "say(\"starting \" .. mode .. \" on \" .. track .. \" with \" .. car)\n"
    "-- Precipitation (game values such as Dry, Light, Mid, Heavy): set before and after Go, which may reset it.\n"
    "local function set_rain() if rain ~= \"\" then GameConfig.data.precipitationType = rain end end\n"
    "set_rain()\n"
    "if mode == \"Race\" then\n"
    "  Automation.Go(mode, track, \"sky_mainline_v2\", laps, false, tod, weather, 1, car, 2, false, 7,\n"
    "    \"he_ve_gt\", \"ruf_rgt\", \"mi_jcw\", \"vw_beetle_gsr\", \"fe_f12\", \"lo_ex\", \"ma_mc_st\")\n"
    "else\n"
    "  Automation.Go(mode, track, \"sky_mainline_v2\", laps, false, tod, weather, 1, car, 2, true, 0)\n"
    "end\n"
    "set_rain()\n"
    "if rain ~= \"\" then mark(\"setrain\") end\n"
    "say(\"precipitation \" .. tostring(GameConfig:GetPrecipitationType()))\n"
    "AutoTest:WaitForEvent(global, \"AnimLibPlaySqMsg\", function(data)\n"
    "  return data == \"prerace\" or data == \"prerace_night\"\n"
    "end)\n"
    "say(\"loaded, skipping the pre-race cameras\")\n"
    "-- The engine applies precipitation when these events fire; it reads GameConfig's precipitationType.\n"
    "if rain ~= \"\" then\n"
    "  set_rain()\n"
    "  mark(\"setrain\")\n"
    "  Event.SendEvent(global, \"SetPrecipitationFromGameConfig\")\n"
    "  Event.SendEvent(global, \"SetInitialPrecipitationState\")\n"
    "end\n"
    "mark(\"loaded\")\n"
    "probe(\"loaded\")\n"
    "AutoTest:WaitForSeconds(2)\n"
    "Event.SendEvent(global, \"Advance\")\n"
    "AutoTest:WaitForEvent(global, \"RaceState\", function(data) return data == \"Racing\" end)\n"
    "if ai then DriveClubTestHelpers:SwitchToAI() end\n"
    "if rain ~= \"\" then Event.SendEvent(global, \"StartDrivingPrecipitation\") end\n"
    "if view ~= \"\" then DriveClubTestHelpers:ChangeView(view) end\n"
    "say(\"racing\")\n"
    "mark(\"racing\")\n"
    "probe(\"racing\")\n"
    "while true do\n"
    "  AutoTest:WaitForSeconds(5)\n"
    "  probe(\"tick\")\n"
    "  if ai then Actor.GetPlayer().UnderPlayerControl = false end\n"
    "end\n";

static void *watchdog(void *unused) {
    (void)unused;
    for (;;) {
        sleep(1);
        const double t = now_ms();
        if (phase == WAITING && t - t_config > cfg.timeout * 1000.0) {
            printf("Benchmark: FAILED, the race did not start within %d s (see the 'Benchmark: [lua' lines)\n", cfg.timeout);
            fflush(NULL);
            _exit(3);
        }
        if ((phase == WARMUP || phase == MEASURING) && t_last_flip > 0 && t - t_last_flip > 30000.0) {
            printf("Benchmark: FAILED, no frame for 30 s\n");
            fflush(NULL);
            _exit(3);
        }
    }
    return NULL;
}

void runtime_benchmark_configure(const char *user) {
    const char *s = getenv("BB_BENCHMARK");
    if (!s || !*s || !strcmp(s, "0") || !strcmp(s, "off")) return;
    if (!parse(s)) { fprintf(stderr, "Benchmark: BB_BENCHMARK ignored\n"); return; }
    snprintf(user_root, sizeof(user_root), "%s/benchmark", user);
    char dir[700], path[800];
    snprintf(dir, sizeof(dir), "%s/scripts/autotest/lua", user_root);
    if (!make_dirs(dir)) { fprintf(stderr, "Benchmark: cannot create %s\n", dir); return; }
    snprintf(path, sizeof(path), "%s/bb_benchmark.lua.bin", dir);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Benchmark: cannot write %s\n", path); return; }
    const char *lua = getenv("BB_BENCHMARK_LUA");
    if (lua && *lua) {
        FILE *pf = fopen(lua, "rb");
        if (pf) {
            fseek(pf, 0, SEEK_END);
            const long size = ftell(pf);
            fseek(pf, 0, SEEK_SET);
            probe_lua = size >= 0 ? calloc(1, (size_t)size + 1) : NULL;
            if (probe_lua && fread(probe_lua, 1, (size_t)size, pf) != (size_t)size) probe_lua[0] = 0;
            fclose(pf);
        } else {
            fprintf(stderr, "Benchmark: cannot read BB_BENCHMARK_LUA %s\n", lua);
        }
    }
    fprintf(f, script_template, cfg.track, cfg.car, cfg.mode, cfg.weather, cfg.view, cfg.rain, cfg.tod, cfg.laps,
            strcmp(cfg.drive, "ai") ? "false" : "true", probe_lua ? probe_lua : "");
    fclose(f);
    runtime_file_add_mods(user_root);
    const char *o = getenv("BB_BENCHMARK_OUT");
    snprintf(out_path, sizeof(out_path), "%s", o && *o ? o : "");
    if (!out_path[0]) snprintf(out_path, sizeof(out_path), "%s/result.txt", user_root);
    interval_cap = (size_t)(cfg.seconds + 1) * 1000;
    intervals = malloc(interval_cap * sizeof(*intervals));
    if (!intervals) { fprintf(stderr, "Benchmark: out of memory\n"); return; }
    t_config = now_ms();
    phase = WAITING;
    pthread_t thread;
    pthread_create(&thread, NULL, watchdog, NULL);
    pthread_detach(thread);
    printf("Benchmark: mode=%s track=%s car=%s tod=%d weather=%s rain=%s laps=%d drive=%s view=%s warmup=%d s, "
           "%d s measured%s\n",
           cfg.mode, cfg.track, cfg.car, cfg.tod, cfg.weather, cfg.rain[0] ? cfg.rain : "default", cfg.laps,
           cfg.drive, cfg.view[0] ? cfg.view : "default", cfg.warmup, cfg.seconds, probe_lua ? ", Lua probe" : "");
}

/* The extra command-line argument for the game, or NULL. */
const char *runtime_benchmark_guest_arg(void) {
    return phase == OFF ? NULL : "-luaautotest=" SCRIPT_GUEST;
}

/* Precipitation: the game's race config is C++ data (GameConfig.dataInCPP is true; GameConfig.data in Lua is a
 * copy made from it), so the script cannot set it. The config's precipitation preset is a std::string; the
 * script opens the marker "setrain" right after Automation.Go and again once loaded, and this assigns cfg.rain
 * to it with the game's own string assign, on the game's thread (eboot 1.28 offsets, from Ghidra):
 *   G      = *(image + 0x1911100)
 *   base   = *(*(G + 0x970) + 0xf8);  config = base[0x780] == 0 ? base + 0x3b8 : base + 8   (FUN_0013d550)
 *   config + 0x210 weather name, config + 0x238 precipitation name (FUN_00016e40 reads both)
 *   FUN_00e978b0(string object, const char *) assigns; the object starts 8 bytes before the text
 *   (allocator word, text or heap pointer, size at +0x18, capacity at +0x20). */
typedef void *(ABI *guest_string_assign)(void *object, const char *text);
static const char *guest_string(const unsigned char *object) {
    const unsigned long long cap = *(const unsigned long long *)(object + 0x20);
    return cap > 15 ? *(const char *const *)(object + 8) : (const char *)(object + 8);
}
static void set_guest_rain(void) {
    if (!cfg.rain[0] || !runtime_image_base) return;
    const unsigned char *g = *(unsigned char *const *)(runtime_image_base + 0x1911100);
    const unsigned char *p = g ? *(unsigned char *const *)(g + 0x970) : NULL;
    unsigned char *base = p ? *(unsigned char *const *)(p + 0xf8) : NULL;
    if (!base) { printf("Benchmark: rain: no race config yet\n"); return; }
    unsigned char *config = base[0x780] == 0 ? base + 0x3b8 : base + 8;
    const char *before = guest_string(config + 0x230);
    printf("Benchmark: rain: config weather '%.40s', precipitation '%.40s'", guest_string(config + 0x208), before);
    {
        const uint64_t args[2] = {(uint64_t)(uintptr_t)(config + 0x230), (uint64_t)(uintptr_t)cfg.rain};
        guest_cpu_call(runtime_image_base + 0xe978b0, 2, args); /* guest_string_assign */
    }
    printf(" -> '%.40s'\n", guest_string(config + 0x230));
    fflush(stdout);
}

/* Called for every guest path the file system resolves; returns 1 for the script's progress markers. */
int runtime_benchmark_marker(const char *guest) {
    if (phase == OFF || strncmp(guest, MARKER_PREFIX, sizeof(MARKER_PREFIX) - 1)) return 0;
    const char *name = guest + sizeof(MARKER_PREFIX) - 1;
    const double t = now_ms();
    if (!strncmp(name, "say:", 4)) {
        printf("Benchmark: [lua %.1f s] %s\n", (t - t_config) / 1000.0, name + 4);
        return 1;
    }
    if (!strcmp(name, "setrain")) {
        set_guest_rain();
        return 1;
    }
    printf("Benchmark: %s at %.1f s\n", name, (t - t_config) / 1000.0);
    if (!strcmp(name, "racing") && phase == WAITING) {
        t_race = t;
        t_measure = t + cfg.warmup * 1000.0;
        __atomic_store_n(&phase, WARMUP, __ATOMIC_RELEASE);
    }
    return 1;
}

/* Graphics detail and add-on content as the game was started with them: the launcher passes its edited global.rpk
 * and the DLC unlock as --mods folders (probe.c reports each one here). The graphics folder holds stamp.txt:
 * "graphics-1 <stamps> car N opponents N scenery N reflections N dynres N". */
static int detail_car, detail_opponents, detail_scenery, detail_reflections, detail_dynres = 1, dlc_unlocked;
void runtime_benchmark_note_mods(const char *dir) {
    char path[700], text[256] = "";
    const size_t n = strlen(dir);
    if (n >= 6 && !strcmp(dir + n - 6, "unlock")) dlc_unlocked = 1;
    snprintf(path, sizeof(path), "%s/stamp.txt", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    const size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[got] = 0;
    const char *at = strncmp(text, "graphics-", 9) ? NULL : strstr(text, " car ");
    if (at) sscanf(at, " car %d opponents %d scenery %d reflections %d dynres %d", &detail_car, &detail_opponents,
                   &detail_scenery, &detail_reflections, &detail_dynres);
}

/* A u32 of the running game image (the resolution patches' targets), or 0. */
static unsigned image_u32(uint64_t offset) {
    unsigned v = 0;
    if (runtime_image_base && offset + 4 <= runtime_image_size) memcpy(&v, (const void *)(runtime_image_base + offset), 4);
    return v;
}

/* What the game ran with, read here rather than taken from the launcher's choices: "ran key=value ...". */
static void ran_with(char *out, size_t size) {
    /* The render and output sizes in eboot 1.28: the values the "Internal resolution" and "Output resolution"
     * patches write (patches/DriveClub.xml addresses minus the 0x400000 patch base). */
    const unsigned rw = image_u32(0x12d0544), rh = image_u32(0x12d0554), ow = image_u32(0x754c1b), oh = image_u32(0x754c21);
    char render[32] = "unknown", output[32] = "unknown";
    if (rw >= 320 && rw <= 16384 && rh >= 200 && rh <= 16384) snprintf(render, sizeof(render), "%ux%u", rw, rh);
    if (ow >= 320 && ow <= 16384 && oh >= 200 && oh <= 16384) snprintf(output, sizeof(output), "%ux%u", ow, oh);
    const char *cap = getenv("BB_FPS_CAP"), *limit = getenv("BB_FPS_LIMIT");
    const char *fps = cap && *cap ? cap : limit && *limit ? limit : "none";
#define ENV(name, fallback) (getenv(name) && *getenv(name) ? getenv(name) : (fallback))
    snprintf(out, size,
             "ran render=%s output=%s fps_limit=%s vblank_hz=%s present=%s aa=%s aa_scale=%s frame_gen=%s frame_gen_x=%s "
             "aniso=%s mirror_rate=%s car_detail=%d opponent_detail=%d scenery_detail=%d hq_reflections=%d dynamic_res=%d "
             "dlc=%d fullscreen=%s",
             render, output, fps, ENV("BB_VBLANK_HZ", "60"), ENV("BB_PRESENT_MODE", "default"), ENV("BB_AA", "off"),
             ENV("BB_AA_SCALE", "native"), ENV("BB_FRAME_GEN", "off"), ENV("BB_FRAME_GEN_MULTIPLIER", "2"),
             ENV("BB_ANISO", "auto"), ENV("BB_MIRROR_RATE", "1"), detail_car, detail_opponents, detail_scenery,
             detail_reflections, detail_dynres, dlc_unlocked, ENV("BB_FULLSCREEN", "0"));
#undef ENV
}

static int cmp_desc(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}
/* Average of the slowest share of the frames, as frames per second. */
static double low_fps(const double *sorted_desc, size_t n, double share) {
    size_t k = (size_t)(n * share);
    if (k < 1) k = 1;
    if (k > n) k = n;
    double sum = 0;
    for (size_t i = 0; i < k; ++i) sum += sorted_desc[i];
    return sum > 0 ? 1000.0 * k / sum : 0;
}

static void finish(double t) {
    const double elapsed = t - t_measure;
    const size_t n = interval_count;
    qsort(intervals, n, sizeof(*intervals), cmp_desc);
    const double avg = elapsed > 0 ? 1000.0 * frames / elapsed : 0;
    const double p99 = n ? intervals[n / 100] : 0, worst = n ? intervals[0] : 0;
    char line[600], rain[sizeof(cfg.rain)];
    snprintf(rain, sizeof(rain), "%s", cfg.rain[0] ? cfg.rain : "dry");
    for (char *c = rain; *c; ++c) if (*c == ' ') *c = '_'; /* one word per value */
    snprintf(line, sizeof(line),
             "mode=%s track=%s car=%s tod=%d weather=%s rain=%s drive=%s seconds=%.1f frames=%zu avg_fps=%.2f "
             "low1_fps=%.2f low01_fps=%.2f p99_ms=%.2f max_ms=%.2f load_s=%.1f",
             cfg.mode, cfg.track, cfg.car, cfg.tod, cfg.weather, rain, cfg.drive, elapsed / 1000.0, frames, avg,
             low_fps(intervals, n, 0.01), low_fps(intervals, n, 0.001), p99, worst, (t_race - t_config) / 1000.0);
    char ran[700];
    ran_with(ran, sizeof(ran));
    printf("Benchmark: result %s\n", line);
    printf("Benchmark: %s\n", ran);
    FILE *f = fopen(out_path, "wb");
    if (f) { fprintf(f, "%s\n%s\n", line, ran); fclose(f); }
    else printf("Benchmark: cannot write %s\n", out_path);
    printf("Benchmark: done, exiting\n");
    fflush(NULL);
    _exit(0); /* like a crash stop: GPU, audio and guest threads are still running */
}

/* Called by the video-out driver after each flip. */
void runtime_benchmark_flip(void) {
    const int p = __atomic_load_n(&phase, __ATOMIC_ACQUIRE);
    if (p < WARMUP || p == DONE) return;
    const double t = now_ms();
    if (p == WARMUP) {
        if (t >= t_measure) { t_measure = t; t_last_flip = t; phase = MEASURING; }
        else t_last_flip = t;
        return;
    }
    if (interval_count < interval_cap) intervals[interval_count++] = t - t_last_flip;
    ++frames;
    t_last_flip = t;
    if (t - t_measure >= cfg.seconds * 1000.0) { phase = DONE; finish(t); }
}
