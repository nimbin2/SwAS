/* sw_draw.h — a software canvas. ARGB8888, premultiplied, which is what
 * wl_shm wants. Rounded rectangles, circles, wedges and triangles come out of
 * signed distance fields, text out of stb_truetype with a per-size glyph
 * cache, images out of a box filter.
 *
 * No GPU and no windowing system: a Canvas is a pixel buffer and a clip
 * rectangle, so a whole frame can be drawn and then inspected in a test with
 * no compositor anywhere near it. That is the point of doing it this way, as
 * much as the short link line.
 *
 * It is written to be fast enough for a full-screen overlay. Blending is in
 * 8-bit integers, a run of one colour composites two channels at a time, and
 * every shape fills its solid middle with plain spans and keeps the distance
 * field for the edge — a rounded rectangle only evaluates it in the four
 * corners, a wedge only inside its own bounding box. Doing this per pixel in
 * floating point, as the first version did, cost a 4K frame most of a second.
 *
 * Lifted out of swbr, which has drawn itself like this all along.
 *
 * Define SW_DRAW_IMPL in exactly one translation unit before including this.
 * stb_truetype.h must be on the include path; this header includes it, and
 * emits its implementation unless SW_DRAW_NO_STB_IMPL says otherwise (define
 * that when something else in the program already does).
 *
 * Written/modified by: Claude Opus 5
 */
#ifndef SW_DRAW_H
#define SW_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* stb_truetype.h has no re-include guard of its own, so only pull it in when
 * the program has not already done so. */
#ifndef __STB_INCLUDE_STB_TRUETYPE_H__
#include "stb_truetype.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types */

typedef struct { uint8_t r, g, b, a; } Col;

typedef struct {
    uint32_t *px;                /* w*h, ARGB8888 premultiplied */
    int w, h;
    int cx0, cy0, cx1, cy1;      /* clip rectangle */
} Canvas;

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    bool ok;
} FontFile;

typedef struct {
    unsigned cp;                 /* 0 = free slot */
    int w, h, x0, y0;
    float adv;
    unsigned char *bmp;
} Glyph;

typedef struct {
    int px;
    float sc_pri, sc_alt, sc_bold, ascent, lineh;
    Glyph *tab;
    int cap, n;
    bool used;
    bool bold;        /* ask the bold face for glyphs */
    bool synth;       /* no bold face: embolden by drawing twice */
} Font;

/* An image to blit: straight RGBA bytes, as nanosvg and stb_image hand them
 * over, not premultiplied. */
typedef struct {
    const unsigned char *rgba;
    int w, h, stride;
} Image;

#define SW_WHITE ((Col){ 255, 255, 255, 255 })

/* -------------------------------------------------------------- interface */

void  sw_parse_color(const char *s, Col *out);
Col   sw_col_alpha(Col c, float f);
Col   sw_col_mix(Col a, Col b, float t);

void  sw_fonts_init(const char *primary, const char *fallback);
void  sw_fonts_free(void);
Font *sw_font(int px);                       /* cached per pixel size */
/* An optional bold face. Without one, sw_font_bold emboldens the regular
   face by drawing each glyph twice, a third of a pixel apart. */
void  sw_fonts_bold(const char *path);
Font *sw_font_bold(int px);
float sw_text_width(Font *f, const char *s);

void  sw_canvas_clip(Canvas *cv, int x0, int y0, int x1, int y1);
void  sw_canvas_clip_all(Canvas *cv);
void  sw_canvas_fill(Canvas *cv, Col c);     /* the whole clip rectangle */
/* The frame's background: the clip rectangle is *replaced* by c rather than
   blended with it. On a buffer that was just cleared the result is the same,
   and it is a plain store instead of a read-modify-write — worth having on a
   full-screen overlay, where this is the single biggest thing drawn. */
void  sw_canvas_bg(Canvas *cv, Col c);

void  sw_fill_rect(Canvas *cv, float x, float y, float w, float h, Col c);
void  sw_fill_round(Canvas *cv, float x, float y, float w, float h,
                    float rtl, float rtr, float rbr, float rbl, Col c);
void  sw_stroke_round(Canvas *cv, float x, float y, float w, float h,
                      float rtl, float rtr, float rbr, float rbl,
                      float t, Col c);
void  sw_fill_tri(Canvas *cv, float x0, float y0, float x1, float y1,
                  float x2, float y2, Col c);
void  sw_fill_circle(Canvas *cv, float cx, float cy, float rad, Col c);
void  sw_fill_sector(Canvas *cv, float cx, float cy, float r0, float r1,
                     float a0, float a1, Col c);
void  sw_blit(Canvas *cv, Image img, float x, float y, float w, float h,
              float alpha);
void  sw_text(Canvas *cv, Font *f, float x, float baseline, Col c, const char *s);
float sw_text_width_n(Font *f, const char *s, size_t n);
float sw_font_descent(Font *f);              /* positive, below the baseline */

/* An offscreen canvas, for a frame that is drawn once and then scaled,
   faded or blurred as a whole. Same pixel format as a window's canvas. */
Canvas *sw_canvas_new(int w, int h);
void    sw_canvas_free(Canvas *cv);
void    sw_canvas_clear(Canvas *cv);         /* to nothing, ignoring the clip */
/* Scale `src` into `dst`, box-filtered when it shrinks. `tint` multiplies the
   colour (use SW_WHITE for none); `alpha` the whole thing. */
void    sw_blit_canvas(Canvas *dst, const Canvas *src, float x, float y,
                       float w, float h, Col tint, float alpha);

/* Write a canvas out as a PNG. Stored (uncompressed) deflate: no zlib, and
   a big file, which is fine for the one frame a --shot asks for. */
bool    sw_write_png(const char *path, const Canvas *cv);

#ifdef __cplusplus
}
#endif
#endif /* SW_DRAW_H */

/* ===================================================================== */
#ifdef SW_DRAW_IMPL
#ifndef SW_DRAW_IMPL_ONCE
#define SW_DRAW_IMPL_ONCE

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One shell call, to ask fontconfig where a family lives. Under -std=c11 the
 * libc headers hide popen, and a feature macro set this late would come after
 * the program's own includes and do nothing — so declare the two directly.
 * Both are POSIX; this is the same declaration unistd would give. */
#if defined(__STRICT_ANSI__)
FILE *popen(const char *, const char *);
int   pclose(FILE *);
#endif

static float sw_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int   sw_clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }

void sw_parse_color(const char *s, Col *out)
{
    if (!s || !out) return;
    if (*s == '#') s++;
    unsigned r, g, b, a = 255;
    if (strlen(s) >= 8 && sscanf(s, "%2x%2x%2x%2x", &r, &g, &b, &a) == 4) { }
    else if (sscanf(s, "%2x%2x%2x", &r, &g, &b) == 3) { }
    else return;
    out->r = (uint8_t)r; out->g = (uint8_t)g; out->b = (uint8_t)b; out->a = (uint8_t)a;
}

Col sw_col_alpha(Col c, float f)
{
    c.a = (uint8_t)sw_clampf((float)c.a * f, 0.0f, 255.0f);
    return c;
}

