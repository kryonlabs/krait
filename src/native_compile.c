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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*GateLineFn)(char *line, void *userdata);

typedef struct {
    char *error;
    size_t error_size;
    int *rc;
    char *all;        /* optional: every diagnostic line, capped */
    size_t all_size;
    size_t all_used;
} GateCapture;

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
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && for src in $(find out -name '*.c' | LC_ALL=C sort); do "
             "cc -fsyntax-only -std=c99 -I%s/include -I%s/src/ui "
             "-I%s/vendor/clay -Iout -I$(dirname $src) $src 2>&1 || exit 1; "
             "done; echo CCOKE",
             tmp, kryon_dir, kryon_dir, kryon_dir);
    if(gate_debug())
        fprintf(stderr, "compile-gate: cc cmd=[%s]\n", cmd);
    if(!kry_process_spawn(&proc, cmd, tmp))
        return 1;
    {
        int rc2 = 1;
        GateCapture cap = {first_error, error_size, &rc2, all_errors, all_size,
                           0};

        gate_run(&proc, &cap);
        if(gate_debug())
            fprintf(stderr, "compile-gate: cc rc2=%d exit=%d\n", rc2,
                    proc.exit_status);
        return rc2 == 0 ? 0 : 1;
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
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s' && cp -R '%s/.' '%s/'",
             tmp, tmp, project_dir, tmp);
    if(system(cmd) != 0)
        return 0;
    for(i = 0; i < overlay_count; i++) {
        char dst[KRAIT_PATH_MAX * 2];

        if(overlay_paths[i] == NULL || !krait_path_has_suffix(overlay_paths[i],
                                                              ".kry"))
            continue;
        snprintf(dst, sizeof(dst), "%s/%s", tmp, overlay_paths[i]);
        krait_write_text_file(dst,
            overlay_bodies[i] != NULL ? overlay_bodies[i] : "");
    }
    snprintf(out, sizeof(out), "%s/out", tmp);
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && '%s' --root . --no-main -o '%s' $(find . -name '*.kry' "
             "| LC_ALL=C sort); echo RC=$?",
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
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if(system(cmd) != 0) {
        /* best effort cleanup */
    }
    if(first_error != NULL && error_size > 0)
        krait_trim(first_error);
    return rc == 0 ? 0 : 1;
}

/* Run one command in a directory and capture combined output. Bounded by
 * timeout_s; output is truncated to out_size. Used by the agent's run
 * tool. Returns the process exit status, or -1 when the spawn fails. */
int
krait_run_capture(const char *dir, const char *cmdline, int timeout_s,
                  char *out, size_t out_size)
{
    char carry[2048];
    char cmd[KRAIT_PATH_MAX * 8];
    KryProcess proc;
    size_t used = 0;
    int spins = 0;

    if(out != NULL && out_size > 0)
        out[0] = '\0';
    if(dir == NULL || cmdline == NULL || cmdline[0] == '\0')
        return -1;
    snprintf(cmd, sizeof(cmd), "(%s) 2>&1", cmdline);
    if(!kry_process_spawn(&proc, cmd, dir))
        return -1;
    carry[0] = '\0';
    while(proc.running && spins++ < timeout_s * 20) {
        char chunk[512];
        int n;

        while((n = kry_process_read_poll(&proc, chunk, sizeof(chunk))) > 0) {
            if(out != NULL && used < out_size - 1) {
                size_t take = (size_t)n;

                if(take > out_size - 1 - used)
                    take = out_size - 1 - used;
                memcpy(out + used, chunk, take);
                used += take;
                out[used] = '\0';
            }
        }
        {
            struct timespec ts = {0, 50 * 1000 * 1000};

            nanosleep(&ts, NULL);
        }
        kry_process_wait_poll(&proc);
    }
    {
        char chunk[512];
        int n;

        while((n = kry_process_read_poll(&proc, chunk, sizeof(chunk))) > 0) {
            if(out != NULL && used < out_size - 1) {
                size_t take = (size_t)n;

                if(take > out_size - 1 - used)
                    take = out_size - 1 - used;
                memcpy(out + used, chunk, take);
                used += take;
                out[used] = '\0';
            }
        }
    }
    return proc.exit_status;
}
