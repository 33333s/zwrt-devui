/*
 * html_view.cpp - render an HTML/CSS page to the RGB565 framebuffer using
 * litehtml (layout/CSS) + FreeType (text). devui becomes a thin HTML shell:
 * the UI is authored in ui/index.html, this draws it.
 *
 * No JavaScript; CSS grid is unsupported by litehtml (falls back to block).
 *
 * SPDX-License-Identifier: MIT
 */
#include <litehtml.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <sys/stat.h>
#include <unordered_map>
#include <atomic>
#include <pthread.h>

#include "livewp.h"

using namespace litehtml;

/* ---- target framebuffer (shared with drm_disp, 180° rotated panel) ---- */
static uint16_t *g_fb;
static int g_w, g_h, g_pitch_px, g_rotate = 1;
static int g_clip_top = 0;

/* How much of the wallpaper (kw) and of glass box gi (kg) shows through each
 * logical pixel of the target, 0..255, kept while a wallpaper is in use: a
 * moving frame swaps that share for the ground where the pixel lands (see
 * html_view_bg_fix_row). kp: how much of that wallpaper is the softened one
 * behind bare text (blur_pad). NULL when the target keeps none. */
struct bg_px { uint8_t kw, kg, gi, kp; };
static bg_px *g_bg;
static const uint16_t *g_bg_fb;
static int g_bg_h = -1;
static void bg_bind(void);
static inline void bg_unbind(void) { g_bg_fb = nullptr; }

static inline bg_px *bg_at(int x, int y)
{
    if (g_fb != g_bg_fb || g_h != g_bg_h) bg_bind();
    if (!g_bg || x < 0 || y < 0 || x >= g_w || y >= g_h) return nullptr;
    return &g_bg[(size_t)y * g_w + x];
}

/* Paint at alpha a over (x, y): the ground keeps 255 - a of its share. */
static inline void bg_cover(int x, int y, int a)
{
    bg_px *q = bg_at(x, y);
    if (!q) return;
    if (a >= 255) { q->kw = q->kg = 0; return; }
    q->kw = (uint8_t)((q->kw * (255 - a) + 127) / 255);
    q->kg = (uint8_t)((q->kg * (255 - a) + 127) / 255);
}

static inline void bg_set_span(int x1, int x2, int y, bg_px v)
{
    for (int x = x1; x < x2; x++)
        if (bg_px *q = bg_at(x, y)) *q = v;
}

static inline void put_px(int x, int y, int r, int g, int b, int a)
{
    if (y < g_clip_top) return;
    if (x < 0 || y < 0 || x >= g_w || y >= g_h || a <= 0) return;
    bg_cover(x, y, a);
    int dx = g_rotate ? (g_w - 1 - x) : x;
    int dy = g_rotate ? (g_h - 1 - y) : y;
    uint16_t *p = &g_fb[dy * g_pitch_px + dx];
    if (a < 255) {
        uint16_t o = *p;
        int orr = ((o >> 11) & 0x1F) << 3, og = ((o >> 5) & 0x3F) << 2, ob = (o & 0x1F) << 3;
        r = (r * a + orr * (255 - a)) / 255;
        g = (g * a + og * (255 - a)) / 255;
        b = (b * a + ob * (255 - a)) / 255;
    }
    *p = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* put_px with 4x4 ordered dithering: a smooth gradient quantized straight to
 * RGB565 (5/6/5 bits) shows bands; spreading the rounding error over a fixed
 * pattern hides them. */
static const uint8_t k_bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static inline void put_px_dither(int x, int y, int r, int g, int b, int a)
{
    if (y < g_clip_top) return;
    if (x < 0 || y < 0 || x >= g_w || y >= g_h || a <= 0) return;
    bg_cover(x, y, a);
    int dx = g_rotate ? (g_w - 1 - x) : x;
    int dy = g_rotate ? (g_h - 1 - y) : y;
    uint16_t *p = &g_fb[dy * g_pitch_px + dx];
    if (a < 255) {
        uint16_t o = *p;
        int orr = ((o >> 11) & 0x1F) << 3, og = ((o >> 5) & 0x3F) << 2, ob = (o & 0x1F) << 3;
        r = (r * a + orr * (255 - a)) / 255;
        g = (g * a + og * (255 - a)) / 255;
        b = (b * a + ob * (255 - a)) / 255;
    }
    int t = k_bayer4[y & 3][x & 3];          /* 0..15: threshold within one quantization step */
    r += t >> 1; g += t >> 2; b += t >> 1;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *p = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* The opaque RGB565 pixel put_px_dither writes at (x, y). */
static inline uint16_t dither565(int x, int y, int r, int g, int b)
{
    int t = k_bayer4[y & 3][x & 3];
    r += t >> 1; g += t >> 2; b += t >> 1;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t get_px565(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_w || y >= g_h || !g_fb) return 0;
    int dx = g_rotate ? (g_w - 1 - x) : x;
    int dy = g_rotate ? (g_h - 1 - y) : y;
    return g_fb[dy * g_pitch_px + dx];
}

/* Is (x,y) inside a rounded rectangle? Used by both rounded fill and rounded
 * border stroking (litehtml hands us radii but leaves the drawing to us). */
/* Coverage (0..1) of pixel (x, y) by a rounded rect [fx, fx+fw) x [fy, fy+fh)
 * with per-corner radii, from the signed distance of the pixel centre to the
 * edge, so curved edges blend over about one pixel instead of stair-stepping.
 * Straight edges on whole pixels still come out as exactly 0 or 1. */
static float round_cov(int x, int y, int fx, int fy, int fw, int fh,
                       int rtl, int rtr, int rbr, int rbl)
{
    if (fw <= 0 || fh <= 0 || x < fx || y < fy || x >= fx + fw || y >= fy + fh) return 0.f;
    float hw = fw * .5f, hh = fh * .5f;
    float px = (float)x + .5f - ((float)fx + hw), py = (float)y + .5f - ((float)fy + hh);
    float r = (float)(py < 0.f ? (px < 0.f ? rtl : rtr) : (px < 0.f ? rbl : rbr));
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float qx = std::fabs(px) - (hw - r), qy = std::fabs(py) - (hh - r);
    /* only the corner squares need the distance to an arc */
    float d = qx > 0.f && qy > 0.f ? std::sqrt(qx * qx + qy * qy) - r : std::max(qx, qy) - r;
    float c = .5f - d;
    return c <= 0.f ? 0.f : c >= 1.f ? 1.f : c;
}

static inline int cov_alpha(float cov, int a) { return (int)(cov * (float)a + .5f); }
static inline float clampf01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

/* ---- FreeType ---- */
static FT_Library g_ft;
static FT_Face    g_face;

struct ft_font { int size; int ascent, descent, height; };

/* Rendered glyphs by (pixel size, code point). Loading and rasterizing a glyph
 * through FreeType costs far more than drawing it, and a page uses the same few
 * dozen glyphs on every refresh. */
struct glyph {
    int adv = 0, left = 0, top = 0, w = 0, h = 0;
    std::vector<uint8_t> px;   /* w*h coverage */
};
static std::unordered_map<uint32_t, glyph> g_glyphs;
static int g_ascent[256];      /* ascender per pixel size, 0 = not looked up */

static const glyph &glyph_get(unsigned cp, int size)
{
    uint32_t key = ((uint32_t)(size & 0x7FF) << 21) | (cp & 0x1FFFFF);
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second;
    if (g_glyphs.size() >= 4096) g_glyphs.clear();
    glyph gl;
    FT_Set_Pixel_Sizes(g_face, 0, size);
    if (!FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) {
        FT_GlyphSlot s = g_face->glyph;
        gl.adv = (int)(s->advance.x >> 6);
        gl.left = s->bitmap_left; gl.top = s->bitmap_top;
        gl.w = (int)s->bitmap.width; gl.h = (int)s->bitmap.rows;
        gl.px.resize((size_t)gl.w * gl.h);
        for (int r = 0; r < gl.h; r++)
            memcpy(&gl.px[(size_t)r * gl.w], s->bitmap.buffer + r * s->bitmap.pitch, (size_t)gl.w);
    }
    return g_glyphs.emplace(key, std::move(gl)).first->second;
}

static int font_ascent(int size)
{
    if (size > 0 && size < 256 && g_ascent[size]) return g_ascent[size];
    FT_Set_Pixel_Sizes(g_face, 0, size);
    int a = (int)(g_face->size->metrics.ascender >> 6);
    if (size > 0 && size < 256) g_ascent[size] = a;
    return a;
}

/* UI base dir (for <link> CSS / images) and last-clicked anchor href. */
static std::string g_ui_dir = "/data/plugins/zwrt-devui/ui";
static std::string g_clicked;

static long long css_file_mtime_ns(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
    return (long long)st.st_mtime * 1000000000LL + (long long)st.st_mtim.tv_nsec;
}

struct css_cache_entry {
    std::string path;
    std::string text;
    long long mtime = -1;
    bool used = false;
};

static css_cache_entry g_css_cache[4];
static int g_css_cache_next;

static int read_file_text(const char *path, std::string &out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long n;
    int ok = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        n = ftell(f);
        if (n >= 0) {
            if (n > 0) {
                out.resize((size_t)n);
                fseek(f, 0, SEEK_SET);
                ok = (fread(&out[0], 1, (size_t)n, f) == (size_t)n);
            } else {
                out.clear();
                ok = 1;
            }
        }
    }
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

static void css_get_cached(std::string &out, const std::string &path)
{
    long long mtime = css_file_mtime_ns(path);
    if (mtime < 0) return;

    for (int i = 0; i < 4; i++) {
        css_cache_entry &e = g_css_cache[i];
        if (!e.used || e.path != path) continue;
        if (e.mtime == mtime && !e.text.empty()) {
            out = e.text;
            return;
        }
        if (read_file_text(path.c_str(), e.text)) {
            e.mtime = mtime;
            out = e.text;
            return;
        }
        out = e.text;
        return;
    }

    css_cache_entry *e = nullptr;
    for (int i = 0; i < 4; i++) {
        if (!g_css_cache[i].used) { e = &g_css_cache[i]; break; }
    }
    if (!e) {
        e = &g_css_cache[g_css_cache_next];
        g_css_cache_next = (g_css_cache_next + 1) % 4;
    }
    std::string next;
    if (!read_file_text(path.c_str(), next)) return;
    e->path = path;
    e->text = std::move(next);
    e->mtime = mtime;
    e->used = true;
    out = e->text;
}

static unsigned utf8_next(const char *&s)
{
    unsigned c = (unsigned char)*s++;
    if (c < 0x80) return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
    c &= (0x3F >> n);
    while (n-- && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F);
    return c;
}

static int text_advance(const char *text, int size)
{
    int w = 0;
    for (const char *s = text; *s; ) w += glyph_get(utf8_next(s), size).adv;
    return w;
}

/* litehtml lays out in fractional pixels. Every draw path snaps an edge with
 * the same rounding, so a box, its text and a native icon placed over it move
 * together instead of truncating to different rows. */
static inline int R(pixel_t v) { return (int)floorf(v + 0.5f); }

/* ---- wallpaper and liquid glass ----
 * The wallpaper is cover-scaled to the 320x480 page once and kept twice: dimmed
 * RGB565 for the page background and a blurred RGB copy that glass boxes show
 * through. It is pinned to the screen: whatever the scroll, buffer row y shows
 * wallpaper row y (mirrored past one screen in the tall scroll buffer, where
 * the host swaps it for the screen row while blitting; see htmlmain.c).
 * CSS asks for it with url(wallpaper) and for a glass box
 * with url(glass) (see style.css). */
extern "C" unsigned char *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp);
extern "C" void stbi_image_free(void *retval_from_stbi_load);

enum { WP_W = 320, WP_H = 480 };
static std::vector<uint16_t> g_wp;      /* WP_W*WP_H dimmed, dithered */
static std::vector<uint8_t>  g_wp_rgb;  /* WP_W*WP_H*3 dimmed, sharp: the source of the blur */
static std::vector<uint8_t>  g_wp_pad;  /* WP_W*WP_H*3 dimmed, softened a little: behind bare text */
static std::vector<uint8_t>  g_wp_soft; /* WP_W*WP_H*3 dimmed, blurred */
static std::vector<uint8_t>  g_wp_blur; /* g_wp_soft as glass shows it (glass_tone) */
static std::vector<uint8_t>  g_wp_thin; /* the same for url(glass-thin) */
enum { GLASS_THIN = 35 };                /* url(glass-thin): percent less pane */
static int  g_glass_clear = 50;         /* glass transparency, 0 (solid) .. 100 (clear) */
static int  g_glass_blur = 50;          /* glass blur, 0 (none) .. 100 (about 14 px) */
static bool g_wp_drawn;                 /* the current page painted the wallpaper */
static int  g_tint_r = 22, g_tint_g = 30, g_tint_b = 44;   /* glass pane colour */
static int  g_wp_keep = 70;             /* percent of the picture's brightness kept (100 - dim) */

/* A live wallpaper plays over a page drawn over the wallpaper above (see
 * the live wallpaper section further down): the frame on screen is *g_lv,
 * which a moving frame and the playing clip reground the page onto, until
 * html_view_live_freeze makes it the wallpaper. */
struct wp_set {
    std::vector<uint8_t> src;    /* the frame as the clip has it */
    std::vector<uint16_t> px;    /* and as g_wp, g_wp_rgb ... are the wallpaper */
    std::vector<uint8_t> rgb, pad, soft, blur, thin;
    int frame = 0;
    bool whole = true;           /* rgb and pad kept up too (the screen needs px, blur and thin; soft is not kept) */
};
static wp_set g_lsets[2];
static wp_set *g_lv = &g_lsets[0], *g_lq = &g_lsets[1];   /* the frame on screen; the next one, ready */
static bool g_lv_on;             /* the frame on screen may differ from the wallpaper */
/* The softened wallpaper (pad) is worked out only while a page shown has
 * text bare on the wallpaper; until then it falls behind (not ok). */
static std::atomic<bool> g_live_pads{false};
static bool g_lw_pad_ok;

/* Wallpaper row for page row y: rows past the image mirror back. */
static inline int wp_row(int y)
{
    y %= 2 * WP_H;
    if (y < 0) y += 2 * WP_H;
    return y < WP_H ? y : 2 * WP_H - 1 - y;
}

/* One running-sum box pass of radius r along each of the n rows of img
 * (WP_W RGB pixels each; edge pixels repeat). */
static void box_rows(uint8_t *img, int n, int r)
{
    if (r <= 0) return;
    const uint32_t win = 2 * r + 1, inv = (65536u + win - 1) / win;   /* s / win as (s * inv) >> 16 */
    uint8_t tmp[WP_W * 3];
    for (int y = 0; y < n; y++) {
        uint8_t *p = img + (size_t)y * WP_W * 3;
        uint32_t s0 = 0, s1 = 0, s2 = 0;
        for (int i = -r; i <= r; i++) {
            const uint8_t *q = p + std::min(std::max(i, 0), WP_W - 1) * 3;
            s0 += q[0]; s1 += q[1]; s2 += q[2];
        }
        for (int i = 0; i < WP_W; i++) {
            uint8_t *t = tmp + i * 3;
            t[0] = (uint8_t)((s0 * inv + 32768) >> 16); t[1] = (uint8_t)((s1 * inv + 32768) >> 16); t[2] = (uint8_t)((s2 * inv + 32768) >> 16);
            const uint8_t *in = p + std::min(i + r + 1, WP_W - 1) * 3, *out = p + std::max(i - r, 0) * 3;
            s0 += in[0] - out[0]; s1 += in[1] - out[1]; s2 += in[2] - out[2];
        }
        memcpy(p, tmp, sizeof tmp);
    }
}

/* The same pass down each column, n rows of src into dst: a row at a time,
 * so it streams through memory. */
static void box_cols(const uint8_t *src, uint8_t *dst, int n, int r)
{
    const int N = WP_W * 3;
    const uint32_t win = 2 * r + 1, inv = (65536u + win - 1) / win;
    uint32_t acc[WP_W * 3] = {};
    for (int i = -r; i <= r; i++) {
        const uint8_t *q = src + (size_t)std::min(std::max(i, 0), n - 1) * N;
        for (int c = 0; c < N; c++) acc[c] += q[c];
    }
    for (int y = 0; y < n; y++) {
        uint8_t *o = dst + (size_t)y * N;
        for (int c = 0; c < N; c++) o[c] = (uint8_t)((acc[c] * inv + 32768) >> 16);
        const uint8_t *in = src + (size_t)std::min(y + r + 1, n - 1) * N, *out = src + (size_t)std::max(y - r, 0) * N;
        for (int c = 0; c < N; c++) acc[c] += in[c] - out[c];
    }
}

/* Blur src into dst where rows [y0, y1) of src changed: three box passes
 * each way approximate a Gaussian, their radii adding up to total, so dst
 * changes within total rows of [y0, y1). Those come out exactly as blurring
 * the whole picture makes them: a pass reads within its radius, so working
 * on the rows within 2 * total is enough. t1, t2: scratch. */
static void blur_rows(const std::vector<uint8_t> &src, std::vector<uint8_t> &dst, int total, int y0, int y1,
                      std::vector<uint8_t> &t1, std::vector<uint8_t> &t2)
{
    const size_t N = (size_t)WP_W * 3;
    dst.resize(src.size());
    if (src.empty()) return;
    const int o0 = std::max(0, y0 - total), o1 = std::min((int)WP_H, y1 + total);
    if (o0 >= o1) return;
    if (total <= 0) { memcpy(&dst[o0 * N], &src[o0 * N], (o1 - o0) * N); return; }
    const int b0 = std::max(0, y0 - 2 * total), b1 = std::min((int)WP_H, y1 + 2 * total), n = b1 - b0;
    t1.resize(n * N);
    t2.resize(n * N);
    memcpy(t1.data(), &src[b0 * N], n * N);
    for (int pass = 0; pass < 3; pass++) {
        int r = total / 3 + (pass < total % 3 ? 1 : 0);
        if (r <= 0) continue;
        box_rows(t1.data(), n, r);
        box_cols(t1.data(), t2.data(), n, r);
        t1.swap(t2);
    }
    memcpy(&dst[o0 * N], &t1[(o0 - b0) * N], (o1 - o0) * N);
}

static void glass_tone(void);
static std::vector<uint8_t> g_blur_t1, g_blur_t2;   /* scratch for the wallpaper (the clip worker has its own) */
enum { PAD_BLUR = 3 };                               /* g_wp_pad: three passes of one pixel */

/* The blur setting spreads 0..42 px of radius over the three passes, so it
 * changes in fine steps (50 is 7 px a pass, about 12 px). */
static inline int blur_total(void) { return g_glass_blur * 42 / 100; }

/* Blur the sharp wallpaper into g_wp_soft, and the glass backdrops from it. */
static void wp_reblur(void)
{
    blur_rows(g_wp_rgb, g_wp_soft, blur_total(), 0, WP_H, g_blur_t1, g_blur_t2);
    glass_tone();
}

/* The rest of the wallpaper from its dimmed picture g_wp_rgb. */
static void wp_derive(void)
{
    g_wp.resize((size_t)WP_W * WP_H);
    for (int y = 0; y < WP_H; y++)
        for (int x = 0; x < WP_W; x++) {
            const uint8_t *p = &g_wp_rgb[((size_t)y * WP_W + x) * 3];
            g_wp[(size_t)y * WP_W + x] = dither565(x, y, p[0], p[1], p[2]);
        }
    blur_rows(g_wp_rgb, g_wp_pad, PAD_BLUR, 0, WP_H, g_blur_t1, g_blur_t2);
    wp_reblur();
}

static bool live_load(const char *path);
static void live_drop(void);

/* Load (or with a NULL path, drop) the wallpaper: a picture (any format
 * stb_image reads) or a live wallpaper clip, which shows its first frame
 * until it plays. dim: 0..90 percent darker; blur: 0..100 (see blur_total).
 * Returns 1 when a wallpaper is in use. */
extern "C" int html_view_load_wallpaper(const char *path, int dim, int blur)
{
    live_drop();
    g_wp.clear();
    g_wp_rgb.clear();
    g_wp_pad.clear();
    g_wp_soft.clear();
    g_wp_blur.clear();
    g_wp_thin.clear();
    g_glass_blur = std::min(std::max(blur, 0), 100);
    g_wp_keep = 100 - std::min(std::max(dim, 0), 90);
    const int keep = g_wp_keep;
    if (!path) return 0;
    if (live_load(path)) {
        g_wp_rgb.resize(g_lv->src.size());
        for (size_t i = 0; i < g_lv->src.size(); i++) g_wp_rgb[i] = (uint8_t)(g_lv->src[i] * keep / 100);
        wp_derive();
        return 1;
    }
    int iw = 0, ih = 0, comp = 0;
    unsigned char *img = stbi_load(path, &iw, &ih, &comp, 3);
    if (!img) {
        fprintf(stderr, "html_view: cannot load wallpaper %s\n", path);
        return 0;
    }
    /* cover: scale so both sides fill the page, crop the overflow evenly */
    float s = std::max((float)WP_W / (float)iw, (float)WP_H / (float)ih);
    float ox = ((float)iw - (float)WP_W / s) * .5f, oy = ((float)ih - (float)WP_H / s) * .5f;
    std::vector<uint8_t> rgb((size_t)WP_W * WP_H * 3);
    for (int y = 0; y < WP_H; y++) {
        int sy0 = (int)(oy + (float)y / s), sy1 = std::max(sy0 + 1, (int)(oy + (float)(y + 1) / s));
        sy0 = std::min(std::max(sy0, 0), ih - 1);
        sy1 = std::min(sy1, ih);
        for (int x = 0; x < WP_W; x++) {
            /* average the source pixels under this one (one of them when enlarging) */
            int sx0 = (int)(ox + (float)x / s), sx1 = std::max(sx0 + 1, (int)(ox + (float)(x + 1) / s));
            sx0 = std::min(std::max(sx0, 0), iw - 1);
            sx1 = std::min(sx1, iw);
            unsigned acc[3] = { 0, 0, 0 }, n = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++, n++) {
                    const unsigned char *p = img + ((size_t)yy * iw + xx) * 3;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
                }
            uint8_t *o = &rgb[((size_t)y * WP_W + x) * 3];
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(acc[c] / (n ? n : 1) * keep / 100);
        }
    }
    stbi_image_free(img);
    g_wp_rgb = std::move(rgb);
    wp_derive();
    return 1;
}

