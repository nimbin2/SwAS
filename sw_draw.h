/* sw_draw.h — a software canvas. ARGB8888, premultiplied, which is what
 * wl_shm wants. Rounded rectangles come out of a signed distance field, text
 * out of stb_truetype with a per-size glyph cache, images out of a box filter.
 *
 * No GPU and no windowing system: a Canvas is a pixel buffer and a clip
 * rectangle, so a whole frame can be drawn and then inspected in a test with
 * no compositor anywhere near it. That is the point of doing it this way, as
 * much as the short link line.
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

static inline void sw_blend(Canvas *cv, int x, int y, Col c, float cov)
{
    if (x < cv->cx0 || x >= cv->cx1 || y < cv->cy0 || y >= cv->cy1) return;
    float a = (float)c.a / 255.0f * cov;
    if (a <= 0.0f) return;
    uint32_t d = cv->px[y * cv->w + x];
    float da = (float)((d >> 24) & 0xff) / 255.0f;
    float dr = (float)((d >> 16) & 0xff) / 255.0f;
    float dg = (float)((d >> 8) & 0xff) / 255.0f;
    float db = (float)(d & 0xff) / 255.0f;
    float sr = (float)c.r / 255.0f * a;
    float sg = (float)c.g / 255.0f * a;
    float sb = (float)c.b / 255.0f * a;
    float ia = 1.0f - a;
    float orr = sr + dr * ia, og = sg + dg * ia, ob = sb + db * ia, oa = a + da * ia;
    cv->px[y * cv->w + x] =
        ((uint32_t)(orr * 255.0f + 0.5f) << 16) |
        ((uint32_t)(og  * 255.0f + 0.5f) << 8)  |
        ((uint32_t)(ob  * 255.0f + 0.5f))       |
        ((uint32_t)(oa  * 255.0f + 0.5f) << 24);
}

void sw_fill_rect(Canvas *cv, float x, float y, float w, float h, Col c)
{
    if (w <= 0 || h <= 0 || !c.a) return;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    int x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            float cxs = sw_clampf(x + w, (float)xx, (float)xx + 1) - sw_clampf(x, (float)xx, (float)xx + 1);
            float cys = sw_clampf(y + h, (float)yy, (float)yy + 1) - sw_clampf(y, (float)yy, (float)yy + 1);
            sw_blend(cv, xx, yy, c, cxs * cys);
        }
}

/* Rounded box with a radius per corner (tl, tr, br, bl), anti-aliased through
 * the usual signed distance trick. A corner radius of 0 is a square corner —
 * that is how the bar stays flush with the screen edge. */
void sw_fill_round(Canvas *cv, float x, float y, float w, float h,
                       float rtl, float rtr, float rbr, float rbl, Col c)
{
    if (w <= 0 || h <= 0 || !c.a) return;
    float hw = w * 0.5f, hh = h * 0.5f;
    float ccx = x + hw, ccy = y + hh;
    float cap = fminf(hw, hh);
    rtl = sw_clampf(rtl, 0, cap); rtr = sw_clampf(rtr, 0, cap);
    rbr = sw_clampf(rbr, 0, cap); rbl = sw_clampf(rbl, 0, cap);

    int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1;
    int x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
    for (int yy = y0; yy < y1; ++yy) {
        if (yy < cv->cy0 || yy >= cv->cy1) continue;
        for (int xx = x0; xx < x1; ++xx) {
            if (xx < cv->cx0 || xx >= cv->cx1) continue;
            float px = (float)xx + 0.5f - ccx, py = (float)yy + 0.5f - ccy;
            float r = px < 0 ? (py < 0 ? rtl : rbl) : (py < 0 ? rtr : rbr);
            float qx = fabsf(px) - (hw - r), qy = fabsf(py) - (hh - r);
            float mx = fmaxf(qx, 0.0f), my = fmaxf(qy, 0.0f);
            float d = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0.0f) - r;
            float cov = sw_clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0) sw_blend(cv, xx, yy, c, cov);
        }
    }
}

/* The same distance field kept to a ring: an outline that follows the corner
 * radius and stays inside the box, so a button with no fill still has an edge. */
