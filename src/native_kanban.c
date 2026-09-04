/*
 * native_kanban.c - agentic kanban board backend.
 *
 * The board IS the filesystem: ~/.kryon/krait/kanban/<column>/<card>.txt.
 * Columns are fixed (backlog/doing/review/done), cards are plain text
 * (first line title, 'project:' header, blank line, prompt body), and AI
 * proposals live in .proposals/<card>/ until applied or rejected. All
 * state stays in C; .kry gets index-based accessors (native_assets
 * pattern).
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include "kry_json.h"
#include "kry_process.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KB_MAX_CARDS 128
#define KB_MAX_PROPOSAL_FILES 16

static const char *const kb_columns[4] = {"backlog", "doing", "review", "done"};
static const char *const kb_column_names[4] = {"Backlog", "Doing", "Review",
                                               "Done"};

/* One initial AI call plus up to two retries after validation rejects
 * the proposal. */
#define KB_AI_MAX_ATTEMPTS 3

typedef struct {
    char id[128];        /* file stem; stable handle for this session */
    char path[KRAIT_PATH_MAX];
    char title[256];
    char project[KRAIT_PATH_MAX];
    char *body;          /* malloc'd; may be NULL */
    char status[256];    /* "", "running...", "proposal ready", error text */
    int proposal_count;
    char proposal_paths[KB_MAX_PROPOSAL_FILES][256];
    char *proposal_bodies[KB_MAX_PROPOSAL_FILES];
    KraitAiRequest *ai;   /* in-flight request, or NULL */
    int ai_attempts;     /* validation attempts spent this run */
    char ai_error[256];  /* first error of the latest rejected attempt */
} KbCard;

typedef struct {
    KbCard cards[KB_MAX_CARDS];
    int count;
} KbColumn;

static KbColumn kb_board[4];
static char kb_dir[KRAIT_PATH_MAX];
static int kb_dir_ready;

static void
kb_board_dir(char *dst, size_t dst_size)
{
    const char *home = getenv("HOME");

    snprintf(dst, dst_size, "%s/.kryon/krait/kanban",
             home != NULL ? home : ".");
}

static void
kb_card_free(KbCard *c)
{
    int i;

    free(c->body);
    c->body = NULL;
    for(i = 0; i < c->proposal_count; i++)
        free(c->proposal_bodies[i]);
    c->proposal_count = 0;
    if(c->ai != NULL) {
        krait_ai_free(c->ai);
        c->ai = NULL;
    }
}

static void
kb_free_all(void)
{
    int col, i;

    for(col = 0; col < 4; col++) {
        for(i = 0; i < kb_board[col].count; i++)
            kb_card_free(&kb_board[col].cards[i]);
        kb_board[col].count = 0;
    }
}

static void
kb_parse_card(KbCard *c, const char *text)
{
    const char *p = text;
    const char *nl;
    size_t n;

    /* first line: title */
    nl = strchr(p, '\n');
    n = nl != NULL ? (size_t)(nl - p) : strlen(p);
    if(n >= sizeof(c->title))
        n = sizeof(c->title) - 1;
    memcpy(c->title, p, n);
    c->title[n] = '\0';
    krait_trim(c->title);
    p = nl != NULL ? nl + 1 : p + strlen(p);

    /* header lines 'key: value' until a blank line */
    while(*p != '\0') {
        nl = strchr(p, '\n');
        n = nl != NULL ? (size_t)(nl - p) : strlen(p);
        if(n == 0)
            break;   /* blank line: body follows */
        if(strncmp(p, "project:", 8) == 0) {
            size_t vn = n > 8 ? n - 8 : 0;
            const char *v = p + 8;

            while(vn > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                vn--;
            }
            if(vn >= sizeof(c->project))
                vn = sizeof(c->project) - 1;
            memcpy(c->project, v, vn);
            c->project[vn] = '\0';
            krait_trim(c->project);
        }
        if(nl == NULL)
            break;
        p = nl + 1;
    }
    /* skip the blank separator */
    while(*p == '\n' || *p == '\r')
        p++;
    free(c->body);
    c->body = *p != '\0' ? strdup(p) : NULL;
}

