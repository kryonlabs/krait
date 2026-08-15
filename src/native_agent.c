/*
 * native_agent.c - the agent-view conversation engine.
 *
 * A ZCode-style loop over the z.ai client: the model answers with plain
 * text or a JSON array of tool actions (list/read/write/compile/run).
 * Actions run against the bound project directly - writes back up the
 * original file and land on disk, and every write batch is followed by
 * the shared compile gate whose diagnostics feed back into the next
 * round. The transcript persists per project as JSON lines under
 * ~/.kryon/krait/agent/<name>-<hash>/history.jsonl. Tool execution is
 * synchronous and blocks the UI thread (compile is seconds, run is
 * capped by timeout); the HTTP wait itself stays async via kry_http.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include "kry_json.h"
#include "kry_sfs.h"

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>

#define AGENT_MSG_USER 0
#define AGENT_MSG_ASSISTANT 1
#define AGENT_MSG_TOOL 2
#define AGENT_MSG_ERROR 3
#define AGENT_MSG_ACTIONS 4

#define AGENT_MAX_MSGS 512
#define AGENT_MAX_ROUNDS 12
#define AGENT_CONTEXT_CHARS 24000
#define AGENT_CONTEXT_KEEP_TOOLS 6
#define AGENT_READ_CAP 16384
#define AGENT_RUN_OUT_CAP 8192
#define AGENT_TOOL_OUT_CAP 16384

typedef struct {
    int kind;
    char *text;   /* malloc'd */
} AgentMsg;

static AgentMsg agent_msgs[AGENT_MAX_MSGS];
static int agent_msg_count;
static char agent_project[KRAIT_PATH_MAX];
static char agent_history_path[KRAIT_PATH_MAX * 2];
static KraitAiRequest *agent_req;
static int agent_round;
static int agent_stop_requested;
static int agent_http_retries;
static int agent_busy;
static char agent_status[160];
static int agent_files_changed;

/* screenshot attachment: captured PNG base64, sent with the next request
 * as a multimodal turn so the model can see the running UI */
static char *agent_pending_image;
static int agent_image_count;

/* tool batches run on a worker thread so compile/run no longer freeze
 * the UI; screenshot batches stay on the GL thread (offscreen render). */
typedef struct AgentToolJob {
    char *json;
    char *result;
    volatile int done;
} AgentToolJob;
static AgentToolJob agent_job;
static KryThread agent_tool_thread;
static int agent_tool_thread_running;

static void *
agent_tool_worker(void *userdata)
{
    AgentToolJob *job = userdata;

    job->result = krait_agent_run_tools(job->json);
    job->done = 1;
    return NULL;
}

/* 1 when the batch needs the GL thread (offscreen render) */
static int
agent_batch_needs_gl(const char *json)
{
    return json != NULL && strstr(json, "\"tool\":\"screenshot\"") != NULL;
}

/* Kick a tool batch: sync for GL-needing batches, threaded otherwise.
 * Returns 1 when the result is already available in *result. */
static int
agent_start_tools(const char *json, char **result)
{
    if(agent_batch_needs_gl(json)) {
        *result = krait_agent_run_tools(json);
        return 1;
    }
    free(agent_job.json);
    free(agent_job.result);
    agent_job.json = strdup(json);
    agent_job.result = NULL;
    agent_job.done = 0;
    if(!KryThreadStart(&agent_tool_thread, agent_tool_worker, &agent_job))
        return 0;
    agent_tool_thread_running = 1;
    return 0;
}

/* 1 with *result set when the threaded batch finished */
static int
agent_poll_tools(char **result)
{
    if(!agent_tool_thread_running || !agent_job.done)
        return 0;
    KryThreadJoin(&agent_tool_thread);
    agent_tool_thread_running = 0;
    *result = agent_job.result;
    agent_job.result = NULL;
    return 1;
}

/* files the current (or most recent) agent batch wrote, so the UI can
 * reload open editors and Revert can restore the .bak copies */
#define AGENT_MAX_WRITTEN 32
static char agent_written[AGENT_MAX_WRITTEN][256];
static int agent_written_count;
static int agent_batch_start;