extern "C" void html_view_live_freeze(void);

/* Change the glass blur (the glass page's slider): re-blurs the wallpaper. */
extern "C" void html_view_set_glass_blur(int blur)
{
    blur = std::min(std::max(blur, 0), 100);
    if (blur == g_glass_blur) return;
    html_view_live_freeze();
    g_glass_blur = blur;
    wp_reblur();
}

extern "C" int  html_view_wallpaper_on(void) { return !g_wp.empty(); }
extern "C" void html_view_set_glass(int clear)
{
    clear = std::min(std::max(clear, 0), 100);
    if (clear == g_glass_clear) return;
    html_view_live_freeze();
    g_glass_clear = clear;
    glass_tone();
}

extern "C" void html_view_set_glass_tint(int r, int g, int b)
{
    if (r == g_tint_r && g == g_tint_g && b == g_tint_b) return;
    html_view_live_freeze();
    g_tint_r = r; g_tint_g = g; g_tint_b = b;
    glass_tone();
}

/* Wallpaper row as drawn into buffer row y (rows past the image mirror), or
 * NULL without a wallpaper: the host compares moving frames against it. */
extern "C" const uint16_t *html_view_wallpaper_row(int y)
{
    return g_wp.empty() ? nullptr : &g_wp[(size_t)wp_row(y) * WP_W];
}

/* Distance of the centre of pixel (x, y) inside a rounded rect (negative
 * outside) and the outward normal of the nearest edge. */
static float round_dist(int x, int y, int fx, int fy, int fw, int fh,
                        int rtl, int rtr, int rbr, int rbl, float *nx, float *ny)
{
    float hw = fw * .5f, hh = fh * .5f;
    float px = (float)x + .5f - ((float)fx + hw), py = (float)y + .5f - ((float)fy + hh);
    float r = (float)(py < 0.f ? (px < 0.f ? rtl : rtr) : (px < 0.f ? rbl : rbr));
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float sx = px < 0.f ? -1.f : 1.f, sy = py < 0.f ? -1.f : 1.f;
    float qx = std::fabs(px) - (hw - r), qy = std::fabs(py) - (hh - r);
    if (qx > 0.f && qy > 0.f) {
        float l = std::sqrt(qx * qx + qy * qy);
        *nx = sx * qx / l; *ny = sy * qy / l;
        return r - l;
    }
    if (qx > qy) { *nx = sx; *ny = 0.f; return r - qx; }
    *nx = 0.f; *ny = sy;
    return r - qy;
}

/* A glass box: its border box and corner radii. */
struct glass_geo { int bx, by, bw, bh, r[4]; int thin; };   /* thin: percent less pane (url(glass-thin)) */

/* Everything about glass pixel (x, y) of a box that does not depend on what
 * lies behind it: coverage, the lens offset of the backdrop sample, and the
 * shading as out = (backdrop * keep + c) >> 8 per channel (the pane colour by
 * the opacity, a faint frost so even clear glass reads as a surface, a rim lit
 * from above and a sheen over the top third). Integer shading and a single
 * compiled copy make drawing and html_view_bg_fix_row agree to the bit. */
struct glass_pix { int cov, lx, ly, keep, cr, cg, cb; };

__attribute__((noinline)) static bool glass_at(const glass_geo &G, int x, int y, glass_pix &o)
{
    float nx, ny, d = round_dist(x, y, G.bx, G.by, G.bw, G.bh, G.r[0], G.r[1], G.r[2], G.r[3], &nx, &ny);
    float cov = clampf01(d + .5f);
    if (cov <= 0.f) return false;
    const float lens = std::min(10.f, std::min(G.bw, G.bh) * .35f);
    const float pane = (float)(100 - g_glass_clear) / 100.f * (float)(100 - G.thin) / 100.f;
    float k = d < lens ? (1.f - d / lens) : 0.f;
    float rim = clampf01(1.5f - d) * (.10f + .32f * std::max(0.f, -ny) + .06f * std::max(0.f, -nx));
    float top = (float)(y - G.by) / ((float)G.bh * .34f);
    if (top < 1.f) rim += .05f * (1.f - top);
    float frost = 255.f * .06f;
    o.cov = cov_alpha(cov, 255);
    o.lx = (int)lrintf(nx * 7.f * k * k);
    o.ly = (int)lrintf(ny * 7.f * k * k);
    o.keep = (int)lrintf((1.f - pane) * .94f * (1.f - rim) * 256.f);
    o.cr = (int)lrintf((((float)g_tint_r * pane * .94f + frost) * (1.f - rim) + 255.f * rim) * 256.f);
    o.cg = (int)lrintf((((float)g_tint_g * pane * .94f + frost) * (1.f - rim) + 255.f * rim) * 256.f);
    o.cb = (int)lrintf((((float)g_tint_b * pane * .94f + frost) * (1.f - rim) + 255.f * rim) * 256.f);
    return true;
}

static inline int glass_ch(int v, int keep, int c) { return std::min(255, (v * keep + c) >> 8); }

/* The blurred wallpaper a glass box shows, as glass_tone keeps it legible
 * behind its pane. */
static inline const uint8_t *glass_back(int thin) { return (thin ? g_wp_thin : g_wp_blur).data(); }

/* The same for the frame on screen. */
static inline const uint8_t *glass_back_now(int thin)
{
    return g_lv_on ? (thin ? g_lv->thin : g_lv->blur).data() : glass_back(thin);
}

/* The full-coverage glass pixel at (x, y) whose backdrop is the blurred
 * wallpaper `back` at (x, y), as glass_box writes it. */
static inline uint16_t glass_wp565(const glass_pix &g, int x, int y, const uint8_t *back)
{
    int sx = std::min(std::max(x - g.lx, 0), WP_W - 1);
    const uint8_t *q = &back[((size_t)wp_row(y - g.ly) * WP_W + sx) * 3];
    return dither565(x, y, glass_ch(q[0], g.keep, g.cr), glass_ch(q[1], g.keep, g.cg), glass_ch(q[2], g.keep, g.cb));
}

/* Paint a liquid-glass box: the blurred wallpaper under it, bent near the rim,
 * shaded by glass_at. Without a wallpaper the page under the box is a smooth
 * gradient, so it is used as is. gi: the box's index in the target's glass
 * log, or -1 when it is not logged (its pixels then count as opaque). */
static bool glass_row_span(const glass_geo &G, int v, int *a, int *b, glass_pix &mid);
static int glass_log_add(const glass_geo &G, int x1, int y1, int x2, int y2, bool create);

