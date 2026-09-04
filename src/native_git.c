/*
 * native_git.c - optional Git/GitHub module backend.
 *
 * Git operations shell out to the git CLI (popen, combined output) inside
 * the open project; everything is synchronous and fast enough for v1
 * (status/log are milliseconds, push/pull pause the frame briefly and the
 * result lands in the module status text). GitHub rides kryon's async
 * kry_http client with the repository resolved from the origin remote, so
 * pull requests and repo facts work wherever the repo is hosted on
 * github.com. The module is opt-in: all entry points are safe no-ops
 * without a repo or a token.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"
#include "kry_json.h"
#include "kry_http.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GIT_MAX_ENTRIES 512
#define GIT_MAX_LOG     128
#define GIT_MAX_BRANCHES 96
#define GIT_MAX_PRS     32
#define GIT_OUT_MAX     8192

typedef struct {
    char path[512];
    char x, y;    /* porcelain status codes */
} GitEntry;

typedef struct {
    char hash[16];
    char subject[256];
    char author[96];
    char when[64];
} GitLogEntry;

typedef struct {
    char name[160];
    int current;
} GitBranch;

typedef struct {
    int number;
    char title[256];
    char user[96];
    char branch[160];
} GitPullRequest;

static struct {
    char project[1024];
    int available;
    int repo;
    char branch[160];
    char upstream[160];
    int ahead, behind;
    GitEntry entries[GIT_MAX_ENTRIES];
    int entry_count;
    GitLogEntry log[GIT_MAX_LOG];
    int log_count;
    GitBranch branches[GIT_MAX_BRANCHES];
    int branch_count;
    char output[GIT_OUT_MAX];     /* last command output (status area) */
    char diff[GIT_OUT_MAX];       /* last fetched file diff */
    char status[192];
    struct {
        char token[160];
        char slug[200];           /* owner/name from origin */
        char repo_desc[256];
        char login[96];
        GitPullRequest prs[GIT_MAX_PRS];
        int pr_count;
        int refreshed;
    } gh;
} g_git;

static void
git_note(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g_git.status, sizeof(g_git.status), fmt, ap);
    va_end(ap);
}

int krait_git_refresh(void);
int krait_git_branch_refresh(void);
int krait_git_log_refresh(void);

/* ------------------------------------------------------------------ */
/* git CLI plumbing                                                    */
/* ------------------------------------------------------------------ */

static int
git_run(const char *args, char *out, size_t out_size, const char *stdin_text)
{
    char cmd[2048];
    FILE *p;
    size_t used = 0;

    if(out != NULL && out_size > 0)
        out[0] = '\0';
    if(!g_git.available)
        return -1;
    if(stdin_text != NULL)
        snprintf(cmd, sizeof(cmd),
                 "cd %s 2>/dev/null && git %s 2>&1 <<'KRAIT_EOF'\n%s\nKRAIT_EOF",
                 g_git.project, args, stdin_text);
    else
        snprintf(cmd, sizeof(cmd), "cd %s 2>/dev/null && git %s 2>&1",
                 g_git.project, args);
    p = popen(cmd, "r");
    if(p == NULL)
        return -1;
    if(out != NULL && out_size > 0) {
        while(used + 1 < out_size) {
            size_t n = fread(out + used, 1, out_size - used - 1, p);

            if(n == 0)
                break;
            used += n;
        }
        out[used] = '\0';
    }
    return pclose(p);
}

/* Quote a path for a git command argument (paths come from status output,
 * already relative; single quotes keep spaces intact). */
