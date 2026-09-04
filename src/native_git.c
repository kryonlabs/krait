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
