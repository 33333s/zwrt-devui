/*
 * htmlmain.c - HTML UI shell. The program is fixed; the UI lives in
 * /data/plugins/zwrt-devui/ui
 * as plain HTML/CSS. Each *.html (except menu.html) is a swipeable page;
 * {{TOKENS}} are filled from the zwrt-datad snapshot. Touch swipes pages and
 * taps fire anchor actions (href="act:..."). Power key: short = backlight,
 * long = the menu.html overlay.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_disp.h"
#include "data.h"
#include "json.h"
#include "touch_input.h"
#include "key_input.h"
#include "backlight.h"
#include "devui_ext.h"
#include "selfcheck.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern void        html_view_init(uint16_t *fb, int w, int h, int pitch_px, int rotate, const char *font_path);
extern void        html_view_set_uidir(const char *dir);
extern void        html_view_set_rotate(int rotate);
extern int         html_view_render_html(const char *html);
extern int         html_view_render_to(uint16_t *buf, const char *html);
extern int         html_view_render_to_scroll(uint16_t *buf, const char *html, int scroll);
extern void        html_view_target_begin(uint16_t *buf, int scroll);
extern void        html_view_target_end(void);
extern const char *html_view_click(float x, float y);
extern int         html_view_rect(const char *sel, int *x, int *y, int *w, int *h);
extern void        html_view_polyline(int x, int y, int w, int h, const int *vals, int n,
                                      int vmin, int vmax, int r, int g, int b, int thick, int fill_a);
extern void        html_view_set_scroll(int y);
extern void        html_view_set_clip_top(int y);
extern void        html_view_fill_rect(int x, int y, int w, int h, int r, int g, int b, int a);
extern void        html_view_fill_poly(const int *xs, const int *ys, int n, int r, int g, int b, int a);
extern void        html_view_fill_round_rect(int x, int y, int w, int h, int rad, int r, int g, int b, int a);
extern void        html_view_fill_circle(float cx, float cy, float rad, int r, int g, int b, int a);
extern void        html_view_line(float x0, float y0, float x1, float y1, float th, int r, int g, int b, int a);
extern void        html_view_round_rect_aa(float x, float y, float w, float h, float rad, float th,
                                           int r, int g, int b, int a);
extern void        html_view_arc(float cx, float cy, float rad, float th, float a0, float a1,
                                 int r, int g, int b, int a);
extern int         html_view_text_width_px(const char *text, int size);
extern void        html_view_text_bounds_px(const char *text, int size, int *x0, int *y0, int *x1, int *y1);
extern void        html_view_draw_text_px(int x, int y, const char *text, int size, int bold,
                                          int r, int g, int b, int a);
extern void        html_view_draw_text_contrast_px(int x, int y, const char *text, int size, int bold,
                                                   int dr, int dg, int db, int lr, int lg, int lb, int a);
extern int         html_view_render_tall(uint16_t *buf, const char *html, int bufh);
extern int         html_view_render_overlay(const char *html);
extern int         html_view_load_wallpaper(const char *path, int dim, int blur);
extern void        html_view_set_glass_blur(int blur);
extern void        html_view_bg_copy(const uint16_t *dst, const uint16_t *src);
extern void        html_view_bg_drop(const uint16_t *buf);
extern int         html_view_bg_fix_row(const uint16_t *src, uint16_t *row, int y, int x0, int n, int ox, int oy);
extern int         html_view_wallpaper_on(void);
extern int         html_view_wallpaper_rect(int x, int y, int w, int h);
extern unsigned    html_view_glass_ink(unsigned rgb);
extern int         html_view_wallpaper_light(int x, int y, int w, int h);
extern void        html_view_set_glass(int clear);
extern void        html_view_set_glass_tint(int r, int g, int b);
extern const uint16_t *html_view_wallpaper_row(int y);
extern void        html_view_set_text_adapt(int on);
extern unsigned    html_view_adapt_color(int x, int y, int w, int h, unsigned rgb);
extern void        html_view_glass_rect(int x, int y, int w, int h, int rad);
extern int         html_view_live_frames(void);
extern int         html_view_live_on(void);
extern void        html_view_live_start(unsigned now);
extern void        html_view_live_freeze(void);
extern int         html_view_live_poll(unsigned now, int *y0, int *y1);
extern int         html_view_live_wait(unsigned now);
extern void        html_view_live_snap(const uint16_t *buf);
extern void        html_view_live_snap_next(void);
extern void        html_view_live_snap_put(const uint16_t *dst);
extern void        html_view_live_fix_row(uint16_t *row, int y);
static void        maybe_dump_fb(drm_disp_t *d);

/* Install directory: the folder the binary runs from when it holds ui/, else
 * the default. An install the old plugin made under /data/plugins/zwrt-devui
 * therefore keeps its pages and settings. UI_DIR given at build time (the host
 * preview) pins the UI folder. */
#define DEVUI_DEFAULT_DIR "/data/plugins/zwrt-devui"
#ifdef UI_DIR
static const char g_fixed_ui_dir[] = UI_DIR;   /* copied now: UI_DIR is redefined below */
#define DEVUI_FIXED_UI_DIR g_fixed_ui_dir
#undef UI_DIR
#endif
static char g_app_dir[160] = DEVUI_DEFAULT_DIR;
static char g_ui_dir[176];
static char g_functions_dir[192];
static char g_device_file[192], g_conf_file[192], g_signal_cache_file[192], g_lockpin_file[192];
static char g_wp_file[192], g_wp_conf_file[192], g_wp_live_file[192];
#define UI_DIR g_ui_dir
#define FUNCTIONS_DIR g_functions_dir

static void paths_init(void)
{
    char exe[160], ui[176];
    struct stat st;
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        char *slash;
        exe[n] = 0;
        slash = strrchr(exe, '/');
        if (slash && slash != exe) {
            *slash = 0;
            snprintf(ui, sizeof ui, "%s/ui", exe);
            if (stat(ui, &st) == 0 && S_ISDIR(st.st_mode))
                snprintf(g_app_dir, sizeof g_app_dir, "%s", exe);
        }
    }
#ifdef DEVUI_FIXED_UI_DIR
    snprintf(g_ui_dir, sizeof g_ui_dir, "%s", DEVUI_FIXED_UI_DIR);
#else
    snprintf(g_ui_dir, sizeof g_ui_dir, "%s/ui", g_app_dir);
#endif
    snprintf(g_functions_dir, sizeof g_functions_dir, "%s/functions", g_ui_dir);
    snprintf(g_device_file, sizeof g_device_file, "%s/.device", g_app_dir);
    snprintf(g_conf_file, sizeof g_conf_file, "%s/devui.conf", g_app_dir);
    snprintf(g_signal_cache_file, sizeof g_signal_cache_file, "%s/signal.cache", g_app_dir);
    snprintf(g_lockpin_file, sizeof g_lockpin_file, "%s/.lockpin", g_ui_dir);
    snprintf(g_wp_file, sizeof g_wp_file, "%s/wallpaper.jpg", g_app_dir);
    snprintf(g_wp_conf_file, sizeof g_wp_conf_file, "%s/wallpaper.conf", g_app_dir);
    snprintf(g_wp_live_file, sizeof g_wp_live_file, "%s/wallpaper.live", g_app_dir);
}
#define UI_FONT "/usr/ui/fonts/ZTEZhengYuan.ttf"

/* The rootfs is read-only and the vendor font is thin at small sizes, so allow
 * an override via the DEVUI_FONT env var. Falls back to UI_FONT (with a log
 * line) when the override is unset or unreadable. */
static const char *resolve_ui_font(void)
{
    const char *env = getenv("DEVUI_FONT");
    if (env && *env) {
        if (access(env, R_OK) == 0) return env;
        fprintf(stderr, "DEVUI_FONT=%s unreadable, using %s\n", env, UI_FONT);
    }
    return UI_FONT;
}
#define DATAD_HTTP_ADDR "127.0.0.1"
#define DATAD_HTTP_PORT 9460
#define DATAD_HTTP_TIMEOUT_MS 300

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static uint32_t millis(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#define TEMPLATE_CACHE_CAP 8
#define PAGE_HTML_CACHE_CAP 1048576

struct template_cache_entry {
    char path[320];
    long long mtime_ns;
    char *content;
    int used;
};

static struct template_cache_entry g_tmpl_cache[TEMPLATE_CACHE_CAP];
static int g_tmpl_next;

/* ---- palette ----
 * The U30 Pro screen palette: light text on a dark navy page, with a primary
 * accent (a) and a second accent (b: 5G badge, uplink) per theme. */
static const struct { const char *name, *cls; unsigned a, b; } g_themes[] = {
    { "海洋",   "th-ocean",  0x38D0FF, 0xFF7AC8 },
    { "极光",   "th-aurora", 0x4ADE80, 0xFFC857 },
    { "日落",   "th-sunset", 0xFF9F43, 0x6C8CFF },
    { "紫罗兰", "th-violet", 0xA78BFA, 0x2DD4BF },
};
#define NTHEMES ((int)(sizeof g_themes / sizeof g_themes[0]))
#define C_TXT   0xF2F6FAu
#define C_DIM   0x93A4B8u
#define C_FAINT 0x56677Au
#define C_OK    0x4ADE80u
#define C_WARN  0xFBBF24u
#define C_BAD   0xF87171u
#define C_BAR   0x070C15u   /* status bar = top of the page background (style.css) */
#define RGB3(c) (int)((c) >> 16 & 255), (int)((c) >> 8 & 255), (int)((c) & 255)

/* ---- pages / theme ---- */
static char g_pages[24][288];
static int  g_npages, g_cur;
static char g_subpage[64];       /* non-empty = a second-level page under ui/subpages */
static char g_subpage_path[300];
static int  g_theme;      /* index into g_themes */
static int  g_show_key;   /* reveal WiFi password (default hidden) */
static int  g_show_cellid; /* reveal NR Cell ID (default hidden) */
static int  g_show_imei;   /* reveal IMEI (default hidden) */
static int  g_speed_bits = 1; /* 1 = Mbps (bit rate), 0 = MB/s (byte rate) */

static unsigned theme_a(void) { return g_themes[g_theme].a; }
static unsigned theme_b(void) { return g_themes[g_theme].b; }

/* ---- device model ----
 * The U60 Pro (MU5250) and the TopFlow (MU5252) share the panel, touch and
 * power key, but not the features: the TopFlow has three modems, link
 * aggregation and fan / liquid cooling, and no reverse charging or USB
 * tethering. The model comes from datad's device.api_template. The last one
 * seen is kept in a dotfile so the right pages show from the first frame after
 * a reboot; DEVUI_DEVICE=mu5250|mu5252 overrides both.
 *
 * Pages follow the model by file name: NN-name.<tag>.html is shown only on
 * that model and replaces NN-name.html there (tag mu5250 or mu5252). The same
 * holds for subpages/<name>.<tag>.html. */
enum { DEV_U60 = 0, DEV_TOPFLOW };
static int g_dev = DEV_U60;
static int g_dev_forced;
#define DEVICE_FILE g_device_file

static int dev_is_tf(void) { return g_dev == DEV_TOPFLOW; }
static const char *dev_tag(void) { return dev_is_tf() ? "mu5252" : "mu5250"; }
static int dev_from_name(const char *s)
{
    return s && (!strcasecmp(s, "mu5252") || !strcasecmp(s, "topflow")) ? DEV_TOPFLOW : DEV_U60;
}

static void device_init(void)
{
    const char *env = getenv("DEVUI_DEVICE");
    char line[32] = "";
    if (env && env[0]) {
        g_dev = dev_from_name(env);
        g_dev_forced = 1;
        return;
    }
    FILE *fp = fopen(DEVICE_FILE, "r");
    if (fp) {
        if (fgets(line, sizeof line, fp)) line[strcspn(line, "\r\n")] = 0;
        fclose(fp);
    }
    g_dev = dev_from_name(line);
}

/* Apply the model datad reports. Returns 1 when it changed. */
static int device_update(const char *model_tag)
{
    int dev;
    if (g_dev_forced || !model_tag || !model_tag[0]) return 0;
    dev = dev_from_name(model_tag);
    if (dev == g_dev) return 0;
    g_dev = dev;
    FILE *fp = fopen(DEVICE_FILE, "w");
    if (fp) { fprintf(fp, "%s\n", dev_tag()); fclose(fp); }
    return 1;
}

/* <body> class: "dark" keeps generated markup's .dark selectors, plus the
 * palette and the model (dev-mu5250 / dev-mu5252) */
static const char *theme_body_class(void)
{
    static char cls[64];
    snprintf(cls, sizeof cls, "dark %s dev-%s%s", g_themes[g_theme].cls, dev_tag(),
             html_view_wallpaper_on() ? " wp" : "");
    return cls;
}
static int  g_show_batpct = 1; /* show percent text inside status battery */
static int  g_show_sbspeed;    /* show down/up rates in the status bar */
static int  g_text_adapt = 1;  /* text colour follows the background under it */
static long g_stat_rx, g_stat_tx;   /* bytes/s, for the status bar */
static int  g_charging;   /* set from snapshot; drives charge animation cadence */
static int  g_stat_bat, g_stat_sig, g_stat_lowbat;
static char g_stat_time[8], g_stat_gen[8];
static unsigned g_phase;  /* animation tick (battery charge sweep) */
static int  g_charge_boot; /* power-off charging boot: full-screen charging UI */
static int  g_scroll;     /* current page vertical scroll offset */
static int  g_page_h;     /* last rendered page content height */
static int  g_autooff_ms; /* auto screen-off timeout, 0 = never */
static int  g_refresh_ms = 1000; /* state refresh interval, 0 = paused */
static int  g_sig_read;   /* ML1/raw signaling read switch */
static int  g_sig_parse;  /* decoded LTE/NR signaling parse switch + page visibility */
static int  g_neighbor_open; /* expand neighbor-cell list on the first signal page */
static int  g_saved_bright = -1; /* persisted backlight level, -1 = none yet */
static int  g_auto_bright;        /* TopFlow: follow the ambient light sensor */
enum { ST_STATE_MISSING = 0, ST_STATE_IDLE, ST_STATE_RUNNING, ST_STATE_DONE, ST_STATE_FAIL };
#define SPEEDTEST_BIN "/data/plugins/better-speedtest/better-speedtest"
#define SPEEDTEST_LOG "/tmp/better-speedtest.log"
#define SPEEDTEST_LOOP_FLAG "/tmp/better-speedtest.loop"
#define SPEEDTEST_LOOP_PID "/tmp/better-speedtest.loop.pid"
static char g_st_src[16] = "auto";
static char g_st_dir[8] = "both";
static int  g_st_dur = 15;
static int  g_st_state = ST_STATE_MISSING;
static int  g_st_installed = -1;
static int  g_st_have_result;
static uint32_t g_st_poll_at;
static char g_st_phase[24], g_st_msg[160], g_st_err[160];
static char g_st_node[96], g_st_source[32], g_st_carrier[32], g_st_city[32], g_st_ulmsg[96];
static double g_st_cur_mbps, g_st_cur_peak;
static double g_st_dl_avg, g_st_dl_peak, g_st_ul_avg, g_st_ul_peak, g_st_ping, g_st_jitter;
#define ST_HIST 48
static int g_st_dl_hist[ST_HIST], g_st_ul_hist[ST_HIST], g_st_dl_n, g_st_ul_n;
static int g_last_clock_min = -1;       /* HH:MM changes only once per minute */
static char g_render_html_cache[PAGE_HTML_CACHE_CAP];
static size_t g_render_html_cache_len;
static char g_render_html_cache_path[320];
static int g_render_html_cache_scroll;
static int g_render_html_cache_modal;
static long g_render_html_cache_sms_open;
static int g_render_html_cache_lock;
static long long g_render_html_cache_css_mtime = -1;
/* The page raster before native overlays: a cache hit restores it, so overlays
 * with alpha (ring tracks, glass discs) are not blended onto the last frame. */
static uint16_t g_render_base[320 * 480];
static int g_render_quiet;   /* render() leaves the panel alone (transition snapshots) */
static long long g_pages_dir_mtime = -1;
static uint32_t g_pages_scan_at;
static int normalize_refresh_ms(int ms);

/* ---- page-2 aux state, cached (-1 = unknown). Wi-Fi switches go through
 * datad (wifi.set_module / wifi.radio.set) and read back from wifi.state, which
 * knows the U60 Pro AP layouts the firmware can actually start; only when datad
 * is unreachable do we fall back to `ifconfig wlanN up/down` + operstate
 * (wlan0=main 2.4G, wlan2=main 5G). PSM = `iw power_save`, persisted via a
 * self-written /etc/hotplug.d/iface/99-disable-powersave script that re-applies
 * the chosen mode on every ifup (so power_save=on sticks across
 * reconnect/reboot). The DHCP pool is computed live from uci. Toggles flip
 * optimistically and hold for a moment; a throttle reconciles. */
static int  g_w24 = -1, g_w5 = -1, g_w6 = -1, g_wpsm = -1;
static int  g_dps = -1;   /* power direct-supply mode (/state power.direct_supply), -1 unknown */
static uint32_t g_wifi_hold_until, g_dps_hold_until;   /* optimistic toggle hold */
/* TopFlow cooling and aggregation (/state cooling / aggregation). A tap sets
 * the value at once and holds it until datad has applied it. Turning
 * aggregation off drops the bonded link, so the first tap only arms it. */
static char g_fan_mode[16], g_liq_mode[16];
static int  g_agg_on = -1;
static uint32_t g_fan_hold_until, g_liq_hold_until, g_agg_hold_until, g_agg_armed_until;
/* main AP config from datad wifi.status: /state no longer carries the password */
static struct { char ssid[64], key[64]; } g_wifi_cfg[3];
static int  g_adb_pending = -1; /* optimistic ADB state while USB re-enumerates */
static int  g_usb_net = -1;     /* Type-C network sharing composition active */
static int  g_typec_source = -1; /* 1 = reverse charge peer, 0 = charge U60 */
static int  g_typec_attached = -1;
static int  g_usb_net_pending = -1;
static int  g_typec_pending = -1;
static uint32_t g_usb_net_until, g_typec_until;
static char g_dhcp_pool[80];
/* currently WiFi-associated client MACs (lowercased, comma-joined). DHCP leases
 * linger after a device leaves, so the device list filters against this (live
 * station dump) to match the vendor's "only connected" behavior. */
static char g_assoc_macs[640];

/* Is this MAC currently associated to a radio? (case-insensitive substring) */
static int mac_assoc(const char *mac)
{
    if (!g_assoc_macs[0] || !mac || !mac[0]) return 1;   /* unknown -> don't hide */
    char lc[24]; int j = 0;
    for (const char *p = mac; *p && j < 23; p++) {
        char c = *p; if (c >= 'A' && c <= 'Z') c += 32; lc[j++] = c;
    }
    lc[j] = 0;
    return strstr(g_assoc_macs, lc) != NULL;
}

static int read_long_path(const char *path, long *out)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    long v;
    int ok = fscanf(fp, "%ld", &v) == 1;
    fclose(fp);
    if (!ok) return 0;
    *out = v;
    return 1;
}

static int read_line_path(const char *path, char *out, size_t cap)
{
    FILE *fp = fopen(path, "r");
    size_t n;
    if (!fp || cap == 0) { if (fp) fclose(fp); return 0; }
    if (!fgets(out, (int)cap, fp)) { fclose(fp); return 0; }
    fclose(fp);
    n = strcspn(out, "\r\n");
    out[n] = 0;
    return 1;
}

static int boot_is_nonsilent(void)
{
    char cmdline[2048];
    FILE *fp = fopen("/proc/cmdline", "r");
    if (!fp) return 0;
    size_t n = fread(cmdline, 1, sizeof cmdline - 1, fp);
    fclose(fp);
    cmdline[n] = 0;
    return strstr(cmdline, "silent_boot.mode=nonsilent") != NULL;
}

static int read_mode_main_state(char *out, size_t cap)
{
    FILE *fp = fopen("/etc/config/zwrt_zte_mc_tmp", "r");
    char line[256];
    if (!fp || cap == 0) { if (fp) fclose(fp); return 0; }
    while (fgets(line, sizeof line, fp)) {
        if (!strstr(line, "option mode_main_state")) continue;
        char *q = strchr(line, '\'');
        if (!q) continue;
        char *e = strchr(q + 1, '\'');
        if (!e) continue;
        size_t n = (size_t)(e - (q + 1));
        if (n >= cap) n = cap - 1;
        memcpy(out, q + 1, n);
        out[n] = 0;
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

static int boot_is_charge_mode(void)
{
    char mode[64];
    if (read_mode_main_state(mode, sizeof mode)) {
        if (!strncmp(mode, "mode_power_off_", 15)) return 1;
        if (!strcmp(mode, "mode_power_on") || !strcmp(mode, "mode_power_on_charger")) return 0;
    }
    return !boot_is_nonsilent();
}

static int boot_has_external_power(void)
{
    long v;
    char st[32];
    if (read_long_path("/sys/class/power_supply/usb/online", &v) && v != 0) return 1;
    if (read_long_path("/sys/class/power_supply/charger_zte/present_mbb", &v) && v != 0) return 1;
    if (read_long_path("/sys/class/power_supply/type-c_zte/present_mbb", &v) && v != 0) return 1;
    if (read_line_path("/sys/class/power_supply/battery/status", st, sizeof st)) {
        if (!strcmp(st, "Charging") || !strcmp(st, "Full")) return 1;
    }
    if (read_long_path("/sys/class/power_supply/charger_zte/status_mbb", &v) && v == 1) return 1;
    if (read_long_path("/sys/class/power_supply/battery_zte/status_mbb", &v) && v == 1) return 1;
    if (read_long_path("/sys/class/power_supply/statistics_zte/batt_status", &v) && v == 1) return 1;
    return 0;
}

/* ---- screen lock (4-digit PIN) ---- */
static char g_pin[8];        /* stored PIN ("" = lock disabled) */
static int  g_lock_state;    /* 0=normal, 1=locked preview, 2=setup pad, 3=unlock pad */
static char g_pin_entry[8];  /* digits typed so far */
static int  g_lock_err;      /* 1 = show the unlock-pad error message */

/* "x.mu5252.html" -> "mu5252"; "x.html" -> "" */
static void page_model_tag(const char *name, char *tag, size_t cap)
{
    size_t l = strlen(name);
    const char *dot;
    tag[0] = 0;
    if (l <= 5) return;
    for (dot = name + l - 6; dot > name && *dot != '.'; dot--) {}
    if (dot > name && *dot == '.') {
        size_t n = (size_t)(name + l - 5 - (dot + 1));
        /* only model names (two letters, four digits) count as a tag */
        if (n < 6 || n >= cap || !isalpha((unsigned char)dot[1]) || !isalpha((unsigned char)dot[2]))
            return;
        for (size_t k = 2; k < 6; k++)
            if (!isdigit((unsigned char)dot[1 + k])) return;
        memcpy(tag, dot + 1, n);
        tag[n] = 0;
    }
}

/* Is name for this model, and not replaced by a model-specific copy in dir? */
static int page_for_model(const char *dir, const char *name)
{
    char tag[16], path[320];
    size_t l = strlen(name);
    page_model_tag(name, tag, sizeof tag);
    if (tag[0]) return !strcasecmp(tag, dev_tag());
    snprintf(path, sizeof path, "%s/%.*s.%s.html", dir, (int)(l - 5), name, dev_tag());
    return access(path, F_OK) != 0;
}

static void scan_pages(void)
{
    g_npages = 0;
    DIR *dp = opendir(UI_DIR);
    if (!dp) return;
    char names[24][64]; int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && n < 24) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] != '.' &&
            l > 5 && strcmp(de->d_name + l - 5, ".html") == 0 &&
            strcmp(de->d_name, "menu.html") != 0 &&
            strcmp(de->d_name, "lockscreen.html") != 0) {
            if (!g_sig_parse && strcmp(de->d_name, "01a-cell.html") == 0)
                continue;
            if (!page_for_model(UI_DIR, de->d_name))
                continue;
            strncpy(names[n], de->d_name, 63); names[n][63] = 0; n++;
        }
    }
    closedir(dp);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t);
            }
    for (int i = 0; i < n; i++)
        snprintf(g_pages[i], sizeof g_pages[i], "%s/%s", UI_DIR, names[i]);
    g_npages = n;
}

static void rescan_pages_keep_current(void)
{
    char cur_name[64];

    cur_name[0] = 0;
    if (g_npages > 0 && g_cur >= 0 && g_cur < g_npages) {
        const char *base = strrchr(g_pages[g_cur], '/');
        snprintf(cur_name, sizeof cur_name, "%s", base ? base + 1 : g_pages[g_cur]);
    }

    scan_pages();
    if (g_npages <= 0) {
        g_cur = 0;
        return;
    }
    if (cur_name[0]) {
        for (int i = 0; i < g_npages; i++) {
            const char *base = strrchr(g_pages[i], '/');
            const char *name = base ? base + 1 : g_pages[i];
            if (!strcmp(name, cur_name)) {
                g_cur = i;
                return;
            }
        }
    }
    if (g_cur >= g_npages) g_cur = g_npages - 1;
    if (g_cur < 0) g_cur = 0;
}

/* ---- value formatting ---- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void fmt_bytes(char *o, size_t n, long b) {
    if (b >= 1024L * 1024 * 1024) snprintf(o, n, "%.2f GB", b / 1073741824.0);
    else                          snprintf(o, n, "%.1f MB", b / 1048576.0);
}
static void fmt_uptime(char *o, size_t n, long s) {
    long d = s / 86400; s %= 86400;
    snprintf(o, n, "%ldd %02ld:%02ld:%02ld", d, s / 3600, (s / 60) % 60, s % 60);
}
static void fmt_uptime_s(char *o, size_t n, long s) {   /* compact, for the grid */
    long d = s / 86400, hh = (s % 86400) / 3600, mm = (s % 3600) / 60;
    if (d > 0)       snprintf(o, n, "%ldd %ldh", d, hh);
    else if (hh > 0) snprintf(o, n, "%ldh %ldm", hh, mm);
    else             snprintf(o, n, "%ldm", mm);
}
/* compact speed for the status bar: 3.0M / 80K / 224B */
static void fmt_speed_c(char *o, size_t n, long bps) {
    if (bps >= 1024 * 1024) snprintf(o, n, "%.1fM", bps / 1048576.0);
    else if (bps >= 1024)   snprintf(o, n, "%.0fK", bps / 1024.0);
    else                    snprintf(o, n, "%ldB", bps);
}
/* Single speed value with unit, honoring the bit/byte-rate setting (matches the
 * status-bar units): bits=1 -> Mbps/Kbps/bps, bits=0 -> MB/s/KB/s/B/s. */
static void fmt_speed_u(char *o, size_t n, long bps, int bits)
{
    double v = bps * (bits ? 8.0 : 1.0);
    const char *unit; double div;
    if (bits) { if (v >= 1e6) { unit = "Mbps"; div = 1e6; } else if (v >= 1e3) { unit = "Kbps"; div = 1e3; } else { unit = "bps"; div = 1; } }
    else      { if (v >= 1e6) { unit = "MB/s"; div = 1e6; } else if (v >= 1e3) { unit = "KB/s"; div = 1e3; } else { unit = "B/s"; div = 1; } }
    double val = v / div;
    if (val >= 10) snprintf(o, n, "%.0f %s", val, unit);
    else           snprintf(o, n, "%.1f %s", val, unit);
}

/* ---- {{key}} template substitution ---- */
struct kv { const char *k; const char *v; };
static void html_esc(char *dst, size_t cap, const char *src);
static int signal_value_present(const char *s);

static char *apply_template(const char *tmpl, struct kv *t, int n)
{
    static char out[1048576];
    size_t o = 0;
    for (const char *p = tmpl; *p && o < sizeof(out) - 1; ) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                int kl = (int)(end - (p + 2));
                const char *v = "";
                for (int i = 0; i < n; i++)
                    if ((int)strlen(t[i].k) == kl && strncmp(t[i].k, p + 2, kl) == 0) { v = t[i].v; break; }
                size_t vl = strlen(v);
                if (o + vl < sizeof(out) - 1) { memcpy(out + o, v, vl); o += vl; }
                else {
                    size_t room = sizeof(out) - 1 - o;
                    if (room) memcpy(out + o, v, room);
                    o = sizeof(out) - 1;
                    out[o] = 0;
                    break;
                }
                p = end + 2;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return out;
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(fp); return NULL; }
    for (;;) {
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char *nbuf = realloc(buf, ncap);
            if (!nbuf) { free(buf); fclose(fp); return NULL; }
            buf = nbuf; cap = ncap;
        }
        size_t r = fread(buf + n, 1, cap - n - 1, fp);
        n += r;
        if (r == 0) break;
    }
    if (ferror(fp)) { free(buf); fclose(fp); return NULL; }
    buf[n] = 0; fclose(fp);
    return buf;
}

static long long file_mtime_ns(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_mtime * 1000000000LL + (long long)st.st_mtim.tv_nsec;
}

/* Wallpaper and glass: wallpaper.jpg (any format stb_image reads), or a
 * live wallpaper clip wallpaper.live (see livewp.c; the plugin writes its
 * first frame as wallpaper.jpg too), and wallpaper.conf next to the binary.
 * The plugin writes them; the glass page on the screen writes the conf too.
 * conf: glass= transparency 0-100, dim= wallpaper darkening 0-90, tint=
 * glass colour (index into g_tints), blur= glass blur 0-100, live= where the
 * clip plays: 0 nowhere (its first frame stays), 1 the lock screen, 2 the
 * lock screen and the home page. */
static const struct { const char *name; unsigned rgb; } g_tints[] = {
    { "夜蓝", 0x161E2Cu },
    { "石墨", 0x1C1C1Eu },
    { "白雾", 0xE1E8F0u },
    { "主题", 0 },          /* the theme accent, darkened */
};
#define NTINTS ((int)(sizeof g_tints / sizeof g_tints[0]))
static int g_glass = 50, g_wp_dim = 30, g_glass_tint, g_glass_blur = 50, g_wp_live = 2;
static int g_wp_gen;                  /* bumped on every wallpaper (re)load */
static long long g_wp_stamp = -2;

static unsigned tint_rgb(int k)
{
    unsigned a = theme_a();
    if (g_tints[k].rgb) return g_tints[k].rgb;
    return ((a >> 16 & 255) * 2 / 5) << 16 | ((a >> 8 & 255) * 2 / 5) << 8 | (a & 255) * 2 / 5;
}

static void apply_glass(void)
{
    html_view_set_glass(g_glass);
    html_view_set_glass_tint(RGB3(tint_rgb(g_glass_tint)));
}

static long long wallpaper_stamp(void)
{
    return (file_mtime_ns(g_wp_file) * 31 + file_mtime_ns(g_wp_live_file)) * 31 + file_mtime_ns(g_wp_conf_file);
}

static void live_stop(void);

/* Reload when any of the files changed; returns 1 then. */
static int wallpaper_poll(void)
{
    long long stamp = wallpaper_stamp();
    int v;
    char line[64];
    if (stamp == g_wp_stamp) return 0;
    g_wp_stamp = stamp;
    g_glass = 50; g_wp_dim = 30; g_glass_tint = 0; g_glass_blur = 50; g_wp_live = 2;
    FILE *fp = fopen(g_wp_conf_file, "r");
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            if (sscanf(line, "glass=%d", &v) == 1) g_glass = clampi(v, 0, 100);
            else if (sscanf(line, "dim=%d", &v) == 1) g_wp_dim = clampi(v, 0, 90);
            else if (sscanf(line, "tint=%d", &v) == 1) g_glass_tint = clampi(v, 0, NTINTS - 1);
            else if (sscanf(line, "blur=%d", &v) == 1) g_glass_blur = clampi(v, 0, 100);
            else if (sscanf(line, "live=%d", &v) == 1) g_wp_live = clampi(v, 0, 2);
        }
        fclose(fp);
    }
    live_stop();
    apply_glass();
    const char *wp = g_wp_live && access(g_wp_live_file, R_OK) == 0 ? g_wp_live_file : NULL;
    if (!wp || !html_view_load_wallpaper(wp, g_wp_dim, g_glass_blur))
        html_view_load_wallpaper(access(g_wp_file, R_OK) == 0 ? g_wp_file : NULL, g_wp_dim, g_glass_blur);
    g_wp_gen++;
    return 1;
}

/* Settings changed on the screen: apply them and write the conf, without
 * reloading the wallpaper for our own write. */
static void wallpaper_save(void)
{
    apply_glass();
    FILE *fp = fopen(g_wp_conf_file, "w");
    if (!fp) return;
    fprintf(fp, "glass=%d\ndim=%d\ntint=%d\nblur=%d\nlive=%d\n", g_glass, g_wp_dim, g_glass_tint, g_glass_blur, g_wp_live);
    fclose(fp);
    g_wp_stamp = wallpaper_stamp();
}

static long long ui_scan_mtime_ns(void)
{
    long long a = file_mtime_ns(UI_DIR);
    long long b = file_mtime_ns(FUNCTIONS_DIR);
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    return a ^ (b << 1);
}

static int rescan_pages_if_changed(void)
{
    long long mtime = ui_scan_mtime_ns();
    if (mtime < 0 || mtime == g_pages_dir_mtime) return 0;
    g_pages_dir_mtime = mtime;
    rescan_pages_keep_current();
    g_render_html_cache_len = 0;
    g_render_html_cache_path[0] = 0;
    g_render_html_cache_css_mtime = -1;
    return 1;
}

static const char *read_template_cached(const char *path)
{
    long long mtime = file_mtime_ns(path);
    if (mtime < 0) return NULL;

    for (int i = 0; i < TEMPLATE_CACHE_CAP; i++) {
        struct template_cache_entry *e = &g_tmpl_cache[i];
        if (!e->used || !e->path[0]) continue;
        if (strcmp(e->path, path) != 0) continue;

        if (e->mtime_ns == mtime && e->content) return e->content;

        char *next = read_file(path);
        if (!next) return e->content;   /* keep previous cached copy if read fails */
        free(e->content);
        e->content = next;
        e->mtime_ns = mtime;
        return e->content;
    }

    char *tmpl = read_file(path);
    if (!tmpl) return NULL;

    for (int i = 0; i < TEMPLATE_CACHE_CAP; i++) {
        if (!g_tmpl_cache[i].used) {
            snprintf(g_tmpl_cache[i].path, sizeof(g_tmpl_cache[i].path), "%s", path);
            g_tmpl_cache[i].mtime_ns = mtime;
            g_tmpl_cache[i].content = tmpl;
            g_tmpl_cache[i].used = 1;
            return g_tmpl_cache[i].content;
        }
    }

    struct template_cache_entry *e = &g_tmpl_cache[g_tmpl_next];
    free(e->content);
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->mtime_ns = mtime;
    e->content = tmpl;
    e->used = 1;
    g_tmpl_next = (g_tmpl_next + 1) % TEMPLATE_CACHE_CAP;
    return e->content;
}

static int usb_pid_is(const char *needle)
{
    int ok = 0;
    char *pid = read_file("/sys/kernel/config/usb_gadget/g1/idProduct");
    if (pid) {
        ok = strstr(pid, needle) != NULL;
        free(pid);
    }
    return ok;
}

static int usb_net_has_carrier(void)
{
    int ok = 0;
    char *c = read_file("/sys/class/net/rndis0/carrier");
    if (c) { if (c[0] == '1') ok = 1; free(c); }
    c = read_file("/sys/class/net/ecm0/carrier");
    if (c) { if (c[0] == '1') ok = 1; free(c); }
    return ok;
}

static void usb_net_watchdog_stop(void)
{
    system("pid=$(cat /tmp/u60-usbnet-watchdog.pid 2>/dev/null); "
           "[ -n \"$pid\" ] && kill \"$pid\" 2>/dev/null; "
           "rm -f /tmp/u60-usbnet-watchdog.pid /tmp/u60-usbnet-enabled");
}

static void usb_net_apply_source_async(void)
{
    system("(touch /tmp/u60-typec-source /tmp/u60-usbnet-switching; "
           "date '+%F %T usbnet apply source' >>/tmp/usb.log 2>/dev/null; "
           "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
           "sleep 1; echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; "
           "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "
           "att=$(ubus call zwrt_bsp.typec list 2>/dev/null | grep cc_attch_state | grep -o '[0-9]' | head -1); "
           "pid2=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$att\" = \"1\" ] && { [ \"$pid2\" = \"0x90b1\" ] || [ \"$pid2\" = \"0x90B1\" ]; } && [ \"$e\" = \"1\" ]; then break; fi; "
           "sleep 1; done; "
           "if [ \"$att\" = \"1\" ] && { [ \"$pid2\" = \"0x90b1\" ] || [ \"$pid2\" = \"0x90B1\" ]; } && [ \"$e\" = \"1\" ]; then "
           "ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}'; "
           "sleep 1; ubus call zwrt_bsp.powerbank set '{\"state\":1}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}'; "
           "sleep 1; /sbin/usb_composition 90B1 n n y n; "
           "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" = \"1\" ] || [ \"$e\" = \"1\" ]; then break; fi; "
           "sleep 1; done; "
           "else "
           "date '+%F %T usbnet source no ecm carrier, try rndis' >>/tmp/usb.log 2>/dev/null; "
           "rm -f /tmp/u60-typec-source; "
           "/sbin/usb_composition 9057 n n y n; "
           "sleep 10; "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" != \"1\" ] && [ \"$e\" != \"1\" ]; then "
           "date '+%F %T usbnet rndis no carrier, fallback 90B1' >>/tmp/usb.log 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; fi; "
           "fi; "
           "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
}

static void usb_net_apply_sink_async(void)
{
    system("(touch /tmp/u60-usbnet-switching; rm -f /tmp/u60-typec-source; "
           "date '+%F %T usbnet apply sink' >>/tmp/usb.log 2>/dev/null; "
           "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
           "sleep 1; echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null; "
           "/sbin/usb_composition 9057 n n y n; "
           "sleep 10; "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" != \"1\" ] && [ \"$e\" != \"1\" ]; then "
           "date '+%F %T usbnet fallback 90B1' >>/tmp/usb.log 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; fi; "
           "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
}

static void usb_power_only_apply_async(int source)
{
    if (source) {
        system("(touch /tmp/u60-typec-source /tmp/u60-usbnet-switching; "
               "date '+%F %T usbpower apply source' >>/tmp/usb.log 2>/dev/null; "
               "ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}'; "
               "sleep 1; ubus call zwrt_bsp.powerbank set '{\"state\":1}'; "
               "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}'; "
               "sleep 1; SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
               "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\"; "
               "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
    } else {
        system("(touch /tmp/u60-usbnet-switching; rm -f /tmp/u60-typec-source; "
               "date '+%F %T usbpower apply sink' >>/tmp/usb.log 2>/dev/null; "
               "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
               "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
               "sleep 1; SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
               "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\"; "
               "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
    }
}

static void usb_net_watchdog_start(void)
{
    usb_net_watchdog_stop();
    system("cat >/tmp/u60-usbnet-watchdog.sh <<'EOF'\n"
           "#!/bin/sh\n"
           "touch /tmp/u60-usbnet-enabled\n"
           "echo $$ >/tmp/u60-usbnet-watchdog.pid\n"
           "while [ -f /tmp/u60-usbnet-enabled ]; do\n"
           "  if [ -f /tmp/u60-usbnet-switching ]; then sleep 2; continue; fi\n"
           "  pid=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null)\n"
           "  tc=$(ubus call zwrt_bsp.typec list 2>/dev/null)\n"
           "  att=$(echo \"$tc\" | grep cc_attch_state | grep -o '[0-9]' | head -1)\n"
           "  pr=$(echo \"$tc\" | grep power_role | grep -o 'source\\|sink' | head -1)\n"
           "  r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
           "  e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "  case \"$pid\" in 0x9057|0x90b1|0x90B1) netpid=1;; *) netpid=0;; esac\n"
            "  wantsrc=0; [ -f /tmp/u60-typec-source ] && wantsrc=1\n"
            "  rolebad=0\n"
            "  if [ \"$wantsrc\" = 1 ] && [ \"$pr\" != source ]; then rolebad=1; fi\n"
            "  if [ \"$wantsrc\" != 1 ] && [ \"$pr\" != sink ]; then rolebad=1; fi\n"
            "  if [ \"$att\" = 1 ] && { [ \"$rolebad\" = 1 ] || [ \"$netpid\" != 1 ] || { [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; }; }; then\n"
            "    date '+%F %T usbnet watchdog rearm' >>/tmp/usb.log 2>/dev/null\n"
            "    touch /tmp/u60-usbnet-switching\n"
            "    if [ \"$wantsrc\" = 1 ]; then\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":0}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null\n"
            "      /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do\n"
            "        att=$(ubus call zwrt_bsp.typec list 2>/dev/null | grep cc_attch_state | grep -o '[0-9]' | head -1)\n"
            "        pid2=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$att\" = 1 ] && { [ \"$pid2\" = 0x90b1 ] || [ \"$pid2\" = 0x90B1 ]; } && [ \"$e\" = 1 ]; then break; fi\n"
            "        sleep 1\n"
            "      done\n"
            "      if [ \"$att\" = 1 ] && { [ \"$pid2\" = 0x90b1 ] || [ \"$pid2\" = 0x90B1 ]; } && [ \"$e\" = 1 ]; then\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":1}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do\n"
            "        r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$r\" = 1 ] || [ \"$e\" = 1 ]; then break; fi\n"
            "        sleep 1\n"
            "      done\n"
            "      else\n"
            "        date '+%F %T usbnet watchdog source no ecm carrier, try rndis' >>/tmp/usb.log 2>/dev/null\n"
            "        rm -f /tmp/u60-typec-source\n"
            "        /sbin/usb_composition 9057 n n y n >>/tmp/usb.log 2>&1\n"
            "        sleep 10\n"
            "        r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; then\n"
            "          date '+%F %T usbnet watchdog rndis no carrier, fallback 90B1' >>/tmp/usb.log 2>/dev/null\n"
            "          /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "        fi\n"
            "      fi\n"
            "    else\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":0}' >/dev/null 2>&1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null\n"
            "      /sbin/usb_composition 9057 n n y n >>/tmp/usb.log 2>&1\n"
            "      sleep 10\n"
            "      r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "      e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "      if [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; then\n"
            "        date '+%F %T usbnet watchdog fallback 90B1' >>/tmp/usb.log 2>/dev/null\n"
            "        /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      fi\n"
            "    fi\n"
            "    rm -f /tmp/u60-usbnet-switching\n"
            "  fi\n"
            "  sleep 12\n"
            "done\n"
           "EOF\n"
           "chmod +x /tmp/u60-usbnet-watchdog.sh\n"
           "nohup /tmp/u60-usbnet-watchdog.sh >/tmp/u60-usbnet-watchdog.log 2>&1 &");
}

/* Read page-2 aux state into the cache: band on/off from netdev operstate (the
 * live truth on this firmware), PSM from `iw power_save`, and the DHCP pool
 * range computed live from uci (lan ip + start/limit offsets). One shell
 * round-trip, called on a throttle. */
static int signal_json_next_object(const char **pp, char *dst, size_t cap);

/* Band switches from datad wifi.state. Returns 0 when datad did not answer. */
/* datad "wifi.state" reply -> radio switch states; 0 if it is unusable */
static int wifi_state_parse(const char *reply)
{
    static char result[8192];
    char part[2048], obj[512], band[8], en[8];
    const char *p;
    int w24 = -1, w5 = -1, w6 = -1, listed = 0;

    if (!json_get(reply, "result", result, sizeof result))
        return 0;
    if (json_get(result, "radios", part, sizeof part)) {
        listed = 1;
        p = part;
        while (signal_json_next_object(&p, obj, sizeof obj)) {
            if (!json_get(obj, "band", band, sizeof band) ||
                !json_get(obj, "enabled", en, sizeof en) || !strcmp(en, "null"))
                continue;
            if (!strcmp(band, "2g")) w24 = !strcmp(en, "true");
            else if (!strcmp(band, "5g")) w5 = !strcmp(en, "true");
            else if (!strcmp(band, "6g")) w6 = !strcmp(en, "true");
        }
    }
    /* The module switch overrides the bands, except while the firmware is
     * restarting Wi-Fi (busy), when it briefly reads as off. */
    if (json_get(result, "busy", en, sizeof en) && strcmp(en, "true") &&
        json_get(result, "module", part, sizeof part) &&
        json_get(part, "enabled", en, sizeof en) && !strcmp(en, "false")) {
#ifdef DEVUI_PLAIN_OPENWRT
        /* only the bands that have a radio */
        if (w24 >= 0) w24 = 0;
        if (w5 >= 0) w5 = 0;
#else
        w24 = w5 = 0;
#endif
        if (w6 >= 0) w6 = 0;
    }
#ifdef DEVUI_PLAIN_OPENWRT
    /* The two radios of this system change bands (2.4 + 5, 5 + 6, both on 6 GHz as one): a band that is not
     * listed any more has no radio, and its switch goes away. */
    if (listed) { g_w24 = w24; g_w5 = w5; g_w6 = w6; }
#else
    (void)listed;
    if (w24 >= 0) g_w24 = w24;
    if (w5 >= 0) g_w5 = w5;
    if (w6 >= 0) g_w6 = w6;
#endif
    return 1;
}

/* Main AP ssid/key from datad wifi.status (2.4G, 5G, 6G). */
/* datad "wifi.status" reply -> SSIDs and keys per band */
static void wifi_cfg_parse(const char *reply)
{
    static const char *const sections[3] = { "main_2g", "main_5g", "main_6g" };
    char result[4096], sec[1024];

    if (!json_get(reply, "result", result, sizeof result))
        return;
    for (int i = 0; i < 3; i++) {
        g_wifi_cfg[i].ssid[0] = g_wifi_cfg[i].key[0] = 0;
        if (!json_get(result, sections[i], sec, sizeof sec)) continue;
        if (!json_get(sec, "ssid", g_wifi_cfg[i].ssid, sizeof g_wifi_cfg[i].ssid))
            g_wifi_cfg[i].ssid[0] = 0;
        if (!json_get(sec, "key", g_wifi_cfg[i].key, sizeof g_wifi_cfg[i].key))
            g_wifi_cfg[i].key[0] = 0;
    }
}

/* Password of the AP broadcasting ssid; falls back to the first one set. */
static const char *wifi_key_for(const char *ssid)
{
    const char *first = "";
    for (int i = 0; i < 3; i++) {
        if (!g_wifi_cfg[i].key[0]) continue;
        if (ssid && ssid[0] && !strcmp(g_wifi_cfg[i].ssid, ssid)) return g_wifi_cfg[i].key;
        if (!first[0]) first = g_wifi_cfg[i].key;
    }
    return first;
}

/* ---- WiFi / USB / Type-C facts the datad snapshot lacks: radio switches
 * and AP settings (datad wifi.state / wifi.status), power save, the DHCP pool,
 * associated stations, USB networking and the Type-C role. Getting them takes
 * datad ~100 ms and iw / uci / ubus process spawns, so a worker thread collects
 * them every AUX_PERIOD_MS while the screen is on and the UI thread only takes
 * the latest sample. ---- */
#define AUX_PERIOD_MS 4000
#define AUX_CFG_MS    30000
#define AUX_UCI_MS    60000
typedef struct {
    int w0_up, w2_up, psm, usbnet, typec_src, typec_attached;
    char pool[80];
    char macs[sizeof g_assoc_macs];
    int state_ok, cfg_ok;           /* replies fetched in this sample */
    char state[8192], cfg[4096];
} aux_sample_t;

static pthread_mutex_t g_aux_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_aux_cv = PTHREAD_COND_INITIALIZER;
static aux_sample_t g_aux;            /* latest sample, under g_aux_mu */
static unsigned g_aux_gen, g_aux_seen;
static int g_aux_awake = 1, g_aux_kick, g_aux_started;

static int read_line_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

static void macs_add(char *list, size_t cap, const char *mac)
{
    char lc[24];
    size_t n = 0, len = strlen(list);
    for (; mac[n] && n < sizeof lc - 2 && mac[n] != ' ' && mac[n] != '\n'; n++)
        lc[n] = (char)tolower((unsigned char)mac[n]);
    lc[n++] = ',';
    lc[n] = 0;
    if (len + n < cap) memcpy(list + len, lc, n + 1);
}

static void aux_collect(aux_sample_t *a, uint32_t now)
{
    static uint32_t uci_at, cfg_at;
    static char pool[80];   /* "a.b.c.st - a.b.c.end" */
    char v[64], cmd[512];
    int o = 0;

    memset(a, 0, sizeof *a);
    a->state_ok = datad_control("wifi.state", NULL, a->state, sizeof a->state, 800) != 0;
    if (!cfg_at || now - cfg_at >= AUX_CFG_MS) {
        a->cfg_ok = datad_control("wifi.status", NULL, a->cfg, sizeof a->cfg, 800) != 0;
        if (a->cfg_ok) cfg_at = now;
    }
    if (read_line_file("/sys/class/net/wlan0/operstate", v, sizeof v)) a->w0_up = !strcmp(v, "up");
    if (read_line_file("/sys/class/net/wlan2/operstate", v, sizeof v)) a->w2_up = !strcmp(v, "up");
    if (read_line_file("/sys/kernel/config/usb_gadget/g1/idProduct", v, sizeof v))
        a->usbnet = !strcasecmp(v, "0x9057") || !strcasecmp(v, "0x90b1");

    /* one shell for the tools; each section is tagged for the parser */
    o += snprintf(cmd + o, sizeof cmd - o,
                  "echo @PS; iw dev wlan0 get power_save 2>/dev/null || iw dev wlan2 get power_save 2>/dev/null;"
                  "echo @ST;");
    for (int i = 0; i < 4; i++) {
        char path[48];
        snprintf(path, sizeof path, "/sys/class/net/wlan%d", i);
        if (access(path, F_OK) == 0)
            o += snprintf(cmd + o, sizeof cmd - o, "iw dev wlan%d station dump 2>/dev/null;", i);
    }
    if (!dev_is_tf())   /* the TopFlow has no reverse charging */
        o += snprintf(cmd + o, sizeof cmd - o, "echo @TC; ubus call zwrt_bsp.typec list 2>/dev/null;");
    int want_uci = !uci_at || now - uci_at >= AUX_UCI_MS;
    if (want_uci)
        snprintf(cmd + o, sizeof cmd - o,
                 "echo @UCI; uci -q get network.lan.ipaddr; uci -q get dhcp.lan.start; uci -q get dhcp.lan.limit");

    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[256], sec = 0, ip[20] = "";
        int uci_n = 0, st = 0, lim = 0;
        a->typec_src = -1;
        while (fgets(line, sizeof line, fp)) {
            if (line[0] == '@') { sec = line[1]; continue; }
            if (sec == 'P' && strstr(line, "Power save:")) a->psm = strstr(line, ": on") != NULL;
            else if (sec == 'S' && !strncmp(line, "Station ", 8)) macs_add(a->macs, sizeof a->macs, line + 8);
            else if (sec == 'T' && a->typec_src < 0 && strstr(line, "power_role")) {
                if (strstr(line, "source")) a->typec_src = 1;
                else if (strstr(line, "sink")) a->typec_src = 0;
            } else if (sec == 'T' && strstr(line, "cc_attch_state")) {
                const char *d = strpbrk(line, "0123456789");
                if (d) a->typec_attached = *d != '0';
            } else if (sec == 'U') {
                line[strcspn(line, "\r\n")] = 0;
                if (uci_n == 0) snprintf(ip, sizeof ip, "%.15s", line);
                else if (uci_n == 1) st = atoi(line);
                else if (uci_n == 2) lim = atoi(line);
                uci_n++;
            }
        }
        pclose(fp);
        if (want_uci) {
            uci_at = now;
            char *dot = strrchr(ip, '.');
            pool[0] = 0;
            if (dot && st > 0) {
                int end = st + lim - 1;
                if (end > 254) end = 254;
                *dot = 0;
                snprintf(pool, sizeof pool, "%s.%d - %s.%d", ip, st, ip, end);
            }
        }
    }
    snprintf(a->pool, sizeof a->pool, "%s", pool);

    /* ARP-complete entries cover LAN/USB clients (leases alone linger 24h) */
    FILE *arp = fopen("/proc/net/arp", "r");
    if (arp) {
        char line[256], ipa[64], hw[32], flags[16], mac[32];
        int first = 1;
        while (fgets(line, sizeof line, arp)) {
            if (first) { first = 0; continue; }
            if (sscanf(line, "%63s %31s %15s %31s", ipa, hw, flags, mac) == 4 && strcmp(flags, "0x0"))
                macs_add(a->macs, sizeof a->macs, mac);
        }
        fclose(arp);
    }
}

static void *aux_worker(void *arg)
{
    static aux_sample_t smp;
    (void)arg;
    for (;;) {
        aux_collect(&smp, millis());
        pthread_mutex_lock(&g_aux_mu);
        g_aux = smp;
        g_aux_gen++;
        while (!g_aux_kick) {
            if (!g_aux_awake) { pthread_cond_wait(&g_aux_cv, &g_aux_mu); continue; }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += AUX_PERIOD_MS / 1000;
            if (pthread_cond_timedwait(&g_aux_cv, &g_aux_mu, &ts) != 0) break;
        }
        g_aux_kick = 0;
        pthread_mutex_unlock(&g_aux_mu);
    }
    return NULL;
}

/* Screen on: sample now and keep sampling; off: pause the worker. */
static void aux_set_awake(int awake)
{
    pthread_mutex_lock(&g_aux_mu);
    if (awake && !g_aux_awake) g_aux_kick = 1;
    g_aux_awake = awake;
    pthread_cond_signal(&g_aux_cv);
    pthread_mutex_unlock(&g_aux_mu);
}

static void aux_start(void)
{
    pthread_t t;
    if (g_aux_started) return;
    if (pthread_create(&t, NULL, aux_worker, NULL) == 0) {
        pthread_detach(t);
        g_aux_started = 1;
    }
}

/* Take a new sample from the worker, if there is one. */
static void aux_apply(void)
{
    static aux_sample_t a;
    pthread_mutex_lock(&g_aux_mu);
    int fresh = g_aux_gen != g_aux_seen;
    g_aux_seen = g_aux_gen;
    if (fresh) a = g_aux;
    pthread_mutex_unlock(&g_aux_mu);
    if (!fresh) return;

    /* right after a switch the radios settle; keep the state the tap set */
    int settled = (int32_t)(millis() - g_wifi_hold_until) >= 0;
    int have_datad = settled && a.state_ok && wifi_state_parse(a.state);
    if (a.cfg_ok) wifi_cfg_parse(a.cfg);
    /* operstate is only the fallback for a datad that is not answering
     * (U60 Pro: wlan0 = 2.4G, wlan2 = 5G; TopFlow: wlan0 = 5G) */
    if (!have_datad && settled) {
        if (dev_is_tf()) g_w5 = a.w0_up;
        else { g_w24 = a.w0_up; g_w5 = a.w2_up; }
    }
    g_wpsm = a.psm;
    if (a.pool[0]) snprintf(g_dhcp_pool, sizeof g_dhcp_pool, "%s", a.pool);
    snprintf(g_assoc_macs, sizeof g_assoc_macs, "%s", a.macs);
    g_usb_net = a.usbnet;
    if (a.typec_src >= 0) g_typec_source = a.typec_src;
    g_typec_attached = a.typec_attached;
}

/* ---- screen lock (PIN) persistence. The PIN lives in a dotfile under the UI
 * dir (which always exists) so it survives reboots and isn't clobbered by
 * pushing the .html pages. lock_enabled() == "a PIN is set". ---- */
#define LOCK_PIN_FILE g_lockpin_file
static int lock_enabled(void) { return g_pin[0] != 0; }
static void load_pin(void)
{
    g_pin[0] = 0;
    FILE *fp = fopen(LOCK_PIN_FILE, "r");
    if (!fp) return;
    char b[16]; int o = 0;
    if (fgets(b, sizeof b, fp))
        for (const char *p = b; *p && o < 4; p++)
            if (*p >= '0' && *p <= '9') g_pin[o++] = *p;
    g_pin[o] = 0;
    if (o != 4) g_pin[0] = 0;   /* ignore anything malformed */
    fclose(fp);
    g_refresh_ms = normalize_refresh_ms(g_refresh_ms);
}
static void save_pin(const char *pin)
{
    snprintf(g_pin, sizeof g_pin, "%s", pin);
    FILE *fp = fopen(LOCK_PIN_FILE, "w");
    if (fp) { fprintf(fp, "%s\n", pin); fclose(fp); }
}
static void clear_pin(void) { g_pin[0] = 0; remove(LOCK_PIN_FILE); }
/* Enter the lock pad. setup=1 -> set a new PIN; setup=0 -> unlock prompt. */
/* The screen locks as on the U30 Pro, PIN or not: at start, on every screen
 * off, and after a minute untouched when it never sleeps. The locked preview
 * is page 1 with live data and no actions. With a PIN a swipe or a short hold
 * opens the pad; without one only a LOCK_HOLD_MS hold unlocks, so a pocket or
 * an NFC tap cannot flip switches. */
#define LOCK_HOLD_MS     600
#define LOCK_PAD_HOLD_MS 450
#define LOCK_IDLE_MS     60000
static int g_lock_hold;      /* 0..100: progress of the unlock hold, drawn on the badge */

static void enter_lock(int setup)
{
    g_lock_state = setup ? 2 : 1;
    g_lock_hold = 0;
    g_pin_entry[0] = 0; g_lock_err = 0; g_scroll = 0;
}

/* ---- persisted UI settings (theme / speed unit / auto-off / refresh interval).
 * Stored next to the binary (devui.conf in the install directory) so they survive reinstalling the
 * binary and re-pushing UI files (the PIN persists separately in .lockpin). */
#define CONF_FILE g_conf_file
static void load_conf(void)
{
    FILE *fp = fopen(CONF_FILE, "r");
    if (!fp) return;
    char line[64], sval[16]; int v;
    while (fgets(line, sizeof line, fp)) {
        if      (sscanf(line, "theme=%d", &v) == 1)      { if (v >= 0 && v < NTHEMES) g_theme = v; }
        else if (sscanf(line, "speed_bits=%d", &v) == 1) g_speed_bits = !!v;
        else if (sscanf(line, "show_batpct=%d", &v) == 1) g_show_batpct = !!v;
        else if (sscanf(line, "show_sbspeed=%d", &v) == 1) g_show_sbspeed = !!v;
        else if (sscanf(line, "text_adapt=%d", &v) == 1) g_text_adapt = !!v;
        else if (sscanf(line, "autooff=%d", &v) == 1)    g_autooff_ms = v;
        else if (sscanf(line, "refresh_ms=%d", &v) == 1) g_refresh_ms = normalize_refresh_ms(v);
        else if (sscanf(line, "sig_read=%d", &v) == 1)   g_sig_read = !!v;
        else if (sscanf(line, "sig_parse=%d", &v) == 1)  g_sig_parse = !!v;
        else if (sscanf(line, "bright=%d", &v) == 1)     g_saved_bright = v;
        else if (sscanf(line, "auto_bright=%d", &v) == 1) g_auto_bright = !!v;
        else if (sscanf(line, "st_src=%15s", sval) == 1) snprintf(g_st_src, sizeof g_st_src, "%s", sval);
        else if (sscanf(line, "st_dir=%15s", sval) == 1) snprintf(g_st_dir, sizeof g_st_dir, "%s", sval);
        else if (sscanf(line, "st_dur=%d", &v) == 1)     g_st_dur = v;
    }
    fclose(fp);
}
static void save_conf(void)
{
    FILE *fp = fopen(CONF_FILE, "w");
    if (!fp) return;
    fprintf(fp,
            "theme=%d\nspeed_bits=%d\nshow_batpct=%d\nshow_sbspeed=%d\ntext_adapt=%d\nautooff=%d\nrefresh_ms=%d\nsig_read=%d\nsig_parse=%d\nbright=%d\nauto_bright=%d\nst_src=%s\nst_dir=%s\nst_dur=%d\n",
            g_theme, g_speed_bits, g_show_batpct, g_show_sbspeed, g_text_adapt, g_autooff_ms, g_refresh_ms,
            g_sig_read, g_sig_parse, backlight_get(), g_auto_bright, g_st_src, g_st_dir, g_st_dur);
    fclose(fp);
}
static void screen_settings_push(void);
static void save_settings(void)
{
    save_conf();
    screen_settings_push();
}

/* Where the backend keeps the screen settings itself (UCI u60pro.screen on
 * plain OpenWrt, also edited in the web interface), those win over devui.conf:
 * a change made there is taken over while running, and one made on the screen
 * is written back. */
static int g_sys_bright = -1, g_sys_timeout = -1, g_sys_screen;
static void screen_settings_pull(void)
{
    int b, t;
    if (!data_screen_settings(&b, &t)) return;
    g_sys_screen = 1;
    if (t >= 0 && t != g_sys_timeout) {
        g_sys_timeout = t;
        g_autooff_ms = t * 1000;
    }
    if (b >= 1 && b != g_sys_bright && backlight_is_on()) {   /* a dark screen takes it over when it is lit */
        int max = backlight_max(), lvl = (max * b + 50) / 100;
        g_sys_bright = b;
        backlight_set(lvl < 1 ? 1 : lvl);
    }
}
static void screen_settings_push(void)
{
    int max = backlight_max(), pct;
    if (!g_sys_screen) return;
    pct = max > 0 ? (backlight_get() * 100 + max / 2) / max : -1;
    if (pct < 1) pct = 1;
    g_sys_bright = pct;
    g_sys_timeout = g_autooff_ms / 1000;
    data_screen_settings_save(pct, g_sys_timeout);
}

static const char *speedtest_norm_src(const char *src)
{
    if (!strcmp(src, "cnspeed")) return "cnspeed";
    if (!strcmp(src, "ookla")) return "ookla";
    if (!strcmp(src, "cdn")) return "cdn";
    return "auto";
}

static const char *speedtest_norm_dir(const char *dir)
{
    if (!strcmp(dir, "dl")) return "dl";
    if (!strcmp(dir, "ul")) return "ul";
    return "both";
}

static int speedtest_norm_dur(int sec)
{
    if (sec == 0) return 0;   /* 0 = loop until stopped by the user */
    if (sec == 10 || sec == 15 || sec == 20) return sec;
    return 15;
}

static int speedtest_loop_mode(void)
{
    return g_st_dur == 0;
}

static void speedtest_apply_saved_prefs(void)
{
    snprintf(g_st_src, sizeof g_st_src, "%s", speedtest_norm_src(g_st_src));
    snprintf(g_st_dir, sizeof g_st_dir, "%s", speedtest_norm_dir(g_st_dir));
    g_st_dur = speedtest_norm_dur(g_st_dur);
}

static int speedtest_binary_ready(void)
{
    return access(SPEEDTEST_BIN, X_OK) == 0;
}

/* Is a better-speedtest process running (started here or elsewhere)? Polled
 * every 300 ms while the page is open, and a /proc walk over a few hundred
 * processes costs 10-25 ms on TopFlow, so the pid found last is checked
 * first and a full walk that found nothing is repeated at most every
 * ST_SCAN_MS (speedtest_start forces the next one). */
#define ST_SCAN_MS 2000
static long g_st_pid;
static uint32_t g_st_scan_at;
static int g_st_scan_due = 1;

static int speedtest_is_pid(long pid)
{
    char path[48], comm[32];
    snprintf(path, sizeof path, "/proc/%ld/comm", pid);
    return pid > 0 && read_line_file(path, comm, sizeof comm) && !strcmp(comm, "better-speedtes");
}

static int speedtest_running(void)
{
    if (!speedtest_binary_ready()) return 0;
    if (g_st_pid > 0 && speedtest_is_pid(g_st_pid)) return 1;
    g_st_pid = 0;
    uint32_t now = millis();
    if (!g_st_scan_due && (uint32_t)(now - g_st_scan_at) < ST_SCAN_MS) return 0;
    g_st_scan_due = 0;
    g_st_scan_at = now;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        long pid = strtol(e->d_name, NULL, 10);
        if (speedtest_is_pid(pid)) { g_st_pid = pid; break; }
    }
    closedir(d);
    return g_st_pid > 0;
}

static int speedtest_loop_running(void)
{
    long pid = 0;
    if (access(SPEEDTEST_LOOP_FLAG, F_OK) != 0) return 0;
    if (read_long_path(SPEEDTEST_LOOP_PID, &pid) && pid > 0 && kill((pid_t)pid, 0) == 0)
        return 1;
    return speedtest_running();
}

static void speedtest_clear_data(void)
{
    g_st_have_result = 0;
    g_st_phase[0] = 0;
    g_st_msg[0] = 0;
    g_st_err[0] = 0;
    g_st_node[0] = 0;
    g_st_source[0] = 0;
    g_st_carrier[0] = 0;
    g_st_city[0] = 0;
    g_st_ulmsg[0] = 0;
    g_st_cur_mbps = g_st_cur_peak = 0;
    g_st_dl_avg = g_st_dl_peak = 0;
    g_st_ul_avg = g_st_ul_peak = 0;
    g_st_ping = g_st_jitter = 0;
}

static void speedtest_hist_clear(void)
{
    g_st_dl_n = 0;
    g_st_ul_n = 0;
}

static void speedtest_hist_push(int *hist, int *n, double mbps)
{
    int v;
    if (mbps < 0.0) mbps = 0.0;
    v = (int)(mbps * 10.0 + 0.5);
    if (*n < ST_HIST) {
        hist[(*n)++] = v;
    } else {
        memmove(hist, hist + 1, (ST_HIST - 1) * sizeof hist[0]);
        hist[ST_HIST - 1] = v;
    }
}

static int speedtest_json_str(const char *line, const char *key, char *out, size_t cap)
{
    char needle[48];
    const char *p;
    size_t o = 0;
    snprintf(needle, sizeof needle, "\"%s\":", key);
    p = strstr(line, needle);
    if (!p || cap == 0) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
    return 1;
}

static int speedtest_json_num(const char *line, const char *key, double *out)
{
    char needle[48];
    char *end = NULL;
    const char *p;
    snprintf(needle, sizeof needle, "\"%s\":", key);
    p = strstr(line, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    *out = strtod(p, &end);
    return end && end != p;
}

static const char *speedtest_phase_label(const char *phase)
{
    if (!strcmp(phase, "download")) return "下行测速";
    if (!strcmp(phase, "upload")) return "上行测速";
    if (!strcmp(phase, "start")) return "准备测速";
    if (!strcmp(phase, "geo")) return "定位中";
    if (!strcmp(phase, "node")) return "选择节点";
    if (!strcmp(phase, "probe")) return "探测中";
    if (!strcmp(phase, "result")) return "测速完成";
    if (!strcmp(phase, "error")) return "测速失败";
    return "测速中";
}

static void speedtest_parse_line(const char *line)
{
    char phase[24];
    if (!line || !line[0] || !strcmp(line, "DONE")) return;
    if (!speedtest_json_str(line, "phase", phase, sizeof phase)) return;
    snprintf(g_st_phase, sizeof g_st_phase, "%s", phase);
    if (!strcmp(phase, "download") || !strcmp(phase, "upload")) {
        (void)speedtest_json_num(line, "mbps", &g_st_cur_mbps);
        (void)speedtest_json_num(line, "peak", &g_st_cur_peak);
        if (!strcmp(phase, "download"))
            speedtest_hist_push(g_st_dl_hist, &g_st_dl_n, g_st_cur_mbps);
        else
            speedtest_hist_push(g_st_ul_hist, &g_st_ul_n, g_st_cur_mbps);
        g_st_state = ST_STATE_RUNNING;
        return;
    }
    if (!strcmp(phase, "result")) {
        g_st_have_result = 1;
        (void)speedtest_json_str(line, "node", g_st_node, sizeof g_st_node);
        (void)speedtest_json_str(line, "source", g_st_source, sizeof g_st_source);
        (void)speedtest_json_str(line, "carrier", g_st_carrier, sizeof g_st_carrier);
        (void)speedtest_json_str(line, "city", g_st_city, sizeof g_st_city);
        (void)speedtest_json_str(line, "ul_msg", g_st_ulmsg, sizeof g_st_ulmsg);
        (void)speedtest_json_num(line, "dl_avg", &g_st_dl_avg);
        (void)speedtest_json_num(line, "dl_peak", &g_st_dl_peak);
        (void)speedtest_json_num(line, "ul_avg", &g_st_ul_avg);
        (void)speedtest_json_num(line, "ul_peak", &g_st_ul_peak);
        (void)speedtest_json_num(line, "ping", &g_st_ping);
        (void)speedtest_json_num(line, "jitter", &g_st_jitter);
        g_st_state = ST_STATE_DONE;
        return;
    }
    if (speedtest_json_str(line, "msg", g_st_msg, sizeof g_st_msg)) {
        if (!strcmp(phase, "error")) {
            snprintf(g_st_err, sizeof g_st_err, "Test failed");
            g_st_state = ST_STATE_FAIL;
        } else {
            g_st_state = ST_STATE_RUNNING;
        }
    }
}

static int speedtest_poll(uint32_t now_ms)
{
    int prev_state = g_st_state, prev_installed = g_st_installed, prev_have_result = g_st_have_result;
    double prev_cur = g_st_cur_mbps, prev_peak = g_st_cur_peak, prev_dl = g_st_dl_avg, prev_ul = g_st_ul_avg;
    double prev_ping = g_st_ping, prev_jitter = g_st_jitter;
    char prev_phase[24], prev_msg[160], prev_err[160], prev_node[96], prev_source[32], prev_ulmsg[96];
    snprintf(prev_phase, sizeof prev_phase, "%s", g_st_phase);
    snprintf(prev_msg, sizeof prev_msg, "%s", g_st_msg);
    snprintf(prev_err, sizeof prev_err, "%s", g_st_err);
    snprintf(prev_node, sizeof prev_node, "%s", g_st_node);
    snprintf(prev_source, sizeof prev_source, "%s", g_st_source);
    snprintf(prev_ulmsg, sizeof prev_ulmsg, "%s", g_st_ulmsg);

    g_st_installed = speedtest_binary_ready();
    if (g_st_installed < 0) g_st_installed = 0;
    {
        int running = speedtest_running();
        int loop_running = speedtest_loop_running();
        char *log = read_file(SPEEDTEST_LOG);
        if (loop_running) running = 1;
        if (log && log[0]) {
            char *save = NULL;
            speedtest_clear_data();
            speedtest_hist_clear();
            for (char *line = strtok_r(log, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
                speedtest_parse_line(line);
            if (running) {
                if (!g_st_have_result && g_st_state != ST_STATE_FAIL)
                    g_st_state = ST_STATE_RUNNING;
                if (loop_running && g_st_have_result)
                    g_st_state = ST_STATE_RUNNING;
            } else if (g_st_have_result) {
                g_st_state = ST_STATE_DONE;
            } else if (g_st_err[0] || g_st_phase[0] || g_st_msg[0]) {
                if (!g_st_err[0]) snprintf(g_st_err, sizeof g_st_err, "测速结束但没有结果");
                g_st_state = ST_STATE_FAIL;
            } else {
                g_st_state = g_st_installed ? ST_STATE_IDLE : ST_STATE_MISSING;
            }
        } else if (!running) {
            if (!g_st_installed) g_st_state = ST_STATE_MISSING;
            else if (g_st_state == ST_STATE_RUNNING) g_st_state = ST_STATE_IDLE;
            else if (g_st_state == ST_STATE_MISSING) g_st_state = ST_STATE_IDLE;
        } else {
            g_st_state = ST_STATE_RUNNING;
            if (!g_st_msg[0] && !g_st_phase[0]) snprintf(g_st_msg, sizeof g_st_msg, "测速中");
        }
        free(log);
    }
    g_st_poll_at = now_ms;
    return prev_state != g_st_state || prev_installed != g_st_installed || prev_have_result != g_st_have_result ||
           strcmp(prev_phase, g_st_phase) || strcmp(prev_msg, g_st_msg) || strcmp(prev_err, g_st_err) ||
           strcmp(prev_node, g_st_node) || strcmp(prev_source, g_st_source) || strcmp(prev_ulmsg, g_st_ulmsg) ||
           fabs(prev_cur - g_st_cur_mbps) > 0.04 || fabs(prev_peak - g_st_cur_peak) > 0.04 ||
           fabs(prev_dl - g_st_dl_avg) > 0.04 || fabs(prev_ul - g_st_ul_avg) > 0.04 ||
           fabs(prev_ping - g_st_ping) > 0.04 || fabs(prev_jitter - g_st_jitter) > 0.04;
}

static void speedtest_start(void)
{
    char cmd[768], extra[32] = "";
    speedtest_apply_saved_prefs();
    g_st_installed = speedtest_binary_ready();
    if (!g_st_installed) {
        speedtest_clear_data();
        speedtest_hist_clear();
        snprintf(g_st_err, sizeof g_st_err, "未安装测速器");
        g_st_state = ST_STATE_MISSING;
        return;
    }
    if (!strcmp(g_st_dir, "dl")) snprintf(extra, sizeof extra, " --no-upload");
    else if (!strcmp(g_st_dir, "ul")) snprintf(extra, sizeof extra, " --no-download");
    speedtest_clear_data();
    speedtest_hist_clear();
    snprintf(g_st_msg, sizeof g_st_msg, "正在启动");
    g_st_state = ST_STATE_RUNNING;
    if (speedtest_loop_mode()) {
        snprintf(cmd, sizeof cmd,
                 "rm -f " SPEEDTEST_LOG " " SPEEDTEST_LOOP_PID "; : > " SPEEDTEST_LOG "; touch " SPEEDTEST_LOOP_FLAG "; "
                 "(while [ -f " SPEEDTEST_LOOP_FLAG " ]; do "
                 ": > " SPEEDTEST_LOG "; "
                 "echo '{\"phase\":\"start\",\"msg\":\"循环测速中，手动停止前不会自动结束\"}' >> " SPEEDTEST_LOG "; "
                 SPEEDTEST_BIN " test --json --src %s --dur 15%s >> " SPEEDTEST_LOG " 2>&1; "
                 "sleep 1; "
                 "done) & echo $! > " SPEEDTEST_LOOP_PID,
                 g_st_src, extra);
    } else {
        unlink(SPEEDTEST_LOOP_FLAG);
        unlink(SPEEDTEST_LOOP_PID);
        snprintf(cmd, sizeof cmd,
                 "rm -f " SPEEDTEST_LOG "; : > " SPEEDTEST_LOG "; " SPEEDTEST_BIN " test --json --src %s --dur %d%s > " SPEEDTEST_LOG " 2>&1 &",
                 g_st_src, g_st_dur, extra);
    }
    g_st_scan_due = 1;                         /* look for the new process on the next poll */
    if (system(cmd) != 0) {
        speedtest_hist_clear();
        snprintf(g_st_err, sizeof g_st_err, "启动失败");
        g_st_state = ST_STATE_FAIL;
    }
}

static void speedtest_stop(void)
{
    unlink(SPEEDTEST_LOOP_FLAG);
    system("if [ -s " SPEEDTEST_LOOP_PID " ]; then kill -9 $(cat " SPEEDTEST_LOOP_PID ") 2>/dev/null || true; fi; "
           "for p in $(pidof better-speedtest 2>/dev/null); do kill -9 $p; done");
    unlink(SPEEDTEST_LOOP_PID);
    unlink(SPEEDTEST_LOG);
    speedtest_clear_data();
    speedtest_hist_clear();
    g_st_installed = speedtest_binary_ready();
    g_st_state = g_st_installed ? ST_STATE_IDLE : ST_STATE_MISSING;
    snprintf(g_st_msg, sizeof g_st_msg, "已停止");
}

static int path_is_speedtest(const char *path)
{
    const char *base = strrchr(path, '/');
    if (!base) base = path; else base++;
    return !strcmp(base, "07-speedtest.html") || !strcmp(base, "speedtest.html") ||
           strstr(base, "-speedtest.html") != NULL;
}

static int path_is_signal_detail(const char *path)
{
    const char *base = strrchr(path, '/');
    if (!base) base = path; else base++;
    return !strcmp(base, "01a-cell.html") || strstr(base, "cell.html") != NULL;
}

static int path_is_signal_home(const char *path)
{
    const char *base = strrchr(path, '/');
    if (!base) base = path; else base++;
    return !strcmp(base, "01-signal.html");
}

static int path_is_signal_page(const char *path)
{
    return path_is_signal_home(path) || path_is_signal_detail(path);
}

static int signal_live_enabled(void)
{
    /* Raw reads may stay on for datad, but UI decoded cards obey parse only. */
    return g_sig_parse;
}

static int speedtest_page_index(void)
{
    for (int i = 0; i < g_npages; i++)
        if (path_is_speedtest(g_pages[i])) return i;
    return -1;
}

static int subpage_name_ok(const char *name)
{
    size_t l;
    if (!name || !*name) return 0;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return 0;
    if (strpbrk(name, "\"'<>&")) return 0;
    l = strlen(name);
    return l > 5 && strcmp(name + l - 5, ".html") == 0;
}

/* Pulled sheets, as on the U30 Pro: the control center comes down from the top
 * edge, the SMS list up from the bottom edge. Each is an ordinary subpage
 * (taps, scrolling and act:backfunc work as usual); g_sheet only remembers
 * which edge pull opened it so the reverse pull can close it. */
#define SHEET_CONTROL 1
#define SHEET_SMS     2
static int g_sheet;

static int subpage_open(const char *name)
{
    char path[300];
    size_t l;
    g_sheet = 0;
    if (!subpage_name_ok(name)) return 0;
    /* a model-specific copy (wifi.mu5252.html) wins over the shared page */
    l = strlen(name);
    snprintf(path, sizeof path, "%s/subpages/%.*s.%s.html", UI_DIR, (int)(l - 5), name, dev_tag());
    if (access(path, R_OK) != 0)
        snprintf(path, sizeof path, "%s/subpages/%s", UI_DIR, name);
    if (access(path, R_OK) != 0) return 0;
    snprintf(g_subpage, sizeof g_subpage, "%s", name);
    snprintf(g_subpage_path, sizeof g_subpage_path, "%s", path);
    g_scroll = 0;
    return 1;
}

static int function_page_open(const char *name)
{
    char path[300];
    if (!subpage_name_ok(name)) return 0;
    snprintf(path, sizeof path, "%s/%s", FUNCTIONS_DIR, name);
    if (access(path, R_OK) != 0) return 0;
    snprintf(g_subpage, sizeof g_subpage, "func:%s", name);
    snprintf(g_subpage_path, sizeof g_subpage_path, "%s", path);
    g_scroll = 0;
    return 1;
}

static void subpage_close(void)
{
    g_sheet = 0;
    g_subpage[0] = 0;
    g_subpage_path[0] = 0;
    g_scroll = 0;
}

static const char *active_page_path(void)
{
    if (g_subpage[0]) return g_subpage_path;
    if (g_npages > 0 && g_cur >= 0 && g_cur < g_npages) return g_pages[g_cur];
    return "";
}

static int path_is_function_page(const char *path)
{
    size_t l = strlen(FUNCTIONS_DIR);
    return path && !strncmp(path, FUNCTIONS_DIR, l) && path[l] == '/';
}

static void function_label_from_name(char *dst, size_t cap, const char *name)
{
    size_t l;
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!name) return;
    l = strlen(name);
    if (l > 5 && !strcmp(name + l - 5, ".html")) l -= 5;
    if (l >= cap) l = cap - 1;
    memcpy(dst, name, l);
    dst[l] = 0;
    for (size_t i = 0; dst[i]; i++)
        if (dst[i] == '_' || dst[i] == '-') dst[i] = ' ';
}

static int function_title_from_html(const char *html, char *dst, size_t cap)
{
    const char *p, *gt, *end;
    size_t l;
    if (!html || !dst || cap == 0) return 0;
    p = strcasestr(html, "<title");
    if (!p) return 0;
    gt = strchr(p, '>');
    if (!gt) return 0;
    gt++;
    end = strcasestr(gt, "</title>");
    if (!end || end <= gt) return 0;
    while (gt < end && (*gt == ' ' || *gt == '\t' || *gt == '\r' || *gt == '\n')) gt++;
    while (end > gt && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    l = (size_t)(end - gt);
    if (l >= cap) l = cap - 1;
    memcpy(dst, gt, l);
    dst[l] = 0;
    return dst[0] != 0;
}

static void function_title_for_file(char *dst, size_t cap, const char *path, const char *name)
{
    char *html = read_file(path);
    if (html) {
        if (function_title_from_html(html, dst, cap)) {
            free(html);
            return;
        }
        free(html);
    }
    function_label_from_name(dst, cap, name);
}

static int g_st_home_open = 0;

static const char *speedtest_home_button_html(void)
{
    if (g_lock_state || !speedtest_binary_ready()) return "";
    return g_st_home_open ?
           "<a href='act:sttoggle' class='speed-home on'>收起测速</a>" :
           "<a href='act:sttoggle' class='speed-home'>网络测速</a>";
}

static const char *speedtest_function_tile_html(void)
{
    if (!speedtest_binary_ready()) return "";
    return "<a href='act:sub:speedtest.html' class='func-tile ft-speed'>"
           "<span class='func-ic' id='fic-speed'></span><span class='func-go'>&gt;</span>"
           "<span class='func-name'>网络测速</span>"
           "<span class='func-desc'>打开完整测速仪表盘</span>"
           "</a>";
}

static const char *custom_function_tiles_html(void)
{
    static char buf[8192];
    char names[32][64];
    int n = 0;
    DIR *dp;
    struct dirent *de;
    int o = 0;

    buf[0] = 0;
    mkdir(FUNCTIONS_DIR, 0755);
    dp = opendir(FUNCTIONS_DIR);
    if (!dp) return buf;
    while ((de = readdir(dp)) && n < 32) {
        if (de->d_name[0] == '.') continue;
        if (!subpage_name_ok(de->d_name)) continue;
        snprintf(names[n], sizeof names[n], "%s", de->d_name);
        n++;
    }
    closedir(dp);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t);
            }

    for (int i = 0; i < n; i++) {
        char path[300], raw[96], title[160], href[160];
        snprintf(path, sizeof path, "%s/%s", FUNCTIONS_DIR, names[i]);
        function_title_for_file(raw, sizeof raw, path, names[i]);
        html_esc(title, sizeof title, raw);
        html_esc(href, sizeof href, names[i]);
        o += snprintf(buf + o, sizeof buf - o,
                      "<a href=\"act:func:%s\" class=\"func-tile func-custom\">"
                      "<span class=\"func-ic\" id=\"fic-custom-%d\"></span><span class=\"func-go\">&gt;</span>"
                      "<span class=\"func-name\">%s</span>"
                      "<span class=\"func-desc\">自定义页面</span>"
                      "</a>",
                      href, i, title);
        if (o >= (int)sizeof(buf) - 256) break;
    }
    return buf;
}

static const char *speedtest_src_html(void)
{
    static const struct { const char *v; const char *lab; } opts[4] = {
        { "auto", "自动" }, { "cnspeed", "全球网测" }, { "ookla", "Ookla" }, { "cdn", "CDN" } };
    static char buf[512];
    int o = snprintf(buf, sizeof buf, "<div class='stseg'>");
    for (int i = 0; i < 4; i++)
        o += snprintf(buf + o, sizeof buf - o, "<a href='act:stsrc:%s' class='stsegc%s'>%s</a>",
                      opts[i].v, !strcmp(g_st_src, opts[i].v) ? " on" : "", opts[i].lab);
    snprintf(buf + o, sizeof buf - o, "</div>");
    return buf;
}

static const char *speedtest_dir_html(void)
{
    static const struct { const char *v; const char *lab; } opts[3] = {
        { "both", "上下行" }, { "dl", "下行" }, { "ul", "上行" } };
    static char buf[384];
    int o = snprintf(buf, sizeof buf, "<div class='stseg'>");
    for (int i = 0; i < 3; i++)
        o += snprintf(buf + o, sizeof buf - o, "<a href='act:stdir:%s' class='stsegc%s'>%s</a>",
                      opts[i].v, !strcmp(g_st_dir, opts[i].v) ? " on" : "", opts[i].lab);
    snprintf(buf + o, sizeof buf - o, "</div>");
    return buf;
}

static const char *speedtest_dur_html(void)
{
    static const struct { int sec; const char *lab; } opts[4] = {
        { 10, "10s" }, { 15, "15s" }, { 20, "20s" }, { 0, "循环" } };
    static char buf[420];
    int o = snprintf(buf, sizeof buf, "<div class='stseg'>");
    for (int i = 0; i < 4; i++)
        o += snprintf(buf + o, sizeof buf - o, "<a href='act:stdur:%d' class='stsegc%s'>%s</a>",
                      opts[i].sec,
                      g_st_dur == opts[i].sec ? (opts[i].sec == 0 ? " loop on" : " on") : (opts[i].sec == 0 ? " loop" : ""),
                      opts[i].lab);
    snprintf(buf + o, sizeof buf - o, "</div>");
    return buf;
}

static const char *speedtest_loop_hint_html(void)
{
    if (!speedtest_loop_mode()) return "";
    return "<div class='stloop-warn'>循环测速不会自动停止，注意流量消耗。</div>";
}

static const char *speedtest_install_html(void)
{
    static char buf[256], esc[160];
    if (g_st_installed > 0) return "";
    html_esc(esc, sizeof esc, SPEEDTEST_BIN);
    snprintf(buf, sizeof buf, "<div class='sthint bad'>未安装：<span class='mono'>%s</span></div>", esc);
    return buf;
}

static const char *speedtest_action_html(void)
{
    if (g_st_installed <= 0) return "<span class='stbtn dis'>安装后可用</span>";
    if (g_st_state == ST_STATE_RUNNING) return "<a href='act:ststop' class='stbtn stop'>停止测速</a>";
    if (speedtest_loop_mode()) return "<a href='act:ststart' class='stbtn loop'>开始循环测速</a>";
    return "<a href='act:ststart' class='stbtn'>开始测速</a>";
}

static int speedtest_pct(double v, double scale)
{
    int pct;
    if (scale < 1.0) scale = 1.0;
    if (v < 0.0) v = 0.0;
    pct = (int)(v * 100.0 / scale + 0.5);
    if (v > 0.0 && pct < 4) pct = 4;
    if (pct > 100) pct = 100;
    return pct;
}

static void speedtest_chart_row(char *buf, size_t cap, int *o,
                                const char *lab, double val, double scale,
                                const char *unit, const char *cls)
{
    int pct = speedtest_pct(val, scale);
    if (*o >= (int)cap) return;
    *o += snprintf(buf + *o, cap - (size_t)*o,
                   "<div class='stbar-row'><span class='stbar-lab'>%s</span>"
                   "<span class='stbar-box'><span class='stbar-fill %s' style='width:%d%%'></span></span>"
                   "<span class='stbar-val'>%.1f%s</span></div>",
                   lab, cls ? cls : "", pct, val, unit);
}

static const char *speedtest_gauge_html(void)
{
    static char buf[640];
    double cur = 0.0, peak = 0.0, scale = 100.0;
    const char *label = "待机";
    char sub[128];
    int pct;
    if (g_st_state == ST_STATE_RUNNING) {
        cur = g_st_cur_mbps;
        peak = g_st_cur_peak;
        label = (!strcmp(g_st_phase, "upload")) ? "当前上行" :
                (!strcmp(g_st_phase, "download")) ? "当前下行" : "测速中";
        snprintf(sub, sizeof sub, "%s / 峰值 %.1f Mbps", speedtest_phase_label(g_st_phase), peak);
    } else if (g_st_have_result) {
        cur = g_st_dl_avg >= g_st_ul_avg ? g_st_dl_avg : g_st_ul_avg;
        peak = g_st_dl_peak >= g_st_ul_peak ? g_st_dl_peak : g_st_ul_peak;
        label = "本次速度";
        snprintf(sub, sizeof sub, "下行 %.1f / 上行 %.1f Mbps", g_st_dl_avg, g_st_ul_avg);
    } else if (g_st_state == ST_STATE_MISSING) {
        label = "未安装";
        snprintf(sub, sizeof sub, "安装测速器后可用");
    } else if (g_st_state == ST_STATE_FAIL) {
        label = "失败";
        snprintf(sub, sizeof sub, "%s", g_st_err[0] ? g_st_err : "测速失败");
    } else {
        label = "准备就绪";
        snprintf(sub, sizeof sub, "点击开始测速");
    }
    if (peak > scale) scale = peak;
    if (cur > scale) scale = cur;
    pct = speedtest_pct(cur, scale);
    snprintf(buf, sizeof buf,
             "<div class='st-card st-gauge'>"
             "<div class='st-gauge-title'>网速仪表盘</div>"
             "<div class='st-gauge-face'><span class='st-gauge-num'>%.1f</span><span class='st-gauge-unit'>Mbps</span></div>"
             "<div class='st-scale'><span class='st-fill' style='width:%d%%'></span></div>"
             "<div class='st-gauge-sub'>%s · %s</div>"
             "</div>",
             cur, pct, label, sub);
    return buf;
}

static const char *speedtest_chart_html(void)
{
    static char buf[1200];
    int o = 0;
    double speed_scale = 100.0;
    double max_speed = g_st_dl_peak;
    if (g_st_ul_peak > max_speed) max_speed = g_st_ul_peak;
    if (g_st_cur_peak > max_speed) max_speed = g_st_cur_peak;
    if (max_speed > speed_scale) speed_scale = max_speed;
    o += snprintf(buf + o, sizeof buf - (size_t)o, "<div class='st-card st-chart'><div class='st-chart-title'>测速图表</div>");
    if (g_st_have_result) {
        speedtest_chart_row(buf, sizeof buf, &o, "下行", g_st_dl_avg, speed_scale, "M", "");
        speedtest_chart_row(buf, sizeof buf, &o, "峰值", g_st_dl_peak, speed_scale, "M", "");
        speedtest_chart_row(buf, sizeof buf, &o, "上行", g_st_ul_avg, speed_scale, "M", "up");
        speedtest_chart_row(buf, sizeof buf, &o, "峰值", g_st_ul_peak, speed_scale, "M", "up");
        speedtest_chart_row(buf, sizeof buf, &o, "延迟", g_st_ping, 100.0, "ms", "ping");
    } else if (g_st_state == ST_STATE_RUNNING) {
        speedtest_chart_row(buf, sizeof buf, &o, "当前", g_st_cur_mbps, speed_scale, "M", !strcmp(g_st_phase, "upload") ? "up" : "");
        speedtest_chart_row(buf, sizeof buf, &o, "峰值", g_st_cur_peak, speed_scale, "M", !strcmp(g_st_phase, "upload") ? "up" : "");
    } else {
        speedtest_chart_row(buf, sizeof buf, &o, "下行", 0.0, speed_scale, "M", "");
        speedtest_chart_row(buf, sizeof buf, &o, "上行", 0.0, speed_scale, "M", "up");
        speedtest_chart_row(buf, sizeof buf, &o, "延迟", 0.0, 100.0, "ms", "ping");
    }
    snprintf(buf + o, sizeof buf - (size_t)o, "</div>");
    return buf;
}

static double speedtest_display_mbps(void)
{
    if (g_st_state == ST_STATE_RUNNING) return g_st_cur_mbps;
    if (g_st_have_result) return g_st_dl_avg >= g_st_ul_avg ? g_st_dl_avg : g_st_ul_avg;
    return 0.0;
}

static double speedtest_display_peak_mbps(void)
{
    double peak = g_st_cur_peak;
    if (g_st_dl_peak > peak) peak = g_st_dl_peak;
    if (g_st_ul_peak > peak) peak = g_st_ul_peak;
    if (peak < speedtest_display_mbps()) peak = speedtest_display_mbps();
    if (peak < 100.0) peak = 100.0;
    return peak;
}

static const char *speedtest_dial_html(void)
{
    static char buf[720], sub[160], esc[220];
    double cur = speedtest_display_mbps();
    if (g_st_state == ST_STATE_RUNNING) {
        snprintf(sub, sizeof sub, "%s / 峰值 %.1f Mbps", speedtest_phase_label(g_st_phase), g_st_cur_peak);
    } else if (g_st_have_result) {
        snprintf(sub, sizeof sub, "下行 %.1f / 上行 %.1f Mbps", g_st_dl_avg, g_st_ul_avg);
    } else if (g_st_state == ST_STATE_MISSING) {
        snprintf(sub, sizeof sub, "安装测速器后可用");
    } else if (g_st_state == ST_STATE_FAIL) {
        snprintf(sub, sizeof sub, "%s", g_st_err[0] ? g_st_err : "测速失败");
    } else if (speedtest_loop_mode()) {
        snprintf(sub, sizeof sub, "循环模式 / 手动停止");
    } else {
        snprintf(sub, sizeof sub, "准备就绪 / %d秒", g_st_dur);
    }
    html_esc(esc, sizeof esc, sub);
    snprintf(buf, sizeof buf,
             "<div class='st-card st-gauge'>"
             "<div id='st-gauge-dial' class='st-gauge-dial' style='width:184px;height:184px;margin:0 auto'></div>"
             "<div class='st-gauge-sub'>%.1f Mbps · %s</div>"
             "</div>",
             cur, esc);
    return buf;
}

static const char *speedtest_lines_html(void)
{
    static char buf[720];
    double dl = g_st_have_result ? g_st_dl_avg : (g_st_dl_n > 0 ? g_st_dl_hist[g_st_dl_n - 1] / 10.0 : 0.0);
    double ul = g_st_have_result ? g_st_ul_avg : (g_st_ul_n > 0 ? g_st_ul_hist[g_st_ul_n - 1] / 10.0 : 0.0);
    snprintf(buf, sizeof buf,
             "<div class='st-card st-lines'>"
             "<div class='st-line-head'>下行折线<b>%.1f Mbps</b></div>"
             "<div id='st-chart-dl' class='st-line-chart'></div>"
             "<div class='st-line-head'>上行折线<b>%.1f Mbps</b></div>"
             "<div id='st-chart-ul' class='st-line-chart'></div>"
             "</div>",
             dl, ul);
    return buf;
}

static const char *speedtest_home_inline_html(void)
{
    static char buf[8192];
    if (g_lock_state || !g_st_home_open || !speedtest_binary_ready()) return "";
    snprintf(buf, sizeof buf,
             "<div class='st-home-panel'>"
             "%s"
             "%s"
             "<div class='st-card st-options'>"
             "<div class='st-caption'>测速源</div>%s"
             "<div class='st-caption'>方向</div>%s"
             "<div class='st-caption'>时长</div>%s"
             "%s"
             "</div>"
             "<div class='st-actionbar'>%s</div>"
             "</div>",
             speedtest_dial_html(),
             speedtest_lines_html(),
             speedtest_src_html(),
             speedtest_dir_html(),
             speedtest_dur_html(),
             speedtest_loop_hint_html(),
             speedtest_action_html());
    return buf;
}
static const char *speedtest_status_html(void)
{
    static char buf[320], raw[180], esc[256];
    const char *cls = "idle";
    if (g_st_state == ST_STATE_RUNNING) {
        cls = "run";
        if (speedtest_loop_mode() && g_st_have_result)
            snprintf(raw, sizeof raw, "循环测速中 / 手动停止");
        else if (!strcmp(g_st_phase, "download") || !strcmp(g_st_phase, "upload"))
            snprintf(raw, sizeof raw, "%s %.1f Mbps / 峰值 %.1f",
                     speedtest_phase_label(g_st_phase), g_st_cur_mbps, g_st_cur_peak);
        else if (speedtest_loop_mode())
            snprintf(raw, sizeof raw, "循环测速中 / 手动停止");
        else if (g_st_phase[0]) snprintf(raw, sizeof raw, "%s", speedtest_phase_label(g_st_phase));
        else if (g_st_msg[0]) snprintf(raw, sizeof raw, "测速中");
        else snprintf(raw, sizeof raw, "测速中");
    } else if (g_st_state == ST_STATE_DONE) {
        cls = "done";
        snprintf(raw, sizeof raw, "完成 / 下行 %.1f Mbps", g_st_dl_avg);
    } else if (g_st_state == ST_STATE_FAIL) {
        cls = "fail";
        snprintf(raw, sizeof raw, "%s", g_st_err[0] ? g_st_err : "测速失败");
    } else if (g_st_state == ST_STATE_MISSING) {
        cls = "fail";
        snprintf(raw, sizeof raw, "未安装测速器");
    } else {
        snprintf(raw, sizeof raw, "准备就绪 / %d秒", g_st_dur);
    }
    html_esc(esc, sizeof esc, raw);
    snprintf(buf, sizeof buf, "<div class='ststat %s'>%s</div>", cls, esc);
    return buf;
}

static const char *speedtest_result_html(void)
{
    static char buf[1400], node[192], source[96], loc[128], ul[192], err[192];
    char loc_raw[96];
    if (g_st_have_result) {
        html_esc(node, sizeof node, g_st_node[0] ? g_st_node : "-");
        html_esc(source, sizeof source, g_st_source[0] ? g_st_source : "-");
        if (g_st_carrier[0] || g_st_city[0])
            snprintf(loc_raw, sizeof loc_raw, "%s%s%s", g_st_carrier, (g_st_carrier[0] && g_st_city[0]) ? " / " : "", g_st_city);
        else
            snprintf(loc_raw, sizeof loc_raw, "-");
        html_esc(loc, sizeof loc, loc_raw);
        snprintf(ul, sizeof ul, "%.1f / %.1f Mbps", g_st_ul_avg, g_st_ul_peak);
        snprintf(buf, sizeof buf,
                 "<div class='stres'>"
                 "<div class='strow'><span>节点</span><b>%s</b></div>"
                 "<div class='strow'><span>来源</span><b>%s</b></div>"
                 "<div class='strow'><span>位置</span><b>%s</b></div>"
                 "<div class='strow'><span>下行</span><b>%.1f / %.1f Mbps</b></div>"
                 "<div class='strow'><span>上行</span><b>%s</b></div>"
                 "<div class='strow'><span>延迟</span><b>%.1f ms / 抖动 %.1f ms</b></div>"
                 "</div>",
                 node, source, loc, g_st_dl_avg, g_st_dl_peak, ul, g_st_ping, g_st_jitter);
        return buf;
    }
    if (g_st_state == ST_STATE_FAIL) {
        html_esc(err, sizeof err, g_st_err[0] ? g_st_err : "测速失败");
        snprintf(buf, sizeof buf, "<div class='stempty bad'>%s</div>", err);
        return buf;
    }
    if (g_st_state == ST_STATE_RUNNING && speedtest_loop_mode()) return "<div class='stempty'>循环测速中，手动停止前不会自动结束。</div>";
    if (g_st_state == ST_STATE_RUNNING) return "<div class='stempty'>测速进行中，结果稍后显示。</div>";
    if (g_st_state == ST_STATE_MISSING) return "<div class='stempty'>安装测速器后显示结果。</div>";
    return "<div class='stempty'>暂无测速结果。</div>";
}

static const char *speedtest_log_card_html(void)
{
    static char buf[2200];
    snprintf(buf, sizeof buf,
             "<div class='st-card st-log'>"
             "<div class='st-caption'>测速日志</div>"
             "%s%s"
             "</div>",
             speedtest_status_html(),
             speedtest_result_html());
    return buf;
}

static int hsr_freq_is_whitelist(long arfcn)
{
    static const long hsr_arfcn[] = {
        507150, 527070, 531390, 153370,
        505230, 627744, 634464, 423630
    };

    for (size_t i = 0; i < sizeof(hsr_arfcn) / sizeof(hsr_arfcn[0]); i++)
        if (arfcn == hsr_arfcn[i])
            return 1;
    return 0;
}

static int hsr_freq_hint(int mcc, const char *arfcn)
{
    char *end = NULL;
    long v;

    if (mcc != 460 || !arfcn || !arfcn[0] || !strcmp(arfcn, "-"))
        return 0;
    v = strtol(arfcn, &end, 10);
    if (end == arfcn || v <= 0)
        return 0;
    return hsr_freq_is_whitelist(v);
}

#define SIGNAL_MAX_CARRIERS 16
#define SIGNAL_MAX_NEIGHBORS 16

struct signal_carrier_metric {
    int valid;
    int used;
    char name[32];
    char type[16];
    char pci[16];
    char arfcn[32];
    char rb[16];
    char grants[16];
    char dl_rb[16];
    char dl_grants[16];
    char ul_rb[16];
    char ul_grants[16];
    char mcs[16];
    char modulation[32];
    char mimo[48];
    char layers[16];
    char bler[16];
    char ssb[16];
    char serving_ssb[16];
    char ml1_rsrp[16];
    char ml1_rsrq[16];
    char ml1_sinr[16];
};

struct signal_neighbor_metric {
    int valid;
    char pci[16];
    char band[24];
    char arfcn[32];
    char rsrp[16];
    char rsrq[16];
};

static void signal_carrier_metric_reset(struct signal_carrier_metric *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "-");
    snprintf(c->type, sizeof c->type, "-");
    snprintf(c->pci, sizeof c->pci, "-");
    snprintf(c->arfcn, sizeof c->arfcn, "-");
    snprintf(c->rb, sizeof c->rb, "-");
    snprintf(c->grants, sizeof c->grants, "-");
    snprintf(c->dl_rb, sizeof c->dl_rb, "-");
    snprintf(c->dl_grants, sizeof c->dl_grants, "-");
    snprintf(c->ul_rb, sizeof c->ul_rb, "-");
    snprintf(c->ul_grants, sizeof c->ul_grants, "-");
    snprintf(c->mcs, sizeof c->mcs, "-");
    snprintf(c->modulation, sizeof c->modulation, "-");
    snprintf(c->mimo, sizeof c->mimo, "-");
    snprintf(c->layers, sizeof c->layers, "-");
    snprintf(c->bler, sizeof c->bler, "-");
    snprintf(c->ssb, sizeof c->ssb, "-");
    snprintf(c->serving_ssb, sizeof c->serving_ssb, "-");
    snprintf(c->ml1_rsrp, sizeof c->ml1_rsrp, "-");
    snprintf(c->ml1_rsrq, sizeof c->ml1_rsrq, "-");
    snprintf(c->ml1_sinr, sizeof c->ml1_sinr, "-");
}

static void signal_neighbor_metric_reset(struct signal_neighbor_metric *n)
{
    if (!n) return;
    memset(n, 0, sizeof *n);
    snprintf(n->pci, sizeof n->pci, "-");
    snprintf(n->band, sizeof n->band, "-");
    snprintf(n->arfcn, sizeof n->arfcn, "-");
    snprintf(n->rsrp, sizeof n->rsrp, "-");
    snprintf(n->rsrq, sizeof n->rsrq, "-");
}

static const char *signal_carrier_val(const char *v)
{
    return signal_value_present(v) ? v : "-";
}

static void carrier_metric_html(char *dst, size_t cap,
                                const struct signal_carrier_metric *m,
                                int is_nr)
{
    char a[64], b[64], c[64], d[64], e[64], f[64];
    size_t off;

    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!m || !m->valid) return;

    if (is_nr) {
        const char *ssb = signal_value_present(m->serving_ssb) ? m->serving_ssb : m->ssb;
        html_esc(a, sizeof a, signal_carrier_val(m->mimo));
        html_esc(b, sizeof b, signal_carrier_val(m->layers));
        html_esc(c, sizeof c, signal_carrier_val(m->dl_grants));
        html_esc(d, sizeof d, signal_carrier_val(m->ul_grants));
        html_esc(e, sizeof e, signal_carrier_val(m->dl_rb));
        html_esc(f, sizeof f, signal_carrier_val(m->ul_rb));
        snprintf(dst, cap,
                 "<div class='cx'><span>MIMO %s</span><span>L %s</span>"
                 "<span>G %s/%s</span><span>RB %s/%s</span>",
                 a, b, c, d, e, f);
        html_esc(a, sizeof a, signal_carrier_val(m->bler));
        html_esc(b, sizeof b, signal_carrier_val(ssb));
        snprintf(dst + strlen(dst), cap - strlen(dst),
                 "<span>BLER %s</span><span>SSB %s</span></div>", a, b);
    } else {
        const char *mod = signal_value_present(m->modulation) ? m->modulation : m->mcs;
        html_esc(a, sizeof a, signal_carrier_val(mod));
        html_esc(b, sizeof b, signal_carrier_val(m->layers));
        html_esc(c, sizeof c, signal_carrier_val(m->grants));
        html_esc(d, sizeof d, signal_carrier_val(m->rb));
        html_esc(e, sizeof e, signal_carrier_val(m->bler));
        snprintf(dst, cap,
                 "<div class='cx'><span>MCS %s</span><span>L %s</span>"
                 "<span>G %s</span><span>RB %s</span><span>BLER %s</span></div>",
                 a, b, c, d, e);
    }
    if (signal_value_present(m->ml1_rsrp) ||
        signal_value_present(m->ml1_rsrq) ||
        signal_value_present(m->ml1_sinr)) {
        html_esc(a, sizeof a, signal_carrier_val(m->ml1_rsrp));
        html_esc(b, sizeof b, signal_carrier_val(m->ml1_rsrq));
        html_esc(c, sizeof c, signal_carrier_val(m->ml1_sinr));
        off = strlen(dst);
        snprintf(dst + off, cap - off,
                 "<div class='cx sigsrc'><span>ML1 RSRP %s</span><span>RSRQ %s</span>",
                 a, b);
        if (signal_value_present(m->ml1_sinr)) {
            off = strlen(dst);
            snprintf(dst + off, cap - off, "<span>SINR %s</span>", c);
        }
        off = strlen(dst);
        snprintf(dst + off, cap - off, "</div>");
    }
}

/* Append one carrier card (band/bw + PCI, then RSRP/SINR colored by quality).
 * A carrier reporting the floor sentinel (RSRP <= -140) is "configured but not
 * active"; its values are grayed out and tagged inactive. */
static int car_row(char *buf, int o, int cap, const char *band, const char *bw,
                    const char *arfcn, const char *pci, const char *rsrp, const char *sinr,
                    int hsr_hint, const struct signal_carrier_metric *metric)
{
    const char *rsrp_show = signal_value_present(rsrp) ? rsrp : "-";
    const char *sinr_show = signal_value_present(sinr) ? sinr : "-";
    int has_rsrp;
    int has_sinr;
    double rp;
    double sn;
    int inactive;
    const char *rq;
    const char *sq;
    const char *tag;
    const char *hsr = hsr_hint ? "<span class='chsr'>高铁专网</span>" : "";
    int is_nr_metric = band && band[0] == 'n';
    const char *al = is_nr_metric ? "ARFCN" : "EARFCN";
    char bw_text[20];
    char extra[768];

    has_rsrp = signal_value_present(rsrp_show);
    has_sinr = signal_value_present(sinr_show);
    rp = has_rsrp ? atof(rsrp_show) : -150.0;
    sn = has_sinr ? atof(sinr_show) : -99.0;
    inactive = has_rsrp && rp <= -140.0;
    rq = !has_rsrp || inactive ? "q-off" : rp >= -85 ? "q-good" : rp >= -105 ? "q-mid" : "q-bad";
    sq = !has_sinr || inactive ? "q-off" : sn >= 13  ? "q-good" : sn >= 0    ? "q-mid" : "q-bad";
    tag = inactive ? "<span class='coff'>未激活</span>" : "";
    carrier_metric_html(extra, sizeof extra, metric, is_nr_metric);
    if (signal_value_present(bw))
        snprintf(bw_text, sizeof bw_text, "%sM", bw);
    else
        snprintf(bw_text, sizeof bw_text, "-");
    return o + snprintf(buf + o, cap - o,
        "<div class='ccd%s%s'><span class='cb'>%s</span><span class='cbw'> %s</span>%s%s"
        "<span class='cinfo'><span class='carfcn'>%s %s</span><span class='cpci'>PCI %s</span></span>"
        "<div class='cm'><span class='ml'>RSRP</span><span class='%s'>%s</span>"
        "<span class='ml ml2'>SINR</span><span class='%s'>%s</span></div>%s</div>",
        inactive ? " off" : "", hsr_hint ? " hsrhint" : "",
        band, bw_text, tag, hsr,
        al, (arfcn && arfcn[0]) ? arfcn : "-", (pci && pci[0]) ? pci : "-",
        rq, rsrp_show, sq, sinr_show,
        extra);
}

static int carrier_is_inactive(const char *rsrp)
{
    return atof((rsrp && rsrp[0]) ? rsrp : "") <= -140.0;
}

/* Split a CA group "f0,f1,..." into fields[] (returns count). */
static int ca_split(char *g, char **f, int maxf)
{
    int n = 0; char *save;
    for (char *tk = strtok_r(g, ",", &save); tk && n < maxf; tk = strtok_r(NULL, ",", &save)) f[n++] = tk;
    return n;
}

static int ca_groups(char *s, char **g, int maxg)
{
    int n = 0; char *save;
    for (char *tk = strtok_r(s, ";", &save); tk && n < maxg; tk = strtok_r(NULL, ";", &save)) g[n++] = tk;
    return n;
}

/* LTE SCC signal is sometimes exposed separately from legacy 5-field lteca.
 * We accept both full 11-field groups and shorter signal-only groups by
 * locating the first "RSRP-like" value (typically <= -40 dBm), then taking
 * SINR from two fields later when present. */
static int ca_sig_pick(char *g, const char **rsrp, const char **sinr)
{
    char *f[16];
    int nf = ca_split(g, f, 16), rp = -1;
    *rsrp = *sinr = NULL;
    for (int i = 0; i < nf; i++) {
        if (atof(f[i]) <= -40.0) { rp = i; break; }
    }
    if (rp < 0) return 0;
    *rsrp = f[rp];
    if (rp + 2 < nf)      *sinr = f[rp + 2];
    else if (rp + 1 < nf) *sinr = f[rp + 1];
    return 1;
}

static int signal_level(int rsrp)
{
    int lvl;
    if      (rsrp == 0)    lvl = 0;   /* unknown / no signal */
    else if (rsrp >= -80)  lvl = 5;
    else if (rsrp >= -90)  lvl = 4;
    else if (rsrp >= -100) lvl = 3;
    else if (rsrp >= -110) lvl = 2;
    else                   lvl = 1;
    return lvl;
}

/* ---- rolling history for the charts page (sampled once per second) ---- */
#define HIST 48
static int    h_n;
static int    h_cpu[HIST], h_mem[HIST], h_ct[HIST], h_bt[HIST], h_pwr[HIST];
static long   h_rx[HIST], h_tx[HIST];
static time_t h_last;

/* charge (charger input) or discharge (battery) power, in milliwatts. */
/* Battery power in milliwatts (use battery_voltage * |battery_current|). */
static int power_mw(const devui_data_t *d)
{
    double w = (d->bat_uv / 1e6) * (labs(d->bat_ua) / 1e6);
    return (int)(w * 1000);
}

static void hist_push(const devui_data_t *d)
{
    if (h_n < HIST) h_n++;
    else {
        memmove(h_cpu, h_cpu + 1, (HIST - 1) * sizeof h_cpu[0]);
        memmove(h_mem, h_mem + 1, (HIST - 1) * sizeof h_mem[0]);
        memmove(h_ct,  h_ct + 1,  (HIST - 1) * sizeof h_ct[0]);
        memmove(h_bt,  h_bt + 1,  (HIST - 1) * sizeof h_bt[0]);
        memmove(h_pwr, h_pwr + 1, (HIST - 1) * sizeof h_pwr[0]);
        memmove(h_rx,  h_rx + 1,  (HIST - 1) * sizeof h_rx[0]);
        memmove(h_tx,  h_tx + 1,  (HIST - 1) * sizeof h_tx[0]);
    }
    int i = h_n - 1;
    h_cpu[i] = d->cpu_usage < 0 ? 0 : (int)d->cpu_usage;
    h_mem[i] = d->mem_used_pct < 0 ? 0 : (int)d->mem_used_pct;
    h_ct[i]  = (int)d->cpu_temp;
    h_bt[i]  = d->bat_temp;
    h_pwr[i] = power_mw(d);
    h_rx[i]  = d->rx_speed;
    h_tx[i]  = d->tx_speed;
}

/* Draw the chart placeholders (#chart-cpu/#chart-mem/#chart-net) natively as
 * Bresenham polylines over the history. Called after the page is rendered; a
 * no-op on pages without those elements. */
static void draw_charts(void)
{
    int x, y, w, h;
    if (html_view_rect("#chart-cpu", &x, &y, &w, &h)) {
        html_view_polyline(x, y, w, h, h_cpu, h_n, 0, 100, RGB3(theme_a()), 2, 26); /* CPU usage */
        html_view_polyline(x, y, w, h, h_ct,  h_n, 20, 70, RGB3(theme_b()), 2, 0);  /* temperature */
    }
    if (html_view_rect("#chart-mem", &x, &y, &w, &h))
        html_view_polyline(x, y, w, h, h_mem, h_n, 0, 100, RGB3(C_OK), 2, 34); /* memory */
    if (html_view_rect("#chart-net", &x, &y, &w, &h)) {
        static int rxi[HIST], txi[HIST];
        long mx = 1;
        for (int i = 0; i < h_n; i++) { if (h_rx[i] > mx) mx = h_rx[i]; if (h_tx[i] > mx) mx = h_tx[i]; }
        for (int i = 0; i < h_n; i++) { rxi[i] = (int)h_rx[i]; txi[i] = (int)h_tx[i]; }
        html_view_polyline(x, y, w, h, rxi, h_n, 0, (int)mx, RGB3(theme_a()), 2, 22); /* downlink */
        html_view_polyline(x, y, w, h, txi, h_n, 0, (int)mx, RGB3(theme_b()), 2, 0);  /* uplink */
    }
    if (html_view_rect("#chart-batt", &x, &y, &w, &h)) {
        static int tn[HIST], pn[HIST];
        int mx = 1;
        for (int i = 0; i < h_n; i++) if (h_pwr[i] > mx) mx = h_pwr[i];
        for (int i = 0; i < h_n; i++) { tn[i] = (h_bt[i] - 20) * 2; pn[i] = (int)((long)h_pwr[i] * 100 / mx); }
        html_view_polyline(x, y, w, h, pn, h_n, 0, 100, RGB3(theme_a()), 2, 22); /* power */
        html_view_polyline(x, y, w, h, tn, h_n, 0, 100, RGB3(theme_b()), 2, 0);  /* temperature */
    }
}

/* Locked preview badge at the bottom: a pill with a padlock and the hint;
 * while the finger holds, the pill fills with the accent. */
static void draw_lock_icon(void)
{
    const int w = 132, h = 32, x = (320 - w) / 2, y = 430;
    const char *hint = lock_enabled() ? "滑动输入密码" : "长按解锁";
    const float cx = (float)x + 24.f, cy = (float)y + h / 2.f + 1.f;
    int tw = html_view_text_width_px(hint, 14);

    html_view_glass_rect(x, y, w, h, h / 2);
    unsigned fg = html_view_glass_ink(C_TXT);   /* light glass: dark glyph */
    if (g_lock_hold > 0)
        html_view_fill_round_rect(x, y, w * g_lock_hold / 100 < h ? h : w * g_lock_hold / 100, h, h / 2,
                                  RGB3(theme_a()), 200);
    /* padlock: shackle arc over a rounded body */
    html_view_arc(cx, cy - 3.f, 4.5f, 2.f, -90.f, 90.f, RGB3(fg), 255);
    html_view_line(cx - 4.5f, cy - 3.f, cx - 4.5f, cy, 2.f, RGB3(fg), 255);
    html_view_line(cx + 4.5f, cy - 3.f, cx + 4.5f, cy, 2.f, RGB3(fg), 255);
    html_view_fill_round_rect((int)cx - 7, (int)cy, 14, 10, 2, RGB3(fg), 255);
    html_view_draw_text_px((int)cx + 14 + (w - 38 - tw) / 2, y + 7, hint, 14, 0, RGB3(fg), 255);
}

/* SMS detail dialog + status-bar envelope state. */
static long g_sms_open = -1;   /* opened SMS message id for detail dialog (-1 = none) */
static int  g_sms_unread_now;  /* unread SMS present -> draw the status-bar envelope */
static char g_sms_num[40], g_sms_date[16], g_sms_text[DEVUI_SMS_TEXT_MAX];   /* opened message */
static int  g_sms_scroll, g_sms_scroll_max;
static int  g_sms_view_x, g_sms_view_y, g_sms_view_w, g_sms_view_h;

/* Closing an SMS removes its overlay before the next input pass.  Discard every
 * latched gesture, rather than one queue entry, so an older release cannot be
 * replayed against the SMS card exposed underneath. */
static void sms_close(touch_input_t *touch, int *need_render)
{
    g_sms_open = -1;
    g_sms_scroll = g_sms_scroll_max = 0;
    touch_input_clear_taps(touch);
    *need_render = 1;
}

#define SBAR_TIME_X 10
static unsigned g_sbar_fg = C_TXT;   /* status-bar glyph colour, set with its background */

/* Draw a small envelope in the fixed status bar, just right of the clock, when
 * there are unread messages: a single-colour line drawing in the clock's
 * colour, centred on the bar like the digits. The font has no envelope glyph. */
static void draw_sms_icon(void)
{
    if (!g_sms_unread_now) return;
    const float ew = 16.f, eh = 12.f, th = 1.5f, ey = 13.f - eh / 2.f;
    float ex = (float)(SBAR_TIME_X + html_view_text_width_px(g_stat_time, 16) + 7);
    html_view_round_rect_aa(ex, ey, ew, eh, 2.f, th, RGB3(g_sbar_fg), 255);
    /* flap: from just inside the top corners down to the middle */
    float fy = ey + th * .5f + .5f, my = ey + eh * .55f;
    html_view_line(ex + 2.f, fy, ex + ew / 2.f, my, th, RGB3(g_sbar_fg), 255);
    html_view_line(ex + ew - 2.f, fy, ex + ew / 2.f, my, th, RGB3(g_sbar_fg), 255);
}

/* ---- band lock (éé¢): comma-list band sets ---- */
/* Status bar text vertically centered on the bar by its digits. */
static int sbar_text_y(int size)
{
    int x0, y0, x1, y1;
    html_view_text_bounds_px("0", size, &x0, &y0, &x1, &y1);
    return 13 - (y0 + y1) / 2;
}

/* Right to left: battery, its percentage, the signal bars and the network
 * generation. Time (and the SMS envelope) on the left. The battery is an outline with a fill in the state color and the
 * percentage stays outside it, so it reads on any charge level. */
static void draw_native_statusbar(void)
{
    const unsigned bad = C_BAD, ok = C_OK;

    if (html_view_wallpaper_rect(0, 0, 320, 26)) {                     /* over the wallpaper a glass capsule, or */
        html_view_glass_rect(2, 2, 316, 22, 11);
        g_sbar_fg = html_view_glass_ink(C_TXT);                         /* dark on a light pane */
    } else {
        html_view_fill_rect(0, 0, 320, 26, RGB3(C_BAR), 255);
        html_view_fill_rect(0, 25, 320, 1, RGB3(C_TXT), 14);          /* bar + hairline */
        g_sbar_fg = html_view_adapt_color(0, 0, 320, 26, C_TXT);
    }
    html_view_draw_text_px(SBAR_TIME_X, sbar_text_y(16), g_stat_time, 16, 0, RGB3(g_sbar_fg), 255);

    /* battery: 23x12 outline + tip */
    const float bw = 23.f, bh = 12.f, bx = 320.f - 9.f - bw, by = 7.f;
    int pct = clampi(g_stat_bat, 0, 100);
    unsigned fill = g_charging ? ok : g_stat_lowbat ? bad : g_sbar_fg;
    html_view_round_rect_aa(bx, by, bw, bh, 3.5f, 1.4f, RGB3(g_sbar_fg), 170);
    html_view_round_rect_aa(bx + bw + 1.f, by + 3.5f, 2.f, 5.f, 1.f, 0.f, RGB3(g_sbar_fg), 170);
    if (pct > 0)
        html_view_round_rect_aa(bx + 2.5f, by + 2.5f, (bw - 5.f) * (float)pct / 100.f, bh - 5.f, 1.5f, 0.f,
                                RGB3(fill), 255);
    int right = (int)bx - 5;

    if (g_show_batpct) {
        char bp[8];
        snprintf(bp, sizeof bp, "%d%%", pct);
        int w = html_view_text_width_px(bp, 15);
        html_view_draw_text_px(right - w, sbar_text_y(15), bp, 15, 0,
                               RGB3(g_stat_lowbat && !g_charging ? bad : g_sbar_fg), 255);
        right -= w + 3;
    }
    if (g_charging) {                                                   /* bolt */
        int x = right - 7, y = 7;
        int xs[6] = { x + 5, x, x + 3, x + 2, x + 7, x + 4 };
        int ys[6] = { y, y + 7, y + 7, y + 12, y + 5, y + 5 };
        html_view_fill_poly(xs, ys, 6, RGB3(ok), 255);
        right -= 10;
    }

    /* signal: four bars, lit up to the level (signal_level gives 0..5) */
    right -= 8;
    int lit = g_stat_sig > 0 ? (g_stat_sig * 4 + 2) / 5 : 0;
    for (int i = 0; i < 4; i++) {
        float h = 4.f + 2.7f * (float)i, x = (float)right - 18.f + 5.f * (float)i;
        html_view_round_rect_aa(x, 19.f - h, 3.f, h, 1.f, 0.f, RGB3(g_sbar_fg), i < lit ? 255 : 55);
    }
    right -= 18 + 4;
    int gw = html_view_text_width_px(g_stat_gen, 14);
    html_view_draw_text_px(right - gw, sbar_text_y(14), g_stat_gen, 14, 0, RGB3(g_sbar_fg), 255);

    if (g_show_sbspeed) {   /* down and up on one line, between the clock (and envelope) and the network */
        static const int sizes[] = { 13, 12, 11 };
        char dn[24], up[24], t[56];
        int x = SBAR_TIME_X + html_view_text_width_px(g_stat_time, 16) + 9 + (g_sms_unread_now ? 23 : 0);
        int room = right - gw - 8 - x, size = 0;
        fmt_speed_u(dn, sizeof dn, g_stat_rx, g_speed_bits);
        fmt_speed_u(up, sizeof up, g_stat_tx, g_speed_bits);
        /* the largest size that fits: with units, else units cut to the prefix
         * ("28 Mbps" -> "28M", "3.5 MB/s" -> "3.5MB") */
        for (int compact = 0; compact < 2 && !size; compact++) {
            if (compact) {
                char *u;
                for (char *v = dn; v; v = v == dn ? up : NULL)
                    if ((u = strchr(v, ' '))) {
                        if (strchr(u, '/')) { memmove(u, u + 1, strlen(u)); *strchr(u, '/') = 0; }
                        else { u[0] = u[1] == 'M' || u[1] == 'K' ? u[1] : 0; u[1] = 0; }
                    }
            }
            snprintf(t, sizeof t, "↓ %s  ↑ %s", dn, up);
            for (int k = 0; k < (int)(sizeof sizes / sizeof sizes[0]) && !size; k++)
                if (html_view_text_width_px(t, sizes[k]) <= room) size = sizes[k];
        }
        if (!size) size = 11;
        html_view_draw_text_px(x, sbar_text_y(size), t, size, 0, RGB3(g_sbar_fg), 220);
    }
}

static void draw_center_text_px(int cx, int y, const char *text, int size, int bold,
                                int r, int g, int b, int a)
{
    int x0, y0, x1, y1;
    html_view_text_bounds_px(text, size, &x0, &y0, &x1, &y1);
    html_view_draw_text_px(cx - (x1 - x0) / 2 - x0, y - y0, text, size, bold, r, g, b, a);
}

/* ---- U30-style dials, drawn into placeholder boxes after the page renders ----
 * #gauge-{lte,nr}-{rsrp,sinr}: a 270 degree arc open at the bottom, the value
 * in the middle and its name in the opening (#gauge-rsrp / #gauge-sinr: the
 * serving cell, for custom templates); #home-ring: battery ring around the home
 * clock; #home-bars / #home-wifi / #home-bolt: small home-page icons.
 * build_kv() fills g_dial from the snapshot it renders. */
typedef struct { int rsrp_ok, sinr_ok; float rsrp, sinr; } dial_cell_t;
static struct {
    dial_cell_t lte, nr;       /* a leg that is not up has no readings: "--" */
    int nr_serving;            /* the NR leg is up: it is the serving cell */
    int bat, charging, sig, wifi_on, nfc_on, dps_on;
    int agg_on, fan_on, liq_on;  /* TopFlow control-center toggles */
} g_dial;

static float dial_clamp(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

static void dial_gauge(const char *sel, int ok, float lv, unsigned col, const char *val, const char *name)
{
    int x, y, w, h, x0, y0, x1, y1;
    float cx, cy, rad;

    if (!html_view_rect(sel, &x, &y, &w, &h)) return;
    rad = (float)(w < h ? w : h) / 2.f - 7.f;
    cx = (float)x + (float)w / 2.f;
    cy = (float)y + (float)h / 2.f + 4.f;
    {
        int big = rad >= 50.f, vs = big ? 30 : 24;
        float th = big ? 8.f : 7.f;
        html_view_arc(cx, cy, rad, th, -135.f, 135.f, RGB3(C_TXT), 26);
        if (ok && lv > .01f)
            html_view_arc(cx, cy, rad, th, -135.f, -135.f + 270.f * lv, RGB3(col), 255);
        html_view_text_bounds_px(ok ? val : "--", vs, &x0, &y0, &x1, &y1);
        draw_center_text_px((int)cx, (int)cy - (y1 - y0) / 2 - 4, ok ? val : "--", vs, 1,
                            RGB3(ok ? C_TXT : C_FAINT), 255);
        draw_center_text_px((int)cx, (int)(cy + rad * .62f), name, big ? 13 : 12, 0, RGB3(C_DIM), 255);
    }
}

/* RSRP and SINR gauges of one cell */
static void dial_cell(const char *rsrp_sel, const char *sinr_sel, const dial_cell_t *c, const char *rat)
{
    char v[16], name[16];
    float lv = dial_clamp((c->rsrp + 125.f) / 55.f);

    snprintf(v, sizeof v, "%.0f", c->rsrp);
    snprintf(name, sizeof name, "%s RSRP", rat);
    dial_gauge(rsrp_sel, c->rsrp_ok, lv, lv > .6f ? C_OK : lv > .35f ? C_WARN : C_BAD, v, name);
    snprintf(v, sizeof v, "%.1f", c->sinr);
    snprintf(name, sizeof name, "%s SINR", rat);
    dial_gauge(sinr_sel, c->sinr_ok, dial_clamp((c->sinr + 5.f) / 30.f),
               c->sinr > 13.f ? C_OK : c->sinr > 0.f ? C_WARN : C_BAD, v, name);
}

static void draw_dials(void)
{
    int x, y, w, h;

    dial_cell("#gauge-nr-rsrp", "#gauge-nr-sinr", &g_dial.nr, "NR");
    dial_cell("#gauge-lte-rsrp", "#gauge-lte-sinr", &g_dial.lte, "LTE");
    dial_cell("#gauge-rsrp", "#gauge-sinr", g_dial.nr_serving ? &g_dial.nr : &g_dial.lte,
              g_dial.nr_serving ? "NR" : "LTE");

    if (html_view_rect("#home-ring", &x, &y, &w, &h)) {
        float cx = (float)x + (float)w / 2.f, cy = (float)y + (float)h / 2.f;
        float rad = (float)(w < h ? w : h) / 2.f - 4.f;
        unsigned col = g_dial.charging || g_dial.bat > 30 ? C_OK : g_dial.bat > 15 ? C_WARN : C_BAD;
        html_view_arc(cx, cy, rad, 6.f, 0.f, 360.f, RGB3(C_TXT), 20);
        if (g_dial.bat > 0)
            html_view_arc(cx, cy, rad, 6.f, 0.f, 360.f * (float)g_dial.bat / 100.f, RGB3(col), 255);
    }
    if (html_view_rect("#home-bars", &x, &y, &w, &h)) {
        for (int i = 0; i < 4; i++) {
            int bh = 5 + i * 4, bx = x + i * 5;
            unsigned c = i < (g_dial.sig + 1) * 4 / 5 && g_dial.sig ? theme_a() : C_FAINT;
            html_view_fill_round_rect(bx, y + h - bh, 3, bh, 1, RGB3(c), 255);
        }
    }
    if (html_view_rect("#home-wifi", &x, &y, &w, &h)) {
        /* off: faint, or over a wallpaper the ink of its glass pill, as its text */
        unsigned c = g_dial.wifi_on ? theme_a() : html_view_wallpaper_on() ? html_view_glass_ink(C_DIM) : C_FAINT;
        float cx = (float)x + (float)w / 2.f, cy = (float)y + (float)h - 2.f;
        html_view_fill_circle(cx, cy, 1.8f, RGB3(c), 255);
        html_view_arc(cx, cy, 5.5f, 2.f, -45.f, 45.f, RGB3(c), 255);
        html_view_arc(cx, cy, 10.f, 2.f, -45.f, 45.f, RGB3(c), 255);
    }
    if (g_dial.charging && html_view_rect("#home-bolt", &x, &y, &w, &h)) {
        int xs[6] = { x + w * 6 / 10, x + w / 6, x + w / 2, x + w * 4 / 10, x + w * 5 / 6, x + w / 2 };
        int ys[6] = { y, y + h * 6 / 10, y + h * 6 / 10, y + h, y + h * 4 / 10, y + h * 4 / 10 };
        html_view_fill_poly(xs, ys, 6, RGB3(C_OK), 255);
    }
}

/* Function-list icons, drawn in the primary accent into the empty .func-ic
 * circles: #fic-wifi, #fic-sms, #fic-cell, #fic-lock, #fic-speed and a generic
 * app grid for each #fic-custom-N. */
static void draw_func_icons(void)
{
    unsigned c = theme_a();
    int x, y, w, h;
    float cx, cy;
    const float t = 2.2f;

#define FIC(sel) (html_view_rect(sel, &x, &y, &w, &h) && \
                  ((cx = (float)x + (float)w / 2.f), (cy = (float)y + (float)h / 2.f), 1))
    if (FIC("#fic-wifi")) {                     /* dot + three arcs */
        float by = cy + 7.f;
        html_view_fill_circle(cx, by, 2.f, RGB3(c), 255);
        html_view_arc(cx, by, 6.f, t, -45.f, 45.f, RGB3(c), 255);
        html_view_arc(cx, by, 11.f, t, -45.f, 45.f, RGB3(c), 255);
        html_view_arc(cx, by, 16.f, t, -45.f, 45.f, RGB3(c), 255);
    }
    if (FIC("#fic-sms")) {                      /* envelope */
        float l = cx - 10.f, r = cx + 10.f, tp = cy - 7.f, bt = cy + 7.f;
        html_view_line(l, tp, r, tp, t, RGB3(c), 255);
        html_view_line(l, bt, r, bt, t, RGB3(c), 255);
        html_view_line(l, tp, l, bt, t, RGB3(c), 255);
        html_view_line(r, tp, r, bt, t, RGB3(c), 255);
        html_view_line(l, tp, cx, cy + 1.f, t, RGB3(c), 255);
        html_view_line(r, tp, cx, cy + 1.f, t, RGB3(c), 255);
    }
    if (FIC("#fic-cell")) {                     /* antenna with radio waves */
        float ty = cy - 4.f;
        html_view_fill_circle(cx, ty, 2.4f, RGB3(c), 255);
        html_view_line(cx, ty, cx - 5.f, cy + 10.f, t, RGB3(c), 255);
        html_view_line(cx, ty, cx + 5.f, cy + 10.f, t, RGB3(c), 255);
        html_view_arc(cx, ty, 6.f, t, 50.f, 130.f, RGB3(c), 255);
        html_view_arc(cx, ty, 6.f, t, 230.f, 310.f, RGB3(c), 255);
        html_view_arc(cx, ty, 11.f, t, 50.f, 130.f, RGB3(c), 255);
        html_view_arc(cx, ty, 11.f, t, 230.f, 310.f, RGB3(c), 255);
    }
    if (FIC("#fic-lock")) {                     /* padlock, shackle top to body bottom centred */
        html_view_arc(cx, cy - 5.f, 5.5f, t, -90.f, 90.f, RGB3(c), 255);
        html_view_line(cx - 5.5f, cy - 5.f, cx - 5.5f, cy - 1.f, t, RGB3(c), 255);
        html_view_line(cx + 5.5f, cy - 5.f, cx + 5.5f, cy - 1.f, t, RGB3(c), 255);
        html_view_fill_round_rect((int)(cx - 9.f), (int)(cy - 1.f), 18, 12, 3, RGB3(c), 255);
    }
    if (FIC("#fic-speed")) {                    /* speedometer */
        html_view_arc(cx, cy + 3.f, 11.f, t, -120.f, 120.f, RGB3(c), 255);
        html_view_line(cx, cy + 3.f, cx + 6.f, cy - 4.f, t, RGB3(c), 255);
        html_view_fill_circle(cx, cy + 3.f, 2.4f, RGB3(c), 255);
    }
    /* control center toggles: dark glyph on the lit accent disc; off, the
     * glass disc's own ink (dark on a light pane) */
    unsigned cc_off = html_view_glass_ink(C_TXT);
    if (FIC("#cc-wifi")) {
        unsigned k = g_dial.wifi_on ? 0x0B1220u : cc_off;
        float by = cy + 9.f;                     /* outer arc top to dot bottom centred */
        html_view_fill_circle(cx, by, 2.2f, RGB3(k), 255);
        html_view_arc(cx, by, 7.f, 2.6f, -45.f, 45.f, RGB3(k), 255);
        html_view_arc(cx, by, 13.f, 2.6f, -45.f, 45.f, RGB3(k), 255);
        html_view_arc(cx, by, 19.f, 2.6f, -45.f, 45.f, RGB3(k), 255);
    }
    if (FIC("#cc-nfc")) {                        /* phone tapping waves */
        unsigned k = g_dial.nfc_on ? 0x0B1220u : cc_off;
        html_view_arc(cx - 8.f, cy, 6.f, 2.6f, 50.f, 130.f, RGB3(k), 255);
        html_view_arc(cx - 8.f, cy, 12.f, 2.6f, 50.f, 130.f, RGB3(k), 255);
        html_view_arc(cx - 8.f, cy, 18.f, 2.6f, 55.f, 125.f, RGB3(k), 255);
        html_view_fill_circle(cx - 8.f, cy, 2.2f, RGB3(k), 255);
    }
    if (FIC("#cc-dps")) {                        /* lightning bolt */
        unsigned k = g_dial.dps_on ? 0x0B1220u : cc_off;
        int xs[6] = { (int)cx + 3, (int)cx - 8, (int)cx - 1, (int)cx - 4, (int)cx + 8, (int)cx + 1 };
        int ys[6] = { (int)cy - 13, (int)cy + 2, (int)cy + 2, (int)cy + 13, (int)cy - 3, (int)cy - 3 };
        html_view_fill_poly(xs, ys, 6, RGB3(k), 255);
    }
    if (FIC("#cc-agg")) {                        /* three links merging into one */
        unsigned k = g_dial.agg_on ? 0x0B1220u : cc_off;
        float lx = cx - 12.f, mx = cx + 2.f, rx = cx + 13.f;
        html_view_line(lx, cy - 9.f, mx, cy, 2.6f, RGB3(k), 255);
        html_view_line(lx, cy, mx, cy, 2.6f, RGB3(k), 255);
        html_view_line(lx, cy + 9.f, mx, cy, 2.6f, RGB3(k), 255);
        html_view_line(mx, cy, rx, cy, 3.4f, RGB3(k), 255);
        html_view_fill_circle(mx, cy, 3.2f, RGB3(k), 255);
    }
    if (FIC("#cc-fan")) {                        /* three blades around a hub */
        unsigned k = g_dial.fan_on ? 0x0B1220u : cc_off;
        cx -= 1.75f;                             /* blades reach cx+12 but cx-8.5 */
        for (int b = 0; b < 3; b++) {
            float a = (float)(b * 120) * 3.14159265f / 180.f;
            html_view_fill_circle(cx + 7.f * cosf(a), cy + 7.f * sinf(a), 5.f, RGB3(k), 255);
        }
        html_view_fill_circle(cx, cy, 3.f, RGB3(k), 255);
    }
    if (FIC("#cc-liq")) {                        /* drop */
        unsigned k = g_dial.liq_on ? 0x0B1220u : cc_off;
        int xs[3] = { (int)cx, (int)cx - 8, (int)cx + 8 };
        int ys[3] = { (int)cy - 13, (int)cy + 2, (int)cy + 2 };
        html_view_fill_poly(xs, ys, 3, RGB3(k), 255);
        html_view_fill_circle(cx, cy + 4.f, 8.f, RGB3(k), 255);
    }
    if (FIC("#cc-theme")) {                      /* the palette's two accents and green */
        html_view_fill_circle(cx - 8.f, cy - 6.f, 5.f, RGB3(theme_a()), 255);
        html_view_fill_circle(cx + 8.f, cy - 6.f, 5.f, RGB3(theme_b()), 255);
        html_view_fill_circle(cx, cy + 6.f, 5.f, RGB3(C_OK), 255);
    }
    for (int i = 0; i < 32; i++) {              /* custom pages: app grid */
        char sel[24];
        snprintf(sel, sizeof sel, "#fic-custom-%d", i);
        if (!FIC(sel)) break;
        for (int k = 0; k < 4; k++)
            html_view_fill_round_rect((int)cx - 9 + (k % 2) * 10, (int)cy - 9 + (k / 2) * 10, 8, 8, 2, RGB3(c), 255);
    }
#undef FIC
}

struct charge_boot_state {
    int pct;
    int charger;
    int charging;
    int full;
    long chg_uv;
    long chg_ua;
};

static void charge_boot_refresh(struct charge_boot_state *s)
{
    static devui_data_t d;
    char st[32];
    long v;
    memset(s, 0, sizeof *s);
    s->pct = -1;

    if (data_refresh(&d)) {
        s->pct = d.bat_percent;
        s->charger = d.charger_connect ? 1 : 0;
        s->charging = (d.charger_connect || d.charging == 1) ? 1 : 0;   /* charge_status 1 = charging */
        s->chg_uv = d.chg_uv;
        s->chg_ua = d.chg_ua;
    }
    if (s->pct < 0 && read_long_path("/sys/class/power_supply/battery/capacity", &v))
        s->pct = (int)v;
    if (read_line_path("/sys/class/power_supply/battery/status", st, sizeof st)) {
        if (!strcmp(st, "Full")) s->full = 1;
        if (!strcmp(st, "Charging")) s->charging = 1;
    }
    if (!s->charger && read_long_path("/sys/class/power_supply/usb/online", &v))
        s->charger = v != 0;
    if (!s->charger && read_long_path("/sys/class/power_supply/usb/present", &v))
        s->charger = v != 0;
    if (!s->charger && (s->charging || s->full)) s->charger = 1;
    if (s->pct < 0) s->pct = 0;
    if (s->pct > 100) s->pct = 100;
}

static void render_charge_boot(drm_disp_t *disp)
{
    struct charge_boot_state s;
    const int W = disp->width, H = disp->height;
    const int bx = 54, by = 130, bw = 208, bh = 104;
    const int ix = bx + 7, iy = by + 7, iw = bw - 14, ih = bh - 14;
    const int tipw = 10, tiph = 32;
    int fill_w, pulse, fr, fg, fb;
    char pct[8], line1[32], line2[64];

    charge_boot_refresh(&s);
    g_phase++;

    html_view_fill_rect(0, 0, W, H, 0x09, 0x0d, 0x14, 255);
    html_view_fill_round_rect(-28, 36, 154, 154, 72, 0x18, 0x24, 0x38, 88);
    html_view_fill_round_rect(184, 282, 156, 156, 78, 0x0f, 0x2a, 0x22, 72);
    draw_center_text_px(W / 2, 50, "关机充电", 20, 0, 0xd9, 0xdf, 0xea, 255);

    html_view_fill_round_rect(bx, by, bw, bh, 26, 0xd9, 0xdf, 0xea, 255);
    html_view_fill_round_rect(bx + 3, by + 3, bw - 6, bh - 6, 23, 0x0f, 0x14, 0x1d, 255);
    html_view_fill_round_rect(bx + bw, by + (bh - tiph) / 2, tipw, tiph, 4, 0xd9, 0xdf, 0xea, 255);

    pulse = (int)(g_phase % 24);
    if (pulse > 12) pulse = 24 - pulse;
    fr = 0x5e; fg = 0xc8; fb = 0x5e;
    if (s.full) { fr = 0x58; fg = 0xb7; fb = 0xff; }
    else if (s.pct <= 20 && !s.charger) { fr = 0xe8; fg = 0x53; fb = 0x3a; }
    if (s.charger && !s.full) {
        fr = clampi(fr + pulse * 3, 0, 255);
        fg = clampi(fg + pulse * 2, 0, 255);
        fb = clampi(fb + pulse * 3 / 2, 0, 255);
    }
    fill_w = iw * s.pct / 100;
    if (fill_w <= 0 && s.charger) fill_w = 8;
    if (fill_w > 0) {
        html_view_fill_round_rect(ix, iy, fill_w, ih, 18, fr, fg, fb, 255);
        if (s.charger && !s.full && fill_w > 20) {
            int sheen = ix + ((int)(g_phase * 11) % (fill_w + 28)) - 28;
            if (sheen < ix) sheen = ix;
            if (sheen < ix + fill_w)
                html_view_fill_rect(sheen, iy + 6, 18, ih - 12, 0xff, 0xff, 0xff, 42);
        }
    }

    if (s.charger) {
        int cx = W / 2, cy = by + bh / 2;
        int xs[7] = { cx - 12, cx + 2, cx - 3, cx + 11, cx - 8, cx - 2, cx - 11 };
        int ys[7] = { cy - 30, cy - 30, cy - 6, cy - 6, cy + 30, cy + 5, cy + 5 };
        int shx[7], shy[7];
        int br = s.full ? 0xf5 : 0xff;
        int bg = s.full ? 0xfb : 0xf4;
        int bb = s.full ? 0xff : 0xa7;
        for (int i = 0; i < 7; i++) { shx[i] = xs[i] + 1; shy[i] = ys[i] + 2; }
        html_view_fill_poly(shx, shy, 7, 0x05, 0x08, 0x0c, 120);
        html_view_fill_poly(xs, ys, 7, br, bg, bb, 245);
    }

    snprintf(pct, sizeof pct, "%d%%", s.pct);
    draw_center_text_px(W / 2, 274, pct, 46, 1, 0xf5, 0xf7, 0xfa, 255);

    if (s.full) snprintf(line1, sizeof line1, "已充满");
    else if (s.charging) snprintf(line1, sizeof line1, "充电中");
    else if (s.charger) snprintf(line1, sizeof line1, "已接入电源");
    else snprintf(line1, sizeof line1, "等待充电");
    draw_center_text_px(W / 2, 330, line1, 20, 0, 0x9f, 0xd6, 0xb5, 255);

    if (s.chg_uv > 0 && s.chg_ua > 0)
        snprintf(line2, sizeof line2, "输入 %.1fV · %ldmA", s.chg_uv / 1000000.0, s.chg_ua / 1000);
    else
        snprintf(line2, sizeof line2, "按电源键可熄屏/亮屏");
    draw_center_text_px(W / 2, 364, line2, 14, 0, 0x8e, 0x98, 0xa6, 255);

    drm_disp_dirty(disp, 0, 0, W - 1, H - 1);
    maybe_dump_fb(disp);
}

static int  g_modal;           /* 0 none, 1 SA, 2 NSA, 3 LTE, 4 signal settings */
static int  g_segdrag;         /* dragging the segmented control (suppress cell highlight) */
static char g_toast[48];       /* toast message ("" = hidden) */
static uint32_t g_toast_until; /* millis the toast hides at */
static int  g_pwr_confirm;     /* power menu: 0 none, 1 poweroff armed, 2 reboot armed */
static uint32_t g_pwr_until;   /* millis the armed state auto-resets at */
static char g_net_pending[16]; /* optimistic net mode until net_select catches up */
static char g_uni_sa[256], g_uni_nsa[256], g_uni_lte[256];   /* available bands (max seen) */
static char g_sel_sa[256], g_sel_nsa[256], g_sel_lte[256];   /* selected (to lock) */

static int band_count(const char *s) { if (!s[0]) return 0; int n = 1; for (; *s; s++) if (*s == ',') n++; return n; }

static int band_in(const char *list, const char *b)
{
    size_t bl = strlen(b);
    for (const char *p = list; *p; ) {
        const char *c = p; while (*c && *c != ',') c++;
        if ((size_t)(c - p) == bl && !strncmp(p, b, bl)) return 1;
        p = *c ? c + 1 : c;
    }
    return 0;
}
static void band_toggle(char *list, size_t cap, const char *b)
{
    if (band_in(list, b)) {
        char out[256]; int o = 0; size_t bl = strlen(b);
        for (const char *p = list; *p; ) {
            const char *c = p; while (*c && *c != ',') c++;
            if (!((size_t)(c - p) == bl && !strncmp(p, b, bl))) {
                if (o) out[o++] = ',';
                int n = (int)(c - p); if (o + n < (int)sizeof out) { memcpy(out + o, p, n); o += n; }
            }
            p = *c ? c + 1 : c;
        }
        out[o] = 0; snprintf(list, cap, "%s", out);
    } else {
        size_t l = strlen(list);
        snprintf(list + l, cap - l, "%s%s", l ? "," : "", b);
    }
}
/* Render bands as pill chips (selected = highlighted) with the given classes. */
static void build_chips_cls(char *buf, size_t cap, const char *uni, const char *sel,
                            const char *act, const char *pfx, const char *on, const char *off)
{
    char u[256]; snprintf(u, sizeof u, "%s", uni);
    int o = 0;
    for (char *tk = strtok(u, ","); tk; tk = strtok(NULL, ",")) {
        o += snprintf(buf + o, cap - o, "<a href='act:%s:%s' class='%s'>%s%s</a>",
                      act, tk, band_in(sel, tk) ? on : off, pfx, tk);
    }
    if (o == 0) snprintf(buf, cap, "<span class='muted'>暂无可选频段</span>");
}

/* One-line summary of a card's current band selection. */
static void band_summary(char *buf, size_t cap, const char *uni, const char *sel, const char *pfx)
{
    if (!sel[0]) { snprintf(buf, cap, "未选择频段"); return; }
    if (!strcmp(uni, sel)) { snprintf(buf, cap, "全部频段（未锁定）"); return; }
    char s[256]; snprintf(s, sizeof s, "%s", sel);
    int o = 0;
    for (char *tk = strtok(s, ","); tk; tk = strtok(NULL, ","))
        o += snprintf(buf + o, cap - o, "%s%s%s", o ? " " : "已锁 ", pfx, tk);
}

static void net_mode_apply(const char *mode)
{
    char params[64], cmd[160];

    snprintf(params, sizeof params, "{\"mode\":\"%s\"}", mode);
    snprintf(cmd, sizeof cmd,
             "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1", mode);
    datad_control_async("network.set_mode", params, cmd);
}

/* segmented-control cells (shared with the touch handler). */
static const struct { const char *v, *lab; } g_netmodes[4] = {
    { "WL_AND_5G", "自动" }, { "Only_5G", "5G SA" },
    { "LTE_AND_5G", "5G NSA" }, { "Only_LTE", "4G" } };
/* The TopFlow firmware names its automatic mode 4G_AND_5G. */
static const char *netmode_value(int k)
{
    return k == 0 && dev_is_tf() ? "4G_AND_5G" : g_netmodes[k].v;
}
static const struct { int ms; const char *lab; } g_autooffs[6] = {
    { 0, "关" }, { 30000, "30秒" }, { 60000, "1分" },
    { 120000, "2分" }, { 300000, "5分" }, { 600000, "10分" } };
static const struct { int ms; const char *lab; } g_refresh_rates[5] = {
    { 0, "停止" }, { 500, "0.5s" }, { 1000, "1s" }, { 2000, "2s" }, { 5000, "5s" } };
static int normalize_refresh_ms(int ms)
{
    switch (ms) {
    case 0:
    case 500:
    case 1000:
    case 2000:
    case 5000:
        return ms;
    default:
        return 1000;
    }
}

/* The /modem/... signaling API only exists in the private datad build; the public
 * datad serves neighbors through /state and its own control actions instead.
 * Probe once datad answers and re-check rarely, so a public datad costs no
 * per-refresh requests. -1 = not known yet. */
static int g_modem_api = -1;
static uint32_t g_modem_api_probe_at;

static int modem_api_available(void)
{
    uint32_t now = millis();
    int status;

    if (g_modem_api >= 0 && (uint32_t)(now - g_modem_api_probe_at) < 300000)
        return g_modem_api;
    if (g_modem_api < 0 && g_modem_api_probe_at &&
        (uint32_t)(now - g_modem_api_probe_at) < 5000)
        return 0;
    g_modem_api_probe_at = now;
    status = datad_request("GET", "/modem/signal-metrics", NULL, NULL, 0,
                           DATAD_HTTP_TIMEOUT_MS);
    if (status > 0) g_modem_api = status != 404;
    return g_modem_api > 0;
}

static void datad_modem_control(const char *query)
{
    char path[160];

    if (snprintf(path, sizeof path, "/modem/control?%s", query) >= (int)sizeof path)
        return;
    (void)datad_request("POST", path, "", NULL, 0, DATAD_HTTP_TIMEOUT_MS);
}

static int datad_set_refresh_ms(int ms)
{
    char params[48];

    ms = normalize_refresh_ms(ms);
    if (modem_api_available()) {
        char q[32];
        snprintf(q, sizeof q, "poll_ms=%d", ms);
        datad_modem_control(q);
    }
    /* 0 only pauses the screen; datad keeps sampling at its own period. */
    if (ms <= 0) return 0;
    snprintf(params, sizeof params, "{\"milliseconds\":%d}", ms);
    datad_control_async("state.set_interval", params, NULL);
    return 0;
}

static int datad_set_signal_read(int on)
{
    if (modem_api_available()) {
        char q[48];
        snprintf(q, sizeof q, "ml1_raw=%d&active=%d", on ? 1 : 0, (on || g_sig_parse) ? 1 : 0);
        datad_modem_control(q);
        return 0;
    }
    /* Public datad: "read signaling" drives the DIAG neighbor collector. */
    datad_control_async("neighbor.set", on ? "{\"enabled\":true}" : "{\"enabled\":false}", NULL);
    return 0;
}

static int datad_set_signal_parse(int on)
{
    if (modem_api_available()) {
        char q[48];
        snprintf(q, sizeof q, "lte=%d&nr=%d&active=%d", on ? 1 : 0, on ? 1 : 0,
                 (g_sig_read || on) ? 1 : 0);
        datad_modem_control(q);
    }
    /* Public datad has no decoder switch; parse only gates the UI pages. */
    return 0;
}

#define SIGNAL_CACHE_PATH g_signal_cache_file

static int signal_cache_get(const char *key, char *dst, size_t cap)
{
    FILE *fp;
    char line[192];
    size_t key_len;
    int found = 0;

    if (!key || !dst || cap == 0)
        return 0;
    key_len = strlen(key);
    fp = fopen(SIGNAL_CACHE_PATH, "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof line, fp)) {
        char *v;
        char *e;

        if (strncmp(line, key, key_len) || line[key_len] != '=')
            continue;
        v = line + key_len + 1;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
            *--e = '\0';
        if (*v) {
            snprintf(dst, cap, "%s", v);
            found = 1;
        }
    }
    fclose(fp);
    return found;
}

static void signal_cache_put(const char *key, const char *value)
{
    FILE *in;
    FILE *out;
    char line[256];
    char old[128];
    char tmp_path[sizeof(SIGNAL_CACHE_PATH) + 8];
    size_t key_len;

    if (!key || !value || !value[0] || !strcmp(value, "-") ||
        !strcmp(value, "已关闭"))
        return;
    if (strchr(key, '=') || strchr(key, '\n') || strchr(value, '\n') ||
        strchr(value, '\r'))
        return;
    if (signal_cache_get(key, old, sizeof old) && !strcmp(old, value))
        return;
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", SIGNAL_CACHE_PATH);
    out = fopen(tmp_path, "w");
    if (!out)
        return;
    key_len = strlen(key);
    in = fopen(SIGNAL_CACHE_PATH, "r");
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, key, key_len) && line[key_len] == '=')
                continue;
            fputs(line, out);
        }
        fclose(in);
    }
    fprintf(out, "%s=%s\n", key, value);
    if (fclose(out) != 0 || rename(tmp_path, SIGNAL_CACHE_PATH) != 0)
        unlink(tmp_path);
}

struct signal_bundle {
    char cell_key[64];
    char ta[32];
    char lte_ta[32];
    char lte_rrc[128];
    char lte_nas[128];
    char lte_mcs[32];
    char lte_modulation[32];
    char lte_rb[32];
    char lte_grants[32];
    char lte_bler[32];
    char lte_pusch[96];
    char lte_pucch[96];
    char nr_ta[32];
    char nr_rrc[128];
    char nr_nas[128];
    char nr_mcs[32];
    char nr_modulation[32];
    char nr_mimo[64];
    char nr_layers[32];
    char nr_dl_rb[32];
    char nr_dl_grants[32];
    char nr_ul_rb[32];
    char nr_ul_grants[32];
    char nr_bler[32];
    char nr_pusch[96];
    char nr_pucch[96];
    char nr_ports[64];
    char nr_beam[128];
    char nr_serving_ssb[32];
    char nr_vendor[64];
    int lte_carrier_n;
    int nr_carrier_n;
    int lte_neighbor_n;
    int nr_neighbor_n;
    struct signal_carrier_metric lte_carriers[SIGNAL_MAX_CARRIERS];
    struct signal_carrier_metric nr_carriers[SIGNAL_MAX_CARRIERS];
    struct signal_neighbor_metric lte_neighbors[SIGNAL_MAX_NEIGHBORS];
    struct signal_neighbor_metric nr_neighbors[SIGNAL_MAX_NEIGHBORS];
};

static void signal_bundle_reset(struct signal_bundle *b)
{
    if (!b) return;
    snprintf(b->cell_key, sizeof b->cell_key, "-");
    snprintf(b->ta, sizeof b->ta, "-");
    snprintf(b->lte_ta, sizeof b->lte_ta, "-");
    snprintf(b->lte_rrc, sizeof b->lte_rrc, "-");
    snprintf(b->lte_nas, sizeof b->lte_nas, "-");
    snprintf(b->lte_mcs, sizeof b->lte_mcs, "-");
    snprintf(b->lte_modulation, sizeof b->lte_modulation, "-");
    snprintf(b->lte_rb, sizeof b->lte_rb, "-");
    snprintf(b->lte_grants, sizeof b->lte_grants, "-");
    snprintf(b->lte_bler, sizeof b->lte_bler, "-");
    snprintf(b->lte_pusch, sizeof b->lte_pusch, "-");
    snprintf(b->lte_pucch, sizeof b->lte_pucch, "-");
    snprintf(b->nr_ta, sizeof b->nr_ta, "-");
    snprintf(b->nr_rrc, sizeof b->nr_rrc, "-");
    snprintf(b->nr_nas, sizeof b->nr_nas, "-");
    snprintf(b->nr_mcs, sizeof b->nr_mcs, "-");
    snprintf(b->nr_modulation, sizeof b->nr_modulation, "-");
    snprintf(b->nr_mimo, sizeof b->nr_mimo, "-");
    snprintf(b->nr_layers, sizeof b->nr_layers, "-");
    snprintf(b->nr_dl_rb, sizeof b->nr_dl_rb, "-");
    snprintf(b->nr_dl_grants, sizeof b->nr_dl_grants, "-");
    snprintf(b->nr_ul_rb, sizeof b->nr_ul_rb, "-");
    snprintf(b->nr_ul_grants, sizeof b->nr_ul_grants, "-");
    snprintf(b->nr_bler, sizeof b->nr_bler, "-");
    snprintf(b->nr_pusch, sizeof b->nr_pusch, "-");
    snprintf(b->nr_pucch, sizeof b->nr_pucch, "-");
    snprintf(b->nr_ports, sizeof b->nr_ports, "-");
    snprintf(b->nr_beam, sizeof b->nr_beam, "-");
    snprintf(b->nr_serving_ssb, sizeof b->nr_serving_ssb, "-");
    snprintf(b->nr_vendor, sizeof b->nr_vendor, "-");
    b->lte_carrier_n = 0;
    b->nr_carrier_n = 0;
    b->lte_neighbor_n = 0;
    b->nr_neighbor_n = 0;
    for (int i = 0; i < SIGNAL_MAX_CARRIERS; i++) {
        signal_carrier_metric_reset(&b->lte_carriers[i]);
        signal_carrier_metric_reset(&b->nr_carriers[i]);
    }
    for (int i = 0; i < SIGNAL_MAX_NEIGHBORS; i++) {
        signal_neighbor_metric_reset(&b->lte_neighbors[i]);
        signal_neighbor_metric_reset(&b->nr_neighbors[i]);
    }
}

static int signal_value_present(const char *s)
{
    return s && s[0] && strcmp(s, "-") != 0 && strcmp(s, "已关闭") != 0;
}

static void signal_value_keep_last(char *cur, size_t cur_cap, char *sticky, size_t sticky_cap)
{
    if (!cur || cur_cap == 0 || !sticky || sticky_cap == 0)
        return;
    if (signal_value_present(cur)) {
        snprintf(sticky, sticky_cap, "%s", cur);
        return;
    }
    if (signal_value_present(sticky))
        snprintf(cur, cur_cap, "%s", sticky);
}

static void signal_metric_get(const char *obj, const char *key, char *dst, size_t cap)
{
    char tmp[128];

    if (!obj || !key || !dst || cap == 0)
        return;
    if (json_get(obj, key, tmp, sizeof tmp) && signal_value_present(tmp))
        snprintf(dst, cap, "%s", tmp);
}

static void signal_copy_value(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (!dst || cap == 0)
        return;
    if (!src)
        src = "";
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int signal_json_next_object(const char **pp, char *dst, size_t cap)
{
    const char *p;
    size_t n = 0;
    int depth = 0;
    int in_str = 0;

    if (!pp || !*pp || !dst || cap == 0)
        return 0;
    p = *pp;
    while (*p && *p != '{')
        p++;
    if (*p != '{')
        return 0;
    while (*p && n < cap - 1) {
        char ch = *p;
        if (in_str) {
            if (ch == '\\' && p[1]) {
                dst[n++] = *p++;
                if (n < cap - 1)
                    dst[n++] = *p++;
                continue;
            }
            if (ch == '"')
                in_str = 0;
        } else {
            if (ch == '"')
                in_str = 1;
            else if (ch == '{')
                depth++;
            else if (ch == '}') {
                depth--;
                dst[n++] = *p++;
                if (depth == 0)
                    break;
                continue;
            }
        }
        dst[n++] = *p++;
    }
    dst[n] = 0;
    *pp = p;
    return depth == 0 && n > 0;
}

static void signal_carrier_metric_get(const char *obj, const char *key,
                                      char *dst, size_t cap)
{
    char tmp[96];

    if (!obj || !key || !dst || cap == 0)
        return;
    if (json_get(obj, key, tmp, sizeof tmp) && signal_value_present(tmp))
        signal_copy_value(dst, cap, tmp);
}

static int signal_parse_carriers(const char *parent, int is_nr,
                                 struct signal_carrier_metric *out, int max)
{
    char arr[16384];
    char obj[1536];
    const char *p;
    int n = 0;

    if (!parent || !out || max <= 0)
        return 0;
    if (!json_get(parent, "carriers", arr, sizeof arr))
        return 0;
    p = arr;
    while (n < max && signal_json_next_object(&p, obj, sizeof obj)) {
        struct signal_carrier_metric *m = &out[n];
        signal_carrier_metric_reset(m);
        m->valid = 1;
        signal_carrier_metric_get(obj, "name", m->name, sizeof m->name);
        signal_carrier_metric_get(obj, "type", m->type, sizeof m->type);
        signal_carrier_metric_get(obj, "pci", m->pci, sizeof m->pci);
        signal_carrier_metric_get(obj, is_nr ? "nrarfcn" : "earfcn", m->arfcn, sizeof m->arfcn);
        signal_carrier_metric_get(obj, "rb", m->rb, sizeof m->rb);
        signal_carrier_metric_get(obj, "grants", m->grants, sizeof m->grants);
        signal_carrier_metric_get(obj, "dl_rb", m->dl_rb, sizeof m->dl_rb);
        signal_carrier_metric_get(obj, "dl_grants", m->dl_grants, sizeof m->dl_grants);
        signal_carrier_metric_get(obj, "ul_rb", m->ul_rb, sizeof m->ul_rb);
        signal_carrier_metric_get(obj, "ul_grants", m->ul_grants, sizeof m->ul_grants);
        signal_carrier_metric_get(obj, "mcs", m->mcs, sizeof m->mcs);
        signal_carrier_metric_get(obj, "modulation", m->modulation, sizeof m->modulation);
        signal_carrier_metric_get(obj, "mimo", m->mimo, sizeof m->mimo);
        signal_carrier_metric_get(obj, "layers", m->layers, sizeof m->layers);
        signal_carrier_metric_get(obj, "bler_pct", m->bler, sizeof m->bler);
        signal_carrier_metric_get(obj, "ssb", m->ssb, sizeof m->ssb);
        signal_carrier_metric_get(obj, "serving_ssb", m->serving_ssb, sizeof m->serving_ssb);
        signal_carrier_metric_get(obj, "ml1_rsrp", m->ml1_rsrp, sizeof m->ml1_rsrp);
        signal_carrier_metric_get(obj, "ml1_rsrq", m->ml1_rsrq, sizeof m->ml1_rsrq);
        signal_carrier_metric_get(obj, "ml1_sinr", m->ml1_sinr, sizeof m->ml1_sinr);
        n++;
    }
    return n;
}

static void signal_lte_band_from_earfcn(const char *earfcn, char *dst, size_t cap)
{
    struct range { long lo, hi; const char *band; };
    static const struct range ranges[] = {
        { 0, 599, "B1" }, { 600, 1199, "B2" }, { 1200, 1949, "B3" },
        { 1950, 2399, "B4" }, { 2400, 2649, "B5" }, { 2750, 3449, "B7" },
        { 3450, 3799, "B8" }, { 5010, 5179, "B12" }, { 5180, 5279, "B13" },
        { 5730, 5849, "B17" }, { 5850, 5999, "B18" }, { 6000, 6149, "B19" },
        { 6150, 6449, "B20" }, { 8690, 9039, "B26" }, { 9210, 9659, "B28" },
        { 36200, 36349, "B34" }, { 37750, 38249, "B38" },
        { 38250, 38649, "B39" }, { 38650, 39649, "B40" },
        { 39650, 41589, "B41" },
    };
    char *end = NULL;
    long v;

    if (!dst || cap == 0) return;
    if (!signal_value_present(earfcn)) { snprintf(dst, cap, "-"); return; }
    v = strtol(earfcn, &end, 10);
    if (end == earfcn || v <= 0) { snprintf(dst, cap, "-"); return; }
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
        if (v >= ranges[i].lo && v <= ranges[i].hi) {
            snprintf(dst, cap, "%s", ranges[i].band);
            return;
        }
    snprintf(dst, cap, "E%.20s", earfcn);
}

static void signal_nr_band_from_nrarfcn(const char *nrarfcn, char *dst, size_t cap)
{
    struct range { long lo, hi; const char *band; };
    static const struct range ranges[] = {
        { 151600, 160600, "n28" }, { 173800, 178800, "n5" },
        { 185000, 192000, "n8" }, { 361000, 376000, "n3" },
        { 422000, 434000, "n1" }, { 460000, 480000, "n40" },
        { 499200, 537999, "n41" }, { 514000, 524000, "n38" },
        { 620000, 653333, "n78" }, { 653334, 680000, "n77" },
        { 636667, 646666, "n48" }, { 693334, 733333, "n79" },
    };
    char *end = NULL;
    long v;

    if (!dst || cap == 0) return;
    if (!signal_value_present(nrarfcn)) { snprintf(dst, cap, "-"); return; }
    v = strtol(nrarfcn, &end, 10);
    if (end == nrarfcn || v <= 0) { snprintf(dst, cap, "-"); return; }
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
        if (v >= ranges[i].lo && v <= ranges[i].hi) {
            snprintf(dst, cap, "%s", ranges[i].band);
            return;
        }
    snprintf(dst, cap, "N%.20s", nrarfcn);
}

static int signal_parse_neighbors(const char *parent, int is_nr,
                                  struct signal_neighbor_metric *out, int max)
{
    char arr[8192];
    char obj[1024];
    const char *p;
    int n = 0;

    if (!parent || !out || max <= 0)
        return 0;
    if (!json_get(parent, "neighbors", arr, sizeof arr))
        return 0;
    p = arr;
    while (n < max && signal_json_next_object(&p, obj, sizeof obj)) {
        struct signal_neighbor_metric *m = &out[n];
        signal_neighbor_metric_reset(m);
        signal_carrier_metric_get(obj, "pci", m->pci, sizeof m->pci);
        signal_carrier_metric_get(obj, "band", m->band, sizeof m->band);
        signal_carrier_metric_get(obj, is_nr ? "nrarfcn" : "earfcn", m->arfcn, sizeof m->arfcn);
        if (!signal_value_present(m->arfcn))
            signal_carrier_metric_get(obj, "arfcn", m->arfcn, sizeof m->arfcn);
        signal_carrier_metric_get(obj, "rsrp", m->rsrp, sizeof m->rsrp);
        signal_carrier_metric_get(obj, "rsrq", m->rsrq, sizeof m->rsrq);
        if (!signal_value_present(m->band)) {
            if (is_nr)
                signal_nr_band_from_nrarfcn(m->arfcn, m->band, sizeof m->band);
            else
                signal_lte_band_from_earfcn(m->arfcn, m->band, sizeof m->band);
        }
        if (signal_value_present(m->pci) ||
            signal_value_present(m->rsrp) ||
            signal_value_present(m->rsrq)) {
            m->valid = 1;
            n++;
        }
    }
    return n;
}

static int signal_carrier_is_pcell(const struct signal_carrier_metric *m)
{
    if (!m || !m->valid)
        return 0;
    return !strcasecmp(m->type, "PCell") || !strcasecmp(m->name, "PCell");
}

static int signal_carrier_same_text(const char *a, const char *b)
{
    return signal_value_present(a) && signal_value_present(b) && !strcmp(a, b);
}

static struct signal_carrier_metric *signal_pick_carrier_metric(struct signal_carrier_metric *list,
                                                               int n,
                                                               const char *pci,
                                                               const char *arfcn,
                                                               int prefer_pcell)
{
    struct signal_carrier_metric *pcell_fallback = NULL;

    if (!list || n <= 0)
        return NULL;

    for (int i = 0; i < n; i++) {
        struct signal_carrier_metric *m = &list[i];
        if (!m->valid || m->used)
            continue;
        if (prefer_pcell && !signal_carrier_is_pcell(m))
            continue;
        if (!prefer_pcell && signal_carrier_is_pcell(m))
            continue;
        if (signal_carrier_same_text(m->arfcn, arfcn)) {
            if (signal_value_present(m->pci) && signal_value_present(pci) &&
                !signal_carrier_same_text(m->pci, pci))
                continue;
            m->used = 1;
            return m;
        }
        if (prefer_pcell && signal_carrier_same_text(m->pci, pci)) {
            m->used = 1;
            return m;
        }
        if (prefer_pcell && !pcell_fallback)
            pcell_fallback = m;
    }

    if (pcell_fallback)
        pcell_fallback->used = 1;
    return prefer_pcell ? pcell_fallback : NULL;
}

static void signal_bundle_apply_metrics(struct signal_bundle *b)
{
    static char json[32768];
    static char lte[8192];
    static char nr[24576];
    FILE *fp;
    size_t n;
    int rc;

    if (!b || !g_sig_read || !modem_api_available())
        return;
    fp = popen("/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/signal-metrics' 2>/dev/null", "r");
    if (!fp)
        return;
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0)
        return;
    json[n] = 0;

    signal_metric_get(json, "cell_vendor", b->nr_vendor, sizeof b->nr_vendor);
    signal_metric_get(json, "cell_key", b->cell_key, sizeof b->cell_key);
    signal_metric_get(json, "ta", b->ta, sizeof b->ta);
    if (json_get(json, "lte", lte, sizeof lte)) {
        signal_metric_get(lte, "ta", b->lte_ta, sizeof b->lte_ta);
        signal_metric_get(lte, "mcs", b->lte_mcs, sizeof b->lte_mcs);
        signal_metric_get(lte, "modulation", b->lte_modulation, sizeof b->lte_modulation);
        signal_metric_get(lte, "rb", b->lte_rb, sizeof b->lte_rb);
        signal_metric_get(lte, "grants", b->lte_grants, sizeof b->lte_grants);
        signal_metric_get(lte, "bler_pct", b->lte_bler, sizeof b->lte_bler);
        signal_metric_get(lte, "pusch", b->lte_pusch, sizeof b->lte_pusch);
        signal_metric_get(lte, "pucch", b->lte_pucch, sizeof b->lte_pucch);
        b->lte_carrier_n = signal_parse_carriers(lte, 0, b->lte_carriers,
                                                 SIGNAL_MAX_CARRIERS);
        b->lte_neighbor_n = signal_parse_neighbors(lte, 0, b->lte_neighbors,
                                                   SIGNAL_MAX_NEIGHBORS);
    }
    if (json_get(json, "nr", nr, sizeof nr)) {
        signal_metric_get(nr, "ta", b->nr_ta, sizeof b->nr_ta);
        signal_metric_get(nr, "mcs", b->nr_mcs, sizeof b->nr_mcs);
        signal_metric_get(nr, "modulation", b->nr_modulation, sizeof b->nr_modulation);
        signal_metric_get(nr, "mimo", b->nr_mimo, sizeof b->nr_mimo);
        signal_metric_get(nr, "layers", b->nr_layers, sizeof b->nr_layers);
        signal_metric_get(nr, "dl_rb", b->nr_dl_rb, sizeof b->nr_dl_rb);
        signal_metric_get(nr, "dl_grants", b->nr_dl_grants, sizeof b->nr_dl_grants);
        signal_metric_get(nr, "ul_rb", b->nr_ul_rb, sizeof b->nr_ul_rb);
        signal_metric_get(nr, "ul_grants", b->nr_ul_grants, sizeof b->nr_ul_grants);
        signal_metric_get(nr, "bler_pct", b->nr_bler, sizeof b->nr_bler);
        signal_metric_get(nr, "pusch", b->nr_pusch, sizeof b->nr_pusch);
        signal_metric_get(nr, "pucch", b->nr_pucch, sizeof b->nr_pucch);
        signal_metric_get(nr, "ports", b->nr_ports, sizeof b->nr_ports);
        signal_metric_get(nr, "ssb", b->nr_beam, sizeof b->nr_beam);
        signal_metric_get(nr, "serving_ssb", b->nr_serving_ssb, sizeof b->nr_serving_ssb);
        b->nr_carrier_n = signal_parse_carriers(nr, 1, b->nr_carriers,
                                                SIGNAL_MAX_CARRIERS);
        b->nr_neighbor_n = signal_parse_neighbors(nr, 1, b->nr_neighbors,
                                                  SIGNAL_MAX_NEIGHBORS);
    }
    if (!signal_value_present(b->ta)) {
        if (signal_value_present(b->nr_ta))
            snprintf(b->ta, sizeof b->ta, "%s", b->nr_ta);
        else if (signal_value_present(b->lte_ta))
            snprintf(b->ta, sizeof b->ta, "%s", b->lte_ta);
    }
}

static void signal_slot_label(const char *slot_json, char *dst, size_t cap)
{
    char data[8192];
    char decoded[8192];
    char message[1024];
    char name[256];
    char scope[256];
    char summary[256];

    if (!dst || cap == 0) return;
    if (!slot_json || !slot_json[0] || !strncmp(slot_json, "null", 4)) {
        snprintf(dst, cap, "-");
        return;
    }
    if (json_get(slot_json, "data", data, sizeof data)) {
        if (json_get(data, "decoded", decoded, sizeof decoded)) {
            if (json_get(decoded, "message", message, sizeof message) && message[0]) {
                if (message[0] == '{') {
                    if (json_get(message, "name", name, sizeof name) && name[0]) {
                        snprintf(dst, cap, "%s", name);
                        return;
                    }
                } else {
                    snprintf(dst, cap, "%s", message);
                    return;
                }
            }
            if (json_get(decoded, "scope", scope, sizeof scope) && scope[0]) {
                snprintf(dst, cap, "%s", scope);
                return;
            }
        }
        if (json_get(data, "summary", summary, sizeof summary) && summary[0]) {
            snprintf(dst, cap, "%s", summary);
            return;
        }
    }
    snprintf(dst, cap, "-");
}

static int json_pick_scalar_string(const char *obj, const char *const *keys, size_t nkeys,
                                   char *dst, size_t cap);
static int fetch_signal_raw_field(const char *kind, const char *const *keys, size_t nkeys,
                                  char *dst, size_t cap);
static int fetch_signal_raw_vendor(const char *kind, char *dst, size_t cap);

static int fetch_signal_raw_int_field(const char *kind, const char *const *keys, size_t nkeys,
                                      char *dst, size_t cap)
{
    char json[4096];
    char data[3600];
    char cmd[256];
    FILE *fp;
    size_t n;
    int rc;

    if (!dst || cap == 0 || !kind || !keys || nkeys == 0 || !g_sig_read ||
        !modem_api_available())
        return 0;
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest?kind=%s' 2>/dev/null",
                 kind) >= (int)sizeof cmd)
        return 0;

    fp = popen(cmd, "r");
    if (!fp) return 0;
    n = fread(json, 1, sizeof json - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0) return 0;
    json[n] = 0;
    if (!json_get(json, "data", data, sizeof data))
        return 0;

    for (size_t i = 0; i < nkeys; i++) {
        long v = json_get_int(data, keys[i], -2147483647L);
        if (v != -2147483647L) {
            snprintf(dst, cap, "%ld", v);
            return 1;
        }
    }
    return 0;
}

static int signal_key_tail_is_boundary(char c)
{
    return !((c >= 'A' && c <= 'Z') ||
             (c >= 'a' && c <= 'z') ||
             (c >= '0' && c <= '9') ||
             c == '_' || c == '-');
}

static int signal_parse_deltaf_number(const char *p, int *out)
{
    int sign = 1;
    int v = 0;
    int any = 0;

    if (!p || !out)
        return 0;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        any = 1;
        v = v * 10 + (*p - '0');
        p++;
    }
    if (!any)
        return 0;
    *out = sign * v;
    return 1;
}

static int signal_extract_deltaf_value(const char *text, const char *key, int *out)
{
    const size_t key_len = strlen(key);
    const char *p;

    if (!text || !key || !out)
        return 0;
    for (p = strstr(text, key); p; p = strstr(p + 1, key)) {
        const char *q;
        const char *limit;
        const char *v;
        const char *colon;

        if (!signal_key_tail_is_boundary(p[key_len]))
            continue;
        q = p + key_len;
        limit = q + 160;
        v = strstr(q, "deltaF");
        if (v && v < limit && signal_parse_deltaf_number(v + 6, out))
            return 1;
        colon = strchr(q, ':');
        if (!colon || colon >= limit)
            continue;
        colon++;
        while (*colon == ' ' || *colon == '\t' || *colon == '"' || *colon == '\\')
            colon++;
        if (signal_parse_deltaf_number(colon, out))
            return 1;
    }
    return 0;
}

static int signal_detect_vendor_from_text(const char *text, char *dst, size_t cap)
{
    static const char *const keys[5] = {
        "deltaF-PUCCH-Format1",
        "deltaF-PUCCH-Format1b",
        "deltaF-PUCCH-Format2",
        "deltaF-PUCCH-Format2a",
        "deltaF-PUCCH-Format2b"
    };
    int v[5];

    if (!text || !dst || cap == 0)
        return 0;
    if (signal_extract_deltaf_value(text, keys[0], &v[0]) &&
        signal_extract_deltaf_value(text, keys[1], &v[1]) &&
        signal_extract_deltaf_value(text, keys[2], &v[2]) &&
        signal_extract_deltaf_value(text, keys[3], &v[3]) &&
        signal_extract_deltaf_value(text, keys[4], &v[4])) {
        if (v[0] == 0 && v[1] == 3 && v[2] == 0 && v[3] == 0 && v[4] == 0)
            snprintf(dst, cap, "Ericsson");
        else if (v[0] == 0 && v[1] == 3 && v[2] == 1 && v[3] == 2 && v[4] == 2)
            snprintf(dst, cap, "Huawei");
        else if (v[0] == 0 && v[1] == 1 && v[2] == 0 && v[3] == 0 && v[4] == 0)
            snprintf(dst, cap, "Nokia");
        else if (v[0] == 2 && v[1] == 3 && v[2] == 1 && v[3] == 2 && v[4] == 2)
            snprintf(dst, cap, "ZTE");
        else
            return 0;
        return 1;
    }
    if (strstr(text, "maxCarriersRequestedDL") &&
        strstr(text, "maxCarriersRequestedUL")) {
        snprintf(dst, cap, "Huawei");
        return 1;
    }
    return 0;
}

static int fetch_signal_bundle(struct signal_bundle *out)
{
    static uint32_t cached_at;
    static int cached_valid;
    static struct signal_bundle cached;
    char json[16384];
    char slot[8192];
    char data[8192];
    char decoded[8192];
    uint32_t now = millis();
    FILE *fp;
    size_t n;
    int rc;
    static const char *const nr_port_keys[] = {
        "csi_rs_ports", "csi_rs_nrof_ports", "nrofPorts", "ports", "num_ports"
    };
    static const char *const nr_ssb_keys[] = {
        "ssb_positions_in_burst_active", "ssb_positions_in_burst", "ssb_index",
        "csi_ssb_resource_set", "ssb_positions_in_burst_short_bitmap",
        "ssb_positions_in_burst_medium_bitmap", "ssb_positions_in_burst_long_bitmap"
    };

    if (out) signal_bundle_reset(out);
    if (cached_at && (uint32_t)(now - cached_at) < 50) {
        if (out) *out = cached;
        return cached_valid;
    }

    cached_at = now;
    cached_valid = 0;
    signal_bundle_reset(&cached);
    if (!modem_api_available()) {
        if (out) *out = cached;
        return 0;
    }
    fp = popen("/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest-signals' 2>/dev/null", "r");
    if (!fp) {
        if (out) *out = cached;
        return 0;
    }
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc == 0 && n > 0) {
        json[n] = 0;
        if (json_get(json, "lte_rrc", slot, sizeof slot)) {
            signal_slot_label(slot, cached.lte_rrc, sizeof cached.lte_rrc);
            if (json_get(slot, "data", data, sizeof data)) {
                if (json_get(data, "decoded", decoded, sizeof decoded))
                    (void)signal_detect_vendor_from_text(decoded, cached.nr_vendor, sizeof cached.nr_vendor);
                if (!signal_value_present(cached.nr_vendor))
                    (void)signal_detect_vendor_from_text(data, cached.nr_vendor, sizeof cached.nr_vendor);
            }
        }
        if (json_get(json, "lte_nas", slot, sizeof slot)) signal_slot_label(slot, cached.lte_nas, sizeof cached.lte_nas);
        if (json_get(json, "nr_rrc", slot, sizeof slot)) {
            signal_slot_label(slot, cached.nr_rrc, sizeof cached.nr_rrc);
            if (json_get(slot, "data", data, sizeof data)) {
                if (json_get(data, "decoded", decoded, sizeof decoded)) {
                    (void)json_pick_scalar_string(decoded, nr_port_keys,
                                                  sizeof nr_port_keys / sizeof nr_port_keys[0],
                                                  cached.nr_ports, sizeof cached.nr_ports);
                    (void)json_pick_scalar_string(decoded, nr_ssb_keys,
                                                  sizeof nr_ssb_keys / sizeof nr_ssb_keys[0],
                                                  cached.nr_beam, sizeof cached.nr_beam);
                    (void)signal_detect_vendor_from_text(decoded, cached.nr_vendor, sizeof cached.nr_vendor);
                }
                if (!strcmp(cached.nr_ports, "-")) {
                    (void)json_pick_scalar_string(data, nr_port_keys,
                                                  sizeof nr_port_keys / sizeof nr_port_keys[0],
                                                  cached.nr_ports, sizeof cached.nr_ports);
                }
                if (!strcmp(cached.nr_beam, "-")) {
                    (void)json_pick_scalar_string(data, nr_ssb_keys,
                                                  sizeof nr_ssb_keys / sizeof nr_ssb_keys[0],
                                                  cached.nr_beam, sizeof cached.nr_beam);
                }
                if (!strcmp(cached.nr_ports, "-")) {
                    fetch_signal_raw_field("nr_rrc_full&scope=rrc_reconfiguration",
                                           nr_port_keys,
                                           sizeof nr_port_keys / sizeof nr_port_keys[0],
                                           cached.nr_ports, sizeof cached.nr_ports);
                }
                if (!strcmp(cached.nr_beam, "-")) {
                    fetch_signal_raw_field("nr_rrc_full&scope=rrc_reconfiguration",
                                           nr_ssb_keys,
                                           sizeof nr_ssb_keys / sizeof nr_ssb_keys[0],
                                           cached.nr_beam, sizeof cached.nr_beam);
                }
                if (!signal_value_present(cached.nr_vendor))
                    (void)signal_detect_vendor_from_text(data, cached.nr_vendor, sizeof cached.nr_vendor);
            }
        }
        if (json_get(json, "nr_nas", slot, sizeof slot)) signal_slot_label(slot, cached.nr_nas, sizeof cached.nr_nas);
        signal_bundle_apply_metrics(&cached);
        if (!signal_value_present(cached.nr_vendor)) {
            if (!fetch_signal_raw_vendor("lte_rrc_full&scope=rrc_system_information",
                                         cached.nr_vendor, sizeof cached.nr_vendor) &&
                !fetch_signal_raw_vendor("lte_rrc_full&scope=rrc_capability_enquiry",
                                         cached.nr_vendor, sizeof cached.nr_vendor))
                (void)fetch_signal_raw_vendor("nr_rrc_full&scope=rrc_capability_enquiry",
                                              cached.nr_vendor, sizeof cached.nr_vendor);
        }
        cached_valid = 1;
    }
    if (out) *out = cached;
    return cached_valid;
}

static pthread_mutex_t g_signal_async_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_signal_async_thread;
static int g_signal_async_started;
static int g_signal_async_valid;
static uint32_t g_signal_async_at;
static struct signal_bundle g_signal_async_bundle;

static int signal_async_snapshot(struct signal_bundle *out)
{
    int ok;
    uint32_t max_age;

    if (!out)
        return 0;
    pthread_mutex_lock(&g_signal_async_lock);
    ok = g_signal_async_valid;
    max_age = (uint32_t)((g_refresh_ms > 0 ? g_refresh_ms : 1000) * 2 + 250);
    if (ok && g_signal_async_at &&
        (uint32_t)(millis() - g_signal_async_at) > max_age)
        ok = 0;
    if (ok)
        *out = g_signal_async_bundle;
    pthread_mutex_unlock(&g_signal_async_lock);
    return ok;
}

static void signal_async_publish(const struct signal_bundle *src, int valid)
{
    pthread_mutex_lock(&g_signal_async_lock);
    if (src && valid) {
        g_signal_async_bundle = *src;
        g_signal_async_valid = 1;
        g_signal_async_at = millis();
    } else {
        signal_bundle_reset(&g_signal_async_bundle);
        g_signal_async_valid = 0;
        g_signal_async_at = 0;
    }
    pthread_mutex_unlock(&g_signal_async_lock);
}

static void *signal_async_worker(void *arg)
{
    uint32_t next_at = 0;
    int was_enabled = 0;
    int last_wait_ms = 0;

    (void)arg;
    signal_async_publish(NULL, 0);
    while (g_run) {
        int enabled = signal_live_enabled();
        int wait_ms = g_refresh_ms > 0 ? g_refresh_ms : 1000;
        uint32_t now = millis();

        if (!enabled) {
            if (was_enabled)
                signal_async_publish(NULL, 0);
            was_enabled = 0;
            next_at = 0;
            usleep(100000);
            continue;
        }
        if (g_refresh_ms <= 0) {
            next_at = 0;
            usleep(100000);
            continue;
        }
        if (wait_ms != last_wait_ms) {
            next_at = 0;
            last_wait_ms = wait_ms;
        }

        was_enabled = 1;
        if (next_at == 0 || (int32_t)(now - next_at) >= 0) {
            struct signal_bundle tmp;
            int ok;

            signal_bundle_reset(&tmp);
            ok = fetch_signal_bundle(&tmp);
            if (ok)
                signal_async_publish(&tmp, 1);
            next_at = millis() + (uint32_t)wait_ms;
        }
        usleep(50000);
    }
    return NULL;
}

static void signal_async_start(void)
{
    if (g_signal_async_started)
        return;
    signal_bundle_reset(&g_signal_async_bundle);
    if (pthread_create(&g_signal_async_thread, NULL, signal_async_worker, NULL) == 0)
        g_signal_async_started = 1;
}

static void signal_async_stop(void)
{
    if (!g_signal_async_started)
        return;
    (void)pthread_join(g_signal_async_thread, NULL);
    g_signal_async_started = 0;
}

static int json_pick_int_string(const char *obj, const char *const *keys, size_t nkeys,
                                char *dst, size_t cap)
{
    const long miss = -2147483647L;

    if (!dst || cap == 0) return 0;
    for (size_t i = 0; i < nkeys; i++) {
        long v = json_get_int(obj, keys[i], miss);
        if (v != miss) {
            snprintf(dst, cap, "%ld", v);
            return 1;
        }
    }
    snprintf(dst, cap, "-");
    return 0;
}

static int json_pick_scalar_string(const char *obj, const char *const *keys, size_t nkeys,
                                   char *dst, size_t cap)
{
    char tmp[512];

    if (!dst || cap == 0) return 0;
    for (size_t i = 0; i < nkeys; i++) {
        if (json_get(obj, keys[i], tmp, sizeof tmp) && tmp[0]) {
            snprintf(dst, cap, "%s", tmp);
            return 1;
        }
    }
    return json_pick_int_string(obj, keys, nkeys, dst, cap);
}

static int fetch_signal_raw_field(const char *kind, const char *const *keys, size_t nkeys,
                                  char *dst, size_t cap)
{
    char cmd[256];
    char json[4096];
    char data[3600];
    char decoded[3200];
    FILE *fp;
    size_t n;
    int rc;

    if (!dst || cap == 0) return 0;
    snprintf(dst, cap, "-");
    if (!g_sig_read || !modem_api_available()) return 0;
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest?kind=%s' 2>/dev/null",
                 kind) >= (int)sizeof cmd)
        return 0;
    fp = popen(cmd, "r");
    if (!fp) return 0;
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0) return 0;
    json[n] = 0;
    if (!strncmp(json, "null", 4)) return 0;
    if (!json_get(json, "data", data, sizeof data)) return 0;
    if (json_get(data, "decoded", decoded, sizeof decoded) &&
        json_pick_scalar_string(decoded, keys, nkeys, dst, cap))
        return 1;
    return json_pick_scalar_string(data, keys, nkeys, dst, cap);
}

static int json_scan_last_scalar_string(const char *json, const char *const *keys, size_t nkeys,
                                        char *dst, size_t cap)
{
    char needle[128];

    if (!json || !dst || cap == 0) return 0;
    for (size_t i = 0; i < nkeys; i++) {
        const char *p = json;
        char last[256] = "";
        int found = 0;

        if (snprintf(needle, sizeof needle, "\"%s\":", keys[i]) >= (int)sizeof needle)
            continue;
        while ((p = strstr(p, needle)) != NULL) {
            const char *v = p + strlen(needle);
            char tmp[256];
            size_t o = 0;

            while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
                v++;
            if (*v == '"') {
                v++;
                while (*v && o + 1 < sizeof tmp) {
                    if (*v == '\\' && v[1]) {
                        v++;
                        tmp[o++] = *v++;
                        continue;
                    }
                    if (*v == '"')
                        break;
                    tmp[o++] = *v++;
                }
            } else {
                while (*v && *v != ',' && *v != '}' && *v != ']' &&
                       *v != ' ' && *v != '\t' && *v != '\r' && *v != '\n' &&
                       o + 1 < sizeof tmp)
                    tmp[o++] = *v++;
            }
            tmp[o] = 0;
            if (tmp[0] && strcmp(tmp, "null") != 0) {
                snprintf(last, sizeof last, "%s", tmp);
                found = 1;
            }
            p += strlen(needle);
        }
        if (found) {
            snprintf(dst, cap, "%s", last);
            return 1;
        }
    }
    return 0;
}

static int fetch_signal_recent_raw_field(const char *kind, const char *const *keys, size_t nkeys,
                                         char *dst, size_t cap)
{
    char cmd[256];
    static char json[65536];
    FILE *fp;
    size_t n;
    int rc;

    if (!dst || cap == 0) return 0;
    if (!g_sig_read || !modem_api_available()) return 0;
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/recent?kind=%s&limit=96' 2>/dev/null",
                 kind) >= (int)sizeof cmd)
        return 0;
    fp = popen(cmd, "r");
    if (!fp) return 0;
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0) return 0;
    json[n] = 0;
    return json_scan_last_scalar_string(json, keys, nkeys, dst, cap);
}

static int fetch_signal_raw_vendor(const char *kind, char *dst, size_t cap)
{
    char cmd[256];
    char json[4096];
    char data[3600];
    char decoded[3200];
    FILE *fp;
    size_t n;
    int rc;

    if (!dst || cap == 0 || !g_sig_read || !modem_api_available())
        return 0;
    if (snprintf(cmd, sizeof cmd,
                 "/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest?kind=%s' 2>/dev/null",
                 kind) >= (int)sizeof cmd)
        return 0;
    fp = popen(cmd, "r");
    if (!fp)
        return 0;
    n = fread(json, 1, sizeof(json) - 1, fp);
    rc = pclose(fp);
    if (rc != 0 || n == 0)
        return 0;
    json[n] = 0;
    if (!strncmp(json, "null", 4))
        return 0;
    if (!json_get(json, "data", data, sizeof data))
        return 0;
    if (json_get(data, "decoded", decoded, sizeof decoded) &&
        signal_detect_vendor_from_text(decoded, dst, cap))
        return 1;
    return signal_detect_vendor_from_text(data, dst, cap);
}

static void signal_settings_summary(char *dst, size_t cap)
{
    if (!dst || cap == 0) return;
    snprintf(dst, cap, "读取 %s · 解析 %s",
             g_sig_read ? "已开启" : "已关闭",
             g_sig_parse ? "已开启" : "已关闭");
}
/* escape &,<,> for safe litehtml parsing of arbitrary SMS text */
static void html_esc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *s = src; *s && o + 7 < cap; s++) {
        const char *r = NULL;
        if      (*s == '&') r = "&amp;";
        else if (*s == '<') r = "&lt;";
        else if (*s == '>') r = "&gt;";
        if (r) { size_t L = strlen(r); memcpy(dst + o, r, L); o += L; }
        else dst[o++] = *s;
    }
    dst[o] = 0;
}

/* SMS bodies are text nodes too, but preserve their original line breaks. */
static void html_esc_breaks(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *s = src; *s && o + 7 < cap; s++) {
        const char *r = NULL;
        if      (*s == '&')  r = "&amp;";
        else if (*s == '<')  r = "&lt;";
        else if (*s == '>')  r = "&gt;";
        else if (*s == '\n') r = "<br>";
        else if (*s == '\r') continue;
        if (r) { size_t L = strlen(r); memcpy(dst + o, r, L); o += L; }
        else dst[o++] = *s;
    }
    dst[o] = 0;
}

/* Copy at most maxb bytes of UTF-8 without splitting a multibyte char; stop at
 * a newline. Sets *cut if the source was longer, so the caller can add ellipsis. */
static void utf8_trunc(char *dst, size_t cap, const char *src, int maxb, int *cut)
{
    int o = 0; *cut = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s) {
        if (*s == '\n' || *s == '\r') { *cut = 1; break; }
        int len = (*s >= 0xF0) ? 4 : (*s >= 0xE0) ? 3 : (*s >= 0xC0) ? 2 : 1;
        if (o + len > maxb || (size_t)(o + len) >= cap) { *cut = 1; break; }
        for (int k = 0; k < len && *s; k++) dst[o++] = (char)*s++;
    }
    dst[o] = 0;
}

static void signal_ui_compact(char *dst, size_t cap, const char *src)
{
    int cut = 0;

    if (!dst || cap == 0) return;
    if (!src || !src[0]) {
        snprintf(dst, cap, "-");
        return;
    }

    if (strstr(src, "rrc_reconfiguration_complete") || strstr(src, "RRCReconfigurationComplete"))
        snprintf(dst, cap, "RRC重配完成");
    else if (strstr(src, "rrc_reconfiguration") || strstr(src, "RRCReconfiguration"))
        snprintf(dst, cap, "RRC重配");
    else if (strstr(src, "rrc_setup_complete") || strstr(src, "RRCSetupComplete"))
        snprintf(dst, cap, "RRC建立完成");
    else if (strstr(src, "rrc_setup") || strstr(src, "RRCSetup"))
        snprintf(dst, cap, "RRC建立");
    else if (strstr(src, "ueCapabilityEnquiry") || strstr(src, "rrc_capability_enquiry"))
        snprintf(dst, cap, "UE能力查询");
    else if (strstr(src, "ueCapabilityInformation") || strstr(src, "rrc_capability_information"))
        snprintf(dst, cap, "UE能力上报");
    else if (strstr(src, "SystemInformation") || strstr(src, "rrc_system_information"))
        snprintf(dst, cap, "系统信息");
    else if (strstr(src, "MasterInformationBlock") || strstr(src, "rrc_mib"))
        snprintf(dst, cap, "MIB");
    else if (strstr(src, "securityModeCommand"))
        snprintf(dst, cap, "安全模式命令");
    else if (strstr(src, "securityModeComplete"))
        snprintf(dst, cap, "安全模式完成");
    else if (strstr(src, "registration_accept"))
        snprintf(dst, cap, "注册接受");
    else if (strstr(src, "registration_request"))
        snprintf(dst, cap, "注册请求");
    else if (strstr(src, "service_request"))
        snprintf(dst, cap, "业务请求");
    else {
        utf8_trunc(dst, cap, src, 18, &cut);
        if (cut && strlen(dst) + 3 < cap)
            strcat(dst, "...");
    }
}

static void signal_bler_compact(char *dst, size_t cap, const char *src)
{
    char *endp = NULL;
    double v;

    if (!dst || cap == 0) return;
    if (!src || !src[0]) {
        snprintf(dst, cap, "-");
        return;
    }
    v = strtod(src, &endp);
    if (endp && endp != src && *endp == '\0') {
        snprintf(dst, cap, "%.1f%%", v);
        return;
    }
    signal_ui_compact(dst, cap, src);
}

/* Second-level SMS detail dialog (number + date + full text), overlaid dimmed. */
static const char *sms_modal_html(void)
{
    static char out[DEVUI_SMS_TEXT_MAX * 6 + 1536];
    static char esc[DEVUI_SMS_TEXT_MAX * 6];
    html_esc_breaks(esc, sizeof esc, g_sms_text);
    snprintf(out, sizeof out,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='style.css'></head>"
        "<body class='mo'><div class='modal %s'>"
        "<div class='mtitle'>%s</div>"
        "<div class='smsmdate'>%s</div>"
        "<div id='smsview' class='smsmview'><div id='smsbody' class='smsmbody' style='top:-%dpx'>%s</div></div>"
        "<div class='mbtns'><a href='act:smsclose' class='mbtn2 prim mfull'>关闭</a></div>"
        "</div></body></html>",
        theme_body_class(), g_sms_num, g_sms_date, g_sms_scroll, esc);
    return out;
}

/* Build the second-level band-lock dialog (overlaid on the dimmed page). */
static const char *modal_html(void)
{
    static char out[3200];
    const char *uni, *sel, *pfx, *title, *act;
    if (g_modal == 4) {
        snprintf(out, sizeof out,
            "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='style.css'></head>"
            "<body class='mo'><div class='modal %s'>"
            "<div class='mtitle'>高级设置</div>"
            "<a href='act:sigread' class='row'><span class='lab'>读取信令<span class='st'>控制 ML1 / 原始信令读取，影响 TA / Ports / Beam</span></span>"
            "<span class='ctrl'><span class='sw %s'><span class='kn'></span></span></span></a>"
            "<a href='act:sigparse' class='row'><span class='lab'>解析信令<span class='st'>控制 LTE / NR RRC / NAS 解析，并决定第二页是否显示</span></span>"
            "<span class='ctrl'><span class='sw %s'><span class='kn'></span></span></span></a>"
            "<div class='sec'>开启解析后才显示第二页。读取关闭时，TA / Ports / SSB Index 会显示为空。</div>"
            "<div class='mbtns'><a href='act:closemodal' class='mbtn2 prim mfull'>关闭</a></div>"
            "</div></body></html>",
            theme_body_class(),
            g_sig_read ? "on" : "off",
            g_sig_parse ? "on" : "off");
        return out;
    }
    if (g_modal == 1)      { uni = g_uni_sa;  sel = g_sel_sa;  pfx = "n"; act = "bsa";  title = "5G SA 锁频"; }
    else if (g_modal == 2) { uni = g_uni_nsa; sel = g_sel_nsa; pfx = "n"; act = "bnsa"; title = "5G NSA 锁频"; }
    else                   { uni = g_uni_lte; sel = g_sel_lte; pfx = "B"; act = "blte"; title = "4G 锁频"; }
    char chips[2200];
    build_chips_cls(chips, sizeof chips, uni, sel, act, pfx, "bchip-on", "bchip");
    snprintf(out, sizeof out,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='style.css'></head>"
        "<body class='mo'><div class='modal %s'>"
        "<div class='mtitle'>%s</div><div class='mbands'>%s</div>"
        "<div class='mbtns'>"
        "<a href='act:mall' class='mbtn2'>全选/全不选</a>"
        "<a href='act:minv' class='mbtn2'>反选</a>"
        "<a href='act:mapply' class='mbtn2 prim'>应用</a></div>"
        "</div></body></html>",
        theme_body_class(), title, chips);
    return out;
}

static int fetch_lte_ta(int *ta_out)
{
    static uint32_t cached_at;
    static uint32_t enable_req_at;
    static int cached_valid;
    static int cached_ta = -1;
    char json[2048];
    char data[1800];
    char cmd[192];
    uint32_t now = millis();
    FILE *fp;
    size_t n;
    int rc;
    int fresh_ta = -1;

    if (ta_out) *ta_out = -1;
    if (!g_sig_read || !modem_api_available()) return 0;
    if (cached_ta < 0) {
        char cached_text[24];
        if (signal_cache_get("ta", cached_text, sizeof cached_text)) {
            char *endp = NULL;
            long v = strtol(cached_text, &endp, 10);

            if (endp && *endp == '\0' && v >= 0 && v <= 4096) {
                cached_ta = (int)v;
                cached_valid = 1;
            }
        }
    }
    if (cached_at && (uint32_t)(now - cached_at) < 900) {
        if (cached_valid && ta_out) *ta_out = cached_ta;
        return cached_valid;
    }

    cached_at = now;
    cached_valid = cached_ta >= 0;
    fp = popen("/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/latest?kind=lte_ml1_raw' 2>/dev/null", "r");
    if (fp) {
        n = fread(json, 1, sizeof(json) - 1, fp);
        rc = pclose(fp);
        if (rc == 0 && n > 0) {
            json[n] = 0;
            if (!strncmp(json, "null", 4)) {
                if ((uint32_t)(now - enable_req_at) >= 5000 &&
                    snprintf(cmd, sizeof cmd,
                            "/usr/bin/wget -qO- --post-data='' 'http://127.0.0.1:9460/modem/control?ml1_raw=1&active=1' >/dev/null 2>&1")
                        < (int)sizeof cmd) {
                    (void)system(cmd);
                    enable_req_at = now;
                }
            } else if (json_get(json, "data", data, sizeof data)) {
                fresh_ta = (int)json_get_int(data, "ta", -1);
            }
        }
    }
    if (fresh_ta < 0) {
        fp = popen("/usr/bin/wget -T 1 -qO- 'http://127.0.0.1:9460/modem/recent?kind=lte_ml1_raw&log_origin=direct&limit=64' 2>/dev/null", "r");
        if (fp) {
            n = fread(json, 1, sizeof(json) - 1, fp);
            rc = pclose(fp);
            if (rc == 0 && n > 0) {
                const char *p;
                json[n] = 0;
                p = json;
                while ((p = strstr(p, "\"ta\":")) != NULL) {
                    char *endp = NULL;
                    long v = strtol(p + 5, &endp, 10);

                    if (endp && endp != p + 5 && v >= 0 && v <= 4096)
                        fresh_ta = (int)v;
                    p = endp && endp > p ? endp : p + 5;
                }
            }
        }
    }
    if (fresh_ta >= 0) {
        char ta_text[24];

        cached_ta = fresh_ta;
        cached_valid = 1;
        snprintf(ta_text, sizeof ta_text, "%d", fresh_ta);
        signal_cache_put("ta", ta_text);
    }
    if (cached_ta < 0) return 0;

    cached_valid = 1;
    if (ta_out) *ta_out = cached_ta;
    return 1;
}

static double nr_ta_unit_m(const devui_data_t *d, const struct signal_bundle *sig)
{
    long arfcn = d ? d->nr_channel : 0;

    if (sig && signal_value_present(sig->cell_key)) {
        const char *p = strrchr(sig->cell_key, ':');
        if (p && p[1]) {
            char *endp = NULL;
            long v = strtol(p + 1, &endp, 10);
            if (endp && endp != p + 1 && v > 0) arfcn = v;
        }
    }
    if (arfcn >= 499200 && arfcn <= 537999) return 39.06; /* n41, 30 kHz */
    if (arfcn >= 620000 && arfcn <= 653333) return 39.06; /* n78, 30 kHz */
    if (arfcn >= 693334 && arfcn <= 733333) return 39.06; /* n79, 30 kHz */
    if (d && (strstr(d->nr_band, "n41") || strstr(d->nr_band, "n77") ||
              strstr(d->nr_band, "n78") || strstr(d->nr_band, "n79")))
        return 39.06;
    return 78.12;
}

static double ta_unit_m_for_signal(const devui_data_t *d, const struct signal_bundle *sig, int ta)
{
    if (sig && signal_value_present(sig->nr_ta)) {
        char *endp = NULL;
        long nr_ta = strtol(sig->nr_ta, &endp, 10);
        if (endp && endp != sig->nr_ta && nr_ta == ta)
            return nr_ta_unit_m(d, sig);
    }
    return 78.12; /* LTE / 15 kHz */
}

static void fmt_ta_distance(char *dst, size_t cap, int ta, double unit_m)
{
    double meters;

    if (!dst || cap == 0) return;
    if (ta < 0) { snprintf(dst, cap, "-"); return; }
    if (unit_m <= 0.0) unit_m = 78.12;
    meters = (double)ta * unit_m;
    if (meters >= 1000.0) snprintf(dst, cap, "约 %.1f km", meters / 1000.0);
    else                  snprintf(dst, cap, "约 %.0f m", meters);
}

static int signal_neighbor_count(const struct signal_neighbor_metric *list, int n)
{
    int count = 0;

    if (!list || n <= 0) return 0;
    for (int i = 0; i < n; i++)
        if (list[i].valid)
            count++;
    return count;
}

static const char *signal_rsrp_class(const char *rsrp)
{
    double v;

    if (!signal_value_present(rsrp)) return "q-off";
    v = atof(rsrp);
    if (v >= -85.0) return "q-good";
    if (v >= -105.0) return "q-mid";
    return "q-bad";
}

static const char *signal_rsrq_class(const char *rsrq)
{
    double v;

    if (!signal_value_present(rsrq)) return "q-off";
    v = atof(rsrq);
    if (v >= -10.0) return "q-good";
    if (v >= -15.0) return "q-mid";
    return "q-bad";
}

static int signal_neighbor_row(char *buf, int o, int cap,
                               const struct signal_neighbor_metric *n)
{
    char pci[32], band[48], rsrp[32], rsrq[32];

    if (!buf || cap <= 0 || o >= cap || !n || !n->valid)
        return o;
    html_esc(pci, sizeof pci, signal_carrier_val(n->pci));
    html_esc(band, sizeof band, signal_carrier_val(n->band));
    html_esc(rsrp, sizeof rsrp, signal_carrier_val(n->rsrp));
    html_esc(rsrq, sizeof rsrq, signal_carrier_val(n->rsrq));
    return o + snprintf(buf + o, cap - o,
                        "<div class='nrow'><span>%s</span><span>%s</span>"
                        "<span class='%s'>%s</span><span class='%s'>%s</span></div>",
                        pci, band, signal_rsrp_class(n->rsrp), rsrp,
                        signal_rsrq_class(n->rsrq), rsrq);
}

static int signal_neighbor_section(char *buf, int o, int cap, const char *title,
                                   const struct signal_neighbor_metric *list, int n)
{
    int count = signal_neighbor_count(list, n);

    if (!count || !buf || cap <= 0 || o >= cap)
        return o;
    o += snprintf(buf + o, cap - o,
                  "<div class='nsec'>%s <span>%d</span></div>"
                  "<div class='nhead'><span>PCI</span><span>频段</span><span>RSRP</span><span>RSRQ</span></div>",
                  title, count);
    for (int i = 0; i < n; i++)
        o = signal_neighbor_row(buf, o, cap, &list[i]);
    return o;
}

/* Set when neighbors come from /state.neighbor (public datad) instead of the
 * private /modem/signal-metrics feed. */
static const char *g_neighbor_src;
static char g_neighbor_wait[96];

static const char *neighbor_status_text(const char *status)
{
    static const struct { const char *k, *v; } map[] = {
        { "disabled", "未开启" }, { "starting", "启动中" }, { "stopping", "停止中" },
        { "collecting", "采集中" }, { "ready", "就绪" }, { "empty", "暂无邻区" },
        { "stale", "数据过期" }, { "blocked", "诊断口被占用" },
        { "dependency_missing", "采集器缺失" }, { "error", "出错" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(status, map[i].k)) return map[i].v;
    return status[0] ? status : "-";
}

static void signal_bundle_neighbors_from_state(struct signal_bundle *b, const devui_data_t *d)
{
    b->lte_neighbor_n = b->nr_neighbor_n = 0;
    for (int i = 0; i < d->neighbor_n; i++) {
        const devui_neighbor_t *c = &d->neighbor[i];
        int is_nr = !strcmp(c->rat, "NR");
        struct signal_neighbor_metric *m;

        if (is_nr ? b->nr_neighbor_n >= SIGNAL_MAX_NEIGHBORS
                  : b->lte_neighbor_n >= SIGNAL_MAX_NEIGHBORS)
            continue;
        m = is_nr ? &b->nr_neighbors[b->nr_neighbor_n++] : &b->lte_neighbors[b->lte_neighbor_n++];
        signal_neighbor_metric_reset(m);
        snprintf(m->pci, sizeof m->pci, "%d", c->pci);
        if (c->has_arfcn) snprintf(m->arfcn, sizeof m->arfcn, "%ld", c->arfcn);
        if (c->has_band) snprintf(m->band, sizeof m->band, "%s%d", is_nr ? "n" : "B", c->band);
        else if (is_nr) signal_nr_band_from_nrarfcn(m->arfcn, m->band, sizeof m->band);
        else signal_lte_band_from_earfcn(m->arfcn, m->band, sizeof m->band);
        if (c->has_rsrp) snprintf(m->rsrp, sizeof m->rsrp, "%d", c->rsrp);
        if (c->has_rsrq) snprintf(m->rsrq, sizeof m->rsrq, "%d", c->rsrq);
        m->valid = 1;
    }
    g_neighbor_src = d->neighbor_enabled == 1 ? "诊断采集" : "厂商扫描";
    if (d->neighbor_enabled == 1)
        snprintf(g_neighbor_wait, sizeof g_neighbor_wait, "邻区采集：%s。",
                 neighbor_status_text(d->neighbor_status));
    else
        snprintf(g_neighbor_wait, sizeof g_neighbor_wait,
                 "暂无 4G 邻区；开启“读取信令”可采集 5G 邻区。");
}

static const char *signal_neighbor_empty_text(void)
{
    if (g_neighbor_src) return g_neighbor_wait;
    return "等待 LTE / NR ML1 邻区测量。";
}

static const char *signal_neighbors_html(const struct signal_bundle *sig)
{
    static char buf[7000];
    int nr_count = sig ? signal_neighbor_count(sig->nr_neighbors, sig->nr_neighbor_n) : 0;
    int lte_count = sig ? signal_neighbor_count(sig->lte_neighbors, sig->lte_neighbor_n) : 0;
    int total = nr_count + lte_count;
    const char *source = !g_sig_parse ? "" : g_neighbor_src ? g_neighbor_src : "信令解析";
    int o = 0;

    if (!g_sig_parse) {
        buf[0] = 0;
        return buf;
    }

    if (!g_neighbor_open) {
        if (source[0])
            snprintf(buf, sizeof buf,
                     "<a href='act:neighbors' class='card neigh-toggle'>"
                     "<span class='title'>邻小区</span><span class='r muted'>%s · %d 个 · 点击展开</span>"
                     "</a>",
                     source, total);
        else
            snprintf(buf, sizeof buf,
                     "<a href='act:neighbors' class='card neigh-toggle'>"
                     "<span class='title'>邻小区</span><span class='r muted'>%d 个 · 点击展开</span>"
                     "</a>",
                     total);
        return buf;
    }

    if (source[0])
        o += snprintf(buf + o, sizeof buf - o,
                      "<div class='card neigh-card'>"
                      "<div class='title'>邻小区 <span class='muted nsrc'>%s</span>"
                      "<a href='act:neighbors' class='rev r'>收起</a></div>",
                      source);
    else
        o += snprintf(buf + o, sizeof buf - o,
                      "<div class='card neigh-card'>"
                      "<div class='title'>邻小区 <a href='act:neighbors' class='rev r'>收起</a></div>");
    if (total <= 0) {
        o += snprintf(buf + o, sizeof buf - o,
                      "<div class='sec'>%s</div>",
                      signal_neighbor_empty_text());
    } else {
        o = signal_neighbor_section(buf, o, sizeof buf, "NR", sig->nr_neighbors, sig->nr_neighbor_n);
        o = signal_neighbor_section(buf, o, sizeof buf, "LTE", sig->lte_neighbors, sig->lte_neighbor_n);
    }
    snprintf(buf + o, sizeof buf - o, "</div>");
    return buf;
}

/* Fill a kv table from the current device state. Buffers are static. */
static int build_kv(struct kv *t, const char *path)
{
    int is_cellinfo = path_is_signal_detail(path);
    int is_signalhome = path_is_signal_home(path);
    int is_charts = path && strstr(path, "charts.html") != NULL;
    g_phase++;

    static char s_time[8], s_bat[8], s_rsrp[12], s_rsrq[12], s_sinr[12], s_bw[12], s_date[40];
    static char s_siggauges[400];
    static char s_cellid[20], s_pci[12], s_clients[8], s_up[24], s_rxs[20], s_txs[20];
    static char s_rxb[16], s_txb[16], s_cpu[8], s_mem[8], w_rsrp[6], w_rsrq[6], w_sinr[6], w_bw[6];
    static char s_oper[48], s_ssid[64], s_key[64], s_page[8], s_np[8], s_model[64], s_fw[80];
    static char s_qci[8], s_ambr[24], s_sbar[640], s_dots[320], s_ta[16], s_tadist[24];
    static char s_nrports[64], s_nrbeam[128], s_nrvendor[64], s_sigadvstate[64], s_sigrefresh[16], s_signalcards[10000], s_neighborcards[7000];
    static char s_last_lte_nas[128] = "-";
        static char s_last_nr_nas[128] = "-";
        static char s_last_nrports[64] = "-";
        static char s_last_nrbeam[128] = "-";
        static char s_last_nrvendor[64] = "-";
        static char s_nrrows[4096], s_lterows[4096], s_carriers[9000], s_nr_block[6000], s_lte_block[5000], s_sigcards[11000], s_gen[8];

    static devui_data_t d;
    static struct signal_bundle sig;
    int ta = -1;
    if (!data_refresh(&d)) memset(&d, 0, sizeof d);
    if (dev_is_tf() && d.modem_n > 0) {
        /* traffic covers the 5G modem only; the WAN rate is all modems together */
        d.rx_speed = d.tx_speed = 0;
        for (int k = 0; k < d.modem_n; k++) {
            d.rx_speed += d.modem[k].rx_speed;
            d.tx_speed += d.modem[k].tx_speed;
        }
    }
    g_charging = d.charger_connect;
    snprintf(s_page, sizeof s_page, "%d", g_cur + 1);
    snprintf(s_np, sizeof s_np, "%d", g_npages);

    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    if (now != h_last) { hist_push(&d); h_last = now; }   /* sample once per second */
    snprintf(s_time, sizeof s_time, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    {
        static const char *const wd[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
        snprintf(s_date, sizeof s_date, "%d月%d日 %s", tmv.tm_mon + 1, tmv.tm_mday, wd[tmv.tm_wday % 7]);
    }
    {
        int nr = d.nr_rsrp != 0;
        g_dial.nr.rsrp = (float)d.nr_rsrp;
        g_dial.nr.rsrp_ok = d.nr_rsrp != 0;
        g_dial.nr.sinr_ok = signal_value_present(d.nr_snr);
        g_dial.nr.sinr = g_dial.nr.sinr_ok ? (float)atof(d.nr_snr) : 0.f;
        g_dial.lte.rsrp = (float)d.lte_rsrp;
        g_dial.lte.rsrp_ok = d.lte_rsrp != 0;
        g_dial.lte.sinr_ok = signal_value_present(d.lte_snr);
        g_dial.lte.sinr = g_dial.lte.sinr_ok ? (float)atof(d.lte_snr) : 0.f;
        g_dial.nr_serving = nr;
        g_dial.bat = clampi(d.bat_percent, 0, 100);
        g_dial.charging = d.charging == 1;   /* charge_status: 1 charging, 2 discharging, 3 idle, 4 full */
        g_dial.sig = signal_level(nr ? d.nr_rsrp : d.lte_rsrp);
        g_dial.wifi_on = g_w24 == 1 || g_w5 == 1 || g_w6 == 1;
        g_dial.nfc_on = d.nfc_switch != 0;
    }
    snprintf(s_sigrefresh, sizeof s_sigrefresh, "%02d:%02d:%02d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    snprintf(s_bat, sizeof s_bat, "%d", d.bat_percent);
    snprintf(s_rsrp, sizeof s_rsrp, "%d", d.nr_rsrp);
    snprintf(s_rsrq, sizeof s_rsrq, "%d", d.nr_rsrq);
    snprintf(s_sinr, sizeof s_sinr, "%s", d.nr_snr[0] ? d.nr_snr : "-");
    snprintf(s_bw, sizeof s_bw, "%s", d.nr_bw[0] ? d.nr_bw : "-");
    if (g_show_cellid) snprintf(s_cellid, sizeof s_cellid, "%ld", d.nr_cell_id);
    else               strcpy(s_cellid, "********");
    snprintf(s_pci, sizeof s_pci, "%d", d.nr_pci);
    snprintf(s_clients, sizeof s_clients, "%d", d.clients_total);
    fmt_uptime(s_up, sizeof s_up, d.uptime);
    fmt_speed_u(s_rxs, sizeof s_rxs, d.rx_speed, g_speed_bits);
    fmt_speed_u(s_txs, sizeof s_txs, d.tx_speed, g_speed_bits);
    fmt_bytes(s_rxb, sizeof s_rxb, d.rx_bytes);
    fmt_bytes(s_txb, sizeof s_txb, d.tx_bytes);
    snprintf(s_cpu, sizeof s_cpu, "%ld", d.cpu_temp);
    snprintf(s_mem, sizeof s_mem, "%ld", d.mem_used_pct);
    snprintf(s_oper, sizeof s_oper, "%s", d.operator_name[0] ? d.operator_name : "--");
    snprintf(s_ssid, sizeof s_ssid, "%s", d.wifi_ssid[0] ? d.wifi_ssid : "-");
    if (!d.wifi_key[0])
        snprintf(d.wifi_key, sizeof d.wifi_key, "%s", wifi_key_for(d.wifi_ssid));
    if (d.dps >= 0 && (int32_t)(millis() - g_dps_hold_until) >= 0) g_dps = d.dps;
    g_dial.dps_on = g_dps == 1;
    if (!d.wifi_key[0]) strcpy(s_key, "-");
    else if (g_show_key) snprintf(s_key, sizeof s_key, "%s", d.wifi_key);
    else { int kn = (int)strlen(d.wifi_key); if (kn > 16) kn = 16; memset(s_key, '*', kn); s_key[kn] = 0; }
    snprintf(s_model, sizeof s_model, "%s", d.model[0] ? d.model : "-");
    snprintf(s_fw, sizeof s_fw, "%s", d.fw[0] ? d.fw : "-");
    if (is_cellinfo && !g_sig_read) {
        snprintf(s_ta, sizeof s_ta, "已关闭");
        snprintf(s_tadist, sizeof s_tadist, "已关闭");
    } else if (is_cellinfo && fetch_lte_ta(&ta)) {
        snprintf(s_ta, sizeof s_ta, "%d", ta);
        fmt_ta_distance(s_tadist, sizeof s_tadist, ta, 78.12);
    } else if (is_cellinfo) {
        snprintf(s_ta, sizeof s_ta, "-");
        snprintf(s_tadist, sizeof s_tadist, "-");
    } else {
        snprintf(s_ta, sizeof s_ta, "-");
        snprintf(s_tadist, sizeof s_tadist, "-");
    }
    signal_settings_summary(s_sigadvstate, sizeof s_sigadvstate);
    snprintf(w_rsrp, sizeof w_rsrp, "%d", clampi((d.nr_rsrp + 120) * 2, 3, 100));
    snprintf(w_rsrq, sizeof w_rsrq, "%d", clampi((d.nr_rsrq + 20) * 100 / 17, 3, 100));
    snprintf(w_sinr, sizeof w_sinr, "%d", clampi((int)((atof(d.nr_snr) + 10) * 100 / 40), 3, 100));
    snprintf(w_bw, sizeof w_bw, "%d", clampi(atoi(d.nr_bw), 3, 100));

    /* qci / ambr (from modem qos): unified Mbps, e.g. 3000/200 Mbps */
    if (d.qci > 0) snprintf(s_qci, sizeof s_qci, "%d", d.qci);
    else           snprintf(s_qci, sizeof s_qci, "-");
    if (d.ambr_dl > 0.0 || d.ambr_ul > 0.0) {
        char dl[12], ul[12];
        if (d.ambr_dl > 0.0) snprintf(dl, sizeof dl, "%.0f", d.ambr_dl);
        else                 snprintf(dl, sizeof dl, "-");
        if (d.ambr_ul > 0.0) snprintf(ul, sizeof ul, "%.0f", d.ambr_ul);
        else                 snprintf(ul, sizeof ul, "-");
        snprintf(s_ambr, sizeof s_ambr, "%s/%s Mbps", dl, ul);
    } else {
        snprintf(s_ambr, sizeof s_ambr, "-");
    }

    /* ---- per-carrier display + generation badge ----
     * nrca group: idx,pci,?,band,arfcn,bw,?,rsrp,rsrq,sinr,rssi
     * lteca is seen in both legacy 5-field and newer 11-field variants.
     * Some firmwares expose LTE SCC signal details separately via ltecasig. */
    int is_endc = strstr(d.net_type, "ENDC") || strstr(d.net_type, "EN-DC");
    int is_nsa  = strstr(d.net_type, "NSA") != NULL;
    int is_sa   = strstr(d.net_type, "SA") && !is_nsa;
    int is_lte  = !is_nsa && !is_sa && (strstr(d.net_type, "LTE") || strstr(d.net_type, "4G"));
    int show_nr  = is_sa || is_nsa || is_endc;
    int show_lte = is_nsa || is_endc || is_lte;
    {
        /* gauges per leg on the air interface: SA -> NR, LTE -> LTE, NSA / EN-DC -> both */
        int g_nr = show_nr, g_lte = show_lte || !show_nr;
        const char *cls = g_nr && g_lte ? "gauge sm" : "gauge";
        int go = snprintf(s_siggauges, sizeof s_siggauges, "<div class='card gauges'>");
        if (g_nr)
            go += snprintf(s_siggauges + go, sizeof s_siggauges - go,
                           "<div id='gauge-nr-rsrp' class='%s'></div><div id='gauge-nr-sinr' class='%s'></div>", cls, cls);
        if (g_lte)
            go += snprintf(s_siggauges + go, sizeof s_siggauges - go,
                           "<div id='gauge-lte-rsrp' class='%s'></div><div id='gauge-lte-sinr' class='%s'></div>", cls, cls);
        snprintf(s_siggauges + go, sizeof s_siggauges - go, "</div>");
    }
    int show_nr_nas = is_sa;
    int nr_cc = 0, nr_bw = 0, lte_cc = 0, lte_bw = 0;
    int nr_show_cc = 0, nr_show_bw = 0, lte_show_cc = 0, lte_show_bw = 0, no = 0, lo = 0;
    char rp[12] = "-", pc[12] = "-", ac[16] = "-";
    const char *nr_band = d.nr_band[0] ? d.nr_band : d.band;

    signal_bundle_reset(&sig);
    if (signal_live_enabled() && (is_cellinfo || is_signalhome)) {
        if (!signal_async_snapshot(&sig)) {
            if (fetch_signal_bundle(&sig))
                signal_async_publish(&sig, 1);
        }
    }
    g_neighbor_src = NULL;
    if (signal_live_enabled() && is_signalhome && !modem_api_available())
        signal_bundle_neighbors_from_state(&sig, &d);
    snprintf(s_neighborcards, sizeof s_neighborcards, "%s",
             is_signalhome ? signal_neighbors_html(&sig) : "");

    if (is_cellinfo) {
        char c_lte_rrc[96], c_lte_nas[96], c_nr_rrc[96], c_nr_nas[96];
        char c_lteta[96], c_nrta[96];
        char c_nrports[96], c_nrbeam[96], c_nrservssb[96], c_nrbler[96], c_ltebler[96];
        char c_nrmcs[96], c_nrmod[96], c_nrmimo[96], c_nrlayers[96], c_nrdlgrant[96], c_nrdlrb[96], c_nrulgrant[96], c_nrulrb[96], c_nrpusch[128], c_nrpucch[128];
        char c_ltemcs[96], c_ltemod[96], c_ltegrant[96], c_lterb[96], c_ltepusch[128], c_ltepucch[128];
        char e_lte_rrc[256], e_lte_nas[256], e_nr_rrc[256], e_nr_nas[256];
        char e_lteta[128], e_nrta[128];
        char e_nrports[160], e_nrbeam[256], e_nrservssb[128], e_nrbler[128], e_ltebler[128], e_nrvendor[128];
        char e_nrmcs[128], e_nrmod[128], e_nrmimo[128], e_nrlayers[128], e_nrdlgrant[128], e_nrdlrb[128], e_nrulgrant[128], e_nrulrb[128], e_nrpusch[192], e_nrpucch[192];
        char e_ltemcs[128], e_ltemod[128], e_ltegrant[128], e_lterb[128], e_ltepusch[192], e_ltepucch[192];
        char s_lteta[64], s_nrta[64], s_nrservssb[64], s_nrbler[64], s_ltebler[64];
        char s_nrmcs[64], s_nrmod[64], s_nrmimo[64], s_nrlayers[64], s_nrdlgrant[64], s_nrdlrb[64], s_nrulgrant[64], s_nrulrb[64], s_nrpusch[96], s_nrpucch[96];
        char s_ltemcs[64], s_ltemod[64], s_ltegrant[64], s_lterb[64], s_ltepusch[96], s_ltepucch[96];
        static char s_last_signal_cell_key[64] = "";
        static char s_last_nrservssb[64] = "-";
        static char s_last_nrbler[64] = "-";
        static char s_last_ltebler[64] = "-";
        int so = 0;

        if (signal_value_present(sig.cell_key) &&
            strcmp(sig.cell_key, s_last_signal_cell_key) != 0) {
            snprintf(s_last_signal_cell_key, sizeof s_last_signal_cell_key, "%s", sig.cell_key);
            snprintf(s_last_nrports, sizeof s_last_nrports, "-");
            snprintf(s_last_nrbeam, sizeof s_last_nrbeam, "-");
            snprintf(s_last_nrservssb, sizeof s_last_nrservssb, "-");
            snprintf(s_last_nrbler, sizeof s_last_nrbler, "-");
            snprintf(s_last_ltebler, sizeof s_last_ltebler, "-");
            snprintf(s_last_nrvendor, sizeof s_last_nrvendor, "-");
        }
        signal_value_keep_last(sig.lte_nas, sizeof sig.lte_nas, s_last_lte_nas, sizeof s_last_lte_nas);
        signal_value_keep_last(sig.nr_nas, sizeof sig.nr_nas, s_last_nr_nas, sizeof s_last_nr_nas);
        if (!signal_live_enabled()) {
            snprintf(s_lteta, sizeof s_lteta, "已关闭");
            snprintf(s_nrta, sizeof s_nrta, "已关闭");
            snprintf(s_nrports, sizeof s_nrports, "已关闭");
            snprintf(s_nrbeam, sizeof s_nrbeam, "已关闭");
            snprintf(s_nrservssb, sizeof s_nrservssb, "已关闭");
            snprintf(s_nrbler, sizeof s_nrbler, "已关闭");
            snprintf(s_ltebler, sizeof s_ltebler, "已关闭");
            snprintf(s_nrmcs, sizeof s_nrmcs, "已关闭");
            snprintf(s_nrmod, sizeof s_nrmod, "已关闭");
            snprintf(s_nrmimo, sizeof s_nrmimo, "已关闭");
            snprintf(s_nrlayers, sizeof s_nrlayers, "已关闭");
            snprintf(s_nrdlgrant, sizeof s_nrdlgrant, "已关闭");
            snprintf(s_nrdlrb, sizeof s_nrdlrb, "已关闭");
            snprintf(s_nrulgrant, sizeof s_nrulgrant, "已关闭");
            snprintf(s_nrulrb, sizeof s_nrulrb, "已关闭");
            snprintf(s_nrpusch, sizeof s_nrpusch, "已关闭");
            snprintf(s_nrpucch, sizeof s_nrpucch, "已关闭");
            snprintf(s_ltemcs, sizeof s_ltemcs, "已关闭");
            snprintf(s_ltemod, sizeof s_ltemod, "已关闭");
            snprintf(s_ltegrant, sizeof s_ltegrant, "已关闭");
            snprintf(s_lterb, sizeof s_lterb, "已关闭");
            snprintf(s_ltepusch, sizeof s_ltepusch, "已关闭");
            snprintf(s_ltepucch, sizeof s_ltepucch, "已关闭");
            snprintf(s_nrvendor, sizeof s_nrvendor, "-");
        } else {
            snprintf(s_lteta, sizeof s_lteta, "%s", sig.lte_ta);
            snprintf(s_nrta, sizeof s_nrta, "%s", sig.nr_ta);
            snprintf(s_nrports, sizeof s_nrports, "%s", sig.nr_ports);
            snprintf(s_nrbeam, sizeof s_nrbeam, "%s", sig.nr_beam);
            snprintf(s_nrservssb, sizeof s_nrservssb, "-");
            snprintf(s_nrbler, sizeof s_nrbler, "-");
            snprintf(s_ltebler, sizeof s_ltebler, "-");
            snprintf(s_nrmcs, sizeof s_nrmcs, "%s", sig.nr_mcs);
            snprintf(s_nrmod, sizeof s_nrmod, "%s", sig.nr_modulation);
            snprintf(s_nrmimo, sizeof s_nrmimo, "%s", sig.nr_mimo);
            snprintf(s_nrlayers, sizeof s_nrlayers, "%s", sig.nr_layers);
            snprintf(s_nrdlgrant, sizeof s_nrdlgrant, "%s", sig.nr_dl_grants);
            snprintf(s_nrdlrb, sizeof s_nrdlrb, "%s", sig.nr_dl_rb);
            snprintf(s_nrulgrant, sizeof s_nrulgrant, "%s", sig.nr_ul_grants);
            snprintf(s_nrulrb, sizeof s_nrulrb, "%s", sig.nr_ul_rb);
            snprintf(s_nrpusch, sizeof s_nrpusch, "%s", sig.nr_pusch);
            snprintf(s_nrpucch, sizeof s_nrpucch, "%s", sig.nr_pucch);
            snprintf(s_ltemcs, sizeof s_ltemcs, "%s", sig.lte_mcs);
            snprintf(s_ltemod, sizeof s_ltemod, "%s", sig.lte_modulation);
            snprintf(s_ltegrant, sizeof s_ltegrant, "%s", sig.lte_grants);
            snprintf(s_lterb, sizeof s_lterb, "%s", sig.lte_rb);
            snprintf(s_ltepusch, sizeof s_ltepusch, "%s", sig.lte_pusch);
            snprintf(s_ltepucch, sizeof s_ltepucch, "%s", sig.lte_pucch);
            snprintf(s_nrservssb, sizeof s_nrservssb, "%s", sig.nr_serving_ssb);
            snprintf(s_nrbler, sizeof s_nrbler, "%s", sig.nr_bler);
            snprintf(s_ltebler, sizeof s_ltebler, "%s", sig.lte_bler);
            snprintf(s_nrvendor, sizeof s_nrvendor, "%s", sig.nr_vendor);
            if (!signal_value_present(sig.nr_ta))
                snprintf(s_nrta, sizeof s_nrta, "等待ML1");
            if (!signal_value_present(sig.lte_ta))
                snprintf(s_lteta, sizeof s_lteta, "等待ML1");
            signal_value_keep_last(s_nrports, sizeof s_nrports, s_last_nrports, sizeof s_last_nrports);
            signal_value_keep_last(s_nrbeam, sizeof s_nrbeam, s_last_nrbeam, sizeof s_last_nrbeam);
            signal_value_keep_last(s_nrservssb, sizeof s_nrservssb,
                                   s_last_nrservssb, sizeof s_last_nrservssb);
            signal_value_keep_last(s_nrbler, sizeof s_nrbler,
                                   s_last_nrbler, sizeof s_last_nrbler);
            signal_value_keep_last(s_ltebler, sizeof s_ltebler,
                                   s_last_ltebler, sizeof s_last_ltebler);
            signal_value_keep_last(s_nrvendor, sizeof s_nrvendor,
                                   s_last_nrvendor, sizeof s_last_nrvendor);
            if (!strcmp(s_nrports, "-"))
                snprintf(s_nrports, sizeof s_nrports, "等待RRC CSI-RS");
            if (!strcmp(s_nrservssb, "-"))
                snprintf(s_nrservssb, sizeof s_nrservssb, "等待ML1");
            if (!strcmp(s_nrbeam, "-"))
                snprintf(s_nrbeam, sizeof s_nrbeam, "等待RRC配置");
            if (!strcmp(s_nrbler, "-"))
                snprintf(s_nrbler, sizeof s_nrbler, "等待PDSCH");
            if (!strcmp(s_ltebler, "-"))
                snprintf(s_ltebler, sizeof s_ltebler, "等待PDSCH");
            if (!strcmp(s_nrmcs, "-"))
                snprintf(s_nrmcs, sizeof s_nrmcs, "等待PDSCH");
            if (!strcmp(s_nrmod, "-"))
                snprintf(s_nrmod, sizeof s_nrmod, "等待PDSCH");
            if (!strcmp(s_nrmimo, "-"))
                snprintf(s_nrmimo, sizeof s_nrmimo, "等待PDSCH");
            if (!strcmp(s_nrlayers, "-"))
                snprintf(s_nrlayers, sizeof s_nrlayers, "等待PDSCH");
            if (!signal_value_present(sig.nr_dl_grants))
                snprintf(s_nrdlgrant, sizeof s_nrdlgrant, "等待PDSCH");
            if (!signal_value_present(sig.nr_dl_rb))
                snprintf(s_nrdlrb, sizeof s_nrdlrb, "等待PDSCH");
            if (!signal_value_present(sig.nr_ul_grants))
                snprintf(s_nrulgrant, sizeof s_nrulgrant, "等待UL调度");
            if (!signal_value_present(sig.nr_ul_rb))
                snprintf(s_nrulrb, sizeof s_nrulrb, "等待UL调度");
            if (!strcmp(s_nrpusch, "-"))
                snprintf(s_nrpusch, sizeof s_nrpusch, "等待UL功控");
            if (!strcmp(s_nrpucch, "-"))
                snprintf(s_nrpucch, sizeof s_nrpucch, "等待UL功控");
            if (!strcmp(s_ltemcs, "-"))
                snprintf(s_ltemcs, sizeof s_ltemcs, "等待PDSCH");
            if (!strcmp(s_ltemod, "-"))
                snprintf(s_ltemod, sizeof s_ltemod, "等待PDSCH");
            if (!signal_value_present(sig.lte_grants))
                snprintf(s_ltegrant, sizeof s_ltegrant, "等待PDSCH");
            if (!signal_value_present(sig.lte_rb))
                snprintf(s_lterb, sizeof s_lterb, "等待PDSCH");
            if (!strcmp(s_ltepusch, "-"))
                snprintf(s_ltepusch, sizeof s_ltepusch, "等待PUSCH");
            if (!strcmp(s_ltepucch, "-"))
                snprintf(s_ltepucch, sizeof s_ltepucch, "等待PUCCH");
            if (signal_value_present(sig.ta)) {
                char *endp = NULL;
                long ta_val = strtol(sig.ta, &endp, 10);
                if (endp && endp != sig.ta && ta_val >= 0 && ta_val <= 65535) {
                    snprintf(s_ta, sizeof s_ta, "%ld", ta_val);
                    fmt_ta_distance(s_tadist, sizeof s_tadist, (int)ta_val,
                                    ta_unit_m_for_signal(&d, &sig, (int)ta_val));
                } else {
                    snprintf(s_ta, sizeof s_ta, "%.15s", sig.ta);
                    snprintf(s_tadist, sizeof s_tadist, "-");
                }
            }
        }
        signal_ui_compact(c_lte_rrc, sizeof c_lte_rrc, sig.lte_rrc);
        signal_ui_compact(c_lte_nas, sizeof c_lte_nas, sig.lte_nas);
        signal_ui_compact(c_nr_rrc, sizeof c_nr_rrc, sig.nr_rrc);
        signal_ui_compact(c_nr_nas, sizeof c_nr_nas, sig.nr_nas);
        signal_ui_compact(c_lteta, sizeof c_lteta, s_lteta);
        signal_ui_compact(c_nrta, sizeof c_nrta, s_nrta);
        signal_ui_compact(c_nrports, sizeof c_nrports, s_nrports);
        signal_ui_compact(c_nrbeam, sizeof c_nrbeam, s_nrbeam);
        signal_ui_compact(c_nrservssb, sizeof c_nrservssb, s_nrservssb);
        signal_bler_compact(c_nrbler, sizeof c_nrbler, s_nrbler);
        signal_bler_compact(c_ltebler, sizeof c_ltebler, s_ltebler);
        signal_ui_compact(c_nrmcs, sizeof c_nrmcs, s_nrmcs);
        signal_ui_compact(c_nrmod, sizeof c_nrmod, s_nrmod);
        signal_ui_compact(c_nrmimo, sizeof c_nrmimo, s_nrmimo);
        signal_ui_compact(c_nrlayers, sizeof c_nrlayers, s_nrlayers);
        signal_ui_compact(c_nrdlgrant, sizeof c_nrdlgrant, s_nrdlgrant);
        signal_ui_compact(c_nrdlrb, sizeof c_nrdlrb, s_nrdlrb);
        signal_ui_compact(c_nrulgrant, sizeof c_nrulgrant, s_nrulgrant);
        signal_ui_compact(c_nrulrb, sizeof c_nrulrb, s_nrulrb);
        signal_ui_compact(c_nrpusch, sizeof c_nrpusch, s_nrpusch);
        signal_ui_compact(c_nrpucch, sizeof c_nrpucch, s_nrpucch);
        signal_ui_compact(c_ltemcs, sizeof c_ltemcs, s_ltemcs);
        signal_ui_compact(c_ltemod, sizeof c_ltemod, s_ltemod);
        signal_ui_compact(c_ltegrant, sizeof c_ltegrant, s_ltegrant);
        signal_ui_compact(c_lterb, sizeof c_lterb, s_lterb);
        signal_ui_compact(c_ltepusch, sizeof c_ltepusch, s_ltepusch);
        signal_ui_compact(c_ltepucch, sizeof c_ltepucch, s_ltepucch);
        html_esc(e_lte_rrc, sizeof e_lte_rrc, c_lte_rrc);
        html_esc(e_lte_nas, sizeof e_lte_nas, c_lte_nas);
        html_esc(e_nr_rrc, sizeof e_nr_rrc, c_nr_rrc);
        html_esc(e_nr_nas, sizeof e_nr_nas, c_nr_nas);
        html_esc(e_lteta, sizeof e_lteta, c_lteta);
        html_esc(e_nrta, sizeof e_nrta, c_nrta);
        html_esc(e_nrports, sizeof e_nrports, c_nrports);
        html_esc(e_nrbeam, sizeof e_nrbeam, c_nrbeam);
        html_esc(e_nrservssb, sizeof e_nrservssb, c_nrservssb);
        html_esc(e_nrbler, sizeof e_nrbler, c_nrbler);
        html_esc(e_ltebler, sizeof e_ltebler, c_ltebler);
        html_esc(e_nrmcs, sizeof e_nrmcs, c_nrmcs);
        html_esc(e_nrmod, sizeof e_nrmod, c_nrmod);
        html_esc(e_nrmimo, sizeof e_nrmimo, c_nrmimo);
        html_esc(e_nrlayers, sizeof e_nrlayers, c_nrlayers);
        html_esc(e_nrdlgrant, sizeof e_nrdlgrant, c_nrdlgrant);
        html_esc(e_nrdlrb, sizeof e_nrdlrb, c_nrdlrb);
        html_esc(e_nrulgrant, sizeof e_nrulgrant, c_nrulgrant);
        html_esc(e_nrulrb, sizeof e_nrulrb, c_nrulrb);
        html_esc(e_nrpusch, sizeof e_nrpusch, c_nrpusch);
        html_esc(e_nrpucch, sizeof e_nrpucch, c_nrpucch);
        html_esc(e_ltemcs, sizeof e_ltemcs, c_ltemcs);
        html_esc(e_ltemod, sizeof e_ltemod, c_ltemod);
        html_esc(e_ltegrant, sizeof e_ltegrant, c_ltegrant);
        html_esc(e_lterb, sizeof e_lterb, c_lterb);
        html_esc(e_ltepusch, sizeof e_ltepusch, c_ltepusch);
        html_esc(e_ltepucch, sizeof e_ltepucch, c_ltepucch);
        html_esc(e_nrvendor, sizeof e_nrvendor, s_nrvendor);
        s_signalcards[0] = 0;
        so += snprintf(s_signalcards + so, sizeof s_signalcards - so,
                       "<div class='card'><div class='title'>基站品牌识别</div><table class='sigtab'>"
                       "<tr><td class='kv-l'>刷新时间</td><td class='val'>%s</td></tr>"
                       "<tr><td class='kv-l'>识别结果</td><td class='val sm'>%s</td></tr>"
                       "<tr><td class='kv-l'>TA</td><td class='val'>%s</td></tr>"
                       "<tr><td class='kv-l'>估算距离</td><td class='val'>%s</td></tr>"
                       "</table></div>",
                       s_sigrefresh, e_nrvendor, s_ta, s_tadist);
        if (show_nr) {
            if (show_nr_nas) {
                so += snprintf(s_signalcards + so, sizeof s_signalcards - so,
                               "<div class='card'><div class='title'>NR 信令 <span class='r muted'>5G</span></div><table class='sigtab'>"
                               "<tr><td class='kv-l'>最近 RRC</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>最近 NAS</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>TA</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>MCS</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>调制</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>MIMO</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>layers</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>DL/UL Grant</td><td class='val sm'>%s/%s</td></tr>"
                               "<tr><td class='kv-l'>DL/UL RB</td><td class='val sm'>%s/%s</td></tr>"
                               "<tr><td class='kv-l'>BLER</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>PUSCH</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>PUCCH</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>Ports</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>SSB Index</td><td class='val sm'>%s</td></tr>"
                               "</table></div>",
                               e_nr_rrc, e_nr_nas, e_nrta, e_nrmcs, e_nrmod, e_nrmimo, e_nrlayers,
                               e_nrdlgrant, e_nrulgrant, e_nrdlrb, e_nrulrb, e_nrbler,
                               e_nrpusch, e_nrpucch, e_nrports, e_nrservssb);
            } else {
                so += snprintf(s_signalcards + so, sizeof s_signalcards - so,
                               "<div class='card'><div class='title'>NR 信令 <span class='r muted'>5G</span></div><table class='sigtab'>"
                               "<tr><td class='kv-l'>最近 RRC</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>TA</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>MCS</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>调制</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>MIMO</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>layers</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>DL/UL Grant</td><td class='val sm'>%s/%s</td></tr>"
                               "<tr><td class='kv-l'>DL/UL RB</td><td class='val sm'>%s/%s</td></tr>"
                               "<tr><td class='kv-l'>BLER</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>PUSCH</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>PUCCH</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>Ports</td><td class='val sm'>%s</td></tr>"
                               "<tr><td class='kv-l'>SSB Index</td><td class='val sm'>%s</td></tr>"
                               "</table></div>",
                               e_nr_rrc, e_nrta, e_nrmcs, e_nrmod, e_nrmimo, e_nrlayers,
                               e_nrdlgrant, e_nrulgrant, e_nrdlrb, e_nrulrb, e_nrbler, e_nrpusch,
                               e_nrpucch, e_nrports, e_nrservssb);
            }
        }
        if (show_lte) {
            so += snprintf(s_signalcards + so, sizeof s_signalcards - so,
                           "<div class='card'><div class='title'>LTE 信令 <span class='r muted'>4G</span></div><table class='sigtab'>"
                           "<tr><td class='kv-l'>最近 RRC</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>最近 NAS</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>TA</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>MCS</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>调制</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>Grant</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>RB</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>BLER</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>PUSCH</td><td class='val sm'>%s</td></tr>"
                           "<tr><td class='kv-l'>PUCCH</td><td class='val sm'>%s</td></tr>"
                           "</table></div>",
                           e_lte_rrc, e_lte_nas, e_lteta, e_ltemcs, e_ltemod,
                           e_ltegrant, e_lterb, e_ltebler, e_ltepusch, e_ltepucch);
        }
        if (so == 0) {
            snprintf(s_signalcards, sizeof s_signalcards,
                     "<div class='card'><div class='title'>信令信息</div><div class='sec'>当前网络没有可展示的 LTE / NR 信令卡片。</div></div>");
            so = (int)strlen(s_signalcards);
        }
    } else {
        snprintf(s_nrports, sizeof s_nrports, "-");
        snprintf(s_nrbeam, sizeof s_nrbeam, "-");
        snprintf(s_nrvendor, sizeof s_nrvendor, "-");
        s_signalcards[0] = 0;
    }

    /* NR carriers: PCell from main fields + nrca SCells */
    s_nrrows[0] = 0;
    if (show_nr && nr_band[0] && d.nr_bw[0]) {
        struct signal_carrier_metric *metric;
        snprintf(rp, sizeof rp, "%d", d.nr_rsrp); snprintf(pc, sizeof pc, "%d", d.nr_pci);
        snprintf(ac, sizeof ac, "%ld", d.nr_channel);
        metric = signal_pick_carrier_metric(sig.nr_carriers, sig.nr_carrier_n, pc, ac, 1);
        no = car_row(s_nrrows, no, sizeof s_nrrows, nr_band, d.nr_bw, ac, pc, rp, d.nr_snr,
                     hsr_freq_hint(d.mcc, ac), metric);
        nr_show_cc = 1; nr_show_bw = atoi(d.nr_bw);
        if (!carrier_is_inactive(rp)) { nr_cc = 1; nr_bw = atoi(d.nr_bw); }
    }
    if (show_nr) {
        char nrca[256]; strncpy(nrca, d.nrca, sizeof nrca - 1); nrca[sizeof nrca - 1] = 0;
        for (char *grp = strtok(nrca, ";"); grp; grp = strtok(NULL, ";")) {
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[12]; int nf = ca_split(g, f, 12);
            if (nf > 5 && atoi(f[5]) > 0) {
                double rpv = (nf > 7 && f[7] && f[7][0]) ? atof(f[7]) : 0.0;
                char bn[12]; snprintf(bn, sizeof bn, "n%s", f[3]);
                const char *nr_ac = nf > 4 ? f[4] : "-";
                const char *nr_pci = nf > 1 ? f[1] : "-";
                struct signal_carrier_metric *metric =
                    signal_pick_carrier_metric(sig.nr_carriers, sig.nr_carrier_n,
                                               nr_pci, nr_ac, 0);
                no = car_row(s_nrrows, no, sizeof s_nrrows, bn, f[5], nr_ac,
                             nr_pci, nf > 7 ? f[7] : "-", nf > 9 ? f[9] : "-",
                             hsr_freq_hint(d.mcc, nr_ac), metric);
                nr_show_cc++;
                nr_show_bw += atoi(f[5]);
                if (rpv > -140.0) { nr_cc++; nr_bw += atoi(f[5]); }
            }
        }
    }
    /* LTE carriers: support both legacy 5-field and newer 11-field groups. */
    s_lterows[0] = 0;
    if (show_lte) {
        char lteca[256]; strncpy(lteca, d.lteca, sizeof lteca - 1); lteca[sizeof lteca - 1] = 0;
        char ltecasig[256]; strncpy(ltecasig, d.ltecasig, sizeof ltecasig - 1); ltecasig[sizeof ltecasig - 1] = 0;
        char *lgrps[8], *sgrps[8];
        int lgn = ca_groups(lteca, lgrps, 8);
        int sgn = ca_groups(ltecasig, sgrps, 8);
        int sig_off = (sgn > 0 && lgn == sgn + 1);   /* ltecasig often contains SCC only */
        for (int gi = 0; gi < lgn; gi++) {
            char *grp = lgrps[gi];
            const char *siggrp = NULL;
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[12]; int nf = ca_split(g, f, 12);
            const char *f_band = NULL, *f_bw = NULL, *f_arfcn = NULL, *f_pci = NULL, *f_rp = NULL, *f_sn = NULL;
            const char *sig_rp = NULL, *sig_sn = NULL;
            char sig_rp_buf[16] = "", sig_sn_buf[16] = "";
            if (nf >= 11) {
                f_band = f[3]; f_arfcn = f[4]; f_bw = f[5]; f_pci = f[1]; f_rp = f[7]; f_sn = f[9];
            } else if (nf >= 5) {
                f_band = f[1]; f_arfcn = f[3]; f_bw = f[4]; f_pci = f[0];
            }
            if (sgn == lgn && gi < sgn) siggrp = sgrps[gi];
            else if (sig_off && gi > 0 && gi - 1 < sgn) siggrp = sgrps[gi - 1];
            else if (gi < sgn) siggrp = sgrps[gi];
            if (siggrp && siggrp[0]) {
                char sg[96]; strncpy(sg, siggrp, sizeof sg - 1); sg[sizeof sg - 1] = 0;
                if (ca_sig_pick(sg, &sig_rp, &sig_sn)) {
                    if (sig_rp && sig_rp[0]) snprintf(sig_rp_buf, sizeof sig_rp_buf, "%s", sig_rp);
                    if (sig_sn && sig_sn[0]) snprintf(sig_sn_buf, sizeof sig_sn_buf, "%s", sig_sn);
                }
            }
            if ((!f_rp || !f_rp[0]) && sig_rp_buf[0]) f_rp = sig_rp_buf;
            if ((!f_sn || !f_sn[0]) && sig_sn_buf[0]) f_sn = sig_sn_buf;
            if (f_bw && atoi(f_bw) > 0) {
                char bn[12];
                if (f_band && (f_band[0] == 'B' || f_band[0] == 'b')) snprintf(bn, sizeof bn, "%s", f_band);
                else snprintf(bn, sizeof bn, "B%s", (f_band && f_band[0]) ? f_band : "-");
                char lr[12] = "-", ls[12] = "-";
                if (f_rp && f_rp[0]) snprintf(lr, sizeof lr, "%s", f_rp);
                if (f_sn && f_sn[0]) snprintf(ls, sizeof ls, "%s", f_sn);
                if (lte_cc == 0) {   /* serving-cell signal from main fields */
                    if (d.lte_rsrp) snprintf(lr, sizeof lr, "%d", d.lte_rsrp);
                    if (d.lte_snr[0]) snprintf(ls, sizeof ls, "%s", d.lte_snr);
                }
                struct signal_carrier_metric *metric =
                    signal_pick_carrier_metric(sig.lte_carriers, sig.lte_carrier_n,
                                               f_pci ? f_pci : "-", f_arfcn ? f_arfcn : "-",
                                               gi == 0);
                lo = car_row(s_lterows, lo, sizeof s_lterows, bn, f_bw, f_arfcn ? f_arfcn : "-",
                             f_pci ? f_pci : "-", lr, ls, hsr_freq_hint(d.mcc, f_arfcn),
                             metric);
                lte_show_cc++;
                lte_show_bw += atoi(f_bw);
                if (!carrier_is_inactive(lr)) { lte_cc++; lte_bw += atoi(f_bw); }
            }
        }
    }
    (void)no; (void)lo; (void)pc; (void)ac;

    /* Display summary includes inactive/configured carriers; generation badge
     * still uses active carrier stats above. */
    int total_bw, total_show_bw; const char *cmode;
    if (is_endc)     { total_bw = nr_bw + lte_bw; total_show_bw = nr_show_bw + lte_show_bw; cmode = "EN-DC"; }
    else if (is_nsa) { total_bw = nr_bw + lte_bw; total_show_bw = nr_show_bw + lte_show_bw; cmode = "5G NSA"; }
    else if (is_sa)  { total_bw = nr_bw;          total_show_bw = nr_show_bw;               cmode = "5G SA"; }
    else if (is_lte) { total_bw = lte_bw;         total_show_bw = lte_show_bw;              cmode = "4G LTE"; }
    else             { total_bw = 0;              total_show_bw = 0;                         cmode = d.net_type[0] ? d.net_type : "No Service"; }
    { char cnt[48];
      if (lte_show_cc && nr_show_cc) snprintf(cnt, sizeof cnt, "%d LTE + %d NR 载波", lte_show_cc, nr_show_cc);
      else if (nr_show_cc)           snprintf(cnt, sizeof cnt, "%d NR 载波", nr_show_cc);
      else if (lte_show_cc)          snprintf(cnt, sizeof cnt, "%d LTE 载波", lte_show_cc);
      else                 snprintf(cnt, sizeof cnt, "无载波");
      snprintf(s_carriers, sizeof s_carriers, "<div class='sec'>%s · %s · %d MHz</div>%s%s",
               cmode, cnt, total_show_bw, s_nrrows, s_lterows); }
    const char *hsr_card_cls = d.hsr ? " hsrmode" : "";
    const char *hsr_mode_note = d.hsr ? "<div class='hsrmode-note'>高铁模式</div>" : "";
    if ((is_endc || is_nsa) && s_nrrows[0] && s_lterows[0]) {
        snprintf(s_nr_block, sizeof s_nr_block,
                 "<div class='card%s'>"
                 "<div class='title'>%s <span class='sub'>%s</span>"
                 "<span class='qa'><b>QCI %s</b><br>AMBR %s</span></div>"
                 "%s"
                 "<div class='sec'>%s · %d LTE + %d NR 载波 · %d MHz</div>"
                 "<div class='ctitle'>NR</div>%s"
                 "</div>",
                 hsr_card_cls, s_oper, d.net_type, s_qci, s_ambr, hsr_mode_note,
                 cmode, lte_show_cc, nr_show_cc, total_show_bw, s_nrrows);
        snprintf(s_lte_block, sizeof s_lte_block,
                 "<div class='card'><div class='ctitle'>LTE</div>"
                 "<div class='sec'>%d LTE 载波 · %d MHz</div>%s</div>",
                 lte_show_cc, lte_show_bw, s_lterows);
        snprintf(s_sigcards, sizeof s_sigcards, "%s%s", s_nr_block, s_lte_block);
    } else {
        snprintf(s_sigcards, sizeof s_sigcards,
                 "<div class='card%s'>"
                 "<div class='title'>%s <span class='sub'>%s</span>"
                 "<span class='qa'><b>QCI %s</b><br>AMBR %s</span></div>"
                 "%s"
                 "%s"
                 "</div>",
                 hsr_card_cls, s_oper, d.net_type, s_qci, s_ambr, hsr_mode_note, s_carriers);
    }

    /* generation badge: 5GA / 5G+ / 5G / 4G / LTE / 3G */
    int gen_nr_cc = nr_show_cc;
    int gen_nr_bw = nr_show_bw;
    int op = 0;   /* 1 mobile, 2 unicom, 3 telecom, 4 broadnet, 5 other-mainland */
    if (d.mcc == 460) {
        int m = d.mnc;
        if (m == 0 || m == 2 || m == 4 || m == 7 || m == 8) op = 1;
        else if (m == 1 || m == 6 || m == 9) op = 2;
        else if (m == 3 || m == 5 || m == 11) op = 3;
        else if (m == 15) op = 4;
        else op = 5;
    }
    if (d.mcc == 460) {
        const char *cn = op == 1 ? "中国移动" : op == 2 ? "中国联通" :
                         op == 3 ? "中国电信" : op == 4 ? "中国广电" : NULL;
        if (cn) snprintf(s_oper, sizeof s_oper, "%s", cn);
    }
    const char *gen = "--";
    if (is_sa) {
        if ((op == 1 || op == 4) && gen_nr_cc >= 3) gen = "5GA";
        else if ((op == 2 || op == 3) && gen_nr_bw >= 200) gen = "5GA";
        else if (gen_nr_bw > 100) gen = "5G+";
        else gen = "5G";
    } else if (is_nsa || is_endc) gen = "5G";
    else if (is_lte) gen = (d.mcc == 460) ? "4G" : "LTE";
    else if (d.net_type[0]) gen = "3G";
    snprintf(s_gen, sizeof s_gen, "%s", gen);
    const char *genc = "g3";
    if      (!strcmp(gen, "5GA")) genc = "g5ga";
    else if (!strcmp(gen, "5G+")) genc = "g5p";
    else if (!strcmp(gen, "5G"))  genc = "g5";
    else if (!strcmp(gen, "4G"))  genc = "g4";
    else if (!strcmp(gen, "LTE")) genc = "glte";
    snprintf(g_stat_time, sizeof g_stat_time, "%s", s_time);
    g_stat_sig = signal_level(d.nr_rsrp ? d.nr_rsrp : d.lte_rsrp);
    snprintf(g_stat_gen, sizeof g_stat_gen, "%s", gen);

    /* ---- shared status bar: native-drawn content over a simple 26px slot ---- */
    {
        g_stat_bat = clampi(d.bat_percent, 0, 100);
        g_stat_lowbat = d.bat_percent <= 20;
        g_sms_unread_now = d.sms_unread > 0;
        g_stat_rx = d.rx_speed;
        g_stat_tx = d.tx_speed;
        snprintf(s_sbar, sizeof s_sbar, "<div class='sbar'></div>");
    }

    /* ---- bottom page dots (dark on a really light wallpaper there) ---- */
    {
        const char *dk = html_view_wallpaper_on() && html_view_wallpaper_light(100, 450, 120, 30) ? " dk" : "";
        int o = snprintf(s_dots, sizeof s_dots, "<div class='dots'>");
        for (int p = 0; p < g_npages; p++)
            o += snprintf(s_dots + o, sizeof s_dots - o, "<span class='dot%s'></span>", p == g_cur ? " on" : dk);
        snprintf(s_dots + o, sizeof s_dots - o, "</div>");
    }

    /* charts are drawn natively after render (see draw_charts), not via tokens. */

    /* ---- band lock: universe grows to the largest set seen; selection mirrors
     * the live lock unless the user is editing in the modal ---- */
    static char s_netseg[640], s_cursa[300], s_curnsa[300], s_curlte[300], s_toast[120];
    if (band_count(d.sa_bands)  >= band_count(g_uni_sa))  snprintf(g_uni_sa,  sizeof g_uni_sa,  "%s", d.sa_bands);
    if (band_count(d.nsa_bands) >= band_count(g_uni_nsa)) snprintf(g_uni_nsa, sizeof g_uni_nsa, "%s", d.nsa_bands);
    if (band_count(d.lte_bands) >= band_count(g_uni_lte)) snprintf(g_uni_lte, sizeof g_uni_lte, "%s", d.lte_bands);
    if (!g_modal) {
        if (d.sa_bands[0])  snprintf(g_sel_sa,  sizeof g_sel_sa,  "%s", d.sa_bands);
        if (d.nsa_bands[0]) snprintf(g_sel_nsa, sizeof g_sel_nsa, "%s", d.nsa_bands);
        if (d.lte_bands[0]) snprintf(g_sel_lte, sizeof g_sel_lte, "%s", d.lte_bands);
    }
    band_summary(s_cursa,  sizeof s_cursa,  g_uni_sa,  g_sel_sa,  "n");
    band_summary(s_curnsa, sizeof s_curnsa, g_uni_nsa, g_sel_nsa, "n");
    band_summary(s_curlte, sizeof s_curlte, g_uni_lte, g_sel_lte, "B");
    {   /* segmented control: highlight current mode (pending overrides until it applies) */
        if (g_net_pending[0] && !strcmp(g_net_pending, d.net_select)) g_net_pending[0] = 0;
        const char *cur = g_net_pending[0] ? g_net_pending : d.net_select;
        int active = 0;
        for (int k = 0; k < 4; k++) if (!strcmp(netmode_value(k), cur)) active = k;
        int hl = g_segdrag == 1 ? -1 : active;   /* during drag the native box is the highlight */
        int o = snprintf(s_netseg, sizeof s_netseg, "<div id='netseg' class='seg seg4'>");
        for (int k = 0; k < 4; k++)
            o += snprintf(s_netseg + o, sizeof s_netseg - o, "<a href='act:net:%s' class='segc%s'>%s</a>",
                          netmode_value(k), k == hl ? " seg-on" : "", g_netmodes[k].lab);
        snprintf(s_netseg + o, sizeof s_netseg - o, "</div>");
    }
    s_toast[0] = 0;
    if (g_toast[0]) snprintf(s_toast, sizeof s_toast, "<div class='toast'>%s</div>", g_toast);

    /* ---- system extras: usage, temps, version, imei, brightness, auto-off ---- */
    static char s_cusage[8], s_ctemp[8], s_btemp[8], s_swver[80], s_imei[24], s_spu[8], s_bright[8];
    static char s_memdet[24], s_upshort[16], s_autooff[420], s_refreshseg[480];
    { int bmax = backlight_max(); if (bmax <= 0) bmax = 255;
      snprintf(s_bright, sizeof s_bright, "%d", clampi(backlight_get() * 100 / bmax, 0, 100)); }
    static char s_glass[8], s_gblur[8], s_tintseg[900];
    snprintf(s_glass, sizeof s_glass, "%d", g_glass);
    snprintf(s_gblur, sizeof s_gblur, "%d", g_glass_blur);
    {   /* glass colour chips, each with a swatch of the pane colour */
        int o = snprintf(s_tintseg, sizeof s_tintseg, "<div class='aoffs'>");
        for (int k = 0; k < NTINTS; k++)
            o += snprintf(s_tintseg + o, sizeof s_tintseg - o,
                          "<a href='act:glasstint:%d' class='%s'><span class='swatch' style='background:#%06x'></span>%s</a>",
                          k, k == g_glass_tint ? "aoff-on" : "aoff", tint_rgb(k), g_tints[k].name);
        snprintf(s_tintseg + o, sizeof s_tintseg - o, "</div>");
    }
    { long mt = d.mem_total, ma = d.mem_avail;
      snprintf(s_memdet, sizeof s_memdet, "%ld/%ld MB", mt ? (mt - ma) / 1048576 : 0, mt / 1048576); }
    static char s_chgv[8], s_chgi[10], s_batv[8], s_bati[10], s_pwr[10], s_batpwr[10], s_batpwrlbl[8];
    snprintf(s_chgv, sizeof s_chgv, "%.2f", d.chg_uv / 1e6);
    snprintf(s_chgi, sizeof s_chgi, "%ld", d.chg_ua / 1000);
    snprintf(s_batv, sizeof s_batv, "%.2f", d.bat_uv / 1e6);
    /* signed: + while charging, - while discharging (matches PWRLBL) */
    snprintf(s_bati, sizeof s_bati, "%s%ld", d.charger_connect ? "+" : "-", labs(d.bat_ua) / 1000);
    { double pw = d.charger_connect ? (d.chg_uv / 1e6) * (d.chg_ua / 1e6)
                                    : (d.bat_uv / 1e6) * (labs(d.bat_ua) / 1e6);
      snprintf(s_pwr, sizeof s_pwr, "%.1f", pw); }
    {
        double bat_pw = (d.bat_uv / 1e6) * (labs(d.bat_ua) / 1e6);
        snprintf(s_batpwr, sizeof s_batpwr, "%.1f", bat_pw);
        snprintf(s_batpwrlbl, sizeof s_batpwrlbl, "%s", d.bat_ua >= 0 ? "充电" : "放电");
    }
    fmt_uptime_s(s_upshort, sizeof s_upshort, d.uptime);
    {   /* auto-off segmented control (#autoseg), same UI as net mode */
        int active = 0;
        for (int k = 0; k < 6; k++) if (g_autooffs[k].ms == g_autooff_ms) active = k;
        int hl = g_segdrag == 2 ? -1 : active;
        int o = snprintf(s_autooff, sizeof s_autooff, "<div id='autoseg' class='seg seg6'>");
        for (int k = 0; k < 6; k++)
            o += snprintf(s_autooff + o, sizeof s_autooff - o,
                "<a href='act:autooff:%d' class='segc%s'>%s</a>",
                g_autooffs[k].ms, k == hl ? " seg-on" : "", g_autooffs[k].lab);
        snprintf(s_autooff + o, sizeof s_autooff - o, "</div>");
    }
    {   /* refresh segmented control (#refreshseg), same UI as auto-off */
        int active = 0;
        for (int k = 0; k < 5; k++) if (g_refresh_rates[k].ms == g_refresh_ms) active = k;
        int hl = g_segdrag == 3 ? -1 : active;
        int o = snprintf(s_refreshseg, sizeof s_refreshseg, "<div id='refreshseg' class='seg seg5'>");
        for (int k = 0; k < 5; k++)
            o += snprintf(s_refreshseg + o, sizeof s_refreshseg - o,
                "<a href='act:refreshms:%d' class='segc%s'>%s</a>",
                g_refresh_rates[k].ms, k == hl ? " seg-on" : "", g_refresh_rates[k].lab);
        snprintf(s_refreshseg + o, sizeof s_refreshseg - o, "</div>");
    }
    snprintf(s_cusage, sizeof s_cusage, "%ld", d.cpu_usage < 0 ? 0 : d.cpu_usage);
    snprintf(s_ctemp, sizeof s_ctemp, "%ld", d.cpu_temp);
    snprintf(s_btemp, sizeof s_btemp, "%d", d.bat_temp);
    snprintf(s_swver, sizeof s_swver, "%s", d.sw_version[0] ? d.sw_version : "-");
    if (g_show_imei && d.imei[0]) snprintf(s_imei, sizeof s_imei, "%s", d.imei);
    else { int n = d.imei[0] ? (int)strlen(d.imei) : 15; if (n > 23) n = 23; memset(s_imei, '*', n); s_imei[n] = 0; }
    snprintf(s_spu, sizeof s_spu, "%s", g_speed_bits ? "Mbps" : "MB/s");

    /* ---- connected device list (page 2): all of them, filtered to currently
     * WiFi-associated MACs (leases linger after a device leaves) ---- */
    static char s_clist[2600];
    {
        int o = 0, shown = 0;
        for (int i = 0; i < d.client_n; i++) {
            if (!mac_assoc(d.client[i].mac)) continue;
            o += snprintf(s_clist + o, sizeof s_clist - o,
                "<div class='cli'><span class='cn'>%s</span><span class='cip'>%s</span></div>",
                d.client[i].name, d.client[i].ip);
            shown++;
        }
        if (shown == 0)
            snprintf(s_clist, sizeof s_clist, "<div class='cli muted'>暂无已连接设备</div>");
        /* keep the header count consistent with the filtered list */
        if (g_assoc_macs[0]) snprintf(s_clients, sizeof s_clients, "%d", shown);
    }

    /* ---- sms list (read-only page): collapsed cards, newest first. Each card is
     * a tap target (act:sms:ID) that opens the full message in a dialog. ---- */
    static char s_smslist[131072];
    {
        int o = 0;
        for (int si = 0; si < d.sms_n; si++) {
            char prev[80]; int cut;
            utf8_trunc(prev, sizeof prev, d.sms[si].text, 40, &cut);
            char esc[512];
            html_esc(esc, sizeof esc, prev);
            if (o >= (int)sizeof(s_smslist)) break;
            int wr = snprintf(s_smslist + o, sizeof(s_smslist) - o,
                "<a href='act:sms:%ld' class='sms%s'>"
                "<div class='smsh'><span class='smsn'>%s</span><span class='smsd'>%s</span></div>"
                "<div class='smsp'>%s%s</div></a>",
                d.sms[si].id, d.sms[si].unread ? " un" : "", d.sms[si].num, d.sms[si].date,
                esc, cut ? "…" : "");
            if (wr < 0) wr = 0;
            if (wr >= (int)(sizeof(s_smslist) - o)) {
                o = (int)sizeof(s_smslist) - 1;
                s_smslist[o] = 0;
                break;
            }
            o += wr;
        }
        if (d.sms_n == 0)
            snprintf(s_smslist, sizeof s_smslist, "<div class='sms muted'>暂无短信</div>");
    }

    /* ---- dhcp summary (page 2) ---- */
    static char s_pool[64], s_lease[24];
    snprintf(s_pool, sizeof s_pool, "%s · 共 %s",
             d.dhcp_start[0] ? d.dhcp_start : "-", d.dhcp_limit[0] ? d.dhcp_limit : "-");
    { long lt = atol(d.dhcp_leasetime);
      if (lt >= 3600)     snprintf(s_lease, sizeof s_lease, "%ld 小时", lt / 3600);
      else if (lt >= 60)  snprintf(s_lease, sizeof s_lease, "%ld 分钟", lt / 60);
      else if (lt > 0)    snprintf(s_lease, sizeof s_lease, "%ld 秒", lt);
      else                strcpy(s_lease, "-"); }

    int adb_on = !strcmp(d.usb_mode, "debug");
    int usb_net_on = (g_usb_net == 1) || usb_pid_is("9057") || usb_pid_is("90b1") || usb_pid_is("90B1");
    int usb_reverse = (g_typec_source == 1);
    uint32_t now_ms = millis();
    if (g_adb_pending >= 0) {
        if (g_adb_pending == adb_on) g_adb_pending = -1;
        else                         adb_on = g_adb_pending;
    }
    if (g_usb_net_pending >= 0) {
        if (g_usb_net_pending == usb_net_on || now_ms > g_usb_net_until) g_usb_net_pending = -1;
        else                                                            usb_net_on = g_usb_net_pending;
    }
    if (g_typec_pending >= 0) {
        if (g_typec_pending == usb_reverse || now_ms > g_typec_until) g_typec_pending = -1;
        else                                                         usb_reverse = g_typec_pending;
    }
    const char *adb_state = g_adb_pending >= 0 ? "切换中" : (adb_on ? "已开启" : "已关闭");
    const char *usb_pwr_state = g_typec_pending >= 0 ? "切换中" : (usb_reverse ? "反向供电" : "给U60充电");
    const char *usb_net_state = g_usb_net_pending >= 0 ? "切换中" : (usb_net_on ? "共享中" : "仅充电");

    /* ---- TopFlow: modems, aggregation, cooling (empty on other models) ---- */
    static char s_modemcards[3200], s_aggpaths[1400], s_aggstate[64], s_aggsum[40];
    static char s_fanseg[420], s_liqseg[420], s_fanstate[40], s_liqstate[40], s_temps[640], s_ccextra[720];
    s_modemcards[0] = s_aggpaths[0] = s_aggsum[0] = s_temps[0] = s_ccextra[0] = 0;
    s_fanseg[0] = s_liqseg[0] = 0;
    snprintf(s_aggstate, sizeof s_aggstate, "—");
    snprintf(s_fanstate, sizeof s_fanstate, "—");
    snprintf(s_liqstate, sizeof s_liqstate, "—");
    if (dev_is_tf()) {
        uint32_t tnow = millis();
        if (d.agg_present && (int32_t)(tnow - g_agg_hold_until) >= 0) g_agg_on = d.agg_enabled;
        if (d.fan_mode[0] && (int32_t)(tnow - g_fan_hold_until) >= 0)
            snprintf(g_fan_mode, sizeof g_fan_mode, "%s", d.fan_mode);
        if (d.liquid_mode[0] && (int32_t)(tnow - g_liq_hold_until) >= 0)
            snprintf(g_liq_mode, sizeof g_liq_mode, "%s", d.liquid_mode);
        g_dial.agg_on = g_agg_on == 1;
        g_dial.fan_on = !strcmp(g_fan_mode, "always_on");
        g_dial.liq_on = !strcmp(g_liq_mode, "low") || !strcmp(g_liq_mode, "high");

        if (d.modem_n > 0) {
            int o = snprintf(s_modemcards, sizeof s_modemcards,
                             "<div class='card mdm'><div class='title'>多基带<span class='sub'>%d 路</span></div>",
                             d.modem_n);
            for (int k = 0; k < d.modem_n && o < (int)sizeof s_modemcards; k++) {
                const devui_modem_t *m = &d.modem[k];
                int nr = m->nr_rsrp != 0;
                char rsrp[12], sinr[12], band[24], rx[20], tx[20], oper[40], temp[12];
                const char *st, *stc;
                snprintf(rsrp, sizeof rsrp, "%d", nr ? m->nr_rsrp : m->lte_rsrp);
                if (!(nr ? m->nr_rsrp : m->lte_rsrp)) snprintf(rsrp, sizeof rsrp, "-");
                snprintf(sinr, sizeof sinr, "%s", signal_value_present(nr ? m->nr_snr : m->lte_snr)
                                                  ? (nr ? m->nr_snr : m->lte_snr) : "-");
                if (nr && !strcmp(m->id, "5G") && d.nr_band[0])
                    snprintf(band, sizeof band, "%s", d.nr_band);
                else if (!strncmp(m->band, "LTE BAND ", 9))
                    snprintf(band, sizeof band, "B%s", m->band + 9);
                else
                    snprintf(band, sizeof band, "%s", m->band[0] ? m->band : "-");
                if (strstr(m->wwan, "connected") && !strstr(m->wwan, "dis")) { st = "在线"; stc = "ok"; }
                else if (!m->sim_state[0] || strstr(m->sim_state, "absent") || strstr(m->sim_state, "no_sim"))
                                                                              { st = "无卡"; stc = "muted"; }
                else                                                          { st = "未连接"; stc = "q-mid"; }
                fmt_speed_u(rx, sizeof rx, m->rx_speed, g_speed_bits);
                fmt_speed_u(tx, sizeof tx, m->tx_speed, g_speed_bits);
                html_esc(oper, sizeof oper, m->oper[0] ? m->oper : "--");
                if (m->has_temp) snprintf(temp, sizeof temp, "%d°", m->temp);
                else             snprintf(temp, sizeof temp, "--");
                o += snprintf(s_modemcards + o, sizeof s_modemcards - o,
                    "<div class='mrow'><div class='mh'><span class='chip %s'>%s</span>"
                    "<span class='mo'>%s</span><span class='mst %s'>%s</span></div>"
                    "<div class='mm'><span class='ml'>%s</span><b class='%s'>%s</b>"
                    "<span class='ml ml2'>SINR</span><b>%s</b><span class='mb'>%s · %s</span></div>"
                    "<div class='mr'><span class='acc'>↓</span>%s <span class='acc2'>↑</span>%s</div></div>",
                    nr ? "g5" : "g4", m->id, oper, stc, st,
                    nr ? "NR" : "RSRP", signal_rsrp_class(rsrp), rsrp, sinr, band, temp, rx, tx);
            }
            if (o < (int)sizeof s_modemcards)
                snprintf(s_modemcards + o, sizeof s_modemcards - o, "</div>");
        }

        if (d.agg_present) {
            int o = 0;
            if (g_agg_on == 0)
                snprintf(s_aggstate, sizeof s_aggstate, "已关闭 · 多链路负载均衡");
            else if (d.agg_online)
                snprintf(s_aggstate, sizeof s_aggstate, "在线 · %d/%d 条链路", d.agg_paths_online, d.agg_paths);
            else
                snprintf(s_aggstate, sizeof s_aggstate, "连接中 · %d/%d 条链路", d.agg_paths_online, d.agg_paths);
            if ((int32_t)(tnow - g_agg_armed_until) < 0)
                snprintf(s_aggstate, sizeof s_aggstate, "再点一次关闭聚合");
            if (g_agg_on == 0) snprintf(s_aggsum, sizeof s_aggsum, "已关闭");
            else               snprintf(s_aggsum, sizeof s_aggsum, "%d/%d 在线", d.agg_paths_online, d.agg_paths);
            for (int k = 0; g_agg_on != 0 && k < d.agg_path_n && o < (int)sizeof s_aggpaths; k++) {
                const devui_agg_path_t *a = &d.agg_path[k];
                char lab[24], lat[16], loss[16];
                html_esc(lab, sizeof lab, a->label);
                if (a->latency_ms >= 0) snprintf(lat, sizeof lat, "%.0f ms", a->latency_ms);
                else                    snprintf(lat, sizeof lat, "-- ms");
                if (a->loss_pct >= 0)   snprintf(loss, sizeof loss, "%.1f%%", a->loss_pct);
                else                    snprintf(loss, sizeof loss, "--");
                o += snprintf(s_aggpaths + o, sizeof s_aggpaths - o,
                    "<div class='aggp'><span class='ad %s'></span><span class='an'>%s</span>"
                    "<span class='av'>%s<span class='muted'> · 丢包 </span>%s</span></div>",
                    a->online ? "on" : "off", lab, lat, loss);
            }
        }

        if (d.cool_present) {
            static const struct { const char *v, *lab; } fans[3] = {
                { "automatic", "原厂" }, { "custom", "曲线" }, { "always_on", "常开" } };
            static const struct { const char *v, *lab; } liqs[3] = {
                { "automatic", "自动" }, { "low", "低档" }, { "high", "高档" } };
            int o = snprintf(s_fanseg, sizeof s_fanseg, "<div class='seg seg3'>");
            for (int k = 0; k < 3; k++)
                o += snprintf(s_fanseg + o, sizeof s_fanseg - o, "<a href='act:fan:%s' class='segc%s'>%s</a>",
                              fans[k].v, !strcmp(g_fan_mode, fans[k].v) ? " seg-on" : "", fans[k].lab);
            snprintf(s_fanseg + o, sizeof s_fanseg - o, "</div>");
            o = snprintf(s_liqseg, sizeof s_liqseg, "<div class='seg seg3'>");
            for (int k = 0; k < 3; k++)
                o += snprintf(s_liqseg + o, sizeof s_liqseg - o, "<a href='act:liq:%s' class='segc%s'>%s</a>",
                              liqs[k].v, !strcmp(g_liq_mode, liqs[k].v) ? " seg-on" : "", liqs[k].lab);
            snprintf(s_liqseg + o, sizeof s_liqseg - o, "</div>");
            snprintf(s_fanstate, sizeof s_fanstate, "%d%% · %d°C", d.fan_pct, d.fan_temp);
            if (d.liquid_active) snprintf(s_liqstate, sizeof s_liqstate, "运行中 · %d%%", d.liquid_pct);
            else                 snprintf(s_liqstate, sizeof s_liqstate, "待机");
        }

        {   /* CPU and per-modem temperatures, two per row */
            int o = snprintf(s_temps, sizeof s_temps, "<table class='grid2'><tr><td class='gl'>CPU <b>%ld°</b></td>",
                             d.cpu_temp);
            for (int k = 0; k < d.modem_n; k++) {
                char v[12];
                if (d.modem[k].has_temp) snprintf(v, sizeof v, "%d°", d.modem[k].temp);
                else                     snprintf(v, sizeof v, "--");
                o += snprintf(s_temps + o, sizeof s_temps - o, "%s<td%s>%s <b>%s</b></td>",
                              (k + 1) % 2 == 0 ? "</tr><tr>" : "", (k + 1) % 2 == 0 ? " class='gl'" : "",
                              d.modem[k].id, v);
            }
            if ((d.modem_n + 1) % 2) o += snprintf(s_temps + o, sizeof s_temps - o, "<td></td>");
            snprintf(s_temps + o, sizeof s_temps - o, "</tr></table>");
        }

        snprintf(s_ccextra, sizeof s_ccextra,
            "<div class='cc-grid cc2'>"
            "<a href='act:agg' class='ccb %s'><span class='ccc' id='cc-agg'></span><span class='ccl'>%s</span></a>"
            "<a href='act:fanon' class='ccb %s'><span class='ccc' id='cc-fan'></span><span class='ccl'>风扇常开</span></a>"
            "<a href='act:liqon' class='ccb %s'><span class='ccc' id='cc-liq'></span><span class='ccl'>液冷</span></a>"
            "</div>",
            g_dial.agg_on ? "on" : "off",
            (int32_t)(tnow - g_agg_armed_until) < 0 ? "再点关闭" : "聚合",
            g_dial.fan_on ? "on" : "off", g_dial.liq_on ? "on" : "off");
    }

    int connected = strstr(d.wan_status, "connect") != NULL;
    int wifi_master = (g_w24 == 1 || g_w5 == 1 || g_w6 == 1);
    /* the 6 GHz switch is there only when a radio is on that band */
    static char s_w6row[320];
    s_w6row[0] = 0;
    if (g_w6 >= 0)
        snprintf(s_w6row, sizeof s_w6row,
            "<div class=\"row\"><span class=\"lab\">6G 频段<span class=\"st\">%s</span></span>"
            "<span class=\"ctrl\"><a href=\"act:wifi6\" class=\"sw %s\"><span class=\"kn\"></span></a></span></div>",
            g_w6 ? "已开启" : "已关闭", g_w6 == 1 ? "on" : "off");
    int i = 0;
    t[i++] = (struct kv){ "STATUSBAR", s_sbar };
    t[i++] = (struct kv){ "DOTS", g_lock_state == 1 ? "" : s_dots };   /* the lock badge sits there */
    t[i++] = (struct kv){ "SIGBARS", "" };
    t[i++] = (struct kv){ "TIME", s_time };        t[i++] = (struct kv){ "BAT", s_bat };
    t[i++] = (struct kv){ "DATE", s_date };
    t[i++] = (struct kv){ "OPER", s_oper };        t[i++] = (struct kv){ "NETTYPE", d.net_type };
    t[i++] = (struct kv){ "BAND", d.band };        t[i++] = (struct kv){ "WAN", connected ? "已连接" : "未连接" };
    t[i++] = (struct kv){ "RSRP", s_rsrp };        t[i++] = (struct kv){ "RSRP_W", w_rsrp };
    t[i++] = (struct kv){ "RSRQ", s_rsrq };        t[i++] = (struct kv){ "RSRQ_W", w_rsrq };
    t[i++] = (struct kv){ "SINR", s_sinr };        t[i++] = (struct kv){ "SINR_W", w_sinr };
    t[i++] = (struct kv){ "BW", s_bw };            t[i++] = (struct kv){ "BW_W", w_bw };
    t[i++] = (struct kv){ "CELLID", s_cellid };    t[i++] = (struct kv){ "PCI", s_pci };
    t[i++] = (struct kv){ "TA", s_ta };            t[i++] = (struct kv){ "TADIST", s_tadist };
    t[i++] = (struct kv){ "SIGDETAILCARDS", s_signalcards };
    t[i++] = (struct kv){ "CELLBTN", g_show_cellid ? "隐藏" : "显示" };
    t[i++] = (struct kv){ "CLIENTS", s_clients };  t[i++] = (struct kv){ "UPTIME", s_up };
    t[i++] = (struct kv){ "RXSPEED", s_rxs };      t[i++] = (struct kv){ "TXSPEED", s_txs };
    t[i++] = (struct kv){ "RXBYTES", s_rxb };      t[i++] = (struct kv){ "TXBYTES", s_txb };
    t[i++] = (struct kv){ "CPU", s_cpu };          t[i++] = (struct kv){ "MEM", s_mem };
    t[i++] = (struct kv){ "QCI", s_qci };          t[i++] = (struct kv){ "AMBR", s_ambr };
    t[i++] = (struct kv){ "SSID", s_ssid };        t[i++] = (struct kv){ "KEY", s_key };
    t[i++] = (struct kv){ "ENC", d.wifi_enc[0] ? d.wifi_enc : "-" };
    t[i++] = (struct kv){ "MODEL", s_model };      t[i++] = (struct kv){ "FW", s_fw };
    t[i++] = (struct kv){ "PAGE", s_page };        t[i++] = (struct kv){ "NPAGES", s_np };
    t[i++] = (struct kv){ "CARRIERS", s_carriers }; t[i++] = (struct kv){ "GEN", s_gen };
    t[i++] = (struct kv){ "SIGNALCARDS", s_sigcards };
    t[i++] = (struct kv){ "SIGGAUGES", s_siggauges };
    t[i++] = (struct kv){ "NEIGHBORCARDS", s_neighborcards };
    t[i++] = (struct kv){ "GENCLASS", genc };
    t[i++] = (struct kv){ "THEME", theme_body_class() };
    t[i++] = (struct kv){ "SHEETCLASS", g_sheet ? "sheet" : "" };   /* opened by an edge pull */
    t[i++] = (struct kv){ "BATCLASS", d.bat_percent <= 20 ? "low" : "" };
    t[i++] = (struct kv){ "KEYBTN", g_show_key ? "隐藏密码" : "显示密码" };
    t[i++] = (struct kv){ "ADBCLASS", adb_on ? "on" : "off" };
    t[i++] = (struct kv){ "ADBSTATE", adb_state };
    t[i++] = (struct kv){ "USBPWRCLASS", usb_reverse ? "on" : "off" };
    t[i++] = (struct kv){ "USBPWRSTATE", usb_pwr_state };
    t[i++] = (struct kv){ "USBNETCLASS", usb_net_on ? "on" : "off" };
    t[i++] = (struct kv){ "USBNETSTATE", usb_net_state };
    t[i++] = (struct kv){ "BATPCTCLASS", g_show_batpct ? "on" : "off" };
    t[i++] = (struct kv){ "BATPCTSTATE", g_show_batpct ? "已显示" : "已隐藏" };
    t[i++] = (struct kv){ "SBSPEEDCLASS", g_show_sbspeed ? "on" : "off" };
    t[i++] = (struct kv){ "SBSPEEDSTATE", g_show_sbspeed ? "已显示" : "已隐藏" };
    t[i++] = (struct kv){ "TEXTADAPTCLASS", g_text_adapt ? "on" : "off" };
    t[i++] = (struct kv){ "TEXTADAPTSTATE", g_text_adapt ? "已开启 · 浅色背景上自动变深" : "已关闭" };
    t[i++] = (struct kv){ "GLASS", s_glass };
    t[i++] = (struct kv){ "GLASSBLUR", s_gblur };
    t[i++] = (struct kv){ "GLASSTINT", g_tints[g_glass_tint].name };
    t[i++] = (struct kv){ "GLASSTINTSEG", s_tintseg };
    t[i++] = (struct kv){ "THEMECLASS", "on" };
    t[i++] = (struct kv){ "THEMESTATE", g_themes[g_theme].name };
    t[i++] = (struct kv){ "SPUNITCLASS", g_speed_bits ? "on" : "off" };
    t[i++] = (struct kv){ "SPUNITSTATE", g_speed_bits ? "比特率 Mbps" : "字节率 MB/s" };
    t[i++] = (struct kv){ "SPUNIT", s_spu };
    t[i++] = (struct kv){ "CPUUSAGE", s_cusage }; t[i++] = (struct kv){ "CPUTEMP", s_ctemp };
    t[i++] = (struct kv){ "BATTEMP", s_btemp };   t[i++] = (struct kv){ "SWVER", s_swver };
    t[i++] = (struct kv){ "IMEI", s_imei };
    t[i++] = (struct kv){ "IMEIBTN", g_show_imei ? "隐藏" : "显示" };
    t[i++] = (struct kv){ "BRIGHT", s_bright };
    t[i++] = (struct kv){ "AUTOBRIGHTCLASS", dev_is_tf() && g_auto_bright ? "on" : "off" };
    t[i++] = (struct kv){ "BRIGHTBAR", dev_is_tf() && g_auto_bright ? "auto" : "" };
    t[i++] = (struct kv){ "AUTOOFF", s_autooff };
    t[i++] = (struct kv){ "REFRESHSEG", s_refreshseg };
    t[i++] = (struct kv){ "STSRCSEG", speedtest_src_html() };
    t[i++] = (struct kv){ "STDIRSEG", speedtest_dir_html() };
    t[i++] = (struct kv){ "STDURSEG", speedtest_dur_html() };
    t[i++] = (struct kv){ "STLOOPHINT", speedtest_loop_hint_html() };
    t[i++] = (struct kv){ "STSTATUS", speedtest_status_html() };
    t[i++] = (struct kv){ "STRESULT", speedtest_result_html() };
    t[i++] = (struct kv){ "STLOGCARD", speedtest_log_card_html() };
    t[i++] = (struct kv){ "STACTION", speedtest_action_html() };
    t[i++] = (struct kv){ "STINSTALL", speedtest_install_html() };
    t[i++] = (struct kv){ "STGAUGE", speedtest_dial_html() };
    t[i++] = (struct kv){ "STCHART", speedtest_lines_html() };
    t[i++] = (struct kv){ "STHOMEBTN", speedtest_home_button_html() };
    t[i++] = (struct kv){ "STHOMEINLINE", speedtest_home_inline_html() };
    t[i++] = (struct kv){ "STFUNCTIONTILE", speedtest_function_tile_html() };
    t[i++] = (struct kv){ "CUSTOMFUNCTIONTILES", custom_function_tiles_html() };
    t[i++] = (struct kv){ "SIGADVSTATE", s_sigadvstate };
    t[i++] = (struct kv){ "MEMDETAIL", s_memdet }; t[i++] = (struct kv){ "UPSHORT", s_upshort };
    t[i++] = (struct kv){ "CHGV", s_chgv };       t[i++] = (struct kv){ "CHGI", s_chgi };
    t[i++] = (struct kv){ "BATV", s_batv };       t[i++] = (struct kv){ "BATI", s_bati };
    t[i++] = (struct kv){ "PWR", s_pwr };         t[i++] = (struct kv){ "PWRLBL", d.charger_connect ? "充电" : "放电" };
    if (is_charts) {
        t[i++] = (struct kv){ "BATPWR", s_batpwr };
        t[i++] = (struct kv){ "BATPWRLBL", s_batpwrlbl };
    } else {
        t[i++] = (struct kv){ "BATPWR", "" };
        t[i++] = (struct kv){ "BATPWRLBL", "" };
    }
    t[i++] = (struct kv){ "NETSEG", s_netseg };   t[i++] = (struct kv){ "TOAST", s_toast };
    t[i++] = (struct kv){ "PWROFFLBL",  g_pwr_confirm == 1 ? "再按一次" : "关机" };
    t[i++] = (struct kv){ "PWROFFCLS",  g_pwr_confirm == 1 ? "armed" : "" };
    t[i++] = (struct kv){ "PWRREBLBL",  g_pwr_confirm == 2 ? "再按一次" : "重启" };
    t[i++] = (struct kv){ "PWRREBCLS",  g_pwr_confirm == 2 ? "armed" : "" };
    t[i++] = (struct kv){ "CURSA", s_cursa };     t[i++] = (struct kv){ "CURNSA", s_curnsa };
    t[i++] = (struct kv){ "CURLTE", s_curlte };
    t[i++] = (struct kv){ "NFCCLASS", d.nfc_switch ? "on" : "off" };
    t[i++] = (struct kv){ "NFCSTATE", d.nfc_switch ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFICLASS", wifi_master ? "on" : "off" };
    t[i++] = (struct kv){ "WIFISTATE", (g_w24 < 0 && g_w5 < 0 && g_w6 < 0) ? "—" : wifi_master ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFI6ROW", s_w6row };
    t[i++] = (struct kv){ "WIFI24CLASS", g_w24 == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "WIFI24STATE", g_w24 < 0 ? "—" : g_w24 ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFI5CLASS", g_w5 == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "WIFI5STATE", g_w5 < 0 ? "—" : g_w5 ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "DPSCLASS", g_dps == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "DPSSTATE", g_dps < 0 ? "—" : g_dps ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "PSMCLASS", g_wpsm == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "PSMSTATE", g_wpsm < 0 ? "—" : g_wpsm ? "已开启（省电）" : "已关闭（高性能）" };
    t[i++] = (struct kv){ "CLIENTLIST", s_clist }; t[i++] = (struct kv){ "DHCP_IP", d.dhcp_ip[0] ? d.dhcp_ip : "-" };
    t[i++] = (struct kv){ "SMSLIST", s_smslist };
    t[i++] = (struct kv){ "DHCP_POOL", g_dhcp_pool[0] ? g_dhcp_pool : s_pool };
    t[i++] = (struct kv){ "DHCP_LEASE", s_lease };
    t[i++] = (struct kv){ "MODEMCARDS", s_modemcards };
    t[i++] = (struct kv){ "AGGCLASS", g_dial.agg_on ? "on" : "off" };
    t[i++] = (struct kv){ "AGGSTATE", s_aggstate };
    t[i++] = (struct kv){ "AGGSUM", s_aggsum };
    t[i++] = (struct kv){ "AGGPATHS", s_aggpaths };
    t[i++] = (struct kv){ "FANSEG", s_fanseg };
    t[i++] = (struct kv){ "FANSTATE", s_fanstate };
    t[i++] = (struct kv){ "LIQSEG", s_liqseg };
    t[i++] = (struct kv){ "LIQSTATE", s_liqstate };
    t[i++] = (struct kv){ "TEMPS", s_temps };
    t[i++] = (struct kv){ "CCEXTRA", s_ccextra };

    /* ---- lock screen (PIN pad): dots reflect typed digits. ---- */
    static char s_pindots[512], s_lockaux[140];
    {
        int filled = (int)strlen(g_pin_entry);
        int o = snprintf(s_pindots, sizeof s_pindots, "<div class='pin-slots'>");
        for (int k = 0; k < 4; k++)
            o += (k < filled)
               ? snprintf(s_pindots + o, sizeof s_pindots - o, "<span class='ps'><span class='dot'></span></span>")
               : snprintf(s_pindots + o, sizeof s_pindots - o,
                          "<span class='ps'><span class='ring'><span class='ri'></span></span></span>");
        snprintf(s_pindots + o, sizeof s_pindots - o, "</div>");
    }
    if (g_lock_state == 2)
        snprintf(s_lockaux, sizeof s_lockaux, "<a href='act:lockcancel' class='kp kpx'>取消</a>");
    else s_lockaux[0] = 0;
    t[i++] = (struct kv){ "PINDOTS", s_pindots };
    t[i++] = (struct kv){ "LOCKAUX", s_lockaux };
    t[i++] = (struct kv){ "LOCKTITLE", g_lock_state == 2 ? "设置锁屏密码" : "请输入锁屏密码" };
    t[i++] = (struct kv){ "LOCKMSG", g_lock_err ? "<div class='lock-err'>密码错误</div>" : "" };
    t[i++] = (struct kv){ "LOCKCLASS", lock_enabled() ? "on" : "off" };
    t[i++] = (struct kv){ "LOCKSTATE", lock_enabled() ? "已开启" : "已关闭" };
    return i;
}

static void refresh_status_cache(void)
{
    struct kv t[256];
    (void)build_kv(t, NULL);
}

static const char *kv_get(struct kv *t, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].k, key)) return t[i].v ? t[i].v : "";
    return "";
}

static const char *custom_page_html(const char *path)
{
    static char out[PAGE_HTML_CACHE_CAP];
    const char *tmpl = read_template_cached(path);
    const char *start, *end, *body, *gt;
    char *body_src;
    size_t len;
    struct kv t[256];
    int n;
    char raw_title[96], title[160];
    const char *base;
    char *filled;
    int o;

    if (!tmpl) return NULL;
    start = tmpl;
    end = tmpl + strlen(tmpl);
    body = strcasestr(tmpl, "<body");
    if (body) {
        gt = strchr(body, '>');
        if (gt) {
            start = gt + 1;
            end = strcasestr(start, "</body>");
            if (!end) end = tmpl + strlen(tmpl);
        }
    }
    len = (size_t)(end - start);
    body_src = malloc(len + 1);
    if (!body_src) return NULL;
    memcpy(body_src, start, len);
    body_src[len] = 0;

    n = build_kv(t, path);
    filled = apply_template(body_src, t, n);
    free(body_src);

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!function_title_from_html(tmpl, raw_title, sizeof raw_title))
        function_label_from_name(raw_title, sizeof raw_title, base);
    html_esc(title, sizeof title, raw_title);

    o = snprintf(out, sizeof out,
                 "<!DOCTYPE html><html lang=\"zh-CN\">"
                 "<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"style.css\"></head>"
                 "<body class=\"%s\">"
                 "%s"
                 "<div class=\"subtop\"><a href=\"act:backfunc\" class=\"subback\">返回</a>"
                 "<span class=\"subname\">%s</span></div>"
                 "<div class=\"custom-page\">",
                 theme_body_class(), kv_get(t, n, "STATUSBAR"), title);
    if (o < 0) o = 0;
    if (o < (int)sizeof(out) - 1) {
        size_t room = sizeof(out) - (size_t)o - 1;
        size_t fl = strlen(filled);
        if (fl > room) fl = room;
        memcpy(out + o, filled, fl);
        o += (int)fl;
        out[o] = 0;
    }
    if (o < (int)sizeof(out) - 1)
        snprintf(out + o, sizeof(out) - (size_t)o,
                 "</div>%s<div class=\"pgpad\"></div></body></html>",
                 kv_get(t, n, "TOAST"));
    return out;
}

/* Build the data-filled HTML for a page (returns a static buffer). */
static const char *page_html(const char *path)
{
    if (path_is_function_page(path)) return custom_page_html(path);
    const char *tmpl = read_template_cached(path);   /* cache templates with mtime invalidation */
    if (!tmpl) return NULL;
    struct kv t[256];
    int n = build_kv(t, path);
    char *html = apply_template(tmpl, t, n);
    return html;
}

static void invalidate_render_html_cache(void)
{
    g_render_html_cache_len = 0;
    g_render_html_cache_path[0] = 0;
    g_render_html_cache_css_mtime = -1;
}

/* ---- power menu: three large round buttons with native-drawn glyphs
 * (litehtml can't render a power symbol / circular arrow / X reliably). The
 * HTML provides the circle placeholders (#pmc-*) + labels + tap targets; we
 * fill the disc and draw the glyph centered over each. ---- */
#define PM_PI 3.14159265358979

static void pm_disc(int cx, int cy, int rad, int r, int g, int b, int a)
{
    html_view_fill_round_rect(cx - rad, cy - rad, rad * 2, rad * 2, rad, r, g, b, a);
}

/* thick line segment (a-b) as a filled quad */
static void pm_line(double x1, double y1, double x2, double y2, double th, int r, int g, int b)
{
    double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy);
    if (len < 0.001) return;
    double nx = -dy / len * th / 2.0, ny = dx / len * th / 2.0;
    int xs[4] = { (int)(x1 + nx + 0.5), (int)(x2 + nx + 0.5), (int)(x2 - nx + 0.5), (int)(x1 - nx + 0.5) };
    int ys[4] = { (int)(y1 + ny + 0.5), (int)(y2 + ny + 0.5), (int)(y2 - ny + 0.5), (int)(y1 - ny + 0.5) };
    html_view_fill_poly(xs, ys, 4, r, g, b, 255);
}

/* annular sector (ring band) from a0..a1 degrees (math angles; screen y down) */
static void pm_arc(int cx, int cy, double ro, double ri, int a0, int a1, int r, int g, int b)
{
    int xs[160], ys[160], N = 0;
    for (int a = a0; a <= a1 && N < 158; a += 8) {
        double t = a * PM_PI / 180.0;
        xs[N] = cx + (int)(ro * cos(t) + 0.5); ys[N] = cy - (int)(ro * sin(t) + 0.5); N++;
    }
    for (int a = a1; a >= a0 && N < 160; a -= 8) {
        double t = a * PM_PI / 180.0;
        xs[N] = cx + (int)(ri * cos(t) + 0.5); ys[N] = cy - (int)(ri * sin(t) + 0.5); N++;
    }
    html_view_fill_poly(xs, ys, N, r, g, b, 255);
}

/* kind: 0=power, 1=reboot, 2=cancel */
/* One power-menu button: a glass disc ringed in the action colour with the
 * glyph in that colour; once armed (first tap) the disc fills with it and the
 * glyph turns dark. kind: 0 power off, 1 reboot, 2 cancel. */
static void pm_glyph(const char *sel, int kind, unsigned col)
{
    int x, y, w, h;
    if (!html_view_rect(sel, &x, &y, &w, &h)) return;
    float rad = (float)(w < h ? w : h) / 2.f;
    float cx = (float)x + (float)w / 2.f, cy = (float)y + (float)h / 2.f;
    int armed = (kind == 0 && g_pwr_confirm == 1) || (kind == 1 && g_pwr_confirm == 2);
    unsigned ic = armed ? 0x0B1220u : col;
    float r = rad * .40f, th = rad * .10f;

    if (armed) {
        html_view_fill_circle(cx, cy, rad + 5.f, RGB3(col), 70);   /* soft halo */
        html_view_fill_circle(cx, cy, rad, RGB3(col), 255);
    } else {
        html_view_glass_rect(x + (w - (int)(2.f * rad)) / 2, y + (h - (int)(2.f * rad)) / 2,
                             (int)(2.f * rad), (int)(2.f * rad), (int)rad);
        html_view_arc(cx, cy, rad - 1.f, 1.6f, 0.f, 360.f, RGB3(col), 110);
    }
    if (kind == 0) {                         /* power: open ring + bar through the gap */
        html_view_arc(cx, cy + 2.f, r, th, 38.f, 322.f, RGB3(ic), 255);
        html_view_line(cx, cy - r * 1.25f, cx, cy - r * .15f, th, RGB3(ic), 255);
    } else if (kind == 1) {                  /* reboot: ring + arrowhead at its end */
        const float a1 = 310.f, ra = a1 * (float)PM_PI / 180.f;
        float ex = cx + r * sinf(ra), ey = cy - r * cosf(ra);
        float tx = cosf(ra), ty = sinf(ra);          /* clockwise tangent */
        float nx = sinf(ra), ny = -cosf(ra);         /* outward normal */
        float L = th * 2.2f, hw = th * 1.6f;
        int ax[3] = { (int)(ex + tx * L + .5f), (int)(ex + nx * hw + .5f), (int)(ex - nx * hw + .5f) };
        int ay[3] = { (int)(ey + ty * L + .5f), (int)(ey + ny * hw + .5f), (int)(ey - ny * hw + .5f) };
        html_view_arc(cx, cy, r, th, 40.f, a1, RGB3(ic), 255);
        html_view_fill_poly(ax, ay, 3, RGB3(ic), 255);
    } else {                                 /* cancel: X */
        float d = r * .78f;
        html_view_line(cx - d, cy - d, cx + d, cy + d, th, RGB3(ic), 255);
        html_view_line(cx - d, cy + d, cx + d, cy - d, th, RGB3(ic), 255);
    }
}

static void draw_power_menu(void)
{
    pm_glyph("#pmc-off",    0, C_BAD);
    pm_glyph("#pmc-reboot", 1, C_WARN);
    pm_glyph("#pmc-cancel", 2, C_DIM);
}

static int st_hist_max(const int *hist, int n)
{
    int mx = 10;
    for (int i = 0; i < n; i++) if (hist[i] > mx) mx = hist[i];
    return mx;
}

static void st_chart_grid(int x, int y, int w, int h)
{
    const int r = 0x2d;
    const int g = 0x3c;
    const int b = 0x4a;
    const int a = 150;

    html_view_fill_rect(x + 1, y + h / 3, w - 2, 1, r, g, b, a);
    html_view_fill_rect(x + 1, y + h * 2 / 3, w - 2, 1, r, g, b, a);
}

static void st_draw_one_chart(const char *sel, const int *hist, int n, int r, int g, int b)
{
    int x, y, w, h;
    if (!html_view_rect(sel, &x, &y, &w, &h)) return;
    st_chart_grid(x, y, w, h);
    if (n >= 2) {
        html_view_polyline(x + 3, y + 3, w - 6, h - 6, hist, n, 0, st_hist_max(hist, n), r, g, b, 2, 24);
    } else if (n == 1) {
        int cy = y + h - 6 - (hist[0] * (h - 12)) / st_hist_max(hist, n);
        html_view_fill_round_rect(x + w - 10, cy - 3, 6, 6, 3, r, g, b, 255);
    }
}

static void draw_speedtest_widgets(void)
{
    int x, y, w, h;
    html_view_set_clip_top(26);
    if (html_view_rect("#st-gauge-dial", &x, &y, &w, &h)) {
        double cur = speedtest_display_mbps();
        double scale = speedtest_display_peak_mbps();
        int pct = speedtest_pct(cur, scale);
        int rad = (w < h ? w : h) / 2 - 3;
        int cx = x + w / 2, cy = y + h / 2;
        double ang = (210.0 - 240.0 * pct / 100.0) * PM_PI / 180.0;
        double vx = cos(ang), vy = -sin(ang);
        double len = rad * 0.58, back = rad * 0.14, half = rad * 0.055;
        double tipx = cx + vx * len, tipy = cy + vy * len;
        double basex = cx - vx * back, basey = cy - vy * back;
        double px = -vy, py = vx;
        int nx[3] = {
            (int)(tipx + 0.5),
            (int)(basex + px * half + 0.5),
            (int)(basex - px * half + 0.5)
        };
        int ny[3] = {
            (int)(tipy + 0.5),
            (int)(basey + py * half + 0.5),
            (int)(basey - py * half + 0.5)
        };
        char num[32];
        const int outer_r = 0x0b;
        const int outer_g = 0x12;
        const int outer_b = 0x1b;
        const int inner_r = 0x12;
        const int inner_g = 0x20;
        const int inner_b = 0x2e;
        const int track_r = 0x34;
        const int track_g = 0x43;
        const int track_b = 0x51;
        const int tick_r = 0xd8;
        const int tick_g = 0xe5;
        const int tick_b = 0xee;
        const int num_r = 0xff;
        const int num_g = 0xff;
        const int num_b = 0xff;
        const int unit_r = 0x9f;
        const int unit_g = 0xb4;
        const int unit_b = 0xc5;
        const int hub_outer_r = 0xf4;
        const int hub_outer_g = 0xf7;
        const int hub_outer_b = 0xfb;
        const int hub_inner_r = 0x12;
        const int hub_inner_g = 0x20;
        const int hub_inner_b = 0x2e;

        pm_disc(cx, cy, rad, outer_r, outer_g, outer_b, 255);
        pm_disc(cx, cy, rad - 5, inner_r, inner_g, inner_b, 255);
        pm_arc(cx, cy, rad - 12, rad - 20, -30, 210, track_r, track_g, track_b);
        if (pct > 0)
            pm_arc(cx, cy, rad - 12, rad - 20, (int)(210.0 - 240.0 * pct / 100.0), 210, RGB3(theme_a()));
        for (int i = 0; i <= 10; i++) {
            double a = (210.0 - 240.0 * i / 10.0) * PM_PI / 180.0;
            double tx = cos(a), ty = -sin(a);
            pm_line(cx + tx * (rad - 30), cy + ty * (rad - 30),
                    cx + tx * (rad - 22), cy + ty * (rad - 22),
                    i % 5 == 0 ? 3.0 : 2.0, tick_r, tick_g, tick_b);
        }
        html_view_fill_poly(nx, ny, 3, 0xf0, 0xb2, 0x47, 255);
        pm_disc(cx, cy, 8, hub_outer_r, hub_outer_g, hub_outer_b, 255);
        pm_disc(cx, cy, 4, hub_inner_r, hub_inner_g, hub_inner_b, 255);
        snprintf(num, sizeof num, "%.1f", cur);
        draw_center_text_px(cx, cy + 38, num, 28, 1, num_r, num_g, num_b, 255);
        draw_center_text_px(cx, cy + 62, "Mbps", 12, 0, unit_r, unit_g, unit_b, 255);
    }
    st_draw_one_chart("#st-chart-dl", g_st_dl_hist, g_st_dl_n, RGB3(theme_a()));
    st_draw_one_chart("#st-chart-ul", g_st_ul_hist, g_st_ul_n, RGB3(theme_b()));
    html_view_set_clip_top(0);
}

/* ---- pulled sheets: open/close with a vertical slide ---- */
#define EDGE_PULL_PX 60      /* vertical travel that commits a sheet pull */
/* The U60 Pro panel is mounted upside down, so the framebuffer holds the page
 * rotated 180 degrees; the TopFlow panel is upright (set in main()). */
static int g_panel_rot = 1;
static uint16_t g_slide_a[320 * 480], g_slide_b[320 * 480];

/* Logical row y of the framebuffer: write n pixels from src at logical x0. */
static void fb_put_row(drm_disp_t *d, int y, int x0, const uint16_t *src, int n)
{
    if (n <= 0) return;
    if (!g_panel_rot) {
        memcpy(d->fb + (size_t)y * d->pitch_px + x0, src, (size_t)n * sizeof *src);
        return;
    }
    uint16_t *dp = d->fb + (size_t)(d->height - 1 - y) * d->pitch_px + (d->width - 1 - x0);
    for (int i = 0; i < n; i++) *dp-- = src[i];
}

static uint16_t fb_get(const drm_disp_t *d, int x, int y)
{
    return g_panel_rot ? d->fb[(size_t)(d->height - 1 - y) * d->pitch_px + (d->width - 1 - x)]
                       : d->fb[(size_t)y * d->pitch_px + x];
}

/* ---- the wallpaper stays put while frames move ----
 * A rendered frame carries the wallpaper drawn into it, so sliding the frame
 * (page swipe, scroll, sheet, back swipe) would slide the wallpaper too. Each
 * buffer html_view drew (or html_view_bg_copy copied to) knows how much
 * wallpaper and glass shows through every pixel, and html_view_bg_fix_row
 * gives a moved row the ground where it lands, tinted fills and text edges
 * included. A frame without that (written some other way) falls back to
 * wp_key: only pixels still equal to the wallpaper follow. Rows here are
 * logical (fb_put_row rotates). */
static uint16_t g_wpl[320 * 480];
static int g_wpc_gen = -1;

static int wp_pinned(const drm_disp_t *d)
{
    if (d->width != 320 || d->height != 480 || d->pitch_px != 320 || !html_view_wallpaper_on()) return 0;
    if (g_wpc_gen != g_wp_gen) {
        for (int y = 0; y < 480; y++) memcpy(&g_wpl[y * 320], html_view_wallpaper_row(y), 320 * sizeof g_wpl[0]);
        g_wpc_gen = g_wp_gen;
    }
    return 1;
}

/* dst = src, except pixels equal to the wallpaper `from` show `to` */
static void wp_key(uint16_t *dst, const uint16_t *src, int n, const uint16_t *from, const uint16_t *to)
{
    for (int i = 0; i < n; i++) dst[i] = src[i] == from[i] ? to[i] : src[i];
}

/* The U60 Pro panel is upside down, the TopFlow one upright; touch follows
 * the panel (they are one TDDI chip). */
static void apply_panel_rotation(drm_disp_t *d, touch_input_t *t)
{
    g_panel_rot = !dev_is_tf();
    d->rot180 = g_panel_rot;
    html_view_set_rotate(g_panel_rot);
    if (t) t->rot180 = g_panel_rot;
}

/* Push logical rows [y0, y1]. */
static void fb_dirty_rows(drm_disp_t *d, int y0, int y1)
{
    if (g_panel_rot) drm_disp_dirty(d, 0, d->height - 1 - y1, d->width - 1, d->height - 1 - y0);
    else             drm_disp_dirty(d, 0, y0, d->width - 1, y1);
}

/* ---- moving frames sent while the next one is worked out ----
 * DIRTYFB blocks for the whole panel transfer (about 32 ms on the U60), so a
 * frame composed and then sent pays both. Page swipes and scrolls compose
 * into g_next while tx_main sends the previous frame, and write the fb only
 * once that send is done (tx_wait); everything else that writes the fb waits
 * for it first too. */
static pthread_mutex_t g_tx_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tx_cv = PTHREAD_COND_INITIALIZER;
static drm_disp_t *g_tx_d;
static int g_tx_busy, g_tx_y0, g_tx_y1, g_tx_state;   /* state: 0 not started, 1 running, -1 none */
static uint16_t g_next[320 * 480];

static void *tx_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_tx_mu);
    for (;;) {
        while (!g_tx_busy) pthread_cond_wait(&g_tx_cv, &g_tx_mu);
        drm_disp_t *d = g_tx_d;
        int y0 = g_tx_y0, y1 = g_tx_y1;
        pthread_mutex_unlock(&g_tx_mu);
        fb_dirty_rows(d, y0, y1);
        pthread_mutex_lock(&g_tx_mu);
        g_tx_busy = 0;
        pthread_cond_broadcast(&g_tx_cv);
    }
    return NULL;
}

static void tx_idle(void)
{
    if (g_tx_state != 1) return;
    pthread_mutex_lock(&g_tx_mu);
    while (g_tx_busy) pthread_cond_wait(&g_tx_cv, &g_tx_mu);
    pthread_mutex_unlock(&g_tx_mu);
}

/* Before writing the fb: a live wallpaper playing stops where it is. */
static void tx_wait(void)
{
    live_stop();
    tx_idle();
}

/* Send logical rows [y0, y1] of the fb without waiting (after tx_wait). */
static void tx_send(drm_disp_t *d, int y0, int y1)
{
    if (g_tx_state == 0) {
        pthread_t t;
        g_tx_state = pthread_create(&t, NULL, tx_main, NULL) == 0 ? 1 : -1;
        if (g_tx_state == 1) pthread_detach(t);
    }
    if (g_tx_state != 1) { fb_dirty_rows(d, y0, y1); return; }
    pthread_mutex_lock(&g_tx_mu);
    g_tx_d = d;
    g_tx_y0 = y0;
    g_tx_y1 = y1;
    g_tx_busy = 1;
    pthread_cond_broadcast(&g_tx_cv);
    pthread_mutex_unlock(&g_tx_mu);
}

static void maybe_dump_fb(drm_disp_t *d);

/* Logical row r of snapshot src (a full frame as the fb holds it) into out. */
static void snap_row(const drm_disp_t *d, const uint16_t *src, int r, uint16_t *out)
{
    const int W = d->width;
    const uint16_t *sp = src + (size_t)(g_panel_rot ? d->height - 1 - r : r) * d->pitch_px;
    if (!g_panel_rot) { memcpy(out, sp, (size_t)W * sizeof *out); return; }
    for (int x = 0; x < W; x++) out[x] = sp[W - 1 - x];
}

/* Logical row r of snapshot src as it shows at logical row dst_r, into out. */
static void slide_row(drm_disp_t *d, const uint16_t *src, int r, int dst_r, uint16_t *out)
{
    snap_row(d, src, r, out);
    if (r != dst_r && wp_pinned(d) && !html_view_bg_fix_row(src, out, dst_r, 0, d->width, 0, r - dst_r))
        wp_key(out, out, d->width, &g_wpl[r * 320], &g_wpl[dst_r * 320]);
}

/* Put g_next rows [y0, y1] on the fb once the frame before is sent, and send
 * them without waiting: the next frame is worked out meanwhile. */
static void next_show(drm_disp_t *d, int y0, int y1)
{
    tx_wait();
    html_view_bg_drop(d->fb);
    for (int y = y0; y <= y1; y++) fb_put_row(d, y, 0, &g_next[(size_t)y * d->width], d->width);
    tx_send(d, y0, y1);
    maybe_dump_fb(d);
}

/* ---- live wallpaper ----
 * While the clip plays (html_view_live_*), a thread of its own (live_main)
 * shows each frame when it is due: the page in g_live_snap (logical rows,
 * drawn over the wallpaper) regrounded onto the frame. A refresh meanwhile
 * draws the page off screen into g_live_back and hands it over as
 * g_live_next (live_render), so neither waits for the other. Whatever else
 * writes the fb stops it first (tx_wait): the frame on screen becomes the
 * wallpaper, and the clip plays on from there once things are quiet again. */
static uint16_t g_live_bufs[3][320 * 480], g_live_stage[320 * 480];
static uint16_t *g_live_snap = g_live_bufs[0], *g_live_next = g_live_bufs[1], *g_live_back = g_live_bufs[2];
static drm_disp_t *g_live_d;
static int g_live_run;
static pthread_mutex_t g_lp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_lp_cv = PTHREAD_COND_INITIALIZER;
static int g_lp_thread, g_lp_on, g_lp_busy, g_lp_new;   /* new: a page handed over in g_live_next */

/* Take the page handed over (lock held). */
static void live_take_next(void)
{
    uint16_t *t = g_live_snap;
    g_live_snap = g_live_next;
    g_live_next = t;
    html_view_live_snap_next();
    g_lp_new = 0;
}

static int g_scrollbuf_ok;   /* g_scrollbuf still holds the page (see scrollbuf_prepare) */

/* Rows [y0, y1) of the page over the frame playing onto the fb, and send. */
static void live_show(drm_disp_t *d, int y0, int y1)
{
    const int W = d->width;
    for (int y = y0; y < y1; y++) {   /* while the last frame is sent */
        uint16_t *row = &g_live_stage[(size_t)y * W];
        memcpy(row, &g_live_snap[(size_t)y * W], (size_t)W * sizeof *row);
        html_view_live_fix_row(row, y);
    }
    tx_idle();
    for (int y = y0; y < y1; y++) fb_put_row(d, y, 0, &g_live_stage[(size_t)y * W], W);
    tx_send(d, y0, y1 - 1);
    maybe_dump_fb(d);
}

static void *live_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lp_mu);
    for (;;) {
        if (!g_lp_on) { pthread_cond_wait(&g_lp_cv, &g_lp_mu); continue; }
        int y0 = 0, y1 = 0;
        if (g_lp_new) {   /* a new page: all of it */
            live_take_next();
            y1 = g_live_d->height;
        } else {
            int ms = html_view_live_wait(millis());
            if (ms > 0 || !html_view_live_poll(millis(), &y0, &y1)) {   /* not due, or not worked out yet */
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                long ns = ts.tv_nsec + (ms > 1 ? ms : 1) * 1000000L;
                ts.tv_sec += ns / 1000000000L;
                ts.tv_nsec = ns % 1000000000L;
                pthread_cond_timedwait(&g_lp_cv, &g_lp_mu, &ts);
                continue;
            }
        }
        g_lp_busy = 1;
        pthread_mutex_unlock(&g_lp_mu);
        live_show(g_live_d, y0, y1);
        pthread_mutex_lock(&g_lp_mu);
        g_lp_busy = 0;
        pthread_cond_broadcast(&g_lp_cv);
    }
    return NULL;
}

static void live_begin(drm_disp_t *d)
{
    if (!wp_pinned(d)) return;
    for (int y = 0; y < d->height; y++) snap_row(d, d->fb, y, &g_live_snap[(size_t)y * d->width]);
    html_view_live_snap(d->fb);
    html_view_live_snap_next();
    html_view_live_start(millis());
    if (!html_view_live_on()) return;
    g_live_d = d;
    g_live_run = 1;
    pthread_mutex_lock(&g_lp_mu);
    if (!g_lp_thread) {
        pthread_t t;
        g_lp_thread = pthread_create(&t, NULL, live_main, NULL) == 0;
        if (g_lp_thread) pthread_detach(t);
    }
    g_lp_new = 0;
    g_lp_on = 1;
    pthread_cond_broadcast(&g_lp_cv);
    pthread_mutex_unlock(&g_lp_mu);
}

static void live_stop(void)
{
    if (!g_live_run) return;
    drm_disp_t *d = g_live_d;
    pthread_mutex_lock(&g_lp_mu);
    g_lp_on = 0;
    while (g_lp_busy) pthread_cond_wait(&g_lp_cv, &g_lp_mu);
    int fresh = g_lp_new;
    if (fresh) live_take_next();
    pthread_mutex_unlock(&g_lp_mu);
    if (fresh) live_show(d, 0, d->height);   /* the page handed over last, not shown yet */
    tx_idle();
    html_view_live_snap_put(d->fb);   /* the fb holds that page now */
    html_view_live_freeze();
    g_live_run = 0;
    g_wp_gen++;                       /* the frame on screen is the wallpaper now: */
    invalidate_render_html_cache();   /* frames kept were drawn over the one before */
    g_scrollbuf_ok = 0;
}

/* Slide between two frames: `over` moves in (opening) or out (closing) from
 * the top (from_top) or the bottom edge, `under` stays put beneath it. */
#define SHEET_SLIDE_MS 220

static void sheet_slide(drm_disp_t *d, const uint16_t *over, const uint16_t *under, int from_top, int opening)
{
    const int H = d->height;
    if (d->width * H > 320 * 480) return;
    uint32_t t0 = millis();
    for (;;) {
        /* as many frames as the panel takes in SHEET_SLIDE_MS, eased out */
        float t = (float)(millis() - t0) / SHEET_SLIDE_MS;
        if (t > 1.f) t = 1.f;
        int vis = (int)(H * (1.f - (1.f - t) * (1.f - t) * (1.f - t)) + .5f);
        if (!opening) vis = H - vis;                          /* visible rows of `over` */
        for (int r = 0; r < H; r++) {
            int in_over = from_top ? r < vis : r >= H - vis;
            uint16_t *row = &g_next[(size_t)r * d->width];
            if (in_over) slide_row(d, over, from_top ? r + H - vis : r - (H - vis), r, row);
            else         slide_row(d, under, r, r, row);
        }
        next_show(d, 0, H - 1);
        if (t >= 1.f) break;
    }
}

static void sheet_snapshot(drm_disp_t *d, uint16_t *dst)
{
    if ((size_t)d->pitch_px * d->height > 320 * 480) return;
    memcpy(dst, d->fb, (size_t)d->pitch_px * d->height * 2);
    html_view_bg_copy(dst, d->fb);
}

static void render(drm_disp_t *disp, const char *path);
static void maybe_dump_fb(drm_disp_t *d);

/* What a vertical pull anywhere on the screen does, by finger direction (dir
 * > 0 down): on a top-level page down opens the control center and up the
 * SMS list; inside a sheet the reverse pull closes it. A page that scrolls
 * gives the pull to the sheet only once it is at that end, so down at the top
 * and up at the bottom (maxs: the page's scroll range). */
static int sheet_pull_target(int dir, int maxs, int *which, int *closing)
{
    *which = *closing = 0;
    if (dir > 0 ? g_scroll > 0 : g_scroll < maxs) return 0;
    if (dir > 0) {
        if (g_sheet == SHEET_SMS) *closing = 1;
        else if (!g_subpage[0]) *which = SHEET_CONTROL;
    } else {
        if (g_sheet == SHEET_CONTROL) *closing = 1;
        else if (!g_subpage[0]) *which = SHEET_SMS;
    }
    return *which || *closing;
}

/* A quick vertical stroke of dy that was only seen after the release: slide
 * the sheet the whole way. Returns 1 when the pull was used. */
static int sheet_edge_pull(drm_disp_t *d, int dy, int maxs)
{
    int which, closing;
    if ((dy < 0 ? -dy : dy) < EDGE_PULL_PX || !sheet_pull_target(dy > 0 ? 1 : -1, maxs, &which, &closing))
        return 0;

    sheet_snapshot(d, g_slide_a);                 /* frame on screen now */
    g_render_quiet = 1;                           /* the next frame only feeds the slide */
    if (closing) {
        int was = g_sheet;
        subpage_close();
        render(d, active_page_path());
        sheet_snapshot(d, g_slide_b);             /* page underneath */
        g_render_quiet = 0;
        sheet_slide(d, g_slide_a, g_slide_b, was == SHEET_CONTROL, 0);
    } else {
        if (!subpage_open(which == SHEET_CONTROL ? "control.html" : "sms.html")) { g_render_quiet = 0; return 0; }
        g_sheet = which;
        render(d, active_page_path());
        sheet_snapshot(d, g_slide_b);             /* the sheet */
        g_render_quiet = 0;
        sheet_slide(d, g_slide_b, g_slide_a, which == SHEET_CONTROL, 1);
    }
    maybe_dump_fb(d);                             /* the slide ends on the rendered frame */
    return 1;
}

/* A live edge pull: the sheet follows the finger and the release finishes or
 * cancels it. g_slide_a holds the frame on screen when the pull began,
 * g_slide_b the frame on the far side (the sheet when opening, the page under
 * it when closing); the state change itself waits for the release. */
static struct {
    int active, which, closing, vis;
    int from_top;   /* edge the sheet hangs from: control center top, SMS bottom */
    int dir;        /* finger direction: +1 down, -1 up */
} g_pull;

/* Rows of the moving sheet on screen for a finger travel of dy. */
static int sheet_pull_vis(int H, int dy)
{
    int travel = g_pull.dir > 0 ? dy : -dy;
    if (travel < 0) travel = 0;
    if (travel > H) travel = H;
    return g_pull.closing ? H - travel : travel;
}

static void sheet_pull_draw(drm_disp_t *d, int vis)
{
    const int H = d->height;
    const uint16_t *over = g_pull.closing ? g_slide_a : g_slide_b;
    const uint16_t *under = g_pull.closing ? g_slide_b : g_slide_a;
    if (d->width * H > 320 * 480) return;
    for (int r = 0; r < H; r++) {
        int in_over = g_pull.from_top ? r < vis : r >= H - vis;
        uint16_t *row = &g_next[(size_t)r * d->width];
        if (in_over) slide_row(d, over, g_pull.from_top ? r + H - vis : r - (H - vis), r, row);
        else         slide_row(d, under, r, r, row);
    }
    next_show(d, 0, H - 1);
    g_pull.vis = vis;
}

/* A vertical drag heading down (dir > 0) or up: prepare the far frame
 * quietly and leave the state as it was. Returns 1 when a sheet can open or
 * close this way. */
static int sheet_pull_begin(drm_disp_t *d, int dir, int maxs)
{
    const int H = d->height;
    int which, closing;
    char sub[sizeof g_subpage], sub_path[sizeof g_subpage_path];
    int sheet = g_sheet, scroll = g_scroll;

    if (!sheet_pull_target(dir, maxs, &which, &closing)) return 0;

    snprintf(sub, sizeof sub, "%s", g_subpage);
    snprintf(sub_path, sizeof sub_path, "%s", g_subpage_path);
    sheet_snapshot(d, g_slide_a);
    g_render_quiet = 1;
    g_pull.dir = dir > 0 ? 1 : -1;
    if (closing) {
        g_pull.from_top = g_sheet == SHEET_CONTROL;   /* it leaves the way it came */
        subpage_close();
    } else {
        g_pull.from_top = which == SHEET_CONTROL;
        if (!subpage_open(which == SHEET_CONTROL ? "control.html" : "sms.html")) { g_render_quiet = 0; return 0; }
        g_sheet = which;
    }
    render(d, active_page_path());
    sheet_snapshot(d, g_slide_b);
    g_render_quiet = 0;
    /* back to where we were until the release decides */
    snprintf(g_subpage, sizeof g_subpage, "%s", sub);
    snprintf(g_subpage_path, sizeof g_subpage_path, "%s", sub_path);
    g_sheet = sheet;
    g_scroll = scroll;
    invalidate_render_html_cache();

    g_pull.active = 1;
    g_pull.which = which;
    g_pull.closing = closing;
    g_pull.vis = closing ? H : 0;
    return 1;
}

static void sheet_pull_move(drm_disp_t *d, int dy)
{
    if (g_pull.active) sheet_pull_draw(d, sheet_pull_vis(d->height, dy));
}

/* Release: past EDGE_PULL_PX the sheet goes the rest of the way, otherwise it
 * springs back; either way from where the finger left it. */
static void sheet_pull_end(drm_disp_t *d, int dy)
{
    const int H = d->height;
    int travel = g_pull.dir > 0 ? dy : -dy;
    int commit = travel >= EDGE_PULL_PX;
    int from = g_pull.vis;
    int to = commit != g_pull.closing ? H : 0;
    int ms = SHEET_SLIDE_MS * abs(to - from) / H;
    uint32_t t0 = millis();

    if (!g_pull.active) return;
    if (ms < 60) ms = 60;
    for (;;) {
        float t = (float)(millis() - t0) / (float)ms;
        if (t > 1.f) t = 1.f;
        float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
        sheet_pull_draw(d, from + (int)((float)(to - from) * e + (to > from ? .5f : -.5f)));
        if (t >= 1.f) break;
    }
    g_pull.active = 0;
    if (!commit) return;
    if (g_pull.closing) {
        subpage_close();
    } else if (subpage_open(g_pull.which == SHEET_CONTROL ? "control.html" : "sms.html")) {
        g_sheet = g_pull.which;
    }
    invalidate_render_html_cache();
    render(d, active_page_path());
}

/* Edge swipe back on a second-level page: a drag that starts on the left edge
 * moves the page right with the finger and uncovers, slightly dimmed, the page
 * it returns to (g_slide_a: the page as it was, g_slide_b: the one beneath).
 * Past BACK_COMMIT_PX, or on a quick flick, it slides off and goes back;
 * otherwise it slides back into place. Pulled sheets close by their own pull. */
#define BACK_EDGE_PX   24
#define BACK_COMMIT_PX 100
#define BACK_FLICK_MS  250
#define BACK_SLIDE_MS  220
static struct { int active, x; } g_back;

static int back_pull_ok(int down_x)
{
    return g_subpage[0] && !g_sheet && !g_modal && g_sms_open < 0 && down_x <= BACK_EDGE_PX;
}

/* RGB565 run scaled by k/256: the uncovered page is dimmed toward black. */
static void dim565(uint16_t *dst, const uint16_t *src, int n, int k)
{
    for (int i = 0; i < n; i++) {
        unsigned v = src[i];
        dst[i] = (uint16_t)(((((v >> 11) & 31) * k >> 8) << 11) | ((((v >> 5) & 63) * k >> 8) << 5) | ((v & 31) * k >> 8));
    }
}

/* The page shifted right by off over the dimmed page beneath. Snapshots are
 * physical frames, so on the rotated U60 panel the shift runs the other way. */
static void back_draw(drm_disp_t *d, int off)
{
    const int W = d->width, H = d->height;
    if (W * H > 320 * 480) return;
    if (off < 0) off = 0;
    if (off > W) off = W;
    int k = 256 - 80 * (W - off) / W;             /* darker while mostly covered */
    int pin = wp_pinned(d);
    for (int y = 0; y < H; y++) {                  /* logical rows, into g_next */
        uint16_t *row = &g_next[(size_t)y * W], tmp[320];
        snap_row(d, g_slide_a, y, tmp);
        if (y < DEVUI_EXT_STATUSBAR_H) {           /* the status bar stays put */
            memcpy(row, tmp, (size_t)W * sizeof *row);
            continue;
        }
        memcpy(row + off, tmp, (size_t)(W - off) * sizeof *row);   /* the page moved right by off */
        if (pin && off > 0 && !html_view_bg_fix_row(g_slide_a, row + off, y, off, W - off, -off, 0)) {
            const uint16_t *wp = &g_wpl[(size_t)y * 320];
            wp_key(row + off, row + off, W - off, wp, wp + off);
        }
        snap_row(d, g_slide_b, y, tmp);
        dim565(row, tmp, off, k);
    }
    next_show(d, 0, H - 1);
    g_back.x = off;
}

static int back_pull_begin(drm_disp_t *d)
{
    char sub[sizeof g_subpage], sub_path[sizeof g_subpage_path];
    int scroll = g_scroll;
    if ((size_t)d->pitch_px * d->height > 320 * 480) return 0;
    snprintf(sub, sizeof sub, "%s", g_subpage);
    snprintf(sub_path, sizeof sub_path, "%s", g_subpage_path);
    sheet_snapshot(d, g_slide_a);
    g_render_quiet = 1;
    subpage_close();
    render(d, active_page_path());
    sheet_snapshot(d, g_slide_b);
    g_render_quiet = 0;
    snprintf(g_subpage, sizeof g_subpage, "%s", sub);   /* back where we were until the release */
    snprintf(g_subpage_path, sizeof g_subpage_path, "%s", sub_path);
    g_scroll = scroll;
    invalidate_render_html_cache();
    g_back.active = 1;
    g_back.x = 0;
    return 1;
}

static void back_pull_end(drm_disp_t *d, int dx, int quick)
{
    const int W = d->width;
    int commit = dx >= BACK_COMMIT_PX || (quick && dx >= 30);
    int from = g_back.x, to = commit ? W : 0;
    int ms = BACK_SLIDE_MS * abs(to - from) / W;
    uint32_t t0 = millis();

    if (!g_back.active) return;
    if (ms < 60) ms = 60;
    for (;;) {
        float t = (float)(millis() - t0) / (float)ms;
        if (t > 1.f) t = 1.f;
        float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
        back_draw(d, from + (int)((float)(to - from) * e + (to > from ? .5f : -.5f)));
        if (t >= 1.f) break;
    }
    g_back.active = 0;
    if (!commit) return;
    subpage_close();
    g_modal = 0;
    g_sms_open = -1;
    invalidate_render_html_cache();
    render(d, active_page_path());
}

static void capture_fb(drm_disp_t *d);   /* fwd: fb snapshot for modal overlay */
static void render_sms_overlay(drm_disp_t *d, int restore_background);

static void maybe_dump_fb(drm_disp_t *d)
{
    if (access("/tmp/u60-dumpfb", F_OK) != 0) return;
    FILE *f = fopen("/tmp/fb.dump", "wb");
    if (!f) return;
    const int W = d->width, H = d->height, pp = d->pitch_px;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t px = fb_get(d, x, y);
            fwrite(&px, sizeof px, 1, f);
        }
    }
    fclose(f);
}

static void live_render(const char *path);

static void render(drm_disp_t *disp, const char *path)
{
    if (g_live_run && !g_render_quiet && !g_modal && g_sms_open < 0) { live_render(path); return; }
    tx_wait();
    g_scrollbuf_ok = 0;   /* the page may have changed: scroll from a fresh buffer */
    const char *html = page_html(path);
    if (!html) return;
    int special_page = strstr(path, "menu.html") || strstr(path, "lockscreen.html");
    int scroll = special_page ? 0 : g_scroll;
    size_t html_len = strlen(html);
    int is_speedtest = path_is_speedtest(path);
    int has_charts = strstr(path, "charts.html") != NULL || is_speedtest;
    char css_path[200];
    snprintf(css_path, sizeof css_path, "%s/style.css", UI_DIR);
    long long css_mtime = file_mtime_ns(css_path);
    if (is_speedtest) {
        snprintf(css_path, sizeof css_path, "%s/speedtest.css", UI_DIR);
        css_mtime ^= file_mtime_ns(css_path);
    }
    int cacheable = html_len > 0 && html_len < sizeof(g_render_html_cache);
    int reuse = cacheable && !has_charts &&
                 g_render_html_cache_scroll == scroll &&
                 g_render_html_cache_modal == g_modal &&
                 g_render_html_cache_sms_open == g_sms_open &&
                 g_render_html_cache_lock == g_lock_state &&
                 g_render_html_cache_css_mtime == css_mtime &&
                 g_render_html_cache_len == html_len &&
                 !strcmp(g_render_html_cache_path, path) &&
                 !strncmp(g_render_html_cache, html, html_len);
    size_t fb_px = (size_t)disp->pitch_px * disp->height;
    if (fb_px > sizeof g_render_base / sizeof g_render_base[0]) reuse = cacheable = 0;
    html_view_set_scroll(scroll);
    if (reuse) {
        memcpy(disp->fb, g_render_base, fb_px * sizeof(uint16_t));
        html_view_bg_copy(disp->fb, g_render_base);
    } else {
        g_page_h = html_view_render_html(html);
        if (cacheable) {
            memcpy(g_render_html_cache, html, html_len + 1);
            g_render_html_cache_len = html_len;
            snprintf(g_render_html_cache_path, sizeof g_render_html_cache_path, "%s", path);
            g_render_html_cache_scroll = scroll;
            g_render_html_cache_modal = g_modal;
            g_render_html_cache_sms_open = g_sms_open;
            g_render_html_cache_lock = g_lock_state;
            g_render_html_cache_css_mtime = css_mtime;
            memcpy(g_render_base, disp->fb, fb_px * sizeof(uint16_t));
            html_view_bg_copy(g_render_base, disp->fb);
        } else {
            invalidate_render_html_cache();
        }
    }
    draw_charts();      /* native polylines into any #chart-* placeholders */
    draw_dials();       /* gauges, home ring and icons into their placeholders */
    draw_func_icons();  /* function-list icons */
    if (!g_lock_state && (is_speedtest || g_st_home_open)) draw_speedtest_widgets();
    draw_native_statusbar();
    draw_sms_icon();    /* native envelope in the status bar when unread SMS */
    if (strstr(path, "menu.html")) draw_power_menu();   /* round power buttons */
    if (g_lock_state == 1) draw_lock_icon();   /* locked preview: lock glyph */
    int H = disp->height, maxs = g_page_h - H;
    (void)maxs;
    if (g_modal) {                                    /* dim page + second-level dialog */
        html_view_fill_rect(0, 28, disp->width, H - 28, 0, 0, 0, 150);
        capture_fb(disp);                             /* snapshot page+dim for fast modal refresh */
        html_view_render_overlay(modal_html());
    } else if (g_sms_open >= 0) {                     /* dim page + SMS detail dialog */
        html_view_fill_rect(0, 28, disp->width, H - 28, 0, 0, 0, 150);
        capture_fb(disp);
        render_sms_overlay(disp, 0);
    }
    if (g_render_quiet) return;
    drm_disp_present(disp, 0, disp->height - 1);
    maybe_dump_fb(disp);
}

static void render_ext_view(drm_disp_t *disp, devui_ext_t *ext)
{
    tx_wait();
    devui_ext_render(ext, disp);
    draw_native_statusbar();
    draw_sms_icon();
    drm_disp_dirty(disp, 0, 0, disp->width - 1, disp->height - 1);
    maybe_dump_fb(disp);
}

/* Offscreen page bitmaps (logical, no rotation) for slide transitions.
 * During a drag: g_bufA = left page, g_bufB = right page (windowed by offset o:
 * window column x shows [left|right][x+o], o in 0..W). */
static uint16_t g_bufA[320 * 480], g_bufB[320 * 480];

/* Status bar stays pinned during a swipe (only the content below slides), so
 * we keep the already-rendered native status bar rows untouched and only
 * update the content area beneath it. */
#define SBAR_H DEVUI_EXT_STATUSBAR_H
/* Keep only a tiny hysteresis so page/scroll drags respond immediately,
 * without turning every resting-finger jitter into a swipe. */
#define DRAG_START_PX 2
#define DRAG_CANCEL_PX 8
#define SCROLL_INERTIA_MIN_V 180.0f
#define SCROLL_INERTIA_MAX_V 2200.0f
#define SCROLL_INERTIA_DECEL 3600.0f
#define STATE_RENDER_THROTTLE_MS 1500
#define STATE_RENDER_IDLE_THROTTLE_MS 2500
#define EXT_BACK_EDGE_PX 24
#define EXT_BACK_COMMIT_PX 56
#define EXT_BACK_MAX_DY 44
#define TOUCH_FRAME_MIN_MS 16
#define TOUCH_QUIET_MS 350
#define QUEUED_SWIPE_PX 52
#define QUEUED_SCROLL_PX 28
#define IDLE_SLEEP_ON_US 8000
#define IDLE_SLEEP_OFF_US 30000
#define MOTION_SLEEP_US 2000

static int motion_frame_due(uint32_t now, uint32_t *last)
{
    if (*last == 0 || now - *last >= TOUCH_FRAME_MIN_MS) {
        *last = now;
        return 1;
    }
    return 0;
}

static float clamp_scroll_v(float v)
{
    if (v >  SCROLL_INERTIA_MAX_V) return  SCROLL_INERTIA_MAX_V;
    if (v < -SCROLL_INERTIA_MAX_V) return -SCROLL_INERTIA_MAX_V;
    return v;
}

static void compose_frame(drm_disp_t *d, int o)
{
    const int W = d->width, H = d->height;
    int left;
    if (o < 0) o = 0; if (o > W) o = W;
    left = W - o;
    if (W * H > 320 * 480) return;
    int pin = wp_pinned(d);
    for (int y = SBAR_H; y < H; y++) {   /* into g_next while the last frame is sent */
        const uint16_t *lr = g_bufA + (size_t)y * W, *rr = g_bufB + (size_t)y * W;
        uint16_t *row = &g_next[(size_t)y * W];
        memcpy(row, lr + o, (size_t)left * sizeof *row);
        memcpy(row + left, rr, (size_t)o * sizeof *row);
        if (pin) {   /* the pages slide, the wallpaper and the glass over it stay */
            const uint16_t *wp = &g_wpl[(size_t)y * 320];
            if (!html_view_bg_fix_row(g_bufA, row, y, 0, left, o, 0)) wp_key(row, row, left, wp + o, wp);
            if (!html_view_bg_fix_row(g_bufB, row + left, y, left, o, -left, 0)) wp_key(row + left, row + left, o, wp, wp + left);
        }
    }
    tx_wait();
    html_view_bg_drop(d->fb);
    for (int y = SBAR_H; y < H; y++) fb_put_row(d, y, 0, &g_next[(size_t)y * W], W);
    /* Only the content area below the status bar changes during a horizontal swipe. */
    tx_send(d, SBAR_H, H - 1);
    maybe_dump_fb(d);
}

/* Render the page pair for a drag direction into A(left)/B(right). dir>0 = next.
 * The current page keeps its scroll; the target page comes in at the top. */
static int render_page_to_pair_buf(uint16_t *buf, const char *path, const char *html, int scroll)
{
    if (!html) return 0;
    int h = html_view_render_to_scroll(buf, html, scroll);
    html_view_target_begin(buf, scroll);
    draw_charts();
    draw_dials();
    draw_func_icons();
    if (!g_lock_state && (path_is_speedtest(path) || g_st_home_open)) draw_speedtest_widgets();
    draw_native_statusbar();
    draw_sms_icon();
    html_view_target_end();
    return h;
}

/* render() while the clip plays: the page off screen, then handed over. */
static void live_render(const char *path)
{
    const char *html = page_html(path);
    if (!html) return;
    int scroll = strstr(path, "menu.html") || strstr(path, "lockscreen.html") ? 0 : g_scroll;
    g_scrollbuf_ok = 0;
    html_view_set_scroll(scroll);
    g_page_h = render_page_to_pair_buf(g_live_back, path, html, scroll);
    html_view_target_begin(g_live_back, scroll);
    if (strstr(path, "menu.html")) draw_power_menu();
    if (g_lock_state == 1) draw_lock_icon();
    html_view_target_end();
    invalidate_render_html_cache();
    pthread_mutex_lock(&g_lp_mu);
    uint16_t *t = g_live_next;
    g_live_next = g_live_back;
    g_live_back = t;
    html_view_live_snap(g_live_next);
    g_lp_new = 1;
    pthread_cond_broadcast(&g_lp_cv);
    pthread_mutex_unlock(&g_lp_mu);
}

/* The current page for a swipe is the frame on screen (unrotated), unless a
 * dialog or toast covers it; only the target page needs rendering, so the
 * page follows the finger sooner. */
static void prep_current(const drm_disp_t *d, uint16_t *buf)
{
    const int W = d->width, H = d->height;
    if (g_modal || g_sms_open >= 0 || g_toast[0] || W * H > 320 * 480) {
        html_view_set_scroll(g_scroll);
        render_page_to_pair_buf(buf, g_pages[g_cur], page_html(g_pages[g_cur]), g_scroll);
        return;
    }
    for (int y = 0; y < H; y++) {
        uint16_t *dp = buf + (size_t)y * W;
        for (int x = 0; x < W; x++) dp[x] = fb_get(d, x, y);
    }
    html_view_bg_copy(buf, d->fb);   /* and what of the ground shows in it */
}

/* The target page as it shows once it is current (its page dot lit). */
static int g_pair_h;   /* page height of the target prep_target rendered */

static void prep_target(uint16_t *buf, int target)
{
    int cur = g_cur;
    g_cur = target;
    html_view_set_scroll(0);
    g_pair_h = render_page_to_pair_buf(buf, g_pages[target], page_html(g_pages[target]), 0);
    g_cur = cur;
}

static void prep_pair(const drm_disp_t *d, int target, int dir)
{
    if (dir > 0) {   /* next: left=current, right=target */
        prep_current(d, g_bufA);
        prep_target(g_bufB, target);
    } else {         /* prev: left=target, right=current */
        prep_current(d, g_bufB);
        prep_target(g_bufA, target);
    }
    html_view_set_scroll(g_scroll);
}

/* A committed swipe ends with the target page full on screen, just as
 * prep_target rendered it into buf (the page rendered last, so taps already
 * hit it): keep that frame rather than render the page again, so a quick run
 * of swipes is not held up. The next refresh brings newer data. */
static void swipe_land(drm_disp_t *d, const uint16_t *buf, int target)
{
    g_scrollbuf_ok = 0;
    g_cur = target;
    g_scroll = 0;
    if (g_pair_h > 0) g_page_h = g_pair_h;
    html_view_set_scroll(0);
    html_view_bg_copy(d->fb, buf);
    invalidate_render_html_cache();
}

/* Settle the offset from o0 to o1, eased out; the panel paces the frames. */
static void anim_o(drm_disp_t *d, int o0, int o1)
{
    int dist = o1 > o0 ? o1 - o0 : o0 - o1;
    uint32_t dur = 90 + (uint32_t)dist / 4, t0 = millis();
    for (;;) {
        float t = (float)(millis() - t0) / (float)dur;
        if (t >= 1.f) break;
        float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
        compose_frame(d, o0 + (int)((float)(o1 - o0) * e + (o1 > o0 ? .5f : -.5f)));
    }
    compose_frame(d, o1);
}

/* Smooth vertical scrolling: the full page is pre-rendered once into g_scrollbuf
 * (logical, unrotated); each drag frame just blits the visible window + scrollbar
 * without re-parsing/re-layout, so it keeps up like the horizontal swipe. */
#define SCROLLMAX 2048
static uint16_t g_scrollbuf[320 * SCROLLMAX];
static int g_scroll_h;

/* The page in g_scrollbuf is kept for the next scroll of the same page as
 * long as nothing rendered the page meanwhile, so a run of flicks starts
 * each one at once instead of rendering the whole page again. */
static char g_scrollbuf_path[200];
static int g_scrollbuf_bufh, g_scrollbuf_rh;

/* Render path into g_scrollbuf (bufh rows) unless it is there already;
 * returns its content height. */
static int scrollbuf_prepare(const char *path, int bufh)
{
    if (g_scrollbuf_ok && g_scrollbuf_bufh == bufh && !strcmp(g_scrollbuf_path, path)) return g_scrollbuf_rh;
    const char *hp = page_html(path);
    if (!hp) return g_page_h;
    g_scrollbuf_rh = html_view_render_tall(g_scrollbuf, hp, bufh);
    snprintf(g_scrollbuf_path, sizeof g_scrollbuf_path, "%s", path);
    g_scrollbuf_bufh = bufh;
    g_scrollbuf_ok = 1;
    return g_scrollbuf_rh;
}

static void scroll_blit(drm_disp_t *d, int scroll)
{
    const int W = d->width, Hh = d->height;
    if (W * Hh > 320 * 480) return;
    int pin = wp_pinned(d);
    for (int y = SBAR_H; y < Hh; y++) {   /* into g_next while the last frame is sent */
        int sy = y + scroll;
        uint16_t *row = &g_next[(size_t)y * W];
        if (sy < 0 || sy >= g_scroll_h) { memset(row, 0, (size_t)W * sizeof *row); continue; }
        memcpy(row, &g_scrollbuf[(size_t)sy * W], (size_t)W * sizeof *row);
        if (pin && !html_view_bg_fix_row(g_scrollbuf, row, y, 0, W, 0, scroll))   /* the wallpaper stays */
            wp_key(row, row, W, html_view_wallpaper_row(sy), &g_wpl[(size_t)y * 320]);
    }
    html_view_set_scroll(scroll);
    /* the page buffer has no native drawings: add them for this window, into
     * g_next too (still while the last frame is sent), below the pinned
     * status bar */
    html_view_target_begin(g_next, scroll);
    html_view_set_clip_top(SBAR_H);
    draw_charts();
    draw_dials();
    draw_func_icons();
    if (!g_lock_state && (path_is_speedtest(active_page_path()) || g_st_home_open)) draw_speedtest_widgets();
    html_view_set_clip_top(0);
    html_view_target_end();
    tx_wait();
    html_view_bg_drop(d->fb);
    for (int y = SBAR_H; y < Hh; y++) fb_put_row(d, y, 0, &g_next[(size_t)y * W], W);
    /* The native status bar is pinned; dragging only changes the content below it. */
    tx_send(d, SBAR_H, Hh - 1);
}

/* fb snapshot for fast overlay refresh (modal toggles, segmented-control drag):
 * avoids re-laying-out the whole page on every interaction. */
static uint16_t g_overbg[320 * 480];
static void capture_fb(drm_disp_t *d)
{
    memcpy(g_overbg, d->fb, (size_t)d->pitch_px * d->height * sizeof(uint16_t));
    html_view_bg_copy(g_overbg, d->fb);
}
static void restore_fb(drm_disp_t *d)
{
    tx_wait();
    memcpy(d->fb, g_overbg, (size_t)d->pitch_px * d->height * sizeof(uint16_t));
    html_view_bg_copy(d->fb, g_overbg);
}

static int sms_scroll_metrics(void)
{
    int bx, by, bw, bh;
    int vx, vy, vw, vh;
    int old_scroll = g_sms_scroll;

    if (html_view_rect("#smsview", &vx, &vy, &vw, &vh) &&
        html_view_rect("#smsbody", &bx, &by, &bw, &bh)) {
        g_sms_view_x = vx;
        g_sms_view_y = vy;
        g_sms_view_w = vw;
        g_sms_view_h = vh;
        g_sms_scroll_max = bh > vh ? bh - vh : 0;
    } else {
        g_sms_view_x = g_sms_view_y = g_sms_view_w = g_sms_view_h = 0;
        g_sms_scroll_max = 0;
    }
    g_sms_scroll = clampi(g_sms_scroll, 0, g_sms_scroll_max);
    return g_sms_scroll != old_scroll;
}

static int sms_point_in_view(int x, int y)
{
    return g_sms_view_w > 0 && g_sms_view_h > 0 &&
           x >= g_sms_view_x && x < g_sms_view_x + g_sms_view_w &&
           y >= g_sms_view_y && y < g_sms_view_y + g_sms_view_h;
}

/* Redraw only the SMS dialog while dragging; the dimmed page remains cached. */
static void render_sms_overlay(drm_disp_t *d, int restore_background)
{
    if (restore_background) restore_fb(d);
    html_view_render_overlay(sms_modal_html());
    if (sms_scroll_metrics()) {
        restore_fb(d);
        html_view_render_overlay(sms_modal_html());
        (void)sms_scroll_metrics();
    }
    if (g_sms_scroll_max > 0 && g_sms_view_h > 0) {
        int content_h = g_sms_view_h + g_sms_scroll_max;
        int thumb_h = g_sms_view_h * g_sms_view_h / content_h;
        int travel, thumb_y;
        if (thumb_h < 18) thumb_h = 18;
        if (thumb_h > g_sms_view_h) thumb_h = g_sms_view_h;
        travel = g_sms_view_h - thumb_h;
        thumb_y = g_sms_view_y + g_sms_scroll * travel / g_sms_scroll_max;
        html_view_fill_round_rect(g_sms_view_x + g_sms_view_w - 3, thumb_y,
                                  3, thumb_h, 2, 0x8a, 0x92, 0x9d, 190);
    }
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}

/* Redraw the modal over the cached dimmed page (fast: no page relayout). */
static void render_modal_overlay(drm_disp_t *d)
{
    restore_fb(d);
    html_view_render_overlay(modal_html());
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}

static void handle_modal_tap(drm_disp_t *disp, int x, int y, uint32_t now,
                             int *need_render, int *animating)
{
    const char *act = html_view_click((float)x, (float)y);

    if (g_modal == 4) {
        if (strncmp(act, "act:", 4)) return;
        const char *a = act + 4;
        if (!strcmp(a, "sigread")) {
            g_sig_read = !g_sig_read;
            (void)datad_set_signal_read(g_sig_read);
            signal_async_publish(NULL, 0);
            save_settings();
            snprintf(g_toast, sizeof g_toast, "信令读取已%s", g_sig_read ? "开启" : "关闭");
            g_toast_until = now + 1600;
            invalidate_render_html_cache();
            *need_render = 1;
        } else if (!strcmp(a, "sigparse")) {
            g_sig_parse = !g_sig_parse;
            (void)datad_set_signal_parse(g_sig_parse);
            signal_async_publish(NULL, 0);
            save_settings();
            rescan_pages_keep_current();
            invalidate_render_html_cache();
            snprintf(g_toast, sizeof g_toast, "信令解析已%s", g_sig_parse ? "开启" : "关闭");
            g_toast_until = now + 1600;
            *need_render = 1;
        } else if (!strcmp(a, "closemodal")) {
            g_modal = 0;
            *need_render = 1;
        }
        return;
    }

    char *sel = g_modal == 1 ? g_sel_sa : g_modal == 2 ? g_sel_nsa : g_sel_lte;
    const char *uni = g_modal == 1 ? g_uni_sa : g_modal == 2 ? g_uni_nsa : g_uni_lte;
    if (!strncmp(act, "act:", 4)) {
        const char *a = act + 4;
        if (!strncmp(a, "bsa:", 4) || !strncmp(a, "bnsa:", 5) || !strncmp(a, "blte:", 5)) {
            band_toggle(sel, 256, strchr(a, ':') + 1);
            render_modal_overlay(disp);
            *animating = 1;
        } else if (!strcmp(a, "mall")) {
            if (!strcmp(sel, uni)) sel[0] = 0;
            else snprintf(sel, 256, "%s", uni);
            render_modal_overlay(disp);
            *animating = 1;
        } else if (!strcmp(a, "minv")) {
            char u[256], out[256];
            int o = 0;
            snprintf(u, sizeof u, "%s", uni);
            for (char *tk = strtok(u, ","); tk; tk = strtok(NULL, ","))
                if (!band_in(sel, tk))
                    o += snprintf(out + o, sizeof out - o, "%s%s", o ? "," : "", tk);
            out[o] = 0;
            snprintf(sel, 256, "%s", out);
            render_modal_overlay(disp);
            *animating = 1;
        } else if (!strcmp(a, "mapply")) {
            if (sel[0]) {
                char params[300], cmd[360];
                snprintf(params, sizeof params, "{\"bands\":\"%s\"}", sel);
                if (g_modal == 3)
                    snprintf(cmd, sizeof cmd,
                             "ubus call zte_nwinfo_api nwinfo_set_lte_ext_band '{\"lte_band\":\"%s\"}' >/dev/null 2>&1",
                             sel);
                else
                    snprintf(cmd, sizeof cmd,
                             "ubus call zte_nwinfo_api nwinfo_set_nrbandlock '{\"nr5g_type\":\"%s\",\"nr5g_band\":\"%s\"}' >/dev/null 2>&1",
                             g_modal == 1 ? "0" : "1", sel);
                datad_control_async(g_modal == 3 ? "band.set_lte" :
                                    g_modal == 1 ? "band.set_nr_sa" : "band.set_nr_nsa",
                                    params, cmd);
            }
            snprintf(g_toast, sizeof g_toast, "锁频成功");
            g_toast_until = now + 1600;
            g_modal = 0;
            *need_render = 1;
        }
    } else {
        g_modal = 0;   /* band-lock dialogs keep their existing tap-outside behavior */
        *need_render = 1;
    }
}

/* Draw the sliding segmented-control highlight box at the finger, over cached fb.
 * n = number of cells. */
static void seg_box(drm_disp_t *d, int sx, int sy, int sw, int sh, int n, int fx)
{
    restore_fb(d);
    int cw = sw / n, bx = fx - cw / 2;
    if (bx < sx) bx = sx; if (bx > sx + sw - cw) bx = sx + sw - cw;
    html_view_fill_rect(bx, sy, cw, sh, 0x2f, 0x6f, 0xe0, 150);
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}

static void als_close(void);

/* Set backlight from a tap/drag x within the brightness bar [bx, bx+bw). */
static void set_bright_x(int x, int bx, int bw)
{
    if (bw <= 0) return;
    if (g_auto_bright) { g_auto_bright = 0; als_close(); }   /* a hand on the slider wins */
    int f = (x - bx) * 100 / bw; if (f < 3) f = 3; if (f > 100) f = 100;
    int m = backlight_max(); if (m <= 0) m = 255;
    backlight_set(f * m / 100);
}

/* Glass transparency from a tap/drag x within its bar, in steps of 5; the
 * page re-renders with it while dragging, the conf is written on release. */
static void set_glass_x(int x, int bx, int bw)
{
    if (bw <= 0) return;
    g_glass = clampi(((x - bx) * 100 / bw + 2) / 5 * 5, 0, 100);
    apply_glass();
    invalidate_render_html_cache();
}

/* Glass blur from a tap/drag x within its bar, stepless (0..100). */
static void set_blur_x(int x, int bx, int bw)
{
    if (bw <= 0) return;
    g_glass_blur = clampi((x - bx) * 100 / bw, 0, 100);
    html_view_set_glass_blur(g_glass_blur);
    invalidate_render_html_cache();
}

/* ---- auto brightness (TopFlow): the ambient light sensor, through datad ----
 * datad switches the sensor (ambient_light.set) and reports its level in
 * /state.ambient_light, refreshed with every snapshot. The sensor runs only
 * while the screen is on and the setting is on. The reading is smoothed,
 * mapped to a level on a log scale, and the backlight steps toward it only
 * once the gap is noticeable, so it settles instead of hunting. */
#define ALS_STEP_MS    80        /* one backlight step per this many ms */
#define ALS_START_PCT  6         /* start moving when this far off (percent) */
#define ALS_RESEND_MS  3000      /* re-send when datad still reports the other state */
static int g_als_want = -1;      /* last state sent to datad, -1 none yet */
static uint32_t g_als_sent_at;
static float g_als_lux = -1.f;
static int g_als_moving;
static uint32_t g_als_step_at;

static int als_supported(void) { return dev_is_tf(); }

static void als_set(int on)
{
    datad_control_async("ambient_light.set", on ? "{\"enabled\":true}" : "{\"enabled\":false}", NULL);
    g_als_want = on;
    g_als_sent_at = millis();
    g_als_lux = -1.f;
    g_als_moving = 0;
}

static void als_close(void)
{
    if (g_als_want == 1) als_set(0);
}

/* Sensor on exactly when it is wanted; also corrects datad after a restart. */
static void als_sync(void)
{
    if (!als_supported()) return;
    int want = g_auto_bright && backlight_is_on(), on;
    (void)data_ambient_light(&on);
    if (want != g_als_want ||
        (on >= 0 && on != want && (uint32_t)(millis() - g_als_sent_at) >= ALS_RESEND_MS))
        als_set(want);
}

/* Backlight percent for a reading, interpolated on log10(lux). */
static int als_percent(float lux)
{
    static const float pt_lux[] = { 1.f, 10.f, 50.f, 200.f, 800.f, 3000.f };
    static const float pt_pct[] = { 6.f, 15.f, 30.f, 55.f, 80.f, 100.f };
    const int n = (int)(sizeof pt_lux / sizeof pt_lux[0]);
    if (lux <= pt_lux[0]) return (int)pt_pct[0];
    if (lux >= pt_lux[n - 1]) return (int)pt_pct[n - 1];
    for (int i = 1; i < n; i++) {
        if (lux > pt_lux[i]) continue;
        float a = log10f(pt_lux[i - 1]), b = log10f(pt_lux[i]);
        float t = (log10f(lux) - a) / (b - a);
        return (int)(pt_pct[i - 1] + (pt_pct[i] - pt_pct[i - 1]) * t + .5f);
    }
    return (int)pt_pct[n - 1];
}

/* Smooth the reading and step the backlight. Called every loop pass. */
static void als_poll(uint32_t now)
{
    int on;
    long raw;
    if (g_als_want != 1 || (int32_t)(now - g_als_step_at) < ALS_STEP_MS) return;
    g_als_step_at = now;
    raw = data_ambient_light(&on);
    if (on != 1 || raw < 0) return;
    g_als_lux = g_als_lux < 0.f ? (float)raw : g_als_lux * .9f + (float)raw * .1f;   /* ~0.8 s */
    int m = backlight_max(); if (m <= 0) m = 255;
    int target = als_percent(g_als_lux) * m / 100, cur = backlight_get();
    int gap = target - cur, agap = gap < 0 ? -gap : gap;
    if (!g_als_moving && agap * 100 < ALS_START_PCT * m) return;
    g_als_moving = agap > m / 100;
    if (!g_als_moving) return;
    int step = agap / 6; if (step < 1) step = 1;
    backlight_set(cur + (gap > 0 ? step : -step));
}

/* Screen off: backlight to 0 and a black frame. The panel itself stays awake:
 * on the U60 the touch controller shares the display driver chip, and with the
 * panel asleep (DPMS off) the touch driver resets the LCD and finally reboots. */
static void screen_off(drm_disp_t *d)
{
    als_close();
    backlight_fade_off();
    tx_wait();
    memset(d->fb, 0, (size_t)d->pitch_px * d->height * sizeof(uint16_t));
    html_view_bg_drop(d->fb);
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}
/* Screen on: render, then "warm up" the command-mode panel with a few more
 * frame pushes while the backlight is still 0; this drives it past the
 * idle-exit transient invisibly, then fades the backlight up from 0. Each push
 * waits for the panel (~33 ms), so the pushes pace themselves. */
#define SCREEN_ON_WARM_PUSHES 2
static void screen_warm_up(drm_disp_t *d)
{
    for (int k = 0; k < SCREEN_ON_WARM_PUSHES; k++)
        drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}

static void screen_on(drm_disp_t *d, const char *path)
{
    if (g_charge_boot) render_charge_boot(d);
    else               render(d, path);       /* backlight still 0 from screen_off */
    screen_warm_up(d);
    backlight_fade_on();   /* 0 -> user level */
    als_sync();
}

static void screen_on_ext(drm_disp_t *d, devui_ext_t *ext)
{
    render_ext_view(d, ext);                  /* backlight still 0 from screen_off */
    screen_warm_up(d);
    backlight_fade_on();
    als_sync();
}

int main(void)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    paths_init();
    g_charge_boot = boot_is_charge_mode();
    device_init();
    if (!g_charge_boot) {
        scan_pages();
        if (g_npages == 0) { fprintf(stderr, "no pages in %s\n", UI_DIR); return 1; }
    }

    drm_disp_t disp;
    if (drm_disp_init(&disp) != 0) { fprintf(stderr, "drm init failed\n"); return 1; }
    html_view_init(disp.fb, disp.width, disp.height, disp.pitch_px, g_panel_rot, resolve_ui_font());
    html_view_set_uidir(UI_DIR);

    touch_input_t touch;
    int touch_ok = touch_input_init(&touch, disp.width, disp.height) == 0;
    key_input_t key;     key_input_init(&key);
    backlight_init();
    (void)data_backend_init();
    (void)data_backend_commit_latest();
    (void)device_update(data_model_tag());   /* pages are rescanned below */
    apply_panel_rotation(&disp, &touch);

    if (!g_charge_boot && !touch_ok && boot_has_external_power()) {
        fprintf(stderr, "boot: forcing charge UI (touch missing, external power present)\n");
        g_charge_boot = 1;
    }

    int menu = 0, prev_press = 0, prev_lock = 0, was_on = 1, down_x = 0, down_y = 0;
    int dragging = 0, drag_dir = 0, drag_target = 0;
    int scroll_dir = 0, scroll_start = 0;
    int sms_dragging = 0, sms_scroll_start = 0;
    int scroll_inertia = 0, scroll_track_valid = 0;
    int scroll_track_pos = 0;
    uint32_t drag_down_ms = 0;
    uint32_t scroll_track_ms = 0;
    uint32_t last_motion_frame = 0;
    float scroll_v = 0.0f, scroll_pos = 0.0f;
    int sliding = 0, bar_x = 0, bar_w = 0;   /* brightness slider drag */
    int edge_pull = 0;                       /* vertical drag that pulls a sheet */
    int back_pull = 0;                       /* drag from the left edge back out of a subpage */
    int lock_holding = 0;                    /* finger down on the locked preview */
    uint32_t lock_down_ms = 0;
    int segging = 0, seg_x = 0, seg_y = 0, seg_w = 0, seg_h = 0;   /* segmented control drag */
    int seg_which = 0, seg_n = 4;   /* 1 = net mode, 2 = auto-off, 3 = refresh */
    const int W = disp.width, H = disp.height;
    devui_ext_t ext;
    memset(&ext, 0, sizeof ext);
    ext.srv_fd = -1;
    int ext_ok = 0;
    if (!g_charge_boot) {
        ext_ok = devui_ext_init(&ext, W, H) == 0;
        if (!ext_ok) fprintf(stderr, "warning: external display socket disabled\n");
    }
    char menu_path[300], lock_path[300];
    snprintf(menu_path, sizeof menu_path, "%s/menu.html", UI_DIR);
    snprintf(lock_path, sizeof lock_path, "%s/lockscreen.html", UI_DIR);

    /* lock pad takes priority over the power menu and the normal pages */
    /* lock states: 1=preview (page 1), 2=setup pad, 3=unlock pad */
    #define CUR_PATH (g_lock_state == 1 ? g_pages[0] : g_lock_state ? lock_path : menu ? menu_path : active_page_path())

    load_conf();                          /* restore persisted UI settings */
    html_view_set_text_adapt(g_text_adapt);
    (void)wallpaper_poll();
    mkdir(FUNCTIONS_DIR, 0755);
    speedtest_apply_saved_prefs();
    speedtest_poll(millis());
    if (!g_charge_boot) rescan_pages_keep_current();
    g_pages_dir_mtime = ui_scan_mtime_ns();
    g_pages_scan_at = millis();
    (void)datad_set_refresh_ms(g_refresh_ms);
    (void)datad_set_signal_read(g_sig_read);
    (void)datad_set_signal_parse(g_sig_parse);
    if (!g_charge_boot)
        signal_async_start();
    if (g_saved_bright >= 0) backlight_set(g_saved_bright);   /* restore brightness */
    if (!g_charge_boot) als_sync();
    if (!g_charge_boot) {
        load_pin();
        aux_start();                          /* page-2 switch states etc., off this thread */
        if (!dev_is_tf() && (usb_pid_is("9057") || usb_pid_is("90b1") || usb_pid_is("90B1")))
            usb_net_watchdog_start();
        enter_lock(0);                        /* boot into the locked preview */
        render(&disp, CUR_PATH);
    } else {
        render_charge_boot(&disp);
    }
    uint32_t last_data = millis(), last_act = millis();
    unsigned datad_generation = data_backend_generation();
    int state_pending = 0;

    /* stuck for 20 s: log where, exit, and start.sh runs the UI again */
    char check_log[200];
    snprintf(check_log, sizeof check_log, "%s/selfcheck.log", g_app_dir);
    selfcheck_start(check_log, 20000);

    while (g_run) {
        selfcheck_beat();
        selfcheck_stage("poll");
        uint32_t now = millis();
        int need_render = 0, animating = 0;
        int live_changed = data_backend_poll(now);
        if (live_changed) screen_settings_pull();
        if (live_changed) state_pending = 1;

        if (ext_ok && g_lock_state && devui_ext_active(&ext)) {
            devui_ext_deactivate(&ext);
            touch_input_clear_taps(&touch);
            need_render = 1;
        }
        if (ext_ok && !g_lock_state) {
            int ext_changed = devui_ext_poll(&ext, now);
            if (ext_changed) {
                dragging = 0; drag_dir = 0; scroll_dir = 0; sliding = 0; segging = 0;
                scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
                menu = 0; g_modal = 0; g_sms_open = -1; subpage_close();
                touch_input_clear_taps(&touch);
                if (devui_ext_active(&ext)) {
                    refresh_status_cache();
                    if (backlight_is_on()) {
                        render_ext_view(&disp, &ext);
                        animating = 1;
                    }
                } else {
                    need_render = 1;
                }
            }
        }

        /* power key */
        int ev = key_input_poll(&key, now);
        if (ev == KEY_EV_SHORT || ev == KEY_EV_LONG) last_act = now;
        if (ev == KEY_EV_SHORT) {
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
            if (backlight_is_on()) {
                screen_off(&disp);
                if (!g_charge_boot) {
                    if (ext_ok) devui_ext_deactivate(&ext);
                    enter_lock(0);   /* power key locks the screen */
                }
            } else {
                if (ext_ok && devui_ext_active(&ext) && !g_lock_state)
                    screen_on_ext(&disp, &ext);
                else
                    screen_on(&disp, g_charge_boot ? NULL : CUR_PATH);
            }
        } else if (ev == KEY_EV_LONG) {
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
            if (ext_ok && devui_ext_active(&ext) && !g_lock_state) {
                if (!backlight_is_on()) screen_on_ext(&disp, &ext);
                else { devui_ext_deactivate(&ext); need_render = 1; }
            } else if (g_charge_boot) {
                if (!backlight_is_on()) screen_on(&disp, NULL);
            } else if (g_lock_state == 1 && !lock_enabled()) {   /* no PIN: the key unlocks */
                g_lock_state = 0; g_lock_hold = 0;
                menu = 1; g_pwr_confirm = 0;
                if (!backlight_is_on()) screen_on(&disp, CUR_PATH);
                else need_render = 1;
            } else if (g_lock_state) {               /* no power menu while locked */
                if (!backlight_is_on()) screen_on(&disp, CUR_PATH);
            } else {
                menu = !menu; g_pwr_confirm = 0;
                if (!backlight_is_on()) screen_on(&disp, CUR_PATH);
                else need_render = 1;
            }
        }

        if (g_charge_boot) {
            if (live_changed && data_backend_commit_latest()) {
                state_pending = 0;
                need_render = 1;
            }
            touch_input_clear_taps(&touch);
            prev_press = 0;
            if (now - last_data >= 120) { need_render = 1; last_data = now; }
            if (need_render && backlight_is_on()) render_charge_boot(&disp);
            was_on = backlight_is_on();
            if (!animating) usleep(backlight_is_on() ? IDLE_SLEEP_ON_US : IDLE_SLEEP_OFF_US);
            continue;
        }

        /* touch: follow-finger swipe (pages) / drag (scroll) + tap actions */
        int x, y, pressed;
        int replay_release = 0;
        selfcheck_stage("touch");
        touch_input_read(&touch, &x, &y, &pressed);

        int on_now = backlight_is_on();
        if (on_now && !was_on) {          /* first frame after waking: drop the wake touch */
            touch_input_clear_taps(&touch);
            pressed = 0;
        }
        if (pressed && on_now) last_act = now;
        if (g_live_run && (pressed || !on_now || !html_view_live_on()))
            live_stop();   /* a touch takes the frame on screen as it is */

        if (!on_now) {
            /* screen off: ignore touch entirely and discard any taps the panel
             * reported while dark, so they can't replay on wake. Power key is the
             * only way to wake (double-tap-to-wake removed). */
            touch_input_clear_taps(&touch);
            pressed = 0;
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
        } else if (g_lock_state == 1) {
            /* locked preview: page 1 with live data, no actions (see enter_lock) */
            if (pressed && !prev_press) {
                down_x = x; down_y = y; lock_down_ms = now; lock_holding = 1;
            } else if (pressed && prev_press && lock_holding) {
                int dx = x - down_x, dy = y - down_y;
                uint32_t need = lock_enabled() ? LOCK_PAD_HOLD_MS : LOCK_HOLD_MS;
                uint32_t held = now - lock_down_ms;
                if (dx * dx + dy * dy > 22 * 22) {
                    lock_holding = 0;                /* a swipe: opens the pad, never unlocks */
                    g_lock_hold = 0;
                    if (lock_enabled()) { g_lock_state = 3; g_pin_entry[0] = 0; g_lock_err = 0; g_scroll = 0; }
                    need_render = 1;
                } else if (held >= need) {
                    lock_holding = 0;
                    g_lock_hold = 0;
                    if (lock_enabled()) { g_lock_state = 3; g_pin_entry[0] = 0; g_lock_err = 0; }
                    else g_lock_state = 0;
                    g_scroll = 0;
                    touch_input_clear_taps(&touch);  /* the holding finger must not tap on release */
                    need_render = 1;
                } else if (motion_frame_due(now, &last_motion_frame)) {
                    g_lock_hold = (int)(held * 100 / need);
                    render(&disp, CUR_PATH);
                    animating = 1;
                }
            } else if (!pressed && prev_press) {
                lock_holding = 0;
                if (g_lock_hold) { g_lock_hold = 0; need_render = 1; }
                touch_input_clear_taps(&touch);
            }
        } else if (g_lock_state) {
            /* PIN pad (3=unlock / 2=setup): tap only, no confirm key; a 4th digit
             * auto-submits. Drain the tap queue so a fast burst of taps all land
             * (the per-digit re-render would otherwise let polls miss presses). */
            int tx, ty;
            while (g_lock_state && touch_input_take_tap(&touch, &tx, &ty)) {
                const char *act = html_view_click((float)tx, (float)ty);
                if (strncmp(act, "act:", 4)) continue;
                const char *a = act + 4;
                if (!strncmp(a, "pin:", 4)) {
                    const char *k = a + 4;
                    int len = (int)strlen(g_pin_entry);
                    if (!strcmp(k, "del")) {
                        if (len > 0) g_pin_entry[len - 1] = 0;
                        g_lock_err = 0;
                    } else if (k[0] >= '0' && k[0] <= '9' && !k[1]) {
                        if (len < 4) { g_pin_entry[len] = k[0]; g_pin_entry[len + 1] = 0; len++; g_lock_err = 0; }
                        if (len == 4) {                       /* auto-submit */
                            if (g_lock_state == 2) {          /* setup: store + enable */
                                save_pin(g_pin_entry);
                                g_pin_entry[0] = 0; g_lock_state = 0;
                            } else if (!strcmp(g_pin_entry, g_pin)) {   /* unlock ok */
                                g_pin_entry[0] = 0; g_lock_err = 0; g_lock_state = 0;
                            } else {                          /* wrong: keep pad, show error */
                                g_pin_entry[0] = 0; g_lock_err = 1;
                            }
                        }
                    }
                    need_render = 1;
                } else if (!strcmp(a, "lockcancel")) {        /* abort PIN setup */
                    g_pin_entry[0] = 0; g_lock_err = 0; g_lock_state = 0; need_render = 1;
                }
            }
        } else if (ext_ok && devui_ext_active(&ext)) {
            if (!pressed) {
                int sx, sy, ex, ey;
                while (devui_ext_active(&ext) &&
                       touch_input_take_stroke(&touch, &sx, &sy, &ex, &ey)) {
                    int dx = ex - sx, dy = ey - sy;
                    int ady = dy < 0 ? -dy : dy;
                    if (sx <= EXT_BACK_EDGE_PX && sy >= ext.content_y &&
                        dx > EXT_BACK_COMMIT_PX && ady <= EXT_BACK_MAX_DY) {
                        devui_ext_deactivate(&ext);
                        touch_input_clear_taps(&touch);
                        need_render = 1;
                        break;
                    }
                }
            }

            if (devui_ext_active(&ext) && !pressed) {
                int tx, ty, cx, cy;
                while (touch_input_take_tap(&touch, &tx, &ty))
                    if (devui_ext_content_point(&ext, tx, ty, &cx, &cy))
                        devui_ext_handle_tap(&ext, cx, cy, now);
            }
            dragging = 0; drag_dir = 0; scroll_dir = 0; sliding = 0; segging = 0;
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
        } else if (g_sms_open >= 0) {
            if (pressed && !prev_press) {
                down_x = x;
                down_y = y;
                sms_scroll_start = g_sms_scroll;
                sms_dragging = 0;
            } else if (pressed && prev_press) {
                int dy = y - down_y;
                int ady = dy < 0 ? -dy : dy;
                int next = clampi(sms_scroll_start - dy, 0, g_sms_scroll_max);
                if (!sms_dragging && ady > 6 && next != g_sms_scroll &&
                    sms_point_in_view(down_x, down_y))
                    sms_dragging = 1;
                if (sms_dragging) {
                    if (next != g_sms_scroll && motion_frame_due(now, &last_motion_frame)) {
                        g_sms_scroll = next;
                        render_sms_overlay(&disp, 1);
                        animating = 1;
                    }
                }
            } else if (!pressed && prev_press) {
                int dx = x - down_x, dy = y - down_y;
                int release_was_tap = dx * dx + dy * dy <= 14 * 14;
                if (sms_dragging) {
                    int next = clampi(sms_scroll_start - dy, 0, g_sms_scroll_max);
                    if (next != g_sms_scroll) {
                        g_sms_scroll = next;
                        render_sms_overlay(&disp, 1);
                        animating = 1;
                    }
                } else if (release_was_tap) {
                    const char *act = html_view_click((float)down_x, (float)down_y);
                    if (!strcmp(act, "act:smsclose")) {
                        sms_close(&touch, &need_render);
                    }
                }
                if (g_sms_open >= 0)
                    touch_input_drop_replayed_release(&touch, release_was_tap);
                sms_dragging = 0;
            } else if (!pressed) {
                /* A complete gesture can land while layout is busy. Collapse queued
                 * input to its newest stroke so it cannot replay below the dialog. */
                int sx, sy, ex, ey;
                if (touch_input_take_latest_stroke(&touch, &sx, &sy, &ex, &ey)) {
                    int tx = sx, ty = sy;
                    int dx = ex - sx, dy = ey - sy;
                    int was_tap = dx * dx + dy * dy <= 14 * 14;
                    int have_tap = touch_input_take_latest_tap(&touch, &tx, &ty);
                    if (was_tap) {
                        const char *act = html_view_click((float)(have_tap ? tx : sx),
                                                          (float)(have_tap ? ty : sy));
                        if (!strcmp(act, "act:smsclose")) {
                            sms_close(&touch, &need_render);
                        }
                    } else if (sms_point_in_view(sx, sy) && g_sms_scroll_max > 0) {
                        int next = clampi(g_sms_scroll - dy, 0, g_sms_scroll_max);
                        if (next != g_sms_scroll) {
                            g_sms_scroll = next;
                            render_sms_overlay(&disp, 1);
                            animating = 1;
                        }
                    }
                } else {
                    int tx, ty;
                    if (touch_input_take_latest_tap(&touch, &tx, &ty)) {
                        const char *act = html_view_click((float)tx, (float)ty);
                        if (!strcmp(act, "act:smsclose")) {
                            sms_close(&touch, &need_render);
                        }
                    }
                }
            }
        } else if (g_modal) {
            /* second-level band-lock dialog: tap only (level-1 is disabled) */
            if (pressed && !prev_press) {
                down_x = x;
                down_y = y;
            } else if (!pressed && prev_press) {
                int mdx = x - down_x, mdy = y - down_y;
                int modal_was_tap = mdx * mdx + mdy * mdy <= 14 * 14;
                if (modal_was_tap)
                    handle_modal_tap(&disp, x, y, now, &need_render, &animating);
                /* The renderer has already handled this release. Remove its matching
                 * queue entries before a closed modal exposes the button underneath. */
                touch_input_drop_replayed_release(&touch, modal_was_tap);
            } else if (!pressed) {
                int sx, sy, ex, ey, tx, ty;
                int have_stroke = touch_input_take_latest_stroke(&touch, &sx, &sy, &ex, &ey);
                int have_tap = touch_input_take_latest_tap(&touch, &tx, &ty);
                if (have_tap && (!have_stroke ||
                    (ex - sx) * (ex - sx) + (ey - sy) * (ey - sy) <= 14 * 14))
                    handle_modal_tap(&disp, tx, ty, now, &need_render, &animating);
            }
        } else {
            int maxs = g_page_h > H ? g_page_h - H : 0;
            if (pressed && scroll_inertia) {
                scroll_inertia = 0;
                scroll_track_valid = 0;
                scroll_v = 0.0f;
            }
            if (scroll_inertia && !pressed && !dragging && !menu && maxs > 0) {
                uint32_t dtm = now > scroll_track_ms ? now - scroll_track_ms : 0;
                if (dtm > 64) dtm = 64;          /* a slow loop slows the glide, never jumps it */
                if (dtm > 0) {
                    float dt = dtm / 1000.0f;
                    float dv = SCROLL_INERTIA_DECEL * dt;
                    if (scroll_v > 0.0f) {
                        scroll_v -= dv;
                        if (scroll_v < 0.0f) scroll_v = 0.0f;
                    } else if (scroll_v < 0.0f) {
                        scroll_v += dv;
                        if (scroll_v > 0.0f) scroll_v = 0.0f;
                    }
                    scroll_pos += scroll_v * dt;
                    int ns = (int)(scroll_pos + (scroll_pos >= 0.0f ? 0.5f : -0.5f));
                    if (ns < 0) { ns = 0; scroll_pos = 0.0f; scroll_v = 0.0f; }
                    if (ns > maxs) { ns = maxs; scroll_pos = (float)maxs; scroll_v = 0.0f; }
                    if (ns != g_scroll) {
                        g_scroll = ns;
                        if (motion_frame_due(now, &last_motion_frame)) {
                            scroll_blit(&disp, g_scroll);
                            animating = 1;
                        }
                    }
                    if (scroll_v == 0.0f || ns == 0 || ns == maxs) {
                        scroll_inertia = 0;
                        need_render = 1;         /* settled: the full page once, at rest */
                    }
                    scroll_track_ms = now;
                }
            }
            if (!pressed && !prev_press && !dragging && !menu) {
                int sx, sy, ex, ey, queued_motion = 0;
                if (touch_input_take_latest_stroke(&touch, &sx, &sy, &ex, &ey)) {
                    int qdx = ex - sx, qdy = ey - sy;
                    int qadx = qdx < 0 ? -qdx : qdx;
                    int qady = qdy < 0 ? -qdy : qdy;
                    if (qady >= qadx && sheet_edge_pull(&disp, qdy, maxs)) {
                        queued_motion = 1;
                        animating = 1;
                        touch_input_clear_taps(&touch);
                    } else if (qdx >= QUEUED_SWIPE_PX && qadx > qady && back_pull_ok(sx) && back_pull_begin(&disp)) {
                        back_pull_end(&disp, qdx, 1);
                        queued_motion = 1;
                        animating = 1;
                        last_act = now;
                        touch_input_clear_taps(&touch);
                    } else if (!g_subpage[0] && g_npages > 1 && qadx >= QUEUED_SWIPE_PX && qadx > qady) {
                        int dir = qdx < 0 ? 1 : -1;
                        int target = (g_cur + (dir > 0 ? 1 : g_npages - 1)) % g_npages;
                        int o_now = dir > 0 ? -qdx : (W - qdx);
                        if (o_now < 0) o_now = 0;
                        if (o_now > W) o_now = W;
                        scroll_inertia = 0;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                        prep_pair(&disp, target, dir);
                        anim_o(&disp, o_now, dir > 0 ? W : 0);
                        swipe_land(&disp, dir > 0 ? g_bufB : g_bufA, target);
                        animating = 1;
                        last_motion_frame = now;
                        queued_motion = 1;
                        touch_input_clear_taps(&touch);
                    } else if (qady >= QUEUED_SCROLL_PX && qady >= qadx && maxs > 0) {
                        int bufh = g_page_h > SCROLLMAX ? SCROLLMAX : g_page_h;
                        int rh = scrollbuf_prepare(CUR_PATH, bufh);
                        int ns;
                        g_scroll_h = rh > bufh ? bufh : rh;
                        maxs = g_scroll_h > H ? g_scroll_h - H : 0;
                        ns = g_scroll - qdy;
                        if (ns < 0) ns = 0;
                        if (ns > maxs) ns = maxs;
                        scroll_inertia = 0;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                        if (ns != g_scroll) {
                            g_scroll = ns;
                            scroll_pos = (float)g_scroll;
                            scroll_blit(&disp, g_scroll);
                            animating = 1;
                            last_motion_frame = now;
                        }
                        queued_motion = 1;
                        touch_input_clear_taps(&touch);
                    }
                }
                if (!queued_motion) {
                    int tx, ty;
                    if (touch_input_take_latest_tap(&touch, &tx, &ty)) {
                        x = tx; y = ty;
                        down_x = tx; down_y = ty;
                        dragging = 1; drag_dir = 0; scroll_dir = 0; scroll_start = g_scroll; sliding = 0; segging = 0;
                        drag_down_ms = now;
                        scroll_inertia = 0;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                        last_motion_frame = 0;
                        replay_release = 1;
                        prev_press = 1;  /* reuse the normal release/action path below */
                    }
                }
            }
            if (pressed && !prev_press) {
                down_x = x; down_y = y; dragging = 1; drag_dir = 0; scroll_dir = 0; scroll_start = g_scroll; sliding = 0; segging = 0;
                last_motion_frame = 0;
                drag_down_ms = now;
                scroll_track_valid = 0; scroll_v = 0.0f;
                int bx, by, bw, bh;   /* grab the brightness slider / segmented control if pressed on it */
                if (html_view_rect("#bright-bar", &bx, &by, &bw, &bh) &&
                    x >= bx && x < bx + bw && y >= by - 5 && y < by + bh + 5) {
                    sliding = 1; bar_x = bx; bar_w = bw;
                    set_bright_x(x, bx, bw);
                    render(&disp, CUR_PATH);
                    last_motion_frame = now;
                    animating = 1;
                } else if (html_view_rect("#glass-bar", &bx, &by, &bw, &bh) &&
                           x >= bx && x < bx + bw && y >= by - 5 && y < by + bh + 5) {
                    sliding = 2; bar_x = bx; bar_w = bw;   /* glass transparency */
                    set_glass_x(x, bx, bw);
                    render(&disp, CUR_PATH);
                    last_motion_frame = now;
                    animating = 1;
                } else if (html_view_rect("#blur-bar", &bx, &by, &bw, &bh) &&
                           x >= bx && x < bx + bw && y >= by - 5 && y < by + bh + 5) {
                    sliding = 3; bar_x = bx; bar_w = bw;   /* glass blur */
                    set_blur_x(x, bx, bw);
                    render(&disp, CUR_PATH);
                    last_motion_frame = now;
                    animating = 1;
                } else {   /* segmented controls: net mode / auto-off / refresh */
                    int which = 0, n = 4;
                    if (html_view_rect("#netseg", &bx, &by, &bw, &bh) &&
                        x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 1; n = 4; }
                    else if (html_view_rect("#autoseg", &bx, &by, &bw, &bh) &&
                             x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 2; n = 6; }
                    else if (html_view_rect("#refreshseg", &bx, &by, &bw, &bh) &&
                             x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 3; n = 5; }
                    if (which) {
                        segging = 1; seg_which = which; seg_n = n;
                        seg_x = bx; seg_y = by; seg_w = bw; seg_h = bh;
                        g_segdrag = which;
                        render(&disp, CUR_PATH);          /* plain seg (no cell highlight) */
                        capture_fb(&disp);
                        seg_box(&disp, seg_x, seg_y, seg_w, seg_h, seg_n, x);
                        last_motion_frame = now;
                        animating = 1;
                    }
                }
            }
            else if (pressed && dragging && !menu) {
                if (sliding) {
                    if (sliding == 3)      set_blur_x(x, bar_x, bar_w);
                    else if (sliding == 2) set_glass_x(x, bar_x, bar_w);
                    else                   set_bright_x(x, bar_x, bar_w);
                    if (motion_frame_due(now, &last_motion_frame)) {
                        render(&disp, CUR_PATH);
                        animating = 1;
                    }
                }
                else if (segging) {
                    if (motion_frame_due(now, &last_motion_frame)) {
                        seg_box(&disp, seg_x, seg_y, seg_w, seg_h, seg_n, x);
                        animating = 1;
                    }
                }
                else {
                int dx = x - down_x, dy = y - down_y;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if (drag_dir == 0 && scroll_dir == 0 && !edge_pull && !back_pull) {
                    if (ady > DRAG_START_PX && ady >= adx &&
                        sheet_pull_begin(&disp, dy > 0 ? 1 : -1, maxs)) {
                        edge_pull = 1;              /* the sheet follows the finger from here */
                    } else if (dx > DRAG_START_PX && adx > ady && back_pull_ok(down_x) && back_pull_begin(&disp)) {
                        back_pull = 1;              /* the page follows the finger off to the right */
                    } else if (!g_subpage[0] && g_npages > 1 && adx > DRAG_START_PX && adx > ady) {
                        drag_dir = dx < 0 ? 1 : -1;
                        drag_target = (g_cur + (drag_dir > 0 ? 1 : g_npages - 1)) % g_npages;
                        prep_pair(&disp, drag_target, drag_dir);
                    } else if (ady > DRAG_START_PX && ady >= adx && maxs > 0) {
                        scroll_dir = 1; scroll_start = g_scroll;
                        int bufh = g_page_h > SCROLLMAX ? SCROLLMAX : g_page_h;  /* prerender once */
                        int rh = scrollbuf_prepare(CUR_PATH, bufh);
                        g_scroll_h = rh > bufh ? bufh : rh;
                        maxs = g_scroll_h > H ? g_scroll_h - H : 0;
                        scroll_track_pos = g_scroll;
                        scroll_track_ms = now;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                    } else if (ady > DRAG_CANCEL_PX) dragging = 0;
                }
                if (drag_dir != 0) {
                    if (motion_frame_due(now, &last_motion_frame)) {
                        compose_frame(&disp, drag_dir > 0 ? -dx : (W - dx));
                        animating = 1;
                    }
                } else if (edge_pull && g_pull.active) {
                    if (motion_frame_due(now, &last_motion_frame)) {
                        sheet_pull_move(&disp, dy);
                        animating = 1;
                    }
                } else if (back_pull) {
                    if (motion_frame_due(now, &last_motion_frame)) {
                        back_draw(&disp, dx);
                        animating = 1;
                    }
                } else if (scroll_dir != 0) {
                    int ns = scroll_start - dy;
                    if (ns < 0) ns = 0;
                    if (ns > maxs) ns = maxs;
                    if (ns != g_scroll) {
                        if (scroll_track_ms && now > scroll_track_ms) {
                            float inst = (ns - scroll_track_pos) * 1000.0f / (float)(now - scroll_track_ms);
                            scroll_v = scroll_track_valid ? (scroll_v * 0.65f + inst * 0.35f) : inst;
                            scroll_v = clamp_scroll_v(scroll_v);
                            scroll_track_valid = 1;
                        }
                        g_scroll = ns;
                        scroll_pos = (float)g_scroll;
                        scroll_track_pos = g_scroll;
                        scroll_track_ms = now;
                        if (motion_frame_due(now, &last_motion_frame)) {
                            scroll_blit(&disp, g_scroll);
                            animating = 1;
                        }
                    }
                }
                }
            }
            else if (!pressed && prev_press) {
                int rel_dx = x - down_x, rel_dy = y - down_y;
                int release_was_tap = dragging && !sliding && !segging && !edge_pull && !back_pull &&
                                      drag_dir == 0 && scroll_dir == 0 &&
                                      rel_dx * rel_dx + rel_dy * rel_dy <= 14 * 14;
                int dx = rel_dx;
                if (back_pull) {
                    back_pull_end(&disp, rel_dx, now - drag_down_ms < BACK_FLICK_MS);
                    touch_input_clear_taps(&touch);
                    animating = 1;
                    last_act = now;
                }
                else if (edge_pull) {
                    if (g_pull.active) {
                        sheet_pull_end(&disp, rel_dy);
                        touch_input_clear_taps(&touch);
                        animating = 1;
                    } else if (dragging && !menu && sheet_edge_pull(&disp, rel_dy, maxs)) {
                        touch_input_clear_taps(&touch);
                        animating = 1;
                    }
                }
                else if (sliding >= 2) wallpaper_save();   /* persist glass after drag */
                else if (sliding) { save_settings(); /* persist brightness after drag */ }
                else if (segging) {   /* snap to nearest cell + apply */
                    int c = clampi((x - seg_x) * seg_n / (seg_w > 0 ? seg_w : 1), 0, seg_n - 1);
                    if (seg_which == 1) {
                        net_mode_apply(netmode_value(c));
                        snprintf(g_net_pending, sizeof g_net_pending, "%s", netmode_value(c));
                    } else if (seg_which == 2) {
                        g_autooff_ms = g_autooffs[c].ms; last_act = now; save_settings();
                    } else {
                        g_refresh_ms = normalize_refresh_ms(g_refresh_rates[c].ms);
                        (void)datad_set_refresh_ms(g_refresh_ms);
                        if (g_refresh_ms > 0) {
                            if (data_backend_commit_latest()) need_render = 1;
                            state_pending = 0;
                        }
                        last_data = 0;
                        save_settings();
                    }
                    g_segdrag = 0; need_render = 1;
                }
                else if (dragging && drag_dir != 0) {            /* finish or snap back */
                    int adx = dx < 0 ? -dx : dx;
                    uint32_t held = now - drag_down_ms;
                    /* far enough, or a quick flick the way the drag went */
                    int commit = adx > W * 30 / 100 ||
                                 (adx >= 24 && held < 300 && (dx < 0) == (drag_dir > 0));
                    int o_now = drag_dir > 0 ? -dx : (W - dx);
                    if (o_now < 0) o_now = 0; if (o_now > W) o_now = W;
                    anim_o(&disp, o_now, drag_dir > 0 ? (commit ? W : 0) : (commit ? 0 : W));
                    if (commit) {
                        swipe_land(&disp, drag_dir > 0 ? g_bufB : g_bufA, drag_target);
                    } else {
                        /* After a snap-back the on-screen pixels no longer match the
                         * cached HTML of the current page. Force a full re-render so
                         * native-drawn content from the neighbor page (for example
                         * charts) cannot leak onto the restored page. */
                        invalidate_render_html_cache();
                        need_render = 1;
                    }
                } else if (dragging && scroll_dir != 0) {   /* scroll settled */
                    uint32_t gdt = (drag_down_ms && now > drag_down_ms) ? (now - drag_down_ms) : 0;
                    if (gdt > 0) {
                        float rel_v = (-(float)(y - down_y)) * 1000.0f / (float)gdt;
                        if (scroll_track_valid) scroll_v = clamp_scroll_v(scroll_v * 0.55f + rel_v * 0.45f);
                        else                    scroll_v = clamp_scroll_v(rel_v);
                    }
                    if (fabsf(scroll_v) >= SCROLL_INERTIA_MIN_V && maxs > 0) {
                        scroll_inertia = 1;          /* glides on from the page buffer; renders once it stops */
                        scroll_pos = (float)g_scroll;
                        scroll_track_ms = now;
                    } else {
                        scroll_inertia = 0;
                        scroll_v = 0.0f;
                        need_render = 1;
                    }
                } else if (dragging) {                      /* tap -> action */
                    const char *act = html_view_click((float)x, (float)y);
                    if (!strncmp(act, "act:", 4)) {
                        const char *a = act + 4;
#ifdef DEVUI_PLAIN_OPENWRT
                        /* On plain OpenWrt the USB mode, ADB and Wi-Fi power-save switches would run
                         * the vendor firmware's scripts; they wait for the system's own interfaces. */
                        if (!strcmp(a, "adb") || !strcmp(a, "usbnet") || !strcmp(a, "usbpower") || !strcmp(a, "psm")) {
                            snprintf(g_toast, sizeof g_toast, "此系统暂不支持");
                            g_toast_until = now + 1600;
                            need_render = 1;
                            goto action_done;
                        }
#endif
                        if (!strcmp(a, "poweroff")) {
                            if (g_pwr_confirm == 1) {        /* confirmed: blank first (instant feedback), then power off */
                                screen_off(&disp); datad_control_async("device.poweroff", NULL, "poweroff");
                            } else { g_pwr_confirm = 1; g_pwr_until = now + 4000; need_render = 1; }
                        }
                        else if (!strcmp(a, "reboot")) {
                            if (g_pwr_confirm == 2) { screen_off(&disp); datad_control_async("device.reboot", NULL, "reboot"); }
                            else { g_pwr_confirm = 2; g_pwr_until = now + 4000; need_render = 1; }
                        }
                        else if (!strcmp(a, "close"))     { menu = 0; g_pwr_confirm = 0; need_render = 1; }
                        else if (!strcmp(a, "menu"))      { backlight_on(); menu = 1; g_pwr_confirm = 0; need_render = 1; }
                        else if (!strncmp(a, "sub:", 4)) {
                            if (subpage_open(a + 4)) {
                                menu = 0;
                                g_modal = 0;
                                g_sms_open = -1;
                                invalidate_render_html_cache();
                            } else {
                                snprintf(g_toast, sizeof g_toast, "页面不可用");
                                g_toast_until = now + 1600;
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "func:", 5)) {
                            if (function_page_open(a + 5)) {
                                menu = 0;
                                g_modal = 0;
                                g_sms_open = -1;
                                invalidate_render_html_cache();
                            } else {
                                snprintf(g_toast, sizeof g_toast, "页面不可用");
                                g_toast_until = now + 1600;
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "backfunc")) {
                            subpage_close();
                            menu = 0;
                            g_modal = 0;
                            g_sms_open = -1;
                            invalidate_render_html_cache();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "theme")) {
                            g_theme = (g_theme + 1) % NTHEMES;
                            save_settings();
                            apply_glass();                    /* the "主题" glass colour follows */
                            snprintf(g_toast, sizeof g_toast, "主题：%s", g_themes[g_theme].name);
                            g_toast_until = now + 1600;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "revealkey")) { g_show_key = !g_show_key; need_render = 1; }
                        else if (!strcmp(a, "revealcell")) { g_show_cellid = !g_show_cellid; need_render = 1; }
                        else if (!strcmp(a, "revealimei")) { g_show_imei = !g_show_imei; need_render = 1; }
                        else if (!strcmp(a, "refreshambr")) {
                            datad_control_async("qos.reload", NULL, NULL);
                            snprintf(g_toast, sizeof g_toast, "已请求刷新 AMBR");
                            g_toast_until = now + 1600;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "neighbors")) {
                            g_neighbor_open = !g_neighbor_open;
                            invalidate_render_html_cache();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "spunit"))    { g_speed_bits = !g_speed_bits; save_settings(); need_render = 1; }
                        else if (!strcmp(a, "batpct"))    { g_show_batpct = !g_show_batpct; save_settings(); need_render = 1; }
                        else if (!strcmp(a, "sbspeed"))   { g_show_sbspeed = !g_show_sbspeed; save_settings(); need_render = 1; }
                        else if (!strcmp(a, "textadapt")) {
                            g_text_adapt = !g_text_adapt;
                            html_view_set_text_adapt(g_text_adapt);
                            save_settings();
                            invalidate_render_html_cache();
                            need_render = 1;
                        }
                        else if (!strcmp(a, "glass")) {     /* tap position on the bar = transparency */
                            int bx, by, bw, bh;
                            if (html_view_rect("#glass-bar", &bx, &by, &bw, &bh) && bw > 0) {
                                set_glass_x(x, bx, bw);
                                wallpaper_save();
                            }
                            invalidate_render_html_cache();
                            need_render = 1;
                        }
                        else if (!strcmp(a, "glassblur")) { /* tap position on the bar = blur */
                            int bx, by, bw, bh;
                            if (html_view_rect("#blur-bar", &bx, &by, &bw, &bh) && bw > 0) {
                                set_blur_x(x, bx, bw);
                                wallpaper_save();
                            }
                            need_render = 1;
                        }
                        else if (!strncmp(a, "glasstint:", 10)) {
                            g_glass_tint = clampi(atoi(a + 10), 0, NTINTS - 1);
                            wallpaper_save();
                            invalidate_render_html_cache();
                            need_render = 1;
                        }
                        else if (!strcmp(a, "nfc")) {
                            static devui_data_t dd; char cmd[120];
                            int on = (data_refresh_live(&dd) && dd.nfc_switch) ? 0 : 1;
                            snprintf(cmd, sizeof cmd,
                                "ubus call zwrt_nfc zwrt_nfc_wifi_set '{\"switch\":%d,\"flag\":2}' >/dev/null 2>&1", on);
                            /* without flag datad keeps the vendor's current NFC Wi-Fi target */
                            datad_control_async("nfc.set", on ? "{\"enabled\":true}" : "{\"enabled\":false}", cmd);
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wifi")) {   /* master: both main bands (wlan0+wlan2) */
                            int on = (g_w24 == 1 || g_w5 == 1 || g_w6 == 1);
                            const char *ic = on ? "down" : "up";
                            char cmd[160];
                            if (dev_is_tf())   /* TopFlow: wlan0 is the only main AP netdev */
                                snprintf(cmd, sizeof cmd, "ifconfig wlan0 %s >/dev/null 2>&1", ic);
                            else
                                snprintf(cmd, sizeof cmd,
                                    "(ifconfig wlan0 %s; ifconfig wlan2 %s) >/dev/null 2>&1", ic, ic);
                            datad_control_async("wifi.set_module",
                                                on ? "{\"enabled\":false}" : "{\"enabled\":true}", cmd);
#ifdef DEVUI_PLAIN_OPENWRT
                            if (g_w24 >= 0) g_w24 = !on;
                            if (g_w5 >= 0) g_w5 = !on;
#else
                            g_w24 = g_w5 = !on;
#endif
                            if (g_w6 >= 0) g_w6 = !on;
                            g_wifi_hold_until = now + 8000;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wifi6")) {   /* a radio on 6 GHz: no netdev to fall back on */
                            int on = g_w6 == 1;
                            datad_control_async("wifi.radio.set", on ? "{\"band\":\"6g\",\"enabled\":false}"
                                                                     : "{\"band\":\"6g\",\"enabled\":true}", NULL);
                            g_w6 = !on;
                            g_wifi_hold_until = now + 8000;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wifi24") || !strcmp(a, "wifi5")) {
                            int is24 = !strcmp(a, "wifi24");
                            int on = (is24 ? g_w24 : g_w5) == 1;
                            char cmd[120], params[48];
                            snprintf(cmd, sizeof cmd, "ifconfig %s %s >/dev/null 2>&1",
                                     dev_is_tf() ? "wlan0" : is24 ? "wlan0" : "wlan2", on ? "down" : "up");
                            snprintf(params, sizeof params, "{\"band\":\"%s\",\"enabled\":%s}",
                                     is24 ? "2g" : "5g", on ? "false" : "true");
                            /* datad refuses layouts the firmware cannot start (last band,
                             * three APs on U60 Pro); the next wifi.state read shows that. */
                            /* no netdev to fall back on for the TopFlow 2.4G radio */
                            datad_control_async("wifi.radio.set", params,
                                                dev_is_tf() && is24 ? NULL : cmd);
                            if (is24) g_w24 = !on; else g_w5 = !on;
                            g_wifi_hold_until = now + 8000;
                            need_render = 1;
                        }
                        else if (dev_is_tf() && !strcmp(a, "agg")) {
                            /* aggregation on: one tap; off drops the bonded link, so arm first */
                            if (g_agg_on == 1 && (int32_t)(now - g_agg_armed_until) >= 0) {
                                g_agg_armed_until = now + 4000;
                                snprintf(g_toast, sizeof g_toast, "再点一次关闭聚合");
                                g_toast_until = now + 2000;
                            } else {
                                int on = g_agg_on != 1;
                                datad_control_async("aggregation.set",
                                                    on ? "{\"enabled\":true}" : "{\"enabled\":false}", NULL);
                                g_agg_on = on;
                                g_agg_armed_until = 0;
                                g_agg_hold_until = now + 15000;
                                snprintf(g_toast, sizeof g_toast, on ? "正在开启聚合" : "正在关闭聚合");
                                g_toast_until = now + 1600;
                            }
                            need_render = 1;
                        }
                        else if (dev_is_tf() && (!strncmp(a, "fan:", 4) || !strcmp(a, "fanon"))) {
                            /* fanon: control-center toggle between always-on and the curve */
                            const char *m = a[3] == ':' ? a + 4 :
                                            !strcmp(g_fan_mode, "always_on") ? "custom" : "always_on";
                            if (!strcmp(m, "automatic") || !strcmp(m, "custom") || !strcmp(m, "always_on")) {
                                char params[48];
                                snprintf(params, sizeof params, "{\"mode\":\"%s\"}", m);
                                datad_control_async("cooling.fan.set_mode", params, NULL);
                                snprintf(g_fan_mode, sizeof g_fan_mode, "%s", m);
                                g_fan_hold_until = now + 5000;
                            }
                            need_render = 1;
                        }
                        else if (dev_is_tf() && (!strncmp(a, "liq:", 4) || !strcmp(a, "liqon"))) {
                            /* liqon: control-center toggle between high and automatic */
                            const char *m = a[3] == ':' ? a + 4 :
                                            (!strcmp(g_liq_mode, "low") || !strcmp(g_liq_mode, "high")) ? "automatic" : "high";
                            if (!strcmp(m, "automatic") || !strcmp(m, "low") || !strcmp(m, "high")) {
                                char params[48];
                                snprintf(params, sizeof params, "{\"mode\":\"%s\"}", m);
                                datad_control_async("cooling.liquid.set_mode", params, NULL);
                                snprintf(g_liq_mode, sizeof g_liq_mode, "%s", m);
                                g_liq_hold_until = now + 5000;
                            }
                            need_render = 1;
                        }
                        else if (!strcmp(a, "dps")) {   /* power direct-supply mode */
                            int on = (g_dps == 1);
                            char cmd[160];
                            snprintf(cmd, sizeof cmd,
                                "ubus call zwrt_bsp.charger set '{\"direct_power_supply_mode\":\"%s\"}' >/dev/null 2>&1",
                                on ? "disable" : "enable");
                            datad_control_async("power.direct_supply.set",
                                                on ? "{\"enabled\":false}" : "{\"enabled\":true}", cmd);
                            g_dps = !on;
                            g_dps_hold_until = now + 5000;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "psm")) {
                            int turn_on = g_wpsm == 1 ? 0 : 1;   /* power_save on = battery saving */
                            const char *m = turn_on ? "on" : "off";
                            /* The switch owns /etc/hotplug.d/iface/99-disable-powersave: it writes
                             * the script itself (re-applying the chosen mode on every ifup) and
                             * applies it now, so the feature is self-contained; nothing needs to be
                             * pre-installed. (Also drop the legacy "psm" file from older builds, which
                             * sorts later and would otherwise override this one.) */
                            char cmd[700];
                            snprintf(cmd, sizeof cmd,
                                "(mkdir -p /etc/hotplug.d/iface; rm -f /etc/hotplug.d/iface/psm; "
                                "{ echo '#!/bin/sh'; echo '[ \"$ACTION\" = ifup ] && {'; "
                                "echo '  iw dev wlan0 set power_save %s 2>/dev/null'; "
                                "echo '  iw dev wlan1 set power_save %s 2>/dev/null'; "
                                "echo '  iw dev wlan2 set power_save %s 2>/dev/null'; "
                                "echo '  iw dev wlan3 set power_save %s 2>/dev/null'; echo '}'; } "
                                "> /etc/hotplug.d/iface/99-disable-powersave; "
                                "chmod +x /etc/hotplug.d/iface/99-disable-powersave; "
                                "for w in wlan0 wlan1 wlan2 wlan3; do iw dev $w set power_save %s 2>/dev/null; done) "
                                ">/dev/null 2>&1 &",
                                m, m, m, m, m);
                            system(cmd);
                            g_wpsm = turn_on;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "adb")) {
                            static devui_data_t dd;
                            const char *mode = "debug";
                            if (data_refresh_live(&dd) && !strcmp(dd.usb_mode, "debug") &&
                                !usb_pid_is("90b1") && !usb_pid_is("90B1"))
                                mode = "user";
                            g_adb_pending = !strcmp(mode, "debug");
                            char cmd[160];
                            if (g_adb_pending)
                                snprintf(cmd, sizeof cmd,
                                    "/sbin/usb_composition 9059 n n y y >/dev/null 2>&1 &");
                            else
                                snprintf(cmd, sizeof cmd,
                                    "ubus call zwrt_bsp.usb set '{\"mode\":\"user\"}' >/dev/null 2>&1");
                            /* Turning ADB on keeps the existing 9059 composition switch;
                             * datad has no action for it. */
                            if (g_adb_pending) system(cmd);
                            else datad_control_async("usb.set", "{\"mode\":\"user\"}", cmd);
                            need_render = 1;
                        }
                        else if (dev_is_tf() && (!strcmp(a, "usbpower") || !strcmp(a, "usbnet"))) {
                            /* no reverse charging or USB tethering on the TopFlow */
                            need_render = 1;
                        }
                        else if (!strcmp(a, "usbpower")) {
                            if (g_typec_pending >= 0 && now < g_typec_until) {
                                need_render = 1;
                                goto action_done;
                            }
                            int cur = g_typec_source == 1;
                            int target = !cur;
                            int net_active = ((g_usb_net_pending >= 0 ? g_usb_net_pending : g_usb_net) == 1 ||
                                              usb_pid_is("9057") ||
                                              usb_pid_is("90b1") || usb_pid_is("90B1"));
                            if (net_active) {
                                system("touch /tmp/u60-usbnet-switching");
                                if (target) usb_net_apply_source_async();
                                else        usb_net_apply_sink_async();
                                usb_net_watchdog_start();
                                g_usb_net = 1;
                            } else {
                                usb_power_only_apply_async(target);
                            }
                            g_typec_source = target;
                            g_typec_pending = target;
                            g_typec_until = now + 20000;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "usbnet")) {
                            char cmd[900];
                            int cur = g_usb_net_pending >= 0 ? g_usb_net_pending :
                                      ((g_usb_net == 1) || usb_pid_is("9057") ||
                                       usb_pid_is("90b1") || usb_pid_is("90B1"));
                            int target = !cur;
                            if (g_usb_net_pending >= 0 && now < g_usb_net_until) {
                                need_render = 1;
                                goto action_done;
                            }
                            if (!target) {
                                snprintf(cmd, sizeof cmd,
                                    "(rm -f /tmp/u60-usbnet-switching; "
                                    "SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
                                    "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\") >/dev/null 2>&1 &");
                                g_usb_net = 0;
                                g_usb_net_pending = 0;
                                g_usb_net_until = now + 20000;
                                usb_net_watchdog_stop();
                                system(cmd);
                            } else {
                                int want_source = g_typec_pending >= 0 ? g_typec_pending : (g_typec_source == 1);
                                g_usb_net = 1;
                                g_typec_source = want_source;
                                g_usb_net_pending = 1;
                                g_typec_pending = want_source;
                                g_usb_net_until = now + 20000;
                                g_typec_until = now + 20000;
                                system("touch /tmp/u60-usbnet-switching");
                                usb_net_watchdog_start();
                                if (want_source) usb_net_apply_source_async();
                                else             usb_net_apply_sink_async();
                            }
                            need_render = 1;
                        }
                        else if (!strcmp(a, "bright")) {    /* tap position on the bar = level */
                            int bx, by, bw, bh;
                            if (html_view_rect("#bright-bar", &bx, &by, &bw, &bh) && bw > 0) {
                                int frac = (x - bx) * 100 / bw;
                                if (frac < 3) frac = 3; if (frac > 100) frac = 100;
                                int bmax = backlight_max(); if (bmax <= 0) bmax = 255;
                                if (g_auto_bright) { g_auto_bright = 0; als_close(); }
                                backlight_set(frac * bmax / 100);
                                save_settings();   /* persist brightness */
                            }
                            need_render = 1;
                        }
                        else if (!strcmp(a, "autobright") && als_supported()) {
                            g_auto_bright = !g_auto_bright;
                            save_settings();
                            als_sync();
                            snprintf(g_toast, sizeof g_toast, g_auto_bright ? "已开启自动亮度" : "已关闭自动亮度");
                            g_toast_until = now + 1400;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "autooff:", 8)) {   /* preset select */
                            g_autooff_ms = atoi(a + 8); last_act = now; save_settings(); need_render = 1;
                        }
                        else if (!strncmp(a, "refreshms:", 10)) {   /* preset select */
                            g_refresh_ms = normalize_refresh_ms(atoi(a + 10));
                            (void)datad_set_refresh_ms(g_refresh_ms);
                            if (g_refresh_ms > 0) {
                                if (data_backend_commit_latest()) need_render = 1;
                                state_pending = 0;
                            }
                            last_data = 0;
                            save_settings();
                            need_render = 1;
                        }
                        else if (!strcmp(a, "stpage")) {
                            if (subpage_open("speedtest.html")) {
                                menu = 0;
                                g_modal = 0;
                                g_sms_open = -1;
                                g_st_home_open = 0;
                            } else {
                                int pi = speedtest_page_index();
                                if (pi >= 0) {
                                    subpage_close();
                                    g_cur = pi;
                                    g_scroll = 0;
                                    menu = 0;
                                }
                            }
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "sttoggle")) {
                            if (speedtest_binary_ready())
                                g_st_home_open = !g_st_home_open;
                            else
                                g_st_home_open = 0;
                            subpage_close();
                            g_cur = 0;
                            g_scroll = 0;
                            menu = 0;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "stclose")) {
                            g_st_home_open = 0;
                            subpage_close();
                            g_cur = 0;
                            g_scroll = 0;
                            menu = 0;
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "ststart")) {
                            speedtest_start();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "ststop")) {
                            speedtest_stop();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "stsrc:", 6)) {
                            snprintf(g_st_src, sizeof g_st_src, "%s", speedtest_norm_src(a + 6));
                            save_settings();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "stdir:", 6)) {
                            snprintf(g_st_dir, sizeof g_st_dir, "%s", speedtest_norm_dir(a + 6));
                            save_settings();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "stdur:", 6)) {
                            g_st_dur = speedtest_norm_dur(atoi(a + 6));
                            save_settings();
                            last_act = now;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "locktoggle")) {   /* on->off clears PIN; off->on opens setup pad */
                            if (lock_enabled()) clear_pin();
                            else                enter_lock(1);
                            need_render = 1;
                        }
                        else if (!strncmp(a, "net:", 4)) {   /* segmented control tap fallback */
                            net_mode_apply(a + 4);
                            snprintf(g_net_pending, sizeof g_net_pending, "%s", a + 4);
                            snprintf(g_toast, sizeof g_toast, "网络模式已切换"); g_toast_until = now + 1600;
                        }
                        else if (!strncmp(a, "openmodal:", 10)) {   /* open band-lock dialog */
                            const char *r = a + 10;
                            if (!strcmp(r, "sig")) g_modal = 4;
                            else g_modal = !strcmp(r, "sa") ? 1 : !strcmp(r, "nsa") ? 2 : 3;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "sms:", 4)) {   /* open SMS detail + mark read */
                            const char *id_txt = a + 4;
                            char *endp = NULL;
                            long sid = strtol(id_txt, &endp, 10);
                            static devui_data_t sd;
                            g_sms_open = -1;
                            g_sms_scroll = g_sms_scroll_max = 0;
                            g_sms_view_x = g_sms_view_y = g_sms_view_w = g_sms_view_h = 0;
                            g_sms_num[0] = g_sms_date[0] = g_sms_text[0] = 0;
                            need_render = 1;
                            if (id_txt != endp && endp && *endp == '\0' && data_refresh(&sd)) {
                                if (sid > 0) {
                                    for (int i = 0; i < sd.sms_n; i++) {
                                        if (sd.sms[i].id != sid) continue;
                                        snprintf(g_sms_num,  sizeof g_sms_num,  "%s", sd.sms[i].num);
                                        snprintf(g_sms_date, sizeof g_sms_date, "%s", sd.sms[i].date);
                                        snprintf(g_sms_text, sizeof g_sms_text, "%s", sd.sms[i].text);
                                        g_sms_open = sid;
                                        if (sd.sms[i].unread && sd.sms[i].id > 0) {
                                            char cmd[128], params[64];
                                            snprintf(cmd, sizeof cmd,
                                                "ubus call zwrt_wms zwrt_wms_modify_tag '{\"id\":\"%ld;\",\"tag\":0}' >/dev/null 2>&1",
                                                sd.sms[i].id);
                                            snprintf(params, sizeof params,
                                                     "{\"ids\":\"%ld;\",\"tag\":0}", sd.sms[i].id);
                                            datad_control_async("sms.mark_read", params, cmd);
                                        }
                                        need_render = 1;
                                        break;
                                    }
                                }
                            }
                        }
                        else if (!strcmp(a, "resetband")) {
                            /* no datad action resets bands and cells in one vendor call */
                            datad_ubus_call_async("zte_nwinfo_api", "nwinfo_reset_band_cell_setting", NULL,
                                                  "ubus call zte_nwinfo_api nwinfo_reset_band_cell_setting '{}' >/dev/null 2>&1");
                            snprintf(g_toast, sizeof g_toast, "锁频已恢复默认"); g_toast_until = now + 1600;
                            need_render = 1;   /* selection re-syncs from the live lock automatically */
                        }
                    }
                }
action_done:
                if (!replay_release)
                    touch_input_drop_replayed_release(&touch, release_was_tap);
                last_motion_frame = 0;
                if (g_pull.active) { g_pull.active = 0; need_render = 1; }
                dragging = 0; drag_dir = 0; scroll_dir = 0; sliding = 0; segging = 0; g_segdrag = 0; edge_pull = 0; back_pull = 0;
                scroll_track_valid = 0;
            }
        }
        prev_press = pressed;

        /* when the lock pad first appears, drop taps that were queued before it
         * (e.g. the toggle tap that opened it) so they don't phantom-press keys */
        if (g_lock_state != prev_lock) touch_input_clear_taps(&touch);
        prev_lock = g_lock_state;

        /* dismiss the toast after its timeout */
        if (g_toast[0] && now >= g_toast_until) { g_toast[0] = 0; need_render = 1; }

        /* power-menu armed state auto-reverts if the second tap doesn't come */
        if (g_pwr_confirm && now >= g_pwr_until) { g_pwr_confirm = 0; if (menu) need_render = 1; }

        /* auto screen-off after the configured idle timeout (locks if enabled) */
        if (g_autooff_ms > 0 && backlight_is_on() && now - last_act >= (uint32_t)g_autooff_ms) {
            screen_off(&disp);
            if (ext_ok) devui_ext_deactivate(&ext);
            enter_lock(0);
        }
        /* never sleeping: still lock after a minute untouched */
        if (g_autooff_ms == 0 && !g_lock_state && backlight_is_on() &&
            now - last_act >= LOCK_IDLE_MS) {
            if (ext_ok) devui_ext_deactivate(&ext);
            menu = 0;
            enter_lock(0);
            need_render = 1;
        }

        /* periodic clock/state check (not mid-drag). refresh=0 pauses promotion of
         * live backend updates, but the minute clock still ticks once per second. */
        {
            uint32_t poll_ms = g_refresh_ms > 0 ? (uint32_t)g_refresh_ms : 1000;
            /* not right after a touch either: a refresh renders the whole page,
             * which would hold up the next swipe of a quick run */
            if (!dragging && !scroll_inertia && now - last_data >= poll_ms && now - last_act >= TOUCH_QUIET_MS) {
                time_t now_t = time(NULL);
                int state_changed = 0;
                int clock_changed = 0;
                int state_render = 0;
                int cur_min = (int)(now_t / 60);
                if (cur_min != g_last_clock_min) {
                    g_last_clock_min = cur_min;
                    clock_changed = 1;
                    need_render = 1;   /* update HH:MM and any minute-level derived tokens */
                }

                if (g_refresh_ms > 0 && state_pending && data_backend_commit_latest()) {
                    state_changed = 1;
                    state_render = 1;
                    state_pending = 0;
                }
                if (g_refresh_ms > 0 && signal_live_enabled() && path_is_signal_page(CUR_PATH))
                    state_render = 1;
                if (state_render) need_render = 1;
                if (ext_ok && devui_ext_active(&ext) && !g_lock_state &&
                    (clock_changed || (g_refresh_ms > 0 && state_changed)))
                    refresh_status_cache();
                last_data = now;
            }
        }

        /* reconcile page-2 WiFi switch states from uci/iw on a slow throttle */
        if (g_aux_started) {
            aux_apply();
            if (backlight_is_on() != g_aux_awake) aux_set_awake(backlight_is_on());
        }
        if (!g_charge_boot && data_backend_generation() != datad_generation) {
            /* new datad connection (first contact or a restarted datad) */
            datad_generation = data_backend_generation();
            g_modem_api = -1;
            g_modem_api_probe_at = 0;
            (void)datad_set_refresh_ms(g_refresh_ms);
            (void)datad_set_signal_read(g_sig_read);
            (void)datad_set_signal_parse(g_sig_parse);
        }

        if (!g_charge_boot && now - g_pages_scan_at >= 1000) {
            g_pages_scan_at = now;
            if (device_update(data_model_tag())) {
                apply_panel_rotation(&disp, &touch);
                rescan_pages_keep_current();
                invalidate_render_html_cache();
                need_render = 1;
            }
            if (rescan_pages_if_changed()) need_render = 1;
            if (wallpaper_poll()) {
                invalidate_render_html_cache();
                need_render = 1;
            }
            als_sync();                       /* picks up a late model detection too */
        }
        if (backlight_is_on()) als_poll(now);

        int st_poll_ms = (g_st_state == ST_STATE_RUNNING || g_st_home_open) ? 300 : 900;
        if (!dragging && !scroll_inertia && now - g_st_poll_at >= (uint32_t)st_poll_ms) {
            int st_changed = speedtest_poll(now);
            if (!g_lock_state && (path_is_speedtest(CUR_PATH) || g_st_home_open) && (st_changed || g_st_state == ST_STATE_RUNNING))
                need_render = 1;
        }

        if (need_render && backlight_is_on()) {
            selfcheck_stage("render");
            if (ext_ok && devui_ext_active(&ext) && !g_lock_state)
                render_ext_view(&disp, &ext);
            else
                render(&disp, CUR_PATH);
        }
        /* the live wallpaper plays on the lock screen (and the home page) left alone */
        int live_ok = g_wp_live && html_view_live_frames() > 0 && backlight_is_on() && !pressed &&
                      !dragging && !scroll_inertia && !sliding && !segging && !edge_pull && !back_pull &&
                      !g_pull.active && now - last_act >= TOUCH_QUIET_MS && !(ext_ok && devui_ext_active(&ext)) &&
                      !menu && !g_modal && g_sms_open < 0 && !g_scroll &&
                      (g_lock_state == 1 || (g_wp_live == 2 && !g_lock_state && !g_subpage[0] && g_cur == 0));
        selfcheck_stage("live");
        if (!live_ok) live_stop();
        else if (!g_live_run) live_begin(&disp);
        selfcheck_stage("idle");

        was_on = backlight_is_on();   /* track for next frame's wake-touch discard */
        if (!animating && (scroll_inertia || (pressed && dragging))) {
            usleep(MOTION_SLEEP_US);   /* mid-motion: back soon for the next frame, no waiting on the send */
        } else if (!animating && g_live_run) {
            usleep(IDLE_SLEEP_ON_US);   /* live_main sends the frames */
        } else if (!animating) {
            tx_wait();   /* nothing writes the fb mid-send */
            usleep(backlight_is_on() ? IDLE_SLEEP_ON_US : IDLE_SLEEP_OFF_US);
        }
    }

    if (ext_ok) devui_ext_close(&ext);
    if (g_als_want == 1) datad_control("ambient_light.set", "{\"enabled\":false}", NULL, 0, 2000);
    signal_async_stop();
    data_backend_close();
    drm_disp_close(&disp);
    return 0;
}