Col sw_col_mix(Col a, Col b, float t)
{
    t = sw_clampf(t, 0.0f, 1.0f);
    Col o;
    o.r = (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * t);
    o.g = (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * t);
    o.b = (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * t);
    o.a = (uint8_t)((float)a.a + ((float)b.a - (float)a.a) * t);
    return o;
}
#ifndef SW_PATHMAX
#define SW_PATHMAX 4096
#endif

/* ------------------------------------------------------------------ fonts */
/* stb_truetype, rasterised straight into our own ARGB buffer. Glyphs are
 * cached per size on first use, so any codepoint works — a status line is
 * full of box drawing characters and symbols. */

static FontFile sw_font_pri, sw_font_alt, sw_font_bld;

#define SW_FONT_CACHE 12
static Font sw_font_cache[SW_FONT_CACHE];

static void *sw_xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "sw_draw: out of memory\n"); abort(); }
    return p;
}

static bool sw_file_exists(const char *p)
{
    return p && *p && access(p, R_OK) == 0;
}

static bool sw_font_file_load(FontFile *ff, const char *path)
{
    memset(ff, 0, sizeof(*ff));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    ff->data = (unsigned char *)sw_xmalloc((size_t)sz);
    if (fread(ff->data, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(ff->data); ff->data = NULL; return false;
    }
    fclose(f);
    if (!stbtt_InitFont(&ff->info, ff->data, stbtt_GetFontOffsetForIndex(ff->data, 0))) {
        free(ff->data); ff->data = NULL; return false;
    }
    ff->ok = true;
    return true;
}

static bool sw_try_font_paths(char *out, size_t n)
{
    const char *cand[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf", NULL
    };
    for (int i = 0; cand[i]; ++i)
        if (sw_file_exists(cand[i])) { snprintf(out, n, "%s", cand[i]); return true; }
    return false;
}
static bool sw_fc_match(const char *pattern, char *out, size_t n)
{
    if (!pattern || !*pattern) pattern = "monospace";
    char safe[256];
    size_t j = 0;
    for (size_t i = 0; pattern[i] && j < sizeof(safe) - 1; ++i) {
        char ch = pattern[i];
        if (ch == '"' || ch == '`' || ch == '$' || ch == '\\' || ch == ';' ||
            ch == '|' || ch == '&' || ch == '\n' || ch == '\r') continue;
        safe[j++] = ch;
    }
    safe[j] = 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fc-match --format=%%{file} \"%s\" 2>/dev/null", safe);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char buf[SW_PATHMAX];
    size_t r = fread(buf, 1, sizeof(buf) - 1, p);
    buf[r] = 0;
    pclose(p);
    char *s = buf;
    while (*s == ' ' || *s == '\n' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                     e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    if (*s && sw_file_exists(s)) { snprintf(out, n, "%s", s); return true; }
    return false;
}

/* The font cache is keyed on pixel size, so a program can ask for text at any
 * size and the glyphs for it are rasterised once. */
void sw_fonts_init(const char *primary, const char *fallback)
{
    char path[SW_PATHMAX] = "";
    if (primary && *primary) {
        snprintf(path, sizeof(path), "%s", primary);
        if (!sw_file_exists(path)) sw_fc_match(primary, path, sizeof(path));
    }
    if (!sw_file_exists(path)) sw_fc_match("monospace", path, sizeof(path));
    if (!sw_file_exists(path)) sw_try_font_paths(path, sizeof(path));
    if (*path) sw_font_file_load(&sw_font_pri, path);

    char alt[SW_PATHMAX] = "";
    if (fallback && *fallback) {
        snprintf(alt, sizeof(alt), "%s", fallback);
        if (!sw_file_exists(alt)) sw_fc_match(fallback, alt, sizeof(alt));
    } else if (!sw_fc_match("Noto Sans Symbols 2", alt, sizeof(alt))) {
        /* a symbol font if the desktop has one: it covers glyphs a mono font
         * usually lacks. Silently skipped when absent. */
        sw_fc_match("Noto Sans Symbols2", alt, sizeof(alt));
    }
    if (sw_file_exists(alt) && strcmp(alt, path)) sw_font_file_load(&sw_font_alt, alt);
}

void sw_fonts_bold(const char *path)
{
    char p[SW_PATHMAX] = "";
    if (!path || !*path) return;
    snprintf(p, sizeof(p), "%s", path);
    if (!sw_file_exists(p) && !sw_fc_match(path, p, sizeof(p))) return;
    if (!sw_file_exists(p)) return;
    sw_font_file_load(&sw_font_bld, p);
}

void sw_fonts_free(void)
{
    for (int i = 0; i < SW_FONT_CACHE; ++i) {
        Font *f = &sw_font_cache[i];
        for (int j = 0; j < f->cap; ++j) free(f->tab[j].bmp);
        free(f->tab);
        f->tab = NULL;
        f->cap = f->n = 0;
        f->used = false;
    }
    free(sw_font_pri.data);
    free(sw_font_alt.data);
    free(sw_font_bld.data);
    memset(&sw_font_pri, 0, sizeof(sw_font_pri));
    memset(&sw_font_alt, 0, sizeof(sw_font_alt));
    memset(&sw_font_bld, 0, sizeof(sw_font_bld));
}

static void sw_glyph_tab_init(Font *f, int cap)
{
    f->cap = cap;
    f->n = 0;
    f->tab = (Glyph *)sw_xmalloc((size_t)cap * sizeof(Glyph));
    memset(f->tab, 0, (size_t)cap * sizeof(Glyph));
}

static Glyph *glyph_slot(Font *f, unsigned cp)
{
    unsigned h = cp * 2654435761u;
    for (;;) {
        unsigned i = h & (unsigned)(f->cap - 1);
        for (int probe = 0; probe < f->cap; ++probe) {
            Glyph *g = &f->tab[(i + (unsigned)probe) & (unsigned)(f->cap - 1)];
            if (g->cp == cp) return g;
            if (g->cp == 0) return g;
        }
        /* full: grow and rehash */
        Glyph *old = f->tab;
        int oldcap = f->cap;
        sw_glyph_tab_init(f, oldcap * 2);
        for (int k = 0; k < oldcap; ++k) {
            if (!old[k].cp) continue;
            Glyph *g = glyph_slot(f, old[k].cp);
            *g = old[k];
            f->n++;
        }
        free(old);
    }
}

static Glyph *sw_glyph_get(Font *f, unsigned cp)
{
    Glyph *g = glyph_slot(f, cp);
    if (g->cp == cp) return g;

    FontFile *ff = (f->bold && sw_font_bld.ok) ? &sw_font_bld : &sw_font_pri;
    float sc = (f->bold && sw_font_bld.ok) ? f->sc_bold : f->sc_pri;
    int gi = ff->ok ? stbtt_FindGlyphIndex(&ff->info, (int)cp) : 0;
    if (!gi && ff != &sw_font_pri && sw_font_pri.ok) {
        int gi0 = stbtt_FindGlyphIndex(&sw_font_pri.info, (int)cp);
        if (gi0) { ff = &sw_font_pri; sc = f->sc_pri; gi = gi0; }
    }
    if (!gi && sw_font_alt.ok) {
        int gi2 = stbtt_FindGlyphIndex(&sw_font_alt.info, (int)cp);
        if (gi2) { ff = &sw_font_alt; sc = f->sc_alt; gi = gi2; }
    }
    memset(g, 0, sizeof(*g));
    g->cp = cp;
    f->n++;
    if (!ff->ok || !gi) {
        g->adv = (float)f->px * 0.5f;      /* missing: leave a blank */
        return g;
    }
    int aw, lsb;
    stbtt_GetGlyphHMetrics(&ff->info, gi, &aw, &lsb);
    g->adv = (float)aw * sc;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&ff->info, gi, sc, sc, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && w < 512 && h < 512) {
        g->bmp = (unsigned char *)sw_xmalloc((size_t)w * (size_t)h);
        stbtt_MakeGlyphBitmap(&ff->info, g->bmp, w, h, w, sc, sc, gi);
        g->w = w; g->h = h; g->x0 = x0; g->y0 = y0;
    }
    return g;
}