static void glass_box(const glass_geo &G, int x1, int y1, int x2, int y2, int gi)
{
    if (G.bw <= 0 || G.bh <= 0) return;
    x1 = std::max(x1, G.bx); y1 = std::max(y1, G.by);
    x2 = std::min(x2, G.bx + G.bw); y2 = std::min(y2, G.by + G.bh);
    const bool wp = g_wp_drawn && !g_wp_blur.empty();
    const uint8_t *back = wp ? glass_back(G.thin) : nullptr;
    glass_pix g, mid;
    for (int y = y1; y < y2; y++) {
        int sa, sb;   /* the row's middle shades alike: work it out once */
        glass_row_span(G, y, &sa, &sb, mid);
        for (int x = x1; x < x2; x++) {
            if (x >= sa && x < sb) g = mid;
            else if (!glass_at(G, x, y, g)) continue;
            if (wp && g.cov == 255) {
                uint16_t px = glass_wp565(g, x, y, back);
                if (y < g_clip_top) continue;
                g_fb[(g_rotate ? g_h - 1 - y : y) * g_pitch_px + (g_rotate ? g_w - 1 - x : x)] = px;
                if (bg_px *q = bg_at(x, y)) *q = gi >= 0 ? bg_px{ 0, 255, (uint8_t)gi } : bg_px{ 0, 0, 0 };
                continue;
            }
            int r, gg, b;
            if (wp) {
                int sx = std::min(std::max(x - g.lx, 0), WP_W - 1);
                const uint8_t *q = &back[((size_t)wp_row(y - g.ly) * WP_W + sx) * 3];
                r = q[0]; gg = q[1]; b = q[2];
            } else {
                uint16_t o = get_px565(x, y);
                r = ((o >> 11) & 0x1F) << 3; gg = ((o >> 5) & 0x3F) << 2; b = (o & 0x1F) << 3;
            }
            put_px_dither(x, y, glass_ch(r, g.keep, g.cr), glass_ch(gg, g.keep, g.cg), glass_ch(b, g.keep, g.cb), g.cov);
            bg_px *q = wp && gi >= 0 && y >= g_clip_top ? bg_at(x, y) : nullptr;
            if (q) {   /* the rim: part this glass, part what was there */
                q->kg = (uint8_t)std::min(255, (q->gi == gi ? q->kg : 0) + g.cov);
                q->gi = (uint8_t)gi;
            }
        }
    }
}

/* ---- frames that move over a wallpaper that stays ----
 * Wallpaper and glass belong to the screen, but a swipe or a scroll moves
 * frames rendered somewhere else. Each buffer keeps how much of the wallpaper
 * and of which glass box shows through every pixel (bg_px), and the glass
 * boxes drawn into it; while a frame moves the host hands rows back, and each
 * pixel trades that share of the ground where it was drawn for the ground
 * where it is now. Bare wallpaper and bare glass come out exactly as if drawn
 * there; tinted fills, lines and text edges over them follow along. */
struct glass_rec { glass_geo g; int x1, y1, x2, y2; };
/* What a moving frame needs of the glass in one buffer row, worked out once:
 * the spans shaded as the row's middle (glass_row_span) and the other pixels
 * of each box one by one. gi: the box's index in recs. */
struct glass_shade { int8_t lx, ly; uint16_t keep, cr, cg, cb; };
struct glass_cell { uint16_t u; uint8_t gi; glass_shade s; };
struct glass_span { uint16_t a, b; uint8_t gi; glass_shade s; };
struct glass_row { bool done = false; std::vector<glass_span> spans; std::vector<glass_cell> cells; };
struct glass_log {
    const uint16_t *buf = nullptr;
    unsigned used = 0;
    std::vector<glass_rec> recs;
    bool rows_sized = false;       /* rows sized for recs (filled a row at a time) */
    std::vector<glass_row> rows;
    int bw = 0, bh = 0;
    std::vector<bg_px> bg;         /* bw*bh, empty without a wallpaper */
};
enum { GLASS_RECS_MAX = 255 };     /* gi is a byte */
static glass_log g_glog[10];
static unsigned g_glog_tick;

static void glog_reset(glass_log *l)
{
    l->recs.clear();
    l->rows_sized = false;
    l->bg.clear();
    l->bw = l->bh = 0;
    bg_unbind();
}

static glass_log *glog_for(const uint16_t *buf, bool create)
{
    glass_log *old = &g_glog[0];
    for (auto &l : g_glog) {
        if (l.buf == buf) { l.used = ++g_glog_tick; return &l; }
        if (l.used < old->used) old = &l;
    }
    if (!create) return nullptr;
    old->buf = buf;
    old->used = ++g_glog_tick;
    glog_reset(old);
    return old;
}

static void bg_bind(void)
{
    g_bg_fb = g_fb;
    g_bg_h = g_h;
    glass_log *l = g_fb ? glog_for(g_fb, false) : nullptr;
    g_bg = l && l->bw == g_w && l->bh == g_h && !l->bg.empty() ? l->bg.data() : nullptr;
}

/* Log glass box G, drawn over [x1, x2) x [y1, y2) of the target, for
 * html_view_bg_fix_row: its index in the target's log, or -1 (it then
 * counts as opaque). create: start a log for a target without one. */
static int glass_log_add(const glass_geo &G, int x1, int y1, int x2, int y2, bool create)
{
    glass_log *gl = g_wp_drawn && !g_wp_blur.empty() ? glog_for(g_fb, create) : nullptr;
    if (!gl || (!create && gl->bg.empty()) || gl->recs.size() >= GLASS_RECS_MAX) return -1;
    gl->recs.push_back({ G, std::max(x1, G.bx), std::max({ y1, G.by, g_clip_top }),
                         std::min(x2, G.bx + G.bw), std::min(y2, G.by + G.bh) });
    gl->rows_sized = false;
    return (int)gl->recs.size() - 1;
}

/* dst now holds a copy of src's pixels: it gets what src keeps of them. */
extern "C" void html_view_bg_copy(const uint16_t *dst, const uint16_t *src)
{
    glass_log *s = glog_for(src, false);
    glass_log *d = glog_for(dst, true);
    if (d == s) return;
    glog_reset(d);
    if (s) {
        d->recs = s->recs;
        d->bg = s->bg;
        d->bw = s->bw;
        d->bh = s->bh;
    }
}

/* buf was written some other way (a moving frame): it keeps nothing. */
extern "C" void html_view_bg_drop(const uint16_t *buf)
{
    if (glass_log *l = glog_for(buf, false)) glog_reset(l);
}

/* The part of row v of a glass box whose shading is the row's: between the
 * corner columns a pixel's distance to the edge is either its distance to the
 * top or bottom (the same along the row) or past the lens band and the rim
 * (no lens offset, no rim), so glass_at at the middle holds for all of it, to
 * the bit. Returns false (and an empty span) when the box is too narrow. */
static bool glass_row_span(const glass_geo &G, int v, int *a, int *b, glass_pix &mid)
{
    int band = (int)std::ceil(std::max(std::min(10.f, std::min(G.bw, G.bh) * .35f), 1.5f)) + 1;
    int side = std::max({ G.r[0], G.r[1], G.r[2], G.r[3], band }) + 1;
    *a = *b = G.bx;
    if (G.bw <= 2 * side + 2 || !glass_at(G, G.bx + G.bw / 2, v, mid) || mid.cov != 255) return false;
    *a = G.bx + side;
    *b = G.bx + G.bw - side;
    return true;
}

static inline glass_shade shade_of(const glass_pix &g)
{
    return { (int8_t)g.lx, (int8_t)g.ly, (uint16_t)g.keep, (uint16_t)g.cr, (uint16_t)g.cg, (uint16_t)g.cb };
}

/* Work out the glass of buffer row v (see glass_row). */
static void glass_prep_row(glass_log *l, int v)
{
    if (!l->rows_sized) {
        int h = 0;
        for (const auto &rec : l->recs) h = std::max(h, rec.y2);
        l->rows.assign((size_t)h, glass_row());
        l->rows_sized = true;
    }
    if (v < 0 || v >= (int)l->rows.size() || l->rows[(size_t)v].done) return;
    glass_row &gr = l->rows[(size_t)v];
    gr.done = true;
    glass_pix g, mid;
    for (size_t i = 0; i < l->recs.size(); i++) {
        const glass_rec &rec = l->recs[i];
        if (v < rec.y1 || v >= rec.y2) continue;
        int a, b;
        if (glass_row_span(rec.g, v, &a, &b, mid)) {
            a = std::max(a, rec.x1);
            b = std::min(b, rec.x2);
            if (a < b) gr.spans.push_back({ (uint16_t)a, (uint16_t)b, (uint8_t)i, shade_of(mid) });
        } else {
            a = b = rec.x2;
        }
        for (int u = rec.x1; u < rec.x2; u++) {
            if (u >= a && u < b) continue;
            if (glass_at(rec.g, u, v, g)) gr.cells.push_back({ (uint16_t)u, (uint8_t)i, shade_of(g) });
        }
    }
}

static inline uint16_t shade565(const glass_shade &s, const uint8_t *q, int x, int y)
{
    return dither565(x, y, glass_ch(q[0], s.keep, s.cr), glass_ch(q[1], s.keep, s.cg), glass_ch(q[2], s.keep, s.cb));
}

/* The full glass pixel of shading s at (x, y), as glass_box draws it there. */
static inline uint16_t glass_ground(const glass_shade &s, int x, int y, const uint8_t *back)
{
    int sx = std::min(std::max(x - s.lx, 0), WP_W - 1);
    return shade565(s, &back[((size_t)wp_row(y - s.ly) * WP_W + sx) * 3], x, y);
}

/* Pixel p was drawn over ground `from` and k/255 of it is that ground: the
 * same pixel over ground `to`. */
static inline int reground_ch(int p, int from, int to, int k, int mx)
{
    int d = to - from, v = p + (d * k + (d < 0 ? -127 : 127)) / 255;
    return v < 0 ? 0 : v > mx ? mx : v;
}

static inline uint16_t reground(uint16_t p, uint16_t from, uint16_t to, int k)
{
    return (uint16_t)(reground_ch(p >> 11, from >> 11, to >> 11, k, 31) << 11
                    | reground_ch(p >> 5 & 63, from >> 5 & 63, to >> 5 & 63, k, 63) << 5
                    | reground_ch(p & 31, from & 31, to & 31, k, 31));
}

/* The wallpaper as drawn at (x, y) from a ground's px, rgb and pad, kp of
 * it softened (as blur_pad draws it). */
static inline uint16_t wall565(const uint16_t *px, const uint8_t *rgb, const uint8_t *pad, int x, int y, int kp)
{
    size_t i = (size_t)wp_row(y) * WP_W + x;
    if (!kp) return px[i];
    const uint8_t *s = &rgb[i * 3], *p = &pad[i * 3];
    return dither565(x, y, s[0] + (p[0] - s[0]) * kp / 255, s[1] + (p[1] - s[1]) * kp / 255, s[2] + (p[2] - s[2]) * kp / 255);
}

/* row[0..n) is screen row y, columns x0.., copied from a buffer that log l
 * keeps (320 wide) at (x + ox, y + oy): give it the wallpaper and glass of
 * where it is on the screen, and of the frame playing there (live
 * wallpaper). Returns 0 when l keeps nothing. */
static int fix_row(glass_log *l, uint16_t *row, int y, int x0, int n, int ox, int oy)
{
    if (!l || g_wp.empty() || l->bg.empty() || l->bw != WP_W) return 0;
    int v = y + oy, u1 = x0 + ox, u2 = x0 + n + ox;
    if ((ox == 0 && oy == 0 && !g_lv_on) || n <= 0) return 1;
    if (y < 0 || v < 0 || v >= l->bh || x0 < 0 || x0 + n > WP_W || u1 < 0 || u2 > WP_W) return 0;
    const bg_px *bp = &l->bg[(size_t)v * WP_W];
    const wp_set *now = g_lv_on ? g_lv : nullptr;
    const uint16_t *np = now ? now->px.data() : g_wp.data();
    const uint8_t *nrgb = now ? now->rgb.data() : g_wp_rgb.data(), *npad = now ? now->pad.data() : g_wp_pad.data();
    const uint16_t *wo = &g_wp[(size_t)wp_row(v) * WP_W], *wn = &np[(size_t)wp_row(y) * WP_W];
    bool glass = false;
    for (int i = 0; i < n; i++) {
        int x = x0 + i, u = x + ox;
        const bg_px &q = bp[u];
        glass |= q.kg != 0;
        if (!q.kw) continue;
        if (!q.kp) {
            row[i] = q.kw == 255 ? wn[x] : reground(row[i], wo[u], wn[x], q.kw);
            continue;
        }
        uint16_t to = wall565(np, nrgb, npad, x, y, q.kp);
        row[i] = q.kw == 255 ? to : reground(row[i], wall565(g_wp.data(), g_wp_rgb.data(), g_wp_pad.data(), u, v, q.kp), to, q.kw);
    }
    if (!glass || l->recs.empty() || g_wp_blur.empty()) return 1;
    glass_prep_row(l, v);
    if (v >= (int)l->rows.size()) return 1;
    const glass_row &gr = l->rows[(size_t)v];
    for (const auto &sp : gr.spans) {   /* no lens offset across: one backdrop row each */
        const int thin = l->recs[sp.gi].g.thin;
        const uint8_t *qo = &glass_back(thin)[(size_t)wp_row(v - sp.s.ly) * WP_W * 3];
        const uint8_t *qn = &glass_back_now(thin)[(size_t)wp_row(y - sp.s.ly) * WP_W * 3];
        for (int u = std::max<int>(sp.a, u1); u < std::min<int>(sp.b, u2); u++) {
            const bg_px &q = bp[u];
            if (!q.kg || q.gi != sp.gi) continue;
            int x = u - ox;
            uint16_t to = shade565(sp.s, qn + x * 3, x, y);
            row[x - x0] = q.kg == 255 ? to : reground(row[x - x0], shade565(sp.s, qo + u * 3, u, v), to, q.kg);
        }
    }
    for (const auto &c : gr.cells) {
        if (c.u < u1 || c.u >= u2) continue;
        const bg_px &q = bp[c.u];
        if (!q.kg || q.gi != c.gi) continue;
        int x = c.u - ox;
        const int thin = l->recs[c.gi].g.thin;
        uint16_t to = glass_ground(c.s, x, y, glass_back_now(thin));
        row[x - x0] = q.kg == 255 ? to : reground(row[x - x0], glass_ground(c.s, c.u, v, glass_back(thin)), to, q.kg);
    }
    return 1;
}

/* fix_row for a row copied from buffer src. Returns 0 when src keeps
 * nothing (the host then falls back to swapping bare wallpaper). */
extern "C" int html_view_bg_fix_row(const uint16_t *src, uint16_t *row, int y, int x0, int n, int ox, int oy)
{
    return fix_row(g_wp.empty() ? nullptr : glog_for(src, false), row, y, x0, n, ox, oy);
}

/* The page under a playing clip, and the one handed over to come next:
 * what of the ground shows through them, for the host's thread that shows
 * the frames (it touches nothing else here). */
static glass_log g_snap_log[2];
static int g_snap_cur;