static const char *const agent_system_prompt =
    "You are a coding agent working inside the Krait IDE on a Kryon "
    "project. Kry (.kry) is a Python-indented UI language lowered to C. "
    "STRICT syntax rules: no comments at all (// and # are errors); no "
    "'let' (declare with 'name := expr' or 'name: Type = value'). Screens "
    "look like: screen Name(viewport: Rectangle) { ... }. Widget calls: "
    "DrawUIGenericButton(x, y, w, h, \"label\", UI_BUTTON_STYLE_PRIMARY, "
    "0, NULL) returns 1 when clicked; DrawUIText(text, x, y, "
    "UI_TEXT_16, GetThemeText()); DrawRectangleRec((Rectangle){x, y, w, "
    "h}, GetThemeButton()). Coordinates are int pixels - wrap sizes with "
    "ScaleUIPx(n), use (int) casts on floats. Read main.kry first and "
    "copy its idioms exactly; write complete file contents, never diffs. "
    "TOOLS: reply EITHER with plain text for the user OR with ONLY a "
    "JSON array of actions, executed in order: "
    "[{\"tool\":\"list\"}, {\"tool\":\"search\",\"query\":\"button\"}, "
    "{\"tool\":\"read\",\"path\":\"main.kry\"}, "
    "{\"tool\":\"write\",\"path\":\"ui.kry\",\"content\":\"...\"}, "
    "{\"tool\":\"compile\"}, {\"tool\":\"run\",\"cmd\":\"make\"}, "
    "{\"tool\":\"screenshot\",\"path\":\"main.kry\"}, "
    "{\"tool\":\"card\",\"title\":\"Follow up\",\"body\":\"notes\"}, "
    "{\"tool\":\"sfs_list\",\"path\":\"/widgets\"}, "
    "{\"tool\":\"sfs_read\",\"path\":\"/widgets/0/bounds\"}, "
    "{\"tool\":\"sfs_write\",\"path\":\"/widgets/0/tap\",\"value\":\"1\"}]. "
    "Writes are applied to the project immediately and a compile check "
    "runs after every write batch - fix any errors it reports before "
    "finishing. screenshot renders a .kry screen offscreen and shows the "
    "image to you on the next turn - use it to check layout work. card "
    "files a kanban card in Backlog for follow-up work. "
    "The sfs_* tools address the RUNNING app through kryon's "
    "synthetic file system: /widgets lists the live widget tree, and "
    "writing /widgets/<i>/tap clicks that widget. Messages beginning "
    "with TOOL RESULTS contain tool output. Finish with a short "
    "plain-text summary of what you changed.";

static void
agent_set_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(agent_status, sizeof(agent_status), fmt, ap);
    va_end(ap);
}

