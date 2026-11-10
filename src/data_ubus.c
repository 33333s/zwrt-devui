/*
 * data_ubus.c - device state for plain OpenWrt, read over ubus and UCI.
 *
 * The alternative to data.c (the zwrt-datad HTTP/SSE client) for systems that
 * run neither the vendor daemons nor datad, such as the native OpenWrt of the
 * U60 Pro.  Build with scripts/build.sh BACKEND=ubus; the interface (data.h)
 * is the same, so the UI code does not know which one it has.
 *
 * What OpenWrt itself knows is read from its standard objects:
 *   system board / info          model, release, uptime, memory
 *   network.interface dump       the uplink (default route) and the LAN address
 *   network.device status        byte counters of the uplink
 *   network.wireless status      radios, the main access point, its key
 *   iwinfo assoclist             Wi-Fi stations
 *   uci dhcp.lan.*               the DHCP pool
 *   system reboot                restart
 *   network.wireless up / down   Wi-Fi switches
 *   u60pro.wifi status / set / radio   the device's own wireless service (the
 *                                QCA hostapd, which netifd cannot run): when it
 *                                answers, Wi-Fi is read and switched there
 *                                instead of network.wireless
 * and what it has no interface for from the device's own objects:
 *   u60pro.hw status             battery, charger, charging policy, temperatures
 *   u60pro.cell status           the cellular link (optional: without it the
 *                                uplink interface is shown instead)
 *   uci u60pro.screen.*          backlight and screen-off time (shared with LuCI)
 *
 * A sampler thread makes the calls once per interval through the ubus and uci
 * command line tools (DEVUI_UBUS / DEVUI_UCI name other programs, which is how
 * the tests run it), so nothing here links against libubus.
 *
 * SPDX-License-Identifier: MIT
 */
#include "data.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef DEVUI_UBUS_MODEL_TAG
#define DEVUI_UBUS_MODEL_TAG "MU5250"
#endif
#ifndef DEVUI_UBUS_INTERVAL_MS
#define DEVUI_UBUS_INTERVAL_MS 1000
#endif
#ifndef DEVUI_UBUS_TIMEOUT_MS
#define DEVUI_UBUS_TIMEOUT_MS 2500
#endif
#define HW "u60pro.hw"
#define CELL "u60pro.cell"
#define OWNWIFI "u60pro.wifi"
#define REPLY_MAX 65536

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static devui_data_t g_live, g_current;
static int g_live_valid, g_current_valid, g_started;
static unsigned long long g_version, g_seen_version, g_committed_version;
static volatile int g_interval_ms = DEVUI_UBUS_INTERVAL_MS;
static volatile int g_stop;
static pthread_t g_thread;
/* u60pro.screen as last read: brightness in per cent and the screen-off time in seconds, -1 while unknown */
static int g_scr_brightness = -1, g_scr_timeout = -1;

static uint32_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ JSON: the members of one object or array level
 * json.c finds a key anywhere below; replies such as network.interface dump
 * repeat the same keys at several depths, so these look at one level only. */
static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* the end of the value at p (one past it), or NULL when it is cut short */
static const char *value_end(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') {
        for (p++; *p; p++) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') return p + 1;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') {
                const char *e = value_end(p);
                if (!e) return NULL;
                p = e - 1;
            } else if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}

/* the value of `key` among the direct members of the object at obj, or NULL */
static const char *jfind(const char *obj, const char *key)
{
    size_t kl = strlen(key);
    const char *p;
    if (!obj) return NULL;
    p = skip_ws(obj);
    if (*p != '{') return NULL;
    p++;
    for (;;) {
        const char *ke, *v, *ve;
        p = skip_ws(p);
        if (*p != '"') return NULL;
        ke = value_end(p);
        if (!ke) return NULL;
        v = skip_ws(ke);
        if (*v != ':') return NULL;
        v = skip_ws(v + 1);
        if ((size_t)(ke - p) == kl + 2 && !memcmp(p + 1, key, kl)) return v;
        ve = value_end(v);
        if (!ve) return NULL;
        p = skip_ws(ve);
        if (*p != ',') return NULL;
        p++;
    }
}

/* a path of keys ("battery", "capacity", NULL) from obj down */
static const char *jpath(const char *obj, ...)
{
    va_list ap;
    const char *k;
    va_start(ap, obj);
    while (obj && (k = va_arg(ap, const char *)) != NULL) obj = jfind(obj, k);
    va_end(ap);
    return obj;
}

static int jstr(const char *v, char *dst, size_t n)
{
    size_t o = 0;
    if (n) dst[0] = 0;
    if (!v || !n) return 0;
    v = skip_ws(v);
    if (*v != '"') {
        const char *e = value_end(v);
        if (!e || !strncmp(v, "null", 4)) return 0;
        while (v < e && o + 1 < n) dst[o++] = *v++;
        dst[o] = 0;
        return o > 0;
    }
    for (v++; *v && *v != '"' && o + 1 < n; v++) {
        if (*v == '\\' && v[1]) {
            v++;
            if (*v == 'n') dst[o++] = '\n';
            else if (*v == 't') dst[o++] = '\t';
            else if (*v == 'u' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2]) &&
                     isxdigit((unsigned char)v[3]) && isxdigit((unsigned char)v[4])) {
                char hex[5] = { v[1], v[2], v[3], v[4], 0 };
                unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                v += 4;
                if (cp < 0x80) dst[o++] = (char)cp;
                else if (cp < 0x800) {
                    if (o + 2 >= n) break;
                    dst[o++] = (char)(0xC0 | (cp >> 6));
                    dst[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    if (o + 3 >= n) break;
                    dst[o++] = (char)(0xE0 | (cp >> 12));
                    dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    dst[o++] = (char)(0x80 | (cp & 0x3F));
                }
            } else dst[o++] = *v;
        } else dst[o++] = *v;
    }
    dst[o] = 0;
    return 1;
}

static int jnum(const char *v, double *out)
{
    char *end;
    double d;
    if (!v) return 0;
    v = skip_ws(v);
    if (*v == '"') v++;
    d = strtod(v, &end);
    if (end == v) return 0;
    *out = d;
    return 1;
}
static long jlong(const char *v, long def)
{
    double d;
    return jnum(v, &d) ? (long)(d < 0 ? d - 0.5 : d + 0.5) : def;
}
static int jbool(const char *v, int def)
{
    if (!v) return def;
    v = skip_ws(v);
    if (!strncmp(v, "true", 4) || *v == '1') return 1;
    if (!strncmp(v, "false", 5) || *v == '0') return 0;
    return def;
}