static int
kb_proposal_dir(KbCard *c, char *dst, size_t dst_size)
{
    return snprintf(dst, dst_size, "%s/.proposals/%s", kb_dir, c->id) <
           (int)dst_size;
}

static int
kb_scan_proposals(KbCard *c)
{
    char dir[KRAIT_PATH_MAX];
    DIR *d;
    struct dirent *e;

    c->proposal_count = 0;
    if(!kb_proposal_dir(c, dir, sizeof(dir)))
        return 0;
    d = opendir(dir);
    if(d == NULL)
        return 0;
    while((e = readdir(d)) != NULL &&
          c->proposal_count < KB_MAX_PROPOSAL_FILES) {
        char path[KRAIT_PATH_MAX * 2];
        char *body = NULL;
        long len;

        if(e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if(!krait_read_file_alloc(path, &body, &len)) {
            free(body);
            continue;
        }
        snprintf(c->proposal_paths[c->proposal_count],
                 sizeof(c->proposal_paths[0]), "%s", e->d_name);
        c->proposal_bodies[c->proposal_count] = body;
        c->proposal_count++;
    }
    closedir(d);
    return c->proposal_count;
}

static void
kb_load_status(KbCard *c)
{
    char status_path[KRAIT_PATH_MAX * 2];
    char *text = NULL;
    long len;

    if(!kb_proposal_dir(c, status_path, sizeof(status_path)))
        return;
    krait_ensure_parent_dir(status_path);
    strncat(status_path, "/.status", sizeof(status_path) - strlen(status_path) - 1);
    if(krait_read_file_alloc(status_path, &text, &len) && text != NULL) {
        snprintf(c->status, sizeof(c->status), "%s", krait_trim(text));
        free(text);
    }
}

int
krait_kanban_rescan(void)
{
    int col;

    kb_board_dir(kb_dir, sizeof(kb_dir));
    for(col = 0; col < 4; col++) {
        char path[KRAIT_PATH_MAX];
        DIR *d;
        struct dirent *e;
        KbColumn *column = &kb_board[col];
        int i;

        for(i = 0; i < column->count; i++)
            kb_card_free(&column->cards[i]);
        column->count = 0;
        snprintf(path, sizeof(path), "%s/%s", kb_dir, kb_columns[col]);
        krait_mkdir_p(path);
        d = opendir(path);
        if(d == NULL)
            continue;
        while((e = readdir(d)) != NULL && column->count < KB_MAX_CARDS) {
            KbCard *c = &column->cards[column->count];
            char *text = NULL;
            long len;
            char *dot;

            if(e->d_name[0] == '.' ||
               !krait_path_has_suffix(e->d_name, ".txt"))
                continue;
            memset(c, 0, sizeof(*c));
            snprintf(c->id, sizeof(c->id), "%s", e->d_name);
            dot = strrchr(c->id, '.');
            if(dot != NULL)
                *dot = '\0';
            snprintf(c->path, sizeof(c->path), "%s/%s", path, e->d_name);
            if(!krait_read_file_alloc(c->path, &text, &len)) {
                free(text);
                continue;
            }
            kb_parse_card(c, text);
            free(text);
            kb_load_status(c);
            kb_scan_proposals(c);
            column->count++;
        }
        closedir(d);
    }
    kb_dir_ready = 1;
    return kb_board[0].count + kb_board[1].count + kb_board[2].count +
           kb_board[3].count;
}

static KbCard *
kb_card_at(int col, int index)
{
    if(!kb_dir_ready)
        krait_kanban_rescan();
    if(col < 0 || col > 3 || index < 0 || index >= kb_board[col].count)
        return NULL;
    return &kb_board[col].cards[index];
}

static void
kb_save_card(KbCard *c)
{
    char text[8192];

    if(c->project[0] != '\0')
        snprintf(text, sizeof(text), "%s\nproject: %s\n\n%s",
                 c->title, c->project, c->body != NULL ? c->body : "");
    else
        snprintf(text, sizeof(text), "%s\n\n%s", c->title,
                 c->body != NULL ? c->body : "");
    krait_write_text_file(c->path, text);
}

int
krait_kanban_count(int col)
{
    if(!kb_dir_ready)
        krait_kanban_rescan();
    if(col < 0 || col > 3)
        return 0;
    return kb_board[col].count;
}

const char *
krait_kanban_column_name(int col)
{
    if(col < 0 || col > 3)
        return "?";
    return kb_column_names[col];
}

const char *
krait_kanban_card_id(int col, int index)
{
    KbCard *c = kb_card_at(col, index);

    return c != NULL ? c->id : "";
}

const char *
krait_kanban_card_title(int col, int index)
{
    KbCard *c = kb_card_at(col, index);

    return c != NULL ? c->title : "";
}

const char *
krait_kanban_card_project(int col, int index)
{
    KbCard *c = kb_card_at(col, index);

    return c != NULL ? c->project : "";
}

const char *
krait_kanban_card_body(int col, int index)
{
    KbCard *c = kb_card_at(col, index);

    return c != NULL && c->body != NULL ? c->body : "";
}

const char *
krait_kanban_card_path(int col, int index)
{
    KbCard *c = kb_card_at(col, index);

    return c != NULL ? c->path : "";
}

const char *
krait_kanban_card_status(int col, int index)
{
    KbCard *c = kb_card_at(col, index);

    return c != NULL ? c->status : "";
}

int
krait_kanban_create(int col, const char *title)
{
    char dir[KRAIT_PATH_MAX];
    char path[KRAIT_PATH_MAX * 2];
    char id[128];
    static int seq;
    KbCard *c;

    if(!kb_dir_ready)
        krait_kanban_rescan();
    if(col < 0 || col > 3 || title == NULL || title[0] == '\0')
        return -1;
    snprintf(dir, sizeof(dir), "%s/%s", kb_dir, kb_columns[col]);
    do {
        snprintf(id, sizeof(id), "card-%d", ++seq);
        snprintf(path, sizeof(path), "%s/%s.txt", dir, id);
    } while(krait_path_exists(path));
    {
        char text[512];

        snprintf(text, sizeof(text), "%s\n\n", title);
        krait_write_text_file(path, text);
    }
    krait_kanban_rescan();
    {
        int i;

        for(i = 0; i < kb_board[col].count; i++)
            if(strcmp(kb_board[col].cards[i].id, id) == 0)
                return i;
    }
    return -1;
}

int
krait_kanban_set_title(int col, int index, const char *title)
{
    KbCard *c = kb_card_at(col, index);

    if(c == NULL || title == NULL)
        return 0;
    snprintf(c->title, sizeof(c->title), "%s", title);
    kb_save_card(c);
    return 1;
}

int
krait_kanban_set_body(int col, int index, const char *body)
{
    KbCard *c = kb_card_at(col, index);

    if(c == NULL || body == NULL)
        return 0;
    free(c->body);
    c->body = strdup(body);
    kb_save_card(c);
    return 1;
}

int
krait_kanban_set_project(int col, int index, const char *project)
{
    KbCard *c = kb_card_at(col, index);

    if(c == NULL || project == NULL)
        return 0;
    snprintf(c->project, sizeof(c->project), "%s", project);
    kb_save_card(c);
    return 1;
}

int
krait_kanban_move(int col, int index, int to_col)
{
    KbCard *c = kb_card_at(col, index);
    char dst[KRAIT_PATH_MAX * 2];
    const char *slash;

    if(c == NULL || to_col < 0 || to_col > 3 || to_col == col)
        return 0;
    slash = strrchr(c->path, '/');
    if(slash == NULL)
        return 0;
    snprintf(dst, sizeof(dst), "%s/%s/%s.txt", kb_dir, kb_columns[to_col],
             c->id);
    if(rename(c->path, dst) != 0)
        return 0;
    krait_kanban_rescan();
    return 1;
}

int
krait_kanban_delete(int col, int index)
{
    KbCard *c = kb_card_at(col, index);
    char dir[KRAIT_PATH_MAX * 2];
    char cmd[KRAIT_PATH_MAX * 8];

    if(c == NULL)
        return 0;
    unlink(c->path);
    if(kb_proposal_dir(c, dir, sizeof(dir))) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if(system(cmd) != 0) {
            /* best effort; stale proposals rescan away */
        }
    }
    krait_kanban_rescan();
    return 1;
}

