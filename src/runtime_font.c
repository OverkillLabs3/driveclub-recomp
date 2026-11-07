/* libSceFont for TrueType fonts the game supplies from memory (DriveClub's UI fonts).
 * Glyph metrics and 8-bit coverage come from stb_truetype; the surface semantics follow what the PS4
 * library does: coverage is stored (not blended), the pen position (x, y) is the baseline origin,
 * and metrics are in pixels with bearingY pointing up. System font sets are not provided. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "../gpu/third_party/imgui/imstb_truetype.h"
#pragma clang diagnostic pop
#include "runtime_font_fallback.inc"

#define FONT_OK 0
#define FONT_ERR_PARAM ((int32_t)0x80460002)
#define FONT_ERR_HANDLE ((int32_t)0x80460001)
#define FONT_ERR_FATAL ((int32_t)0x80460009)

typedef struct { uint32_t reserved[64]; } GuestFont; /* 256 bytes: the game treats the handle as opaque */
typedef struct {
    GuestFont guest;
    stbtt_fontinfo info;
    float sx, sy;      /* SetScalePixel */
    float rx, ry;      /* SetupRenderScalePixel */
    int valid;
} Font;
typedef struct { float width, height, hbx, hby, hadv, vbx, vby, vadv; } GlyphMetrics;
typedef struct { uint8_t *address; uint32_t widthByte; uint8_t pixelSizeByte, pixelFormat; uint16_t pad; } SurfaceImage;
typedef struct { float bearingX, bearingY, advance, stride; uint32_t width, height; } ImageMetrics;
typedef struct { const void *stage; SurfaceImage surface; struct { uint32_t x, y, w, h; } update; ImageMetrics image; } RenderOutput;
typedef struct {
    void *buffer; int32_t widthByte; int8_t pixelSizeByte; uint8_t pad0, styleFlag, pad2;
    int32_t width, height; uint32_t sc_x0, sc_y0, sc_x1, sc_y1; uint64_t reserved[11];
} Surface;
typedef struct { float baselineOffset, lineAdvance, decorationExtent; } HorizontalLayout;
typedef struct { float offsetX, offsetY, positionX, positionY; } Kerning;

static Font *font_of(void *h) { Font *f=h; return f && f->valid ? f : NULL; }
static float scale_for(const Font *f,float pixels) { return bb_scale(&f->info,pixels); }