/* buf (a page as drawn) is the page to come next (host lock held). */
extern "C" void html_view_live_snap(const uint16_t *buf)
{
    glass_log *s = glog_for(buf, false), &n = g_snap_log[!g_snap_cur];
    n.recs = s ? s->recs : std::vector<glass_rec>();
    n.bg = s ? s->bg : std::vector<bg_px>();
    n.bw = s ? s->bw : 0;
    n.bh = s ? s->bh : 0;
    n.rows_sized = false;
    for (const bg_px &q : n.bg)
        if (q.kp) { g_live_pads = true; break; }   /* text bare on the wallpaper: keep the pad up */
}

/* The page to come next is the page under the clip now (host lock held). */
extern "C" void html_view_live_snap_next(void) { g_snap_cur = !g_snap_cur; }

/* dst holds the page under the clip now. */
extern "C" void html_view_live_snap_put(const uint16_t *dst)
{
    const glass_log &c = g_snap_log[g_snap_cur];
    glass_log *d = glog_for(dst, true);
    glog_reset(d);
    d->recs = c.recs;
    d->bg = c.bg;
    d->bw = c.bw;
    d->bh = c.bh;
}

/* Row y of that page (logical, 320 wide) over the frame playing. */
extern "C" void html_view_live_fix_row(uint16_t *row, int y) { fix_row(&g_snap_log[g_snap_cur], row, y, 0, WP_W, 0, 0); }

/* ---- text that stays legible over the wallpaper ----
 * As with Apple's Liquid Glass and Fluent's acrylic, legibility comes first
 * from the material: glass_tone keeps the wallpaper behind a dark pane from
 * showing too bright (and behind a light pane too dark), so text on glass
 * keeps its designed colour, or takes the dark ink of a light pane. Little
 * text is left bare on the wallpaper (the style puts glass under it): that
 * is coloured a line at a time (wall_ink), drawn a weight bolder, over the
 * wallpaper softened right behind it (blur_pad). Text on an opaque
 * fill (a switch, a chip, a selected segment) is left as designed. With the
 * setting off, or without a wallpaper, only the old per-character rule (dark
 * on dark, light on light) applies. */
static bool g_text_adapt = true;

static inline int luma(int r, int g, int b) { return (r * 299 + g * 587 + b * 114) / 1000; }

/* The most (dark pane) or least (light pane) the glass may show of the
 * wallpaper's brightness, as luma of the glass itself. */
enum { GLASS_LUMA_MAX = 112, GLASS_THIN_LUMA_MAX = 135, GLASS_LUMA_MIN = 168 };
/* The dark pane first lowers the exposure of a bright wallpaper (glass_tone:
 * the picture's GLASS_TOP percentile of luma to the limit), then rounds off
 * what is still above GLASS_KNEE of the limit. */
#define GLASS_KNEE .85f
enum { GLASS_TOP = 90 };
static float g_glass_gain = 1.f;   /* that exposure; set only while no clip frame is being worked out */

static inline bool glass_is_light(void) { return luma(g_tint_r, g_tint_g, g_tint_b) >= 128; }

/* Above the default transparency (50) the glass lets more of the
 * wallpaper's light through: up to 24 more luma at 100, where it had pressed
 * a pale picture to a dull grey just as hard as at 50. */
static inline int glass_more(void) { return g_glass_clear > 50 ? (g_glass_clear - 50) * 24 / 50 : 0; }

/* Rows [y0, y1) of src as glass of the given pane strength (0..1 of the
 * setting's) shows it, into dst: brightness past luma lmax (dark pane) is
 * eased down, or below lmin (light pane) lifted, keeping the hue. Interior
 * glass is backdrop * keep + pane, as in glass_at. */
static void tone_rows(const std::vector<uint8_t> &src, std::vector<uint8_t> &dst, float strength, int lmax, int lmin,
                      int y0, int y1)
{
    const size_t N = (size_t)WP_W * 3;
    dst.resize(src.size());
    if (src.empty() || y0 >= y1) return;
    memcpy(&dst[y0 * N], &src[y0 * N], (y1 - y0) * N);
    const float pane = (float)(100 - g_glass_clear) / 100.f * strength, keep = (1.f - pane) * .94f;
    const bool light = glass_is_light();
    const float base = (float)luma(g_tint_r, g_tint_g, g_tint_b) * pane * .94f + 255.f * .06f;
    if (keep < .02f) return;
    float lim = ((light ? lmin : lmax) - base) / keep;   /* as backdrop luma */
    if (light ? lim <= 0.f : lim >= 255.f) return;
    int map[256], chroma[256];
    for (int l = 0; l < 256; l++) {   /* a soft knee, so nothing clips flat */
        float v = (float)l;
        if (!light) {
            float knee = lim * GLASS_KNEE;
            v *= g_glass_gain;
            if (v > knee) { float t = (v - knee) / (lim - knee); v = knee + (lim - knee) * t / (1.f + t); }
        } else {
            float knee = lim + (255.f - lim) * .4f;
            if (v < knee) { float t = (knee - v) / (knee - lim); v = knee - (knee - lim) * t / (1.f + t); }
        }
        map[l] = std::min(255, std::max(0, (int)lrintf(v)));
        /* the chroma of a darkened pixel: the square root of the luma ratio,
         * between the uniform scale and keeping it whole (16.16) */
        chroma[l] = l ? (int)lrintf(sqrtf((float)map[l] / (float)l) * 65536.f) : 65536;
    }
    for (size_t i = y0 * N; i < y1 * N; i += 3) {
        uint8_t *p = &dst[i];
        int l = luma(p[0], p[1], p[2]), m = map[l];
        if (m == l) continue;
        for (int c = 0; c < 3; c++) {
            /* darkening moves the luma to the mapped one but scales the chroma
             * (channel - luma) only by chroma[l]: a uniform scale took the
             * colour out with the light, and pale pinks and skin tones turned
             * a muddy grey-brown under the glass */
            int v = !light ? m + (((p[c] - l) * chroma[l]) >> 16) : p[c] + (255 - p[c]) * (m - l) / std::max(1, 255 - l);
            p[c] = (uint8_t)std::min(255, std::max(0, v));
        }
    }
}

/* The backdrops of both panes over rows [y0, y1): a thin one lets more of
 * the wallpaper's brightness through, still short of washing out light ink. */
static void glass_tone_rows(const std::vector<uint8_t> &soft, std::vector<uint8_t> &blur, std::vector<uint8_t> &thin,
                            int y0, int y1)
{
    const int more = glass_more();
    tone_rows(soft, blur, 1.f, GLASS_LUMA_MAX + more, GLASS_LUMA_MIN, y0, y1);
    tone_rows(soft, thin, 1.f - GLASS_THIN / 100.f, GLASS_THIN_LUMA_MAX + more, GLASS_LUMA_MIN, y0, y1);
}

/* The knee alone had to take a pale picture down by itself and pressed
 * nearly all of it into the few lumas under the limit: dark, but flat like an
 * overexposed photo turned down. So the exposure of the dimmed picture goes
 * down first (contrast kept), by as much as brings its GLASS_TOP percentile
 * to the dark pane's limit, and the knee only rounds off the brightest rest;
 * a dark picture keeps 1. Both panes use it. A clip keeps the exposure of the
 * frame this was worked out on while it plays (its frames are toned a few
 * rows at a time), so the picture does not pump either. */
static void glass_gain(void)
{
    g_glass_gain = 1.f;
    const float pane = (float)(100 - g_glass_clear) / 100.f, keep = (1.f - pane) * .94f;
    if (g_wp_rgb.empty() || glass_is_light() || keep < .02f) return;
    const float lim = ((float)(GLASS_LUMA_MAX + glass_more()) - (float)luma(g_tint_r, g_tint_g, g_tint_b) * pane * .94f -
                       255.f * .06f) / keep;
    int hist[256] = { 0 }, top = 0;
    const int n = (int)(g_wp_rgb.size() / 3);
    for (int i = 0; i < n; i++) hist[luma(g_wp_rgb[i * 3], g_wp_rgb[i * 3 + 1], g_wp_rgb[i * 3 + 2])]++;
    for (int s = 0, l = 0; l < 256; l++) if ((s += hist[l]) * 100 < n * GLASS_TOP) top = l + 1;
    if (lim > 0.f && (float)top > lim) g_glass_gain = lim / (float)top;
}

static void glass_tone(void)
{
    glass_gain();
    glass_tone_rows(g_wp_soft, g_wp_blur, g_wp_thin, 0, WP_H);
}

/* ---- live wallpaper ----
 * A clip (livewp.c) plays on the lock screen and the home page while nobody
 * touches them. A worker works out each next frame the way the wallpaper is
 * worked out (dimmed, softened for text, blurred and toned for glass), but
 * only over the rows the frame changed and as far as the blurs reach from
 * them, into g_lw, and brings *g_lq up to it while it goes on with the next
 * one (a frame that takes long does not hold the screen up); when the frame
 * is due the host's thread swaps *g_lq for *g_lv, the frame on screen, and
 * shows its page regrounded onto it (html_view_bg_fix_row). Only what the
 * screen needs is brought up; html_view_live_freeze works out the rest. The
 * page itself stays drawn over the wallpaper, so text keeps the colours it
 * was given. */
static livewp_t g_clip;
static bool g_clip_ok;
static std::atomic<bool> g_clip_bad{false};   /* a frame did not decode: stop playing */
static wp_set g_lw;                            /* the worker's frame */
static pthread_mutex_t g_lw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_lw_cv = PTHREAD_COND_INITIALIZER;
static bool g_lw_thread, g_lw_play, g_lw_busy, g_lw_ready;
static std::atomic<bool> g_lw_stop{false};
static int g_lw_y0, g_lw_y1;                   /* rows that changed in the ready frame */
static int g_lw_py0, g_lw_py1;                 /* and in the one before (*g_lq is two behind) */
static unsigned g_lv_t0;                       /* when it started playing, host ms */
static double g_lv_due;                        /* when the next frame shows, ms after g_lv_t0 */

/* Rows [y0, y1) of set s into d: what the screen shows and the frame
 * itself, and with whole rgb and pad too. */
static void set_copy(wp_set &d, const wp_set &s, int y0, int y1, bool whole)
{
    d.frame = s.frame;
    d.whole = whole && (d.whole || (y0 == 0 && y1 == WP_H));
    if (y0 >= y1) return;
    const size_t o = (size_t)y0 * WP_W, k = (size_t)(y1 - y0) * WP_W;
    memcpy(&d.px[o], &s.px[o], k * sizeof d.px[0]);
    for (auto m : { &wp_set::src, &wp_set::blur, &wp_set::thin })
        memcpy(&(d.*m)[o * 3], &(s.*m)[o * 3], k * 3);
    if (whole)
        for (auto m : { &wp_set::rgb, &wp_set::pad })
            memcpy(&(d.*m)[o * 3], &(s.*m)[o * 3], k * 3);
}

/* Rows [y0, y1) of s changed in s.src: the rest of it there and as far as
 * the blurs reach (the pad only with pads set). Returns false when told to
 * stop meanwhile. */
static bool live_rows(wp_set &s, int y0, int y1, bool pads, std::vector<uint8_t> &t1, std::vector<uint8_t> &t2)
{
    const int keep = g_wp_keep, total = blur_total();
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < WP_W; x++) {
            size_t i = (size_t)y * WP_W + x;
            const uint8_t *c = &s.src[i * 3];
            uint8_t *o = &s.rgb[i * 3];
            for (int k = 0; k < 3; k++) o[k] = (uint8_t)(c[k] * keep / 100);
            s.px[i] = dither565(x, y, o[0], o[1], o[2]);
        }
    if (g_lw_stop) return false;
    if (pads) blur_rows(s.rgb, s.pad, PAD_BLUR, y0, y1, t1, t2);
    if (g_lw_stop) return false;
    blur_rows(s.rgb, s.soft, total, y0, y1, t1, t2);
    if (g_lw_stop) return false;
    glass_tone_rows(s.soft, s.blur, s.thin, std::max(0, y0 - total), std::min((int)WP_H, y1 + total));
    return true;
}

static void *live_main(void *)
{
    enum { TX = LIVEWP_W / LIVEWP_TILE };
    std::vector<uint8_t> t1, t2, scratch(LIVEWP_PAYLOAD_MAX);
    uint8_t dirty[LIVEWP_TILES];
    pthread_mutex_lock(&g_lw_mu);
    for (;;) {
        while (!g_lw_play) pthread_cond_wait(&g_lw_cv, &g_lw_mu);
        g_lw_busy = true;
        pthread_mutex_unlock(&g_lw_mu);
        memset(dirty, 0, sizeof dirty);
        g_lw.frame = (g_lw.frame + 1) % g_clip.n;
        bool ok = livewp_apply(&g_clip, g_lw.frame, g_lw.src.data(), dirty, scratch.data()) == 0;
        int first = -1, last = -1;
        for (int t = 0; t < LIVEWP_TILES; t++)
            if (dirty[t]) { if (first < 0) first = t; last = t; }
        int y0 = 0, y1 = 0;
        if (first >= 0) {
            y0 = first / TX * LIVEWP_TILE;
            y1 = (last / TX + 1) * LIVEWP_TILE;
        }
        const bool pads = g_live_pads, catch_up = pads && !g_lw_pad_ok;   /* a page with pads: all of the pad anew */
        bool done = ok && (first < 0 || live_rows(g_lw, y0, y1, pads && !catch_up, t1, t2));
        if (done && catch_up && !g_lw_stop) {
            blur_rows(g_lw.rgb, g_lw.pad, PAD_BLUR, 0, WP_H, t1, t2);
            g_lw_pad_ok = true;
            first = 0;
            y0 = 0;
            y1 = WP_H;
        } else if (!pads && first >= 0) {
            g_lw_pad_ok = false;
        }
        pthread_mutex_lock(&g_lw_mu);
        if (!ok) {
            g_clip_bad = true;
            g_lw_play = false;
        } else if (done) {
            while (g_lw_ready && !g_lw_stop) pthread_cond_wait(&g_lw_cv, &g_lw_mu);   /* the one before is still queued */
            if (!g_lw_stop) {
                int reach = std::max((int)PAD_BLUR, blur_total());
                g_lw_py0 = g_lw_y0;
                g_lw_py1 = g_lw_y1;
                g_lw_y0 = first < 0 ? 0 : std::max(0, y0 - reach);
                g_lw_y1 = first < 0 ? 0 : std::min((int)WP_H, y1 + reach);
                int a = g_lw_y0, b = g_lw_y1;   /* *g_lq is two frames behind: what changed in both */
                if (g_lw_py0 < g_lw_py1) {
                    a = a < b ? std::min(a, g_lw_py0) : g_lw_py0;
                    b = std::max(b, g_lw_py1);
                }
                set_copy(*g_lq, g_lw, a, b, g_live_pads && g_lw_pad_ok);
                g_lw_ready = true;
            }
        }
        g_lw_busy = false;
        pthread_cond_broadcast(&g_lw_cv);
    }
    return nullptr;
}