/* One Font per pixel size (and therefore per output scale). */
static Font *sw_font_get(int px, bool bold)
{
    if (px < 4) px = 4;
    for (int i = 0; i < SW_FONT_CACHE; ++i)
        if (sw_font_cache[i].used && sw_font_cache[i].px == px &&
            sw_font_cache[i].bold == bold) return &sw_font_cache[i];

    int slot = -1;
    for (int i = 0; i < SW_FONT_CACHE; ++i) if (!sw_font_cache[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;                       /* recycle the first one */
    Font *f = &sw_font_cache[slot];
    if (f->used) {
        for (int k = 0; k < f->cap; ++k) free(f->tab[k].bmp);
        free(f->tab);
        memset(f, 0, sizeof(*f));
    }
    f->used = true;
    f->px = px;
    f->bold = bold;
    f->synth = bold && !sw_font_bld.ok;
    f->sc_pri = sw_font_pri.ok ? stbtt_ScaleForPixelHeight(&sw_font_pri.info, (float)px) : 1.0f;
    f->sc_alt = sw_font_alt.ok ? stbtt_ScaleForPixelHeight(&sw_font_alt.info, (float)px) : 1.0f;
    f->sc_bold = sw_font_bld.ok ? stbtt_ScaleForPixelHeight(&sw_font_bld.info, (float)px) : 1.0f;
    {
        FontFile *mf = (bold && sw_font_bld.ok) ? &sw_font_bld : &sw_font_pri;
        float msc = (bold && sw_font_bld.ok) ? f->sc_bold : f->sc_pri;
        int asc = 0, desc = 0, gap = 0;
        if (mf->ok) stbtt_GetFontVMetrics(&mf->info, &asc, &desc, &gap);
        f->ascent = (float)asc * msc;
        f->lineh = (float)(asc - desc) * msc;
    }
    if (f->ascent <= 0) { f->ascent = (float)px * 0.8f; f->lineh = (float)px; }
    sw_glyph_tab_init(f, 256);
    return f;
}

Font *sw_font(int px)      { return sw_font_get(px, false); }
Font *sw_font_bold(int px) { return sw_font_get(px, true); }

static unsigned sw_utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned cp;
    int n;
    if (s[0] < 0x80) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1Fu; n = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0Fu; n = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07u; n = 4; }
    else { *p = (const char *)(s + 1); return 0xFFFD; }
    for (int i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) { n = i; break; }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    *p = (const char *)(s + n);
    return cp;
}
float sw_text_width(Font *f, const char *s)
{
    float w = 0;
    const char *p = s;
    while (*p) {
        unsigned cp = sw_utf8_next(&p);
        if (cp == 0xFE0F || cp == 0x200D) continue;   /* variation selectors */
        w += sw_glyph_get(f, cp)->adv + (f->synth ? 0.35f : 0.0f);
    }
    return w;
}

void sw_canvas_clip(Canvas *cv, int x0, int y0, int x1, int y1)
{
    cv->cx0 = sw_clampi(x0, 0, cv->w);
    cv->cy0 = sw_clampi(y0, 0, cv->h);
    cv->cx1 = sw_clampi(x1, 0, cv->w);
    cv->cy1 = sw_clampi(y1, 0, cv->h);
}

void sw_canvas_clip_all(Canvas *cv) { sw_canvas_clip(cv, 0, 0, cv->w, cv->h); }

/* ------------------------------------------------------------- blending
 * The canvas is premultiplied ARGB8888. Everything below works in 8-bit
 * integers: a float blend per pixel is what made a full-screen frame cost
 * tens of milliseconds. SW_MUL is the usual x*y/255 without a divide.
 */

#define SW_MUL(x, y) ((unsigned)((((unsigned)(x) * (unsigned)(y) + 128u) * 257u) >> 16))

/* Two 8-bit channels packed in 0x00ff00ff, each already multiplied by a
   0..255 factor: divide both by 255, rounded, without a divide. */
