/*
 * native_compile.c - the shared compile gate.
 *
 * One gate, two consumers: the kanban validator checks AI proposals
 * before Apply, and the agent view self-checks after writing files. The
 * project is copied to a temp overlay (optionally with .kry files
 * replaced), transpiled through k2c, and every generated .c is checked
 * with cc -fsyntax-only against the vendored kryon headers. Returns 0
 * when everything compiles (or the gate cannot run), 1 otherwise, with
 * the first diagnostic copied out for feedback loops.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include "kry_process.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

typedef void (*GateLineFn)(char *line, void *userdata);

typedef struct {
    char *error;
    size_t error_size;
    int *rc;
    char *all;        /* optional: every diagnostic line, capped */
    size_t all_size;
    size_t all_used;
} GateCapture;

/* cc verdict cache: the syntax check re-parses kryon's headers for every
 * generated file on every run, which measured ~3.5s for two dozen files
 * while the transpile took 12ms. Files whose content hash was already
 * checked green stay green, so a validation after a 1-file edit checks
 * one file, not the whole tree. */
#define GATE_CACHE_MAX 512
typedef struct {
    unsigned long hash;
    int verdict;      /* 1 = compiles */
    char error[96];
} GateCcEntry;
static GateCcEntry gate_cc_cache[GATE_CACHE_MAX];
static int gate_cc_cache_count;
static int gate_stats_cc_runs;
static int gate_stats_cache_hits;
static int gate_stats_copies;

int
krait_gate_cc_runs(void)
{
    return gate_stats_cc_runs;
}

int
krait_gate_cache_hits(void)
{
    return gate_stats_cache_hits;
}

int
krait_gate_copies(void)
{
    return gate_stats_copies;
}

static unsigned long
gate_hash(const char *a, const char *b)
{
    unsigned long h = 2166136261ul;
    const char *p = a;

    while(p != NULL && *p != '\0')
        h = (h ^ (unsigned char)*p++) * 16777619ul;
    p = b;
    while(p != NULL && *p != '\0')
        h = (h ^ (unsigned char)*p++) * 16777619ul;
    return h;
}

static GateCcEntry *
gate_cache_find(unsigned long h)
{
    int i;

    for(i = 0; i < gate_cc_cache_count; i++)
        if(gate_cc_cache[i].hash == h)
            return &gate_cc_cache[i];
    return NULL;
}

static void
gate_cache_store(unsigned long h, int verdict, const char *error)
{
    GateCcEntry *e;

    if(gate_cc_cache_count >= GATE_CACHE_MAX)
        return;   /* full: degrade to uncached, never wrong */
    e = &gate_cc_cache[gate_cc_cache_count++];
    e->hash = h;
    e->verdict = verdict;
    snprintf(e->error, sizeof(e->error), "%s", error != NULL ? error : "");
}

/* (path, mtime, size) manifest of a directory tree, so the overlay copy
 * is skipped when the project has not changed since the last one. */
static unsigned long
gate_manifest(const char *dir, unsigned long h)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    struct stat st;
    char path[KRAIT_PATH_MAX * 2];

    if(d == NULL)
        return h;
    while((e = readdir(d)) != NULL) {
        if(e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if(stat(path, &st) != 0)
            continue;
        if(S_ISDIR(st.st_mode)) {
            if(strcmp(e->d_name, "out") == 0)
                continue;   /* our own transpile output */
            h = gate_manifest(path, h);
            h = (h ^ (unsigned long)e->d_name[0]) * 16777619ul;
            continue;
        }
        h = gate_hash(path, "");
        /* nanosecond mtime: whole seconds collide when a project is
         * rebuilt within the same second (tests scaffold this way) */
        h ^= (unsigned long)st.st_mtim.tv_nsec * 31 +
             (unsigned long)st.st_size * 1000003u +
             (unsigned long)st.st_mtim.tv_sec;
    }
    closedir(d);
    return h;
}

static int
gate_debug(void)
{
    return getenv("KRAIT_COMPILE_DEBUG") != NULL;
}

/* Sentinels: the k2c leg echoes "RC=N"; the cc leg echoes "CCOKE" on
 * success. Everything else is potential diagnostic output. */
static void
gate_capture_line(char *line, void *userdata)
{
    GateCapture *cap = userdata;

    if(gate_debug())
        fprintf(stderr, "compile-gate: [%s]\n", line);
    if(strncmp(line, "RC=", 3) == 0)
        *cap->rc = atoi(line + 3);
    else if(strstr(line, "CCOKE") != NULL)
        *cap->rc = 0;
    else {
        if(cap->error[0] == '\0')
            snprintf(cap->error, cap->error_size, "%s", line);
        if(cap->all != NULL && cap->all_size > 1 &&
           cap->all_used + strlen(line) + 2 < cap->all_size) {
            cap->all_used += (size_t)snprintf(cap->all + cap->all_used,
                                              cap->all_size - cap->all_used,
                                              "%s\n", line);
        }
    }
}

