/* Glyph-level fallback for the HLE libSceFont.
 *
 * The game's fonts_*.ctl lists a Latin font first and a CJK font as a later sub-font. The port
 * hands each sub-font to the game as a separate opaque handle, so the game's own fallback only
 * works when sceFontGetCharGlyphMetrics reports a missing glyph. Rather than depend on that
 * (unverified) error-code contract, resolve a missing glyph against a shared CJK face here:
 * whichever handle the game asks, the character still gets drawn.
 *
 * Included by src/runtime_font.c and tools/font_cjk_fallback_test.c. The includer must provide
 * stbtt_fontinfo (imstb_truetype.h) and runtime_file_translate(). Define BB_FONT_FALLBACK_TEST
 * to expose the test-only install/reset helpers.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int runtime_file_translate(const char *guest, char *out, size_t size);

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    size_t size;
    int ready;
    int tried;
} BBFontFallback;

typedef struct {
    const stbtt_fontinfo *info; /* face the glyph actually came from */
    int glyph;
} BBGlyphChoice;

static BBFontFallback bb_fallback;
static pthread_mutex_t bb_fallback_lock = PTHREAD_MUTEX_INITIALIZER;

/* Ordered by CJK coverage: the simplified-Chinese faces first, then the generic Asian ones.
 * These live in the game's newui/fonts folder (loose, a mods folder, or a user-supplied path). */
#ifndef BB_FONT_FALLBACK_GUESTS
#define BB_FONT_FALLBACK_GUESTS \
    "/app0/newui/fonts/tradchinese-dfheistdw7-gb.ttf", \
    "/app0/newui/fonts/dfheistdw7-webfont.ttf", \
    "/app0/newui/fonts/tradchinese-dfheistdw9-gb.ttf", \
    "/app0/newui/fonts/asia-kgde-webfont.ttf", \
    "/app0/newui/fonts/sce-shingo-medium.ttf"
#endif

static unsigned char *bb_read_font_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || n > 64L * 1024L * 1024L) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *data = (unsigned char *)malloc((size_t)n);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(data); return NULL; }
    *out_size = (size_t)n;
    return data;
}

static int bb_fallback_open(const char *host_path, BBFontFallback *fb) {
    size_t size = 0;
    unsigned char *data = bb_read_font_file(host_path, &size);
    if (!data) return 0;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&fb->info, data, offset)) { free(data); return 0; }
    fb->data = data;
    fb->size = size;
    fb->ready = 1;
    return 1;
}

/* Loaded once, on the first missing glyph. Never freed for the life of the process: font handles
 * keep pointers into the face, and the game closes fonts independently of this shared copy. */
static void bb_fallback_load(BBFontFallback *fb) {
    static const char *guests[] = { BB_FONT_FALLBACK_GUESTS };
    const char *override = getenv("BB_FONT_CJK_FALLBACK");
    if (override && override[0]) {
        if (!strcmp(override, "0") || !strcmp(override, "off")) return;
        char host[1024];
        if (!strncmp(override, "/app0/", 6)) {
            if (runtime_file_translate(override, host, sizeof(host)) == 0 &&
                bb_fallback_open(host, fb)) {
                printf("Runtime: CJK fallback font: %s\n", override);
                return;
            }
        } else if (bb_fallback_open(override, fb)) {
            printf("Runtime: CJK fallback font: %s\n", override);
            return;
        }
        printf("Runtime: BB_FONT_CJK_FALLBACK could not be opened: %s\n", override);
        return;
    }
    for (size_t i = 0; i < sizeof(guests) / sizeof(*guests); ++i) {
        char host[1024];
        if (runtime_file_translate(guests[i], host, sizeof(host)) == 0 &&
            bb_fallback_open(host, fb)) {
            printf("Runtime: CJK fallback font: %s\n", guests[i]);
            return;
        }
    }
    /* A loose/mods folder beside the runtime still works when the guest-side miss cache has
     * already marked /app0/newui/fonts as absent (it short-circuits translate()). */
    static const char *hosts[] = {
        "mods/newui/fonts/tradchinese-dfheistdw7-gb.ttf",
        "mods/newui/fonts/dfheistdw7-webfont.ttf",
        "newui/fonts/tradchinese-dfheistdw7-gb.ttf",
    };
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); ++i) {
        if (bb_fallback_open(hosts[i], fb)) {
            printf("Runtime: CJK fallback font: %s\n", hosts[i]);
            return;
        }
    }
    printf("Runtime: no CJK fallback font found (set BB_FONT_CJK_FALLBACK to a CJK .ttf)\n");
}

static const BBFontFallback *bb_fallback_get(void) {
    /* CJK text asks for thousands of glyphs per frame: stay off the mutex once loaded. */
    if (__atomic_load_n(&bb_fallback.tried, __ATOMIC_ACQUIRE)) return &bb_fallback;
    pthread_mutex_lock(&bb_fallback_lock);
    if (!bb_fallback.tried) {
        bb_fallback_load(&bb_fallback);
        /* Release only after the face is ready, so a fast-path reader sees the data. */
        __atomic_store_n(&bb_fallback.tried, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&bb_fallback_lock);
    return &bb_fallback;
}

static float bb_scale(const stbtt_fontinfo *info, float pixels) {
    return pixels > 0.f ? stbtt_ScaleForMappingEmToPixels(info, pixels) : 0.f;
}

static BBGlyphChoice bb_glyph_choose(const stbtt_fontinfo *primary, uint32_t code) {
    BBGlyphChoice c;
    c.info = primary;
    c.glyph = stbtt_FindGlyphIndex(primary, (int)code);
    if (c.glyph) return c;
    const BBFontFallback *fb = bb_fallback_get();
    if (fb->ready) {
        int glyph = stbtt_FindGlyphIndex(&fb->info, (int)code);
        if (glyph) { c.info = &fb->info; c.glyph = glyph; }
    }
    return c;
}

#ifdef BB_FONT_FALLBACK_TEST
static int bb_fallback_install(const unsigned char *source, size_t size) {
    unsigned char *data = (unsigned char *)malloc(size);
    if (!data) return 0;
    memcpy(data, source, size);
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&bb_fallback.info, data, offset)) { free(data); return 0; }
    free(bb_fallback.data);
    bb_fallback.data = data;
    bb_fallback.size = size;
    bb_fallback.ready = 1;
    bb_fallback.tried = 1;
    return 1;
}
static void bb_fallback_reset(void) {
    free(bb_fallback.data);
    memset(&bb_fallback, 0, sizeof(bb_fallback));
}
#endif