static inline uint32_t sw_div255x2(uint32_t x)
{
    uint32_t t = x + 0x00800080u;
    return ((t + ((t >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
}

/* One pixel, no bounds check: callers clip their spans first. `a` is the
   coverage and the colour's own alpha already multiplied together. */
static inline void sw_put(uint32_t *p, Col c, unsigned a)
{
    unsigned ia, d, da, dr, dg, db;
    if (!a) return;
    if (a == 255) {
        *p = 0xff000000u | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
        return;
    }
    ia = 255u - a;
    d = *p;
    da = (d >> 24) & 0xffu; dr = (d >> 16) & 0xffu;
    dg = (d >> 8) & 0xffu;  db = d & 0xffu;
    *p = ((SW_MUL(da, ia) + a) << 24)
       | ((SW_MUL(c.r, a) + SW_MUL(dr, ia)) << 16)
       | ((SW_MUL(c.g, a) + SW_MUL(dg, ia)) << 8)
       |  (SW_MUL(c.b, a) + SW_MUL(db, ia));
}

static inline void sw_blend8(Canvas *cv, int x, int y, Col c, unsigned a)
{
    if (x < cv->cx0 || x >= cv->cx1 || y < cv->cy0 || y >= cv->cy1) return;
    sw_put(&cv->px[(size_t)y * (size_t)cv->w + x], c, a);
}

/* Kept for callers that think in coverage. */
static inline void sw_blend(Canvas *cv, int x, int y, Col c, float cov)
{
    int a;
    if (cov <= 0.0f) return;
    if (cov > 1.0f) cov = 1.0f;
    a = (int)((float)c.a * cov + 0.5f);
    if (a <= 0) return;
    sw_blend8(cv, x, y, c, (unsigned)a);
}

/* A run of pixels on one row at a constant alpha. This is where the time
   goes, so the clip is applied once and the inner loop is bare. */
static void sw_span(Canvas *cv, int x0, int x1, int y, Col c, unsigned a)
{
    uint32_t *p, *e;
    if (!a || y < cv->cy0 || y >= cv->cy1) return;
    if (x0 < cv->cx0) x0 = cv->cx0;
    if (x1 > cv->cx1) x1 = cv->cx1;
    if (x0 >= x1) return;
    p = &cv->px[(size_t)y * (size_t)cv->w + x0];
    e = p + (x1 - x0);
    if (a == 255) {
        uint32_t v = 0xff000000u | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
        while (p < e) *p++ = v;
        return;
    }
    {   /* two channels at a time: the colour is constant along the run, so
           the only work per pixel is one multiply per half and an add */
        unsigned ia = 255u - a;
        uint32_t srb = (SW_MUL(c.r, a) << 16) | SW_MUL(c.b, a);
        uint32_t sag = ((unsigned)a << 16) | SW_MUL(c.g, a);
        while (p < e) {
            uint32_t d = *p;
            uint32_t rb = sw_div255x2((d & 0x00ff00ffu) * ia);
            uint32_t ag = sw_div255x2(((d >> 8) & 0x00ff00ffu) * ia);
            *p++ = ((srb + rb) & 0x00ff00ffu) | (((sag + ag) & 0x00ff00ffu) << 8);
        }
    }
}

/* how much of [a0,a1) the unit cell [i,i+1) covers */
static inline float sw_cov1(float a0, float a1, int i)
{
    float lo = (float)i, hi = lo + 1.0f;
    float l = a0 > lo ? a0 : lo, r = a1 < hi ? a1 : hi;
    return r > l ? r - l : 0.0f;
}

void sw_fill_rect(Canvas *cv, float x, float y, float w, float h, Col c)
{
    int ix0, ix1, iy0, iy1, yy, fx0, fx1;
    float x1f = x + w, y1f = y + h;
    if (w <= 0 || h <= 0 || !c.a) return;

    ix0 = (int)floorf(x); ix1 = (int)ceilf(x1f);
    iy0 = (int)floorf(y); iy1 = (int)ceilf(y1f);
    if (ix0 < cv->cx0) ix0 = cv->cx0;
    if (ix1 > cv->cx1) ix1 = cv->cx1;
    if (iy0 < cv->cy0) iy0 = cv->cy0;
    if (iy1 > cv->cy1) iy1 = cv->cy1;
    if (ix0 >= ix1 || iy0 >= iy1) return;

    fx0 = (int)ceilf(x);  if (fx0 < ix0) fx0 = ix0;   /* first full column */
    fx1 = (int)floorf(x1f); if (fx1 > ix1) fx1 = ix1; /* past the last one */

    for (yy = iy0; yy < iy1; ++yy) {
        float yc = sw_cov1(y, y1f, yy);
        unsigned af;
        if (yc <= 0.0f) continue;
        af = (unsigned)((float)c.a * yc + 0.5f);
        if (fx0 > ix0) sw_blend8(cv, ix0, yy,
                                 c, (unsigned)((float)c.a * yc * sw_cov1(x, x1f, ix0) + 0.5f));
        if (fx1 > fx0) sw_span(cv, fx0, fx1, yy, c, af);
        if (fx1 < ix1) sw_blend8(cv, ix1 - 1, yy,
                                 c, (unsigned)((float)c.a * yc * sw_cov1(x, x1f, ix1 - 1) + 0.5f));
    }
}

/* One rounded corner: inside its own box the distance to the shape is just
   the distance to the circle's centre, so this is a quarter disc. */
static void sw_corner(Canvas *cv, float cx, float cy, float r,
                      int sx, int sy, Col c)
{
    int x0, y0, x1, y1, xx, yy;
    if (r <= 0.0f) return;
    x0 = (int)floorf(sx < 0 ? cx - r - 1.0f : cx);
    x1 = (int)ceilf (sx < 0 ? cx           : cx + r + 1.0f);
    y0 = (int)floorf(sy < 0 ? cy - r - 1.0f : cy);
    y1 = (int)ceilf (sy < 0 ? cy           : cy + r + 1.0f);
    if (x0 < cv->cx0) x0 = cv->cx0;
    if (x1 > cv->cx1) x1 = cv->cx1;
    if (y0 < cv->cy0) y0 = cv->cy0;
    if (y1 > cv->cy1) y1 = cv->cy1;
    for (yy = y0; yy < y1; ++yy) {
        float dy = (float)yy + 0.5f - cy;
        uint32_t *row = &cv->px[(size_t)yy * (size_t)cv->w];
        for (xx = x0; xx < x1; ++xx) {
            float dx = (float)xx + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy) - r;
            float cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            sw_put(&row[xx], c, (unsigned)((float)c.a * cov + 0.5f));
        }
    }
}

/* Rounded box with a radius per corner (tl, tr, br, bl). The straight parts
 * are plain rectangles and only the four corners go through a distance
 * field, which is what keeps a full-screen fill cheap. A corner radius of 0
 * is a square corner — that is how the bar stays flush with the screen edge. */
void sw_fill_round(Canvas *cv, float x, float y, float w, float h,
                       float rtl, float rtr, float rbr, float rbl, Col c)
{
    float cap, rt, rb, mid;
    if (w <= 0 || h <= 0 || !c.a) return;
    cap = fminf(w * 0.5f, h * 0.5f);
    rtl = sw_clampf(rtl, 0, cap); rtr = sw_clampf(rtr, 0, cap);
    rbr = sw_clampf(rbr, 0, cap); rbl = sw_clampf(rbl, 0, cap);
    if (rtl <= 0.0f && rtr <= 0.0f && rbr <= 0.0f && rbl <= 0.0f) {
        sw_fill_rect(cv, x, y, w, h, c);
        return;
    }

    rt = fmaxf(rtl, rtr);
    rb = fmaxf(rbr, rbl);
    mid = h - rt - rb;

    if (mid > 0.0f) sw_fill_rect(cv, x, y + rt, w, mid, c);

    /* top band */
    if (rt > 0.0f) {
        sw_fill_rect(cv, x + rtl, y, w - rtl - rtr, rt, c);
        if (rtl < rt) sw_fill_rect(cv, x, y + rtl, rtl, rt - rtl, c);
        if (rtr < rt) sw_fill_rect(cv, x + w - rtr, y + rtr, rtr, rt - rtr, c);
        sw_corner(cv, x + rtl,     y + rtl, rtl, -1, -1, c);
        sw_corner(cv, x + w - rtr, y + rtr, rtr, +1, -1, c);
    }
    /* bottom band */
    if (rb > 0.0f) {
        sw_fill_rect(cv, x + rbl, y + h - rb, w - rbl - rbr, rb, c);
        if (rbl < rb) sw_fill_rect(cv, x, y + h - rb, rbl, rb - rbl, c);
        if (rbr < rb) sw_fill_rect(cv, x + w - rbr, y + h - rb, rbr, rb - rbr, c);
        sw_corner(cv, x + rbl,     y + h - rbl, rbl, -1, +1, c);
        sw_corner(cv, x + w - rbr, y + h - rbr, rbr, +1, +1, c);
    }
}

/* One corner of an outline: the same quarter disc kept to a ring. */
static void sw_corner_ring(Canvas *cv, float cx, float cy, float r, float t,
                           int sx, int sy, Col c)
{
    int x0, y0, x1, y1, xx, yy;
    if (r <= 0.0f || t <= 0.0f) return;
    x0 = (int)floorf(sx < 0 ? cx - r - 1.0f : cx);
    x1 = (int)ceilf (sx < 0 ? cx           : cx + r + 1.0f);
    y0 = (int)floorf(sy < 0 ? cy - r - 1.0f : cy);
    y1 = (int)ceilf (sy < 0 ? cy           : cy + r + 1.0f);
    if (x0 < cv->cx0) x0 = cv->cx0;
    if (x1 > cv->cx1) x1 = cv->cx1;
    if (y0 < cv->cy0) y0 = cv->cy0;
    if (y1 > cv->cy1) y1 = cv->cy1;
    for (yy = y0; yy < y1; ++yy) {
        float dy = (float)yy + 0.5f - cy;
        uint32_t *row = &cv->px[(size_t)yy * (size_t)cv->w];
        for (xx = x0; xx < x1; ++xx) {
            float dx = (float)xx + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy) - r;
            float cov = 0.5f - fabsf(d + t * 0.5f) + t * 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            sw_put(&row[xx], c, (unsigned)((float)c.a * cov + 0.5f));
        }
    }
}

/* An outline that follows the corner radius and stays inside the box, so a
 * button with no fill still has an edge. Four straight bars and four arcs. */
void sw_stroke_round(Canvas *cv, float x, float y, float w, float h,
                         float rtl, float rtr, float rbr, float rbl,
                         float t, Col c)
{
    float cap;
    if (w <= 0 || h <= 0 || t <= 0 || !c.a) return;
    cap = fminf(w * 0.5f, h * 0.5f);
    rtl = sw_clampf(rtl, 0, cap); rtr = sw_clampf(rtr, 0, cap);
    rbr = sw_clampf(rbr, 0, cap); rbl = sw_clampf(rbl, 0, cap);
    if (t > cap) t = cap;

    /* the straight runs, between the corners */
    if (w - rtl - rtr > 0.0f) sw_fill_rect(cv, x + rtl, y, w - rtl - rtr, t, c);
    if (w - rbl - rbr > 0.0f) sw_fill_rect(cv, x + rbl, y + h - t, w - rbl - rbr, t, c);
    if (h - rtl - rbl > 0.0f) sw_fill_rect(cv, x, y + rtl, t, h - rtl - rbl, c);
    if (h - rtr - rbr > 0.0f) sw_fill_rect(cv, x + w - t, y + rtr, t, h - rtr - rbr, c);

    sw_corner_ring(cv, x + rtl,     y + rtl,     rtl, t, -1, -1, c);
    sw_corner_ring(cv, x + w - rtr, y + rtr,     rtr, t, +1, -1, c);
    sw_corner_ring(cv, x + w - rbr, y + h - rbr, rbr, t, +1, +1, c);
    sw_corner_ring(cv, x + rbl,     y + h - rbl, rbl, t, -1, +1, c);
}


void sw_text(Canvas *cv, Font *f, float x, float baseline, Col c, const char *s)
{
    const char *p = s;
    float pen = x;
    while (*p) {
        unsigned cp = sw_utf8_next(&p);
        if (cp == 0xFE0F || cp == 0x200D) continue;
        Glyph *g = sw_glyph_get(f, cp);
        if (g->bmp) {
            int gx = (int)lroundf(pen) + g->x0;
            int gy = (int)lroundf(baseline) + g->y0;
            int pass, passes = f->synth ? 2 : 1;
            for (pass = 0; pass < passes; ++pass) {
                int bx = gx + pass;
                int x0 = bx < cv->cx0 ? cv->cx0 - bx : 0;
                int x1 = bx + g->w > cv->cx1 ? cv->cx1 - bx : g->w;
                for (int yy = 0; yy < g->h; ++yy) {
                    int ty = gy + yy;
                    const unsigned char *row;
                    uint32_t *dst;
                    if (ty < cv->cy0 || ty >= cv->cy1) continue;
                    row = g->bmp + (size_t)yy * (size_t)g->w;
                    dst = &cv->px[(size_t)ty * (size_t)cv->w + bx];
                    for (int xx = x0; xx < x1; ++xx) {
                        unsigned a = row[xx];
                        if (a) sw_put(&dst[xx], c, SW_MUL(c.a, a));
                    }
                }
            }
        }
        pen += g->adv + (f->synth ? 0.35f : 0.0f);
    }
}


void sw_canvas_fill(Canvas *cv, Col c)
{
    int y;
    if (!c.a) return;
    for (y = cv->cy0; y < cv->cy1; ++y) sw_span(cv, cv->cx0, cv->cx1, y, c, c.a);
}

void sw_canvas_bg(Canvas *cv, Col c)
{
    uint32_t v = ((uint32_t)c.a << 24) | (SW_MUL(c.r, c.a) << 16)
               | (SW_MUL(c.g, c.a) << 8) | SW_MUL(c.b, c.a);
    int y;
    for (y = cv->cy0; y < cv->cy1; ++y) {
        uint32_t *p = &cv->px[(size_t)y * (size_t)cv->w + cv->cx0];
        uint32_t *e = p + (cv->cx1 - cv->cx0);
        while (p < e) *p++ = v;
    }
}

/* A filled triangle, by half-plane tests on a 2x2 grid per pixel. swbr never
 * needed one; swas draws its wheel slices and its chevrons out of them. */
/* A filled triangle as a distance field: the signed distance to each of the
 * three edges, normalised so it is in pixels, and the smallest of them is the
 * coverage. One pass, no supersampling, and the span is stepped rather than
 * recomputed from scratch. */
void sw_fill_tri(Canvas *cv, float x0, float y0, float x1, float y1,
                 float x2, float y2, Col c)
{
    float minx, maxx, miny, maxy, d;
    float nx[3], ny[3], off[3];
    int ix0, iy0, ix1, iy1, y, e;

    if (!c.a) return;
    d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabsf(d) < 1e-6f) return;              /* degenerate */
    if (d < 0.0f) {                            /* make it counter-clockwise */
        float tx = x1, ty = y1;
        x1 = x2; y1 = y2; x2 = tx; y2 = ty;
    }

    minx = fminf(x0, fminf(x1, x2)); maxx = fmaxf(x0, fmaxf(x1, x2));
    miny = fminf(y0, fminf(y1, y2)); maxy = fmaxf(y0, fmaxf(y1, y2));
    ix0 = (int)floorf(minx) - 1; ix1 = (int)ceilf(maxx) + 1;
    iy0 = (int)floorf(miny) - 1; iy1 = (int)ceilf(maxy) + 1;
    if (ix0 < cv->cx0) ix0 = cv->cx0;
    if (ix1 > cv->cx1) ix1 = cv->cx1;
    if (iy0 < cv->cy0) iy0 = cv->cy0;
    if (iy1 > cv->cy1) iy1 = cv->cy1;
    if (ix0 >= ix1 || iy0 >= iy1) return;

    {   /* inward unit normals, one per edge */
        float ex[3], ey[3], ax[3], ay[3];
        ax[0] = x0; ay[0] = y0; ex[0] = x1 - x0; ey[0] = y1 - y0;
        ax[1] = x1; ay[1] = y1; ex[1] = x2 - x1; ey[1] = y2 - y1;
        ax[2] = x2; ay[2] = y2; ex[2] = x0 - x2; ey[2] = y0 - y2;
        for (e = 0; e < 3; ++e) {
            float len = sqrtf(ex[e] * ex[e] + ey[e] * ey[e]);
            if (len < 1e-6f) return;
            nx[e] = -ey[e] / len;
            ny[e] =  ex[e] / len;
            off[e] = -(nx[e] * ax[e] + ny[e] * ay[e]);
        }
    }

    for (y = iy0; y < iy1; ++y) {
        float py = (float)y + 0.5f;
        float base[3];
        uint32_t *row = &cv->px[(size_t)y * (size_t)cv->w];
        int x;
        for (e = 0; e < 3; ++e) base[e] = ny[e] * py + off[e] + nx[e] * ((float)ix0 + 0.5f);
        for (x = ix0; x < ix1; ++x) {
            float dmin = base[0], cov;
            if (base[1] < dmin) dmin = base[1];
            if (base[2] < dmin) dmin = base[2];
            cov = 0.5f + dmin;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                sw_put(&row[x], c, (unsigned)((float)c.a * cov + 0.5f));
            }
            base[0] += nx[0]; base[1] += nx[1]; base[2] += nx[2];
        }
    }
}