static ABI int32_t font_mem_init(void *mem,void *region,uint32_t size,const void *iface,void *mspace,void *destroy,void *ctx) {
    (void)region;(void)size;(void)iface;(void)mspace;(void)destroy;(void)ctx;
    if (mem) memset(mem,0,32);
    return FONT_OK;
}
static ABI int32_t font_create_library(const void *mem,void *params,uint64_t edition,void **out) {
    (void)mem;(void)params;(void)edition;
    if (!out) return FONT_ERR_PARAM;
    *out=calloc(1,256);
    return *out ? FONT_OK : FONT_ERR_FATAL;
}
static ABI int32_t font_destroy_library(void **lib) { if (!lib) return FONT_ERR_PARAM; free(*lib); *lib=NULL; return FONT_OK; }
static ABI int32_t font_support_system(void *lib) { (void)lib; return FONT_OK; }
static ABI int32_t font_support_external(void *lib,uint32_t max,uint32_t formats) { (void)lib;(void)max;(void)formats; return FONT_OK; }
static ABI int32_t font_attach_cache(void *lib,void *buffer,uint32_t size) { (void)lib;(void)buffer;(void)size; return FONT_OK; }
static ABI int32_t font_select_library_ft(int32_t version) { (void)version; return FONT_OK; }
static ABI void *font_select_renderer_ft(int32_t version) { (void)version; return NULL; }
static ABI void font_surface_init(Surface *s,void *buffer,int32_t widthByte,int32_t pixelSize,int32_t w,int32_t h) {
    if (!s) return;
    memset(s,0,sizeof(*s));
    s->buffer=buffer; s->widthByte=widthByte; s->pixelSizeByte=(int8_t)pixelSize; s->width=w; s->height=h;
    s->sc_x0=0; s->sc_y0=0; s->sc_x1=(uint32_t)w; s->sc_y1=(uint32_t)h;
}
static ABI int32_t font_create_renderer(const void *mem,void *params,uint64_t edition,void **out) {
    (void)mem;(void)params;(void)edition;
    if (!out) return FONT_ERR_PARAM;
    *out=calloc(1,256);
    return *out ? FONT_OK : FONT_ERR_FATAL;
}
static ABI int32_t font_destroy_renderer(void **r) { if (!r) return FONT_ERR_PARAM; free(*r); *r=NULL; return FONT_OK; }
static ABI int32_t font_open_memory(void *lib,const void *data,uint32_t size,const void *params,void **out) {
    (void)lib;(void)size;(void)params;
    if (!out || !data) return FONT_ERR_PARAM;
    Font *f=calloc(1,sizeof(*f));
    if (!f) return FONT_ERR_FATAL;
    int offset=stbtt_GetFontOffsetForIndex((const unsigned char *)data,0);
    if (offset<0 || !stbtt_InitFont(&f->info,(const unsigned char *)data,offset)) { free(f); return FONT_ERR_PARAM; }
    f->sx=f->sy=f->rx=f->ry=16.f; f->valid=1;
    *out=f;
    return FONT_OK;
}
/* System font sets are not shipped with the port; the game falls back to its own fonts. */
static ABI int32_t font_open_set(void *lib,uint32_t type,uint32_t mode,const void *params,void **out) {
    (void)lib;(void)params;
    printf("Runtime: sceFontOpenFontSet(type=0x%x, mode=%u) not provided\n",type,mode);
    if (out) *out=NULL;
    return FONT_ERR_FATAL;
}
static ABI int32_t font_close(void *h) { Font *f=font_of(h); if (!f) return FONT_ERR_HANDLE; f->valid=0; free(f); return FONT_OK; }
static ABI int32_t font_bind_renderer(void *h,void *renderer) { (void)renderer; return font_of(h) ? FONT_OK : FONT_ERR_HANDLE; }
static ABI int32_t font_unbind_renderer(void *h) { return font_of(h) ? FONT_OK : FONT_ERR_HANDLE; }
static ABI int32_t font_set_scale(void *h,float w,float hh) { Font *f=font_of(h); if (!f) return FONT_ERR_HANDLE; f->sx=w; f->sy=hh; return FONT_OK; }
static ABI int32_t font_setup_render_scale(void *h,float w,float hh) { Font *f=font_of(h); if (!f) return FONT_ERR_HANDLE; f->rx=w; f->ry=hh; return FONT_OK; }
static ABI int32_t font_horizontal_layout(void *h,HorizontalLayout *out) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    if (!out) return FONT_ERR_PARAM;
    int asc,desc,gap; stbtt_GetFontVMetrics(&f->info,&asc,&desc,&gap);
    float s=scale_for(f,f->sy);
    out->baselineOffset=asc*s; out->lineAdvance=(asc-desc+gap)*s; out->decorationExtent=0.f;
    return FONT_OK;
}
static void glyph_metrics(Font *f,uint32_t code,float sx,float sy,GlyphMetrics *m,
                          int *x0,int *y0,int *x1,int *y1,BBGlyphChoice *choice) {
    BBGlyphChoice c=bb_glyph_choose(&f->info,code);
    if (choice) *choice=c;
    float xs=bb_scale(c.info,sx), ys=bb_scale(c.info,sy);
    int adv,lsb; stbtt_GetGlyphHMetrics(c.info,c.glyph,&adv,&lsb);
    stbtt_GetGlyphBitmapBox(c.info,c.glyph,xs,ys,x0,y0,x1,y1);
    memset(m,0,sizeof(*m));
    m->width=(float)(*x1-*x0); m->height=(float)(*y1-*y0);
    m->hbx=(float)*x0; m->hby=(float)-*y0; m->hadv=adv*xs;
}
static ABI int32_t font_char_metrics(void *h,uint32_t code,GlyphMetrics *out) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    if (!out) return FONT_ERR_PARAM;
    int a,b,c,d; glyph_metrics(f,code,f->sx,f->sy,out,&a,&b,&c,&d,NULL);
    return FONT_OK;
}
static ABI int32_t font_kerning(void *h,uint32_t pre,uint32_t code,Kerning *out) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    if (!out) return FONT_ERR_PARAM;
    memset(out,0,sizeof(*out));
    /* A fallback glyph has no kern pair in the primary face. */
    if (!stbtt_FindGlyphIndex(&f->info,(int)pre) || !stbtt_FindGlyphIndex(&f->info,(int)code))
        return FONT_OK;
    out->positionX=stbtt_GetCodepointKernAdvance(&f->info,(int)pre,(int)code)*scale_for(f,f->sx);
    return FONT_OK;
}
static ABI int32_t font_render_glyph(void *h,uint32_t code,Surface *surf,float x,float y,GlyphMetrics *metrics,RenderOutput *result) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    if (!surf || !result) return FONT_ERR_PARAM;
    GlyphMetrics local; if (!metrics) metrics=&local;
    int x0,y0,x1,y1;
    BBGlyphChoice choice;
    glyph_metrics(f,code,f->rx,f->ry,metrics,&x0,&y0,&x1,&y1,&choice);
    int gw=x1-x0, gh=y1-y0, bpp=surf->pixelSizeByte;
    int dest_x=(int)floorf(x)+x0, dest_y=(int)floorf(y)+y0;
    int cx0=(int)surf->sc_x0, cy0=(int)surf->sc_y0, cx1=(int)surf->sc_x1, cy1=(int)surf->sc_y1;
    if (cx1>surf->width) cx1=surf->width;
    if (cy1>surf->height) cy1=surf->height;
    int ux=dest_x, uy=dest_y, uw=0, uh=0;
    if (gw>0 && gh>0 && bpp>0 && surf->buffer && cx1>cx0 && cy1>cy0) {
        uint8_t *bitmap=malloc((size_t)gw*gh);
        if (!bitmap) return FONT_ERR_FATAL;
        stbtt_MakeGlyphBitmap(choice.info,bitmap,gw,gh,gw,
                              bb_scale(choice.info,f->rx),bb_scale(choice.info,f->ry),choice.glyph);
        int r0=dest_y>cy0?dest_y:cy0, r1=dest_y+gh<cy1?dest_y+gh:cy1;
        int c0=dest_x>cx0?dest_x:cx0, c1=dest_x+gw<cx1?dest_x+gw:cx1;
        ux=c0; uy=r0; uw=c1>c0?c1-c0:0; uh=r1>r0?r1-r0:0;
        for (int row=r0;row<r1;++row) {
            uint8_t *dst=(uint8_t *)surf->buffer+(size_t)row*(size_t)surf->widthByte;
            const uint8_t *src=bitmap+(size_t)(row-dest_y)*gw;
            for (int col=c0;col<c1;++col) {
                uint8_t cov=src[col-dest_x];
                uint8_t *p=dst+(size_t)col*bpp;
                for (int k=0;k<bpp;++k) p[k]=cov;
            }
        }
        free(bitmap);
    }
    memset(result,0,sizeof(*result));
    result->surface.widthByte=(uint32_t)surf->widthByte; result->surface.pixelSizeByte=(uint8_t)bpp;
    result->update.x=ux>0?(uint32_t)ux:0; result->update.y=uy>0?(uint32_t)uy:0; result->update.w=(uint32_t)uw; result->update.h=(uint32_t)uh;
    result->surface.address=(uint8_t *)surf->buffer+(size_t)result->update.y*(size_t)surf->widthByte+(size_t)result->update.x*(size_t)(bpp>0?bpp:0);
    float left=x+metrics->hbx, top=y+metrics->hby, right=left+metrics->width, bottom=top-metrics->height;
    int li=(int)floorf(left), ti=(int)ceilf(top), ri=(int)ceilf(right), bi=(int)floorf(bottom);
    int ai=(int)ceilf(x+metrics->hadv);
    result->image.bearingX=(float)li-x; result->image.bearingY=(float)ti-y; result->image.advance=(float)ai-x;
    result->image.stride=(float)(ri>ai?ri:ai)-x;
    result->image.width=ri>li?(uint32_t)(ri-li):0; result->image.height=ti>bi?(uint32_t)(ti-bi):0;
    return FONT_OK;
}

