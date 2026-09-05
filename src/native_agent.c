/*
 * native_agent.c - the agent-view conversation engine.
 *
 * A ZCode-style loop over the selected coding-model provider: the model
 * answers with plain text or a JSON array of tool actions
 * (list/read/write/compile/run).
 * Actions run against the bound project directly - writes back up the
 * original file and land on disk, and every write batch is followed by
 * the shared compile gate whose diagnostics feed back into the next
 * round. The transcript persists per project as JSON lines under
 * ~/.kryon/krait/agent/<name>-<hash>/history.jsonl. Tool execution is
 * performed on a worker except graphics operations; HTTP waits stay
 * asynchronous. Task histories and the latest change set survive restart.
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
#include <sys/types.h>
#include <sys/time.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <errno.h>
#include "kry_sha256.h"

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
    char *tool;   /* malloc'd tool name (TOOL/ACTIONS rows) or NULL */
    char *arg;    /* malloc'd short arg summary or NULL */
    int status;   /* tool rows: 0 running, 1 ok, 2 error */
    int dur_ms;
    long ts;      /* wall-clock seconds at append */
} AgentMsg;

/* one executed tool call from the latest batch, captured while the batch
 * ran so the transcript can show per-call cards with status + duration */
#define AGENT_MAX_RECS 64
typedef struct {
    char tool[32];
    char arg[192];
    char result[8192];
    int status;
    int dur_ms;
} AgentToolRec;
static AgentToolRec agent_recs[AGENT_MAX_RECS];
static int agent_rec_count;

static AgentMsg agent_msgs[AGENT_MAX_MSGS];
static int agent_msg_count;
static char agent_project[KRAIT_PATH_MAX];
static char agent_task[128];
static int agent_bind_session(const char *project, const char *task);
static char agent_history_path[KRAIT_PATH_MAX * 2];
static KraitAiRequest *agent_req;
static int agent_round;
static atomic_int agent_stop_requested;
static char agent_run_state[32] = "idle";
static void agent_run_save(const char *state);
static int agent_http_retries;
static int agent_busy;
static int agent_manual_validation;
static int agent_validation_passed;
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
    atomic_int done;
} AgentToolJob;
static AgentToolJob agent_job;
static KryThread agent_tool_thread;
static int agent_tool_thread_running;

static void *
agent_tool_worker(void *userdata)
{
    AgentToolJob *job = userdata;

    job->result = krait_agent_run_tools(job->json);
    atomic_store(&job->done, 1);
    return NULL;
}

/* 1 when the batch needs the GL thread (offscreen render) */
static int
agent_batch_needs_gl(const char *json)
{
    KryJson *root = kry_json_parse(json);
    int needs = 0;
    for(int i = 0; root != NULL && i < kry_json_count(root); i++) {
        const char *tool = kry_json_string(kry_json_get(kry_json_at(root, i), "tool"));
        if(tool != NULL && strcmp(tool, "screenshot") == 0)
            needs = 1;
    }
    kry_json_free(root);
    return needs;
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
    atomic_store(&agent_job.done, 0);
    if(agent_job.json == NULL ||
       !KryThreadStart(&agent_tool_thread, agent_tool_worker, &agent_job)) {
        *result = NULL;
        return 1;
    }
    agent_tool_thread_running = 1;
    return 0;
}

/* 1 with *result set when the threaded batch finished */
static int
agent_poll_tools(char **result)
{
    if(!agent_tool_thread_running || !atomic_load(&agent_job.done))
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

typedef struct {
    char path[256];
    char *before;
    char *after;
    int existed;
    int review; /* 0 pending, 1 accepted, 2 reverted */
} AgentChange;
static AgentChange agent_changes[AGENT_MAX_WRITTEN];
static int agent_change_count;
static int agent_new_batch;

static KryJson *agent_checkpoint_view;
static struct dirent **agent_checkpoint_entries;
static int agent_checkpoint_count = -1;
static int agent_checkpoint_selected = -1;

static void
agent_checkpoints_reset(void)
{
    kry_json_free(agent_checkpoint_view);
    agent_checkpoint_view = NULL;
    for(int i = 0; i < agent_checkpoint_count; i++) free(agent_checkpoint_entries[i]);
    free(agent_checkpoint_entries);
    agent_checkpoint_entries = NULL;
    agent_checkpoint_count = -1;
    agent_checkpoint_selected = -1;
}

static int
agent_checkpoint_filter(const struct dirent *entry)
{
    return strncmp(entry->d_name, "batch-", 6) == 0;
}

int
krait_agent_checkpoint_count(void)
{
    char dir[KRAIT_PATH_MAX * 3];
    if(agent_busy || agent_tool_thread_running) return 0;
    if(agent_checkpoint_count >= 0) return agent_checkpoint_count;
    snprintf(dir, sizeof(dir), "%s.checkpoints", agent_history_path);
    agent_checkpoint_count = scandir(dir, &agent_checkpoint_entries, agent_checkpoint_filter, alphasort);
    if(agent_checkpoint_count < 0) agent_checkpoint_count = 0;
    return agent_checkpoint_count;
}

int
krait_agent_checkpoint_selected(void)
{
    return agent_checkpoint_selected;
}

const char *
krait_agent_checkpoint_name(void)
{
    return agent_checkpoint_selected >= 0 && agent_checkpoint_selected < agent_checkpoint_count ?
        agent_checkpoint_entries[agent_checkpoint_selected]->d_name : "Latest batch";
}

int
krait_agent_checkpoint_select(int index)
{
    char path[KRAIT_PATH_MAX * 3], *text = NULL;
    long len;
    if(agent_busy || agent_tool_thread_running || index < -1) return 0;
    if(index == -1) {
        kry_json_free(agent_checkpoint_view);
        agent_checkpoint_view = NULL;
        agent_checkpoint_selected = -1;
        return 1;
    }
    if(index >= krait_agent_checkpoint_count()) return 0;
    snprintf(path, sizeof(path), "%s.checkpoints/%s", agent_history_path,
             agent_checkpoint_entries[index]->d_name);
    if(!krait_read_file_alloc(path, &text, &len)) return 0;
    KryJson *root = kry_json_parse(text);
    free(text);
    if(kry_json_type(root) != KRY_JSON_ARRAY) { kry_json_free(root); return 0; }
    for(int i = 0; i < kry_json_count(root); i++) {
        KryJson *item = kry_json_at(root, i);
        if(kry_json_string(kry_json_get(item, "path")) == NULL ||
           kry_json_string(kry_json_get(item, "before")) == NULL ||
           kry_json_string(kry_json_get(item, "after")) == NULL) {
            kry_json_free(root); return 0;
        }
    }
    kry_json_free(agent_checkpoint_view);
    agent_checkpoint_view = root;
    agent_checkpoint_selected = index;
    return 1;
}

static int
agent_checkpoint_archive(void)
{
    char current[KRAIT_PATH_MAX * 3], dir[KRAIT_PATH_MAX * 3], path[KRAIT_PATH_MAX * 3];
    char *text = NULL;
    long len;
    if(agent_change_count == 0) return 1;
    snprintf(current, sizeof(current), "%s.changes.json", agent_history_path);
    if(!krait_read_file_alloc(current, &text, &len)) return 0;
    snprintf(dir, sizeof(dir), "%s.checkpoints", agent_history_path);
    krait_mkdir_p(dir);
    struct timeval now;
    gettimeofday(&now, NULL);
    if(snprintf(path, sizeof(path), "%s/batch-%020ld-%06ld-XXXXXX", dir,
                (long)now.tv_sec, (long)now.tv_usec) >= (int)sizeof(path)) {
        free(text); return 0;
    }
    int fd = mkstemp(path);
    if(fd < 0) { free(text); return 0; }
    close(fd);
    int ok = krait_write_text_file_atomic(path, text);
    free(text);
    if(!ok) unlink(path);
    else agent_checkpoints_reset();
    return ok;
}

static void
agent_changes_clear(void)
{
    for(int i = 0; i < agent_change_count; i++) {
        free(agent_changes[i].before);
        free(agent_changes[i].after);
    }
    memset(agent_changes, 0, sizeof(agent_changes));
    agent_change_count = 0;
}

static void
agent_changes_path(char *path, size_t size)
{
    snprintf(path, size, "%s.changes.json", agent_history_path);
}

static int
agent_changes_save(void)
{
    char path[KRAIT_PATH_MAX * 3];
    KryJsonBuf buf = {0};
    int ok;
    agent_changes_path(path, sizeof(path));
    kry_json_buf_raw(&buf, "[");
    for(int i = 0; i < agent_change_count; i++) {
        AgentChange *c = &agent_changes[i];
        if(i) kry_json_buf_raw(&buf, ",");
        kry_json_buf_raw(&buf, "{\"path\":");
        kry_json_buf_str(&buf, c->path);
        kry_json_buf_raw(&buf, ",\"before\":");
        kry_json_buf_str(&buf, c->before);
        kry_json_buf_raw(&buf, ",\"after\":");
        kry_json_buf_str(&buf, c->after);
        kry_json_buf_raw(&buf, ",\"existed\":");
        kry_json_buf_num(&buf, c->existed);
        kry_json_buf_raw(&buf, ",\"review\":");
        kry_json_buf_num(&buf, c->review);
        kry_json_buf_raw(&buf, "}");
    }
    kry_json_buf_raw(&buf, "]");
    const char *json = kry_json_buf_finish(&buf);
    ok = json != NULL && krait_write_text_file_atomic(path, json);
    kry_json_buf_free(&buf);
    return ok;
}

static void
agent_changes_load(void)
{
    char path[KRAIT_PATH_MAX * 3];
    char *text = NULL;
    long len;
    agent_changes_clear();
    agent_changes_path(path, sizeof(path));
    if(!krait_read_file_alloc(path, &text, &len))
        return;
    KryJson *root = kry_json_parse(text);
    free(text);
    if(root == NULL)
        return;
    for(int i = 0; i < kry_json_count(root) && i < AGENT_MAX_WRITTEN; i++) {
        KryJson *item = kry_json_at(root, i);
        const char *p = kry_json_string(kry_json_get(item, "path"));
        const char *before = kry_json_string(kry_json_get(item, "before"));
        const char *after = kry_json_string(kry_json_get(item, "after"));
        if(p == NULL || p[0] == '/' || strstr(p, "..") ||
           strlen(p) >= sizeof(agent_changes[0].path) || before == NULL || after == NULL)
            continue;
        AgentChange *c = &agent_changes[agent_change_count];
        c->before = strdup(before);
        c->after = strdup(after);
        if(c->before == NULL || c->after == NULL) {
            free(c->before);
            free(c->after);
            memset(c, 0, sizeof(*c));
            break;
        }
        snprintf(c->path, sizeof(c->path), "%s", p);
        c->existed = kry_json_number(kry_json_get(item, "existed")) != 0;
        c->review = (int)kry_json_number(kry_json_get(item, "review"));
        if(c->review < 0 || c->review > 2) c->review = 0;
        agent_change_count++;
    }
    kry_json_free(root);
}

static const char *const agent_system_prompt =
    "You are a coding agent working inside the Krait IDE on a Kryon "
    "project. Kry (.kry) is a Jai-like language lowered through KIR to the "
    "project's target backend, commonly C or Go. "
    "It uses explicit braces, :: declarations, C-style calls and structs, "
    "and statement order inside blocks. STRICT syntax rules: no // comments; "
    "no 'let' (declare with 'name := expr' or 'name: Type = value'). "
    "UI entry points look like: "
    "Main :: () #ui { Screen root: { ... } }. Widget calls: "
    "StyledButton(x, y, w, h, \"label\", ButtonStylePrimary, 0, NULL) "
    "returns 1 when clicked; Text(text, x, y, ScaleUIPx(16), "
    "GetThemeText()); DrawRectangleRec((Rectangle){x, y, w, h}, "
    "GetThemeButton()). Coordinates are int pixels - wrap sizes with "
    "ScaleUIPx(n), use (int) casts on floats. Read main.kry first and "
    "copy its idioms exactly; write complete file contents, never diffs. "
    "TOOLS: reply EITHER with plain text for the user OR with ONLY a "
    "JSON array of actions, executed in order: "
    "[{\"tool\":\"list\"}, {\"tool\":\"search\",\"query\":\"button\"}, "
    "{\"tool\":\"read\",\"path\":\"main.kry\"}, "
    "{\"tool\":\"write\",\"path\":\"ui.kry\",\"content\":\"...\"}, "
    "{\"tool\":\"validate\"}, {\"tool\":\"compile\"}, {\"tool\":\"run\",\"cmd\":\"make\"}, "
    "{\"tool\":\"screenshot\",\"path\":\"main.kry\"}, "
    "{\"tool\":\"card\",\"title\":\"Follow up\",\"body\":\"notes\"}, "
    "{\"tool\":\"sfs_list\",\"path\":\"/widgets\"}, "
    "{\"tool\":\"sfs_read\",\"path\":\"/widgets/0/bounds\"}, "
    "{\"tool\":\"sfs_write\",\"path\":\"/widgets/0/tap\",\"value\":\"1\"}]. "
    "validate runs the project checks defined in .krait/tasks.json and saves "
    "their command results. Use it to verify work when configured. "
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

/* Project instructions are independent of the rolling transcript budget. */
char *
krait_agent_instructions(const char *project)
{
    char paths[2][KRAIT_PATH_MAX * 2];
    char *result = calloc(1, 1);
    size_t used = 0;
    snprintf(paths[0], sizeof(paths[0]), "%s/.kryon/krait/AGENTS.md",
             getenv("HOME") != NULL ? getenv("HOME") : ".");
    snprintf(paths[1], sizeof(paths[1]), "%s/AGENTS.md", project != NULL ? project : ".");
    for(int i = 0; i < 2 && result != NULL; i++) {
        char *body = NULL, *next;
        long len;
        if(!krait_read_file_alloc(paths[i], &body, &len) || body == NULL)
            continue;
        size_t extra = strlen(body) + strlen(paths[i]) + 48;
        next = realloc(result, used + extra + 1);
        if(next == NULL) {
            free(body);
            free(result);
            return NULL;
        }
        result = next;
        snprintf(result + used, extra + 1, "\n\nProject instructions (%s):\n%s",
                 paths[i], body);
        used = strlen(result);
        free(body);
    }
    return result;
}

static void
agent_set_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(agent_status, sizeof(agent_status), fmt, ap);
    va_end(ap);
}