static void
git_quote(char *dst, size_t dst_size, const char *path)
{
    size_t n = 0;

    dst[n++] = '\'';
    for(const char *p = path; *p != '\0' && n + 3 < dst_size; p++) {
        if(*p == '\'') {
            dst[n++] = '\'';
            dst[n++] = '\\';
            dst[n++] = '\'';
            dst[n++] = '\'';
        } else {
            dst[n++] = *p;
        }
    }
    dst[n++] = '\'';
    dst[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* module lifecycle                                                    */
/* ------------------------------------------------------------------ */

int krait_git_available(void)
{
    if(!g_git.available) {
        if(system(NULL) == 0)
            return 0;
        g_git.available = system("git --version >/dev/null 2>&1") == 0;
    }
    return g_git.available;
}

void krait_git_set_project(const char *path)
{
    if(path == NULL || path[0] == '\0') {
        g_git.project[0] = '\0';
        g_git.repo = 0;
        g_git.entry_count = 0;
        g_git.log_count = 0;
        g_git.branch_count = 0;
        g_git.gh.slug[0] = '\0';
        g_git.gh.refreshed = 0;
        return;
    }
    /* the pane calls this every frame: only re-probe when the project or
     * the working tree state actually changed */
    if(strcmp(g_git.project, path) == 0 && g_git.repo)
        return;
    snprintf(g_git.project, sizeof(g_git.project), "%s", path);
    g_git.repo = 0;
    {
        char out[256];

        if(git_run("rev-parse --is-inside-work-tree", out, sizeof(out), NULL) == 0 &&
           strncmp(out, "true", 4) == 0)
            g_git.repo = 1;
    }
    if(g_git.repo) {
        krait_git_refresh();
        krait_git_branch_refresh();
        git_note("%s on %s", "repo", g_git.branch);
    } else {
        git_note("not a git repository");
    }
}

int krait_git_repo(void)
{
    return g_git.repo;
}

const char *krait_git_status_text(void)
{
    return g_git.status;
}

const char *krait_git_output(void)
{
    return g_git.output;
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

int krait_git_refresh(void)
{
    char out[64 * 1024];
    char *line;

    g_git.entry_count = 0;
    g_git.branch[0] = '\0';
    g_git.upstream[0] = '\0';
    g_git.ahead = g_git.behind = 0;
    if(!g_git.repo)
        return 0;
    if(git_run("status --porcelain -b", out, sizeof(out), NULL) != 0) {
        git_note("git status failed");
        return 0;
    }
    snprintf(g_git.output, sizeof(g_git.output), "%s", out);
    line = strtok(out, "\n");
    while(line != NULL) {
        if(line[0] == '#') {
            if(strncmp(line, "## ", 3) == 0) {
                char *unborn = strstr(line, "No commits yet on ");

                if(unborn != NULL) {
                    snprintf(g_git.branch, sizeof(g_git.branch), "%s",
                             unborn + 18);
                    {
                        char *sp = strchr(g_git.branch, ' ');

                        if(sp != NULL)
                            *sp = '\0';
                    }
                    line = strtok(NULL, "\n");
                    continue;
                }
                {
                char *dots = strstr(line + 3, "...");

                if(dots != NULL) {
                    size_t n = (size_t)(dots - (line + 3));

                    if(n >= sizeof(g_git.branch))
                        n = sizeof(g_git.branch) - 1;
                    memcpy(g_git.branch, line + 3, n);
                    g_git.branch[n] = '\0';
                    snprintf(g_git.upstream, sizeof(g_git.upstream), "%s",
                             dots + 3);
                    {
                        char *sp = strchr(g_git.upstream, ' ');

                        if(sp != NULL)
                            *sp = '\0';
                    }
                } else {
                    snprintf(g_git.branch, sizeof(g_git.branch), "%s", line + 3);
                    {
                        char *sp = strchr(g_git.branch, ' ');

                        if(sp != NULL)
                            *sp = '\0';
                    }
                }
                if(strstr(line, "ahead ") != NULL)
                    g_git.ahead = atoi(strstr(line, "ahead ") + 6);
                if(strstr(line, "behind ") != NULL)
                    g_git.behind = atoi(strstr(line, "behind ") + 7);
                }
            }
        } else if(strlen(line) >= 3 &&
                  g_git.entry_count < GIT_MAX_ENTRIES) {
            GitEntry *e = &g_git.entries[g_git.entry_count++];

            e->x = line[0];
            e->y = line[1];
            snprintf(e->path, sizeof(e->path), "%s", line + 3);
        }
        line = strtok(NULL, "\n");
    }
    git_note("%d change(s) on %s%s%s", g_git.entry_count, g_git.branch,
             g_git.ahead ? " (ahead)" : "",
             g_git.behind ? " (behind)" : "");
    return 1;
}

const char *krait_git_branch(void)
{
    return g_git.branch;
}

int krait_git_ahead(void) { return g_git.ahead; }
int krait_git_behind(void) { return g_git.behind; }

int krait_git_entry_count(void) { return g_git.entry_count; }
const char *krait_git_entry_path(int index)
{
    if(index < 0 || index >= g_git.entry_count) return "";
    return g_git.entries[index].path;
}
/* staged = index copy differs from HEAD; changed = work tree differs */
int krait_git_entry_staged(int index)
{
    if(index < 0 || index >= g_git.entry_count) return 0;
    return g_git.entries[index].x != ' ' && g_git.entries[index].x != '?';
}
int krait_git_entry_changed(int index)
{
    if(index < 0 || index >= g_git.entry_count) return 0;
    return g_git.entries[index].y != ' ';
}
int krait_git_entry_untracked(int index)
{
    if(index < 0 || index >= g_git.entry_count) return 0;
    return g_git.entries[index].x == '?';
}

/* ------------------------------------------------------------------ */
/* staging, commits, remotes                                           */
/* ------------------------------------------------------------------ */

int krait_git_stage(int index)
{
    char quoted[600];
    char args[700];

    if(index < 0 || index >= g_git.entry_count)
        return 0;
    git_quote(quoted, sizeof(quoted), g_git.entries[index].path);
    snprintf(args, sizeof(args), "add -- %s", quoted);
    if(git_run(args, g_git.output, sizeof(g_git.output), NULL) != 0)
        return 0;
    return krait_git_refresh();
}

int krait_git_unstage(int index)
{
    char quoted[600];
    char args[700];

    if(index < 0 || index >= g_git.entry_count)
        return 0;
    git_quote(quoted, sizeof(quoted), g_git.entries[index].path);
    snprintf(args, sizeof(args), "restore --staged -- %s", quoted);
    if(git_run(args, g_git.output, sizeof(g_git.output), NULL) != 0) {
        /* before the first commit there is no HEAD to restore from */
        snprintf(args, sizeof(args), "rm --cached -q -- %s", quoted);
        if(git_run(args, g_git.output, sizeof(g_git.output), NULL) != 0)
            return 0;
    }
    return krait_git_refresh();
}

int krait_git_stage_all(void)
{
    if(git_run("add -A", g_git.output, sizeof(g_git.output), NULL) != 0)
        return 0;
    return krait_git_refresh();
}

int krait_git_commit(const char *message)
{
    if(message == NULL || message[0] == '\0') {
        git_note("commit needs a message");
        return 0;
    }
    if(git_run("commit --file -", g_git.output, sizeof(g_git.output),
               message) != 0) {
        git_note("commit failed");
        return 0;
    }
    krait_git_refresh();
    krait_git_log_refresh();
    return 1;
}

int krait_git_push(void)
{
    if(git_run("push", g_git.output, sizeof(g_git.output), NULL) != 0) {
        git_note("push failed");
        return 0;
    }
    git_note("pushed %s", g_git.branch);
    return krait_git_refresh();
}

int krait_git_pull(void)
{
    if(git_run("pull --no-edit", g_git.output, sizeof(g_git.output), NULL) != 0) {
        git_note("pull failed");
        return 0;
    }
    git_note("pulled %s", g_git.branch);
    krait_git_refresh();
    krait_git_log_refresh();
    return 1;
}

int krait_git_fetch(void)
{
    if(git_run("fetch --all", g_git.output, sizeof(g_git.output), NULL) != 0) {
        git_note("fetch failed");
        return 0;
    }
    return krait_git_refresh();
}

const char *krait_git_diff(int index)
{
    char quoted[600];
    char args[700];

    g_git.diff[0] = '\0';
    if(index < 0 || index >= g_git.entry_count)
        return "";
    git_quote(quoted, sizeof(quoted), g_git.entries[index].path);
    snprintf(args, sizeof(args), "diff --no-color HEAD -- %s", quoted);
    if(git_run(args, g_git.diff, sizeof(g_git.diff), NULL) != 0)
        return "";
    if(g_git.diff[0] == '\0')
        git_run("diff --no-color --cached", g_git.diff, sizeof(g_git.diff), NULL);
    return g_git.diff;
}

/* ------------------------------------------------------------------ */
/* log + branches                                                      */
/* ------------------------------------------------------------------ */

int krait_git_log_refresh(void)
{
    char out[64 * 1024];
    char *line;

    g_git.log_count = 0;
    if(!g_git.repo)
        return 0;
    if(git_run("log -n 100 --pretty=format:%h%x09%s%x09%an%x09%ar",
               out, sizeof(out), NULL) != 0)
        return 0;
    line = strtok(out, "\n");
    while(line != NULL && g_git.log_count < GIT_MAX_LOG) {
        GitLogEntry *e = &g_git.log[g_git.log_count];
        char *t1 = strchr(line, '\t');
        char *t2 = t1 != NULL ? strchr(t1 + 1, '\t') : NULL;
        char *t3 = t2 != NULL ? strchr(t2 + 1, '\t') : NULL;

        if(t1 != NULL && t2 != NULL && t3 != NULL) {
            *t1 = *t2 = *t3 = '\0';
            snprintf(e->hash, sizeof(e->hash), "%s", line);
            snprintf(e->subject, sizeof(e->subject), "%s", t1 + 1);
            snprintf(e->author, sizeof(e->author), "%s", t2 + 1);
            snprintf(e->when, sizeof(e->when), "%s", t3 + 1);
            g_git.log_count++;
        }
        line = strtok(NULL, "\n");
    }
    return 1;
}

int krait_git_log_count(void) { return g_git.log_count; }
const char *krait_git_log_hash(int index)
{
    if(index < 0 || index >= g_git.log_count) return "";
    return g_git.log[index].hash;
}
const char *krait_git_log_subject(int index)
{
    if(index < 0 || index >= g_git.log_count) return "";
    return g_git.log[index].subject;
}
const char *krait_git_log_author(int index)
{
    if(index < 0 || index >= g_git.log_count) return "";
    return g_git.log[index].author;
}
const char *krait_git_log_when(int index)
{
    if(index < 0 || index >= g_git.log_count) return "";
    return g_git.log[index].when;
}

int krait_git_branch_refresh(void)
{
    char out[32 * 1024];
    char *line;

    g_git.branch_count = 0;
    if(!g_git.repo)
        return 0;
    if(git_run("branch --format='%(HEAD)%(refname:short)'", out, sizeof(out),
               NULL) != 0)
        return 0;
    line = strtok(out, "\n");
    while(line != NULL && g_git.branch_count < GIT_MAX_BRANCHES) {
        GitBranch *b = &g_git.branches[g_git.branch_count];
        int current = line[0] == '*';
        const char *name = line + (current ? 1 : 0);

        while(*name == ' ' || *name == '+')
            name++;
        snprintf(b->name, sizeof(b->name), "%s", name);
        /* local branches first pass: keep remotes but mark dedup later */
        if(strstr(b->name, "origin/") == b->name) {
            line = strtok(NULL, "\n");
            continue;
        }
        b->current = current;
        g_git.branch_count++;
        line = strtok(NULL, "\n");
    }
    return 1;
}

int krait_git_branch_count(void) { return g_git.branch_count; }
const char *krait_git_branch_name(int index)
{
    if(index < 0 || index >= g_git.branch_count) return "";
    return g_git.branches[index].name;
}
int krait_git_branch_current(int index)
{
    if(index < 0 || index >= g_git.branch_count) return 0;
    return g_git.branches[index].current;
}

int krait_git_branch_checkout(int index)
{
    char quoted[256];
    char args[300];

    if(index < 0 || index >= g_git.branch_count)
        return 0;
    git_quote(quoted, sizeof(quoted), g_git.branches[index].name);
    snprintf(args, sizeof(args), "checkout %s", quoted);
    if(git_run(args, g_git.output, sizeof(g_git.output), NULL) != 0)
        return 0;
    krait_git_refresh();
    krait_git_branch_refresh();
    krait_git_log_refresh();
    return 1;
}

int krait_git_branch_create(const char *name)
{
    char quoted[256];
    char args[300];

    if(name == NULL || name[0] == '\0' || name[0] == '-')
        return 0;
    git_quote(quoted, sizeof(quoted), name);
    snprintf(args, sizeof(args), "checkout -b %s", quoted);
    if(git_run(args, g_git.output, sizeof(g_git.output), NULL) != 0)
        return 0;
    krait_git_refresh();
    krait_git_branch_refresh();
    krait_git_log_refresh();
    return 1;
}

int krait_git_clone(const char *url, const char *dest_dir)
{
    char cmd[1500];
    char quoted_url[700], quoted_dest[700];
    FILE *p;

    if(url == NULL || url[0] == '\0' || dest_dir == NULL || dest_dir[0] == '\0')
        return 0;
    git_quote(quoted_url, sizeof(quoted_url), url);
    git_quote(quoted_dest, sizeof(quoted_dest), dest_dir);
    snprintf(cmd, sizeof(cmd), "git clone %s %s 2>&1", quoted_url, quoted_dest);
    p = popen(cmd, "r");
    if(p == NULL)
        return 0;
    {
        size_t used = 0;

        g_git.output[0] = '\0';
        while(used + 1 < sizeof(g_git.output)) {
            size_t n = fread(g_git.output + used, 1,
                             sizeof(g_git.output) - used - 1, p);

            if(n == 0)
                break;
            used += n;
        }
        g_git.output[used] = '\0';
    }
    if(pclose(p) != 0) {
        git_note("clone failed");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* GitHub                                                              */
/* ------------------------------------------------------------------ */

static void
git_conf_path(char *dst, size_t dst_size)
{
    const char *home = getenv("HOME");

    if(home == NULL || home[0] == '\0')
        snprintf(dst, dst_size, ".kryon/krait/git.conf");
    else
        snprintf(dst, dst_size, "%s/.kryon/krait/git.conf", home);
}

static void
github_token_load(void)
{
    char path[1024];
    char *text = NULL;
    long len = 0;

    if(g_git.gh.token[0] != '\0')
        return;
    git_conf_path(path, sizeof(path));
    if(!krait_read_file_alloc(path, &text, &len) || text == NULL)
        return;
    {
        char *line = text;

        while(line != NULL && *line != '\0') {
            char *nl = strchr(line, '\n');

            if(nl != NULL)
                *nl = '\0';
            if(strncmp(line, "github_token=", 13) == 0)
                snprintf(g_git.gh.token, sizeof(g_git.gh.token), "%s", line + 13);
            line = nl != NULL ? nl + 1 : NULL;
        }
    }
    free(text);
}

static void
github_token_save(void)
{
    char path[1024];

    git_conf_path(path, sizeof(path));
    krait_ensure_parent_dir(path);
    if(g_git.gh.token[0] != '\0') {
        char text[256];

        snprintf(text, sizeof(text), "github_token=%s\n", g_git.gh.token);
        krait_write_text_file(path, text);
        chmod(path, 0600);
    }
}

int krait_github_token_set(const char *token)
{
    snprintf(g_git.gh.token, sizeof(g_git.gh.token), "%s",
             token != NULL ? token : "");
    github_token_save();
    g_git.gh.refreshed = 0;
    g_git.gh.pr_count = 0;
    g_git.gh.login[0] = '\0';
    git_note(token != NULL && token[0] != '\0'
                 ? "github token saved"
                 : "github token cleared");
    return 1;
}

const char *krait_github_token_masked(void)
{
    static char masked[24];
    size_t n;

    github_token_load();
    n = strlen(g_git.gh.token);
    if(n == 0)
        return "";
    if(n <= 8)
        return "ghp_****";
    snprintf(masked, sizeof(masked), "%.4s...%.4s (%lu chars)",
             g_git.gh.token, g_git.gh.token + n - 4, (unsigned long)n);
    return masked;
}

int krait_github_has_token(void)
{
    github_token_load();
    return g_git.gh.token[0] != '\0';
}

/* one blocking round trip (poll loop): UI freezes briefly on refresh */
static int
github_json(const char *method, const char *url, const char *body,
            char *out, size_t out_size)
{
    const char *headers[3];
    char auth[200];
    KryHttpRequest *req;
    KryHttpStatus state;
    int waited = 0;

    github_token_load();
    if(g_git.gh.token[0] == '\0') {
        git_note("set a github token first");
        return 0;
    }
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_git.gh.token);
    headers[0] = auth;
    headers[1] = "Accept: application/vnd.github+json";
    headers[2] = "X-GitHub-Api-Version: 2022-11-28";
    if(out != NULL && out_size > 0)
        out[0] = '\0';
    if(strcmp(method, "GET") == 0)
        req = kry_http_get_with_headers(url, 30, headers, 3);
    else
        req = kry_http_post_json_with_headers(url, NULL,
                                              body != NULL ? body : "{}", 30,
                                              headers, 3);
    if(req == NULL) {
        git_note("http unavailable");
        return 0;
    }
    while((state = kry_http_poll(req)) == KRY_HTTP_PENDING ||
          state == KRY_HTTP_RUNNING) {
        usleep(20000);
        if(++waited > 1500)   /* 30s cap */
            break;
    }
    if(state == KRY_HTTP_DONE) {
        const char *response = kry_http_response(req);

        if(out != NULL && out_size > 0 && response != NULL)
            snprintf(out, out_size, "%s", response);
        kry_http_free(req);
        return 1;
    }
    {
        const char *response = kry_http_response(req);

        git_note("github: %.150s", response != NULL ? response : "request failed");
    }
    kry_http_free(req);
    return 0;
}

static void
github_resolve_slug(void)
{
    char out[2048];
    char *nl;

    if(g_git.gh.slug[0] != '\0' || !g_git.repo)
        return;
    if(git_run("remote get-url origin", out, sizeof(out), NULL) != 0 ||
       out[0] == '\0')
        return;
    nl = strchr(out, '\n');
    if(nl != NULL)
        *nl = '\0';
    /* git@github.com:owner/name.git or https://github.com/owner/name.git */
    if(strncmp(out, "git@github.com:", 15) == 0)
        snprintf(g_git.gh.slug, sizeof(g_git.gh.slug), "%s", out + 15);
    else if(strncmp(out, "https://github.com/", 19) == 0)
        snprintf(g_git.gh.slug, sizeof(g_git.gh.slug), "%s", out + 19);
    else
        return;
    {
        size_t n = strlen(g_git.gh.slug);

        if(n > 4 && strcmp(g_git.gh.slug + n - 4, ".git") == 0)
            g_git.gh.slug[n - 4] = '\0';
    }
}

int krait_github_refresh(void)
{
    char out[64 * 1024];
    char url[512];

    github_resolve_slug();
    if(g_git.gh.slug[0] == '\0') {
        git_note("no github origin remote");
        return 0;
    }
    /* repo + login */
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s", g_git.gh.slug);
    if(!github_json("GET", url, NULL, out, sizeof(out)))
        return 0;
    {
        KryJson *root = kry_json_parse(out);

        if(root != NULL) {
            const char *desc = kry_json_string(kry_json_get(root, "description"));
            const char *full = kry_json_string(kry_json_get(root, "full_name"));

            snprintf(g_git.gh.repo_desc, sizeof(g_git.gh.repo_desc), "%s",
                     desc != NULL ? desc : (full != NULL ? full : ""));
            kry_json_free(root);
        }
    }
    if(!github_json("GET", "https://api.github.com/user", NULL, out, sizeof(out)))
        return 0;
    {
        KryJson *root = kry_json_parse(out);

        if(root != NULL) {
            const char *login = kry_json_string(kry_json_get(root, "login"));

            snprintf(g_git.gh.login, sizeof(g_git.gh.login), "%s",
                     login != NULL ? login : "");
            kry_json_free(root);
        }
    }
    /* open pull requests */
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/pulls?state=open&per_page=30",
             g_git.gh.slug);
    g_git.gh.pr_count = 0;
    if(github_json("GET", url, NULL, out, sizeof(out))) {
        KryJson *root = kry_json_parse(out);

        if(root != NULL && kry_json_type(root) == KRY_JSON_ARRAY) {
            for(int i = 0; i < kry_json_count(root) &&
                           g_git.gh.pr_count < GIT_MAX_PRS; i++) {
                const KryJson *item = kry_json_at(root, i);
                GitPullRequest *pr = &g_git.gh.prs[g_git.gh.pr_count];
                const KryJson *n = kry_json_get(item, "number");
                const char *title = kry_json_string(kry_json_get(item, "title"));
                const KryJson *user = kry_json_get(item, "user");
                const char *branch = kry_json_string(kry_json_get(item, "head"));

                pr->number = n != NULL ? (int)kry_json_number(n) : 0;
                snprintf(pr->title, sizeof(pr->title), "%s",
                         title != NULL ? title : "");
                snprintf(pr->user, sizeof(pr->user), "%s",
                         user != NULL &&
                                 kry_json_string(kry_json_get(user, "login")) != NULL
                             ? kry_json_string(kry_json_get(user, "login"))
                             : "");
                snprintf(pr->branch, sizeof(pr->branch), "%s",
                         branch != NULL ? branch : "");
                g_git.gh.pr_count++;
            }
            kry_json_free(root);
        }
    }
    g_git.gh.refreshed = 1;
    git_note("github: %s as @%s, %d open pr(s)", g_git.gh.slug,
             g_git.gh.login[0] != '\0' ? g_git.gh.login : "?",
             g_git.gh.pr_count);
    return 1;
}

int krait_github_repo_is_github(void)
{
    github_resolve_slug();
    return g_git.gh.slug[0] != '\0';
}

const char *krait_github_slug(void)
{
    github_resolve_slug();
    return g_git.gh.slug;
}

const char *krait_github_repo_desc(void) { return g_git.gh.repo_desc; }
const char *krait_github_login(void) { return g_git.gh.login; }
int krait_github_pr_count(void) { return g_git.gh.pr_count; }
const char *krait_github_pr_title(int index)
{
    if(index < 0 || index >= g_git.gh.pr_count) return "";
    return g_git.gh.prs[index].title;
}
const char *krait_github_pr_user(int index)
{
    if(index < 0 || index >= g_git.gh.pr_count) return "";
    return g_git.gh.prs[index].user;
}
int krait_github_pr_number(int index)
{
    if(index < 0 || index >= g_git.gh.pr_count) return 0;
    return g_git.gh.prs[index].number;
}

const char *krait_github_browse_url(void)
{
    static char url[300];

    github_resolve_slug();
    if(g_git.gh.slug[0] == '\0')
        return "";
    snprintf(url, sizeof(url), "https://github.com/%s", g_git.gh.slug);
    return url;
}

int krait_github_pr_create(const char *title, const char *body)
{
    char url[512];
    char payload[1200];
    char out[4096];

    github_resolve_slug();
    if(g_git.gh.slug[0] == '\0' || g_git.branch[0] == '\0') {
        git_note("pr needs a github origin and a branch");
        return 0;
    }
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/pulls",
             g_git.gh.slug);
    {
        /* head must be "login:branch" for forks; same-repo just branch */
        char head[220];

        if(g_git.gh.login[0] != '\0')
            snprintf(head, sizeof(head), "%s:%s", g_git.gh.login, g_git.branch);
        else
            snprintf(head, sizeof(head), "%s", g_git.branch);
        /* JSON-escape minimally: strip quotes from title/body in the UI */
        snprintf(payload, sizeof(payload),
                 "{\"title\":\"%s\",\"body\":\"%s\",\"head\":\"%s\",\"base\":\"main\"}",
                 title != NULL ? title : "", body != NULL ? body : "", head);
    }
    if(!github_json("POST", url, payload, out, sizeof(out)))
        return 0;
    git_note("pull request created");
    krait_github_refresh();
    return 1;
}

/* ------------------------------------------------------------------ */
/* GitHub issues + Projects (v2) sync                                  */
/* ------------------------------------------------------------------ */

int krait_kanban_rescan(void);
int krait_kanban_count(int col);
const char *krait_kanban_card_id(int col, int index);
const char *krait_kanban_card_title(int col, int index);
const char *krait_kanban_card_body(int col, int index);
const char *krait_kanban_column_name(int col);
int krait_kanban_set_body(int col, int index, const char *body);
int krait_kanban_set_title(int col, int index, const char *title);
int krait_kanban_move(int col, int index, int to_col);
int krait_kanban_create(int col, const char *title);

#define GH_MAX_ISSUES 128
#define GH_MAX_ITEMS  128
#define GH_MAX_OPTIONS 16
#define GH_MAX_MAP    512

typedef struct {
    int number;
    char title[256];
    char state[16];
    char url[160];
    char labels[160];
} GhIssue;

typedef struct {
    char item_id[80];
    int issue_number;
    char title[256];
    char state[16];
    char status_name[48];
    char url[160];
} GhProjectItem;

typedef struct {
    char id[80];
    char name[48];
    int column;
} GhStatusOption;

typedef struct {
    int connected;
    char owner[64];
    char kind[16];
    int number;
    char project_id[80];
    char title[128];
    char status_field_id[80];
    GhStatusOption options[GH_MAX_OPTIONS];
    int option_count;
    GhProjectItem items[GH_MAX_ITEMS];
    int item_count;
    GhIssue issues[GH_MAX_ISSUES];
    int issue_count;
} GhProject;

static GhProject g_gh_project;
static struct {
    char card_id[128];
    char item_id[80];
    int issue_number;
} g_gh_map[GH_MAX_MAP];
static int g_gh_map_count;

/* copy a JSON string field into dst ("" when absent) */
static const char *
gh_json_string(const KryJson *v, const char *key, char *dst, size_t dst_size)
{
    const char *s;

    if(v == NULL || dst == NULL || dst_size == 0)
        return dst;
    dst[0] = '\0';
    s = kry_json_string(kry_json_get(v, key));
    if(s != NULL)
        snprintf(dst, dst_size, "%s", s);
    return dst;
}

/* JSON string escape for GraphQL/REST bodies */
static void
gh_escape_json(char *dst, size_t dst_size, const char *src)
{
    size_t n = 0;

    if(dst == NULL || dst_size == 0)
        return;
    dst[0] = '\0';
    if(src == NULL)
        return;
    for(const char *p = src; *p != '\0' && n + 8 < dst_size; p++) {
        switch(*p) {
        case '"':  dst[n++] = '\\'; dst[n++] = '"';  break;
        case '\\': dst[n++] = '\\'; dst[n++] = '\\'; break;
        case '\n': dst[n++] = '\\'; dst[n++] = 'n';  break;
        case '\r': dst[n++] = '\\'; dst[n++] = 'r';  break;
        case '\t': dst[n++] = '\\'; dst[n++] = 't';  break;
        default:
            if((unsigned char)*p < 0x20) {
                n += (size_t)snprintf(dst + n, dst_size - n, "\\u%04x",
                                      (unsigned char)*p);
            } else {
                dst[n++] = *p;
            }
            break;
        }
    }
    dst[n] = '\0';
}

/* map a GitHub status option name onto a krait kanban column */
int
krait_github_status_column(const char *status_name)
{
    char lower[64];
    size_t i;

    if(status_name == NULL || status_name[0] == '\0')
        return 0;
    snprintf(lower, sizeof(lower), "%s", status_name);
    for(i = 0; lower[i] != '\0'; i++)
        lower[i] = (char)tolower((unsigned char)lower[i]);
    if(strstr(lower, "done") != NULL || strstr(lower, "complete") != NULL ||
       strstr(lower, "shipp") != NULL || strstr(lower, "closed") != NULL)
        return 3;
    if(strstr(lower, "review") != NULL || strstr(lower, "test") != NULL ||
       strstr(lower, "qa") != NULL)
        return 2;
    if(strstr(lower, "progress") != NULL || strstr(lower, "doing") != NULL ||
       strstr(lower, "in dev") != NULL || strstr(lower, "working") != NULL)
        return 1;
    return 0;
}

static const char *
gh_column_status_name(int column)
{
    const char *fallback = NULL;

    for(int i = 0; i < g_gh_project.option_count; i++) {
        if(g_gh_project.options[i].column == column)
            return g_gh_project.options[i].name;
        if(fallback == NULL)
            fallback = g_gh_project.options[i].name;
    }
    return fallback != NULL ? fallback : "Todo";
}

static int
gh_graphql(const char *query, char *out, size_t out_size)
{
    char body[8192];
    char escaped[7600];

    gh_escape_json(escaped, sizeof(escaped), query);
    snprintf(body, sizeof(body), "{\"query\":\"%s\"}", escaped);
    return github_json("POST", "https://api.github.com/graphql", body, out,
                       out_size);
}

/* parse .../users/<login>/projects/<n> or .../orgs/<org>/projects/<n> */
int
krait_github_project_parse_url(const char *url)
{
    const char *p;
    char kind[8] = "";
    char owner[64];
    int num = 0;

    if(url == NULL)
        return 0;
    p = strstr(url, "github.com/");
    if(p == NULL)
        return 0;
    p += 11;
    if(strncmp(p, "users/", 6) == 0) {
        snprintf(kind, sizeof(kind), "user");
        p += 6;
    } else if(strncmp(p, "orgs/", 5) == 0) {
        snprintf(kind, sizeof(kind), "organization");
        p += 5;
    } else {
        return 0;
    }
    {
        size_t n = 0;

        while(*p != '\0' && *p != '/' && n + 1 < sizeof(owner))
            owner[n++] = *p++;
        owner[n] = '\0';
    }
    if(strncmp(p, "/projects/", 10) != 0)
        return 0;
    num = atoi(p + 10);
    if(owner[0] == '\0' || num <= 0)
        return 0;
    snprintf(g_gh_project.owner, sizeof(g_gh_project.owner), "%s", owner);
    g_gh_project.number = num;
    snprintf(g_gh_project.kind, sizeof(g_gh_project.kind), "%s", kind);
    return 1;
}

/* --- card <-> project item sidecar map -------------------------------- */

static void
gh_map_path(char *dst, size_t dst_size)
{
    const char *home = getenv("HOME");

    if(home == NULL || home[0] == '\0')
        snprintf(dst, dst_size, ".kryon/krait/kanban/gh_map.txt");
    else
        snprintf(dst, dst_size, "%s/.kryon/krait/kanban/gh_map.txt", home);
}

static int g_gh_map_loaded;

static void
gh_map_load(void)
{
    char path[1024];
    FILE *f;

    if(g_gh_map_loaded)
        return;
    g_gh_map_loaded = 1;
    g_gh_map_count = 0;
    gh_map_path(path, sizeof(path));
    f = fopen(path, "r");
    if(f == NULL)
        return;
    while(g_gh_map_count < GH_MAX_MAP &&
          fscanf(f, "%127s %79s %d",
                 g_gh_map[g_gh_map_count].card_id,
                 g_gh_map[g_gh_map_count].item_id,
                 &g_gh_map[g_gh_map_count].issue_number) == 3)
        g_gh_map_count++;
    fclose(f);
}

static void
gh_map_save(void)
{
    char path[1024];
    FILE *f;

    gh_map_path(path, sizeof(path));
    krait_ensure_parent_dir(path);
    f = fopen(path, "w");
    if(f == NULL)
        return;
    for(int i = 0; i < g_gh_map_count; i++)
        fprintf(f, "%s %s %d\n", g_gh_map[i].card_id,
                g_gh_map[i].item_id, g_gh_map[i].issue_number);
    fclose(f);
}

static int
gh_map_find_card(const char *card_id)
{
    gh_map_load();
    for(int i = 0; i < g_gh_map_count; i++)
        if(strcmp(g_gh_map[i].card_id, card_id) == 0)
            return i;
    return -1;
}

static void
gh_map_add(const char *card_id, const char *item_id, int issue_number)
{
    gh_map_load();
    if(gh_map_find_card(card_id) >= 0)
        return;
    if(g_gh_map_count >= GH_MAX_MAP)
        return;
    snprintf(g_gh_map[g_gh_map_count].card_id,
             sizeof g_gh_map[0].card_id, "%s", card_id);
    snprintf(g_gh_map[g_gh_map_count].item_id,
             sizeof g_gh_map[0].item_id, "%s", item_id);
    g_gh_map[g_gh_map_count].issue_number = issue_number;
    g_gh_map_count++;
    gh_map_save();
}

/* --- project load ------------------------------------------------------ */

int
krait_github_project_connect(const char *url)
{
    char query[2048];
    char out[256 * 1024];

    if(!krait_github_has_token()) {
        git_note("set a github token first");
        return 0;
    }
    if(!krait_github_project_parse_url(url)) {
        git_note("expected a github projects url");
        return 0;
    }
    snprintf(query, sizeof(query),
        "query { %s(login: \"%s\") { projectV2(number: %d) { id title "
        "fields(first: 20) { nodes { __typename ... on ProjectV2SingleSelectField "
        "{ id name options { id name } } } } "
        "items(first: 100) { nodes { id type "
        "content { ... on Issue { number title state url } ... on DraftIssue { title } } "
        "fieldValues(first: 10) { nodes { ... on ProjectV2ItemFieldSingleSelectValue { name } } } "
        "} } } } }",
        g_gh_project.kind, g_gh_project.owner, g_gh_project.number);
    if(!gh_graphql(query, out, sizeof(out))) {
        g_gh_project.connected = 0;
        return 0;
    }
    {
        KryJson *root = kry_json_parse(out);
        KryJson *project = root != NULL
            ? kry_json_get(kry_json_get(root, "data"), "projectV2") : NULL;
        /* error surface: {"errors":[...]} with INSUFFICIENT_SCOPES etc. */
        KryJson *errors = root != NULL ? kry_json_get(root, "errors") : NULL;

        if(errors != NULL && kry_json_count(errors) > 0) {
            const KryJson *first = kry_json_at(errors, 0);
            const char *message =
                kry_json_string(kry_json_get(first, "message"));

            git_note("github project: %.140s",
                     message != NULL ? message : "request failed");
            kry_json_free(root);
            g_gh_project.connected = 0;
            return 0;
        }
        if(project == NULL) {
            git_note("github project not found");
            kry_json_free(root);
            g_gh_project.connected = 0;
            return 0;
        }
        g_gh_project.connected = 1;
        g_gh_project.option_count = 0;
        g_gh_project.item_count = 0;
        gh_json_string(project, "id", g_gh_project.project_id,
                       sizeof(g_gh_project.project_id));
        gh_json_string(project, "title", g_gh_project.title,
                       sizeof(g_gh_project.title));
        {
            const KryJson *fields = kry_json_get(project, "fields");

            for(int i = 0; i < kry_json_count(fields) &&
                           g_gh_project.option_count < GH_MAX_OPTIONS; i++) {
                const KryJson *field = kry_json_at(fields, i);
                const char *name = kry_json_string(kry_json_get(field, "name"));
                const KryJson *options = kry_json_get(field, "options");

                if(name == NULL || options == NULL ||
                   kry_json_count(options) == 0)
                    continue;
                if(g_gh_project.status_field_id[0] == '\0' ||
                    (name != NULL && strcmp(name, "Status") == 0)) {
                    snprintf(g_gh_project.status_field_id,
                             sizeof(g_gh_project.status_field_id), "%s",
                             kry_json_string(kry_json_get(field, "id")) != NULL
                                 ? kry_json_string(kry_json_get(field, "id")) : "");
                    g_gh_project.option_count = 0;
                    for(int k = 0; k < kry_json_count(options) &&
                                   g_gh_project.option_count < GH_MAX_OPTIONS; k++) {
                        const KryJson *opt = kry_json_at(options, k);
                        GhStatusOption *slot =
                            &g_gh_project.options[g_gh_project.option_count++];
                        const char *opt_name =
                            kry_json_string(kry_json_get(opt, "name"));

                        snprintf(slot->id, sizeof(slot->id), "%s",
                                 kry_json_string(kry_json_get(opt, "id")) != NULL
                                     ? kry_json_string(kry_json_get(opt, "id")) : "");
                        snprintf(slot->name, sizeof(slot->name), "%s",
                                 opt_name != NULL ? opt_name : "?");
                        slot->column = krait_github_status_column(slot->name);
                    }
                }
            }
        }
        {
            const KryJson *items = kry_json_get(project, "items");

            for(int i = 0; i < kry_json_count(items) &&
                           g_gh_project.item_count < GH_MAX_ITEMS; i++) {
                const KryJson *item = kry_json_at(items, i);
                GhProjectItem *slot = &g_gh_project.items[g_gh_project.item_count++];
                const KryJson *content = kry_json_get(item, "content");
                const KryJson *values =
                    kry_json_get(kry_json_get(item, "fieldValues"), "nodes");

                gh_json_string(item, "id", slot->item_id,
                               sizeof(slot->item_id));
                slot->issue_number = 0;
                slot->state[0] = '\0';
                slot->url[0] = '\0';
                if(content != NULL) {
                    const KryJson *num = kry_json_get(content, "number");

                    slot->issue_number = num != NULL && kry_json_type(num) == KRY_JSON_NUMBER
                        ? (int)kry_json_number(num) : 0;
                    gh_json_string(content, "title", slot->title,
                                   sizeof(slot->title));
                    gh_json_string(content, "state", slot->state,
                                   sizeof(slot->state));
                    gh_json_string(content, "url", slot->url,
                                   sizeof(slot->url));
                }
                slot->status_name[0] = '\0';
                for(int k = 0; k < kry_json_count(values); k++) {
                    const KryJson *value = kry_json_at(values, k);
                    const char *status =
                        kry_json_string(kry_json_get(value, "name"));

                    if(status != NULL)
                        snprintf(slot->status_name, sizeof(slot->status_name),
                                 "%s", status);
                }
            }
        }
        kry_json_free(root);
    }
    git_note("project '%s': %d item(s), %d status option(s)",
             g_gh_project.title, g_gh_project.item_count,
             g_gh_project.option_count);
    return 1;
}

int krait_github_project_connected(void) { return g_gh_project.connected; }
const char *krait_github_project_title(void) { return g_gh_project.title; }
int krait_github_project_item_count(void) { return g_gh_project.item_count; }
const char *krait_github_project_item_title(int index)
{
    if(index < 0 || index >= g_gh_project.item_count) return "";
    return g_gh_project.items[index].title;
}
const char *krait_github_project_item_status(int index)
{
    if(index < 0 || index >= g_gh_project.item_count) return "";
    return g_gh_project.items[index].status_name;
}
int krait_github_project_item_issue(int index)
{
    if(index < 0 || index >= g_gh_project.item_count) return 0;
    return g_gh_project.items[index].issue_number;
}
const char *krait_github_project_item_url(int index)
{
    if(index < 0 || index >= g_gh_project.item_count) return "";
    return g_gh_project.items[index].url;
}

/* --- sync: project -> kanban ------------------------------------------- */

int
krait_github_project_pull(void)
{
    int created = 0, updated = 0, moved = 0;

    if(!g_gh_project.connected) {
        git_note("connect a github project first");
        return 0;
    }
    gh_map_load();
    for(int i = 0; i < g_gh_project.item_count; i++) {
        GhProjectItem *item = &g_gh_project.items[i];
        int map_index = -1;
        int column = krait_github_status_column(item->status_name);

        for(int m = 0; m < g_gh_map_count; m++)
            if(strcmp(g_gh_map[m].item_id, item->item_id) == 0) {
                map_index = m;
                break;
            }
        if(map_index < 0) {
            /* find the local card by stable id when possible, else create */
            int col = -1, idx = -1;

            for(int c = 0; c < 4 && col < 0; c++) {
                int count = krait_kanban_count(c);

                for(int k = 0; k < count; k++)
                    if(strcmp(krait_kanban_card_id(c, k),
                              item->item_id) == 0) {
                        col = c;
                        idx = k;
                        break;
                    }
            }
            if(col < 0) {
                col = column;
                idx = krait_kanban_create(col, item->title);
                if(idx < 0)
                    continue;
                created++;
            }
            gh_map_add(krait_kanban_card_id(col, idx), item->item_id,
                       item->issue_number);
        } else {
            /* update title + column of the mapped card */
            for(int c = 0; c < 4; c++) {
                int count = krait_kanban_count(c);

                for(int k = 0; k < count; k++) {
                    if(strcmp(krait_kanban_card_id(c, k),
                              g_gh_map[map_index].card_id) != 0)
                        continue;
                    if(strcmp(krait_kanban_card_title(c, k), item->title) != 0) {
                        krait_kanban_set_title(c, k, item->title);
                        updated++;
                    }
                    if(c != column) {
                        krait_kanban_move(c, k, column);
                        moved++;
                    }
                }
            }
        }
    }
    krait_kanban_rescan();
    git_note("pull: %d new, %d updated, %d moved", created, updated, moved);
    return 1;
}

/* --- sync: kanban -> project ------------------------------------------ */

static char g_gh_add_issue_node_id[80];

static int
gh_project_add_issue_item(const char *repo_slug, const char *title,
                          const char *body, int issue_number,
                          const char *status_name, char *item_id_out,
                          size_t item_id_size, int *number_out)
{
    char query[4096];
    char out[32 * 1024];
    char esc_title[512], esc_body[2048], esc_status[96], esc_repo[220];
    int created_number = issue_number;

    gh_escape_json(esc_title, sizeof(esc_title), title);
    gh_escape_json(esc_body, sizeof(esc_body), body != NULL ? body : "");
    gh_escape_json(esc_status, sizeof(esc_status), status_name);
    if(issue_number > 0 || repo_slug == NULL || repo_slug[0] == '\0') {
        /* draft issue lives directly in the project */
        gh_escape_json(esc_repo, sizeof(esc_repo), "");
        (void)esc_repo;
        snprintf(query, sizeof(query),
            "mutation { addProjectV2DraftIssue(input: {projectId: \"%s\", "
            "title: \"%s\", body: \"%s\"}) { projectItem { id } } }",
            g_gh_project.project_id, esc_title, esc_body);
    } else {
        gh_escape_json(esc_repo, sizeof(esc_repo), repo_slug);
        snprintf(query, sizeof(query),
            "mutation { createIssue(input: {repositoryId: \"\", title: \"\", body: \"\"}) { issue { id } } }",
            esc_repo, esc_title, esc_body);
        /* createIssue needs a repository ID: resolve it via REST instead */
        {
            char url[512];
            char payload[3072];
            char rest_out[16 * 1024];
            KryJson *root;
            const char *node_id;

            snprintf(url, sizeof(url), "https://api.github.com/repos/%s",
                     repo_slug);
            if(!github_json("GET", url, NULL, rest_out, sizeof(rest_out)))
                return 0;
            root = kry_json_parse(rest_out);
            node_id = root != NULL
                ? kry_json_string(kry_json_get(root, "node_id")) : NULL;
            if(node_id == NULL) {
                kry_json_free(root);
                return 0;
            }
            snprintf(query, sizeof(query),
                "mutation { createIssue(input: {repositoryId: \"%s\", "
                "title: \"%s\", body: \"%s\"}) { issue { number id } } }",
                node_id, esc_title, esc_body);
            kry_json_free(root);
        }
    }
    if(!gh_graphql(query, out, sizeof(out)))
        return 0;
    {
        KryJson *root = kry_json_parse(out);
        KryJson *data = root != NULL ? kry_json_get(root, "data") : NULL;
        const char *item_id = NULL;
        KryJson *errors = root != NULL ? kry_json_get(root, "errors") : NULL;

        if(errors != NULL && kry_json_count(errors) > 0) {
            const char *message =
                kry_json_string(kry_json_get(kry_json_at(errors, 0), "message"));

            git_note("github: %.140s", message != NULL ? message : "mutation failed");
            kry_json_free(root);
            return 0;
        }
        if(data == NULL) {
            kry_json_free(root);
            return 0;
        }
        {
            KryJson *add = kry_json_get(data, "addProjectV2DraftIssue");
            KryJson *create = kry_json_get(data, "createIssue");

            if(create != NULL) {
                const KryJson *issue = kry_json_get(create, "issue");
                const KryJson *num = issue != NULL
                    ? kry_json_get(issue, "number") : NULL;
                const KryJson *iid = issue != NULL
                    ? kry_json_get(issue, "id") : NULL;

                created_number = num != NULL ? (int)kry_json_number(num) : 0;
                snprintf(g_gh_add_issue_node_id,
                         sizeof(g_gh_add_issue_node_id), "%s",
                         iid != NULL && kry_json_string(iid) != NULL
                             ? kry_json_string(iid) : "");
                item_id = "PENDING_ADD";
            } else if(add != NULL) {
                const KryJson *pi = kry_json_get(add, "projectItem");

                item_id = pi != NULL && kry_json_string(kry_json_get(pi, "id")) != NULL
                    ? kry_json_string(kry_json_get(pi, "id")) : NULL;
            }
        }
        if(item_id == NULL) {
            kry_json_free(root);
            return 0;
        }
        if(strcmp(item_id, "PENDING_ADD") == 0) {
            /* the issue exists; now attach it to the project */
            snprintf(query, sizeof(query),
                "mutation { addProjectV2ItemById(input: {projectId: \"%s\", "
                "contentId: \"%s\"}) { item { id } } }",
                g_gh_project.project_id, g_gh_add_issue_node_id);
            if(!gh_graphql(query, out, sizeof(out))) {
                kry_json_free(root);
                return 0;
            }
            {
                KryJson *root2 = kry_json_parse(out);

                item_id = NULL;
                if(root2 != NULL) {
                    const KryJson *add =
                        kry_json_get(kry_json_get(root2, "data"),
                                     "addProjectV2ItemById");

                    if(add != NULL) {
                        const KryJson *it = kry_json_get(add, "item");

                        item_id = it != NULL &&
                                  kry_json_string(kry_json_get(it, "id")) != NULL
                            ? kry_json_string(kry_json_get(it, "id")) : NULL;
                    }
                }
                kry_json_free(root2);
                if(item_id == NULL) {
                    kry_json_free(root);
                    return 0;
                }
            }
        }
        snprintf(item_id_out, item_id_size, "%s", item_id);
        if(number_out != NULL)
            *number_out = created_number;
        kry_json_free(root);
    }
    /* set the status field */
    {
        const char *option_id = NULL;

        for(int i = 0; i < g_gh_project.option_count; i++)
            if(strcmp(g_gh_project.options[i].name, status_name) == 0)
                option_id = g_gh_project.options[i].id;
        if(option_id != NULL) {
            snprintf(query, sizeof(query),
                "mutation { updateProjectV2ItemFieldValue(input: {projectId: \"%s\", "
                "itemId: \"%s\", fieldId: \"%s\", value: { singleSelectOptionId: \"%s\" }}) "
                "{ projectV2Item { id } } }",
                g_gh_project.project_id, item_id_out,
                g_gh_project.status_field_id, option_id);
            gh_graphql(query, out, sizeof(out));
        }
    }
    return 1;
}


static char g_gh_project_last_url[300];
static void krait_github_project_connect_last_url(void);

int
krait_github_project_push(void)
{
    int pushed = 0, updated = 0;

    if(!g_gh_project.connected) {
        git_note("connect a github project first");
        return 0;
    }
    gh_map_load();
    for(int c = 0; c < 4; c++) {
        int count = krait_kanban_count(c);

        for(int k = 0; k < count; k++) {
            const char *card_id = krait_kanban_card_id(c, k);
            const char *title = krait_kanban_card_title(c, k);
            const char *body = krait_kanban_card_body(c, k);
            int map_index = gh_map_find_card(card_id);

            if(map_index < 0) {
                char item_id[80];
                int issue_number = 0;

                if(!gh_project_add_issue_item(g_git.gh.slug, title, body, 0,
                                              gh_column_status_name(c),
                                              item_id, sizeof(item_id),
                                              &issue_number))
                    continue;
                gh_map_add(card_id, item_id, issue_number);
                pushed++;
            } else {
                /* card moved locally: push the new status */
                const char *want = gh_column_status_name(c);

                if(strcmp(g_gh_map[map_index].item_id, "") != 0 &&
                   g_gh_project.status_field_id[0] != '\0') {
                    const char *option_id = NULL;

                    for(int i = 0; i < g_gh_project.option_count; i++)
                        if(strcmp(g_gh_project.options[i].name, want) == 0)
                            option_id = g_gh_project.options[i].id;
                    if(option_id != NULL) {
                        char query[2048];
                        char out[16 * 1024];

                        snprintf(query, sizeof(query),
                            "mutation { updateProjectV2ItemFieldValue(input: "
                            "{projectId: \"%s\", itemId: \"%s\", fieldId: \"%s\", "
                            "value: { singleSelectOptionId: \"%s\" }}) "
                            "{ projectV2Item { id } } }",
                            g_gh_project.project_id,
                            g_gh_map[map_index].item_id,
                            g_gh_project.status_field_id, option_id);
                        if(gh_graphql(query, out, sizeof(out)))
                            updated++;
                    }
                }
            }
        }
    }
    krait_github_project_connect_last_url();
    git_note("push: %d new item(s), %d status update(s)", pushed, updated);
    return 1;
}

static void
krait_github_project_connect_last_url(void)
{
    if(g_gh_project_last_url[0] != '\0')
        krait_github_project_connect(g_gh_project_last_url);
}

/* --- issues REST ------------------------------------------------------- */

int
krait_github_issues_refresh(void)
{
    char url[512];
    char out[128 * 1024];

    github_resolve_slug();
    if(g_git.gh.slug[0] == '\0') {
        git_note("no github origin remote for issues");
        return 0;
    }
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/issues?state=all&per_page=100",
             g_git.gh.slug);
    g_gh_project.issue_count = 0;
    if(!github_json("GET", url, NULL, out, sizeof(out)))
        return 0;
    {
        KryJson *root = kry_json_parse(out);

        if(root == NULL || kry_json_type(root) != KRY_JSON_ARRAY) {
            kry_json_free(root);
            return 0;
        }
        for(int i = 0; i < kry_json_count(root) &&
                       g_gh_project.issue_count < GH_MAX_ISSUES; i++) {
            const KryJson *item = kry_json_at(root, i);
            GhIssue *issue = &g_gh_project.issues[g_gh_project.issue_count++];
            const KryJson *num = kry_json_get(item, "number");

            issue->number = num != NULL ? (int)kry_json_number(num) : 0;
            gh_json_string(item, "title", issue->title, sizeof(issue->title));
            gh_json_string(item, "state", issue->state, sizeof(issue->state));
            gh_json_string(item, "html_url", issue->url, sizeof(issue->url));
            issue->labels[0] = '\0';
            {
                const KryJson *labels = kry_json_get(item, "labels");

                for(int k = 0; k < kry_json_count(labels) && k < 4; k++) {
                    const char *name = kry_json_string(
                        kry_json_get(kry_json_at(labels, k), "name"));

                    if(name != NULL) {
                        if(issue->labels[0] != '\0')
                            strncat(issue->labels, ",",
                                    sizeof(issue->labels) - strlen(issue->labels) - 1);
                        strncat(issue->labels, name,
                                sizeof(issue->labels) - strlen(issue->labels) - 1);
                    }
                }
            }
        }
        kry_json_free(root);
    }
    git_note("%d issue(s) in %s", g_gh_project.issue_count, g_git.gh.slug);
    return 1;
}

int krait_github_issue_count(void) { return g_gh_project.issue_count; }
int krait_github_issue_number(int index)
{
    if(index < 0 || index >= g_gh_project.issue_count) return 0;
    return g_gh_project.issues[index].number;
}
const char *krait_github_issue_title(int index)
{
    if(index < 0 || index >= g_gh_project.issue_count) return "";
    return g_gh_project.issues[index].title;
}
const char *krait_github_issue_state(int index)
{
    if(index < 0 || index >= g_gh_project.issue_count) return "";
    return g_gh_project.issues[index].state;
}
const char *krait_github_issue_labels(int index)
{
    if(index < 0 || index >= g_gh_project.issue_count) return "";
    return g_gh_project.issues[index].labels;
}
const char *krait_github_issue_url(int index)
{
    if(index < 0 || index >= g_gh_project.issue_count) return "";
    return g_gh_project.issues[index].url;
}

/* create a real repo issue from a kanban card and link the map */
int
krait_github_card_to_issue(int col, int index)
{
    char url[512];
    char payload[4096];
    char out[16 * 1024];
    char esc_title[512], esc_body[2048];
    const char *card_id;
    const char *title;
    const char *body;

    if(col < 0 || col > 3 || index < 0 || index >= krait_kanban_count(col))
        return 0;
    github_resolve_slug();
    if(g_git.gh.slug[0] == '\0') {
        git_note("card needs a github repo project");
        return 0;
    }
    card_id = krait_kanban_card_id(col, index);
    title = krait_kanban_card_title(col, index);
    body = krait_kanban_card_body(col, index);
    gh_escape_json(esc_title, sizeof(esc_title), title);
    gh_escape_json(esc_body, sizeof(esc_body), body);
    snprintf(payload, sizeof(payload), "{\"title\":\"%s\",\"body\":\"%s\"}",
             esc_title, esc_body);
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/issues",
             g_git.gh.slug);
    if(!github_json("POST", url, payload, out, sizeof(out)))
        return 0;
    {
        KryJson *root = kry_json_parse(out);
        const KryJson *num = root != NULL ? kry_json_get(root, "number") : NULL;
        int number = num != NULL ? (int)kry_json_number(num) : 0;

        kry_json_free(root);
        if(number > 0) {
            gh_map_add(card_id, "", number);
            git_note("created issue #%d", number);
            return number;
        }
    }
    return 0;
}