/* Stop the worker and drop the frame it has ready. */
static void live_halt(void)
{
    if (!g_lw_thread) return;
    pthread_mutex_lock(&g_lw_mu);
    g_lw_play = false;
    g_lw_stop = true;
    pthread_cond_broadcast(&g_lw_cv);
    while (g_lw_busy) pthread_cond_wait(&g_lw_cv, &g_lw_mu);
    g_lw_ready = false;
    g_lw_stop = false;
    pthread_mutex_unlock(&g_lw_mu);
}

/* path as a clip: keep it, its first frame in g_lv->src. */
static bool live_load(const char *path)
{
    livewp_t lw;
    if (livewp_open(&lw, path) != 0) return false;
    std::vector<uint8_t> src((size_t)WP_W * WP_H * 3), scratch(LIVEWP_PAYLOAD_MAX);
    uint8_t dirty[LIVEWP_TILES];
    if (livewp_apply(&lw, 0, src.data(), dirty, scratch.data()) != 0 || scratch[0] != 0) {   /* starts on a key frame */
        livewp_close(&lw);
        return false;
    }
    g_clip = lw;
    g_clip_ok = true;
    g_clip_bad = false;
    g_lv->src = std::move(src);
    g_lv->frame = 0;
    return true;
}

static void live_drop(void)
{
    live_halt();
    g_lv_on = false;
    if (g_clip_ok) livewp_close(&g_clip);
    g_clip_ok = false;
    g_lsets[0] = g_lsets[1] = g_lw = wp_set();
    g_lv = &g_lsets[0];
    g_lq = &g_lsets[1];
}

/* Frames of the clip that can play, 0 without one. */
extern "C" int html_view_live_frames(void) { return g_clip_ok && !g_clip_bad && !g_wp.empty() ? g_clip.n : 0; }
extern "C" int html_view_live_on(void) { return g_lv_on; }

/* Start playing from the frame the wallpaper is (now, host ms). */
extern "C" void html_view_live_start(unsigned now)
{
    if (g_lv_on || !html_view_live_frames()) return;
    wp_set &l = *g_lv;   /* its src is the frame the wallpaper was made from */
    l.px = g_wp;
    l.rgb = g_wp_rgb;
    l.pad = g_wp_pad;
    l.soft = g_wp_soft;
    l.blur = g_wp_blur;
    l.thin = g_wp_thin;
    l.whole = true;
    g_lw = l;
    *g_lq = l;
    g_lw_pad_ok = true;
    g_lw_y0 = g_lw_y1 = 0;
    g_lv_on = true;
    g_lv_t0 = now;
    g_lv_due = 1000.0 / g_clip.fps;
    pthread_mutex_lock(&g_lw_mu);
    if (!g_lw_thread) {
        pthread_t t;
        g_lw_thread = pthread_create(&t, nullptr, live_main, nullptr) == 0;
        if (g_lw_thread) pthread_detach(t);
    }
    g_lw_ready = false;
    g_lw_play = g_lw_thread;
    pthread_cond_broadcast(&g_lw_cv);
    pthread_mutex_unlock(&g_lw_mu);
}

/* Stop playing: the frame on screen becomes the wallpaper (pages drawn from
 * now on are drawn over it). */
extern "C" void html_view_live_freeze(void)
{
    if (!g_lv_on) return;
    live_halt();
    wp_set &l = *g_lv;
    if (!l.whole) {   /* rgb and pad fell behind */
        for (size_t i = 0; i < l.src.size(); i++) l.rgb[i] = (uint8_t)(l.src[i] * g_wp_keep / 100);
        blur_rows(l.rgb, l.pad, PAD_BLUR, 0, WP_H, g_blur_t1, g_blur_t2);
    }
    blur_rows(l.rgb, l.soft, blur_total(), 0, WP_H, g_blur_t1, g_blur_t2);
    g_live_pads = false;
    g_wp = l.px;
    g_wp_rgb = l.rgb;
    g_wp_pad = l.pad;
    g_wp_soft = l.soft;
    g_wp_blur = l.blur;
    g_wp_thin = l.thin;
    g_lv_on = false;
}

/* When the next frame is due and ready: take it as the frame on screen and
 * return 1 with the rows [*y0, *y1) it changed, for the host to show. */
extern "C" int html_view_live_poll(unsigned now, int *y0, int *y1)
{
    if (!g_lv_on) return 0;
    /* A clip as fast as the panel (30 fps) goes at the panel's pace: each
     * frame is sent as the one before is done, in step with its refresh. */
    const double t = (double)(unsigned)(now - g_lv_t0), step = g_clip.fps >= 30 ? 30.0 : 1000.0 / g_clip.fps;
    if (t < g_lv_due) return 0;
    pthread_mutex_lock(&g_lw_mu);
    if (!g_lw_ready) { pthread_mutex_unlock(&g_lw_mu); return 0; }
    const int a = g_lw_y0, b = g_lw_y1;
    std::swap(g_lv, g_lq);
    g_lw_ready = false;
    /* Late (the panel takes longer than a frame of a fast clip, or the
     * page took a while): every frame still shows, as soon as it can. */
    g_lv_due = t > g_lv_due + 2 ? t + step : g_lv_due + step;
    pthread_cond_broadcast(&g_lw_cv);
    pthread_mutex_unlock(&g_lw_mu);
    *y0 = a;
    *y1 = b;
    return a < b;
}

/* Milliseconds until the next frame is due, -1 when not playing. */
extern "C" int html_view_live_wait(unsigned now)
{
    if (!g_lv_on) return -1;
    double d = g_lv_due - (double)(unsigned)(now - g_lv_t0);
    return d <= 0 ? 0 : (int)d + 1;
}

/* Average luma of the framebuffer over a box, every other pixel. */
static int fb_luma(int x, int y, int w, int h)
{
    long sum = 0, n = 0;
    for (int yy = std::max(y, 0); yy < std::min(y + h, g_h); yy += 2)
        for (int xx = std::max(x, 0); xx < std::min(x + w, g_w); xx += 2, n++) {
            uint16_t o = get_px565(xx, yy);
            sum += luma(((o >> 11) & 0x1F) << 3, ((o >> 5) & 0x3F) << 2, (o & 0x1F) << 3);
        }
    return n ? (int)(sum / n) : 0;
}

/* The old rule: adjust (r, g, b) for a ground of luma bl. */
static void adapt_rgb(int bl, int &r, int &g, int &b)
{
    int tl = luma(r, g, b);
    if (bl >= 140 && tl > bl - 70) {                 /* light on light: darken */
        int target = std::max(16, bl - 150 + (255 - tl) * 3 / 5);
        if (tl > 0) {
            r = std::min(255, r * target / tl); g = std::min(255, g * target / tl); b = std::min(255, b * target / tl);
        }
    } else if (bl < 110 && tl < bl + 30) {           /* dark on dark: lighten */
        int target = std::min(240, bl + 150), k = tl < 255 ? (target - tl) * 256 / (255 - tl) : 0;
        r += (255 - r) * k / 256; g += (255 - g) * k / 256; b += (255 - b) * k / 256;
    }
}

/* Ink of the same hue for a light ground: about as dark as a light ink is light. */
static void dark_ink(int &r, int &g, int &b)
{
    int tl = luma(r, g, b), target = std::max(24, (255 - tl) * 2 / 5 + 24);
    if (tl > target && tl > 0) { r = r * target / tl; g = g * target / tl; b = b * target / tl; }
}

/* What lies under a box: average luma, and how much of it is wallpaper and
 * glass (0..255 each), every other pixel. */
struct ground { int luma, wall, glass; };
static ground ground_of(int x, int y, int w, int h)
{
    long sl = 0, sw = 0, sg = 0, n = 0;
    for (int yy = std::max(y, 0); yy < std::min(y + h, g_h); yy += 2)
        for (int xx = std::max(x, 0); xx < std::min(x + w, g_w); xx += 2, n++) {
            uint16_t o = get_px565(xx, yy);
            sl += luma(((o >> 11) & 0x1F) << 3, ((o >> 5) & 0x3F) << 2, (o & 0x1F) << 3);
            if (const bg_px *q = bg_at(xx, yy)) { sw += q->kw; sg += q->kg; }
        }
    if (!n) return { 0, 0, 0 };
    return { (int)(sl / n), (int)(sw / n), (int)(sg / n) };
}

/* The wallpaper across the rows [y, y + h), where it shows: its average
 * luma and the share (0..255) of dark pixels. A line of text on it is
 * coloured as one block. */
static bool wall_tone(int y, int h, int &l, int &dark)
{
    long sl = 0, nd = 0, n = 0;
    for (int yy = std::max(y, 0); yy < std::min(y + h, g_h); yy += 2)
        for (int xx = 0; xx < g_w; xx += 2) {
            const bg_px *q = bg_at(xx, yy);
            if (!q || q->kw < 200) continue;
            uint16_t o = get_px565(xx, yy);
            int v = luma(((o >> 11) & 0x1F) << 3, ((o >> 5) & 0x3F) << 2, (o & 0x1F) << 3);
            sl += v;
            nd += v < 100;
            n++;
        }
    if (!n) return false;
    l = (int)(sl / n);
    dark = (int)(nd * 255 / n);
    return true;
}

/* A really light stretch of wallpaper: luma past 200 with hardly a dark
 * pixel (under 5 %): snow, sky, a white backdrop. Only there does what sits
 * bare on it turn dark. */
static inline bool wall_light(int l, int dark) { return l > 200 && dark < 13; }

/* Ink for a neutral text colour on the wallpaper, kept in its rank (primary
 * or secondary): an icy white, bright and clean against the warm and white
 * highlights of most pictures, or a deep slate on a really light stretch. */
static void wall_ink(int tl, int wl, int wdark, int &r, int &g, int &b)
{
    static const int ink[4][3] = { { 244, 249, 255 }, { 228, 237, 248 }, { 15, 23, 42 }, { 51, 65, 85 } };
    const int *c = ink[(wall_light(wl, wdark) ? 2 : 0) + (tl >= 200 ? 0 : 1)];
    r = c[0]; g = c[1]; b = c[2];
}

/* Is the wallpaper really light over [x, x + w) x [y, y + h) of the screen?
 * For the host's marks drawn bare on it (the page dots). */
extern "C" int html_view_wallpaper_light(int x, int y, int w, int h)
{
    if (g_wp_rgb.empty()) return 0;
    long sl = 0, nd = 0, n = 0;
    for (int yy = std::max(y, 0); yy < y + h; yy++)
        for (int xx = std::max(x, 0); xx < std::min(x + w, (int)WP_W); xx++, n++) {
            const uint8_t *p = &g_wp_rgb[((size_t)wp_row(yy) * WP_W + xx) * 3];
            int v = luma(p[0], p[1], p[2]);
            sl += v;
            nd += v < 100;
        }
    return n && wall_light((int)(sl / n), (int)(nd * 255 / n));
}

/* How a text run is drawn: its colour, a faint one-pixel shadow (0..255),
 * and whether it sits bare on the wallpaper (then bolder, over a blur pad). */
struct text_look { int r, g, b, shadow; bool wall; };

/* The look for a text run of colour (r, g, b) over the box (see the section
 * comment). */
static text_look look_for(int x, int y, int w, int h, int r, int g, int b)
{
    text_look t = { r, g, b, 0, false };
    if (!g_text_adapt || !bg_at(0, 0)) {   /* the old rule, with a plain shadow over a wallpaper */
        if (g_text_adapt) adapt_rgb(fb_luma(x, y, w, h), t.r, t.g, t.b);
        t.shadow = g_wp_drawn && luma(t.r, t.g, t.b) > 128 ? 136 : 0;
        return t;
    }
    ground gr = ground_of(x - 8, y, w + 16, h);
    const int tl = luma(r, g, b);
    if (gr.wall + gr.glass < 128) return t;               /* on an opaque fill */
    if (gr.glass >= gr.wall) {                             /* on glass: the pane's own ink */
        if (glass_is_light()) { if (tl > 110) dark_ink(t.r, t.g, t.b); }
        else if (tl <= 110) { if (gr.luma < 110) adapt_rgb(gr.luma, t.r, t.g, t.b); }   /* dark ink lifts */
        else if (tl < gr.luma + 100) {                     /* a dim light ink lifts clear of a bright pane */
            int k = (std::min(255, gr.luma + 100) - tl) * 256 / (255 - tl);
            t.r += (255 - t.r) * k / 256; t.g += (255 - t.g) * k / 256; t.b += (255 - t.b) * k / 256;
        }
        return t;
    }
    t.wall = true;                                         /* bare on the wallpaper */
    int wl, wdark;
    bool neutral = std::max({ r, g, b }) - std::min({ r, g, b }) < 48;   /* accents keep their colour */
    if (neutral && wall_tone(y, h, wl, wdark)) wall_ink(tl, wl, wdark, t.r, t.g, t.b);
    return t;
}

/* Smooth the wallpaper behind a text run, [x1, x2) x [y1, y2), fading out
 * over F pixels around with rounded corners: its fine detail (hair,
 * outlines) no longer cuts through one-pixel strokes, its brightness stays.
 * Bare wallpaper pixels take the softened wallpaper (g_wp_pad) by the
 * strongest pad over them, so runs side by side make one smooth patch. */
static std::vector<uint8_t> g_pad_k;   /* per target pixel: how softened, 0..255 */
static const uint16_t *g_pad_fb;

static void blur_pad(int x1, int y1, int x2, int y2, int cx1, int cy1, int cx2, int cy2)
{
    const int F = 6;
    if (g_wp_pad.empty()) return;
    if (g_pad_fb != g_fb || g_pad_k.size() != (size_t)g_w * g_h) {
        g_pad_k.assign((size_t)g_w * g_h, 0);
        g_pad_fb = g_fb;
    }
    for (int y = std::max({ y1 - F, cy1, g_clip_top, 0 }); y < std::min({ y2 + F, cy2, g_h }); y++)
        for (int x = std::max({ x1 - F, cx1, 0 }); x < std::min({ x2 + F, cx2, g_w, (int)WP_W }); x++) {
            bg_px *q = bg_at(x, y);
            if (!q || q->kw != 255) continue;
            int dx = std::max({ x1 - x, x - (x2 - 1), 0 }), dy = std::max({ y1 - y, y - (y2 - 1), 0 });
            float t = 1.f - std::min(1.f, std::sqrt((float)(dx * dx + dy * dy)) / (float)(F + 1));
            int k = (int)(255.f * t * t * (3.f - 2.f * t));
            uint8_t &done = g_pad_k[(size_t)y * g_w + x];
            if (k <= done) continue;
            done = (uint8_t)k;
            q->kp = (uint8_t)k;   /* a moving frame keeps it softened */
            size_t i = ((size_t)wp_row(y) * WP_W + x) * 3;
            const uint8_t *s = &g_wp_rgb[i], *p = &g_wp_pad[i];
            uint16_t px = dither565(x, y, s[0] + (p[0] - s[0]) * k / 255, s[1] + (p[1] - s[1]) * k / 255,
                                    s[2] + (p[2] - s[2]) * k / 255);
            g_fb[(g_rotate ? g_h - 1 - y : y) * g_pitch_px + (g_rotate ? g_w - 1 - x : x)] = px;
        }
}