static void
agent_run_save(const char *state)
{
    char path[KRAIT_PATH_MAX * 3];
    snprintf(agent_run_state, sizeof(agent_run_state), "%s", state);
    if(agent_history_path[0] == 0)
        return;
    snprintf(path, sizeof(path), "%s.run", agent_history_path);
    if(!krait_write_text_file_atomic(path, state))
        agent_set_status("Could not persist run state");
}

static void
agent_run_load(void)
{
    char path[KRAIT_PATH_MAX * 3];
    char *text = NULL;
    long len;
    snprintf(agent_run_state, sizeof(agent_run_state), "idle");
    snprintf(path, sizeof(path), "%s.run", agent_history_path);
    if(krait_read_file_alloc(path, &text, &len) && text != NULL) {
        if(strcmp(text, "running") == 0 || strcmp(text, "tools") == 0 ||
           strcmp(text, "approval") == 0 || strcmp(text, "interrupted") == 0) {
            agent_run_save("interrupted");
            agent_set_status("Interrupted run: Resume checks current files before continuing");
        } else if(strcmp(text, "stopped") == 0 || strcmp(text, "failed") == 0 ||
                  strcmp(text, "review") == 0 || strcmp(text, "idle") == 0)
            snprintf(agent_run_state, sizeof(agent_run_state), "%s", text);
    }
    free(text);
}

const char *
krait_agent_run_state(void)
{
    return agent_run_state;
}

int
krait_agent_can_resume(void)
{
    return !agent_busy && (strcmp(agent_run_state, "interrupted") == 0 ||
           strcmp(agent_run_state, "stopped") == 0 || strcmp(agent_run_state, "failed") == 0);
}

int
krait_agent_resume(void)
{
    if(!krait_agent_can_resume())
        return 0;
    return krait_agent_send("Continue the interrupted task. First inspect current files and "
        "the recorded tool results: an interrupted command may already have changed files. "
        "Do not blindly replay commands. Verify remaining work before proceeding.");
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
agent_history_dir(char *dst, size_t dst_size, const char *project, const char *task)
{
    const char *home = getenv("HOME");
    char name[256];
    const char *base;
    const char *slash;

    if(home == NULL || home[0] == '\0')
        home = ".";
    base = project[0] != '\0' ? project : "none";
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
                 agent_path_hash(project));
        if(task[0] != '\0') {
            size_t used = strlen(dst);
            snprintf(dst + used, dst_size - used, "--%s", task);
        }
    }
}

static void
agent_msg_free(AgentMsg *m)
{
    free(m->text);
    free(m->tool);
    free(m->arg);
    m->text = NULL;
    m->tool = NULL;
    m->arg = NULL;
    m->kind = AGENT_MSG_USER;
    m->status = 1;
    m->dur_ms = 0;
    m->ts = 0;
}

static void
agent_append_full(int kind, const char *text, const char *tool,
                  const char *arg, int status, int dur_ms)
{
    AgentMsg *m;

    if(agent_msg_count >= AGENT_MAX_MSGS) {
        agent_msg_free(&agent_msgs[0]);
        memmove(&agent_msgs[0], &agent_msgs[1],
                sizeof(AgentMsg) * (AGENT_MAX_MSGS - 1));
        agent_msg_count = AGENT_MAX_MSGS - 1;
    }
    m = &agent_msgs[agent_msg_count++];
    m->kind = kind;
    m->text = strdup(text != NULL ? text : "");
    m->tool = tool != NULL && tool[0] != '\0' ? strdup(tool) : NULL;
    m->arg = arg != NULL && arg[0] != '\0' ? strdup(arg) : NULL;
    m->status = status;
    m->dur_ms = dur_ms;
    m->ts = time(NULL);
}

static void
agent_append(int kind, const char *text)
{
    agent_append_full(kind, text, NULL, NULL, kind == AGENT_MSG_ERROR ? 2 : 1,
                      0);
}