/* import an issue as a kanban card (in its project column when connected) */
int
krait_github_issue_to_card(int issue_index)
{
    char query[2048];
    char out[16 * 1024];
    GhIssue *issue;
    int col = 0, idx;
    char item_id[80] = "";

    if(issue_index < 0 || issue_index >= g_gh_project.issue_count)
        return 0;
    issue = &g_gh_project.issues[issue_index];
    for(int c = 0; c < 4; c++) {
        int count = krait_kanban_count(c);

        for(int k = 0; k < count; k++)
            if(strcmp(krait_kanban_card_title(c, k), issue->title) == 0)
                return 1;   /* already imported */
    }
    /* status from the project item when the issue is on the board */
    if(g_gh_project.connected) {
        for(int i = 0; i < g_gh_project.item_count; i++)
            if(g_gh_project.items[i].issue_number == issue->number) {
                col = krait_github_status_column(
                    g_gh_project.items[i].status_name);
                snprintf(item_id, sizeof(item_id), "%s",
                         g_gh_project.items[i].item_id);
                break;
            }
    }
    idx = krait_kanban_create(col, issue->title);
    if(idx < 0)
        return 0;
    gh_map_add(krait_kanban_card_id(col, idx), item_id, issue->number);
    /* attach the issue to the connected project if it is not there yet */
    if(g_gh_project.connected && item_id[0] == '\0') {
        char node_url[512];
        char node_out[8192];
        KryJson *root;
        const char *node_id;

        snprintf(node_url, sizeof(node_url),
                 "https://api.github.com/repos/%s/issues/%d", g_git.gh.slug,
                 issue->number);
        if(github_json("GET", node_url, NULL, node_out, sizeof(node_out))) {
            root = kry_json_parse(node_out);
            node_id = root != NULL
                ? kry_json_string(kry_json_get(root, "node_id")) : NULL;
            if(node_id != NULL) {
                snprintf(query, sizeof(query),
                    "mutation { addProjectV2ItemById(input: {projectId: \"%s\", "
                    "contentId: \"%s\"}) { item { id } } }",
                    g_gh_project.project_id, node_id);
                if(gh_graphql(query, out, sizeof(out))) {
                    KryJson *root2 = kry_json_parse(out);

                    if(root2 != NULL) {
                        const KryJson *add = kry_json_get(
                            kry_json_get(root2, "data"), "addProjectV2ItemById");
                        const KryJson *it = add != NULL
                            ? kry_json_get(add, "item") : NULL;
                        const char *iid = it != NULL &&
                                kry_json_string(kry_json_get(it, "id")) != NULL
                            ? kry_json_string(kry_json_get(it, "id")) : NULL;

                        if(iid != NULL)
                            gh_map_add(krait_kanban_card_id(col, idx), iid,
                                       issue->number);
                        kry_json_free(root2);
                    }
                }
            }
            kry_json_free(root);
        }
    }
    krait_kanban_rescan();
    git_note("imported issue #%d", issue->number);
    return 1;
}