void sw_fill_circle(Canvas *cv, float cx, float cy, float rad, Col c)
{
    int y0, y1, y;
    if (!c.a || rad <= 0.0f) return;
    y0 = (int)floorf(cy - rad) - 1; y1 = (int)ceilf(cy + rad) + 1;
    if (y0 < cv->cy0) y0 = cv->cy0;
    if (y1 > cv->cy1) y1 = cv->cy1;
    for (y = y0; y < y1; ++y) {
        float dy = (float)y + 0.5f - cy;
        float q = rad * rad - dy * dy;
        float hw = q > 0.0f ? sqrtf(q) : 0.0f;
        int x0 = (int)floorf(cx - hw) - 1, x1 = (int)ceilf(cx + hw) + 1, x;
        uint32_t *row = &cv->px[(size_t)y * (size_t)cv->w];
        int fx0, fx1;
        if (x0 < cv->cx0) x0 = cv->cx0;
        if (x1 > cv->cx1) x1 = cv->cx1;
        /* the middle of the span is solid; only its ends need a distance */
        fx0 = (int)ceilf(cx - hw + 0.5f); if (fx0 < x0) fx0 = x0;
        fx1 = (int)floorf(cx + hw - 0.5f); if (fx1 > x1) fx1 = x1;
        for (x = x0; x < x1; ++x) {
            float dx, d, cov;
            if (x == fx0 && fx1 > fx0) {        /* jump the solid middle */
                sw_span(cv, fx0, fx1, y, c, c.a);
                x = fx1 - 1;
                continue;
            }
            dx = (float)x + 0.5f - cx;
            d = sqrtf(dx * dx + dy * dy) - rad;
            cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            sw_put(&row[x], c, (unsigned)((float)c.a * cov + 0.5f));
        }
    }
}