void sw_stroke_round(Canvas *cv, float x, float y, float w, float h,
                         float rtl, float rtr, float rbr, float rbl,
                         float t, Col c)
{
    if (w <= 0 || h <= 0 || t <= 0 || !c.a) return;
    float hw = w * 0.5f, hh = h * 0.5f;
    float ccx = x + hw, ccy = y + hh;
    float cap = fminf(hw, hh);
    rtl = sw_clampf(rtl, 0, cap); rtr = sw_clampf(rtr, 0, cap);
    rbr = sw_clampf(rbr, 0, cap); rbl = sw_clampf(rbl, 0, cap);

    int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1;
    int x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
    for (int yy = y0; yy < y1; ++yy) {
        if (yy < cv->cy0 || yy >= cv->cy1) continue;
        for (int xx = x0; xx < x1; ++xx) {
            if (xx < cv->cx0 || xx >= cv->cx1) continue;
            float px = (float)xx + 0.5f - ccx, py = (float)yy + 0.5f - ccy;
            float r = px < 0 ? (py < 0 ? rtl : rbl) : (py < 0 ? rtr : rbr);
            float qx = fabsf(px) - (hw - r), qy = fabsf(py) - (hh - r);
            float mx = fmaxf(qx, 0.0f), my = fmaxf(qy, 0.0f);
            float d = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0.0f) - r;
            float cov = sw_clampf(0.5f - fabsf(d + t * 0.5f) + t * 0.5f, 0.0f, 1.0f);
            if (cov > 0) sw_blend(cv, xx, yy, c, cov);
        }
    }
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
                for (int yy = 0; yy < g->h; ++yy) {
                    int ty = gy + yy;
                    if (ty < cv->cy0 || ty >= cv->cy1) continue;
                    const unsigned char *row = g->bmp + (size_t)yy * (size_t)g->w;
                    for (int xx = 0; xx < g->w; ++xx) {
                        unsigned char a = row[xx];
                        if (a) sw_blend(cv, gx + pass + xx, ty, c, (float)a / 255.0f);
                    }
                }
            }
        }
        pen += g->adv + (f->synth ? 0.35f : 0.0f);
    }
}


void sw_canvas_fill(Canvas *cv, Col c)
{
    sw_fill_rect(cv, (float)cv->cx0, (float)cv->cy0,
                 (float)(cv->cx1 - cv->cx0), (float)(cv->cy1 - cv->cy0), c);
}

/* A filled triangle, by half-plane tests on a 2x2 grid per pixel. swbr never
 * needed one; swas draws its wheel slices and its chevrons out of them. */
void sw_fill_tri(Canvas *cv, float x0, float y0, float x1, float y1,
                 float x2, float y2, Col c)
{
    if (!c.a) return;
    float minx = fminf(x0, fminf(x1, x2)), maxx = fmaxf(x0, fmaxf(x1, x2));
    float miny = fminf(y0, fminf(y1, y2)), maxy = fmaxf(y0, fmaxf(y1, y2));
    int ix0 = sw_clampi((int)floorf(minx), cv->cx0, cv->cx1);
    int iy0 = sw_clampi((int)floorf(miny), cv->cy0, cv->cy1);
    int ix1 = sw_clampi((int)ceilf(maxx) + 1, cv->cx0, cv->cx1);
    int iy1 = sw_clampi((int)ceilf(maxy) + 1, cv->cy0, cv->cy1);

    float d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabsf(d) < 1e-6f) return;              /* degenerate */
    float inv = 1.0f / d;

    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    float px = (float)x + 0.25f + 0.5f * (float)sx;
                    float py = (float)y + 0.25f + 0.5f * (float)sy;
                    float l0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
                    float l1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
                    float l2 = 1.0f - l0 - l1;
                    if (l0 >= 0.0f && l1 >= 0.0f && l2 >= 0.0f) hits++;
                }
            }
            if (hits) sw_blend(cv, x, y, c, (float)hits * 0.25f);
        }
    }
}