/* remember the connected project across refreshes */
int
krait_github_project_connect_url(const char *url)
{
    snprintf(g_gh_project_last_url, sizeof(g_gh_project_last_url), "%s",
             url != NULL ? url : "");
    return krait_github_project_connect(url);
}

/* persisted project url so the kanban toolbar can sync without the pane */
int
krait_github_project_url_set(const char *url)
{
    char path[1024];

    git_conf_path(path, sizeof(path));
    krait_ensure_parent_dir(path);
    {
        FILE *out = fopen(path, "a");

        if(out != NULL) {
            fprintf(out, "github_project_url=%s\n", url != NULL ? url : "");
            fclose(out);
        }
    }
    snprintf(g_gh_project_last_url, sizeof(g_gh_project_last_url), "%s",
             url != NULL ? url : "");
    return 1;
}

const char *
krait_github_project_url_get(void)
{
    char path[1024];
    char *text = NULL;
    long len = 0;

    if(g_gh_project_last_url[0] != '\0')
        return g_gh_project_last_url;
    git_conf_path(path, sizeof(path));
    if(!krait_read_file_alloc(path, &text, &len) || text == NULL)
        return "";
    {
        char *line = text;

        while(line != NULL && *line != '\0') {
            char *nl = strchr(line, '\n');

            if(nl != NULL)
                *nl = '\0';
            if(strncmp(line, "github_project_url=", 19) == 0)
                snprintf(g_gh_project_last_url, sizeof(g_gh_project_last_url),
                         "%s", line + 19);   /* appends: last wins */
            line = nl != NULL ? nl + 1 : NULL;
        }
    }
    free(text);
    return g_gh_project_last_url;
}