/* New project card: scaffold a real project directory and bind the card to
 * it. Reuses the native_scaffold template writer. */
int
krait_kanban_new_project(int col, int index, const char *dir_path)
{
    KbCard *c = kb_card_at(col, index);
    char status[512];

    if(c == NULL || dir_path == NULL || dir_path[0] == '\0')
        return 0;
    if(!krait_scaffold_project(dir_path, status, sizeof(status)))
        return 0;
    krait_kanban_set_project(col, index, dir_path);
    return 1;
}

int
krait_kanban_ai_configured(void)
{
    return krait_ai_configured();
}

/* Build the user prompt: the card task plus enough project context for the
 * model to author whole files. */
static void
kb_build_prompt(KbCard *c, char *dst, size_t dst_size)
{
    size_t n = 0;

    n += (size_t)snprintf(dst + n, dst_size - n, "%s\n\n", c->body != NULL &&
                           c->body[0] != '\0' ? c->body : c->title);
    if(c->project[0] != '\0') {
        char manifest_path[KRAIT_PATH_MAX * 2];
        DIR *d;

        n += (size_t)snprintf(dst + n, dst_size - n,
                              "Project directory: %s\nFiles:\n", c->project);
        d = opendir(c->project);
        if(d != NULL) {
            struct dirent *e;
            int shown = 0;

            while((e = readdir(d)) != NULL && shown < 24) {
                if(e->d_name[0] == '.')
                    continue;
                n += (size_t)snprintf(dst + n, dst_size - n, "  %s\n",
                                      e->d_name);
                shown++;
            }
            closedir(d);
        }
        snprintf(manifest_path, sizeof(manifest_path), "%s/main.kry",
                 c->project);
        {
            char *text = NULL;
            long len;

            if(krait_read_file_alloc(manifest_path, &text, &len) &&
               text != NULL) {
                n += (size_t)snprintf(dst + n, dst_size - n,
                                      "\nCurrent main.kry:\n```\n%s\n```\n",
                                      text);
                free(text);
            }
        }
    }
    snprintf(dst + n, dst_size - n,
             "\nAnswer with ONLY a JSON object: {\"files\":[{\"path\":"
             "\"relative/path\",\"content\":\"full file text\"}]} — no "
             "markdown fences, no commentary.");
}