void sw_fill_circle(Canvas *cv, float cx, float cy, float rad, Col c)
{
    sw_fill_round(cv, cx - rad, cy - rad, rad * 2.0f, rad * 2.0f,
                  rad, rad, rad, rad, c);
}

/* A wedge of an annulus: the slice shape a radial menu is made of. Tested
 * per pixel against the two radii and the two angles, four samples a pixel so
 * the long curved edges do not stair-step. A0 may exceed a1 going the other
 * way round; the wedge is always drawn the short way from a0 to a1. */
void sw_fill_sector(Canvas *cv, float cx, float cy, float r0, float r1,
                    float a0, float a1, Col c)
{
    if (!c.a || r1 <= 0.0f || r1 <= r0) return;
    int ix0 = sw_clampi((int)floorf(cx - r1) - 1, cv->cx0, cv->cx1);
    int iy0 = sw_clampi((int)floorf(cy - r1) - 1, cv->cy0, cv->cy1);
    int ix1 = sw_clampi((int)ceilf(cx + r1) + 1, cv->cx0, cv->cx1);
    int iy1 = sw_clampi((int)ceilf(cy + r1) + 1, cv->cy0, cv->cy1);

    float span = a1 - a0;
    while (span < 0.0f)               span += 6.283185307f;
    while (span > 6.283185307f)       span -= 6.283185307f;
    float r0s = r0 > 0.0f ? r0 * r0 : -1.0f, r1s = r1 * r1;

    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    float dx = (float)x + 0.25f + 0.5f * (float)sx - cx;
                    float dy = (float)y + 0.25f + 0.5f * (float)sy - cy;
                    float d2 = dx * dx + dy * dy;
                    if (d2 < r0s || d2 > r1s) continue;
                    float da = atan2f(dy, dx) - a0;
                    while (da < 0.0f)         da += 6.283185307f;
                    while (da > 6.283185307f) da -= 6.283185307f;
                    if (da <= span) hits++;
                }
            }
            if (hits) sw_blend(cv, x, y, c, (float)hits * 0.25f);
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
            Col c = { (uint8_t)(rs / n), (uint8_t)(gs / n), (uint8_t)(bs / n), 255 };
            sw_blend(cv, dx, dy, c, (float)(as / n) / 255.0f * alpha);
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
    if (!dst || !src || !src->px || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;

    ix0 = sw_clampi((int)floorf(x),     dst->cx0, dst->cx1);
    iy0 = sw_clampi((int)floorf(y),     dst->cy0, dst->cy1);
    ix1 = sw_clampi((int)ceilf(x + w),  dst->cx0, dst->cx1);
    iy1 = sw_clampi((int)ceilf(y + h),  dst->cy0, dst->cy1);

    /* 1:1 and aligned: a straight premultiplied-over copy, no resampling */
    if (tint.r == 255 && tint.g == 255 && tint.b == 255 &&
        (int)w == src->w && (int)h == src->h &&
        x == floorf(x) && y == floorf(y)) {
        int ox = (int)x, oy = (int)y;
        for (dy = iy0; dy < iy1; ++dy) {
            const uint32_t *srow;
            int sy1 = dy - oy;
            if (sy1 < 0 || sy1 >= src->h) continue;
            srow = src->px + (size_t)sy1 * (size_t)src->w;
            for (dx = ix0; dx < ix1; ++dx) {
                uint32_t p;
                unsigned av;
                Col c;
                int sx1 = dx - ox;
                if (sx1 < 0 || sx1 >= src->w) continue;
                p = srow[sx1];
                av = (p >> 24) & 0xff;
                if (!av) continue;
                c.r = (uint8_t)sw_clampi((int)((p >> 16 & 0xff) * 255 / av), 0, 255);
                c.g = (uint8_t)sw_clampi((int)((p >> 8 & 0xff) * 255 / av), 0, 255);
                c.b = (uint8_t)sw_clampi((int)((p & 0xff) * 255 / av), 0, 255);
                c.a = 255;
                sw_blend(dst, dx, dy, c, (float)av / 255.0f * alpha);
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
            sw_blend(dst, dx, dy, c, a * alpha);
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