/* Drain a KryProcess line-by-line: chunks from read_poll are split on
 * newlines with carry across polls. Each complete line goes through fn. */
static void
gate_process_lines(KryProcess *proc, char *carry, size_t *carry_len,
                   size_t carry_size, GateLineFn fn, void *userdata)
{
    char chunk[512];
    int n;

    while((n = kry_process_read_poll(proc, chunk, sizeof(chunk))) > 0) {
        size_t used = 0;

        while(used < (size_t)n) {
            char *nl = memchr(chunk + used, '\n', (size_t)n - used);
            size_t piece;

            if(nl == NULL) {
                if(*carry_len < carry_size - 1) {
                    memcpy(carry + *carry_len, chunk + used, (size_t)n - used);
                    *carry_len += (size_t)(n - used);
                    carry[*carry_len] = '\0';
                }
                break;
            }
            piece = (size_t)(nl - (chunk + used));
            {
                if(*carry_len + piece < carry_size - 1) {
                    memcpy(carry + *carry_len, chunk + used, piece);
                    carry[*carry_len + piece] = '\0';
                    krait_trim(carry);
                    if(carry[0] != '\0')
                        fn(carry, userdata);
                }
                *carry_len = 0;
                carry[0] = '\0';
            }
            used += piece + 1;
        }
    }
}

static void
gate_run(KryProcess *proc, GateCapture *cap)
{
    char carry[1024];
    size_t carry_len = 0;
    int spins = 0;

    carry[0] = '\0';
    while(proc->running && spins++ < 2400) {
        struct timespec ts = {0, 50 * 1000 * 1000};

        gate_process_lines(proc, carry, &carry_len, sizeof(carry),
                           gate_capture_line, cap);
        nanosleep(&ts, NULL);
        kry_process_wait_poll(proc);
    }
    /* final drain: the sentinel line often lands right at exit, after the
     * reap flipped running to 0 */
    gate_process_lines(proc, carry, &carry_len, sizeof(carry),
                       gate_capture_line, cap);
}

/* cc -fsyntax-only over the k2c output: k2c proves Kry syntax, this
 * proves the C compiles against the real kryon headers (catches wrong
 * widget names k2c cannot see). */
static int
gate_cc_check_all(const char *tmp, const char *k2c_path,
                  char *first_error, size_t error_size,
                  char *all_errors, size_t all_size)
{
    char kryon_dir[KRAIT_PATH_MAX];
    char cmd[KRAIT_PATH_MAX * 8];
    KryProcess proc;
    char *bin_pos;

    /* <kryon>/build/<platform>-<arch>/bin/k2c -> <kryon>, made absolute so
     * the check survives the cd into the temp overlay. */
    snprintf(kryon_dir, sizeof(kryon_dir), "%s", k2c_path);
    bin_pos = strstr(kryon_dir, "/build/");
    if(bin_pos == NULL)
        return 0;   /* unexpected layout; skip the cc pass */
    *bin_pos = '\0';
    if(kryon_dir[0] != '/') {
        char abs[KRAIT_PATH_MAX];

        if(realpath(kryon_dir, abs) != NULL)
            snprintf(kryon_dir, sizeof(kryon_dir), "%s", abs);
    }
    /* one cc per not-yet-green file; cached verdicts skip the spawn.
     * the hash covers the file content AND the kryon checkout it is
     * checked against, so a vendor bump invalidates everything. */
    {
        char listpath[KRAIT_PATH_MAX * 2];
        FILE *list;
        char line[KRAIT_PATH_MAX];
        int failed = 0;

        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && find out -name '*.c' | LC_ALL=C sort > out/.clist",
                 tmp);
        if(system(cmd) != 0)
            return 1;
        snprintf(listpath, sizeof(listpath), "%s/out/.clist", tmp);
        list = fopen(listpath, "r");
        if(list == NULL)
            return 1;
        while(fgets(line, sizeof(line), list) != NULL) {
            char *nl = strchr(line, '\n');
            char src[KRAIT_PATH_MAX * 2];
            char *content = NULL;
            long len;
            unsigned long h;
            GateCcEntry *entry;

            if(nl != NULL)
                *nl = '\0';
            if(line[0] == '\0')
                continue;
            snprintf(src, sizeof(src), "%s/%s", tmp, line);
            if(!krait_read_file_alloc(src, &content, &len)) {
                free(content);
                failed = 1;
                break;
            }
            h = gate_hash(content, kryon_dir);
            free(content);
            entry = gate_cache_find(h);
            if(gate_debug())
                fprintf(stderr, "compile-gate: file %s hash=%lx cache=%s\n",
                        line, h,
                        entry != NULL ? (entry->verdict ? "green" : "red")
                                      : "miss");
            if(entry != NULL && entry->verdict) {
                gate_stats_cache_hits++;
                continue;
            }
            gate_stats_cc_runs++;
            snprintf(cmd, sizeof(cmd),
                     "cd '%s' && cc -fsyntax-only -std=c99 -I%s/include "
                     "-I%s/src/ui -I%s/vendor/clay -Iout -I$(dirname '%s') "
                     "'%s' 2>&1; echo RC=$?",
                     tmp, kryon_dir, kryon_dir, kryon_dir, line, line);
            if(gate_debug())
                fprintf(stderr, "compile-gate: cc cmd=[%s]\n", cmd);
            if(!kry_process_spawn(&proc, cmd, tmp)) {
                failed = 1;
                break;
            }
            {
                int rc2 = 1;
                char one_err[192];
                GateCapture cap = {one_err, sizeof(one_err), &rc2, all_errors,
                                   all_size, 0};

                one_err[0] = '\0';
                gate_run(&proc, &cap);
                if(rc2 == 0) {
                    gate_cache_store(h, 1, NULL);
                } else {
                    gate_cache_store(h, 0, one_err);
                    if(first_error != NULL && error_size > 0 &&
                       first_error[0] == '\0')
                        snprintf(first_error, error_size, "%s",
                                 one_err[0] != '\0' ? one_err : line);
                    failed = 1;
                    break;
                }
            }
        }
        fclose(list);
        if(!failed)
            return 0;
        return 1;
    }
}