static void
agent_persist_msg(const AgentMsg *m)
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

        kry_json_buf_raw(&buf, "{\"v\":2,\"kind\":");
        kry_json_buf_num(&buf, m->kind);
        kry_json_buf_raw(&buf, ",\"text\":");
        kry_json_buf_str(&buf, m->text != NULL ? m->text : "");
        if(m->tool != NULL) {
            kry_json_buf_raw(&buf, ",\"tool\":");
            kry_json_buf_str(&buf, m->tool);
        }
        if(m->arg != NULL) {
            kry_json_buf_raw(&buf, ",\"arg\":");
            kry_json_buf_str(&buf, m->arg);
        }
        if(m->kind == AGENT_MSG_TOOL || m->kind == AGENT_MSG_ACTIONS) {
            kry_json_buf_raw(&buf, ",\"status\":");
            kry_json_buf_num(&buf, m->status);
            kry_json_buf_raw(&buf, ",\"dur\":");
            kry_json_buf_num(&buf, m->dur_ms);
        }
        kry_json_buf_raw(&buf, ",\"ts\":");
        kry_json_buf_num(&buf, (double)m->ts);
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
                    agent_append_full(
                        (int)kry_json_number(kry_json_get(root, "kind")),
                        msg_text,
                        kry_json_string(kry_json_get(root, "tool")),
                        kry_json_string(kry_json_get(root, "arg")),
                        (int)kry_json_number(kry_json_get(root, "status")),
                        (int)kry_json_number(kry_json_get(root, "dur")));
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
    agent_persist_msg(&agent_msgs[agent_msg_count - 1]);
}

/* UI publishes paths, never mutable editor buffers, to the tool worker. */
static atomic_flag editor_paths_lock = ATOMIC_FLAG_INIT;
static char editor_dirty_paths[32][1024];
static int editor_dirty_count;

void
krait_agent_sync_editors(const IdeState *st)
{
    while(atomic_flag_test_and_set(&editor_paths_lock)) {}
    editor_dirty_count = 0;
    if(st) for(int i = 0; i < st->open_count; i++) {
        int dirty = st->open_files[i].dirty || st->open_files[i].artifact_dirty;
        for(int j = 0; j < 4; j++) dirty |= st->open_files[i].artifact_cache_dirty[j];
        if(!dirty) continue;
        if(editor_dirty_count >= 32) { editor_dirty_count = -1; break; }
        snprintf(editor_dirty_paths[editor_dirty_count++], 1024, "%s", st->open_files[i].path);
    }
    atomic_flag_clear(&editor_paths_lock);
}

static int
agent_editor_dirty(const char *root, const char *path)
{
    char full[KRAIT_PATH_MAX * 2];
    if(!root || !path || snprintf(full, sizeof(full), "%s/%s", root, path) >= (int)sizeof(full)) return 1;
    while(atomic_flag_test_and_set(&editor_paths_lock)) {}
    int dirty = editor_dirty_count < 0;
    for(int i = 0; i < editor_dirty_count; i++)
        if(!strcmp(full, editor_dirty_paths[i])) dirty = 1;
    atomic_flag_clear(&editor_paths_lock);
    return dirty;
}

static int
project_path_safe(const char *rel)
{
    if(rel == NULL || !*rel || *rel == '/' || strlen(rel) >= 512) return 0;
    const char *part = rel;
    for(const char *p = rel;; p++) {
        if(*p == '/' || !*p) {
            size_t n = (size_t)(p - part);
            if(!n || (n == 1 && part[0] == '.') || (n == 2 && !memcmp(part, "..", 2)) ||
               (n == 4 && !memcmp(part, ".git", 4))) return 0;
            if(!*p) break;
            part = p + 1;
        }
    }
    return 1;
}

static int
agent_path_safe(const char *rel)
{
    return rel && strlen(rel) < 256 && project_path_safe(rel);
}

/* Descriptor-relative file access: no symlink component is followed. */
static int
agent_file_parent(const char *root, const char *path, int create, char leaf[512])
{
    char copy[512], *save = NULL;
    if(!project_path_safe(path)) return -1;
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(dir < 0) return -1;
    snprintf(copy, sizeof(copy), "%s", path);
    char *part = strtok_r(copy, "/", &save);
    for(;;) {
        char *next = strtok_r(NULL, "/", &save);
        if(next == NULL) { snprintf(leaf, 512, "%s", part); return dir; }
        if(create && mkdirat(dir, part, 0755) != 0 && errno != EEXIST) {
            close(dir); return -1;
        }
        int child = openat(dir, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dir);
        if(child < 0) return -1;
        struct stat repository;
        if(create && fstatat(child, ".git", &repository, AT_SYMLINK_NOFOLLOW) == 0) {
            close(child); return -1; /* Nested repositories must be edited upstream. */
        }
        dir = child;
        part = next;
    }
}

/* 1 regular text file, 0 absent leaf, -1 refused or unreadable. */
static int
agent_read_at(int parent, const char *leaf, char **text, mode_t *mode)
{
    *text = NULL;
    int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 16 * 1024 * 1024) {
        close(fd); return -1;
    }
    size_t size = (size_t)st.st_size, used = 0;
    char *data = malloc(size + 1);
    if(data == NULL) { close(fd); return -1; }
    while(used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) { free(data); close(fd); return -1; }
        used += (size_t)n;
    }
    char extra;
    ssize_t tail;
    do { tail = read(fd, &extra, 1); } while(tail < 0 && errno == EINTR);
    close(fd);
    if(tail != 0 || memchr(data, 0, size) != NULL) { free(data); return -1; }
    data[size] = 0;
    *text = data;
    if(mode) *mode = st.st_mode & 0777;
    return 1;
}

static int
agent_file_read(const char *path, char **text)
{
    char leaf[512];
    int parent = agent_file_parent(agent_project, path, 0, leaf);
    *text = NULL;
    if(parent < 0) return -1;
    int result = agent_read_at(parent, leaf, text, NULL);
    close(parent);
    return result;
}

int
krait_project_file_replace(const char *root, const char *path, const char *expected, int existed, const char *content)
{
    if(agent_editor_dirty(root, path)) return 0;
    char leaf[512], temp[128], *current = NULL;
    mode_t mode = 0644;
    int parent = agent_file_parent(root, path, content != NULL, leaf);
    if(parent < 0) return 0;
    int found = agent_read_at(parent, leaf, &current, &mode);
    if(found < 0 || found != existed || (found && strcmp(current, expected) != 0)) {
        free(current); close(parent); return 0;
    }
    free(current);
    if(content == NULL) {
        int ok = !found || unlinkat(parent, leaf, 0) == 0;
        close(parent); return ok;
    }
    static unsigned seq;
    int fd = -1;
    for(int i = 0; i < 100 && fd < 0; i++) {
        snprintf(temp, sizeof(temp), ".krait-write-%d-%u", (int)getpid(), ++seq);
        fd = openat(parent, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if(fd < 0 && errno != EEXIST) break;
    }
    if(fd < 0) { close(parent); return 0; }
    size_t size = strlen(content), used = 0;
    int ok = fchmod(fd, mode) == 0;
    while(ok && used < size) {
        ssize_t n = write(fd, content + used, size - used);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) { ok = 0; break; }
        used += (size_t)n;
    }
    if(fsync(fd) != 0) ok = 0;
    if(close(fd) != 0) ok = 0;
    current = NULL;
    found = agent_read_at(parent, leaf, &current, NULL);
    if(found < 0 || found != existed || (found && strcmp(current, expected) != 0)) ok = 0;
    free(current);
    if(agent_editor_dirty(root, path)) ok = 0;
    if(ok) ok = renameat(parent, temp, parent, leaf) == 0;
    if(!ok) unlinkat(parent, temp, 0);
    close(parent);
    return ok;
}

static int
agent_file_replace(const char *path, const char *expected, int existed, const char *content)
{
    return krait_project_file_replace(agent_project, path, expected, existed, content);
}

typedef struct { char path[256]; char digest[65]; } AgentReadVersion;
static AgentReadVersion *agent_read_versions;
static int agent_read_version_count;
static int agent_read_version_capacity;
static int agent_read_version_failed;

static void
agent_text_digest(const char *text, char digest[65])
{
    KrySha256 hash;
    unsigned char raw[32];
    kry_sha256_init(&hash);
    kry_sha256_update(&hash, text, strlen(text));
    kry_sha256_final(&hash, raw);
    kry_sha256_hex(raw, digest);
}

static void
agent_read_versions_path(char *path, size_t size)
{
    char dir[KRAIT_PATH_MAX * 2];
    agent_history_dir(dir, sizeof(dir), agent_project, agent_task);
    snprintf(path, size, "%s/reads.json", dir);
}

static int
agent_read_versions_save(void)
{
    if(agent_read_version_failed) return 0;
    KryJsonBuf json = {0};
    kry_json_buf_raw(&json, "{\"version\":1,\"files\":[");
    for(int i = 0; i < agent_read_version_count; i++) {
        if(i) kry_json_buf_raw(&json, ",");
        kry_json_buf_raw(&json, "{\"path\":"); kry_json_buf_str(&json, agent_read_versions[i].path);
        kry_json_buf_raw(&json, ",\"digest\":"); kry_json_buf_str(&json, agent_read_versions[i].digest);
        kry_json_buf_raw(&json, "}");
    }
    kry_json_buf_raw(&json, "]}");
    char path[KRAIT_PATH_MAX * 3];
    agent_read_versions_path(path, sizeof(path));
    const char *data = kry_json_buf_finish(&json);
    int ok = data && krait_write_text_file_atomic(path, data);
    kry_json_buf_free(&json);
    if(!ok) agent_read_version_failed = 1;
    return ok;
}