/* A wedge of an annulus: the slice shape a radial menu is made of.
 *
 * Written as a distance field rather than by sampling. Both radial edges are
 * circles and both angular edges are rays through the centre, and the signed
 * distance to a ray through the origin is just the cross product with its
 * unit vector — so one sqrt a pixel covers all four edges, with no atan2 and
 * no supersampling. Rows outside the outer circle are skipped and the hole in
 * the middle is jumped over, which is most of a thick wheel.
 *
 * a0 may exceed a1 going the other way round; the wedge is always drawn the
 * short way from a0 to a1. */
void sw_fill_sector(Canvas *cv, float cx, float cy, float r0, float r1,
                    float a0, float a1, Col c)
{
    const float TAU = 6.283185307f;
    float span, u0x, u0y, u1x, u1y, sx0, sy0, sx1, sy1;
    int wide, y0, y1, y;

    if (!c.a || r1 <= 0.0f || r1 <= r0) return;
    if (r0 < 0.0f) r0 = 0.0f;

    span = a1 - a0;
    while (span < 0.0f)  span += TAU;
    while (span >= TAU)  span -= TAU;
    if (span >= TAU - 1e-4f) {          /* a full ring: no angular edges */
        span = TAU;
    }
    wide = span > 3.14159265f;
    u0x = cosf(a0);          u0y = sinf(a0);
    u1x = cosf(a0 + span);   u1y = sinf(a0 + span);

    /* The wedge's own box, not the whole circle's: a slice of a eleven-slot
       wheel touches about a tenth of the disc, and walking the rest of it is
       what made a frame cost tens of milliseconds. */
    {
        float bx0 = 0.0f, by0 = 0.0f, bx1 = 0.0f, by1 = 0.0f;
        float pts[8], k;
        int i, nq;
        pts[0] = r0 * u0x; pts[1] = r0 * u0y;
        pts[2] = r1 * u0x; pts[3] = r1 * u0y;
        pts[4] = r0 * u1x; pts[5] = r0 * u1y;
        pts[6] = r1 * u1x; pts[7] = r1 * u1y;
        bx0 = bx1 = pts[0]; by0 = by1 = pts[1];
        for (i = 1; i < 4; ++i) {
            if (pts[i * 2] < bx0) bx0 = pts[i * 2];
            if (pts[i * 2] > bx1) bx1 = pts[i * 2];
            if (pts[i * 2 + 1] < by0) by0 = pts[i * 2 + 1];
            if (pts[i * 2 + 1] > by1) by1 = pts[i * 2 + 1];
        }
        /* and wherever the arc crosses an axis inside the wedge */
        for (nq = 0; nq < 4; ++nq) {
            float ang = (float)nq * 1.570796327f;
            float rel = ang - a0;
            while (rel < 0.0f) rel += TAU;
            while (rel >= TAU) rel -= TAU;
            if (rel > span) continue;
            k = r1;
            if (nq == 0 && k > bx1) bx1 = k;
            if (nq == 1 && k > by1) by1 = k;
            if (nq == 2 && -k < bx0) bx0 = -k;
            if (nq == 3 && -k < by0) by0 = -k;
        }
        sy0 = cy + by0; sy1 = cy + by1;
        sx0 = cx + bx0; sx1 = cx + bx1;
    }

    y0 = (int)floorf(sy0) - 1; y1 = (int)ceilf(sy1) + 1;
    if (y0 < cv->cy0) y0 = cv->cy0;
    if (y1 > cv->cy1) y1 = cv->cy1;

    for (y = y0; y < y1; ++y) {
        float dy = (float)y + 0.5f - cy;
        float q = r1 * r1 - dy * dy;
        float ow = q > 0.0f ? sqrtf(q) : 0.0f;
        float qi = r0 * r0 - dy * dy;
        float iw = (r0 > 0.0f && qi > 0.0f) ? sqrtf(qi) : -1.0f;
        int x0, x1, x;
        int hole0 = 0, hole1 = 0;
        uint32_t *row = &cv->px[(size_t)y * (size_t)cv->w];
        float lo = cx - ow, hi = cx + ow;
        if (lo < sx0) lo = sx0;
        if (hi > sx1) hi = sx1;
        x0 = (int)floorf(lo) - 1; x1 = (int)ceilf(hi) + 1;
        if (x0 < cv->cx0) x0 = cv->cx0;
        if (x1 > cv->cx1) x1 = cv->cx1;
        if (iw > 1.0f) {                        /* the untouched middle */
            hole0 = (int)ceilf(cx - iw + 1.0f);
            hole1 = (int)floorf(cx + iw - 1.0f);
        }
        for (x = x0; x < x1; ++x) {
            float dx, len, d, da, cov;
            if (hole1 > hole0 && x >= hole0 && x < hole1) { x = hole1 - 1; continue; }
            dx = (float)x + 0.5f - cx;
            len = sqrtf(dx * dx + dy * dy);
            d = r1 - len;                        /* inside the outer circle */
            if (r0 > 0.0f) { float di = len - r0; if (di < d) d = di; }
            if (d <= -1.0f) continue;
            if (span < TAU) {
                float e0 = u0x * dy - u0y * dx;  /* left of the first ray  */
                float e1 = dx * u1y - dy * u1x;  /* right of the second    */
                da = wide ? (e0 > e1 ? e0 : e1) : (e0 < e1 ? e0 : e1);
                if (da < d) d = da;
            }
            cov = 0.5f + d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            sw_put(&row[x], c, (unsigned)((float)c.a * cov + 0.5f));
        }
    }
}