extern "C" void html_view_set_text_adapt(int on) { g_text_adapt = !!on; }

/* The ink for text of colour rgb on a glass pane (a host's native glass, as
 * the status-bar capsule): dark on a light pane, as designed on a dark one. */
extern "C" unsigned html_view_glass_ink(unsigned rgb)
{
    int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    if (g_text_adapt && glass_is_light() && luma(r, g, b) > 110) dark_ink(r, g, b);
    return (unsigned)(r << 16 | g << 8 | b);
}

/* A colour for host drawings (status-bar icons) over the box, as text there would get. */
extern "C" unsigned html_view_adapt_color(int x, int y, int w, int h, unsigned rgb)
{
    text_look t = look_for(x, y, w, h, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
    return (unsigned)(t.r << 16 | t.g << 8 | t.b);
}

/* One glyph at (ox, oy) inside the clip [cx1, cx2) x [cy1, cy2). */
static void blit_glyph(const glyph &gl, int ox, int oy, int r, int g, int b, int a, int cx1, int cy1, int cx2, int cy2)
{
    if (a <= 0) return;
    for (int row = 0; row < gl.h; row++) {
        int yy = oy + row;
        if (yy < cy1 || yy >= cy2) continue;
        for (int col = 0; col < gl.w; col++) {
            int xx = ox + col, aa = gl.px[(size_t)row * gl.w + col];
            if (aa && xx >= cx1 && xx < cx2) put_px(xx, yy, r, g, b, aa * a / 255);
        }
    }
}

/* The shadow under a glyph at strength 0..255: the glyph itself a pixel
 * low. At 320x480 a blurred halo only smears the strokes, so legibility comes
 * from the ink (look_for) and this stays a crisp, light touch. */
static void shadow_glyph(const glyph &gl, int ox, int oy, int a, int strength, int cx1, int cy1, int cx2, int cy2)
{
    if (strength > 0) blit_glyph(gl, ox, oy + 1, 0, 0, 0, a * strength / 255, cx1, cy1, cx2, cy2);
}

/* ---- container ---- */
class fb_container : public document_container {
    std::vector<position> m_clip;

    position eff_clip() const {
        position r(0, 0, (pixel_t)g_w, (pixel_t)g_h);
        for (auto &c : m_clip) {
            pixel_t x1 = std::max(r.left(), c.left()), y1 = std::max(r.top(), c.top());
            pixel_t x2 = std::min(r.right(), c.right()), y2 = std::min(r.bottom(), c.bottom());
            r = position(x1, y1, std::max<pixel_t>(0, x2 - x1), std::max<pixel_t>(0, y2 - y1));
        }
        return r;
    }
    void fill(pixel_t fx, pixel_t fy, pixel_t fw, pixel_t fh, web_color c) {
        if (c.alpha == 0) return;
        position cl = eff_clip();
        int x1 = std::max(R(fx), R(cl.left())),  y1 = std::max(R(fy), R(cl.top()));
        int x2 = std::min(R(fx + fw), R(cl.right())), y2 = std::min(R(fy + fh), R(cl.bottom()));
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++)
                put_px(x, y, c.red, c.green, c.blue, c.alpha);
    }
    /* Filled rectangle with rounded corners. litehtml only hands us the radii;
     * it's up to the container to honor them. Corners are anti-aliased. */
    void fill_rounded(int fx, int fy, int fw, int fh, int rtl, int rtr, int rbr, int rbl, web_color c) {
        if (c.alpha == 0 || fw <= 0 || fh <= 0) return;
        position cl = eff_clip();
        int x1 = std::max(fx, R(cl.left())),  y1 = std::max(fy, R(cl.top()));
        int x2 = std::min(fx + fw, R(cl.right())), y2 = std::min(fy + fh, R(cl.bottom()));
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++) {
                float cov = round_cov(x, y, fx, fy, fw, fh, rtl, rtr, rbr, rbl);
                if (cov > 0.f) put_px(x, y, c.red, c.green, c.blue, cov_alpha(cov, c.alpha));
            }
    }
    /* Stroke a rounded-rect outline of uniform thickness t: the outer rounded
     * rect minus the inner one (inset by t), both anti-aliased. */
    void stroke_rounded(int fx, int fy, int fw, int fh, int rtl, int rtr, int rbr, int rbl, int t, web_color c) {
        if (c.alpha == 0 || t <= 0 || fw <= 0 || fh <= 0) return;
        int itl = rtl - t > 0 ? rtl - t : 0, itr = rtr - t > 0 ? rtr - t : 0;
        int ibr = rbr - t > 0 ? rbr - t : 0, ibl = rbl - t > 0 ? rbl - t : 0;
        position cl = eff_clip();
        int x1 = std::max(fx, R(cl.left())),  y1 = std::max(fy, R(cl.top()));
        int x2 = std::min(fx + fw, R(cl.right())), y2 = std::min(fy + fh, R(cl.bottom()));
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++) {
                float cov = round_cov(x, y, fx, fy, fw, fh, rtl, rtr, rbr, rbl) *
                            (1.f - round_cov(x, y, fx + t, fy + t, fw - 2 * t, fh - 2 * t, itl, itr, ibr, ibl));
                if (cov > 0.f) put_px(x, y, c.red, c.green, c.blue, cov_alpha(cov, c.alpha));
            }
    }

    void fill_background_box(const background_layer &layer, web_color c) {
        const position &b = layer.border_box;
        const border_radiuses &br = layer.border_radius;
        if (br.top_left_x > 0 || br.top_right_x > 0 || br.bottom_right_x > 0 || br.bottom_left_x > 0)
            fill_rounded(R(b.x), R(b.y), R(b.x + b.width) - R(b.x), R(b.y + b.height) - R(b.y),
                         (int)br.top_left_x, (int)br.top_right_x,
                         (int)br.bottom_right_x, (int)br.bottom_left_x, c);
        else
            fill(b.x, b.y, b.width, b.height, c);
    }

public:
    /* Metrics for optical centering. The font's own ascender and descender are
     * each rounded up, so they add up to more than its line height and push
     * text down, and its capitals sit off the middle of the box. Instead keep
     * the font's line height, place the baseline so the middle of the
     * capitals/digits lands on the middle of the text box, and report
     * x_height as twice that middle so vertical-align: middle lines icons up
     * with digits and CJK rather than with lowercase letters. */
    uint_ptr create_font(const font_description &d, const document *, font_metrics *fm) override {
        auto *f = new ft_font();
        f->size = (int)d.size;
        FT_Set_Pixel_Sizes(g_face, 0, f->size);
        f->height = (int)(g_face->size->metrics.height >> 6);
        if (f->height <= 0) f->height = f->size;
        const glyph &cap = glyph_get('H', f->size);
        double cap_mid = cap.h ? cap.top - (cap.h - 1) / 2.0 : f->size * 0.36;
        /* rows 0..h-1 centre on (h-1)/2; when that falls between rows, sit a
         * hair high rather than low */
        f->ascent  = (int)floor((f->height - 1) / 2.0 + cap_mid);
        f->descent = f->height - f->ascent;
        if (fm) {
            fm->font_size = d.size;
            fm->ascent = f->ascent; fm->descent = f->descent;
            fm->height = f->height;
            fm->x_height = (int)lround(2 * cap_mid); fm->ch_width = f->size / 2;
            fm->draw_spaces = true;
        }
        return (uint_ptr)f;
    }
    void delete_font(uint_ptr h) override { delete (ft_font *)h; }

    pixel_t text_width(const char *text, uint_ptr h) override {
        return (pixel_t)text_advance(text, ((ft_font *)h)->size);
    }

    void draw_text(uint_ptr, const char *text, uint_ptr h, web_color color, const position &pos) override {
        auto *f = (ft_font *)h;
        position cl = eff_clip();
        int pen = R(pos.x), top = R(pos.y), base = top + f->ascent;
        int cx1 = R(cl.left()), cy1 = R(cl.top()), cx2 = R(cl.right()), cy2 = R(cl.bottom());
        /* the whole run by the ground around it */
        int adv = std::max(text_advance(text, f->size), 1);
        text_look t = look_for(pen, top, adv, R(pos.height), color.red, color.green, color.blue);
        if (t.wall) blur_pad(pen - 2, top, pen + adv + 2, top + R(pos.height), cx1, cy1, cx2, cy2);
        for (const char *s = text; *s; ) {
            const glyph &gl = glyph_get(utf8_next(s), f->size);
            int ox = pen + gl.left, oy = base - gl.top;
            shadow_glyph(gl, ox, oy, color.alpha, t.shadow, cx1, cy1, cx2, cy2);
            for (int pass = 0; pass < (t.wall ? 2 : 1); pass++)   /* bare on the wallpaper: a weight bolder */
                blit_glyph(gl, ox + pass, oy, t.r, t.g, t.b, color.alpha, cx1, cy1, cx2, cy2);
            pen += gl.adv;
        }
    }

    pixel_t pt_to_px(float pt) const override { return (pixel_t)(pt * 96.0f / 72.0f + 0.5f); }
    pixel_t get_default_font_size() const override { return 16; }
    const char *get_default_font_name() const override { return "sans-serif"; }

    void draw_solid_fill(uint_ptr, const background_layer &layer, const web_color &color) override {
        fill_background_box(layer, color);
    }
    /* Color of a gradient at t (0..1 along the gradient line) from its stops. */
    static web_color gradient_at(const std::vector<background_layer::color_point> &pts, float t) {
        if (pts.empty()) return web_color(0, 0, 0, 0);
        if (t <= pts.front().offset) return pts.front().color;
        if (t >= pts.back().offset) return pts.back().color;
        for (size_t i = 1; i < pts.size(); i++) {
            if (t > pts[i].offset) continue;
            const web_color &a = pts[i - 1].color, &b = pts[i].color;
            float span = pts[i].offset - pts[i - 1].offset;
            float k = span > 0 ? (t - pts[i - 1].offset) / span : 1.f;
            return web_color((byte)(a.red + (b.red - a.red) * k), (byte)(a.green + (b.green - a.green) * k),
                             (byte)(a.blue + (b.blue - a.blue) * k), (byte)(a.alpha + (b.alpha - a.alpha) * k));
        }
        return pts.back().color;
    }
    /* Opaque gradients repaint the same pixels on every refresh (the page
     * background), so the last few are kept and copied back. */
    struct grad_cache { std::string key; std::vector<uint16_t> px; };
    grad_cache m_grad[4];
    int m_grad_next = 0;

    /* Paint the layer's (rounded) box with color_of(x, y) -> t. `kind` names
     * the gradient geometry for the cache. */
    template <class F>
    void fill_gradient_box(const background_layer &l, const std::vector<background_layer::color_point> &pts,
                           const std::string &kind, F t_of) {
        const position &b = l.border_box;
        const border_radiuses &br = l.border_radius;
        int rtl = (int)br.top_left_x, rtr = (int)br.top_right_x, rbr = (int)br.bottom_right_x, rbl = (int)br.bottom_left_x;
        position cl = eff_clip();
        int bx = R(b.left()), by = R(b.top()), bw = R(b.right()) - bx, bh = R(b.bottom()) - by;
        int x1 = std::max({bx, R(cl.left()), R(l.clip_box.left())});
        int y1 = std::max({by, R(cl.top()), R(l.clip_box.top())});
        int x2 = std::min({bx + bw, R(cl.right()), R(l.clip_box.right())});
        int y2 = std::min({by + bh, R(cl.bottom()), R(l.clip_box.bottom())});
        if (x2 <= x1 || y2 <= y1) return;
        bool opaque = !(rtl || rtr || rbr || rbl);
        for (const auto &p : pts) if (p.color.alpha != 255) opaque = false;
        if (opaque) {
            char geo[96];
            snprintf(geo, sizeof geo, "|%d,%d,%d,%d|%d,%d,%d,%d|", bx, by, bw, bh, x1, y1, x2, y2);
            std::string key = kind + geo;
            for (const auto &p : pts) {
                char st[48];
                snprintf(st, sizeof st, "%g:%02x%02x%02x;", p.offset, p.color.red, p.color.green, p.color.blue);
                key += st;
            }
            grad_cache *c = nullptr;
            for (auto &e : m_grad) if (e.key == key) c = &e;
            if (!c) {
                c = &m_grad[m_grad_next];
                m_grad_next = (m_grad_next + 1) % 4;
                c->key = key;
                c->px.resize((size_t)(x2 - x1) * (y2 - y1));
                uint16_t *o = c->px.data();
                for (int y = y1; y < y2; y++)
                    for (int x = x1; x < x2; x++) {
                        web_color col = gradient_at(pts, t_of((float)x + .5f, (float)y + .5f));
                        *o++ = dither565(x, y, col.red, col.green, col.blue);
                    }
            }
            const uint16_t *src = c->px.data();
            for (int y = y1; y < y2; y++, src += x2 - x1) {
                if (y < g_clip_top) continue;
                if (g_rotate) {
                    uint16_t *dp = &g_fb[(g_h - 1 - y) * g_pitch_px + (g_w - 1 - x1)];
                    for (int i = 0; i < x2 - x1; i++) *dp-- = src[i];
                } else {
                    memcpy(&g_fb[y * g_pitch_px + x1], src, (size_t)(x2 - x1) * sizeof(uint16_t));
                }
                bg_set_span(x1, x2, y, bg_px{ 0, 0, 0 });
            }
            return;
        }
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++) {
                float cov = (rtl || rtr || rbr || rbl) ? round_cov(x, y, bx, by, bw, bh, rtl, rtr, rbr, rbl) : 1.f;
                if (cov <= 0.f) continue;
                web_color c = gradient_at(pts, t_of((float)x + .5f, (float)y + .5f));
                put_px_dither(x, y, c.red, c.green, c.blue, cov_alpha(cov, c.alpha));
            }
    }
    void draw_linear_gradient(uint_ptr, const background_layer &l, const background_layer::linear_gradient &g) override {
        float vx = g.end.x - g.start.x, vy = g.end.y - g.start.y, len2 = vx * vx + vy * vy;
        char kind[96];
        snprintf(kind, sizeof kind, "L%g,%g,%g,%g", g.start.x, g.start.y, g.end.x, g.end.y);
        fill_gradient_box(l, g.color_points, kind, [&](float x, float y) {
            return len2 > 0 ? ((x - g.start.x) * vx + (y - g.start.y) * vy) / len2 : 0.f;
        });
    }
    void draw_radial_gradient(uint_ptr, const background_layer &l, const background_layer::radial_gradient &g) override {
        float rx = g.radius.x > 0 ? g.radius.x : 1.f, ry = g.radius.y > 0 ? g.radius.y : 1.f;
        char kind[96];
        snprintf(kind, sizeof kind, "R%g,%g,%g,%g", g.position.x, g.position.y, rx, ry);
        fill_gradient_box(l, g.color_points, kind, [&](float x, float y) {
            float dx = (x - g.position.x) / rx, dy = (y - g.position.y) / ry;
            return std::sqrt(dx * dx + dy * dy);
        });
    }
    void draw_conic_gradient(uint_ptr, const background_layer &l, const background_layer::conic_gradient &) override {
        fill_background_box(l, web_color(45, 46, 50));
    }

    void draw_borders(uint_ptr, const borders &b, const position &p, bool) override {
        /* Rounded outline for the common case: all four sides equal width, same
         * color, with a corner radius (cards, the battery frame, ...). */
        int rtl = (int)b.radius.top_left_x, rtr = (int)b.radius.top_right_x;
        int rbr = (int)b.radius.bottom_right_x, rbl = (int)b.radius.bottom_left_x;
        const web_color &tc = b.top.color;
        bool same_w = b.top.width == b.left.width && b.top.width == b.right.width && b.top.width == b.bottom.width;
        bool same_c = tc.red == b.left.color.red && tc.green == b.left.color.green && tc.blue == b.left.color.blue && tc.alpha == b.left.color.alpha
                   && tc.red == b.right.color.red && tc.green == b.right.color.green && tc.blue == b.right.color.blue && tc.alpha == b.right.color.alpha
                   && tc.red == b.bottom.color.red && tc.green == b.bottom.color.green && tc.blue == b.bottom.color.blue && tc.alpha == b.bottom.color.alpha;
        bool all_solid = b.top.style != border_style_none && b.left.style != border_style_none
                      && b.right.style != border_style_none && b.bottom.style != border_style_none;
        if ((rtl || rtr || rbr || rbl) && same_w && same_c && all_solid && b.top.width > 0) {
            stroke_rounded(R(p.x), R(p.y), R(p.x + p.width) - R(p.x), R(p.y + p.height) - R(p.y),
                           rtl, rtr, rbr, rbl, (int)b.top.width, tc);
            return;
        }
        if (b.top.width > 0    && b.top.style    != border_style_none) fill(p.x, p.y, p.width, b.top.width, b.top.color);
        if (b.bottom.width > 0 && b.bottom.style != border_style_none) fill(p.x, p.bottom() - b.bottom.width, p.width, b.bottom.width, b.bottom.color);
        if (b.left.width > 0   && b.left.style   != border_style_none) fill(p.x, p.y, b.left.width, p.height, b.left.color);
        if (b.right.width > 0  && b.right.style  != border_style_none) fill(p.right() - b.right.width, p.y, b.right.width, p.height, b.right.color);
    }

    void draw_list_marker(uint_ptr, const list_marker &) override {}

    /* Images: only the built-in backgrounds, url(wallpaper), url(glass) and
     * url(glass-thin) (a lighter pane, for large areas such as the home
     * dial). They size themselves to the box, so any size will do. */
    void load_image(const char *, const char *, bool) override {}
    void get_image_size(const char *src, const char *, size &sz) override {
        bool ours = src && (!strcmp(src, "wallpaper") || !strcmp(src, "glass") || !strcmp(src, "glass-thin"));
        sz.width = sz.height = ours ? 1 : 0;
    }
    void draw_image(uint_ptr, const background_layer &l, const std::string &url, const std::string &) override {
        const position &b = l.border_box;
        position cl = eff_clip();
        int x1 = std::max(R(cl.left()), R(l.clip_box.left())), y1 = std::max(R(cl.top()), R(l.clip_box.top()));
        int x2 = std::min(R(cl.right()), R(l.clip_box.right())), y2 = std::min(R(cl.bottom()), R(l.clip_box.bottom()));
        if (url == "glass" || url == "glass-thin") {
            const border_radiuses &br = l.border_radius;
            glass_geo G = { R(b.left()), R(b.top()), R(b.right()) - R(b.left()), R(b.bottom()) - R(b.top()),
                            { (int)br.top_left_x, (int)br.top_right_x, (int)br.bottom_right_x, (int)br.bottom_left_x },
                            url == "glass-thin" ? GLASS_THIN : 0 };
            glass_box(G, x1, y1, x2, y2, glass_log_add(G, x1, y1, x2, y2, true));
        } else if (url == "wallpaper" && !g_wp.empty()) {
            int bx = R(b.left());
            g_wp_drawn = true;
            x1 = std::max(x1, bx); x2 = std::min(x2, bx + WP_W);
            y1 = std::max({ y1, R(b.top()), g_clip_top }); y2 = std::min(y2, R(b.bottom()));
            for (int y = y1; y < y2; y++) {
                const uint16_t *src = &g_wp[(size_t)wp_row(y) * WP_W + (x1 - bx)];
                if (g_rotate) {
                    uint16_t *dp = &g_fb[(g_h - 1 - y) * g_pitch_px + (g_w - 1 - x1)];
                    for (int i = 0; i < x2 - x1; i++) *dp-- = src[i];
                } else if (x2 > x1) {
                    memcpy(&g_fb[y * g_pitch_px + x1], src, (size_t)(x2 - x1) * sizeof(uint16_t));
                }
                bg_set_span(x1, x2, y, bg_px{ 255, 0, 0 });
            }
        }
    }

    void set_clip(const position &pos, const border_radiuses &) override { m_clip.push_back(pos); }
    void del_clip() override { if (!m_clip.empty()) m_clip.pop_back(); }
    void reset_state() { m_clip.clear(); }

    void get_viewport(position &v) const override { v = position(0, 0, (pixel_t)g_w, (pixel_t)g_h); }
    void get_media_features(media_features &m) const override {
        m.type = media_type_screen;
        m.width = m.device_width = (pixel_t)g_w;
        m.height = m.device_height = (pixel_t)g_h;
        m.color = 8; m.resolution = 96;
    }
    void get_language(string &, string &) const override {}

    /* trivial stubs */
    void set_caption(const char *) override {}
    void set_base_url(const char *) override {}
    void link(const std::shared_ptr<document> &, const element::ptr &) override {}
    void on_anchor_click(const char *url, const element::ptr &) override { g_clicked = url ? url : ""; }
    void on_mouse_event(const element::ptr &, mouse_event) override {}
    void set_cursor(const char *) override {}
    void transform_text(string &, text_transform) override {}
    void import_css(string &text, const string &url, string &) override {
        std::string path = g_ui_dir + "/" + url;
        css_get_cached(text, path);
    }
    element::ptr create_element(const char *, const string_map &, const std::shared_ptr<document> &) override { return nullptr; }
};