static const char *const kb_system_prompt =
    "You are a coding agent working on Kryon projects. Kry (.kry) is a "
    "Jai-like language lowered through KIR to the project's target backend, "
    "commonly C or Go. It uses explicit braces, :: declarations, C-style "
    "calls and structs, and statement order inside blocks. STRICT syntax "
    "rules: no // comments; no 'let' (declare with 'name := expr' or "
    "'name: Type = value'); types use 'T name' order when handwritten C is "
    "embedded. UI entry points look like: Main :: () #ui { Screen root: "
    "{ ... } }. Widget calls: StyledButton(x, y, w, h, \"label\", "
    "ButtonStylePrimary, 0, NULL) returns 1 when clicked; Text(text, x, y, "
    "ScaleUIPx(16), GetThemeText()); DrawRectangleRec((Rectangle){x, y, w, "
    "h}, GetThemeButton()). Coordinates are int pixels - wrap sizes with "
    "ScaleUIPx(n), use (int) casts on floats. Study main.kry in the context "
    "and copy its idioms exactly. Return complete file contents for every "
    "file you create or change; the harness writes them verbatim after "
    "review.";

/* Drop a rejected attempt: free the in-memory copies and remove the
 * proposal dir so the next attempt starts from a clean slate. */
static void
kb_clear_proposals(KbCard *c)
{
    char dir[KRAIT_PATH_MAX * 2];
    char cmd[KRAIT_PATH_MAX * 8];
    int i;

    for(i = 0; i < c->proposal_count; i++)
        free(c->proposal_bodies[i]);
    memset(c->proposal_paths, 0, sizeof(c->proposal_paths));
    memset(c->proposal_bodies, 0, sizeof(c->proposal_bodies));
    c->proposal_count = 0;
    if(kb_proposal_dir(c, dir, sizeof(dir))) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        system(cmd);
    }
}