static void
agent_read_versions_load(void)
{
    char path[KRAIT_PATH_MAX * 3], *text = NULL;
    long length;
    agent_read_versions_path(path, sizeof(path));
    struct stat st;
    int exists = lstat(path, &st);
    if(exists != 0 && errno == ENOENT) return; /* Older sessions have no snapshot. */
    agent_read_version_failed = 1;
    if(exists != 0) return;
    if(!S_ISREG(st.st_mode) || st.st_size > 8 * 1024 * 1024 ||
       !krait_read_file_alloc(path, &text, &length)) return;
    KryJson *json = kry_json_parse(text); free(text);
    KryJson *files = kry_json_get(json, "files");
    int count = kry_json_count(files);
    if(kry_json_number(kry_json_get(json, "version")) != 1 ||
       kry_json_type(files) != KRY_JSON_ARRAY || count > 16384) { kry_json_free(json); return; }
    AgentReadVersion *versions = calloc(count ? (size_t)count : 1, sizeof(*versions));
    if(!versions) { kry_json_free(json); return; }
    int valid = 1;
    for(int i = 0; i < count && valid; i++) {
        KryJson *item = kry_json_at(files, i);
        const char *file = kry_json_string(kry_json_get(item, "path"));
        const char *digest = kry_json_string(kry_json_get(item, "digest"));
        if(!agent_path_safe(file) || !digest || (strcmp(digest, "-") &&
            (strlen(digest) != 64 || strspn(digest, "0123456789abcdef") != 64))) { valid = 0; break; }
        for(int j = 0; j < i; j++) if(!strcmp(versions[j].path, file)) valid = 0;
        snprintf(versions[i].path, sizeof(versions[i].path), "%s", file);
        snprintf(versions[i].digest, sizeof(versions[i].digest), "%s", digest);
    }
    kry_json_free(json);
    if(!valid) { free(versions); return; }
    free(agent_read_versions);
    agent_read_versions = versions;
    agent_read_version_count = count;
    agent_read_version_capacity = count ? count : 1;
    agent_read_version_failed = 0;
}

static void
agent_remember_read(const char *path, const char *text)
{
    int i;
    for(i = 0; i < agent_read_version_count; i++)
        if(!strcmp(path, agent_read_versions[i].path)) break;
    if(i >= 16384) { agent_read_version_failed = 1; return; }
    if(i == agent_read_version_capacity) {
        int capacity = agent_read_version_capacity ? agent_read_version_capacity * 2 : 64;
        AgentReadVersion *next = realloc(agent_read_versions, (size_t)capacity * sizeof(*next));
        if(next == NULL) { agent_read_version_failed = 1; return; }
        agent_read_versions = next;
        agent_read_version_capacity = capacity;
    }
    if(i == agent_read_version_count) agent_read_version_count++;
    snprintf(agent_read_versions[i].path, sizeof(agent_read_versions[i].path), "%s", path);
    if(text != NULL) agent_text_digest(text, agent_read_versions[i].digest);
    else snprintf(agent_read_versions[i].digest, sizeof(agent_read_versions[i].digest), "-");
    agent_read_versions_save();
}