/* ---- C interface for the (C) main harness ---- */
static fb_container   *g_container;
static document::ptr   g_doc;
static int             g_scroll_y;   /* vertical scroll offset (logical px) */
static int             g_doc_overlay; /* overlay docs are already in screen coords */

extern "C" void html_view_init(uint16_t *fb, int w, int h, int pitch_px, int rotate, const char *font_path)
{
    g_fb = fb; g_w = w; g_h = h; g_pitch_px = pitch_px; g_rotate = rotate;
    FT_Init_FreeType(&g_ft);
    if (FT_New_Face(g_ft, font_path, 0, &g_face))
        fprintf(stderr, "html_view: cannot load font %s\n", font_path);
    g_container = new fb_container();
}

/* Draw into the framebuffer rotated by 180 degrees (upside-down panel) or not. */
extern "C" void html_view_set_rotate(int rotate)
{
    g_rotate = rotate;
}

/* Parse + lay out + paint an HTML string into the framebuffer. Returns height. */
extern "C" int html_view_render_html(const char *html)
{
    if (!html || !g_container) return -1;
    g_container->reset_state();
    g_doc.reset();
    document::ptr doc = document::createFromString(html, g_container);
    if (!doc) return -1;

    g_doc = doc;
    g_doc_overlay = 0;
    g_wp_drawn = false;
    g_pad_fb = nullptr;
    glass_log *gl = glog_for(g_fb, true);
    glog_reset(gl);
    if (!g_wp.empty()) {   /* starts all black: no ground shows */
        gl->bw = g_w;
        gl->bh = g_h;
        gl->bg.assign((size_t)g_w * g_h, bg_px{ 0, 0, 0 });
    }
    g_doc->render((pixel_t)g_w);
    for (int i = 0; i < g_pitch_px * g_h; i++) g_fb[i] = 0;
    position clip(0, 0, (pixel_t)g_w, (pixel_t)g_h);
    g_doc->draw((uint_ptr)0, 0, -g_scroll_y, &clip);   /* shift up by scroll */
    element::ptr root = g_doc->root();                 /* full content height */
    return root ? (int)root->get_placement().height : g_h;
}

/* Render an overlay on top of the current framebuffer (no clear): the body must
 * be transparent so only its boxes paint. g_doc is set to the overlay so taps
 * hit it. Used for modal dialogs. */
extern "C" int html_view_render_overlay(const char *html)
{
    if (!html || !g_container) return -1;
    g_container->reset_state();
    g_doc.reset();
    document::ptr doc = document::createFromString(html, g_container);
    if (!doc) return -1;

    g_doc = doc;
    g_doc_overlay = 1;
    g_doc->render((pixel_t)g_w);
    position clip(0, 0, (pixel_t)g_w, (pixel_t)g_h);
    g_doc->draw((uint_ptr)0, 0, 0, &clip);
    return 0;
}

/* Render into a plain logical W*H RGB565 buffer (no rotation) for animations. */
extern "C" int html_view_render_to(uint16_t *buf, const char *html)
{
    uint16_t *sfb = g_fb; int sw = g_w, sh = g_h, sp = g_pitch_px, sr = g_rotate, ssc = g_scroll_y;
    g_fb = buf; g_w = sw; g_h = sh; g_pitch_px = sw; g_rotate = 0; g_scroll_y = 0;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_w = sw; g_h = sh; g_pitch_px = sp; g_rotate = sr; g_scroll_y = ssc;
    return hh;
}

extern "C" int html_view_render_to_scroll(uint16_t *buf, const char *html, int scroll)
{
    uint16_t *sfb = g_fb; int sw = g_w, sh = g_h, sp = g_pitch_px, sr = g_rotate, ssc = g_scroll_y;
    g_fb = buf; g_w = sw; g_h = sh; g_pitch_px = sw; g_rotate = 0; g_scroll_y = scroll < 0 ? 0 : scroll;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_w = sw; g_h = sh; g_pitch_px = sp; g_rotate = sr; g_scroll_y = ssc;
    return hh;
}

static uint16_t *g_saved_fb = nullptr;
static int g_saved_w = 0, g_saved_h = 0, g_saved_pitch = 0, g_saved_rotate = 0, g_saved_scroll = 0;

extern "C" void html_view_target_begin(uint16_t *buf, int scroll)
{
    if (g_saved_fb) return;
    g_saved_fb = g_fb; g_saved_w = g_w; g_saved_h = g_h;
    g_saved_pitch = g_pitch_px; g_saved_rotate = g_rotate; g_saved_scroll = g_scroll_y;
    g_fb = buf; g_pitch_px = g_w; g_rotate = 0; g_scroll_y = scroll < 0 ? 0 : scroll;
}

extern "C" void html_view_target_end(void)
{
    if (!g_saved_fb) return;
    g_fb = g_saved_fb; g_w = g_saved_w; g_h = g_saved_h;
    g_pitch_px = g_saved_pitch; g_rotate = g_saved_rotate; g_scroll_y = g_saved_scroll;
    g_saved_fb = nullptr;
}

extern "C" int html_view_render_to_size(uint16_t *buf, int w, int h, const char *html)
{
    uint16_t *sfb = g_fb; int sw = g_w, sh = g_h, sp = g_pitch_px, sr = g_rotate, ssc = g_scroll_y;
    g_fb = buf; g_w = w; g_h = h; g_pitch_px = w; g_rotate = 0; g_scroll_y = 0;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_w = sw; g_h = sh; g_pitch_px = sp; g_rotate = sr; g_scroll_y = ssc;
    return hh;
}

extern "C" void html_view_set_uidir(const char *d) { g_ui_dir = d; }
extern "C" void html_view_set_scroll(int y) { g_scroll_y = y < 0 ? 0 : y; }
extern "C" void html_view_set_clip_top(int y) { g_clip_top = y < 0 ? 0 : y; }

/* Render the full page (no rotation, no clip to 480) into a tall W*bufh logical
 * buffer, for smooth windowed scrolling. Returns content height. */
extern "C" int html_view_render_tall(uint16_t *buf, const char *html, int bufh)
{
    uint16_t *sfb = g_fb; int sh = g_h, sp = g_pitch_px, sr = g_rotate, ssc = g_scroll_y;
    g_fb = buf; g_h = bufh; g_pitch_px = g_w; g_rotate = 0; g_scroll_y = 0;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_h = sh; g_pitch_px = sp; g_rotate = sr; g_scroll_y = ssc;
    return hh;
}

/* Fill a rect directly (used for the scrollbar overlay). */
extern "C" void html_view_fill_rect(int x, int y, int w, int h, int r, int g, int b, int a)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_px(xx, yy, r, g, b, a);
}

/* Fill an arbitrary (possibly concave) polygon, even-odd rule. Used by the
 * host to draw shapes litehtml/CSS can't, e.g. the battery charging bolt.
 * Vertex coordinates name pixel centres, as with the old scanline fill. Each
 * pixel row is cut by four sub-scanlines and every span adds its exact
 * horizontal coverage, so slanted edges are anti-aliased at scanline cost. */
