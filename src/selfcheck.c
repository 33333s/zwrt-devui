/*
 * selfcheck.c - a thread that watches the main loop. When the loop has not
 * come round for a while the UI is stuck (nothing on screen moves, touches and
 * keys go unanswered), so it writes where each thread is to the log and exits
 * with SELFCHECK_EXIT_HUNG for start.sh to run the UI again. Fatal signals are
 * noted in the same log on their way through.
 *
 * Code addresses are logged as they are; look them up in the unstripped build
 * (addr2line -fe zwrt-devui ADDR). Nothing here may take a lock another thread
 * could be holding when it got stuck, so no stdio streams, malloc or opendir.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "selfcheck.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define LOG_MAX   (64 * 1024)   /* then the log moves to .0 and starts over */
#define SAMPLE_SIG SIGUSR2
#define SLOTS     16
#define ADDRS     20
#define SCAN      1024          /* stack words searched for return addresses */

extern char __executable_start[], etext[];

static char g_path[192], g_old[200];
static uint32_t g_limit;
static volatile uint32_t g_beat;
static const char *volatile g_what = "start";

static uint32_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void selfcheck_beat(void) { __atomic_store_n(&g_beat, ms_now(), __ATOMIC_RELAXED); }
void selfcheck_stage(const char *what) { g_what = what; }

/* --- output that is safe anywhere, even in a signal handler --- */

typedef struct { char b[1024]; int n; } line_t;

static void put(line_t *l, const char *s)
{
    while (*s && l->n < (int)sizeof l->b - 1) l->b[l->n++] = *s++;
}

