/*
 * bench_test.c - krait performance benchmark.
 *
 * Measures the three hot paths and writes a plain-text report
 * (tests/bench-results.txt): compile-gate cold vs cached, transcript
 * wrap cold vs repeat, app idle CPU + peak RSS (spawns the real krait
 * binary and samples /proc). Numbers are wall-clock microseconds via
 * clock_gettime; RSS via getrusage + /proc/<pid>/status.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static FILE *report;

static long
now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static long
bench_gate(int cold)
{
    char status[256];
    char err[256];
    char all[2048];
    long t0;
    long t1;
    int rc;

    system("rm -rf /tmp/krait-bench-proj");
    if(krait_scaffold_project("/tmp/krait-bench-proj", status,
                              sizeof(status)) != 1)
        return -1;
    if(cold) {
        /* unique content per cold run so the cc cache misses */
        static int n;
        char kry[512];

        snprintf(kry, sizeof(kry),
                 "#import \"kryon.h\"\n\n"
                 "B%d :: (viewport: Rectangle) #ui {\n"
                 "    Screen root: {\n"
                 "    Background(GetThemeBackground())\n"
                 "    Text(\"v%d\", ScaleUIPx(10), ScaleUIPx(10), "
                 "ScaleUIPx(16), GetThemeText())\n    }\n}\n", n, n);
        krait_write_text_file("/tmp/krait-bench-proj/extra.kry", kry);
        n++;
    }
    t0 = now_us();
    rc = krait_compile_gate_all("/tmp/krait-bench-proj", NULL, NULL, 0, err,
                                sizeof(err), all, sizeof(all));
    t1 = now_us();
    if(rc != 0) {
        fprintf(stderr, "bench: gate failed: %s / %.100s\n", err, all);
        return -1;
    }
    return t1 - t0;
}

static void
bench_section(const char *title)
{
    printf("\n== %s ==\n", title);
    fprintf(report, "\n== %s ==\n", title);
}

static void
bench_line(const char *label, const char *fmt, long value)
{
    printf("%-34s %ld%s\n", label, value, fmt);
    fprintf(report, "%-34s %ld%s\n", label, value, fmt);
}

/* fill a buffer with paragraph-ish text the size of a big tool result */
static void
fill_text(char *buf, size_t size)
{
    size_t used = 0;

    while(used < size - 128) {
        used += (size_t)snprintf(buf + used, size - used,
            "Demo :: (viewport: Rectangle) #ui { Screen root: { Button((ButtonProps){ "
            ".bounds = {ScaleUIPx(12), ScaleUIPx(20)} }) Text ok } }\\n");
    }
}

static long
bench_wrap_cold(char *text)
{
    char line[512];
    long t0 = now_us();
    int n = krait_wrap_lines(text, 500, 12, 4096);

    for(int i = 0; i < n; i++)
        krait_wrap_line(text, 500, 12, 4096, i, line, sizeof(line));
    return now_us() - t0;
}

/* utime+stime ticks from /proc/<pid>/stat, robust to the (comm) field */
static long
proc_cpu_ticks(pid_t pid)
{
    char path[128];
    char buf[4096];
    FILE *f;
    char *close;
    unsigned long u;
    unsigned long st;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    f = fopen(path, "r");
    if(f == NULL)
        return -1;
    buf[0] = '\0';
    if(fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    close = strrchr(buf, ')');
    if(close == NULL)
        return -1;
    if(sscanf(close + 1,
              " %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu",
              &u, &st) != 2)
        return -1;
    return (long)(u + st);
}

static long
proc_rss_kb(pid_t pid)
{
    char path[128];
    char buf[4096];
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    f = fopen(path, "r");
    if(f == NULL)
        return -1;
    while(fgets(buf, sizeof(buf), f) != NULL)
        if(strncmp(buf, "VmRSS:", 6) == 0)
            return strtol(buf + 6, NULL, 10);
    fclose(f);
    return -1;
}

int
main(int argc, char **argv)
{
    char home[256];
    const char *krait_bin = argc > 1 ? argv[1] : "build/linux-x86_64/bin/krait";
    char proj[256];
    struct rusage ru;
    long rss_self;

    snprintf(home, sizeof(home), "/tmp/krait-bench-home.%d", (int)getpid());
    setenv("HOME", home, 1);

    report = fopen("tests/bench-results.txt", "w");
    if(report == NULL) {
        fprintf(stderr, "bench: cannot write tests/bench-results.txt\n");
        return 1;
    }
    fprintf(report, "krait benchmark results\n");
    fprintf(report, "=======================\n");

    /* ---- compile gate ---- */
    bench_section("compile gate (scaffold project)");
    {
        long cold = bench_gate(1);
        long warm1 = bench_gate(0);
        long warm2 = bench_gate(0);

        bench_line("cold (unique file, full cc)", " us", cold);
        bench_line("warm run 1 (cache hits)", " us", warm1);
        bench_line("warm run 2 (cache hits)", " us", warm2);
        bench_line("cc invocations total", "", krait_gate_cc_runs());
        bench_line("cc cache hits total", "", krait_gate_cache_hits());
        bench_line("overlay copies", "", krait_gate_copies());
    }

    /* ---- transcript wrap ---- */
    bench_section("transcript wrap (16KB message, 500px)");
    {
        static char text[16384];
        long cold;
        long repeat = 0;

        fill_text(text, sizeof(text));
        cold = bench_wrap_cold(text);
        for(int i = 0; i < 1000; i++)
            repeat += bench_wrap_cold(text);
        bench_line("cold wrap+render (1 pass)", " us", cold);
        bench_line("repeat x1000 (cached)", " us total", repeat);
        bench_line("per cached frame", " us", repeat / 1000);
    }

    /* ---- live app: idle CPU + RSS ---- */
    bench_section("live app (spawned krait, 6s sample)");
    snprintf(proj, sizeof(proj), "/tmp/krait-bench-proj");
    {
        char cmd[512];
        pid_t pid;
        long utime1;
        long utime2;
        long rss = 0;

        (void)cmd;
        pid = fork();
        if(pid == 0) {
            execl(krait_bin, krait_bin, proj, (char *)NULL);
            _exit(127);
        }
        sleep(3);   /* settle past the 6-frame active window */
        utime1 = proc_cpu_ticks(pid);
        sleep(5);
        utime2 = proc_cpu_ticks(pid);
        rss = proc_rss_kb(pid);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        bench_line("idle CPU (ticks / 5s)", "", utime2 - utime1);
        bench_line("idle CPU (% of one core)", " %",
                   (utime2 - utime1) / 5);
        bench_line("resident memory (VmRSS)", " kB", rss);
    }

    /* ---- self ---- */
    bench_section("bench harness process");
    getrusage(RUSAGE_SELF, &ru);
    rss_self = ru.ru_maxrss;
    bench_line("peak RSS (ru_maxrss)", " kB", rss_self);

    fclose(report);
    printf("\nreport written: tests/bench-results.txt\n");
    system("rm -rf /tmp/krait-bench-proj");
    (void)rss_self;
    return 0;
}