static int
agent_read_is_current(const char *path, const char *text)
{
    if(agent_read_version_failed) return 0;
    for(int i = 0; i < agent_read_version_count; i++) {
        if(!strcmp(path, agent_read_versions[i].path)) {
            char digest[65];
            if(text == NULL) return !strcmp(agent_read_versions[i].digest, "-");
            agent_text_digest(text, digest);
            return !strcmp(digest, agent_read_versions[i].digest);
        }
    }
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
    int read_status = agent_file_read(path, &text);
    if(read_status != 1) {
        if(read_status == 0) agent_remember_read(path, NULL);
        snprintf(out + used, out_size - used, "[read] refused or unavailable: %s\n",
                 path);
        free(text);
        return;
    }
    agent_remember_read(path, text);
    len = (long)strlen(text);
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
    char *orig = NULL;
    int had_orig, index;
    AgentChange *change;

    if(!agent_path_safe(path) || strlen(path) >= sizeof(agent_changes[0].path) ||
       content == NULL || agent_editor_dirty(agent_project, path))
        return 0;
    char leaf[512];
    int parent = agent_file_parent(agent_project, path, 1, leaf);
    if(parent < 0) return 0;
    had_orig = agent_read_at(parent, leaf, &orig, NULL);
    close(parent);
    if(had_orig < 0 || !agent_read_is_current(path, had_orig == 1 ? orig : NULL)) {
        free(orig);
        return 0;
    }
    if(agent_new_batch) {
        if(!agent_checkpoint_archive()) { free(orig); return 0; }
        agent_changes_clear();
        agent_new_batch = 0;
    }
    for(index = 0; index < agent_change_count; index++)
        if(strcmp(agent_changes[index].path, path) == 0)
            break;
    if(index == AGENT_MAX_WRITTEN) {
        free(orig);
        return 0;
    }
    change = &agent_changes[index];
    if(index == agent_change_count) {
        change->before = strdup(had_orig ? orig : "");
        change->existed = had_orig;
        snprintf(change->path, sizeof(change->path), "%s", path);
        agent_change_count++;
    } else if(!had_orig || (change->after == NULL || strcmp(orig, change->after) != 0)) {
        free(orig);
        return 0; /* Another writer changed the file during this batch. */
    }
    char *after = strdup(content);
    if(change->before == NULL || after == NULL) {
        free(after);
        free(orig);
        return 0;
    }
    free(change->after);
    change->after = after;
    /* Persist the before/after images before modifying the project. */
    if(!agent_changes_save() || !agent_file_replace(path, orig, had_orig, content)) {
        free(orig);
        return 0;
    }
    agent_remember_read(path, content);
    agent_diff_counts(had_orig ? orig : NULL, content,
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
 * files; Revert restores the persisted before-images of the latest batch. */
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
krait_agent_change_count(void)
{
    return agent_busy || agent_tool_thread_running ? 0 :
        agent_checkpoint_view != NULL ? kry_json_count(agent_checkpoint_view) : agent_change_count;
}

const char *
krait_agent_change_path(int index)
{
    if(index < 0 || index >= krait_agent_change_count()) return "";
    if(agent_checkpoint_view != NULL)
        return kry_json_string(kry_json_get(kry_json_at(agent_checkpoint_view, index), "path"));
    return agent_changes[index].path;
}

const char *
krait_agent_change_content(int index, int after)
{
    if(index < 0 || index >= krait_agent_change_count()) return "";
    if(agent_checkpoint_view != NULL)
        return kry_json_string(kry_json_get(kry_json_at(agent_checkpoint_view, index), after ? "after" : "before"));
    return after ? agent_changes[index].after : agent_changes[index].before;
}

int
krait_agent_change_review(int index)
{
    if(index < 0 || index >= krait_agent_change_count()) return -1;
    if(agent_checkpoint_view != NULL)
        return (int)kry_json_number(kry_json_get(kry_json_at(agent_checkpoint_view, index), "review"));
    return agent_changes[index].review;
}

static int
agent_change_matches(AgentChange *c, int reverting)
{
    char *current = NULL;
    int exists = agent_file_read(c->path, &current);
    int matches = exists == 1 && (strcmp(current, c->after) == 0 ||
                             (reverting && c->existed && strcmp(current, c->before) == 0));
    free(current);
    return matches || (reverting && !c->existed && exists == 0);
}

int
krait_agent_review_change(int index, int accept)
{
    if(agent_checkpoint_view != NULL || index < 0 || index >= krait_agent_change_count() || agent_changes[index].review != 0)
        return 0;
    AgentChange *c = &agent_changes[index];
    if(!agent_change_matches(c, !accept)) {
        agent_set_status("Review stopped: %s changed after the agent write", c->path);
        return 0;
    }
    if(!accept) {
        char full[KRAIT_PATH_MAX * 2];
        snprintf(full, sizeof(full), "%s/%s", agent_project, c->path);
        char *current = NULL;
        int found = agent_file_read(c->path, &current);
        int ok = found >= 0 && agent_file_replace(c->path, current, found,
                                                c->existed ? c->before : NULL);
        free(current);
        if(ok) agent_remember_read(c->path, c->existed ? c->before : NULL);
        if(!ok) {
            agent_set_status("Could not restore %s; retry available", c->path);
            return 0;
        }
        agent_written_count = 1;
        snprintf(agent_written[0], sizeof(agent_written[0]), "%s", c->path);
        agent_files_changed = 1;
    }
    c->review = accept ? 1 : 2;
    if(!agent_changes_save()) {
        c->review = 0;
        agent_set_status("Could not save review decision; retry available");
        return 0;
    }
    agent_set_status("%s %s", accept ? "Accepted" : "Reverted", c->path);
    return 1;
}

int
krait_agent_can_revert(void)
{
    if(agent_checkpoint_view != NULL) return 0;
    for(int i = 0; i < krait_agent_change_count(); i++)
        if(agent_changes[i].review == 0) return 1;
    return 0;
}

int
krait_agent_revert(void)
{
    int restored = 0;
    char paths[AGENT_MAX_WRITTEN][256];
    if(!krait_agent_can_revert()) return 0;
    /* Check all pending files before touching any. Accepted files stay put. */
    for(int i = 0; i < agent_change_count; i++) {
        if(agent_changes[i].review == 0 && !agent_change_matches(&agent_changes[i], 1)) {
            agent_set_status("Revert stopped: %s changed after the agent write", agent_changes[i].path);
            return 0;
        }
    }
    for(int i = 0; i < agent_change_count; i++) {
        if(agent_changes[i].review != 0) continue;
        if(!krait_agent_review_change(i, 0)) break;
        snprintf(paths[restored++], sizeof(paths[0]), "%s", agent_changes[i].path);
    }
    agent_written_count = restored;
    for(int i = 0; i < restored; i++)
        snprintf(agent_written[i], sizeof(agent_written[0]), "%s", paths[i]);
    agent_files_changed = restored > 0;
    return restored;
}

static char agent_compile_errors[2048];

/* Latest agent compile diagnostics for the Problems pane (empty when the
 * last gate passed). */
const char *
krait_agent_compile_errors(void)
{
    return agent_compile_errors;
}

static void
agent_tool_compile(char *out, size_t out_size)
{
    char err[256];
    int rc = krait_compile_gate_all(agent_project, NULL, NULL, 0, err,
                                    sizeof(err), agent_compile_errors,
                                    sizeof(agent_compile_errors));
    size_t used = strlen(out);

    if(rc == 0) {
        agent_compile_errors[0] = '\0';
        snprintf(out + used, out_size - used, "[compile] ok\n");
    } else {
        used += (size_t)snprintf(out + used, out_size - used,
                                 "[compile] FAILED: %s\n",
                                 err[0] != '\0' ? err : "unknown");
        if(agent_compile_errors[0] != '\0' && used < out_size - 1)
            snprintf(out + used, out_size - used, "%s", agent_compile_errors);
    }
}

static char agent_console_pending[AGENT_RUN_OUT_CAP + 512];

/* Run-tool output queued for the Console pane; the UI drains it per
 * frame and prints it into the live terminal. */
const char *
krait_agent_consume_console(void)
{
    return agent_console_pending;
}

void
krait_agent_clear_console(void)
{
    agent_console_pending[0] = '\0';
}

static int
agent_command_cancelled(void *unused)
{
    (void)unused;
    return atomic_load(&agent_stop_requested);
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
    exit_status = krait_run_capture_cancel(agent_project, cmd, 60, buf, sizeof(buf),
                                            agent_command_cancelled, NULL);
    if(exit_status < 0) {
        snprintf(out + used, out_size - used, "[run] spawn failed: %s\n", cmd);
        return;
    }
    snprintf(out + used, out_size - used, "[run '%s'] exit %d\n%s\n", cmd,
             exit_status, buf);
    {
        size_t curlen = strlen(agent_console_pending);

        if(curlen < sizeof(agent_console_pending) - 1) {
            size_t room = sizeof(agent_console_pending) - curlen;

            curlen += (size_t)snprintf(agent_console_pending + curlen, room,
                                       "$ %s\n%s[exit %d]\n", cmd, buf,
                                       exit_status);
            if(curlen >= sizeof(agent_console_pending))
                agent_console_pending[sizeof(agent_console_pending) - 1] =
                    '\0';
        }
    }
}

/* Project checks are explicit shell commands, executed with the same
 * permissions and cancellation behavior as the run tool. */
static void
agent_tool_validate(char *out, size_t out_size)
{
    char config_path[KRAIT_PATH_MAX * 2], report_path[KRAIT_PATH_MAX * 3];
    char *config = NULL;
    char before[65], after[65], spec_before[65], spec_after[65];
    long len;
    KryJson *root = NULL, *tasks = NULL;
    KryJsonBuf report = {0};
    int count = 0, passed = 1;
    size_t used = strlen(out);
    agent_validation_passed = 0;
    snprintf(report_path, sizeof(report_path), "%s.validation.json", agent_history_path);
    if(!krait_write_text_file_atomic(report_path, "{\"version\":1,\"passed\":0,\"state\":\"incomplete\"}")) {
        snprintf(out + used, out_size - used, "[validate] FAILED: cannot create result record\n");
        return;
    }
    snprintf(config_path, sizeof(config_path), "%s/.krait/tasks.json", agent_project);
    if(krait_read_file_alloc(config_path, &config, &len) && config != NULL)
        root = kry_json_parse(config);
    tasks = root != NULL ? kry_json_get(root, "tasks") : NULL;
    count = tasks != NULL ? kry_json_count(tasks) : 0;
    if(kry_json_type(tasks) != KRY_JSON_ARRAY || count < 1 || count > 32) {
        snprintf(out + used, out_size - used,
                 "[validate] FAILED: .krait/tasks.json must define 1–32 tasks\n");
        kry_json_free(root); free(config);
        return;
    }
    /* Validate the entire configuration before executing any command. */
    for(int i = 0; i < count; i++) {
        KryJson *task = kry_json_at(tasks, i);
        const char *name = kry_json_string(kry_json_get(task, "name"));
        const char *command = kry_json_string(kry_json_get(task, "command"));
        KryJson *timeout_value = kry_json_get(task, "timeout_seconds");
        double timeout = kry_json_number(timeout_value);
        if(name == NULL || !*name || command == NULL || !*command ||
           (timeout_value != NULL && (kry_json_type(timeout_value) != KRY_JSON_NUMBER ||
            !(timeout >= 1 && timeout <= 3600) || timeout != (int)timeout))) {
            snprintf(out + used, out_size - used, "[validate] FAILED: invalid task %d\n", i + 1);
            kry_json_free(root); free(config);
            return;
        }
    }
    if(!krait_project_snapshot(agent_project, before) ||
       !krait_kanban_spec_snapshot(agent_task, spec_before)) {
        snprintf(out + used, out_size - used, "[validate] FAILED: cannot snapshot project sources\n");
        kry_json_free(root); free(config);
        return;
    }
    kry_json_buf_raw(&report, "{\"version\":1,\"project\":");
    kry_json_buf_str(&report, agent_project);
    kry_json_buf_raw(&report, ",\"task\":");
    kry_json_buf_str(&report, agent_task);
    kry_json_buf_raw(&report, ",\"started_at\":");
    kry_json_buf_num(&report, (double)time(NULL));
    kry_json_buf_raw(&report, ",\"results\":[");
    for(int i = 0; i < count; i++) {
        KryJson *task = kry_json_at(tasks, i);
        const char *name = kry_json_string(kry_json_get(task, "name"));
        const char *command = kry_json_string(kry_json_get(task, "command"));
        int timeout = (int)kry_json_number(kry_json_get(task, "timeout_seconds"));
        char output[8192];
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        int rc = krait_run_capture_cancel(agent_project, command, timeout > 0 ? timeout : 60,
                                          output, sizeof(output), agent_command_cancelled, NULL);
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                         (end.tv_nsec - start.tv_nsec) / 1000000.0;
        if(rc != 0) passed = 0;
        if(i) kry_json_buf_raw(&report, ",");
        kry_json_buf_raw(&report, "{\"name\":"); kry_json_buf_str(&report, name);
        kry_json_buf_raw(&report, ",\"command\":"); kry_json_buf_str(&report, command);
        kry_json_buf_raw(&report, ",\"exit_code\":"); kry_json_buf_num(&report, rc);
        kry_json_buf_raw(&report, ",\"duration_ms\":"); kry_json_buf_num(&report, elapsed);
        kry_json_buf_raw(&report, ",\"output\":"); kry_json_buf_str(&report, output);
        kry_json_buf_raw(&report, "}");
        used = strlen(out);
        snprintf(out + used, out_size - used, "[validate %s] %s (exit %d, %.0f ms)\n%s\n",
                 name, rc == 0 ? "passed" : "FAILED", rc, elapsed, output);
        if(atomic_load(&agent_stop_requested)) { passed = 0; break; }
    }
    if(!krait_project_snapshot(agent_project, after) || strcmp(before, after) != 0 ||
       !krait_kanban_spec_snapshot(agent_task, spec_after) || strcmp(spec_before, spec_after) != 0) {
        passed = 0;
        used = strlen(out);
        snprintf(out + used, out_size - used, "[validate] FAILED: project sources changed during validation\n");
    }
    kry_json_buf_raw(&report, "],\"source_snapshot\":"); kry_json_buf_str(&report, before);
    kry_json_buf_raw(&report, ",\"task_spec\":"); kry_json_buf_str(&report, spec_before);
    kry_json_buf_raw(&report, ",\"passed\":"); kry_json_buf_num(&report, passed);
    kry_json_buf_raw(&report, "}");
    const char *json = kry_json_buf_finish(&report);
    snprintf(report_path, sizeof(report_path), "%s.validation.json", agent_history_path);
    int saved = json != NULL && krait_write_text_file_atomic(report_path, json);
    used = strlen(out);
    snprintf(out + used, out_size - used, "[validate] %s%s\n",
             passed && saved ? "passed" : "FAILED", saved ? " (report saved)" : " (could not save report)");
    agent_validation_passed = passed && saved;
    kry_json_buf_free(&report); kry_json_free(root); free(config);
}

int
krait_agent_validation_for(const char *bound_project, const char *bound_task)
{
    char path[KRAIT_PATH_MAX * 3], snapshot[65];
    char *text = NULL;
    long len;
    int valid = 0;
    char task_spec[65];
    char dir[KRAIT_PATH_MAX * 2];
    if(agent_busy || bound_project == NULL || bound_task == NULL || bound_project[0] == 0 ||
       strchr(bound_task, '/') != NULL || strstr(bound_task, "..") != NULL)
        return 0;
    agent_history_dir(dir, sizeof(dir), bound_project, bound_task);
    snprintf(path, sizeof(path), "%s/history.jsonl.validation.json", dir);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    KryJson *root = kry_json_parse(text);
    free(text);
    const char *saved_spec = kry_json_string(kry_json_get(root, "task_spec"));
    const char *saved = kry_json_string(kry_json_get(root, "source_snapshot"));
    const char *project = kry_json_string(kry_json_get(root, "project"));
    const char *task = kry_json_string(kry_json_get(root, "task"));
    if(saved != NULL && saved_spec != NULL && project != NULL && task != NULL &&
       krait_kanban_spec_snapshot(bound_task, task_spec) && strcmp(saved_spec, task_spec) == 0 &&
       strcmp(project, bound_project) == 0 && strcmp(task, bound_task) == 0 &&
       kry_json_number(kry_json_get(root, "passed")) == 1 &&
       krait_project_snapshot(bound_project, snapshot) && strcmp(snapshot, saved) == 0)
        valid = 1;
    kry_json_free(root);
    return valid;
}

int
krait_agent_validation_current(void)
{
    return krait_agent_validation_for(agent_project, agent_task);
}

static KryJson *validation_view;
static char validation_view_status[160];

int
krait_agent_checks_load(void)
{
    kry_json_free(validation_view); validation_view = NULL;
    snprintf(validation_view_status, sizeof(validation_view_status), "No saved validation report");
    if(agent_busy || agent_tool_thread_running) {
        snprintf(validation_view_status, sizeof(validation_view_status), "Wait for the active run to stop, then refresh"); return 0;
    }
    char path[KRAIT_PATH_MAX * 3], *text = NULL;
    long len;
    snprintf(path, sizeof(path), "%s.validation.json", agent_history_path);
    struct stat st;
    if(stat(path, &st) != 0) return 0;
    if(st.st_size > 1024 * 1024 || !krait_read_file_alloc(path, &text, &len)) {
        snprintf(validation_view_status, sizeof(validation_view_status), "Cannot read validation report"); return 0;
    }
    KryJson *root = kry_json_parse(text); free(text);
    KryJson *checks = kry_json_get(root, "results");
    int count = kry_json_count(checks);
    if(kry_json_number(kry_json_get(root, "version")) != 1 ||
       kry_json_type(checks) != KRY_JSON_ARRAY || count > 32) {
        kry_json_free(root);
        snprintf(validation_view_status, sizeof(validation_view_status), "Incomplete or invalid report; inspect the conversation and validate again"); return 0;
    }
    for(int i = 0; i < count; i++) {
        KryJson *item = kry_json_at(checks, i);
        if(!kry_json_string(kry_json_get(item, "name")) || !kry_json_string(kry_json_get(item, "command")) ||
           !kry_json_string(kry_json_get(item, "output")) ||
           kry_json_type(kry_json_get(item, "exit_code")) != KRY_JSON_NUMBER ||
           kry_json_type(kry_json_get(item, "duration_ms")) != KRY_JSON_NUMBER) {
            kry_json_free(root); snprintf(validation_view_status, sizeof(validation_view_status), "Invalid check record"); return 0;
        }
    }
    validation_view = root;
    int passed = kry_json_number(kry_json_get(root, "passed")) == 1;
    snprintf(validation_view_status, sizeof(validation_view_status), "%s — %d recorded checks",
        passed ? (krait_agent_validation_current() ? "Passed; evidence is current" : "Passed; evidence is stale") : "Validation failed", count);
    return count;
}

const char *krait_agent_checks_status(void) { return validation_view_status; }
int krait_agent_checks_count(void) { return kry_json_count(kry_json_get(validation_view, "results")); }
const char *krait_agent_check_text(int index, int field)
{
    const char *keys[] = {"name", "command", "output"};
    if(index < 0 || index >= krait_agent_checks_count() || field < 0 || field > 2) return "";
    const char *value = kry_json_string(kry_json_get(kry_json_at(kry_json_get(validation_view, "results"), index), keys[field]));
    return value ? value : "";
}
int krait_agent_check_number(int index, int duration)
{
    if(index < 0 || index >= krait_agent_checks_count()) return 0;
    return (int)kry_json_number(kry_json_get(kry_json_at(kry_json_get(validation_view, "results"), index), duration ? "duration_ms" : "exit_code"));
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

/* ---- per-call capture inside krait_agent_run_tools ---- */
static struct timeval agent_rec_tv;

static void
agent_rec_begin(const char *tool, const char *a1, const char *a2)
{
    AgentToolRec *r;

    if(agent_rec_count >= AGENT_MAX_RECS)
        return;
    r = &agent_recs[agent_rec_count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->tool, sizeof(r->tool), "%s", tool != NULL ? tool : "?");
    {
        const char *a = a1 != NULL && a1[0] != '\0' ? a1 : a2;

        snprintf(r->arg, sizeof(r->arg), "%s", a != NULL ? a : "");
    }
    gettimeofday(&agent_rec_tv, NULL);
}

static void
agent_rec_end(const char *out, size_t before, size_t after)
{
    AgentToolRec *r;
    struct timeval tv;
    size_t len;

    if(agent_rec_count <= 0)
        return;
    r = &agent_recs[agent_rec_count - 1];
    gettimeofday(&tv, NULL);
    r->dur_ms = (int)((tv.tv_sec - agent_rec_tv.tv_sec) * 1000 +
                      (tv.tv_usec - agent_rec_tv.tv_usec) / 1000);
    len = after > before ? after - before : 0;
    if(len > sizeof(r->result) - 1)
        len = sizeof(r->result) - 1;
    memcpy(r->result, out + before, len);
    r->result[len] = '\0';
    krait_trim(r->result);
    r->status = (strstr(r->result, "refused") != NULL ||
                 strstr(r->result, "failed") != NULL ||
                 strstr(r->result, "FAILED") != NULL ||
                 strstr(r->result, "not found") != NULL) ? 2 : 1;
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
    agent_rec_count = 0;
    if(root == NULL) {
        snprintf(out, AGENT_TOOL_OUT_CAP, "TOOL RESULTS: bad action json\n");
        return out;
    }
    agent_new_batch = 1;
    count = kry_json_count(root);
    for(i = 0; i < count; i++) {
        KryJson *action = kry_json_at(root, i);
        const char *tool = kry_json_string(kry_json_get(action, "tool"));
        size_t before;

        if(atomic_load(&agent_stop_requested))
            break;
        if(tool == NULL)
            continue;
        agent_rec_begin(tool,
                        kry_json_string(kry_json_get(action, "path")),
                        kry_json_string(kry_json_get(action, "query")) != NULL ?
                        kry_json_string(kry_json_get(action, "query")) :
                        kry_json_string(kry_json_get(action, "cmd")) != NULL ?
                        kry_json_string(kry_json_get(action, "cmd")) :
                        kry_json_string(kry_json_get(action, "title")));
        before = strlen(out);
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
                         "[write] refused: %s%s\n",
                         path != NULL ? path : "",
                         agent_editor_dirty(agent_project, path) ? " (unsaved editor changes; save or close the file first)" :
                         agent_read_version_failed ? " (file-read snapshot unavailable; restore reads.json or use a new session)" : "");
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
        else if(strcmp(tool, "validate") == 0)
            agent_tool_validate(out, AGENT_TOOL_OUT_CAP);
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
        agent_rec_end(out, before, strlen(out));
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

/* Some providers occasionally prepend a short sentence before the JSON tool
 * batch. Keep the prompt strict, but recover by extracting the first valid
 * tool array so the agent loop still makes progress. */
static int
agent_extract_action_array(char *text)
{
    char *start;
    char *end;

    if(text == NULL || agent_reply_is_actions(text))
        return text != NULL;
    for(start = strchr(text, '['); start != NULL; start = strchr(start + 1, '[')) {
        for(end = strrchr(start, ']'); end != NULL && end > start;) {
            char saved = end[1];
            char *prev;

            end[1] = '\0';
            if(agent_reply_is_actions(start)) {
                if(start != text)
                    memmove(text, start, strlen(start) + 1);
                return 1;
            }
            end[1] = saved;
            prev = end - 1;
            while(prev > start && *prev != ']')
                prev--;
            end = prev > start ? prev : NULL;
        }
    }
    return 0;
}

/* Strip one markdown fence pair and surrounding whitespace, in place. */
static char *
agent_strip_reply(char *text)
{
    char *json = text;
    size_t n;

    if(text == NULL)
        return NULL;
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
    if(json != text)
        memmove(text, json, strlen(json) + 1);
    return text;
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
    char *instructions = krait_agent_instructions(agent_project);
    char *system = NULL;
    if(instructions != NULL && instructions[0] != '\0') {
        size_t len = strlen(agent_system_prompt) + strlen(instructions) + 1;
        system = malloc(len);
        if(system != NULL) {
            snprintf(system, len, "%s%s", agent_system_prompt, instructions);
            temps[(*temp_count)++] = system;
        }
    }
    free(instructions);
    msgs[count].content = system != NULL ? system : agent_system_prompt;
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

/* ---- per-call transcript rows from the latest batch ---- */
static void
agent_remember_tool_results(char *blob)
{
    int i;

    if(blob == NULL)
        return;
    if(agent_rec_count <= 0) {
        agent_remember(AGENT_MSG_TOOL, blob);
        free(blob);
        return;
    }
    for(i = 0; i < agent_rec_count; i++) {
        AgentToolRec *r = &agent_recs[i];
        char *head;
        size_t need = strlen(r->tool) + strlen(r->arg) + strlen(r->result) + 16;

        head = malloc(need);
        if(head == NULL)
            continue;
        snprintf(head, need, "[%s %s] %s", r->tool,
                 r->arg[0] != '\0' ? r->arg : "-", r->result);
        agent_append_full(AGENT_MSG_TOOL, head, r->tool, r->arg, r->status,
                          r->dur_ms);
        free(head);
        agent_persist_msg(&agent_msgs[agent_msg_count - 1]);
    }
    free(blob);
}

/* ---- permission gate: park a tool batch until the user approves ---- */
static char *agent_perm_json;
static char agent_perm_lines[AGENT_MAX_RECS][224];
static int agent_perm_count;
static int agent_perm_always;

int
krait_agent_permission_pending(void)
{
    return agent_perm_json != NULL;
}

int
krait_agent_permission_count(void)
{
    return agent_perm_count;
}

const char *
krait_agent_permission_line(int index)
{
    if(index < 0 || index >= agent_perm_count)
        return "";
    return agent_perm_lines[index];
}

static void
agent_build_perm_lines(const char *json)
{
    KryJson *root = kry_json_parse(json);
    int i;

    agent_perm_count = 0;
    if(root == NULL)
        return;
    for(i = 0; i < kry_json_count(root) && agent_perm_count < AGENT_MAX_RECS;
        i++) {
        KryJson *a = kry_json_at(root, i);
        const char *tool = kry_json_string(kry_json_get(a, "tool"));
        const char *detail;

        if(tool == NULL)
            continue;
        detail = kry_json_string(kry_json_get(a, "path"));
        if(detail == NULL || detail[0] == '\0')
            detail = kry_json_string(kry_json_get(a, "cmd"));
        if(detail == NULL || detail[0] == '\0')
            detail = kry_json_string(kry_json_get(a, "query"));
        snprintf(agent_perm_lines[agent_perm_count++],
                 sizeof(agent_perm_lines[0]), "%s %s", tool,
                 detail != NULL ? detail : "");
    }
    kry_json_free(root);
}

/* Forward: shared execution path used by the poll loop and the
 * permission approve button. Consumes (frees) the reply json. */
static void
agent_execute_actions(char *reply);

void
krait_agent_permission_respond(int allow, int always)
{
    char *json = agent_perm_json;

    if(json == NULL)
        return;
    agent_perm_json = NULL;
    agent_perm_count = 0;
    if(allow) {
        if(always)
            agent_perm_always = 1;
        agent_execute_actions(json);
        return;
    }
    free(json);
    agent_busy = 0;
    agent_run_save("stopped");
    agent_set_status("tools denied");
    agent_remember(AGENT_MSG_ERROR, "tool batch denied by user");
}

int
krait_agent_full_access_enabled(void)
{
    return agent_perm_always;
}

void
krait_agent_set_full_access(int enabled)
{
    agent_perm_always = enabled != 0 ? 1 : 0;
}

/* ---- retry: replay a user turn from the transcript ---- */
int
krait_agent_retry(int index)
{
    char *text;
    int i;

    if(agent_busy || index < 0 || index >= agent_msg_count)
        return 0;
    if(agent_msgs[index].kind != AGENT_MSG_USER)
        return 0;
    text = strdup(agent_msgs[index].text != NULL ? agent_msgs[index].text : "");
    for(i = index; i < agent_msg_count; i++)
        agent_msg_free(&agent_msgs[i]);
    agent_msg_count = index;
    {
        int rc = krait_agent_send(text);

        free(text);
        return rc;
    }
}

/* ---- session picker: histories under ~/.kryon/krait/agent ---- */
#define AGENT_MAX_SESSIONS 32
typedef struct {
    char dir_name[256];
    char project[KRAIT_PATH_MAX];
    char task[128];
    long mtime;
} AgentSession;
static AgentSession agent_sessions[AGENT_MAX_SESSIONS];
static int agent_session_count;
static long agent_session_scan;

static void
agent_sessions_scan(void)
{
    char root[KRAIT_PATH_MAX * 2];
    DIR *d;
    struct dirent *e;

    if(time(NULL) - agent_session_scan < 2)
        return;
    agent_session_scan = time(NULL);
    agent_session_count = 0;
    snprintf(root, sizeof(root), "%s/.kryon/krait/agent",
             getenv("HOME") != NULL ? getenv("HOME") : ".");
    d = opendir(root);
    if(d == NULL)
        return;
    while((e = readdir(d)) != NULL &&
          agent_session_count < AGENT_MAX_SESSIONS) {
        char path[KRAIT_PATH_MAX * 2];
        char proj[KRAIT_PATH_MAX];
        struct stat st;
        AgentSession *s;

        if(e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s/history.jsonl", root, e->d_name);
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        proj[0] = '\0';
        snprintf(path, sizeof(path), "%s/%s/project.txt", root, e->d_name);
        {
            char *data = NULL;
            long len;

            if(krait_read_file_alloc(path, &data, &len) && data != NULL) {
                krait_trim(data);
                snprintf(proj, sizeof(proj), "%s", data);
                free(data);
            }
        }
        s = &agent_sessions[agent_session_count++];
        snprintf(s->dir_name, sizeof(s->dir_name), "%s", e->d_name);
        snprintf(s->project, sizeof(s->project), "%s", proj);
        s->task[0] = '\0';
        snprintf(path, sizeof(path), "%s/%s/task.txt", root, e->d_name);
        {
            char *data = NULL;
            long len;
            if(krait_read_file_alloc(path, &data, &len) && data != NULL)
                snprintf(s->task, sizeof(s->task), "%s", krait_trim(data));
            free(data);
        }
        s->mtime = (long)st.st_mtime;
    }
    closedir(d);
}

int
krait_agent_session_count(void)
{
    agent_sessions_scan();
    return agent_session_count;
}

const char *
krait_agent_session_name(int index)
{
    if(index < 0 || index >= agent_session_count)
        return "";
    return agent_sessions[index].dir_name;
}

const char *
krait_agent_session_project(int index)
{
    if(index < 0 || index >= agent_session_count)
        return "";
    return agent_sessions[index].project;
}

long
krait_agent_session_mtime(int index)
{
    if(index < 0 || index >= agent_session_count)
        return 0;
    return agent_sessions[index].mtime;
}

int
krait_agent_open_session(int index)
{
    const char *proj;

    if(index < 0 || index >= agent_session_count)
        return 0;
    proj = agent_sessions[index].project;
    if(proj == NULL || proj[0] == '\0' || !krait_path_exists(proj))
        return 0;
    return agent_bind_session(proj, agent_sessions[index].task);
}

static void
agent_start_request(void)
{
    KraitAiMessage msgs[AGENT_MAX_MSGS + 2] = {0};
    char *temps[AGENT_MAX_MSGS + 2] = {0};
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
        agent_run_save("failed");
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
    agent_run_save("review");
    agent_round = 0;
    agent_set_status("%s", usage[0] != '\0' ? usage : "");
    if(text != NULL && text[0] != '\0')
        agent_remember(AGENT_MSG_ASSISTANT, text);
}

static int
agent_bind_session(const char *project_dir, const char *task)
{
    char dir[KRAIT_PATH_MAX * 2];
    int i;

    if(project_dir == NULL)
        project_dir = "";
    if(task == NULL)
        task = "";
    if(strlen(task) >= sizeof(agent_task))
        return 0;
    for(const char *p = task; *p; p++)
        if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    if(strcmp(agent_project, project_dir) == 0 && strcmp(agent_task, task) == 0)
        return 1;
    if(agent_busy || agent_tool_thread_running)
        return 0;
    free(agent_perm_json);
    agent_perm_json = NULL;
    agent_perm_count = 0;
    agent_perm_always = 0;
    agent_checkpoints_reset();
    agent_written_count = 0;
    kry_json_free(validation_view); validation_view = NULL;
    snprintf(validation_view_status, sizeof(validation_view_status), "Session changed; refresh to load its checks");
    agent_read_version_count = 0;
    agent_read_version_failed = 0;
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
    snprintf(agent_task, sizeof(agent_task), "%s", task);
    agent_history_dir(dir, sizeof(dir), agent_project, agent_task);
    snprintf(agent_history_path, sizeof(agent_history_path), "%s/history.jsonl",
             dir);
    krait_mkdir_p(dir);
    /* session picker reads this to map a history dir back to its project */
    {
        char proj_path[KRAIT_PATH_MAX * 2];

        snprintf(proj_path, sizeof(proj_path), "%s/project.txt", dir);
        if(!krait_path_exists(proj_path) && agent_project[0] != '\0')
            krait_write_text_file(proj_path, agent_project);
    }
    if(getenv("KRAIT_AGENT_DEBUG") != NULL)
        fprintf(stderr, "agent-bind: project='%s' history='%s'\n",
                agent_project, agent_history_path);
    {
        char task_path[KRAIT_PATH_MAX * 2];
        snprintf(task_path, sizeof(task_path), "%s/task.txt", dir);
        krait_write_text_file_atomic(task_path, agent_task);
    }
    agent_load_history();
    agent_changes_load();
    agent_run_load();
    agent_read_versions_load();
    if(agent_read_version_failed) agent_set_status("File-read snapshot unavailable: file writes disabled for this session");
    agent_session_scan = 0;
    return 1;
}

void
krait_agent_bind(const char *project_dir)
{
    /* Drawing the same project must not replace its selected task session. */
    if(project_dir != NULL && strcmp(agent_project, project_dir) == 0)
        return;
    agent_bind_session(project_dir, "");
}

int
krait_agent_bind_task(const char *project_dir, const char *task)
{
    return agent_bind_session(project_dir, task);
}

const char *
krait_agent_task(void)
{
    return agent_task;
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

/* Live partial reply while the request streams; empty when nothing yet. */
const char *
krait_agent_streaming_text(void)
{
    static char partial[16384];
    char *text;

    if(!agent_busy || agent_req == NULL)
        return "";
    text = krait_ai_stream_text(agent_req);
    if(text == NULL) {
        partial[0] = '\0';
        return partial;
    }
    snprintf(partial, sizeof(partial), "%s", text);
    free(text);
    return partial;
}

const char *
krait_agent_tool_name(int index)
{
    if(index < 0 || index >= agent_msg_count)
        return "";
    return agent_msgs[index].tool != NULL ? agent_msgs[index].tool : "";
}

const char *
krait_agent_tool_arg(int index)
{
    if(index < 0 || index >= agent_msg_count)
        return "";
    return agent_msgs[index].arg != NULL ? agent_msgs[index].arg : "";
}

int
krait_agent_tool_status(int index)
{
    if(index < 0 || index >= agent_msg_count)
        return 1;
    return agent_msgs[index].status;
}

int
krait_agent_tool_dur(int index)
{
    if(index < 0 || index >= agent_msg_count)
        return 0;
    return agent_msgs[index].dur_ms;
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
    const char *title, *body, *project, *id;
    char *prompt;
    size_t size;
    int result;

    if(!krait_ai_configured() || agent_busy)
        return 0;
    title = krait_kanban_card_title(col, index);
    body = krait_kanban_card_body(col, index);
    project = krait_kanban_card_project(col, index);
    id = krait_kanban_card_id(col, index);
    if(title == NULL || title[0] == '\0' || id[0] == '\0')
        return 0;
    if(!agent_bind_session(project, id))
        return 0;
    const char *criteria = krait_kanban_field(col, index, 3);
    size = strlen(title) + strlen(body) + strlen(criteria) + 80;
    prompt = malloc(size);
    if(prompt == NULL)
        return 0;
    snprintf(prompt, size, "Kanban card: %s\n\n%s\n\nAcceptance criteria:\n%s", title, body, criteria);
    result = krait_agent_send(prompt);
    free(prompt);
    return result;
}

int
krait_agent_send(const char *text)
{
    if(text == NULL || text[0] == '\0')
        return 0;
    if(agent_busy)
        return 0;
    if(!krait_ai_configured()) {
        agent_remember(AGENT_MSG_ERROR, krait_ai_config_hint());
        return 0;
    }
    agent_checkpoints_reset();
    agent_manual_validation = 0;
    agent_remember(AGENT_MSG_USER, text);
    agent_run_save("running");
    agent_http_retries = 0;
    agent_busy = 1;
    agent_round = 0;
    agent_stop_requested = 0;
    agent_start_request();
    return 1;
}

void
krait_agent_stop(void)
{
    if(!agent_busy)
        return;
    atomic_store(&agent_stop_requested, 1);
    agent_run_save("stopped");
    free(agent_perm_json);
    agent_perm_json = NULL;
    agent_perm_count = 0;
    if(agent_req != NULL) {
        krait_ai_free(agent_req);
        agent_req = NULL;
    }
    if(!agent_tool_thread_running) {
        agent_busy = 0;
        agent_set_status("stopped");
    } else {
        agent_set_status("Stopping active command; waiting for tool cleanup");
    }
}

void
krait_agent_clear(void)
{
    int i;

    if(agent_busy || agent_tool_thread_running)
        return;

    if(agent_req != NULL) {
        krait_ai_free(agent_req);
        agent_req = NULL;
    }
    free(agent_perm_json);
    agent_perm_json = NULL;
    agent_perm_count = 0;
    agent_perm_always = 0;
    for(i = 0; i < agent_msg_count; i++)
        agent_msg_free(&agent_msgs[i]);
    agent_msg_count = 0;
    agent_busy = 0;
    agent_round = 0;
    agent_set_status("");
    agent_run_save("idle");
    if(agent_history_path[0] != '\0')
        remove(agent_history_path);
}

#define AGENT_MAX_HTTP_RETRIES 2

/* Execute an approved tool-action reply: remember it, run the batch
 * (threaded or sync), then feed the next request. Consumes reply. */
static void
agent_execute_actions(char *reply)
{
    char *results;

    agent_round++;
    agent_run_save("tools");
    agent_remember(AGENT_MSG_ACTIONS, reply);
    agent_set_status("running tools (round %d/%d)...", agent_round,
                     AGENT_MAX_ROUNDS);
    if(agent_start_tools(reply, &results)) {
        free(reply);
        if(results == NULL) {
            agent_busy = 0;
            agent_run_save("failed");
            agent_set_status("tool failure");
            return;
        }
        agent_remember_tool_results(results);
        if(agent_round >= AGENT_MAX_ROUNDS) {
            agent_busy = 0;
            agent_run_save("failed");
            agent_set_status("stopped: too many tool rounds");
            agent_remember(AGENT_MSG_ERROR, "stopped: too many tool rounds");
            return;
        }
        agent_start_request();
        return;
    }
    free(reply);   /* threaded batch: poll picks up the result */
}

int
krait_agent_validate(void)
{
    char *result = NULL;
    if(agent_busy || agent_tool_thread_running || agent_project[0] == 0)
        return 0;
    atomic_store(&agent_stop_requested, 0);
    agent_manual_validation = 1;
    agent_busy = 1;
    agent_run_save("tools");
    agent_set_status("Running project validation...");
    if(agent_start_tools("[{\"tool\":\"validate\"}]", &result)) {
        agent_busy = 0;
        agent_manual_validation = 0;
        free(result);
        agent_run_save("failed");
        agent_set_status("Could not start validation worker");
        return 0;
    }
    return 1;
}

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
            agent_run_save("failed");
            agent_set_status("tool failure");
            return 0;
        }
        agent_remember_tool_results(results);
        if(atomic_load(&agent_stop_requested)) {
            agent_busy = 0;
            agent_run_save("stopped");
            agent_set_status("stopped");
            return 0;
        }
        if(agent_manual_validation) {
            agent_manual_validation = 0;
            agent_busy = 0;
            agent_run_save(agent_validation_passed ? "review" : "failed");
            agent_set_status(agent_validation_passed ? "Project validation passed" : "Project validation failed; inspect tool results");
            return 0;
        }
        if(agent_round >= AGENT_MAX_ROUNDS) {
            agent_busy = 0;
            agent_run_save("failed");
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
        char err[512];
        snprintf(err, sizeof(err), "%s", krait_ai_error(agent_req) != NULL ? krait_ai_error(agent_req) : "request failed");

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
        agent_run_save("failed");
        agent_set_status("request failed");
        agent_remember(AGENT_MSG_ERROR, err);
        return 0;
    }
    agent_http_retries = 0;
    reply = agent_strip_reply(strdup(krait_ai_text(agent_req) != NULL ?
                                     krait_ai_text(agent_req) : ""));
    krait_ai_free(agent_req);
    agent_req = NULL;
    if(reply == NULL) {
        agent_busy = 0;
        agent_run_save("failed");
        agent_set_status("out of memory");
        agent_remember(AGENT_MSG_ERROR, "out of memory");
        return 0;
    }
    if(agent_stop_requested) {
        agent_busy = 0;
        agent_set_status("stopped");
        agent_remember(AGENT_MSG_ASSISTANT,
                       reply[0] != '\0' ? reply : "(stopped)");
        free(reply);
        return 0;
    }
    if(agent_extract_action_array(reply)) {
        if(!agent_perm_always) {
            free(agent_perm_json);
            agent_build_perm_lines(reply);
            agent_perm_json = reply;
            agent_run_save("approval");
            agent_set_status("approve tools? (round %d/%d)", agent_round + 1,
                             AGENT_MAX_ROUNDS);
            return 1;
        }
        agent_execute_actions(reply);
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

    if(agent_tool_thread_running) {
        KryThreadJoin(&agent_tool_thread);
        agent_tool_thread_running = 0;
    }
    free(agent_job.json);
    free(agent_job.result);
    memset(&agent_job, 0, sizeof(agent_job));
    agent_changes_clear();
    agent_checkpoints_reset();
    kry_json_free(validation_view); validation_view = NULL;
    free(agent_read_versions);
    agent_read_versions = NULL;
    agent_read_version_count = agent_read_version_capacity = 0;

    if(agent_req != NULL) {
        krait_ai_free(agent_req);
        agent_req = NULL;
    }
    free(agent_perm_json);
    agent_perm_json = NULL;
    for(i = 0; i < agent_msg_count; i++)
        agent_msg_free(&agent_msgs[i]);
    agent_msg_count = 0;
}