int
krait_compile_gate_all(const char *project_dir,
                       const char *const *overlay_paths,
                       const char *const *overlay_bodies, int overlay_count,
                       char *first_error, size_t error_size,
                       char *all_errors, size_t all_size)
{
    char k2c[KRAIT_PATH_MAX];
    char tmp[KRAIT_PATH_MAX];
    char cmd[KRAIT_PATH_MAX * 8];
    char out[KRAIT_PATH_MAX];
    KryProcess proc;
    int rc;
    int i;

    if(first_error != NULL && error_size > 0)
        first_error[0] = '\0';
    if(project_dir == NULL || project_dir[0] == '\0')
        return 0;
    krait_kryon_tool_path(k2c, sizeof(k2c), "k2c");
    if(k2c[0] != '/') {
        char abs[KRAIT_PATH_MAX];

        if(realpath(k2c, abs) != NULL)
            snprintf(k2c, sizeof(k2c), "%s", abs);
    }
    if(access(k2c, X_OK) != 0)
        return 0;   /* no toolchain; the gate cannot run */
    snprintf(tmp, sizeof(tmp), "/tmp/krait-gate-%d", (int)getpid());
    {
        /* warm overlay: reuse the copy while the project tree is
         * unchanged (mtime+size manifest), saving the rm -rf + cp -R */
        static char warm_dir[KRAIT_PATH_MAX];
        static unsigned long warm_hash;
        unsigned long manifest = gate_manifest(project_dir, 0x9e3779b9ul);

        if(strcmp(warm_dir, project_dir) != 0 || manifest != warm_hash) {
            snprintf(cmd, sizeof(cmd),
                     "rm -rf '%s' && mkdir -p '%s' && cp -R '%s/.' '%s/'",
                     tmp, tmp, project_dir, tmp);
            if(system(cmd) != 0)
                return 0;
            snprintf(warm_dir, sizeof(warm_dir), "%s", project_dir);
            warm_hash = manifest;
            gate_stats_copies++;
        }
    }
    for(i = 0; i < overlay_count; i++) {
        char dst[KRAIT_PATH_MAX * 2];

        if(overlay_paths[i] == NULL || !krait_path_has_suffix(overlay_paths[i],
                                                              ".kry"))
            continue;
        snprintf(dst, sizeof(dst), "%s/%s", tmp, overlay_paths[i]);
        {
            int wok = krait_write_text_file(dst,
                            overlay_bodies[i] != NULL ? overlay_bodies[i]
                                                      : "");

            if(gate_debug())
                fprintf(stderr, "compile-gate: overlay write %s ok=%d\n",
                        dst, wok);
        }
    }
    snprintf(out, sizeof(out), "%s/out", tmp);
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && '%s' --root . --no-main -o '%s' $(find . -name '*.kry' "
             "| LC_ALL=C sort) 2>&1; echo RC=$?",
             tmp, k2c, out);
    if(all_errors != NULL && all_size > 0)
        all_errors[0] = '\0';
    rc = -1;
    if(kry_process_spawn(&proc, cmd, tmp)) {
        GateCapture cap = {first_error, error_size, &rc, all_errors, all_size,
                           0};

        gate_run(&proc, &cap);
    }
    if(rc == 0)
        rc = gate_cc_check_all(tmp, k2c, first_error, error_size, all_errors,
                               all_size);
    /* the overlay stays warm for the next call (pid-keyed under /tmp);
     * deleting it here would defeat the manifest skip and make the next
     * spawn chdir into a missing dir */
    if(first_error != NULL && error_size > 0)
        krait_trim(first_error);
    if(rc != 0 && first_error != NULL && error_size > 0 &&
       first_error[0] == '\0')
        snprintf(first_error, error_size, "k2c failed (exit %d)", rc);
    return rc == 0 ? 0 : 1;
}