/* one-shot board sync used by the kanban toolbar */
int
krait_github_board_sync(void)
{
    const char *url = krait_github_project_url_get();

    if(!krait_github_has_token()) {
        git_note("set a github token first");
        return 0;
    }
    if(url == NULL || url[0] == '\0') {
        git_note("connect a github project in the git pane first");
        return 0;
    }
    if(!krait_github_project_connected() &&
       !krait_github_project_connect(url))
        return 0;
    if(!krait_github_project_pull())
        return 0;
    if(!krait_github_project_push())
        return 0;
    krait_github_project_connect(url);
    return 1;
}

/* issue link for the card modal: returns the issue number (0 = none) */
int
krait_github_card_issue(int col, int index)
{
    const char *card_id;
    int map_index;

    if(col < 0 || col > 3 || index < 0 || index >= krait_kanban_count(col))
        return 0;
    card_id = krait_kanban_card_id(col, index);
    map_index = gh_map_find_card(card_id);
    if(map_index < 0)
        return 0;
    return g_gh_map[map_index].issue_number;
}

/* close / reopen an issue from the pane */
int
krait_github_issue_set_state(int issue_index, const char *state)
{
    char url[512];
    char payload[128];
    char out[16 * 1024];
    GhIssue *issue;

    if(issue_index < 0 || issue_index >= g_gh_project.issue_count)
        return 0;
    if(state == NULL || (strcmp(state, "open") != 0 &&
                         strcmp(state, "closed") != 0))
        return 0;
    github_resolve_slug();
    if(g_git.gh.slug[0] == '\0')
        return 0;
    issue = &g_gh_project.issues[issue_index];
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", state);
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/issues/%d",
             g_git.gh.slug, issue->number);
    if(!github_json("PATCH", url, payload, out, sizeof(out)))
        return 0;
    snprintf(issue->state, sizeof(issue->state), "%s", state);
    git_note("issue #%d %s", issue->number, state);
    return 1;
}