static void put_num(line_t *l, unsigned long long v, int base)
{
    char t[24];
    int i = 0;
    do { t[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v && i < (int)sizeof t);
    if (base == 16) put(l, "0x");
    while (i && l->n < (int)sizeof l->b - 1) l->b[l->n++] = t[--i];
}

static int log_open(void)
{
    struct stat st;
    if (stat(g_path, &st) == 0 && st.st_size > LOG_MAX) rename(g_path, g_old);
    return open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

static void flush(int fd, line_t *l)
{
    if (l->n && fd >= 0) (void)!write(fd, l->b, (size_t)l->n);
    l->n = 0;
}

static int text(uintptr_t a) { return a >= (uintptr_t)__executable_start && a < (uintptr_t)etext; }

/* Code addresses found on a stack from sp up: the calls it is inside, more or
 * less (stale ones can turn up too). The read fails safely past the stack. */
static int stack_calls(uintptr_t sp, uintptr_t *out, int max)
{
    static uintptr_t words[SCAN];
    int n = 0;
    for (int got = 0; got < SCAN && n < max; ) {
        struct iovec lo = { words, sizeof words[0] * 64 }, re = { (void *)(sp + got * sizeof words[0]), lo.iov_len };
        ssize_t r = syscall(SYS_process_vm_readv, getpid(), &lo, 1, &re, 1, 0);
        if (r <= 0) break;
        for (int i = 0; i < (int)(r / sizeof words[0]) && n < max; i++)
            if (text(words[i] - 4)) out[n++] = words[i];
        if (r < (ssize_t)lo.iov_len) break;
        got += 64;
    }
    return n;
}

static void put_regs(line_t *l, uintptr_t pc, uintptr_t lr, uintptr_t sp)
{
    put(l, " pc "); put_num(l, pc, 16);
    put(l, " lr "); put_num(l, lr, 16);
    put(l, " sp "); put_num(l, sp, 16);
}

static void put_calls(line_t *l, const uintptr_t *a, int n)
{
    if (!n) return;
    put(l, "\n    calls");
    for (int i = 0; i < n; i++) { put(l, " "); put_num(l, a[i], 16); }
}

static void regs_of(void *ctx, uintptr_t *pc, uintptr_t *lr, uintptr_t *sp)
{
    *pc = *lr = *sp = 0;
#if defined(__aarch64__)
    const mcontext_t *m = &((const ucontext_t *)ctx)->uc_mcontext;
    *pc = m->pc; *lr = m->regs[30]; *sp = m->sp;
#else
    (void)ctx;
#endif
}

static void put_head(line_t *l, const char *what)
{
    struct timespec rt, up;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &up);
    put(l, "=== time "); put_num(l, (unsigned long long)rt.tv_sec, 10);
    put(l, " up "); put_num(l, (unsigned long long)up.tv_sec, 10);
    put(l, "s pid "); put_num(l, (unsigned long long)getpid(), 10);
    put(l, " text "); put_num(l, (uintptr_t)__executable_start, 16);
    put(l, ": "); put(l, what);
    put(l, " (main loop at "); put(l, g_what); put(l, ")\n");
}

/* --- fatal signals --- */

static char g_alt[16384];

static void on_fatal(int sig, siginfo_t *si, void *ctx)
{
    int fd = log_open();
    line_t l = { .n = 0 };
    uintptr_t pc, lr, sp, a[ADDRS];
    regs_of(ctx, &pc, &lr, &sp);
    put_head(&l, "fatal signal");
    put(&l, "  sig "); put_num(&l, (unsigned)sig, 10);
    put(&l, " tid "); put_num(&l, (unsigned long long)syscall(SYS_gettid), 10);
    put(&l, " addr "); put_num(&l, (uintptr_t)si->si_addr, 16);
    put_regs(&l, pc, lr, sp);
    put_calls(&l, a, sp ? stack_calls(sp, a, ADDRS) : 0);
    put(&l, "\n");
    flush(fd, &l);
    if (fd >= 0) close(fd);
    /* SA_RESETHAND put the default back; it ends us once this returns (sent
     * with kill there is no fault to run again, hence the raise) */
    raise(sig);
}

/* A write to a socket whose peer has gone: the write fails with EPIPE instead
 * of the signal ending the UI. Noted once. */
static void on_pipe(int sig)
{
    static volatile sig_atomic_t told;
    if (told) return;
    told = 1;
    int e = errno, fd = log_open();
    line_t l = { .n = 0 };
    (void)sig;
    put_head(&l, "SIGPIPE, carried on");
    flush(fd, &l);
    if (fd >= 0) close(fd);
    errno = e;
}

/* --- hang report --- */

static struct slot { volatile int tid, done; uintptr_t pc, lr, sp; } g_slot[SLOTS];

static void on_sample(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    int tid = (int)syscall(SYS_gettid);
    for (int i = 0; i < SLOTS; i++)
        if (g_slot[i].tid == tid) {
            regs_of(ctx, &g_slot[i].pc, &g_slot[i].lr, &g_slot[i].sp);
            __atomic_store_n(&g_slot[i].done, 1, __ATOMIC_RELEASE);
        }
}

static int read_small(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int n = 0;
    for (ssize_t r; n < cap - 1 && (r = read(fd, buf + n, (size_t)(cap - 1 - n))) > 0; ) n += (int)r;
    close(fd);
    while (n && buf[n - 1] == '\n') n--;
    buf[n] = 0;
    return n;
}

static int task_ids(int *ids, int max)
{
    struct d64 { unsigned long long ino; long long off; unsigned short len; unsigned char type; char name[]; };
    char buf[2048];
    int fd = open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC), n = 0;
    if (fd < 0) return 0;
    for (long r; (r = syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0; )
        for (long o = 0; o < r; ) {
            struct d64 *e = (struct d64 *)(buf + o);
            if (e->name[0] >= '0' && e->name[0] <= '9' && n < max) ids[n++] = (int)strtol(e->name, NULL, 10);
            o += e->len;
        }
    close(fd);
    return n;
}

static void report(int fd, uint32_t still)
{
    line_t l = { .n = 0 };
    int ids[SLOTS], n = task_ids(ids, SLOTS), self = (int)syscall(SYS_gettid);
    char what[64];
    snprintf(what, sizeof what, "hung, main loop still for %u ms", (unsigned)still);
    put_head(&l, what);
    flush(fd, &l);

    /* where each thread is in its own code: ask it, it answers in on_sample */
    for (int i = 0; i < n; i++) {
        g_slot[i].done = 0;
        g_slot[i].tid = ids[i];
        if (ids[i] != self) syscall(SYS_tgkill, getpid(), ids[i], SAMPLE_SIG);
    }
    for (int t = 0; t < 20; t++) {   /* a thread stuck in the kernel cannot answer */
        int left = 0;
        for (int i = 0; i < n; i++) left += ids[i] != self && !__atomic_load_n(&g_slot[i].done, __ATOMIC_ACQUIRE);
        if (!left) break;
        usleep(10000);
    }
    for (int i = 0; i < n; i++) {
        char p[64], s[1024];
        put(&l, "  task "); put_num(&l, (unsigned)ids[i], 10);
        if (ids[i] == self) { put(&l, " (this check)\n"); flush(fd, &l); continue; }
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", ids[i]);
        if (read_small(p, s, sizeof s)) {
            const char *e = strrchr(s, ')');
            if (e && e[1] && e[2]) { char st[2] = { e[2], 0 }; put(&l, " state "); put(&l, st); }
        }
        snprintf(p, sizeof p, "/proc/self/task/%d/wchan", ids[i]);
        if (read_small(p, s, sizeof s)) { put(&l, " wchan "); put(&l, s); }
        if (__atomic_load_n(&g_slot[i].done, __ATOMIC_ACQUIRE)) {
            uintptr_t a[ADDRS];
            put_regs(&l, g_slot[i].pc, g_slot[i].lr, g_slot[i].sp);
            put_calls(&l, a, stack_calls(g_slot[i].sp, a, ADDRS));
        } else {
            put(&l, " (no answer)");
        }
        snprintf(p, sizeof p, "/proc/self/task/%d/syscall", ids[i]);
        if (read_small(p, s, sizeof s)) { put(&l, "\n    syscall "); put(&l, s); }
        put(&l, "\n");
        flush(fd, &l);
        snprintf(p, sizeof p, "/proc/self/task/%d/stack", ids[i]);
        if (read_small(p, s, sizeof s)) {
            for (char *q = s, *nl; *q; q = nl ? nl + 1 : q + strlen(q)) {
                nl = strchr(q, '\n');
                if (nl) *nl = 0;
                put(&l, "    "); put(&l, q); put(&l, "\n");
                flush(fd, &l);
            }
        }
    }
}

static void *watch(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(1000 * 1000);
        uint32_t still = ms_now() - __atomic_load_n(&g_beat, __ATOMIC_RELAXED);
        if (still < g_limit) continue;
        int fd = log_open();
        if (fd >= 0) { report(fd, still); fsync(fd); close(fd); }
        _exit(SELFCHECK_EXIT_HUNG);
    }
    return NULL;
}

void selfcheck_start(const char *path, uint32_t limit_ms)
{
    snprintf(g_path, sizeof g_path, "%s", path);
    snprintf(g_old, sizeof g_old, "%s.0", path);
    g_limit = limit_ms;
    selfcheck_beat();

    stack_t alt = { .ss_sp = g_alt, .ss_size = sizeof g_alt };
    sigaltstack(&alt, NULL);   /* this thread's (the main loop's) stack can be what ran out */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof fatal / sizeof fatal[0]; i++) sigaction(fatal[i], &sa, NULL);
    /* a handler, not SIG_IGN: what the UI runs (popen, system) gets SIGPIPE back */
    signal(SIGPIPE, on_pipe);
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SAMPLE_SIG, &sa, NULL);

    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64 * 1024);
    if (pthread_create(&t, &at, watch, NULL) != 0) fprintf(stderr, "selfcheck: no thread\n");
    pthread_attr_destroy(&at);
}
