/* Regression test for src/runtime_font_fallback.inc. No game data is needed.
 *
 *   cc -O2 -pthread tools/font_cjk_fallback_test.c -lm -o out/font_cjk_fallback_test
 *   out/font_cjk_fallback_test
 *
 * Optional: BB_TEST_CJK_FONT=<real CJK .ttf> additionally checks Chinese glyph coverage.
 * Run from the repository root (the bundled fonts are referenced by relative path).
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../gpu/third_party/imgui/imstb_truetype.h"

#ifdef _WIN32
#define BB_SETENV(k, v) _putenv_s(k, v)
#else
#define BB_SETENV(k, v) setenv(k, v, 1)
#endif

#define BB_FONT_FALLBACK_TEST 1
static int translate_calls;
static const char *translate_target;
int runtime_file_translate(const char *guest, char *out, size_t size) {
    ++translate_calls;
    if (size) snprintf(out, size, "%s", translate_target ? translate_target : guest);
    return 0;
}
#include "../src/runtime_font_fallback.inc"

static void require(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static unsigned char *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", path); exit(1); }
    require(fseek(f, 0, SEEK_END) == 0, "seek");
    long n = ftell(f);
    require(n > 0, "empty font");
    rewind(f);
    unsigned char *data = (unsigned char *)malloc((size_t)n);
    require(data != NULL, "out of memory");
    require(fread(data, 1, (size_t)n, f) == (size_t)n, "short read");
    fclose(f);
    *size = (size_t)n;
    return data;
}

static int glyph_ink(const stbtt_fontinfo *info, int glyph, float pixels) {
    if (!glyph) return 0;
    float s = bb_scale(info, pixels);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(info, glyph, s, s, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return 0;
    unsigned char *bitmap = (unsigned char *)calloc((size_t)w * (size_t)h, 1);
    require(bitmap != NULL, "bitmap alloc");
    stbtt_MakeGlyphBitmap(info, bitmap, w, h, w, s, s, glyph);
    int ink = 0;
    for (int i = 0; i < w * h; ++i) if (bitmap[i]) ++ink;
    free(bitmap);
    return ink;
}

int main(void) {
    size_t primary_size = 0, fallback_size = 0, cjk_size = 0;
    unsigned char *primary_data = read_all("launcher/assets/fonts/001futura-webfont.ttf", &primary_size);
    unsigned char *fallback_data = read_all("gpu/third_party/fonts/DejaVuSans.ttf", &fallback_size);
    stbtt_fontinfo primary;
    require(stbtt_InitFont(&primary, primary_data, stbtt_GetFontOffsetForIndex(primary_data, 0)),
            "init primary face");
    require(bb_fallback_install(fallback_data, fallback_size), "install fallback face");

    /* A glyph the primary has must never be taken over by the fallback. */
    BBGlyphChoice ascii = bb_glyph_choose(&primary, 0x41);
    require(ascii.info == &primary && ascii.glyph != 0, "ASCII must stay on the primary face");
    require(glyph_ink(ascii.info, ascii.glyph, 24.f) > 0, "ASCII glyph must have ink");

    /* Find a codepoint the primary lacks but the fallback covers: this is the CJK case in
     * miniature (the bundled repo fonts are Latin-only, DejaVuSans covers much more). */
    uint32_t probe = 0;
    for (uint32_t cp = 0x80; cp < 0x3000 && !probe; ++cp) {
        if (!stbtt_FindGlyphIndex(&primary, (int)cp) &&
            stbtt_FindGlyphIndex(&bb_fallback.info, (int)cp)) probe = cp;
    }
    require(probe != 0, "need a codepoint missing from the primary but present in the fallback");
    BBGlyphChoice chosen = bb_glyph_choose(&primary, probe);
    require(chosen.info == &bb_fallback.info, "missing glyph must resolve to the fallback face");
    int ink = glyph_ink(chosen.info, chosen.glyph, 24.f);
    require(ink > 0, "fallback glyph must have ink");
    require(bb_scale(chosen.info, 24.f) == stbtt_ScaleForMappingEmToPixels(chosen.info, 24.f),
            "scale must be computed from the chosen face");

    /* Both bundled faces lack CJK: the missing glyph must stay missing, not crash. */
    BBGlyphChoice missing = bb_glyph_choose(&primary, 0x4e2d);
    require(missing.glyph == 0 && missing.info == &primary, "absent glyph must stay absent");

    unsigned cjk_ok = 0;
    const char *cjk_path = getenv("BB_TEST_CJK_FONT");
    if (cjk_path && *cjk_path) {
        unsigned char *cjk_data = read_all(cjk_path, &cjk_size);
        require(bb_fallback_install(cjk_data, cjk_size), "install real CJK fallback");
        static const uint32_t han[] = {0x4e2d, 0x6587, 0x9a7e, 0x9a76, 0x4ff1,
                                       0x4e50, 0x90e8, 0x8d5b, 0x8bbe, 0x7f6e};
        for (unsigned i = 0; i < sizeof(han) / sizeof(*han); ++i) {
            BBGlyphChoice c = bb_glyph_choose(&primary, han[i]);
            require(c.info == &bb_fallback.info && c.glyph != 0, "CJK glyph must resolve to fallback");
            require(glyph_ink(c.info, c.glyph, 24.f) > 0, "CJK glyph must have ink");
            ++cjk_ok;
        }
    }

    /* BB_FONT_CJK_FALLBACK=0 opts out without touching any game path. */
    bb_fallback_reset();
    translate_calls = 0;
    BB_SETENV("BB_FONT_CJK_FALLBACK", "0");
    require(!bb_fallback_get()->ready, "opt-out must disable the fallback");
    (void)bb_glyph_choose(&primary, probe);
    require(translate_calls == 0, "opt-out must not resolve game paths");

    /* An explicit host path is loaded from disk. */
    bb_fallback_reset();
    BB_SETENV("BB_FONT_CJK_FALLBACK", "gpu/third_party/fonts/DejaVuSans.ttf");
    require(bb_fallback_get()->ready, "host path must load the fallback");
    require(bb_glyph_choose(&primary, probe).glyph != 0, "host-path fallback must resolve glyphs");

    /* Default discovery goes through the game's /app0 path translator. */
    bb_fallback_reset();
    BB_SETENV("BB_FONT_CJK_FALLBACK", "");
    translate_target = "gpu/third_party/fonts/DejaVuSans.ttf";
    translate_calls = 0;
    require(bb_fallback_get()->ready, "guest path via translate must load the fallback");
    require(translate_calls > 0, "default discovery must consult the game path translator");
    translate_target = NULL;

    bb_fallback_reset();
    free(primary_data);
    free(fallback_data);
    printf("PASS: ASCII kept on primary; U+%04X fell back (ink=%d); U+4E2D absent without a CJK face; "
           "opt-out and host-path loading OK; real CJK codes rendered: %u\n",
           probe, ink, cjk_ok);
    return 0;
}