/* An RGBA image scaled into a box, box-filtered when shrinking so an icon
 * rasterised large and drawn small does not come out speckled. The source is
 * straight (not premultiplied) RGBA, which is what nanosvg and stb_image give.
 */
void sw_blit(Canvas *cv, Image img, float x, float y, float w, float h,
             float alpha)
{
    if (!img.rgba || img.w <= 0 || img.h <= 0 || w <= 0.0f || h <= 0.0f) return;
    if (alpha <= 0.0f) return;
    int stride = img.stride > 0 ? img.stride : img.w * 4;
    unsigned ga = alpha >= 1.0f ? 255u : (unsigned)(alpha * 255.0f + 0.5f);

    int ix0 = sw_clampi((int)floorf(x), cv->cx0, cv->cx1);
    int iy0 = sw_clampi((int)floorf(y), cv->cy0, cv->cy1);
    int ix1 = sw_clampi((int)ceilf(x + w), cv->cx0, cv->cx1);
    int iy1 = sw_clampi((int)ceilf(y + h), cv->cy0, cv->cy1);

    float sx = (float)img.w / w, sy = (float)img.h / h;
    int bx = (int)sx, by = (int)sy;            /* box size when shrinking */
    if (bx < 1) bx = 1;
    if (by < 1) by = 1;

    for (int dy = iy0; dy < iy1; ++dy) {
        for (int dx = ix0; dx < ix1; ++dx) {
            float u = ((float)dx + 0.5f - x) * sx;
            float v = ((float)dy + 0.5f - y) * sy;
            int u0 = (int)u, v0 = (int)v;
            if (u0 < 0 || v0 < 0 || u0 >= img.w || v0 >= img.h) continue;

            unsigned rs = 0, gs = 0, bs = 0, as = 0, n = 0;
            for (int j = 0; j < by; ++j) {
                int vv = v0 + j;
                if (vv >= img.h) break;
                const unsigned char *row = img.rgba + (size_t)vv * (size_t)stride;
                for (int i = 0; i < bx; ++i) {
                    int uu = u0 + i;
                    if (uu >= img.w) break;
                    const unsigned char *p = row + (size_t)uu * 4;
                    rs += p[0]; gs += p[1]; bs += p[2]; as += p[3];
                    n++;
                }
            }
            if (!n || !as) continue;
            {
                Col c;
                unsigned av = as / n;
                c.r = (uint8_t)(rs / n); c.g = (uint8_t)(gs / n);
                c.b = (uint8_t)(bs / n); c.a = 255;
                sw_blend8(cv, dx, dy, c, SW_MUL(av, ga));
            }
        }
    }
}


float sw_text_width_n(Font *f, const char *s, size_t n)
{
    char buf[512];
    if (!f || !s || n == 0) return 0.0f;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return sw_text_width(f, buf);
}

float sw_font_descent(Font *f)
{
    if (!f) return 0.0f;
    float d = f->lineh - f->ascent;
    return d > 0.0f ? d : 0.0f;
}

Canvas *sw_canvas_new(int w, int h)
{
    Canvas *cv;
    if (w <= 0 || h <= 0) return NULL;
    cv = (Canvas *)calloc(1, sizeof *cv);
    if (!cv) return NULL;
    cv->px = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!cv->px) { free(cv); return NULL; }
    cv->w = w; cv->h = h;
    sw_canvas_clip_all(cv);
    return cv;
}

void sw_canvas_free(Canvas *cv)
{
    if (!cv) return;
    free(cv->px);
    free(cv);
}

void sw_canvas_clear(Canvas *cv)
{
    if (!cv || !cv->px) return;
    memset(cv->px, 0, (size_t)cv->w * (size_t)cv->h * 4);
}