extern "C" void html_view_fill_poly(const int *xs, const int *ys, int n, int r, int g, int b, int a)
{
    if (n < 3) return;
    int xmin = xs[0], xmax = xs[0], ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) {
        xmin = std::min(xmin, xs[i]); xmax = std::max(xmax, xs[i]);
        ymin = std::min(ymin, ys[i]); ymax = std::max(ymax, ys[i]);
    }
    const int W = xmax - xmin + 1;             /* pixel p covers [p - .5, p + .5) around xmin + p */
    std::vector<float> acc((size_t)W), xi((size_t)n);
    for (int y = ymin; y <= ymax; y++) {
        std::fill(acc.begin(), acc.end(), 0.f);
        for (int sub = 0; sub < 4; sub++) {
            float py = (float)y - .375f + sub * .25f;
            int cnt = 0;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                float yi = (float)ys[i], yj = (float)ys[j];
                if ((yi > py) != (yj > py))
                    xi[cnt++] = (float)xs[j] + (py - yj) * (float)(xs[i] - xs[j]) / (yi - yj);
            }
            std::sort(xi.begin(), xi.begin() + cnt);
            for (int k = 0; k + 1 < cnt; k += 2) {
                float u0 = std::max(xi[k] - (float)xmin + .5f, 0.f);
                float u1 = std::min(xi[k + 1] - (float)xmin + .5f, (float)W);
                if (u1 <= u0) continue;
                int i0 = (int)u0, i1 = (int)u1;
                if (i0 == i1) { acc[i0] += (u1 - u0) * .25f; continue; }
                acc[i0] += ((float)(i0 + 1) - u0) * .25f;
                for (int p = i0 + 1; p < i1; p++) acc[p] += .25f;
                if (i1 < W) acc[i1] += (u1 - (float)i1) * .25f;
            }
        }
        for (int p = 0; p < W; p++)
            if (acc[p] > 0.f) put_px(xmin + p, y, r, g, b, (int)(std::min(acc[p], 1.f) * (float)a + .5f));
    }
}

/* Anti-aliased filled circle (coverage from the signed distance to the edge). */
extern "C" void html_view_fill_circle(float cx, float cy, float rad, int r, int g, int b, int a)
{
    for (int y = (int)std::floor(cy - rad - 1); y <= (int)std::ceil(cy + rad + 1); y++)
        for (int x = (int)std::floor(cx - rad - 1); x <= (int)std::ceil(cx + rad + 1); x++) {
            float dx = (float)x + .5f - cx, dy = (float)y + .5f - cy;
            float cov = clampf01(rad - std::sqrt(dx * dx + dy * dy) + .5f);
            if (cov > 0.f) put_px(x, y, r, g, b, (int)(cov * (float)a + .5f));
        }
}

/* Anti-aliased line segment of thickness th with round ends. */
extern "C" void html_view_line(float x0, float y0, float x1, float y1, float th, int r, int g, int b, int a)
{
    float half = th * .5f, vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int y = (int)std::floor(std::min(y0, y1) - half - 1); y <= (int)std::ceil(std::max(y0, y1) + half + 1); y++)
        for (int x = (int)std::floor(std::min(x0, x1) - half - 1); x <= (int)std::ceil(std::max(x0, x1) + half + 1); x++) {
            float px = (float)x + .5f - x0, py = (float)y + .5f - y0;
            float t = len2 > 0 ? clampf01((px * vx + py * vy) / len2) : 0.f;
            float dx = px - t * vx, dy = py - t * vy;
            float cov = clampf01(half - std::sqrt(dx * dx + dy * dy) + .5f);
            if (cov > 0.f) put_px(x, y, r, g, b, (int)(cov * (float)a + .5f));
        }
}

/* Anti-aliased arc of thickness th around radius rad with round caps. Angles in
 * degrees, 0 = 12 o'clock, clockwise, a0 < a1 (a span of 360 draws a ring). */
extern "C" void html_view_arc(float cx, float cy, float rad, float th, float a0, float a1,
                              int r, int g, int b, int a)
{
    const float pi = 3.14159265f;
    float half = th * .5f, r0 = rad - half, r1 = rad + half;
    int ring = a1 - a0 >= 359.9f;

    if (a1 <= a0) return;
    for (int y = (int)std::floor(cy - r1 - 1); y <= (int)std::ceil(cy + r1 + 1); y++)
        for (int x = (int)std::floor(cx - r1 - 1); x <= (int)std::ceil(cx + r1 + 1); x++) {
            float dx = (float)x + .5f - cx, dy = (float)y + .5f - cy, d2 = dx * dx + dy * dy;
            if (d2 > (r1 + 1) * (r1 + 1) || d2 < (r0 - 1) * (r0 - 1)) continue;
            float d = std::sqrt(d2);
            float cov = clampf01(d - r0 + .5f) * clampf01(r1 - d + .5f);
            if (!ring) {
                float deg = std::atan2(dx, -dy) * 180.f / pi;
                while (deg < a0) deg += 360.f;
                while (deg >= a0 + 360.f) deg -= 360.f;
                if (deg > a1) continue;
            }
            if (cov > 0.f) put_px(x, y, r, g, b, (int)(cov * (float)a + .5f));
        }
    if (!ring) {
        float ra0 = a0 * pi / 180.f, ra1 = a1 * pi / 180.f;
        html_view_fill_circle(cx + rad * std::sin(ra0), cy - rad * std::cos(ra0), half, r, g, b, a);
        html_view_fill_circle(cx + rad * std::sin(ra1), cy - rad * std::cos(ra1), half, r, g, b, a);
    }
}

/* Anti-aliased rounded rectangle: filled when th <= 0, else an outline of
 * thickness th inside the edge (coverage from the signed distance). */
extern "C" void html_view_round_rect_aa(float x, float y, float w, float h, float rad, float th,
                                        int r, int g, int b, int a)
{
    if (w <= 0.f || h <= 0.f) return;
    float hw = w * .5f, hh = h * .5f, cx = x + hw, cy = y + hh;
    if (rad > hw) rad = hw;
    if (rad > hh) rad = hh;
    for (int py = (int)std::floor(y) - 1; py <= (int)std::ceil(y + h); py++)
        for (int px = (int)std::floor(x) - 1; px <= (int)std::ceil(x + w); px++) {
            float qx = std::fabs((float)px + .5f - cx) - (hw - rad), qy = std::fabs((float)py + .5f - cy) - (hh - rad);
            float ox = qx > 0.f ? qx : 0.f, oy = qy > 0.f ? qy : 0.f;
            float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - rad;
            float cov = clampf01(.5f - d);
            if (th > 0.f) cov *= clampf01(.5f + d + th);
            if (cov > 0.f) put_px(px, py, r, g, b, (int)(cov * (float)a + .5f));
        }
}

/* Filled rounded rectangle directly (host overlays like the lock icon). */
extern "C" void html_view_fill_round_rect(int x, int y, int w, int h, int rad, int r, int g, int b, int a)
{
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
        {
            float cov = round_cov(xx, yy, x, y, w, h, rad, rad, rad, rad);
            if (cov > 0.f) put_px(xx, yy, r, g, b, cov_alpha(cov, a));
        }
}

/* Native glass box (host overlays: lock pill, power discs); a circle when
 * rad is half the side. */
extern "C" void html_view_glass_rect(int x, int y, int w, int h, int rad)
{
    glass_geo G = { x, y, w, h, { rad, rad, rad, rad }, 0 };
    int y1 = std::max(0, g_clip_top);
    glass_box(G, 0, y1, g_w, g_h, glass_log_add(G, 0, y1, g_w, g_h, false));   /* follows a live wallpaper too */
}

/* The wallpaper behind the native status bar. Returns 0 (drawing nothing)
 * when the page has none. */
extern "C" int html_view_wallpaper_rect(int x, int y, int w, int h)
{
    if (g_wp.empty() || !g_wp_drawn) return 0;
    for (int yy = std::max(y, 0); yy < std::min(y + h, g_h); yy++)
        for (int xx = std::max(x, 0); xx < std::min(x + w, std::min(g_w, (int)WP_W)); xx++) {
            uint16_t px = g_wp[(size_t)wp_row(yy) * WP_W + xx];
            int dx = g_rotate ? (g_w - 1 - xx) : xx, dy = g_rotate ? (g_h - 1 - yy) : yy;
            g_fb[dy * g_pitch_px + dx] = px;
            if (bg_px *q = bg_at(xx, yy)) *q = bg_px{ 255, 0, 0 };
        }
    return 1;
}

extern "C" int html_view_text_width_px(const char *text, int size)
{
    if (!g_face || !text) return 0;
    return text_advance(text, size);
}

extern "C" void html_view_text_bounds_px(const char *text, int size,
                                         int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = 0;
    if (y0) *y0 = 0;
    if (x1) *x1 = 0;
    if (y1) *y1 = 0;
    if (!g_face || !text) return;
    int ascent = font_ascent(size);
    int pen = 0, minx = 0, miny = 0, maxx = 0, maxy = 0, seen = 0;
    for (const char *s = text; *s; ) {
        const glyph &gl = glyph_get(utf8_next(s), size);
        int gx0 = pen + gl.left;
        int gy0 = ascent - gl.top;
        int gx1 = gx0 + gl.w;
        int gy1 = gy0 + gl.h;
        if (!seen) {
            minx = gx0; miny = gy0; maxx = gx1; maxy = gy1; seen = 1;
        } else {
            if (gx0 < minx) minx = gx0;
            if (gy0 < miny) miny = gy0;
            if (gx1 > maxx) maxx = gx1;
            if (gy1 > maxy) maxy = gy1;
        }
        pen += gl.adv;
    }
    if (!seen) return;
    if (x0) *x0 = minx;
    if (y0) *y0 = miny;
    if (x1) *x1 = maxx;
    if (y1) *y1 = maxy;
}

extern "C" void html_view_draw_text_px(int x, int y, const char *text, int size, int bold,
                                       int r, int g, int b, int a)
{
    if (!g_face || !text) return;
    int pen = x, base = y + font_ascent(size);
    int adv = std::max(text_advance(text, size), 1);
    text_look t = look_for(x, y, adv, size, r, g, b);   /* as in draw_text */
    if (t.wall) { blur_pad(x - 2, y, x + adv + 2, y + size, 0, g_clip_top, g_w, g_h); bold = 1; }
    for (const char *s = text; *s; ) {
        const glyph &gl = glyph_get(utf8_next(s), size);
        for (int pass = 0; pass < (bold ? 2 : 1); pass++)
            shadow_glyph(gl, pen + gl.left + pass, base - gl.top, a, t.shadow, 0, g_clip_top, g_w, g_h);
        for (int pass = 0; pass < (bold ? 2 : 1); pass++)
            blit_glyph(gl, pen + gl.left + pass, base - gl.top, t.r, t.g, t.b, a, 0, g_clip_top, g_w, g_h);
        pen += gl.adv;
    }
}

extern "C" void html_view_draw_text_contrast_px(int x, int y, const char *text, int size, int bold,
                                                int dr, int dg, int db, int lr, int lg, int lb, int a)
{
    if (!g_face || !text) return;
    int pen = x, base = y + font_ascent(size);
    for (const char *s = text; *s; ) {
        const glyph &gl = glyph_get(utf8_next(s), size);
        for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
            int ox = pen + gl.left + pass, oy = base - gl.top;
            for (int row = 0; row < gl.h; row++)
                for (int col = 0; col < gl.w; col++) {
                    int aa = gl.px[(size_t)row * gl.w + col];
                    if (!aa) continue;
                    uint16_t bg = get_px565(ox + col, oy + row);
                    int br = ((bg >> 11) & 0x1F) * 255 / 31;
                    int bgc = ((bg >> 5) & 0x3F) * 255 / 63;
                    int bb = (bg & 0x1F) * 255 / 31;
                    int luma = (br * 299 + bgc * 587 + bb * 114) / 1000;
                    int r = luma > 116 ? dr : lr;
                    int g = luma > 116 ? dg : lg;
                    int b = luma > 116 ? db : lb;
                    put_px(ox + col, oy + row, r, g, b, aa * a / 255);
                }
        }
        pen += gl.adv;
    }
}

/* Hit-test a tap; returns the clicked anchor href ("" if none). */
extern "C" const char *html_view_click(float x, float y)
{
    g_clicked.clear();
    if (g_doc) {
        position::vector rb;
        float yy = y + (g_doc_overlay ? 0 : g_scroll_y);
        g_doc->on_lbutton_down(x, yy, x, yy, rb);
        g_doc->on_lbutton_up(x, yy, x, yy, rb);
    }
    return g_clicked.c_str();
}

/* ---- custom chart drawing: native polylines into a litehtml placeholder ----
 * Flow: render the page (which lays out an empty <div id="chart-..">), query the
 * div's box with html_view_rect(), then draw series into it with html_view_polyline().
 * Coords are logical (pre-rotation); put_px maps to the panel. */

/* Bresenham line, thick px wide, clipped to [rx,rx+rw) x [ry,ry+rh). */
static void chart_line(int x0, int y0, int x1, int y1,
                       int rx, int ry, int rw, int rh, int r, int g, int b, int thick)
{
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int ty = 0; ty < thick; ty++)
            for (int tx = 0; tx < thick; tx++) {
                int px = x0 + tx, py = y0 + ty;
                if (px >= rx && px < rx + rw && py >= ry && py < ry + rh)
                    put_px(px, py, r, g, b, 255);
            }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Find an element by CSS selector and return its laid-out box. 1 if found. */
extern "C" int html_view_rect(const char *sel, int *x, int *y, int *w, int *h)
{
    if (!g_doc) return 0;
    element::ptr root = g_doc->root();
    if (!root) return 0;
    element::ptr el = root->select_one(sel);
    if (!el) return 0;
    position p = el->get_placement();
    int sy = g_doc_overlay ? 0 : g_scroll_y;
    *x = R(p.x); *y = R(p.y) - sy; *w = R(p.x + p.width) - *x; *h = R(p.y + p.height) - R(p.y);
    return (*w > 1 && *h > 1);
}

/* Draw a value series as a polyline inside the rect. vals normalized by
 * [vmin,vmax]; top = vmax. fill_a>0 fills the area under the line at that alpha. */
extern "C" void html_view_polyline(int x, int y, int w, int h,
                                   const int *vals, int n, int vmin, int vmax,
                                   int r, int g, int b, int thick, int fill_a)
{
    if (n <= 0 || w <= 1 || h <= 1) return;
    if (vmax <= vmin) vmax = vmin + 1;

    if (fill_a > 0) {
        for (int col = 0; col < w; col++) {
            double t = (n > 1) ? (double)col / (w - 1) * (n - 1) : 0.0;
            int i0 = (int)t; double fr = t - i0;
            int v0 = vals[i0], v1 = (i0 + 1 < n) ? vals[i0 + 1] : v0;
            double v = v0 + (v1 - v0) * fr;
            if (v < vmin) v = vmin; if (v > vmax) v = vmax;
            int py = y + (h - 1) - (int)((v - vmin) * (h - 1) / (vmax - vmin));
            for (int yy = py; yy < y + h; yy++)
                if (((col + yy) & 1) == 0) put_px(x + col, yy, r, g, b, fill_a);
        }
    }

    int px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        int v = vals[i]; if (v < vmin) v = vmin; if (v > vmax) v = vmax;
        int cx = x + (n > 1 ? i * (w - 1) / (n - 1) : 0);
        int cy = y + (h - 1) - (int)((long)(v - vmin) * (h - 1) / (vmax - vmin));
        if (i > 0) chart_line(px, py, cx, cy, x, y, w, h, r, g, b, thick);
        px = cx; py = cy;
    }
}
