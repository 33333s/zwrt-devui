/*
 * preview_host.c - render the DevUI pages on a Linux host, without a panel.
 *
 * Builds the real htmlmain.c (its main() renamed) against an in-memory
 * framebuffer, so pages go through the same token substitution, CSS and
 * native drawing as on the device. State comes from a datad on the port the
 * build was given (tests/ui/preview.sh starts tests/mock_datad.py).
 *
 * usage: preview-host OUT_DIR [theme: 0 dark, 1 light]
 * Writes one PPM per screen; tall pages get a second, scrolled frame.
 *
 * SPDX-License-Identifier: MIT
 */
#define main devui_main
#include "../../src/htmlmain.c"
#undef main

static uint16_t g_preview_fb[320 * 480];

static void shot(const drm_disp_t *d, const char *dir, const char *name)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", d->width, d->height);
    for (int i = 0; i < d->width * d->height; i++) {
        uint16_t p = d->fb[i];
        unsigned char rgb[3] = {
            (unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63),
            (unsigned char)((p & 31) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* render path at the top and, when it is taller than the screen, scrolled to its end (with DEVUI_PREVIEW_ALL=1 also at every
 * screen in between: name-1, name-2, ...) */
static void shot_page(drm_disp_t *d, const char *dir, const char *name, const char *path)
{
    char tail[96];
    const char *all = getenv("DEVUI_PREVIEW_ALL");

    g_scroll = 0;
    invalidate_render_html_cache();
    render(d, path);
    shot(d, dir, name);
    if (g_page_h > d->height + 8) {
        int step = d->height - 40;
        for (int k = 1; all && all[0] == '1' && k * step < g_page_h - d->height; k++) {
            g_scroll = k * step;
            invalidate_render_html_cache();
            render(d, path);
            snprintf(tail, sizeof tail, "%s-%d", name, k);
            shot(d, dir, tail);
        }
        g_scroll = g_page_h - d->height;
        invalidate_render_html_cache();
        render(d, path);
        snprintf(tail, sizeof tail, "%s-end", name);
        shot(d, dir, tail);
        g_scroll = 0;
    }
}

int main(int argc, char **argv)
{
    static const char *const subs[] = { "control.html", "wifi.html", "sms.html", "cell.html", "lock.html", "glass.html",
                                        "speedtest.html", "simpin.html", "smsnew.html" };
    drm_disp_t disp = { .fd = -1, .fb = g_preview_fb, .width = 320, .height = 480,
                        .pitch = 640, .pitch_px = 320 };
    const char *out = argc > 1 ? argv[1] : ".";
    char name[96], path[300];

    g_theme = argc > 2 ? atoi(argv[2]) : 0;
    g_sig_parse = 1;            /* show the neighbor card */
    paths_init();
    device_init();
    scan_pages();
    if (g_npages == 0) { fprintf(stderr, "no pages in %s\n", UI_DIR); return 1; }
    g_panel_rot = 0;            /* shots are taken from the logical, unrotated frame */
    html_view_init(disp.fb, disp.width, disp.height, disp.pitch_px, 0, resolve_ui_font());
    html_view_set_uidir(UI_DIR);
    {   /* DEVUI_PREVIEW_WALLPAPER=DIR: DIR/wallpaper.jpg and DIR/wallpaper.conf */
        const char *wd = getenv("DEVUI_PREVIEW_WALLPAPER");
        if (wd && wd[0]) {
            snprintf(g_wp_file, sizeof g_wp_file, "%s/wallpaper.jpg", wd);
            snprintf(g_wp_conf_file, sizeof g_wp_conf_file, "%s/wallpaper.conf", wd);
        }
        (void)wallpaper_poll();
    }
    (void)data_backend_init();
    (void)data_backend_commit_latest();
    if (device_update(data_model_tag())) scan_pages();   /* model from the sample state */
    {   /* one WiFi/aux sample, taken here instead of on the worker thread */
        static aux_sample_t a;
        aux_collect(&a, millis());
        g_aux = a;
        g_aux_gen++;
        aux_apply();
    }

    for (int i = 0; i < g_npages; i++) {
        const char *base = strrchr(g_pages[i], '/');
        g_cur = i;
        snprintf(name, sizeof name, "%.60s", base ? base + 1 : g_pages[i]);
        if (strrchr(name, '.')) *strrchr(name, '.') = 0;
        shot_page(&disp, out, name, g_pages[i]);
    }
    g_cur = 0;
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        if (!subpage_open(subs[i])) continue;
        snprintf(name, sizeof name, "sub-%.40s", subs[i]);
        *strrchr(name, '.') = 0;
        shot_page(&disp, out, name, active_page_path());
        subpage_close();
    }
#ifdef DEVUI_PLAIN_OPENWRT
    if (subpage_open("smsnew.html")) {      /* writing a message: the text with the letters, then the number */
        snprintf(g_cmp_num, sizeof g_cmp_num, "+85212345678");
        snprintf(g_cmp_text, sizeof g_cmp_text, "On my way, see you at 8");
        g_cmp_field = 1;
        shot_page(&disp, out, "sub-smsnew-text", active_page_path());
        g_cmp_layer = 2;
        shot_page(&disp, out, "sub-smsnew-sym", active_page_path());
        g_cmp_layer = 0;
        g_cmp_field = 0;
        subpage_close();
    }
    if (subpage_open("sms.html")) {         /* a message opened, delete asked once */
        snprintf(g_sms_num, sizeof g_sms_num, "10086");
        snprintf(g_sms_date, sizeof g_sms_date, "11-12 23:18");
        snprintf(g_sms_text, sizeof g_sms_text, "余额 12.50 元");
        g_sms_open = 100003;
        g_sms_del_armed = 100003;
        shot_page(&disp, out, "sub-sms-delete", active_page_path());
        g_sms_del_armed = -1;
        shot_page(&disp, out, "sub-sms-open", active_page_path());
        g_sms_del_armed = g_sms_open = -1;
        subpage_close();
    }
#endif
    snprintf(path, sizeof path, "%s/menu.html", UI_DIR);
    shot_page(&disp, out, "menu", path);
    g_pwr_confirm = 1;                      /* power off armed */
    shot_page(&disp, out, "menu-armed", path);
    g_pwr_confirm = 0;
    g_lock_state = 1;                       /* locked preview (no PIN: hold to unlock) */
    shot_page(&disp, out, "locked", g_pages[0]);
    g_lock_hold = 60;
    shot_page(&disp, out, "locked-hold", g_pages[0]);
    g_lock_hold = 0;
    snprintf(path, sizeof path, "%s/lockscreen.html", UI_DIR);
    g_lock_state = 3;
    shot_page(&disp, out, "lockscreen", path);
    g_lock_state = 0;
    data_backend_close();
    return 0;
}