static unsigned int
agent_path_hash(const char *s)
{
    unsigned int h = 2166136261u;

    while(*s != '\0')
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static void
agent_history_dir(char *dst, size_t dst_size)
{
    const char *home = getenv("HOME");
    char name[256];
    const char *base;
    const char *slash;

    if(home == NULL || home[0] == '\0')
        home = ".";
    base = agent_project[0] != '\0' ? agent_project : "none";
    slash = strrchr(base, '/');
    snprintf(name, sizeof(name), "%s",
             slash != NULL && slash[1] != '\0' ? slash + 1 : base);
    {
        char clean[256];
        char *p;

        snprintf(clean, sizeof(clean), "%s", name);
        for(p = clean; *p != '\0'; p++)
            if(*p == '/' || *p == ' ' || *p == ':')
                *p = '_';
        snprintf(dst, dst_size, "%s/.kryon/krait/agent/%s-%08x", home, clean,
                 agent_path_hash(agent_project));
    }
}

static void
agent_msg_free(AgentMsg *m)
{
    free(m->text);
    m->text = NULL;
    m->kind = AGENT_MSG_USER;
}

static void
agent_append(int kind, const char *text)
{
    AgentMsg *m;

    if(agent_msg_count >= AGENT_MAX_MSGS) {
        int i;

        agent_msg_free(&agent_msgs[0]);
        memmove(&agent_msgs[0], &agent_msgs[1],
                sizeof(AgentMsg) * (AGENT_MAX_MSGS - 1));
        agent_msg_count = AGENT_MAX_MSGS - 1;
    }
    m = &agent_msgs[agent_msg_count++];
    m->kind = kind;
    m->text = strdup(text != NULL ? text : "");
}

static void
agent_persist_msg(int kind, const char *text)
{
    FILE *f;

    if(agent_history_path[0] == '\0')
        return;
    f = fopen(agent_history_path, "a");
    if(f == NULL)
        return;
    {
        KryJsonBuf buf = {0};
        const char *line;

        kry_json_buf_raw(&buf, "{\"kind\":");
        kry_json_buf_num(&buf, kind);
        kry_json_buf_raw(&buf, ",\"text\":");
        kry_json_buf_str(&buf, text != NULL ? text : "");
        kry_json_buf_raw(&buf, "}");
        line = kry_json_buf_finish(&buf);
        if(line != NULL)
            fprintf(f, "%s\n", line);
        kry_json_buf_free(&buf);
    }
    fclose(f);
}

/* load every stored message of the transcript file */
static void
agent_load_history(void)
{
    char *text = NULL;
    long len;
    char *p;

    if(!krait_read_file_alloc(agent_history_path, &text, &len) ||
       text == NULL) {
        free(text);
        return;
    }
    p = text;
    while(*p != '\0') {
        char *nl = strchr(p, '\n');

        if(nl != NULL)
            *nl = '\0';
        if(p[0] != '\0') {
            KryJson *root = kry_json_parse(p);
            const char *msg_text;

            if(getenv("KRAIT_AGENT_DEBUG") != NULL)
                fprintf(stderr, "agent-load: parse %s -> %s\n", p,
                        root != NULL ? "ok" : "FAIL");
            if(root != NULL) {
                msg_text = kry_json_string(kry_json_get(root, "text"));
                if(msg_text != NULL)
                    agent_append(
                        (int)kry_json_number(kry_json_get(root, "kind")),
                        msg_text);
                kry_json_free(root);
            }
        }
        if(nl == NULL)
            break;
        p = nl + 1;
    }
    free(text);
}

static void
agent_remember(int kind, const char *text)
{
    agent_append(kind, text);
    agent_persist_msg(kind, text);
}

static int
agent_path_safe(const char *rel)
{
    if(rel == NULL || rel[0] == '\0' || rel[0] == '/')
        return 0;
    if(strstr(rel, "..") != NULL)
        return 0;
    return 1;
}

/* Execute one parsed action; appends a compact result line to out. */
static void
agent_tool_list(char *out, size_t out_size)
{
    size_t used = strlen(out);

    used += (size_t)snprintf(out + used, out_size - used, "[list]\n");
    {
        int exit_status = krait_run_capture(agent_project,
            "find . -type f -not -path './.git/*' -not -path './out/*' "
            "| LC_ALL=C sort | head -200", 10,
            out + used, out_size - used);

        if(exit_status != 0 && used + 16 < out_size)
            snprintf(out + used, out_size - used, "(find failed: %d)\n",
                     exit_status);
    }
    krait_trim(out);
}

static void
agent_tool_read(const char *path, char *out, size_t out_size)
{
    char full[KRAIT_PATH_MAX * 2];
    char *text = NULL;
    long len;
    size_t used = strlen(out);

    if(!agent_path_safe(path)) {
        snprintf(out + used, out_size - used, "[read] refused: %s\n",
                 path != NULL ? path : "");
        return;
    }
    snprintf(full, sizeof(full), "%s/%s", agent_project, path);
    if(!krait_read_file_alloc(full, &text, &len) || text == NULL) {
        snprintf(out + used, out_size - used, "[read] no such file: %s\n",
                 path);
        free(text);
        return;
    }
    if(len > AGENT_READ_CAP)
        text[AGENT_READ_CAP] = '\0';
    snprintf(out + used, out_size - used, "[read %s]\n%s\n", path, text);
    free(text);
}

/* rough +/- line counts between the backup and the new content, shown
 * with every write so the transcript reads like a changelog */
static void
agent_diff_counts(const char *orig, const char *next, int *added, int *removed)
{
    const char *p = orig != NULL ? orig : "";
    const char *q = next != NULL ? next : "";
    const char *pe;
    const char *qe;

    *added = 0;
    *removed = 0;
    for(;;) {
        pe = strchr(p, '\n');
        qe = strchr(q, '\n');
        if(pe == NULL && qe == NULL) {
            if(strcmp(p, q) != 0 && (*p != '\0' || *q != '\0')) {
                (*added)++;
                (*removed)++;
            }
            break;
        }
        if(pe == NULL) {
            (*added)++;
            q = qe + 1;
            continue;
        }
        if(qe == NULL) {
            (*removed)++;
            p = pe + 1;
            continue;
        }
        {
            size_t plen = (size_t)(pe - p);
            size_t qlen = (size_t)(qe - q);

            if(plen != qlen || memcmp(p, q, plen) != 0) {
                (*added)++;
                (*removed)++;
            }
        }
        p = pe + 1;
        q = qe + 1;
    }
}

static int agent_last_write_added;
static int agent_last_write_removed;

static int
agent_tool_write(const char *path, const char *content)
{
    char full[KRAIT_PATH_MAX * 2];
    char bak[KRAIT_PATH_MAX * 2];
    char *orig = NULL;
    long len;
    int had_orig;

    if(!agent_path_safe(path))
        return 0;
    snprintf(full, sizeof(full), "%s/%s", agent_project, path);
    snprintf(bak, sizeof(bak), "%s.bak", full);
    krait_ensure_parent_dir(full);
    had_orig = krait_read_file_alloc(full, &orig, &len);
    if(had_orig)
        krait_write_text_file(bak, orig);
    if(!krait_write_text_file(full, content != NULL ? content : "")) {
        free(orig);
        return 0;
    }
    agent_diff_counts(had_orig ? orig : NULL,
                      content != NULL ? content : "",
                      &agent_last_write_added, &agent_last_write_removed);
    if(!had_orig)
        agent_last_write_removed = 0;
    free(orig);
    if(agent_written_count < AGENT_MAX_WRITTEN)
        snprintf(agent_written[agent_written_count++],
                 sizeof(agent_written[0]), "%s", path);
    agent_files_changed = 1;
    return 1;
}

/* The UI reloads open editors from disk when the agent reports written
 * files; Revert restores the .bak backups of the latest batch. */
int
krait_agent_written_count(void)
{
    return agent_written_count;
}

const char *
krait_agent_written_path(int index)
{
    if(index < 0 || index >= agent_written_count)
        return "";
    return agent_written[index];
}

int
krait_agent_can_revert(void)
{
    return agent_written_count > agent_batch_start && !agent_busy;
}

int
krait_agent_revert(void)
{
    int i;
    int restored = 0;

    if(!krait_agent_can_revert())
        return 0;
    for(i = agent_batch_start; i < agent_written_count; i++) {
        char full[KRAIT_PATH_MAX * 2];
        char bak[KRAIT_PATH_MAX * 2];
        char *orig = NULL;
        long len;

        snprintf(full, sizeof(full), "%s/%s", agent_project,
                 agent_written[i]);
        snprintf(bak, sizeof(bak), "%s.bak", full);
        if(krait_read_file_alloc(bak, &orig, &len) && orig != NULL) {
            if(krait_write_text_file(full, orig))
                restored++;
            free(orig);
        }
    }
    agent_written_count = agent_batch_start;
    agent_files_changed = 1;
    agent_set_status("reverted %d file(s)", restored);
    return restored;
}

static void
agent_tool_compile(char *out, size_t out_size)
{
    char err[256];
    int rc = krait_compile_gate(agent_project, NULL, NULL, 0, err,
                                sizeof(err));
    size_t used = strlen(out);

    if(rc == 0)
        snprintf(out + used, out_size - used, "[compile] ok\n");
    else
        snprintf(out + used, out_size - used,
                 "[compile] FAILED: %s\n", err[0] != '\0' ? err : "unknown");
}

static void
agent_tool_run(const char *cmd, char *out, size_t out_size)
{
    char buf[AGENT_RUN_OUT_CAP];
    int exit_status;
    size_t used = strlen(out);

    if(cmd == NULL || cmd[0] == '\0') {
        snprintf(out + used, out_size - used, "[run] refused: empty cmd\n");
        return;
    }
    if(strstr(cmd, "rm -rf /") != NULL || system(NULL) == 0) {
        snprintf(out + used, out_size - used, "[run] refused: %s\n", cmd);
        return;
    }
    exit_status = krait_run_capture(agent_project, cmd, 60, buf, sizeof(buf));
    if(exit_status < 0) {
        snprintf(out + used, out_size - used, "[run] spawn failed: %s\n", cmd);
        return;
    }
    snprintf(out + used, out_size - used, "[run '%s'] exit %d\n%s\n", cmd,
             exit_status, buf);
}

/* Render the project's screen offscreen, encode the PNG, and hold it for
 * the next request. The tool result reports the size so the model knows
 * the screenshot landed. */
static void
agent_tool_screenshot(const char *path, char *out, size_t out_size)
{
    char png[KRAIT_PATH_MAX * 2];
    char status[512];
    char rel[256];
    size_t used = strlen(out);

    snprintf(rel, sizeof(rel), "%s",
             path != NULL && path[0] != '\0' ? path : "main.kry");
    snprintf(png, sizeof(png), "/tmp/krait-agent-shot-%d.png", (int)getpid());
    if(!krait_live_capture_png(agent_project, rel, 640, 480, png, status,
                               sizeof(status))) {
        snprintf(out + used, out_size - used,
                 "[screenshot %s] failed: %s\n", rel,
                 status[0] != '\0' ? status : "render error");
        return;
    }
    free(agent_pending_image);
    agent_pending_image = krait_ai_base64_file(png);
    if(agent_pending_image == NULL) {
        snprintf(out + used, out_size - used,
                 "[screenshot %s] failed: could not encode %s\n", rel, png);
        return;
    }
    agent_image_count++;
    snprintf(out + used, out_size - used,
             "[screenshot %s] attached (%zu bytes base64) - study it "
             "before answering\n", rel, strlen(agent_pending_image));
}

/* create a kanban card from the agent: the two agentic surfaces connect */
static void
agent_tool_card(const char *title, const char *body, char *out, size_t out_size)
{
    size_t used = strlen(out);
    int index;

    if(title == NULL || title[0] == '\0') {
        snprintf(out + used, out_size - used, "[card] refused: no title\n");
        return;
    }
    index = krait_kanban_create(0, title);
    if(index < 0) {
        snprintf(out + used, out_size - used, "[card] failed\n");
        return;
    }
    krait_kanban_set_body(0, index, body != NULL ? body : "");
    snprintf(out + strlen(out), out_size - strlen(out),
             "[card '%s'] created in Backlog\n", title);
}

/* content search over the project through the same engine the studio
 * search pane uses */
static void
agent_tool_search(const char *query, char *out, size_t out_size)
{
    SearchResult results[24];
    size_t used = strlen(out);
    int found;

    if(query == NULL || query[0] == '\0') {
        snprintf(out + used, out_size - used, "[search] refused: no query\n");
        return;
    }
    found = krait_search_project(agent_project, query, results, 24);
    used += (size_t)snprintf(out + used, out_size - used,
                             "[search '%s'] %d match(es)\n", query, found);
    for(int i = 0; i < found && i < 24; i++)
        used += (size_t)snprintf(out + used, out_size - used, "  %s:%d: %s\n",
                                 results[i].path, results[i].line,
                                 results[i].excerpt);
}

/* Parse and execute a JSON array of tool actions. Returns the compact
 * TOOL RESULTS text (malloc'd). Public so tests drive the exact tool
 * path the live loop uses. */
char *
krait_agent_run_tools(const char *json)
{
    KryJson *root = kry_json_parse(json);
    char *out;
    int i;
    int count;
    int wrote = 0;

    out = malloc(AGENT_TOOL_OUT_CAP);
    if(out == NULL) {
        kry_json_free(root);
        return NULL;
    }
    out[0] = '\0';
    if(root == NULL) {
        snprintf(out, AGENT_TOOL_OUT_CAP, "TOOL RESULTS: bad action json\n");
        return out;
    }
    agent_batch_start = agent_written_count;
    count = kry_json_count(root);
    for(i = 0; i < count; i++) {
        KryJson *action = kry_json_at(root, i);
        const char *tool = kry_json_string(kry_json_get(action, "tool"));

        if(tool == NULL)
            continue;
        if(strcmp(tool, "list") == 0)
            agent_tool_list(out, AGENT_TOOL_OUT_CAP);
        else if(strcmp(tool, "search") == 0)
            agent_tool_search(kry_json_string(kry_json_get(action, "query")),
                              out, AGENT_TOOL_OUT_CAP);
        else if(strcmp(tool, "read") == 0)
            agent_tool_read(kry_json_string(kry_json_get(action, "path")),
                            out, AGENT_TOOL_OUT_CAP);
        else if(strcmp(tool, "write") == 0) {
            const char *path = kry_json_string(kry_json_get(action, "path"));
            const char *content = kry_json_string(kry_json_get(action,
                                                                "content"));

            if(agent_tool_write(path, content)) {
                size_t used = strlen(out);

                snprintf(out + used, AGENT_TOOL_OUT_CAP - used,
                         "[write %s] ok (+%d -%d lines)\n",
                         path != NULL ? path : "", agent_last_write_added,
                         agent_last_write_removed);
                wrote = 1;
            } else {
                size_t used = strlen(out);

                snprintf(out + used, AGENT_TOOL_OUT_CAP - used,
                         "[write] refused: %s\n",
                         path != NULL ? path : "");
            }
        } else if(strcmp(tool, "card") == 0)
            agent_tool_card(kry_json_string(kry_json_get(action, "title")),
                            kry_json_string(kry_json_get(action, "body")),
                            out, AGENT_TOOL_OUT_CAP);
        else if(strcmp(tool, "screenshot") == 0)
            agent_tool_screenshot(kry_json_string(kry_json_get(action,
                                                               "path")),
                                  out, AGENT_TOOL_OUT_CAP);
        else if(strcmp(tool, "compile") == 0)
            agent_tool_compile(out, AGENT_TOOL_OUT_CAP);
        else if(strcmp(tool, "run") == 0)
            agent_tool_run(kry_json_string(kry_json_get(action, "cmd")),
                           out, AGENT_TOOL_OUT_CAP);
        else if(strcmp(tool, "sfs_list") == 0 ||
                strcmp(tool, "sfs_read") == 0 ||
                strcmp(tool, "sfs_write") == 0) {
            const char *path = kry_json_string(kry_json_get(action, "path"));

            if(strcmp(tool, "sfs_list") == 0) {
                KrySfsEntry entries[32];
                int n = path != NULL ? KrySfsList(path, entries, 32) : -1;
                size_t used = strlen(out);

                if(n < 0) {
                    snprintf(out + used, AGENT_TOOL_OUT_CAP - used,
                             "[sfs_list %s] failed\n",
                             path != NULL ? path : "");
                } else {
                    snprintf(out + used, AGENT_TOOL_OUT_CAP - used,
                             "[sfs_list %s] %d entries\n",
                             path != NULL ? path : "", n);
                    for(int e = 0; e < n; e++)
                        snprintf(out + strlen(out),
                                 AGENT_TOOL_OUT_CAP - strlen(out), "  %s%s\n",
                                 entries[e].name,
                                 entries[e].is_dir ? "/" : "");
                }
            } else if(strcmp(tool, "sfs_read") == 0) {
                char buf[512];
                int n = path != NULL ? KrySfsRead(path, buf, sizeof(buf))
                                     : KRY_SFS_ENOENT;
                size_t used = strlen(out);

                if(n < 0)
                    snprintf(out + used, AGENT_TOOL_OUT_CAP - used,
                             "[sfs_read %s] not found\n",
                             path != NULL ? path : "");
                else
                    snprintf(out + used, AGENT_TOOL_OUT_CAP - used,
                             "[sfs_read %s] %s", path, buf);
            } else {
                const char *value = kry_json_string(kry_json_get(action,
                                                                 "value"));
                int rc = KrySfsWrite(path != NULL ? path : "",
                                     value != NULL ? value : "");
                size_t used = strlen(out);

                snprintf(out + used, AGENT_TOOL_OUT_CAP - used,
                         "[sfs_write %s] %s\n", path != NULL ? path : "",
                         rc == 1 ? "ok" : "failed");
            }
        }
    }
    kry_json_free(root);
    if(wrote)
        agent_tool_compile(out, AGENT_TOOL_OUT_CAP);
    return out;
}

/* Is the reply a tool-action array? An array whose first element has a
 * "tool" key. */
static int
agent_reply_is_actions(const char *text)
{
    KryJson *root = kry_json_parse(text);
    int is_actions = 0;

    if(root != NULL && kry_json_count(root) > 0) {
        KryJson *first = kry_json_at(root, 0);

        if(first != NULL && kry_json_get(first, "tool") != NULL)
            is_actions = 1;
    }
    kry_json_free(root);
    return is_actions;
}

/* Strip one markdown fence pair and surrounding whitespace, in place. */
static char *
agent_strip_reply(char *text)
{
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
    return json;
}

/* Build the message list for the next request: system prompt plus the
 * newest transcript messages that fit the context budget. Tool results
 * get a "TOOL RESULTS" prefix as user-role text (the API has no tool
 * role); prefixed copies are malloc'd into temps and must be freed by
 * the caller after krait_ai_chat has copied the body. */
static int
agent_build_context(KraitAiMessage *msgs, int max, char *temps[], int *temp_count)
{
    int count = 0;
    int i;
    size_t used = 0;

    const char *pending_image = agent_pending_image;
    int tool_seen = 0;

    *temp_count = 0;
    msgs[count].role = "system";
    msgs[count].content = agent_system_prompt;
    msgs[count].image_b64 = NULL;
    count++;
    for(i = agent_msg_count - 1; i >= 0 && count < max; i--) {
        AgentMsg *m = &agent_msgs[i];
        const char *role = "user";
        char *content = NULL;
        const char *image = NULL;

        /* compaction: bulky tool output older than the newest few
         * rounds drops out of context; the conversation stays */
        if(m->kind == AGENT_MSG_TOOL || m->kind == AGENT_MSG_ACTIONS) {
            if(tool_seen >= AGENT_CONTEXT_KEEP_TOOLS)
                continue;
            tool_seen++;
        }

        /* the newest user/tool turn carries the screenshot */
        if(pending_image != NULL &&
           (m->kind == AGENT_MSG_USER || m->kind == AGENT_MSG_TOOL)) {
            image = pending_image;
            pending_image = NULL;
        }
        if(m->kind == AGENT_MSG_ASSISTANT || m->kind == AGENT_MSG_ACTIONS)
            role = "assistant";
        if(m->kind == AGENT_MSG_TOOL) {
            size_t need = strlen(m->text) + 32;

            content = malloc(need);
            if(content == NULL)
                continue;
            snprintf(content, need, "TOOL RESULTS:\n%s", m->text);
            temps[(*temp_count)++] = content;
        }
        used += strlen(m->text) + 32;
        if(used > AGENT_CONTEXT_CHARS && count > 1) {
            if(content != NULL)
                (*temp_count)--;
            free(content);
            break;
        }
        msgs[count].role = role;
        msgs[count].content = content != NULL ? content : m->text;
        msgs[count].image_b64 = image;
        count++;
    }
    /* reverse the tail so the conversation reads chronologically */
    {
        int first = 1;
        int last = count - 1;

        while(first < last) {
            KraitAiMessage tmp = msgs[first];

            msgs[first] = msgs[last];
            msgs[last] = tmp;
            first++;
            last--;
        }
    }
    return count;
}

static void
agent_start_request(void)
{
    KraitAiMessage msgs[AGENT_MAX_MSGS + 2];
    char *temps[AGENT_MAX_MSGS + 2];
    int temp_count = 0;
    int count = agent_build_context(msgs, AGENT_MAX_MSGS + 2, temps,
                                    &temp_count);
    int i;

    agent_req = krait_ai_chat(msgs, count, 180);
    for(i = 0; i < temp_count; i++)
        free(temps[i]);
    free(agent_pending_image);
    agent_pending_image = NULL;
    if(agent_req == NULL) {
        agent_busy = 0;
        agent_set_status("agent request failed to start");
        agent_remember(AGENT_MSG_ERROR, "agent request failed to start");
        return;
    }
    snprintf(agent_status, sizeof(agent_status), "thinking (round %d/%d)...",
             agent_round + 1, AGENT_MAX_ROUNDS);
}

static void
agent_finish(const char *text)
{
    const char *usage = krait_ai_last_usage();

    agent_busy = 0;
    agent_round = 0;
    agent_set_status("%s", usage[0] != '\0' ? usage : "");
    if(text != NULL && text[0] != '\0')
        agent_remember(AGENT_MSG_ASSISTANT, text);
}

void
krait_agent_bind(const char *project_dir)
{
    char dir[KRAIT_PATH_MAX * 2];
    int i;

    if(project_dir == NULL)
        project_dir = "";
    if(strcmp(agent_project, project_dir) == 0)
        return;
    if(agent_req != NULL) {
        krait_ai_free(agent_req);
        agent_req = NULL;
    }
    for(i = 0; i < agent_msg_count; i++)
        agent_msg_free(&agent_msgs[i]);
    agent_msg_count = 0;
    agent_busy = 0;
    agent_round = 0;
    agent_stop_requested = 0;
    agent_set_status("");
    free(agent_pending_image);
    agent_pending_image = NULL;
    snprintf(agent_project, sizeof(agent_project), "%s", project_dir);
    agent_history_dir(dir, sizeof(dir));
    snprintf(agent_history_path, sizeof(agent_history_path), "%s/history.jsonl",
             dir);
    krait_mkdir_p(dir);
    if(getenv("KRAIT_AGENT_DEBUG") != NULL)
        fprintf(stderr, "agent-bind: project='%s' history='%s'\n",
                agent_project, agent_history_path);
    agent_load_history();
}

int
krait_agent_count(void)
{
    return agent_msg_count;
}

int
krait_agent_kind(int index)
{
    if(index < 0 || index >= agent_msg_count)
        return AGENT_MSG_ERROR;
    return agent_msgs[index].kind;
}

const char *
krait_agent_text(int index)
{
    if(index < 0 || index >= agent_msg_count)
        return "";
    return agent_msgs[index].text != NULL ? agent_msgs[index].text : "";
}

const char *
krait_agent_status_text(void)
{
    return agent_status;
}

int
krait_agent_busy(void)
{
    return agent_busy;
}

int
krait_agent_files_changed(void)
{
    int changed = agent_files_changed;

    agent_files_changed = 0;
    return changed;
}

/* Kanban bridge: hand a card to the agent as a user prompt. The card
 * body plus its project context becomes the next conversation turn. */
int
krait_agent_bridge_card(int col, int index)
{
    char prompt[8192];
    const char *title;
    const char *body;
    const char *project;

    if(!krait_ai_configured() || agent_busy)
        return 0;
    title = krait_kanban_card_title(col, index);
    body = krait_kanban_card_body(col, index);
    project = krait_kanban_card_project(col, index);
    if(title == NULL || title[0] == '\0')
        return 0;
    snprintf(prompt, sizeof(prompt), "Kanban card: %s\n\n%s",
             title, body != NULL ? body : "");
    if(project != NULL && project[0] != '\0')
        krait_agent_bind(project);
    return krait_agent_send(prompt);
}

int
krait_agent_send(const char *text)
{
    if(text == NULL || text[0] == '\0')
        return 0;
    if(agent_busy)
        return 0;
    if(!krait_ai_configured()) {
        agent_remember(AGENT_MSG_ERROR, "AI off: set ZAI_API_KEY");
        return 0;
    }
    agent_remember(AGENT_MSG_USER, text);
    agent_busy = 1;
    agent_round = 0;
    agent_stop_requested = 0;
    agent_start_request();
    return 1;
}

void
krait_agent_stop(void)
{
    if(agent_busy)
        agent_stop_requested = 1;
}

void
krait_agent_clear(void)
{
    char cmd[KRAIT_PATH_MAX * 8];
    int i;

    if(agent_req != NULL) {
        krait_ai_free(agent_req);
        agent_req = NULL;
    }
    for(i = 0; i < agent_msg_count; i++)
        agent_msg_free(&agent_msgs[i]);
    agent_msg_count = 0;
    agent_busy = 0;
    agent_round = 0;
    agent_set_status("");
    if(agent_history_path[0] != '\0') {
        snprintf(cmd, sizeof(cmd), "rm -f '%s'", agent_history_path);
        system(cmd);
    }
}

#define AGENT_MAX_HTTP_RETRIES 2

int
krait_agent_poll(void)
{
    KraitAiStatus s;
    char *reply;
    char *results;

    if(!agent_busy)
        return 1;

    /* a threaded tool batch is in flight */
    if(agent_req == NULL) {
        if(!agent_tool_thread_running)
            return 1;   /* defensive: nothing to do */
        if(!agent_poll_tools(&results))
            return 1;
        if(results == NULL) {
            agent_busy = 0;
            agent_set_status("tool failure");
            return 0;
        }
        agent_remember(AGENT_MSG_TOOL, results);
        free(results);
        if(agent_round >= AGENT_MAX_ROUNDS) {
            agent_busy = 0;
            agent_set_status("stopped: too many tool rounds");
            agent_remember(AGENT_MSG_ERROR, "stopped: too many tool rounds");
            return 0;
        }
        agent_start_request();
        return 1;
    }

    s = krait_ai_poll(agent_req);
    if(s == KRAIT_AI_RUNNING || s == KRAIT_AI_PENDING)
        return 1;
    if(s != KRAIT_AI_DONE) {
        const char *err = krait_ai_error(agent_req);

        krait_ai_free(agent_req);
        agent_req = NULL;
        if(agent_http_retries < AGENT_MAX_HTTP_RETRIES) {
            agent_http_retries++;
            agent_set_status("network hiccup, retry %d/%d...",
                             agent_http_retries, AGENT_MAX_HTTP_RETRIES);
            agent_start_request();
            return 1;
        }
        agent_busy = 0;
        agent_set_status("request failed");
        agent_remember(AGENT_MSG_ERROR, err != NULL ? err : "request failed");
        return 0;
    }
    agent_http_retries = 0;
    reply = agent_strip_reply(strdup(krait_ai_text(agent_req) != NULL ?
                                     krait_ai_text(agent_req) : ""));
    krait_ai_free(agent_req);
    agent_req = NULL;
    if(agent_stop_requested) {
        agent_busy = 0;
        agent_set_status("stopped");
        agent_remember(AGENT_MSG_ASSISTANT,
                       reply[0] != '\0' ? reply : "(stopped)");
        free(reply);
        return 0;
    }
    if(agent_reply_is_actions(reply)) {
        agent_round++;
        agent_remember(AGENT_MSG_ACTIONS, reply);
        agent_set_status("running tools (round %d/%d)...", agent_round,
                         AGENT_MAX_ROUNDS);
        if(agent_start_tools(reply, &results)) {
            /* synchronous batch (or spawn failure): result is ready */
            free(reply);
            if(results == NULL) {
                agent_busy = 0;
                agent_set_status("tool failure");
                return 0;
            }
            agent_remember(AGENT_MSG_TOOL, results);
            free(results);
            if(agent_round >= AGENT_MAX_ROUNDS) {
                agent_busy = 0;
                agent_set_status("stopped: too many tool rounds");
                agent_remember(AGENT_MSG_ERROR, "stopped: too many tool rounds");
                return 0;
            }
            agent_start_request();
            return 1;
        }
        free(reply);   /* threaded batch: poll picks up the result */
        return 1;
    }
    agent_finish(reply);
    free(reply);
    return 0;
}

void
krait_agent_shutdown(void)
{
    int i;

    if(agent_req != NULL) {
        krait_ai_free(agent_req);
        agent_req = NULL;
    }
    for(i = 0; i < agent_msg_count; i++)
        agent_msg_free(&agent_msgs[i]);
    agent_msg_count = 0;
}