/* Start (or restart) the AI chat for a card. A retry_error means the
 * previous attempt failed validation: the prompt is rebuilt with the
 * compiler diagnostic and the rejected files appended so the model can
 * correct itself, and the old proposals are cleared. */
static int
kb_ai_start(KbCard *c, const char *retry_error)
{
    char prompt[24576];
    KraitAiMessage msgs[2] = {0};

    kb_build_prompt(c, prompt, sizeof(prompt));
    if(retry_error != NULL && retry_error[0] != '\0') {
        int i;
        size_t used = strlen(prompt);

        snprintf(prompt + used, sizeof(prompt) - used,
                 "\n\nPREVIOUS ATTEMPT REJECTED BY VALIDATION.\n"
                 "first error: %s\nFix the Kry code and return the corrected "
                 "complete files.", retry_error);
        for(i = 0; i < c->proposal_count && i < 3; i++) {
            used = strlen(prompt);
            snprintf(prompt + used, sizeof(prompt) - used,
                     "\n--- rejected %s (truncated) ---\n%.1200s\n",
                     c->proposal_paths[i],
                     c->proposal_bodies[i] != NULL ? c->proposal_bodies[i]
                                                   : "");
        }
        kb_clear_proposals(c);
    }
    c->ai_attempts++;
    msgs[0].role = "system";
    msgs[0].content = kb_system_prompt;
    msgs[1].role = "user";
    msgs[1].content = prompt;
    c->ai = krait_ai_chat(msgs, 2, 180);
    if(c->ai == NULL) {
        snprintf(c->status, sizeof(c->status), "AI start failed");
        return 0;
    }
    if(c->ai_attempts > 1)
        snprintf(c->status, sizeof(c->status), "retrying (attempt %d)...",
                 c->ai_attempts);
    else
        snprintf(c->status, sizeof(c->status), "running...");
    return 1;
}

int
krait_kanban_ai_run(int col, int index)
{
    KbCard *c = kb_card_at(col, index);

    if(c == NULL || !krait_ai_configured())
        return 0;
    if(c->ai != NULL)
        return 1;   /* already running */
    c->ai_attempts = 0;
    c->ai_error[0] = '\0';
    return kb_ai_start(c, NULL);
}

/* Transpile the project overlayed with the proposals through the shared
 * compile gate; the verdict lands on the card status so Apply only ever
 * offers code that compiles (or shows the first error for review).
 * Returns 0 when the proposal compiles (or cannot be checked), nonzero
 * otherwise, with the first diagnostic copied into err_out for the
 * retry loop. */