void sw_blit_canvas(Canvas *dst, const Canvas *src, float x, float y,
                    float w, float h, Col tint, float alpha)
{
    int ix0, iy0, ix1, iy1, dx, dy, bx, by;
    float sx, sy;
    unsigned ga;
    if (!dst || !src || !src->px || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;
    ga = alpha >= 1.0f ? 255u : (unsigned)(alpha * 255.0f + 0.5f);
    if (!ga) return;

    ix0 = sw_clampi((int)floorf(x),     dst->cx0, dst->cx1);
    iy0 = sw_clampi((int)floorf(y),     dst->cy0, dst->cy1);
    ix1 = sw_clampi((int)ceilf(x + w),  dst->cx0, dst->cx1);
    iy1 = sw_clampi((int)ceilf(y + h),  dst->cy0, dst->cy1);

    /* 1:1 and aligned: compose premultiplied over premultiplied, which needs
       no un-premultiply and no resampling. This is the backdrop's own path,
       so it runs on every frame of a fade. */
    if (tint.r == 255 && tint.g == 255 && tint.b == 255 &&
        (int)w == src->w && (int)h == src->h &&
        x == floorf(x) && y == floorf(y)) {
        int ox = (int)x, oy = (int)y;
        for (dy = iy0; dy < iy1; ++dy) {
            const uint32_t *srow;
            uint32_t *drow;
            int sy1 = dy - oy;
            if (sy1 < 0 || sy1 >= src->h) continue;
            srow = src->px + (size_t)sy1 * (size_t)src->w;
            drow = dst->px + (size_t)dy * (size_t)dst->w;
            for (dx = ix0; dx < ix1; ++dx) {
                uint32_t sp, dp;
                unsigned sa, sr, sg, sb, ia;
                int sx1 = dx - ox;
                if (sx1 < 0 || sx1 >= src->w) continue;
                sp = srow[sx1];
                sa = (sp >> 24) & 0xffu;
                if (!sa) continue;
                sr = (sp >> 16) & 0xffu; sg = (sp >> 8) & 0xffu; sb = sp & 0xffu;
                if (ga != 255u) {
                    sa = SW_MUL(sa, ga); sr = SW_MUL(sr, ga);
                    sg = SW_MUL(sg, ga); sb = SW_MUL(sb, ga);
                    if (!sa) continue;
                }
                if (sa == 255u) { drow[dx] = 0xff000000u | (sr << 16) | (sg << 8) | sb; continue; }
                dp = drow[dx];
                ia = 255u - sa;
                drow[dx] = ((sa + SW_MUL((dp >> 24) & 0xffu, ia)) << 24)
                         | ((sr + SW_MUL((dp >> 16) & 0xffu, ia)) << 16)
                         | ((sg + SW_MUL((dp >> 8) & 0xffu, ia)) << 8)
                         |  (sb + SW_MUL(dp & 0xffu, ia));
            }
        }
        return;
    }

    sx = (float)src->w / w;
    sy = (float)src->h / h;
    bx = (int)sx; if (bx < 1) bx = 1;
    by = (int)sy; if (by < 1) by = 1;

    for (dy = iy0; dy < iy1; ++dy) {
        for (dx = ix0; dx < ix1; ++dx) {
            float u = ((float)dx + 0.5f - x) * sx;
            float v = ((float)dy + 0.5f - y) * sy;
            int u0 = (int)u, v0 = (int)v, i, j;
            unsigned as = 0, rs = 0, gs = 0, bs = 0, n = 0;
            Col c;
            float a;
            if (u0 < 0 || v0 < 0 || u0 >= src->w || v0 >= src->h) continue;
            for (j = 0; j < by; ++j) {
                int vv = v0 + j;
                const uint32_t *row;
                if (vv >= src->h) break;
                row = src->px + (size_t)vv * (size_t)src->w;
                for (i = 0; i < bx; ++i) {
                    uint32_t p;
                    int uu = u0 + i;
                    if (uu >= src->w) break;
                    p = row[uu];
                    as += (p >> 24) & 0xff;
                    rs += (p >> 16) & 0xff;
                    gs += (p >> 8) & 0xff;
                    bs += p & 0xff;
                    n++;
                }
            }
            if (!n || !as) continue;
            a = (float)(as / n) / 255.0f;
            /* the source is premultiplied; sw_blend wants straight colour */
            c.r = (uint8_t)sw_clampi((int)((float)(rs / n) / a + 0.5f), 0, 255);
            c.g = (uint8_t)sw_clampi((int)((float)(gs / n) / a + 0.5f), 0, 255);
            c.b = (uint8_t)sw_clampi((int)((float)(bs / n) / a + 0.5f), 0, 255);
            c.a = 255;
            if (tint.r != 255 || tint.g != 255 || tint.b != 255) {
                c.r = (uint8_t)(c.r * tint.r / 255);
                c.g = (uint8_t)(c.g * tint.g / 255);
                c.b = (uint8_t)(c.b * tint.b / 255);
            }
            sw_blend8(dst, dx, dy, c, SW_MUL((unsigned)(as / n), ga));
        }
    }
}

/* ------------------------------------------------------------------ PNG */

static uint32_t sw_crc32(const unsigned char *p, size_t n, uint32_t crc)
{
    static uint32_t tab[256];
    static int built = 0;
    size_t i;
    if (!built) {
        uint32_t k, c;
        int b;
        for (k = 0; k < 256; ++k) {
            c = k;
            for (b = 0; b < 8; ++b) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            tab[k] = c;
        }
        built = 1;
    }
    crc ^= 0xffffffffu;
    for (i = 0; i < n; ++i) crc = tab[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

static void sw_png_chunk(FILE *f, const char *tag, const unsigned char *d, size_t n)
{
    unsigned char hdr[8];
    uint32_t crc;
    unsigned char cb[4];
    hdr[0] = (unsigned char)(n >> 24); hdr[1] = (unsigned char)(n >> 16);
    hdr[2] = (unsigned char)(n >> 8);  hdr[3] = (unsigned char)n;
    memcpy(hdr + 4, tag, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    crc = sw_crc32((const unsigned char *)tag, 4, 0);
    if (n) crc = sw_crc32(d, n, crc);
    cb[0] = (unsigned char)(crc >> 24); cb[1] = (unsigned char)(crc >> 16);
    cb[2] = (unsigned char)(crc >> 8);  cb[3] = (unsigned char)crc;
    fwrite(cb, 1, 4, f);
}

bool sw_write_png(const char *path, const Canvas *cv)
{
    FILE *f;
    unsigned char ihdr[13];
    unsigned char *raw, *z;
    size_t rawn, zn, pos, off;
    uint32_t a = 1, b = 0;
    int y, x;
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

    if (!path || !cv || !cv->px) return false;
    rawn = ((size_t)cv->w * 4 + 1) * (size_t)cv->h;
    raw = (unsigned char *)malloc(rawn);
    if (!raw) return false;

    /* un-premultiply back to straight RGBA, one filter byte a row */
    pos = 0;
    for (y = 0; y < cv->h; ++y) {
        raw[pos++] = 0;
        for (x = 0; x < cv->w; ++x) {
            uint32_t p = cv->px[(size_t)y * (size_t)cv->w + x];
            unsigned av = (p >> 24) & 0xff;
            unsigned rv = (p >> 16) & 0xff, gv = (p >> 8) & 0xff, bv = p & 0xff;
            if (av && av < 255) {
                rv = rv * 255u / av; if (rv > 255) rv = 255;
                gv = gv * 255u / av; if (gv > 255) gv = 255;
                bv = bv * 255u / av; if (bv > 255) bv = 255;
            }
            raw[pos++] = (unsigned char)rv;
            raw[pos++] = (unsigned char)gv;
            raw[pos++] = (unsigned char)bv;
            raw[pos++] = (unsigned char)av;
        }
    }

    /* zlib stream, stored deflate blocks of at most 65535 bytes */
    zn = 2 + 4 + rawn + 5 * ((rawn + 65534) / 65535) + 1;
    z = (unsigned char *)malloc(zn);
    if (!z) { free(raw); return false; }
    pos = 0;
    z[pos++] = 0x78; z[pos++] = 0x01;
    for (off = 0; off < rawn; ) {
        size_t n = rawn - off > 65535 ? 65535 : rawn - off;
        z[pos++] = (off + n >= rawn) ? 1 : 0;
        z[pos++] = (unsigned char)n;         z[pos++] = (unsigned char)(n >> 8);
        z[pos++] = (unsigned char)(~n);      z[pos++] = (unsigned char)((~n) >> 8);
        memcpy(z + pos, raw + off, n);
        pos += n;
        off += n;
    }
    for (off = 0; off < rawn; ++off) {       /* adler32 */
        a = (a + raw[off]) % 65521u;
        b = (b + a) % 65521u;
    }
    z[pos++] = (unsigned char)(b >> 8); z[pos++] = (unsigned char)b;
    z[pos++] = (unsigned char)(a >> 8); z[pos++] = (unsigned char)a;
    free(raw);

    f = fopen(path, "wb");
    if (!f) { free(z); return false; }
    fwrite(sig, 1, 8, f);
    ihdr[0] = (unsigned char)((unsigned)cv->w >> 24);
    ihdr[1] = (unsigned char)((unsigned)cv->w >> 16);
    ihdr[2] = (unsigned char)((unsigned)cv->w >> 8);
    ihdr[3] = (unsigned char)cv->w;
    ihdr[4] = (unsigned char)((unsigned)cv->h >> 24);
    ihdr[5] = (unsigned char)((unsigned)cv->h >> 16);
    ihdr[6] = (unsigned char)((unsigned)cv->h >> 8);
    ihdr[7] = (unsigned char)cv->h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    sw_png_chunk(f, "IHDR", ihdr, 13);
    sw_png_chunk(f, "IDAT", z, pos);
    sw_png_chunk(f, "IEND", NULL, 0);
    free(z);
    fclose(f);
    return true;
}

#endif /* SW_DRAW_IMPL_ONCE */
#endif /* SW_DRAW_IMPL */