/* Commands get their own process group, so cancellation also stops children.
 * Return 124 for timeout, 130 for cancellation, -1 for launch failure. */
static double
command_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

int
krait_run_capture_cancel(const char *dir, const char *cmdline, int timeout_s,
                         char *out, size_t out_size,
                         int (*cancelled)(void *), void *userdata)
{
    int pipes[2], status = 0, reason = 0, reaped = 0;
    pid_t pid;
    size_t used = 0;
    double deadline, stop_time = 0;
    if(out != NULL && out_size > 0)
        out[0] = 0;
    if(dir == NULL || cmdline == NULL || cmdline[0] == 0 || timeout_s <= 0)
        return -1;
    if(cancelled != NULL && cancelled(userdata))
        return 130;
    if(pipe(pipes) != 0)
        return -1;
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if(pid < 0) {
        close(pipes[0]); close(pipes[1]);
        return -1;
    }
    if(pid == 0) {
        setpgid(0, 0);
        close(pipes[0]);
        if(chdir(dir) != 0 || dup2(pipes[1], STDOUT_FILENO) < 0 ||
           dup2(pipes[1], STDERR_FILENO) < 0)
            _exit(126);
        close(pipes[1]);
        int input = open("/dev/null", O_RDONLY);
        if(input >= 0) { dup2(input, STDIN_FILENO); close(input); }
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }
    close(pipes[1]);
    setpgid(pid, pid);
    fcntl(pipes[0], F_SETFL, fcntl(pipes[0], F_GETFL) | O_NONBLOCK);
    deadline = command_time() + timeout_s;
    for(;;) {
        char chunk[4096];
        ssize_t n;
        /* Bounded draining prevents endless output from starving Stop. */
        for(int i = 0; i < 64 && (n = read(pipes[0], chunk, sizeof(chunk))) > 0; i++) {
            if(out != NULL && out_size > 0 && used < out_size - 1) {
                size_t take = (size_t)n;
                if(take > out_size - 1 - used) take = out_size - 1 - used;
                memcpy(out + used, chunk, take);
                used += take;
                out[used] = 0;
            }
        }
        double now = command_time();
        if(!reason && cancelled != NULL && cancelled(userdata)) reason = 130;
        if(!reason && now >= deadline) reason = 124;
        if(reason && stop_time == 0) {
            kill(-pid, SIGTERM);
            stop_time = now;
        }
        if(!reaped) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if(result == pid || (result < 0 && errno == ECHILD)) reaped = 1;
        }
        if(reason && now - stop_time >= 0.25) {
            kill(-pid, SIGKILL);
            if(!reaped) {
                while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                reaped = 1;
            }
            break;
        }
        if(reaped && !reason) {
            /* Drain tail after exit; no producer can block this read. */
            for(int i = 0; i < 64 && (n = read(pipes[0], chunk, sizeof(chunk))) > 0; i++) {
                if(out != NULL && out_size > 0 && used < out_size - 1) {
                    size_t take = (size_t)n;
                    if(take > out_size - 1 - used) take = out_size - 1 - used;
                    memcpy(out + used, chunk, take); used += take; out[used] = 0;
                }
                if(command_time() >= deadline) break;
            }
            break;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    close(pipes[0]);
    if(reason) return reason;
    return WIFEXITED(status) ? WEXITSTATUS(status) :
           WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

int
krait_run_capture(const char *dir, const char *cmdline, int timeout_s,
                  char *out, size_t out_size)
{
    return krait_run_capture_cancel(dir, cmdline, timeout_s, out, out_size, NULL, NULL);
}