/* walk an array or the members of an object: *pp starts at the '[' or '{' and
 * moves on; for an object *key gets the member name (in keybuf).  Returns the
 * element, or NULL at the end. */
static const char *jnext(const char **pp, char *keybuf, size_t kn)
{
    const char *p = skip_ws(*pp), *v, *e;
    if (*p == '[' || *p == '{' || *p == ',') p = skip_ws(p + 1);
    if (!*p || *p == ']' || *p == '}') return NULL;
    v = p;
    if (keybuf) {
        const char *ke = value_end(p);
        if (*p != '"' || !ke) return NULL;
        jstr(p, keybuf, kn);
        v = skip_ws(ke);
        if (*v != ':') return NULL;
        v = skip_ws(v + 1);
    }
    e = value_end(v);
    if (!e) return NULL;
    *pp = skip_ws(e);
    return v;
}

/* str as a JSON string body (no quotes) */
static void jesc(const char *s, char *dst, size_t n)
{
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(dst + o, n - o, "\\u%04x", c);
        else dst[o++] = (char)c;
    }
    dst[o] = 0;
}

/* ------------------------------------------------------------------ running ubus and uci */
/* argv's standard output into out; returns the exit status, or -1 (not started, timed out) */
static int run_capture(char *const argv[], char *out, size_t cap, int timeout_ms)
{
    int fds[2], status = -1;
    size_t len = 0;
    pid_t pid;
    uint32_t t0 = mono_ms();

    if (out && cap) out[0] = 0;
    if (pipe(fds) < 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(fds[1], 1);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        close(fds[0]); close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    for (;;) {
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        int left = timeout_ms - (int)(mono_ms() - t0), r;
        char tmp[4096];
        ssize_t n;
        if (left <= 0) break;
        r = poll(&pfd, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n = read(fds[0], tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { left = -1; status = 0; break; }
        if (out && cap) {
            size_t take = (size_t)n < cap - 1 - len ? (size_t)n : cap - 1 - len;
            memcpy(out + len, tmp, take);
            len += take;
            out[len] = 0;
        }
    }
    close(fds[0]);
    if (status != 0) {   /* timed out: do not leave it behind */
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static const char *ubus_bin(void)
{
    const char *e = getenv("DEVUI_UBUS");
    return e && e[0] ? e : "ubus";
}
static const char *uci_bin(void)
{
    const char *e = getenv("DEVUI_UCI");
    return e && e[0] ? e : "uci";
}

/* ubus call obj method args: 1 when the call succeeded (the reply, JSON, is in out) */
static int ubus_call(const char *obj, const char *method, const char *args, char *out, size_t cap)
{
    char *argv[9];
    int n = 0;
    argv[n++] = (char *)ubus_bin();
    argv[n++] = "-S";
    argv[n++] = "-t";
    argv[n++] = "2";
    argv[n++] = "call";
    argv[n++] = (char *)obj;
    argv[n++] = (char *)method;
    if (args) argv[n++] = (char *)args;
    argv[n] = NULL;
    return run_capture(argv, out, cap, DEVUI_UBUS_TIMEOUT_MS) == 0;
}

static int uci_get(const char *key, char *out, size_t cap)
{
    char *argv[] = { (char *)uci_bin(), "-q", "get", (char *)key, NULL };
    size_t l;
    if (run_capture(argv, out, cap, DEVUI_UBUS_TIMEOUT_MS) != 0) { if (cap) out[0] = 0; return 0; }
    l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    return l > 0;
}

static int uci_set(const char *key, const char *value)
{
    char kv[256];
    char *argv[] = { (char *)uci_bin(), "-q", "set", kv, NULL };
    snprintf(kv, sizeof kv, "%s=%s", key, value);
    return run_capture(argv, NULL, 0, DEVUI_UBUS_TIMEOUT_MS) == 0;
}

static int uci_commit(const char *config)
{
    char *argv[] = { (char *)uci_bin(), "-q", "commit", (char *)config, NULL };
    return run_capture(argv, NULL, 0, DEVUI_UBUS_TIMEOUT_MS) == 0;
}

/* ------------------------------------------------------------------ one sample */
struct sampler {
    char buf[REPLY_MAX], buf2[REPLY_MAX];
    /* fetched once or rarely */
    char model[64], fw[80], kernel[80];
    int have_board;
    char dhcp_start[8], dhcp_limit[8], dhcp_leasetime[12];
    unsigned round;
    /* rates */
    char rate_dev[32];
    long long rate_rx, rate_tx;
    uint32_t rate_t;
    int rate_valid;
    /* cpu */
    unsigned long long cpu_total, cpu_idle;
    int cpu_valid;
};

static void up_str(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static void sample_hw(struct sampler *s, devui_data_t *d)
{
    const char *b, *c, *g, *t;
    char st[24];
    double v;

    d->dps = -1;
    if (!ubus_call(HW, "status", NULL, s->buf, sizeof s->buf)) return;
    b = jfind(s->buf, "battery");
    c = jfind(s->buf, "charger");
    g = jfind(s->buf, "charge");
    t = jfind(s->buf, "thermal");
    if (jbool(jfind(b, "available"), 0)) {
        d->bat_percent = (int)jlong(jfind(b, "capacity"), 0);
        d->bat_temp = (int)jlong(jfind(b, "temp_c"), 0);
        d->bat_uv = jlong(jfind(b, "voltage_mv"), 0) * 1000;
        d->bat_ua = jlong(jfind(b, "current_ma"), 0) * 1000;
        jstr(jfind(b, "status"), st, sizeof st);
        /* the vendor's charge_status codes the pages expect */
        d->charging = !strcmp(st, "Charging") ? 1 : !strcmp(st, "Full") ? 4 : !strcmp(st, "Not charging") ? 3 : 2;
    }
    d->charger_connect = jbool(jfind(c, "online"), 0);
    d->chg_uv = jlong(jfind(c, "voltage_mv"), 0) * 1000;
    d->chg_ua = jlong(jfind(c, "current_ma"), 0) * 1000;
    if (jbool(jfind(g, "policy_available"), 0)) {
        jstr(jfind(g, "mode"), st, sizeof st);
        d->dps = !strcmp(st, "bypass") || jbool(jfind(g, "paused"), 0);
    }
    if (jnum(jfind(t, "soc_c"), &v)) d->cpu_temp = (long)(v + 0.5);
}

static void sample_system(struct sampler *s, devui_data_t *d)
{
    FILE *fp;

    if (!s->have_board && ubus_call("system", "board", NULL, s->buf, sizeof s->buf)) {
        jstr(jfind(s->buf, "model"), s->model, sizeof s->model);
        jstr(jpath(s->buf, "release", "description", NULL), s->fw, sizeof s->fw);
        jstr(jfind(s->buf, "kernel"), s->kernel, sizeof s->kernel);
        s->have_board = s->model[0] || s->fw[0];
    }
    snprintf(d->model, sizeof d->model, "%s", s->model);
    snprintf(d->fw, sizeof d->fw, "%s", s->fw);
    snprintf(d->sw_version, sizeof d->sw_version, "%s", s->kernel);

    if (ubus_call("system", "info", NULL, s->buf, sizeof s->buf)) {
        const char *m = jfind(s->buf, "memory");
        double total = 0, avail = 0;
        d->uptime = jlong(jfind(s->buf, "uptime"), 0);
        jnum(jfind(m, "total"), &total);
        if (!jnum(jfind(m, "available"), &avail)) jnum(jfind(m, "free"), &avail);
        d->mem_total = (long)total;
        d->mem_avail = (long)avail;
        d->mem_used_pct = total > 0 ? (long)((total - avail) * 100 / total + 0.5) : 0;
    }

    /* the processor's load since the previous sample: no ubus object reports it */
    d->cpu_usage = -1;
    fp = fopen("/proc/stat", "r");
    if (fp) {
        unsigned long long v[8] = { 0 }, total = 0;
        if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
            for (int i = 0; i < 8; i++) total += v[i];
            if (s->cpu_valid && total > s->cpu_total)
                d->cpu_usage = (long)(100 - (v[3] + v[4] - s->cpu_idle) * 100 / (total - s->cpu_total));
            s->cpu_total = total;
            s->cpu_idle = v[3] + v[4];
            s->cpu_valid = 1;
        }
        fclose(fp);
    }
}

/* the uplink: the interface with the IPv4 default route of the lowest metric */
static void sample_network(struct sampler *s, devui_data_t *d, char *uplink, size_t un, char *proto, size_t pn)
{
    const char *arr, *p, *it;
    long best = -1;
    char dev[32] = "";

    uplink[0] = proto[0] = 0;
    if (!ubus_call("network.interface", "dump", NULL, s->buf, sizeof s->buf)) return;
    arr = jfind(s->buf, "interface");
    if (!arr) return;
    for (p = arr; (it = jnext(&p, NULL, 0)) != NULL; ) {
        char name[32], l3[32];
        const char *routes, *rp, *r;
        jstr(jfind(it, "interface"), name, sizeof name);
        if (!strcmp(name, "lan")) {
            const char *a4 = jfind(it, "ipv4-address"), *ap = a4, *a;
            if (a4 && (a = jnext(&ap, NULL, 0)) != NULL) jstr(jfind(a, "address"), d->dhcp_ip, sizeof d->dhcp_ip);
        }
        if (!jbool(jfind(it, "up"), 0) || !strcmp(name, "loopback")) continue;
        routes = jfind(it, "route");
        for (rp = routes; routes && (r = jnext(&rp, NULL, 0)) != NULL; ) {
            char target[48];
            long metric = jlong(jfind(it, "metric"), 0);
            jstr(jfind(r, "target"), target, sizeof target);
            if (jlong(jfind(r, "mask"), -1) != 0 || strcmp(target, "0.0.0.0")) continue;
            if (best >= 0 && metric >= best) continue;
            best = metric;
            snprintf(uplink, un, "%s", name);
            jstr(jfind(it, "proto"), proto, pn);
            if (!jstr(jfind(it, "l3_device"), l3, sizeof l3)) jstr(jfind(it, "device"), l3, sizeof l3);
            snprintf(dev, sizeof dev, "%s", l3);
        }
    }
    snprintf(d->wan_status, sizeof d->wan_status, "%s", uplink[0] ? "connected" : "idle");

    if (dev[0]) {
        char args[64], esc[40];
        jesc(dev, esc, sizeof esc);
        snprintf(args, sizeof args, "{\"name\":\"%s\"}", esc);
        if (ubus_call("network.device", "status", args, s->buf, sizeof s->buf)) {
            const char *st = jfind(s->buf, "statistics");
            double rx = 0, tx = 0;
            if (jnum(jfind(st, "rx_bytes"), &rx) && jnum(jfind(st, "tx_bytes"), &tx)) {
                uint32_t now = mono_ms();
                d->rx_bytes = (long)rx;
                d->tx_bytes = (long)tx;
                if (s->rate_valid && !strcmp(s->rate_dev, dev) && now != s->rate_t &&
                    (long long)rx >= s->rate_rx && (long long)tx >= s->rate_tx) {
                    uint32_t dt = now - s->rate_t;
                    d->rx_speed = (long)(((long long)rx - s->rate_rx) * 1000 / dt);
                    d->tx_speed = (long)(((long long)tx - s->rate_tx) * 1000 / dt);
                }
                snprintf(s->rate_dev, sizeof s->rate_dev, "%s", dev);
                s->rate_rx = (long long)rx;
                s->rate_tx = (long long)tx;
                s->rate_t = now;
                s->rate_valid = 1;
            }
        }
    } else s->rate_valid = 0;
}

/* The device's own wireless service: {enabled, up, ssid, key, encryption, radios: [{name, disabled, up, band, ssid,
 * encryption, stations}, ...]}.  1 when it answered (buf then holds the reply). */
static int own_wifi_status(char *buf, size_t cap)
{
    return ubus_call(OWNWIFI, "status", NULL, buf, cap) && jfind(buf, "radios") != NULL;
}

/* its band names ("2g", "5g", "5gl", "5gh", "6g") as the three bands the pages know: 0, 1, 2, or -1 */
static int own_band_index(const char *band)
{
    return band[0] == '2' ? 0 : band[0] == '5' ? 1 : band[0] == '6' ? 2 : -1;
}

/* Wi-Fi from the own service; returns the number of stations, -1 when the service is not there */
static int sample_own_wifi(struct sampler *s, devui_data_t *d)
{
    const char *arr, *ap, *it;
    int stations = 0, first = 1;

    if (!own_wifi_status(s->buf, sizeof s->buf)) return -1;
    d->wifi_enabled = jbool(jfind(s->buf, "up"), jbool(jfind(s->buf, "enabled"), 0));
    jstr(jfind(s->buf, "ssid"), d->wifi_ssid, sizeof d->wifi_ssid);
    jstr(jfind(s->buf, "key"), d->wifi_key, sizeof d->wifi_key);
    jstr(jfind(s->buf, "encryption"), d->wifi_enc, sizeof d->wifi_enc);
    arr = jfind(s->buf, "radios");
    for (ap = arr; arr && (it = jnext(&ap, NULL, 0)) != NULL; ) {
        char ssid[sizeof d->wifi_ssid], enc[sizeof d->wifi_enc];
        if (jbool(jfind(it, "disabled"), 0)) continue;
        if (first) {
            /* the first radio in use names the network on the home page */
            if (jstr(jfind(it, "ssid"), ssid, sizeof ssid) && ssid[0]) snprintf(d->wifi_ssid, sizeof d->wifi_ssid, "%s", ssid);
            if (jstr(jfind(it, "encryption"), enc, sizeof enc) && enc[0]) snprintf(d->wifi_enc, sizeof d->wifi_enc, "%s", enc);
            first = 0;
        }
        if (jbool(jfind(it, "up"), 0)) stations += (int)jlong(jfind(it, "stations"), 0);
    }
    return stations;
}

/* radios and the first access point; returns the number of stations on all access points */
static int sample_wifi(struct sampler *s, devui_data_t *d)
{
    const char *p, *radio;
    char rname[32], ifnames[4][24];
    int nif = 0, any_up = 0, radios = 0, stations = 0;

    d->wifi_enabled = 0;
    if ((stations = sample_own_wifi(s, d)) >= 0) return stations;
    stations = 0;
    if (!ubus_call("network.wireless", "status", NULL, s->buf, sizeof s->buf)) return 0;
    for (p = s->buf; (radio = jnext(&p, rname, sizeof rname)) != NULL; ) {
        const char *ifs = jfind(radio, "interfaces"), *ip = ifs, *it;
        int up = jbool(jfind(radio, "up"), 0);
        radios++;
        if (up) any_up = 1;
        while (ifs && (it = jnext(&ip, NULL, 0)) != NULL) {
            const char *cfg = jfind(it, "config");
            char mode[12];
            jstr(jfind(cfg, "mode"), mode, sizeof mode);
            if (strcmp(mode, "ap")) continue;
            if (!d->wifi_ssid[0] || (up && !nif)) {
                jstr(jfind(cfg, "ssid"), d->wifi_ssid, sizeof d->wifi_ssid);
                jstr(jfind(cfg, "key"), d->wifi_key, sizeof d->wifi_key);
                jstr(jfind(cfg, "encryption"), d->wifi_enc, sizeof d->wifi_enc);
            }
            if (up && nif < 4 && jstr(jfind(it, "ifname"), ifnames[nif], sizeof ifnames[0])) nif++;
        }
    }
    d->wifi_enabled = radios ? any_up : 0;
    for (int i = 0; i < nif; i++) {
        char args[64], esc[40];
        const char *res, *rp;
        jesc(ifnames[i], esc, sizeof esc);
        snprintf(args, sizeof args, "{\"device\":\"%s\"}", esc);
        if (!ubus_call("iwinfo", "assoclist", args, s->buf2, sizeof s->buf2)) continue;
        res = jfind(s->buf2, "results");
        for (rp = res; res && jnext(&rp, NULL, 0) != NULL; ) stations++;
    }
    return stations;
}

/* DHCP leases of dnsmasq (expiry, MAC, address, name, client id per line): the client list */
static void sample_clients(devui_data_t *d, int stations)
{
    const char *path = getenv("DEVUI_DHCP_LEASES");
    FILE *fp = fopen(path && path[0] ? path : "/tmp/dhcp.leases", "r");
    char line[256];

    d->client_n = 0;
    d->clients_total = 0;
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            char mac[20], ip[48], name[64];
            long exp;
            if (sscanf(line, "%ld %19s %47s %63s", &exp, mac, ip, name) < 3) continue;
            d->clients_total++;
            if (d->client_n >= 16) continue;
            snprintf(d->client[d->client_n].mac, sizeof d->client[0].mac, "%s", mac);
            snprintf(d->client[d->client_n].ip, sizeof d->client[0].ip, "%.23s", ip);
            snprintf(d->client[d->client_n].name, sizeof d->client[0].name, "%.39s", strcmp(name, "*") ? name : "");
            d->client_n++;
        }
        fclose(fp);
    }
    if (stations > d->clients_total) d->clients_total = stations;
    d->clients_wifi = stations;
    d->clients_lan = d->clients_total - stations;
}

/* a signal-to-noise ratio as the pages show it: one decimal, whatever the service printed (a poller in shell gives
 * -6.5999999999999996 for -6.6) */
static void sinr_text(const char *v, char *dst, size_t n)
{
    double x;
    dst[0] = 0;
    if (v && jnum(v, &x)) snprintf(dst, n, "%.1f", x);
}

/* the cellular link from u60pro.cell; returns 0 when no modem service publishes it */
static int sample_cell(struct sampler *s, devui_data_t *d)
{
    const char *nr, *lte;
    char rat[12], mode[12];
    double v;

    if (!ubus_call(CELL, "status", NULL, s->buf, sizeof s->buf) || !jfind(s->buf, "connected")) return 0;
    jstr(jfind(s->buf, "rat"), rat, sizeof rat);
    jstr(jfind(s->buf, "mode"), mode, sizeof mode);
    snprintf(d->net_type, sizeof d->net_type, "%.7s%s%.7s", rat, rat[0] && mode[0] ? " " : "", mode);
    jstr(jfind(s->buf, "operator"), d->operator_name, sizeof d->operator_name);
    jstr(jfind(s->buf, "band"), d->band, sizeof d->band);
    d->bars = (int)jlong(jfind(s->buf, "bars"), 0);
    d->mcc = (int)jlong(jfind(s->buf, "mcc"), 0);
    d->mnc = (int)jlong(jfind(s->buf, "mnc"), 0);
    jstr(jfind(s->buf, "nrca"), d->nrca, sizeof d->nrca);
    jstr(jfind(s->buf, "lteca"), d->lteca, sizeof d->lteca);
    jstr(jfind(s->buf, "ltecasig"), d->ltecasig, sizeof d->ltecasig);
    jstr(jfind(s->buf, "net_select"), d->net_select, sizeof d->net_select);
    jstr(jfind(s->buf, "sa_bands"), d->sa_bands, sizeof d->sa_bands);
    jstr(jfind(s->buf, "nsa_bands"), d->nsa_bands, sizeof d->nsa_bands);
    jstr(jfind(s->buf, "lte_bands"), d->lte_bands, sizeof d->lte_bands);
    jstr(jfind(s->buf, "imei"), d->imei, sizeof d->imei);
    d->qci = (int)jlong(jfind(s->buf, "qci"), 0);
    if (jnum(jfind(s->buf, "ambr_dl"), &v)) d->ambr_dl = v;
    if (jnum(jfind(s->buf, "ambr_ul"), &v)) d->ambr_ul = v;
    d->sms_unread = (int)jlong(jfind(s->buf, "sms_unread"), 0);
    if (!jbool(jfind(s->buf, "connected"), 0)) snprintf(d->wan_status, sizeof d->wan_status, "idle");

    nr = jfind(s->buf, "nr");
    if (nr) {
        jstr(jfind(nr, "band"), d->nr_band, sizeof d->nr_band);
        d->nr_rsrp = (int)jlong(jfind(nr, "rsrp"), 0);
        d->nr_rsrq = (int)jlong(jfind(nr, "rsrq"), 0);
        d->nr_rssi = (int)jlong(jfind(nr, "rssi"), 0);
        sinr_text(jfind(nr, "sinr"), d->nr_snr, sizeof d->nr_snr);
        d->nr_pci = (int)jlong(jfind(nr, "pci"), 0);
        d->nr_cell_id = jlong(jfind(nr, "cell_id"), 0);
        d->nr_channel = jlong(jfind(nr, "arfcn"), 0);
        jstr(jfind(nr, "bw"), d->nr_bw, sizeof d->nr_bw);
        d->rssi = d->nr_rssi;
    }
    lte = jfind(s->buf, "lte");
    if (lte) {
        if (!d->band[0]) jstr(jfind(lte, "band"), d->band, sizeof d->band);
        d->lte_rsrp = (int)jlong(jfind(lte, "rsrp"), 0);
        d->lte_rsrq = (int)jlong(jfind(lte, "rsrq"), 0);
        d->lte_rssi = (int)jlong(jfind(lte, "rssi"), 0);
        sinr_text(jfind(lte, "sinr"), d->lte_snr, sizeof d->lte_snr);
        if (!nr) d->rssi = d->lte_rssi;
    }
    return 1;
}

/* dhcp.lan: the pool as the pages show it (first address, size, lease time) */
static void sample_dhcp(struct sampler *s, devui_data_t *d)
{
    if (s->round % 30 == 0) {
        uci_get("dhcp.lan.start", s->dhcp_start, sizeof s->dhcp_start);
        uci_get("dhcp.lan.limit", s->dhcp_limit, sizeof s->dhcp_limit);
        uci_get("dhcp.lan.leasetime", s->dhcp_leasetime, sizeof s->dhcp_leasetime);
    }
    if (d->dhcp_ip[0] && s->dhcp_start[0]) {
        unsigned a, b, c, e;
        if (sscanf(d->dhcp_ip, "%u.%u.%u.%u", &a, &b, &c, &e) == 4)
            snprintf(d->dhcp_start, sizeof d->dhcp_start, "%u.%u.%u.%d", a, b, c, atoi(s->dhcp_start));
    }
    snprintf(d->dhcp_limit, sizeof d->dhcp_limit, "%s", s->dhcp_limit);
    {   /* the pages take seconds; UCI has "12h", "30m", "2d", plain seconds or "infinite" */
        char *end;
        long n = strtol(s->dhcp_leasetime, &end, 10);
        if (end != s->dhcp_leasetime && n > 0) {
            if (*end == 'h') n *= 3600;
            else if (*end == 'm') n *= 60;
            else if (*end == 'd') n *= 86400;
            else if (*end == 'w') n *= 604800;
            char tmp[24];
            snprintf(tmp, sizeof tmp, "%ld", n);
            snprintf(d->dhcp_leasetime, sizeof d->dhcp_leasetime, "%.11s", tmp);
        } else snprintf(d->dhcp_leasetime, sizeof d->dhcp_leasetime, "%.11s", s->dhcp_leasetime);
    }
}

static void sample_screen(struct sampler *s)
{
    char v[16];
    int b = -1, t = -1;
    if (s->round % 5) return;
    if (uci_get("u60pro.screen.brightness", v, sizeof v)) b = atoi(v);
    if (uci_get("u60pro.screen.timeout", v, sizeof v)) t = atoi(v);
    pthread_mutex_lock(&g_lock);
    g_scr_brightness = b;
    g_scr_timeout = t;
    pthread_mutex_unlock(&g_lock);
}

static void sample(struct sampler *s, devui_data_t *d)
{
    char uplink[32], proto[16];
    int stations;

    memset(d, 0, sizeof *d);
    d->als_enabled = -1;
    d->als_lux = -1;
    d->neighbor_enabled = -1;
    snprintf(d->model_tag, sizeof d->model_tag, "%s", DEVUI_UBUS_MODEL_TAG);
    snprintf(d->datad_version, sizeof d->datad_version, "ubus");
    d->sample_interval_ms = g_interval_ms;

    sample_hw(s, d);
    sample_system(s, d);
    sample_network(s, d, uplink, sizeof uplink, proto, sizeof proto);
    stations = sample_wifi(s, d);
    sample_clients(d, stations);
    if (!sample_cell(s, d) && uplink[0]) {
        /* no modem service: the uplink interface stands for the operator */
        snprintf(d->operator_name, sizeof d->operator_name, "%s", uplink);
        up_str(d->operator_name);
        d->bars = 4;
    }
    sample_dhcp(s, d);
    sample_screen(s);
    s->round++;
    d->valid = 1;
}

static struct sampler g_sampler;

static void sample_publish(void)
{
    static devui_data_t d;
    sample(&g_sampler, &d);
    pthread_mutex_lock(&g_lock);
    g_live = d;
    g_live_valid = 1;
    g_version++;
    pthread_mutex_unlock(&g_lock);
}

static void *sampler_main(void *arg)
{
    (void)arg;
    while (!g_stop) {
        uint32_t t0 = mono_ms();
        int wait;
        sample_publish();
        wait = g_interval_ms - (int)(mono_ms() - t0);
        if (wait < 100) wait = 100;
        while (wait > 0 && !g_stop) {
            usleep(50000);
            wait -= 50;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ data.h */
int data_backend_init(void)
{
    const char *warm = getenv("DEVUI_UBUS_WARMUP_MS");
    if (g_started) return 1;
    g_stop = 0;
    /* the first snapshot is taken here, so the first frame already has data;
     * DEVUI_UBUS_WARMUP_MS (the host preview) waits and takes a second one,
     * which is what it takes to have data rates */
    sample_publish();
    if (warm && atoi(warm) > 0) {
        usleep((useconds_t)atoi(warm) * 1000);
        sample_publish();
    }
    if (pthread_create(&g_thread, NULL, sampler_main, NULL) != 0) return 0;
    g_started = 1;
    return 1;
}

int data_backend_poll(uint32_t now_ms)
{
    int changed;
    (void)now_ms;
    if (!g_started) data_backend_init();
    pthread_mutex_lock(&g_lock);
    changed = g_version != g_seen_version;
    g_seen_version = g_version;
    pthread_mutex_unlock(&g_lock);
    return changed;
}

int data_backend_commit_latest(void)
{
    int changed = 0;
    pthread_mutex_lock(&g_lock);
    if (g_live_valid && g_committed_version != g_version) {
        g_current = g_live;
        g_current_valid = 1;
        g_committed_version = g_version;
        changed = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return changed;
}

unsigned data_backend_generation(void) { return 1; }

void data_backend_close(void)
{
    if (!g_started) return;
    g_stop = 1;
    pthread_join(g_thread, NULL);
    g_started = 0;
}

long data_ambient_light(int *enabled)
{
    if (enabled) *enabled = -1;
    return -1;
}

const char *data_model_tag(void) { return DEVUI_UBUS_MODEL_TAG; }

int data_refresh(devui_data_t *d)
{
    int ok;
    pthread_mutex_lock(&g_lock);
    if (!g_current_valid && g_live_valid) {
        g_current = g_live;
        g_current_valid = 1;
        g_committed_version = g_version;
    }
    ok = g_current_valid;
    if (ok) *d = g_current;
    pthread_mutex_unlock(&g_lock);
    if (!ok) memset(d, 0, sizeof *d);
    return ok;
}

int data_refresh_live(devui_data_t *d)
{
    int ok;
    pthread_mutex_lock(&g_lock);
    ok = g_live_valid;
    if (ok) *d = g_live;
    pthread_mutex_unlock(&g_lock);
    if (!ok) memset(d, 0, sizeof *d);
    return ok;
}

int data_screen_settings(int *brightness_pct, int *timeout_s)
{
    int ok;
    pthread_mutex_lock(&g_lock);
    ok = g_scr_brightness >= 0 || g_scr_timeout >= 0;
    if (brightness_pct) *brightness_pct = g_scr_brightness;
    if (timeout_s) *timeout_s = g_scr_timeout;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void data_screen_settings_save(int brightness_pct, int timeout_s)
{
    char params[64];
    int changed = 0;
    pthread_mutex_lock(&g_lock);
    if (brightness_pct >= 1 && brightness_pct != g_scr_brightness) { g_scr_brightness = brightness_pct; changed |= 1; }
    if (timeout_s >= 0 && timeout_s != g_scr_timeout) { g_scr_timeout = timeout_s; changed |= 2; }
    pthread_mutex_unlock(&g_lock);
    if (!changed) return;
    /* written by a background job: uci is three short processes */
    snprintf(params, sizeof params, "{\"brightness\":%d,\"timeout\":%d}", (changed & 1) ? brightness_pct : -1, (changed & 2) ? timeout_s : -1);
    datad_control_async("screen.save", params, NULL);
}

/* There is no datad: its extra HTTP interfaces do not exist here. */
int datad_request(const char *method, const char *path, const char *body,
                  char *out, size_t cap, int timeout_ms)
{
    (void)method; (void)path; (void)body; (void)timeout_ms;
    if (out && cap) out[0] = 0;
    return 404;
}

/* ------------------------------------------------------------------ actions
 * The pages name actions as datad does; here each one is carried out with the
 * matching standard call, and the replies of the two queries are given in
 * datad's shape ({"ok":true,"result":{...}}) so that their parsers are shared. */
static void put_reply(char *out, size_t cap, const char *text)
{
    if (out && cap) snprintf(out, cap, "%s", text);
}

/* "2g" / "5g" / "6g" -> the radio of that band and whether it is up; 1 when found */
static int radio_of_band(const char *status, const char *band, char *name, size_t nn, int *up)
{
    const char *p = status, *radio;
    char rname[32], b[8];
    while ((radio = jnext(&p, rname, sizeof rname)) != NULL) {
        jstr(jpath(radio, "config", "band", NULL), b, sizeof b);
        if (strcmp(b, band)) continue;
        if (name) snprintf(name, nn, "%s", rname);
        if (up) *up = jbool(jfind(radio, "up"), 0);
        return 1;
    }
    return 0;
}

/* The two queries from the own service: a band is on when one of its radios is in use and Wi-Fi as a whole is enabled;
 * a radio that is switched off still shows its band, so that it can be switched on again. */
static void own_wifi_bands(const char *st, int present[3], int on[3], char ssid[3][64])
{
    const char *arr = jfind(st, "radios"), *ap = arr, *it;
    char main_ssid[64];
    int enabled = jbool(jfind(st, "enabled"), 0);
    jstr(jfind(st, "ssid"), main_ssid, sizeof main_ssid);
    for (int i = 0; i < 3; i++) { present[i] = on[i] = 0; ssid[i][0] = 0; }
    while (arr && (it = jnext(&ap, NULL, 0)) != NULL) {
        char band[8], own[64];
        int bi, off = jbool(jfind(it, "disabled"), 0);
        jstr(jfind(it, "band"), band, sizeof band);
        if ((bi = own_band_index(band)) < 0) continue;
        present[bi] = 1;
        if (!off && enabled) on[bi] = 1;
        if (!ssid[bi][0] || !off)
            snprintf(ssid[bi], sizeof ssid[0], "%s", jstr(jfind(it, "ssid"), own, sizeof own) && own[0] ? own : main_ssid);
    }
}

static int act_own_wifi_state(const char *st, char *out, size_t cap)
{
    static const char *const bands[] = { "2g", "5g", "6g" };
    char reply[512], ssid[3][64];
    int present[3], on[3], o, n = 0;
    own_wifi_bands(st, present, on, ssid);
    o = snprintf(reply, sizeof reply, "{\"ok\":true,\"result\":{\"busy\":false,\"radios\":[");
    for (int i = 0; i < 3; i++)
        if (present[i])
            o += snprintf(reply + o, sizeof reply - (size_t)o, "%s{\"band\":\"%s\",\"enabled\":%s}", n++ ? "," : "", bands[i], on[i] ? "true" : "false");
    snprintf(reply + o, sizeof reply - (size_t)o, "],\"module\":{\"enabled\":%s}}}", jbool(jfind(st, "enabled"), 0) ? "true" : "false");
    put_reply(out, cap, reply);
    return 1;
}

static int act_own_wifi_status(const char *st, char *out, size_t cap)
{
    static const char *const bands[] = { "2g", "5g", "6g" };
    char reply[1024], ssid[3][64], key[64], es[140], ek[140];
    int present[3], on[3], o, n = 0;
    own_wifi_bands(st, present, on, ssid);
    jstr(jfind(st, "key"), key, sizeof key);
    jesc(key, ek, sizeof ek);
    o = snprintf(reply, sizeof reply, "{\"ok\":true,\"result\":{");
    for (int i = 0; i < 3; i++) {
        if (!present[i]) continue;
        jesc(ssid[i], es, sizeof es);
        o += snprintf(reply + o, sizeof reply - (size_t)o, "%s\"main_%s\":{\"ssid\":\"%s\",\"key\":\"%s\"}", n++ ? "," : "", bands[i], es, ek);
    }
    snprintf(reply + o, sizeof reply - (size_t)o, "}}");
    put_reply(out, cap, reply);
    return 1;
}

/* a reply of the own service: {"ok": true} or {"ok": false, "error": ...} */
static int own_ok(const char *reply)
{
    return jbool(jfind(reply, "ok"), 1);
}

/* switch every radio of a band ("2g" / "5g" / "6g") of the own service; 0 when it has none or a call failed */
static int own_wifi_radio_set(const char *st, const char *band, int enabled)
{
    const char *arr = jfind(st, "radios"), *ap = arr, *it;
    int want = own_band_index(band), done = 0, ok = 1;
    while (want >= 0 && arr && (it = jnext(&ap, NULL, 0)) != NULL) {
        char b[8], name[32], esc[40], args[96], reply[256];
        jstr(jfind(it, "band"), b, sizeof b);
        if (own_band_index(b) != want || !jstr(jfind(it, "name"), name, sizeof name) || !name[0]) continue;
        jesc(name, esc, sizeof esc);
        snprintf(args, sizeof args, "{\"name\":\"%s\",\"disabled\":%s}", esc, enabled ? "false" : "true");
        if (!ubus_call(OWNWIFI, "radio", args, reply, sizeof reply) || !own_ok(reply)) ok = 0;
        done++;
    }
    return done && ok;
}

static int act_wifi_state(char *out, size_t cap)
{
    static const char *const bands[] = { "2g", "5g", "6g" };
    char *st = malloc(REPLY_MAX), reply[512];
    int any = 0, o, ok = 0;
    if (!st) return 0;
    if (own_wifi_status(st, REPLY_MAX)) {
        ok = act_own_wifi_state(st, out, cap);
    } else if (ubus_call("network.wireless", "status", NULL, st, REPLY_MAX)) {
        o = snprintf(reply, sizeof reply, "{\"ok\":true,\"result\":{\"busy\":false,\"radios\":[");
        for (int i = 0, n = 0; i < 3; i++) {
            int up = 0;
            if (!radio_of_band(st, bands[i], NULL, 0, &up)) continue;
            o += snprintf(reply + o, sizeof reply - (size_t)o, "%s{\"band\":\"%s\",\"enabled\":%s}", n++ ? "," : "", bands[i], up ? "true" : "false");
            if (up) any = 1;
        }
        snprintf(reply + o, sizeof reply - (size_t)o, "],\"module\":{\"enabled\":%s}}}", any ? "true" : "false");
        put_reply(out, cap, reply);
        ok = 1;
    }
    free(st);
    return ok;
}

static int act_wifi_status(char *out, size_t cap)
{
    static const char *const bands[] = { "2g", "5g", "6g" };
    char *st = malloc(REPLY_MAX), reply[1024];
    int o, ok = 0;
    if (!st) return 0;
    if (own_wifi_status(st, REPLY_MAX)) {
        ok = act_own_wifi_status(st, out, cap);
    } else if (ubus_call("network.wireless", "status", NULL, st, REPLY_MAX)) {
        const char *p = st, *radio;
        char rname[32];
        o = snprintf(reply, sizeof reply, "{\"ok\":true,\"result\":{");
        for (int n = 0; (radio = jnext(&p, rname, sizeof rname)) != NULL; ) {
            const char *ifs = jfind(radio, "interfaces"), *ip = ifs, *it;
            char b[8], ssid[64], key[64], es[140], ek[140], mode[12];
            int bi = -1;
            jstr(jpath(radio, "config", "band", NULL), b, sizeof b);
            for (int i = 0; i < 3; i++) if (!strcmp(b, bands[i])) bi = i;
            if (bi < 0) continue;
            while (ifs && (it = jnext(&ip, NULL, 0)) != NULL) {
                const char *cfg = jfind(it, "config");
                jstr(jfind(cfg, "mode"), mode, sizeof mode);
                if (strcmp(mode, "ap")) continue;
                jstr(jfind(cfg, "ssid"), ssid, sizeof ssid);
                jstr(jfind(cfg, "key"), key, sizeof key);
                jesc(ssid, es, sizeof es);
                jesc(key, ek, sizeof ek);
                o += snprintf(reply + o, sizeof reply - (size_t)o, "%s\"main_%s\":{\"ssid\":\"%s\",\"key\":\"%s\"}", n++ ? "," : "", bands[bi], es, ek);
                break;
            }
            if ((size_t)o >= sizeof reply - 300) break;
        }
        snprintf(reply + o, sizeof reply - (size_t)o, "}}");
        put_reply(out, cap, reply);
        ok = 1;
    }
    free(st);
    return ok;
}

int datad_control(const char *action, const char *params_json,
                  char *out, size_t cap, int timeout_ms)
{
    const char *params = params_json && params_json[0] ? params_json : "{}";
    char args[512], reply[512];
    int ok = 0;
    (void)timeout_ms;

    if (out && cap) out[0] = 0;
    if (!action) return 0;
    if (!strcmp(action, "wifi.state")) return act_wifi_state(out, cap);
    if (!strcmp(action, "wifi.status")) return act_wifi_status(out, cap);

    if (!strcmp(action, "wifi.set_module")) {
        char *st = malloc(REPLY_MAX);
        int on = jbool(jfind(params, "enabled"), 1);
        if (st && own_wifi_status(st, REPLY_MAX)) {
            snprintf(args, sizeof args, "{\"enabled\":%s}", on ? "true" : "false");
            ok = ubus_call(OWNWIFI, "set", args, reply, sizeof reply) && own_ok(reply);
        } else ok = ubus_call("network.wireless", on ? "up" : "down", NULL, reply, sizeof reply);
        free(st);
    } else if (!strcmp(action, "wifi.radio.set")) {
        char band[8], name[32], esc[40], *st = malloc(REPLY_MAX);
        jstr(jfind(params, "band"), band, sizeof band);
        if (st && own_wifi_status(st, REPLY_MAX)) {
            ok = own_wifi_radio_set(st, band, jbool(jfind(params, "enabled"), 1));
        } else if (st && ubus_call("network.wireless", "status", NULL, st, REPLY_MAX) && radio_of_band(st, band, name, sizeof name, NULL)) {
            jesc(name, esc, sizeof esc);
            snprintf(args, sizeof args, "{\"radio\":\"%s\"}", esc);
            ok = ubus_call("network.wireless", jbool(jfind(params, "enabled"), 1) ? "up" : "down", args, reply, sizeof reply);
        }
        free(st);
    } else if (!strcmp(action, "device.reboot")) {
        ok = ubus_call("system", "reboot", NULL, reply, sizeof reply);
    } else if (!strcmp(action, "device.poweroff")) {
        /* procd has no call that powers off */
        char *argv[] = { "poweroff", NULL };
        ok = run_capture(argv, NULL, 0, 5000) == 0;
    } else if (!strcmp(action, "power.direct_supply.set")) {
        ok = uci_set("u60pro.battery.charge_mode", jbool(jfind(params, "enabled"), 0) ? "bypass" : "normal") &&
             uci_commit("u60pro") && ubus_call(HW, "reload", NULL, reply, sizeof reply);
    } else if (!strcmp(action, "screen.save")) {
        /* our own: u60pro.screen (data_screen_settings_save) */
        long b = jlong(jfind(params, "brightness"), -1), t = jlong(jfind(params, "timeout"), -1);
        char v[24];
        ok = 1;
        if (b >= 1) { snprintf(v, sizeof v, "%ld", b); ok &= uci_set("u60pro.screen.brightness", v); }
        if (t >= 0) { snprintf(v, sizeof v, "%ld", t); ok &= uci_set("u60pro.screen.timeout", v); }
        ok &= uci_commit("u60pro");
    } else if (!strcmp(action, "state.set_interval")) {
        long ms = jlong(jfind(params, "milliseconds"), 0);
        if (ms >= 200 && ms <= 60000) { g_interval_ms = (int)ms; ok = 1; }
    } else if (!strncmp(action, "network.", 8) || !strncmp(action, "sms.", 4) ||
               !strncmp(action, "neighbor.", 9) || !strncmp(action, "qos.", 4)) {
        /* the modem's business: handed to the modem service as it is */
        char esc[64];
        jesc(action, esc, sizeof esc);
        if (snprintf(args, sizeof args, "{\"action\":\"%s\",\"params\":%s}", esc, params) < (int)sizeof args)
            ok = ubus_call(CELL, "control", args, reply, sizeof reply);
        if (ok) put_reply(out, cap, reply);
        return ok;
    } else {
        fprintf(stderr, "data: action %s has no counterpart on this system\n", action);
        return 0;
    }
    put_reply(out, cap, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ok;
}

struct job {
    char *action, *params, *service, *method;
};

static void *job_main(void *arg)
{
    struct job *j = arg;
    char reply[512];
    if (j->action) (void)datad_control(j->action, j->params, NULL, 0, DEVUI_UBUS_TIMEOUT_MS);
    else if (!ubus_call(j->service, j->method, j->params, reply, sizeof reply))
        fprintf(stderr, "data: ubus call %s %s failed\n", j->service, j->method);
    free(j->action); free(j->params); free(j->service); free(j->method);
    free(j);
    return NULL;
}

static void job_start(const char *action, const char *params, const char *service, const char *method)
{
    struct job *j = calloc(1, sizeof *j);
    pthread_t t;
    pthread_attr_t attr;
    if (!j) return;
    if (action) j->action = strdup(action);
    if (params) j->params = strdup(params);
    if (service) j->service = strdup(service);
    if (method) j->method = strdup(method);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, job_main, j) != 0) {
        free(j->action); free(j->params); free(j->service); free(j->method);
        free(j);
    }
    pthread_attr_destroy(&attr);
}

/* The fallback commands of the pages are the vendor firmware's (ifconfig on its
 * interface names, zwrt_bsp calls); they are never run here. */
void datad_control_async(const char *action, const char *params_json,
                         const char *fallback_cmd)
{
    (void)fallback_cmd;
    job_start(action, params_json, NULL, NULL);
}

void datad_ubus_call_async(const char *service, const char *method,
                           const char *args_json, const char *fallback_cmd)
{
    (void)fallback_cmd;
    job_start(NULL, args_json, service, method);
}