static const RuntimeExport exports[]={
    {"sceFontMemoryInit",font_mem_init}, {"sceFontCreateLibraryWithEdition",font_create_library},
    {"sceFontDestroyLibrary",font_destroy_library}, {"sceFontSupportSystemFonts",font_support_system},
    {"sceFontSupportExternalFonts",font_support_external}, {"sceFontAttachDeviceCacheBuffer",font_attach_cache},
    {"sceFontRenderSurfaceInit",font_surface_init}, {"sceFontCreateRendererWithEdition",font_create_renderer},
    {"sceFontDestroyRenderer",font_destroy_renderer}, {"sceFontOpenFontMemory",font_open_memory},
    {"sceFontOpenFontSet",font_open_set}, {"sceFontCloseFont",font_close},
    {"sceFontBindRenderer",font_bind_renderer}, {"sceFontUnbindRenderer",font_unbind_renderer},
    {"sceFontSetScalePixel",font_set_scale}, {"sceFontSetupRenderScalePixel",font_setup_render_scale},
    {"sceFontGetHorizontalLayout",font_horizontal_layout}, {"sceFontGetCharGlyphMetrics",font_char_metrics},
    {"sceFontGetKerning",font_kerning}, {"sceFontRenderCharGlyphImageHorizontal",font_render_glyph},
    {"sceFontRenderCharGlyphImage",font_render_glyph},
    {"sceFontSelectLibraryFt",font_select_library_ft}, {"sceFontSelectRendererFt",font_select_renderer_ft},
};
uintptr_t runtime_font_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