static int
kb_validate_proposals(KbCard *c, char *err_out, size_t err_size)
{
    const char *paths[KB_MAX_PROPOSAL_FILES];
    const char *bodies[KB_MAX_PROPOSAL_FILES];
    char first_error[160];
    int i;
    int rc;

    if(err_out != NULL && err_size > 0)
        err_out[0] = '\0';
    if(c->project[0] == '\0') {
        snprintf(c->status, sizeof(c->status), "proposal ready");
        return 0;
    }
    for(i = 0; i < c->proposal_count && i < KB_MAX_PROPOSAL_FILES; i++) {
        paths[i] = c->proposal_paths[i];
        bodies[i] = c->proposal_bodies[i];
    }
    rc = krait_compile_gate_all(c->project, paths, bodies,
                                c->proposal_count < KB_MAX_PROPOSAL_FILES ?
                                    c->proposal_count : KB_MAX_PROPOSAL_FILES,
                                first_error, sizeof(first_error), NULL, 0);
    if(rc == 0) {
        snprintf(c->status, sizeof(c->status), "proposal ready (compiles)");
        return 0;
    }
    krait_trim(first_error);
    snprintf(c->status, sizeof(c->status), "proposal ready (k2c: %s)",
             first_error[0] != '\0' ? first_error : "compile failed");
    if(err_out != NULL && err_size > 0)
        snprintf(err_out, err_size, "%s",
                 first_error[0] != '\0' ? first_error : "compile failed");
    return 1;
}

/* 0 idle, 1 running, 2 proposal ready, 3 failed */
static int
kb_ai_state(KbCard *c)
{
    if(c == NULL)
        return 0;
    if(c->ai != NULL)
        return 1;
    if(c->proposal_count > 0)
        return 2;
    if(c->status[0] != '\0')
        return 3;
    return 0;
}

int
krait_kanban_ai_poll(int col, int index)
{
    KbCard *c = kb_card_at(col, index);
    KraitAiStatus s;
    const char *text;
    int retry = 0;

    if(c == NULL)
        return 0;
    if(c->ai == NULL)
        return kb_ai_state(c);
    s = krait_ai_poll(c->ai);
    if(s == KRAIT_AI_RUNNING || s == KRAIT_AI_PENDING)
        return 1;
    text = krait_ai_text(c->ai);
    if(s == KRAIT_AI_DONE && text != NULL) {
        /* models sometimes wrap JSON in fences; strip one pair */
        char *json = text;
        size_t n;

        while(*json == '\n' || *json == ' ')
            json++;
        n = strlen(json);
        while(n > 0 && (json[n - 1] == '\n' || json[n - 1] == ' '))
            json[--n] = '\0';
        if(strncmp(json, "```", 3) == 0) {
            char *end;

            json += 3;
            while(*json != '\0' && *json != '\n')
                json++;
            if(*json == '\n')
                json++;
            end = strstr(json, "```");
            if(end != NULL)
                *end = '\0';
        }
        {
            KryJson *root = kry_json_parse(json);
            KryJson *files = root != NULL ? kry_json_get(root, "files") : NULL;
            char dir[KRAIT_PATH_MAX];
            int i;
            int written = 0;

            if(!kb_proposal_dir(c, dir, sizeof(dir)))
                root = NULL;
            if(root != NULL && files != NULL) {
                for(i = 0; i < kry_json_count(files); i++) {
                    KryJson *f = kry_json_at(files, i);
                    const char *rel = kry_json_string(kry_json_get(f, "path"));
                    const char *content = kry_json_string(kry_json_get(f, "content"));

                    if(rel == NULL || rel[0] == '\0' || content == NULL)
                        continue;
                    if(strstr(rel, "..") != NULL || rel[0] == '/')
                        continue;   /* refuse escapes */
                    {
                        char path[KRAIT_PATH_MAX * 2];

                        snprintf(path, sizeof(path), "%s/%s", dir, rel);
                        krait_ensure_parent_dir(path);
                        if(krait_write_text_file(path, content))
                            written++;
                    }
                }
            }
            kry_json_free(root);
            if(written > 0) {
                char retry_error[256];

                kb_scan_proposals(c);
                retry = kb_validate_proposals(c, retry_error,
                                              sizeof(retry_error)) != 0;
                if(retry)
                    snprintf(c->ai_error, sizeof(c->ai_error), "%s",
                             retry_error);
            } else {
                retry = 1;
                snprintf(c->ai_error, sizeof(c->ai_error),
                         "reply was not usable JSON with files");
                snprintf(c->status, sizeof(c->status),
                         "AI reply had no usable files");
            }
        }
    } else {
        const char *err = krait_ai_error(c->ai);

        snprintf(c->status, sizeof(c->status), "%s",
                 err != NULL ? err : "AI request failed");
    }
    krait_ai_free(c->ai);
    c->ai = NULL;
    if(retry && c->ai_attempts < KB_AI_MAX_ATTEMPTS &&
       kb_ai_start(c, c->ai_error))
        return 1;
    return kb_ai_state(c);
}

int
krait_kanban_proposal_count(int col, int index)
{
    KbCard *c = kb_card_at(col, index);

    return c != NULL ? c->proposal_count : 0;
}

const char *
krait_kanban_proposal_path(int col, int index, int file_index)
{
    KbCard *c = kb_card_at(col, index);

    if(c == NULL || file_index < 0 || file_index >= c->proposal_count)
        return "";
    return c->proposal_paths[file_index];
}

const char *
krait_kanban_proposal_content(int col, int index, int file_index)
{
    KbCard *c = kb_card_at(col, index);

    if(c == NULL || file_index < 0 || file_index >= c->proposal_count)
        return "";
    return c->proposal_bodies[file_index] != NULL
               ? c->proposal_bodies[file_index] : "";
}

/* Agent bridge hooks: exposed here so kanban UI code stays in kanban. */
int
krait_kanban_agent_ready(void)
{
    return krait_ai_configured() && !krait_agent_busy();
}

int
krait_kanban_agent_bridge(int col, int index)
{
    return krait_agent_bridge_card(col, index);
}

int
krait_kanban_apply(int col, int index)
{
    KbCard *c = kb_card_at(col, index);
    int i;
    int applied = 0;

    if(c == NULL || c->proposal_count == 0 || c->project[0] == '\0')
        return 0;
    for(i = 0; i < c->proposal_count; i++) {
        char src[KRAIT_PATH_MAX * 2];
        char dst[KRAIT_PATH_MAX * 2];
        char bak[KRAIT_PATH_MAX * 2];
        char *orig = NULL;
        long len;

        if(krait_path_has_suffix(c->proposal_paths[i], ".bak"))
            continue;
        snprintf(src, sizeof(src), "%s/.proposals/%s/%s", kb_dir, c->id,
                 c->proposal_paths[i]);
        snprintf(dst, sizeof(dst), "%s/%s", c->project,
                 c->proposal_paths[i]);
        krait_ensure_parent_dir(dst);
        snprintf(bak, sizeof(bak), "%s.bak", dst);
        if(krait_read_file_alloc(dst, &orig, &len))
            krait_write_text_file(bak, orig);
        free(orig);
        if(krait_write_text_file(dst, c->proposal_bodies[i] != NULL
                                    ? c->proposal_bodies[i] : ""))
            applied++;
    }
    if(applied > 0)
        snprintf(c->status, sizeof(c->status), "applied %d file(s)", applied);
    return applied;
}

int
krait_kanban_reject(int col, int index)
{
    KbCard *c = kb_card_at(col, index);
    char dir[KRAIT_PATH_MAX * 2];
    char cmd[KRAIT_PATH_MAX * 8];
    int i;

    if(c == NULL)
        return 0;
    if(!kb_proposal_dir(c, dir, sizeof(dir)))
        return 1;
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if(system(cmd) == 0 || !krait_path_exists(dir)) {
        for(i = 0; i < c->proposal_count; i++)
            free(c->proposal_bodies[i]);
        c->proposal_count = 0;
        c->status[0] = '\0';
        return 1;
    }
    return 0;
}

void
krait_kanban_shutdown(void)
{
    int col;
    int i;

    for(col = 0; col < 4; col++)
        for(i = 0; i < kb_board[col].count; i++)
            if(kb_board[col].cards[i].ai != NULL) {
                krait_ai_free(kb_board[col].cards[i].ai);
                kb_board[col].cards[i].ai = NULL;
            }
    kb_free_all();
}
