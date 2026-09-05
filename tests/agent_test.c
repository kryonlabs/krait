/*
 * agent_test.c - agent engine + shared compile gate.
 *
 * Hermetic: history persist/load round-trip against a redirected HOME,
 * compile-gate verdicts on a scaffold project (clean and deliberately
 * broken overlays), and the bounded command runner. A live provider round
 * trip with real tool use runs only when KRAIT_AGENT_LIVE=1 and the selected
 * provider has an API key.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <glob.h>

static int failures;

#define CHECK(cond) do { \
    if(!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while(0)

static int
config_child_check(void)
{
    return krait_ai_active_provider() == 2 &&
           krait_ai_active_effort() == 1 &&
           krait_ai_provider_configured(2) == 1 &&
           strcmp(krait_ai_provider_model(2), "claude-test-model") == 0 &&
           strcmp(krait_ai_provider_base_url(2),
                  "https://claude.example/v1") == 0 ? 0 : 1;
}

static void
test_history_roundtrip(void)
{
    krait_agent_bind("/tmp/krait-agent-test-proj");
    if(krait_ai_configured()) {
        printf("key configured; skipping no-key send path\n");
        return;
    }
    CHECK(krait_agent_send("hello agent") == 0);
    CHECK(krait_agent_count() == 1);
    CHECK(krait_agent_kind(0) == 3);   /* error: AI off */

    /* rebind through a different project, then back: forces a reload */
    krait_agent_bind("/tmp/krait-agent-test-proj-other");
    CHECK(krait_agent_count() == 0);
    krait_agent_bind("/tmp/krait-agent-test-proj");
    CHECK(krait_agent_count() == 1);
    CHECK(krait_agent_kind(0) == 3);
    CHECK(strstr(krait_agent_text(0), "OPENAI_API_KEY") != NULL);

    krait_agent_clear();
    CHECK(krait_agent_count() == 0);
    krait_agent_bind("/tmp/krait-agent-test-proj");
    CHECK(krait_agent_count() == 0);   /* clear wiped the history file */
}

static void
test_task_sessions(void)
{
    const char *project = "/tmp/krait-agent-test-proj";
    krait_mkdir_p(project);
    if(krait_ai_configured())
        return;
    CHECK(krait_agent_bind_task(project, "card-first"));
    CHECK(strcmp(krait_agent_task(), "card-first") == 0);
    CHECK(krait_agent_count() == 0);
    CHECK(krait_agent_send("first task") == 0);
    CHECK(krait_agent_count() == 1);
    CHECK(krait_agent_bind_task(project, "card-second"));
    CHECK(krait_agent_count() == 0);
    CHECK(krait_agent_send("second task") == 0);
    CHECK(krait_agent_bind_task(project, "card-first"));
    CHECK(krait_agent_count() == 1);
    krait_agent_bind(project);
    CHECK(strcmp(krait_agent_task(), "card-first") == 0);
    CHECK(!krait_agent_bind_task(project, "../escape"));
    CHECK(strcmp(krait_agent_task(), "card-first") == 0);
    CHECK(krait_agent_bind_task(project, ""));
    CHECK(krait_agent_count() == 0);
    CHECK(krait_agent_session_count() >= 2);
    for(int i = 0; i < krait_agent_session_count(); i++) {
        if(strstr(krait_agent_session_name(i), "--card-second") != NULL) {
            CHECK(krait_agent_open_session(i));
            CHECK(strcmp(krait_agent_task(), "card-second") == 0);
            CHECK(krait_agent_count() == 1);
            break;
        }
    }
}

static void
test_interrupted_run_recovery(void)
{
    const char *project = "/tmp/krait-agent-run-recovery";
    char path[2048];
    unsigned hash = 2166136261u;
    for(const char *p = project; *p; p++)
        hash = (hash ^ (unsigned char)*p) * 16777619u;
    krait_mkdir_p(project);
    CHECK(krait_agent_bind_task(project, "restart"));
    CHECK(!krait_agent_can_resume());
    snprintf(path, sizeof(path), "%s/.kryon/krait/agent/krait-agent-run-recovery-%08x--restart/history.jsonl.run",
             getenv("HOME"), hash);
    const char *states[] = {"running", "tools", "approval"};
    for(int i = 0; i < 3; i++) {
        CHECK(krait_agent_bind_task(project, "other"));
        CHECK(krait_write_text_file_atomic(path, states[i]));
        CHECK(krait_agent_bind_task(project, "restart"));
        CHECK(strcmp(krait_agent_run_state(), "interrupted") == 0);
        CHECK(krait_agent_can_resume());
        CHECK(!krait_agent_busy());
        CHECK(!krait_agent_permission_pending());
    }
    krait_agent_clear();
    CHECK(strcmp(krait_agent_run_state(), "idle") == 0);
    CHECK(!krait_agent_can_resume());
    CHECK(krait_agent_bind_task(project, "other"));
    CHECK(krait_agent_bind_task(project, "restart"));
    CHECK(strcmp(krait_agent_run_state(), "idle") == 0);
}

static void
test_compile_gate(void)
{
    char status[256];
    char err[256];
    const char *paths[1];
    const char *bodies[1];

    CHECK(krait_scaffold_project("/tmp/krait-agent-test-proj", status,
                                 sizeof(status)) == 1);
    err[0] = '\0';
    CHECK(krait_compile_gate_all("/tmp/krait-agent-test-proj", NULL, NULL, 0,
                                 err, sizeof(err), NULL, 0) == 0);

    paths[0] = "broken.kry";
    bodies[0] = "Bad :: () #ui {\n    Screen root: {\n    let x := 5\n    }\n}\n";
    err[0] = '\0';
    char all_errs[1024] = {0};

    CHECK(krait_compile_gate_all("/tmp/krait-agent-test-proj", paths, bodies,
                                 1, err, sizeof(err), all_errs,
                                 sizeof(all_errs)) == 1);
    CHECK(err[0] != '\0');
    CHECK(strstr(all_errs, "error") != NULL);   /* diagnostics captured */
}

static void
test_run_capture(void)
{
    char out[256];

    out[0] = '\0';
    CHECK(krait_run_capture("/tmp", "echo hello-agent", 5, out,
                            sizeof(out)) == 0);
    CHECK(strstr(out, "hello-agent") != NULL);
    CHECK(krait_run_capture("/tmp", "exit 3", 5, out, sizeof(out)) == 3);
    CHECK(krait_run_capture("/tmp", "", 5, out, sizeof(out)) == -1);
}

static int
cancel_after_polls(void *userdata)
{
    int *polls = userdata;
    return ++*polls > 5;
}

static void
test_command_cancellation(void)
{
    char output[256];
    char marker[256], command[1024];
    int polls = 0;
    snprintf(marker, sizeof(marker), "/tmp/krait-command-marker-%d", (int)getpid());
    unlink(marker);
    snprintf(command, sizeof(command),
        "trap '' TERM; (trap '' TERM; sleep 1; echo leaked > '%s') & wait", marker);
    CHECK(krait_run_capture_cancel("/tmp", command, 10, output, sizeof(output),
                                  cancel_after_polls, &polls) == 130);
    struct timespec delay = {1, 100000000};
    nanosleep(&delay, NULL);
    CHECK(access(marker, F_OK) != 0);
    CHECK(krait_run_capture("/tmp", "trap '' TERM; sleep 5", 1,
                            output, sizeof(output)) == 124);
    CHECK(krait_run_capture("/tmp", "printf tail-output", 2,
                            output, sizeof(output)) == 0);
    CHECK(strcmp(output, "tail-output") == 0);
    CHECK(krait_run_capture("/tmp", "printf large-output", 2, output, 1) == 0);
    CHECK(output[0] == 0);
    CHECK(krait_run_capture("/tmp", "true", 2, NULL, 0) == 0);
}

static void
test_project_validation(void)
{
    const char *project = "/tmp/krait-validation-test";
    char directory[1024], config[1024], report_path[2048];
    char *result, *report = NULL;
    long len;
    unsigned hash = 2166136261u;
    for(const char *p = project; *p; p++) hash = (hash ^ (unsigned char)*p) * 16777619u;
    snprintf(directory, sizeof(directory), "%s/.krait", project);
    krait_mkdir_p(directory);
    snprintf(config, sizeof(config), "%s/.krait/tasks.json", project);
    CHECK(krait_agent_bind_task(project, "checks"));
    snprintf(report_path, sizeof(report_path), "%s/.kryon/krait/agent/krait-validation-test-%08x--checks/history.jsonl.validation.json", getenv("HOME"), hash);
    CHECK(krait_write_text_file_atomic(config,
        "{\"tasks\":[{\"name\":\"build\",\"command\":\"printf build-ok\"},{\"name\":\"test\",\"command\":\"exit 7\"}]}"));
    result = krait_agent_run_tools("[{\"tool\":\"validate\"}]");
    CHECK(result != NULL && strstr(result, "[validate build] passed") != NULL);
    CHECK(result != NULL && strstr(result, "[validate test] FAILED") != NULL);
    free(result);
    CHECK(krait_read_file_alloc(report_path, &report, &len));
    CHECK(report != NULL && strstr(report, "\"exit_code\":7") != NULL);
    CHECK(report != NULL && strstr(report, "\"passed\":0") != NULL);
    CHECK(krait_agent_checks_load() == 2);
    CHECK(!strcmp(krait_agent_check_text(0, 0), "build"));
    CHECK(!strcmp(krait_agent_check_text(0, 1), "printf build-ok"));
    CHECK(strstr(krait_agent_check_text(0, 2), "build-ok"));
    CHECK(krait_agent_check_number(1, 0) == 7);
    CHECK(krait_agent_check_number(0, 1) >= 0);
    CHECK(strstr(krait_agent_checks_status(), "failed"));
    CHECK(!*krait_agent_check_text(99, 0));
    free(report); report = NULL;
    CHECK(krait_write_text_file_atomic(config,
        "{\"tasks\":[{\"name\":\"test\",\"command\":\"printf all-passed\"}]}"));
    CHECK(krait_agent_validate());
    CHECK(!krait_agent_validate()); /* single owner of tool execution */
    for(int i = 0; i < 500 && krait_agent_busy(); i++) {
        krait_agent_poll();
        usleep(10000);
    }
    CHECK(!krait_agent_busy());
    CHECK(strcmp(krait_agent_run_state(), "review") == 0);
    CHECK(krait_read_file_alloc(report_path, &report, &len));
    CHECK(report != NULL && strstr(report, "\"passed\":1") != NULL);
    CHECK(krait_agent_validation_current());
    CHECK(krait_agent_checks_load() == 1);
    CHECK(strstr(krait_agent_checks_status(), "current"));
    char source[1024];
    snprintf(source, sizeof(source), "%s/source.txt", project);
    CHECK(krait_write_text_file_atomic(source, "changed after validation"));
    CHECK(!krait_agent_validation_current());
    CHECK(krait_agent_checks_load() == 1);
    CHECK(strstr(krait_agent_checks_status(), "stale"));
    unlink(source);
    CHECK(krait_agent_validation_current());
    free(report); report = NULL;
    CHECK(krait_write_text_file_atomic(report_path, "{broken"));
    CHECK(krait_agent_checks_load() == 0);
    CHECK(krait_agent_checks_count() == 0);
    CHECK(strstr(krait_agent_checks_status(), "invalid"));
    CHECK(krait_write_text_file_atomic(config, "{broken"));
    result = krait_agent_run_tools("[{\"tool\":\"validate\"}]");
    CHECK(result != NULL && strstr(result, "FAILED") != NULL);
    free(result);
    CHECK(krait_read_file_alloc(report_path, &report, &len));
    CHECK(report != NULL && strstr(report, "\"passed\":0") != NULL);
    free(report);
}

static void
test_card_acceptance_gate(void)
{
    const char *project = "/tmp/krait-card-acceptance";
    char config[1024], source[1024], card_id[128];
    char *result;
    snprintf(source, sizeof(source), "%s/source.txt", project);
    unlink(source);
    snprintf(config, sizeof(config), "%s/.krait", project);
    krait_mkdir_p(config);
    snprintf(config, sizeof(config), "%s/.krait/tasks.json", project);
    CHECK(krait_write_text_file_atomic(config,
        "{\"tasks\":[{\"name\":\"check\",\"command\":\"true\"}]}"));
    int index = krait_kanban_create(2, "Acceptance regression");
    CHECK(index >= 0);
    snprintf(card_id, sizeof(card_id), "%s", krait_kanban_card_id(2, index));
    CHECK(krait_kanban_set_project(2, index, project));
    CHECK(!krait_kanban_move(2, index, 3));
    CHECK(strstr(krait_kanban_card_status(2, index), "Cannot accept") != NULL);
    CHECK(krait_agent_bind_task(project, card_id));
    result = krait_agent_run_tools("[{\"tool\":\"validate\"}]"); free(result);
    CHECK(krait_agent_validation_current());
    CHECK(krait_kanban_set_field(2, index, 3, "New acceptance criterion"));
    CHECK(!krait_agent_validation_current());
    CHECK(!krait_kanban_move(2, index, 3));
    result = krait_agent_run_tools("[{\"tool\":\"validate\"}]"); free(result);
    CHECK(krait_agent_validation_current());
    snprintf(source, sizeof(source), "%s/source.txt", project);
    CHECK(krait_write_text_file_atomic(source, "new source"));
    CHECK(!krait_kanban_move(2, index, 3));
    result = krait_agent_run_tools("[{\"tool\":\"validate\"}]"); free(result);
    CHECK(krait_kanban_move(2, index, 3));
    for(int i = 0; i < krait_kanban_count(3); i++) {
        if(strcmp(krait_kanban_card_id(3, i), card_id) == 0) {
            CHECK(krait_kanban_delete(3, i));
            break;
        }
    }
    CHECK(krait_write_text_file_atomic(config,
        "{\"tasks\":[{\"name\":\"mutates source\",\"command\":\"echo mutation >> source.txt\"}]}"));
    result = krait_agent_run_tools("[{\"tool\":\"validate\"}]");
    CHECK(result != NULL && strstr(result, "sources changed during validation") != NULL);
    free(result);
    CHECK(!krait_agent_validation_current());
}

static void
test_workspace_search(void)
{
    char root[] = "/tmp/krait-search-XXXXXX";
    char path[1024], linkpath[1024];
    SearchResult matches[64];
    CHECK(mkdtemp(root) != NULL);
    snprintf(path, sizeof(path), "%s/source.py", root);
    char *content = malloc(3000);
    CHECK(content != NULL);
    if(content == NULL) return;
    memset(content, 'x', 2000);
    strcpy(content + 2000, " Needle42\nsecond line\nNEEDLE77\n");
    CHECK(krait_write_text_file_atomic(path, content));
    free(content);
    CHECK(krait_search_project_options(root, "needle[0-9]+", 1, 0, 0, "", matches, 64) == 2);
    CHECK(matches[0].line == 1);
    CHECK(strstr(matches[0].excerpt, "Needle42") != NULL);
    CHECK(matches[1].line == 3);
    CHECK(krait_search_project_options(root, "^NEEDLE77$", 1, 1, 0, "", matches, 64) == 1);
    CHECK(matches[0].line == 3);
    CHECK(krait_search_project_options(root, "Needle42", 0, 1, 0, "", matches, 64) == 1);
    CHECK(krait_search_project_options(root, "needle42", 0, 1, 0, "", matches, 64) == 0);
    CHECK(krait_search_project_options(root, "needle", 0, 0, 0, "*.py", matches, 64) == 0);
    CHECK(krait_search_project_options(root, "[broken", 1, 0, 0, "", matches, 64) == -1);
    CHECK(krait_search_project_options(root, "source", 0, 0, 1, "", matches, 64) == 1);
    CHECK(strcmp(matches[0].path, "source.py") == 0);
    CHECK(krait_search_project_options(root, "source", 0, 0, 0, "", matches, 64) == 0);
    snprintf(linkpath, sizeof(linkpath), "%s/linked.py", root);
    CHECK(symlink(path, linkpath) == 0);
    CHECK(krait_search_project_options(root, "Needle42", 0, 1, 0, "", matches, 64) == 1);
    snprintf(path, sizeof(path), "%s/asset.bin", root);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if(f) { fwrite("\0Needle42", 1, 9, f); fclose(f); }
    CHECK(krait_search_project_options(root, "Needle42", 0, 1, 0, "", matches, 64) == 1);
}

static void
test_workspace_replacement(void)
{
    int matches;
    char *out = krait_replace_text("Cat cat CAT", "cat", "$1", 0, 0, &matches);
    CHECK(out && !strcmp(out, "$1 $1 $1") && matches == 3); free(out);
    out = krait_replace_text("cat42 cat7", "(cat)([0-9]+)", "$2:$1:$$", 1, 1, &matches);
    CHECK(out && !strcmp(out, "42:cat:$ 7:cat:$") && matches == 2); free(out);
    out = krait_replace_text("é\nx", "^", ">", 1, 1, &matches);
    CHECK(out && !strcmp(out, ">é\n>x") && matches == 2); free(out);
    out = krait_replace_text("é", "x*", "-", 1, 1, &matches);
    CHECK(out && !strcmp(out, "-é-") && matches == 2); free(out);
    CHECK(krait_replace_text("x", "[", "y", 1, 1, &matches) == NULL);
    out = krait_replace_text("xx", "x", "", 0, 1, NULL);
    CHECK(out && !*out); free(out);

    char root[] = "/tmp/krait-replace-XXXXXX", first[1024], second[1024], path[1024];
    CHECK(mkdtemp(root) != NULL);
    snprintf(first, sizeof(first), "%s/a.txt", root);
    snprintf(second, sizeof(second), "%s/b.txt", root);
    CHECK(krait_write_text_file_atomic(first, "cat\ncat\n"));
    CHECK(krait_write_text_file_atomic(second, "cat"));
    CHECK(krait_replace_preview(root, "cat", "dog", 0, 1, "b.*") == 1);
    CHECK(!strcmp(krait_replace_content(0, 0), "cat\ncat\n"));
    CHECK(!strcmp(krait_replace_content(0, 1), "dog\ndog\n"));
    char *text = NULL; long len;
    CHECK(krait_read_file_alloc(first, &text, &len) && !strcmp(text, "cat\ncat\n")); free(text);
    CHECK(krait_replace_preview(root, "cat", "dog", 0, 1, "") == 2);
    IdeState *st = calloc(1, sizeof(*st));
    CHECK(st != NULL);
    if(!st) return;
    snprintf(st->project.path, sizeof(st->project.path), "%s", root);
    st->open_count = 1;
    snprintf(st->open_files[0].path, sizeof(st->open_files[0].path), "%s", first);
    st->open_files[0].dirty = 1;
    CHECK(krait_replace_apply(st) == 0);
    CHECK(strstr(krait_replace_status(), "unsaved") != NULL);
    st->open_files[0].dirty = 0;
    CHECK(krait_write_text_file_atomic(second, "external edit"));
    CHECK(krait_replace_apply(st) == 0);
    CHECK(strstr(krait_replace_status(), "changed since preview") != NULL);
    CHECK(krait_read_file_alloc(first, &text, &len) && !strcmp(text, "cat\ncat\n")); free(text);
    CHECK(krait_write_text_file_atomic(second, "cat"));
    CHECK(krait_replace_apply(st) == 2);
    CHECK(krait_read_file_alloc(first, &text, &len) && !strcmp(text, "dog\ndog\n")); free(text);
    CHECK(krait_read_file_alloc(second, &text, &len) && !strcmp(text, "dog")); free(text);
    const char *backup = strstr(krait_replace_status(), "recovery record: ");
    CHECK(backup != NULL);
    if(backup) {
        CHECK(krait_read_file_alloc(backup + strlen("recovery record: "), &text, &len));
        CHECK(text && strstr(text, "cat") && strstr(text, "dog") && strstr(text, root)); free(text);
    }
    CHECK(krait_replace_apply(st) == 0); /* old before-images cannot be replayed */
    snprintf(path, sizeof(path), "%s/link.txt", root);
    CHECK(symlink(first, path) == 0);
    CHECK(krait_replace_preview(root, "dog", "fox", 0, 1, "") == 2);
    CHECK(unlink(first) == 0);
    CHECK(symlink(second, first) == 0);
    CHECK(krait_replace_apply(st) == 0);
    CHECK(unlink(first) == 0);
    CHECK(krait_write_text_file_atomic(first, "dog"));
    snprintf(path, sizeof(path), "%s/nested/.git", root); krait_mkdir_p(path);
    snprintf(path, sizeof(path), "%s/nested/source.txt", root);
    CHECK(krait_write_text_file_atomic(path, "dog"));
    CHECK(krait_replace_preview(root, "dog", "fox", 0, 1, "") == -1);
    CHECK(krait_replace_count() == 0);
    CHECK(krait_replace_preview(root, "dog", "fox", 0, 1, "nested/*") == 2);
    CHECK(krait_replace_preview(root, "[", "x", 1, 1, "") == -1);
    CHECK(krait_replace_count() == 0);
    char many[257];
    for(int i = 0; i < 64; i++) memcpy(many + i * 4, "dog\n", 4);
    many[256] = 0;
    CHECK(krait_write_text_file_atomic(first, many));
    CHECK(krait_replace_preview(root, "dog", "fox", 0, 1, "nested/*") == -1);
    CHECK(strstr(krait_replace_status(), "limit reached") != NULL);
    CHECK(krait_replace_count() == 0);
    CHECK(krait_read_file_alloc(first, &text, &len) && !strcmp(text, many)); free(text);
    krait_replace_clear(); free(st);
}

/* Tools execute through the same public entry the live loop uses. */
static void
test_tools(void)
{
    char *results;
    char status[256];
    char full[KRAIT_PATH_MAX * 2];

    system("rm -rf /tmp/krait-agent-tools-proj");
    CHECK(krait_scaffold_project("/tmp/krait-agent-tools-proj", status,
                                 sizeof(status)) == 1);
    krait_agent_bind("/tmp/krait-agent-tools-proj");

    results = krait_agent_run_tools(
        "[{\"tool\":\"search\",\"query\":\"Screen\"}]");
    CHECK(results != NULL);
    CHECK(strstr(results, "[search 'Screen']") != NULL);
    CHECK(strstr(results, "main.kry") != NULL);
    free(results);

    results = krait_agent_run_tools(
        "[{\"tool\":\"sfs_read\",\"path\":\"/info\"}]");
    CHECK(results != NULL);
    CHECK(strstr(results, "kryon") != NULL);
    free(results);

    results = krait_agent_run_tools(
        "[{\"tool\":\"sfs_list\",\"path\":\"/\"}]");
    CHECK(results != NULL);
    CHECK(strstr(results, "widgets/") != NULL);
    free(results);

    /* write + backup + revert round-trip: first write creates the file,
     * the second backs it up, revert restores the first content */
    results = krait_agent_run_tools(
        "[{\"tool\":\"write\",\"path\":\"notes.txt\",\"content\":\"v1\"}]");
    CHECK(results != NULL);
    free(results);
    results = krait_agent_run_tools(
        "[{\"tool\":\"write\",\"path\":\"notes.txt\",\"content\":\"v2\"}]");
    CHECK(results != NULL);
    CHECK(strstr(results, "[write notes.txt] ok (+1 -1 lines)") != NULL);
    CHECK(strstr(results, "[compile]") != NULL);   /* auto gate after write */
    free(results);
    CHECK(krait_agent_written_count() == 2);
    CHECK(strcmp(krait_agent_written_path(1), "notes.txt") == 0);
    snprintf(full, sizeof(full), "%s/notes.txt",
             "/tmp/krait-agent-tools-proj");
    {
        char *text = NULL;
        long len;

        CHECK(krait_read_file_alloc(full, &text, &len));
        CHECK(text != NULL && strcmp(text, "v2") == 0);
        free(text);
    }
    CHECK(krait_agent_can_revert());
    CHECK(krait_agent_revert() == 1);
    {
        char *text = NULL;
        long len;

        CHECK(krait_read_file_alloc(full, &text, &len));
        CHECK(text != NULL && strcmp(text, "v1") == 0);   /* restored */
        free(text);
    }
    CHECK(!krait_agent_can_revert());

    /* the agent files a kanban card in Backlog */
    results = krait_agent_run_tools(
        "[{\"tool\":\"card\",\"title\":\"From agent\","
        "\"body\":\"check the go button\"}]");
    CHECK(results != NULL);
    CHECK(strstr(results, "[card 'From agent'] created in Backlog") != NULL);
    free(results);
    CHECK(krait_kanban_count(0) > 0);
    CHECK(strcmp(krait_kanban_card_title(0, krait_kanban_count(0) - 1),
                 "From agent") == 0);
    krait_kanban_delete(0, krait_kanban_count(0) - 1);

    results = krait_agent_run_tools("[{\"tool\":\"nope\"}]");
    CHECK(results != NULL);
    free(results);
    system("rm -rf /tmp/krait-agent-tools-proj");
}

static void
test_change_recovery(void)
{
    const char *project = "/tmp/krait-agent-recovery";
    char full[1024], instruction[1024];
    char *result, *text = NULL;
    long len;
    krait_mkdir_p(project);
    CHECK(krait_agent_bind_task(project, "recovery"));
    snprintf(full, sizeof(full), "%s/new.txt", project);
    unlink(full);
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"new.txt\",\"content\":\"first\"},{\"tool\":\"write\",\"path\":\"new.txt\",\"content\":\"second\"}]");
    CHECK(result != NULL && strstr(result, "refused") == NULL);
    free(result);
    CHECK(krait_agent_can_revert());
    CHECK(krait_agent_bind_task(project, "elsewhere"));
    CHECK(!krait_agent_can_revert());
    CHECK(krait_agent_bind_task(project, "recovery"));
    CHECK(krait_agent_can_revert());
    CHECK(krait_write_text_file(full, "user edit"));
    CHECK(krait_agent_revert() == 0);
    CHECK(krait_read_file_alloc(full, &text, &len));
    CHECK(text != NULL && strcmp(text, "user edit") == 0);
    free(text);
    CHECK(krait_write_text_file(full, "second"));
    CHECK(krait_agent_revert() == 1);
    CHECK(access(full, F_OK) != 0);
    CHECK(!krait_agent_can_revert());

    snprintf(instruction, sizeof(instruction), "%s/AGENTS.md", project);
    CHECK(krait_write_text_file(instruction, "Use project conventions."));
    result = krait_agent_instructions(project);
    CHECK(result != NULL && strstr(result, "Use project conventions.") != NULL);
    free(result);
    unlink(instruction);
}

static void
test_selective_review(void)
{
    const char *project = "/tmp/krait-selective-review";
    char a[1024], b[1024];
    char *result, *text = NULL;
    long len;
    krait_mkdir_p(project);
    snprintf(a, sizeof(a), "%s/a.txt", project);
    snprintf(b, sizeof(b), "%s/b.txt", project);
    CHECK(krait_write_text_file_atomic(a, "original a"));
    CHECK(krait_write_text_file_atomic(b, "original b"));
    CHECK(krait_agent_bind_task(project, "review"));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"a.txt\",\"content\":\"new a\"},{\"tool\":\"write\",\"path\":\"b.txt\",\"content\":\"new b\"}]");
    free(result);
    CHECK(krait_agent_change_count() == 2);
    CHECK(strcmp(krait_agent_change_content(0, 0), "original a") == 0);
    CHECK(strcmp(krait_agent_change_content(0, 1), "new a") == 0);
    CHECK(krait_agent_review_change(0, 1));
    CHECK(krait_agent_change_review(0) == 1);
    CHECK(!krait_agent_review_change(0, 0));
    CHECK(krait_write_text_file_atomic(b, "user changed b"));
    CHECK(!krait_agent_review_change(1, 0));
    CHECK(krait_agent_change_review(1) == 0);
    CHECK(krait_agent_bind_task(project, "elsewhere"));
    CHECK(krait_agent_bind_task(project, "review"));
    CHECK(krait_agent_change_review(0) == 1);
    CHECK(krait_agent_change_review(1) == 0);
    CHECK(krait_write_text_file_atomic(b, "new b"));
    CHECK(krait_agent_revert() == 1);
    CHECK(krait_agent_change_review(1) == 2);
    CHECK(!krait_agent_can_revert());
    CHECK(krait_read_file_alloc(a, &text, &len));
    CHECK(text != NULL && strcmp(text, "new a") == 0);
    free(text); text = NULL;
    CHECK(krait_read_file_alloc(b, &text, &len));
    CHECK(text != NULL && strcmp(text, "original b") == 0);
    free(text);
    CHECK(krait_agent_bind_task(project, "elsewhere"));
    CHECK(krait_agent_bind_task(project, "review"));
    CHECK(krait_agent_change_review(0) == 1);
    CHECK(krait_agent_change_review(1) == 2);
}

static int
read_snapshot_child(const char *root)
{
    if(!krait_agent_bind_task(root, "durable-tests")) return 1;
    char *result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"source.txt\",\"content\":\"stale agent write\"}]");
    int ok = result && strstr(result, "refused"); free(result);
    krait_agent_shutdown();
    return ok ? 0 : 1;
}

static void
test_read_snapshot_restart(const char *binary)
{
    char root[] = "/tmp/krait-read-snapshot-XXXXXX", path[1024], pattern[2048];
    CHECK(mkdtemp(root) != NULL);
    CHECK(krait_agent_bind_task(root, "durable-tests"));
    snprintf(path, sizeof(path), "%s/source.txt", root);
    CHECK(krait_write_text_file_atomic(path, "original"));
    char *result = krait_agent_run_tools("[{\"tool\":\"read\",\"path\":\"source.txt\"},{\"tool\":\"read\",\"path\":\"absent.txt\"}]"); free(result);
    CHECK(krait_write_text_file_atomic(path, "external edit"));
    pid_t child = fork(); CHECK(child >= 0);
    if(child == 0) {
        setenv("KRAIT_READ_SNAPSHOT_CHILD", root, 1);
        execl(binary, binary, (char *)NULL); _exit(127);
    }
    if(child > 0) { int status; CHECK(waitpid(child, &status, 0) == child); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0); }
    CHECK(krait_agent_bind_task(root, "other-task"));
    CHECK(krait_agent_bind_task(root, "durable-tests"));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"source.txt\",\"content\":\"stale agent write\"}]");
    CHECK(result && strstr(result, "refused")); free(result);
    snprintf(path, sizeof(path), "%s/absent.txt", root);
    CHECK(krait_write_text_file_atomic(path, "created externally"));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"absent.txt\",\"content\":\"overwrite\"}]");
    CHECK(result && strstr(result, "refused")); free(result);
    result = krait_agent_run_tools("[{\"tool\":\"read\",\"path\":\"source.txt\"},{\"tool\":\"write\",\"path\":\"source.txt\",\"content\":\"updated\"}]");
    CHECK(result && strstr(result, "] ok")); free(result);
    snprintf(pattern, sizeof(pattern), "%s/.kryon/krait/agent/%s-*--durable-tests/reads.json", getenv("HOME"), strrchr(root, '/') + 1);
    glob_t files = {0};
    CHECK(glob(pattern, 0, NULL, &files) == 0 && files.gl_pathc == 1);
    if(files.gl_pathc == 1) {
        CHECK(krait_write_text_file_atomic(files.gl_pathv[0], "corrupt"));
        CHECK(krait_agent_bind_task(root, "other-task"));
        CHECK(krait_agent_bind_task(root, "durable-tests"));
        result = krait_agent_run_tools("[{\"tool\":\"read\",\"path\":\"source.txt\"},{\"tool\":\"write\",\"path\":\"source.txt\",\"content\":\"overwrite\"}]");
        CHECK(result && strstr(result, "snapshot unavailable")); free(result);
        char *text = NULL; long len;
        CHECK(krait_read_file_alloc(files.gl_pathv[0], &text, &len) && !strcmp(text, "corrupt")); free(text);
    }
    globfree(&files);
}

static void
test_unsaved_write_guard(void)
{
    char root[] = "/tmp/krait-unsaved-XXXXXX", path[1024];
    CHECK(mkdtemp(root) != NULL);
    CHECK(krait_agent_bind_task(root, "unsaved"));
    snprintf(path, sizeof(path), "%s/source.txt", root);
    CHECK(krait_write_text_file_atomic(path, "disk"));
    IdeState *st = calloc(1, sizeof(*st));
    CHECK(st != NULL);
    if(!st) return;
    st->open_count = 1;
    snprintf(st->open_files[0].path, sizeof(st->open_files[0].path), "%s", path);
    strcpy(st->open_files[0].source, "local draft");
    st->open_files[0].dirty = 1;
    krait_agent_sync_editors(st);
    char *result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"source.txt\",\"content\":\"agent\"}]");
    CHECK(result && strstr(result, "unsaved editor changes")); free(result);
    CHECK(krait_agent_change_count() == 0);
    CHECK(!krait_project_file_replace(root, "source.txt", "disk", 1, "overwrite"));
    CHECK(!krait_project_file_replace(root, "source.txt", "disk", 1, NULL));
    st->open_files[0].dirty = 0;
    st->open_files[0].artifact_cache_dirty[2] = 1;
    krait_agent_sync_editors(st);
    CHECK(!krait_project_file_replace(root, "source.txt", "disk", 1, "overwrite"));
    char *text = NULL; long len;
    CHECK(krait_read_file_alloc(path, &text, &len) && !strcmp(text, "disk")); free(text);
    CHECK(!strcmp(st->open_files[0].source, "local draft"));
    st->open_files[0].artifact_cache_dirty[2] = 0;
    krait_agent_sync_editors(st);
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"source.txt\",\"content\":\"agent\"}]");
    CHECK(result && strstr(result, "] ok")); free(result);
    CHECK(krait_agent_change_count() == 1);
    st->open_files[0].dirty = 1;
    krait_agent_sync_editors(st);
    CHECK(krait_agent_review_change(0, 0) == 0);
    CHECK(krait_read_file_alloc(path, &text, &len) && !strcmp(text, "agent")); free(text);
    krait_agent_sync_editors(NULL);
    CHECK(krait_agent_review_change(0, 0) == 1);
    CHECK(krait_read_file_alloc(path, &text, &len) && !strcmp(text, "disk")); free(text);
    free(st);
}

static void
test_file_guards(void)
{
    char project[] = "/tmp/krait-file-guards-XXXXXX";
    char outside[] = "/tmp/krait-outside-XXXXXX";
    char file[1024], linkpath[1024], outside_file[1024];
    char *result, *text = NULL;
    long len;
    CHECK(mkdtemp(project) != NULL);
    CHECK(mkdtemp(outside) != NULL);
    CHECK(krait_agent_bind_task(project, "guards"));
    snprintf(outside_file, sizeof(outside_file), "%s/private.txt", outside);
    CHECK(krait_write_text_file_atomic(outside_file, "outside untouched"));
    snprintf(linkpath, sizeof(linkpath), "%s/link", project);
    CHECK(symlink(outside, linkpath) == 0);
    result = krait_agent_run_tools("[{\"tool\":\"read\",\"path\":\"link/private.txt\"},{\"tool\":\"write\",\"path\":\"link/private.txt\",\"content\":\"overwrite\"}]");
    CHECK(result != NULL && strstr(result, "refused") != NULL);
    CHECK(result != NULL && strstr(result, "outside untouched") == NULL);
    free(result);
    snprintf(file, sizeof(file), "%s/direct.txt", project);
    CHECK(symlink(outside_file, file) == 0);
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"direct.txt\",\"content\":\"overwrite\"}]");
    CHECK(result != NULL && strstr(result, "refused") != NULL); free(result);
    CHECK(krait_read_file_alloc(outside_file, &text, &len));
    CHECK(text != NULL && strcmp(text, "outside untouched") == 0); free(text); text = NULL;
    snprintf(file, sizeof(file), "%s/vendor/module", project);
    krait_mkdir_p(file);
    snprintf(file, sizeof(file), "%s/vendor/module/.git", project);
    CHECK(krait_write_text_file_atomic(file, "gitdir: elsewhere"));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"vendor/module/code.txt\",\"content\":\"wrong repository\"}]");
    CHECK(result != NULL && strstr(result, "refused") != NULL); free(result);
    snprintf(file, sizeof(file), "%s/source.txt", project);
    CHECK(krait_write_text_file_atomic(file, "first"));
    result = krait_agent_run_tools("[{\"tool\":\"read\",\"path\":\"source.txt\"}]"); free(result);
    CHECK(krait_write_text_file_atomic(file, "user revision"));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"source.txt\",\"content\":\"stale overwrite\"}]");
    CHECK(result != NULL && strstr(result, "refused") != NULL); free(result);
    CHECK(krait_read_file_alloc(file, &text, &len));
    CHECK(text != NULL && strcmp(text, "user revision") == 0); free(text);
    result = krait_agent_run_tools("[{\"tool\":\"read\",\"path\":\"source.txt\"},{\"tool\":\"write\",\"path\":\"source.txt\",\"content\":\"fresh revision\"}]");
    CHECK(result != NULL && strstr(result, "[write source.txt] ok") != NULL); free(result);
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"nested/deep/new.txt\",\"content\":\"nested\"}]");
    CHECK(result != NULL && strstr(result, "[write nested/deep/new.txt] ok") != NULL); free(result);
    snprintf(file, sizeof(file), "%s/nested/deep/new.txt", project);
    CHECK(unlink(file) == 0);
    CHECK(symlink(outside_file, file) == 0);
    CHECK(!krait_agent_review_change(0, 0));
    CHECK(unlink(file) == 0);
    CHECK(krait_agent_review_change(0, 0));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"nested/deep/new.txt\",\"content\":\"recreated\"}]");
    CHECK(result != NULL && strstr(result, "[write nested/deep/new.txt] ok") != NULL); free(result);
}

static void
test_checkpoint_history(void)
{
    char project[] = "/tmp/krait-checkpoints-XXXXXX";
    CHECK(mkdtemp(project) != NULL);
    CHECK(krait_agent_bind_task(project, "history"));
    char *result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"a.txt\",\"content\":\"first\"}]");
    free(result);
    CHECK(krait_agent_checkpoint_count() == 0);
    CHECK(krait_agent_review_change(0, 1));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"a.txt\",\"content\":\"second\"}]"); free(result);
    CHECK(krait_agent_checkpoint_count() == 1);
    CHECK(krait_agent_checkpoint_select(0));
    CHECK(strcmp(krait_agent_change_content(0, 1), "first") == 0);
    CHECK(krait_agent_change_review(0) == 1);
    CHECK(!krait_agent_review_change(0, 0));
    CHECK(!krait_agent_can_revert());
    CHECK(krait_agent_checkpoint_select(-1));
    CHECK(strcmp(krait_agent_change_content(0, 1), "second") == 0);
    CHECK(krait_agent_bind_task(project, "other"));
    CHECK(krait_agent_bind_task(project, "history"));
    CHECK(krait_agent_checkpoint_count() == 1);
    CHECK(krait_agent_checkpoint_select(0));
    CHECK(strcmp(krait_agent_change_content(0, 1), "first") == 0);
    CHECK(krait_agent_checkpoint_select(-1));
    CHECK(krait_agent_revert() == 1);
    CHECK(krait_agent_change_review(0) == 2);
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"b.txt\",\"content\":\"third\"}]"); free(result);
    CHECK(krait_agent_checkpoint_count() == 2);
    CHECK(krait_agent_checkpoint_select(1));
    CHECK(strcmp(krait_agent_change_content(0, 1), "second") == 0);
    CHECK(krait_agent_change_review(0) == 2);
    CHECK(!krait_agent_checkpoint_select(2));
    CHECK(krait_agent_checkpoint_select(-1));
    CHECK(strcmp(krait_agent_change_path(0), "b.txt") == 0);

    CHECK(krait_agent_bind_task(project, "archive-failure"));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"c.txt\",\"content\":\"preserve me\"}]"); free(result);
    unsigned hash = 2166136261u;
    for(const char *p = project; *p; p++) hash = (hash ^ (unsigned char)*p) * 16777619u;
    char blocked[2048];
    snprintf(blocked, sizeof(blocked), "%s/.kryon/krait/agent/%s-%08x--archive-failure/history.jsonl.checkpoints",
             getenv("HOME"), krait_basename(project), hash);
    CHECK(krait_write_text_file_atomic(blocked, "not a directory"));
    result = krait_agent_run_tools("[{\"tool\":\"write\",\"path\":\"c.txt\",\"content\":\"must not land\"}]");
    CHECK(result != NULL && strstr(result, "refused") != NULL); free(result);
    CHECK(krait_agent_change_count() == 1);
    CHECK(strcmp(krait_agent_change_content(0, 1), "preserve me") == 0);
    CHECK(krait_agent_can_revert());
    CHECK(krait_agent_revert() == 1);
}

/* Hex editor backend: open/edit/save round-trip with .bak backup. */
static void
test_hex_editor(void)
{
    const char path[] = "/tmp/krait-hex-test.krb";
    unsigned char payload[8] = {0x4B, 0x52, 0x42, 0x01, 0x00, 0x00,
                                0xAB, 0xCD};
    FILE *f = fopen(path, "wb");

    CHECK(f != NULL);
    if(f != NULL) {
        fwrite(payload, 1, sizeof(payload), f);
        fclose(f);
    }

    CHECK(krait_hex_open(path) == 1);
    CHECK(krait_hex_size() == 8);
    CHECK(krait_hex_byte(0) == 0x4B);
    CHECK(krait_hex_byte(7) == 0xCD);
    CHECK(krait_hex_byte(8) == -1);   /* out of range */
    CHECK(krait_hex_dirty() == 0);

    CHECK(krait_hex_set_byte(3, 0x42) == 1);
    CHECK(krait_hex_byte(3) == 0x42);
    CHECK(krait_hex_dirty() == 1);
    CHECK(krait_hex_changed_count() == 1);

    CHECK(krait_hex_save() == 1);
    CHECK(krait_hex_dirty() == 0);
    CHECK(access("/tmp/krait-hex-test.krb.bak", F_OK) == 0);

    CHECK(krait_hex_open(path) == 1);   /* reopen reads saved bytes */
    CHECK(krait_hex_byte(3) == 0x42);
    CHECK(krait_hex_byte(7) == 0xCD);
    unlink(path);
    unlink("/tmp/krait-hex-test.krb.bak");
}

/* Multimodal plumbing: base64 encoding and request-body shape. */
static void
test_vision_body(void)
{
    char png[KRAIT_PATH_MAX];
    char *b64;
    char *body;
    KraitAiMessage msgs[2];

    CHECK(krait_ai_set_provider(1) == 1);   /* Z.ai keeps the vision default */
    snprintf(png, sizeof(png), "/tmp/krait-agent-b64-test.png");
    {
        const unsigned char png_magic[16] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a,
                                             0x1a, 0x0a, 0, 0, 0, 0x0d,
                                             'I', 'H', 'D', 'R'};
        FILE *f = fopen(png, "wb");

        CHECK(f != NULL);
        if(f != NULL) {
            fwrite(png_magic, 1, sizeof(png_magic), f);
            fclose(f);
        }
    }
    b64 = krait_ai_base64_file(png);
    CHECK(b64 != NULL);
    if(b64 != NULL) {
        /* known base64 of the PNG magic prefix is iVBORw0 */
        if(strncmp(b64, "iVBORw0", 7) != 0)
            fprintf(stderr, "b64 got: '%.24s'\n", b64);
        CHECK(strncmp(b64, "iVBORw0", 7) == 0);
        free(b64);
    }

    msgs[0].role = "system";
    msgs[0].content = "sys";
    msgs[0].image_b64 = NULL;
    msgs[1].role = "user";
    msgs[1].content = "look";
    msgs[1].image_b64 = "QUJD";   /* base64 of "ABC" */
    body = krait_ai_build_body(msgs, 2);
    CHECK(body != NULL);
    if(body != NULL) {
        CHECK(strstr(body, "image_url") != NULL);
        CHECK(strstr(body, "data:image/png;base64,QUJD") != NULL);
        CHECK(strstr(body, "\"text\":\"look\"") != NULL);
        CHECK(strstr(body, "glm-5v-turbo") != NULL);   /* vision model for image turns */
        free(body);
    }

    msgs[1].image_b64 = NULL;
    body = krait_ai_build_body(msgs, 2);
    CHECK(body != NULL);
    if(body != NULL) {
        CHECK(strstr(body, "image_url") == NULL);
        CHECK(strstr(body, "glm-5v-turbo") == NULL);
        free(body);
    }

    CHECK(krait_ai_set_provider(2) == 1);
    msgs[1].image_b64 = "QUJD";
    body = krait_ai_build_body(msgs, 2);
    CHECK(body != NULL);
    if(body != NULL) {
        CHECK(strstr(body, "\"type\":\"image\"") != NULL);
        CHECK(strstr(body, "\"source\"") != NULL);
        CHECK(strstr(body, "\"media_type\":\"image/png\"") != NULL);
        CHECK(strstr(body, "\"data\":\"QUJD\"") != NULL);
        CHECK(strstr(body, "\"image_url\"") == NULL);
        free(body);
    }
    unlink(png);
}

static void
test_provider_selection(void)
{
    KraitAiMessage msgs[2];
    char *body;

    CHECK(krait_ai_provider_count() == 3);
    CHECK(strcmp(krait_ai_provider_id(0), "codex") == 0);
    CHECK(strcmp(krait_ai_provider_id(1), "zai") == 0);
    CHECK(strcmp(krait_ai_provider_id(2), "claude") == 0);
    CHECK(krait_ai_effort_count() == 4);
    CHECK(strcmp(krait_ai_effort_name(3), "Max") == 0);
    CHECK(krait_ai_set_effort(3) == 1);
    CHECK(krait_ai_active_effort() == 3);

    msgs[0].role = "system";
    msgs[0].content = "sys";
    msgs[0].image_b64 = NULL;
    msgs[1].role = "user";
    msgs[1].content = "hi";
    msgs[1].image_b64 = NULL;

    CHECK(krait_ai_set_provider(0) == 1);
    body = krait_ai_build_body(msgs, 2);
    CHECK(body != NULL);
    if(body != NULL) {
        CHECK(strstr(body, krait_ai_provider_model(0)) != NULL);
        CHECK(strstr(body, "\"reasoning\":{\"effort\":\"xhigh\"}") != NULL);
        CHECK(strstr(body, "\"thinking\"") == NULL);
        free(body);
    }

    CHECK(krait_ai_set_provider(2) == 1);
    body = krait_ai_build_body(msgs, 2);
    CHECK(body != NULL);
    if(body != NULL) {
        CHECK(strstr(body, krait_ai_provider_model(2)) != NULL);
        CHECK(strstr(body, "\"system\":\"sys\"") != NULL);
        CHECK(strstr(body, "\"messages\"") != NULL);
        CHECK(strstr(body, "\"max_tokens\"") != NULL);
        CHECK(strstr(body, "\"stream\":true") != NULL);
        CHECK(strstr(body, "\"input\"") == NULL);
        CHECK(strstr(body, "\"reasoning\"") == NULL);
        CHECK(strstr(body, "\"thinking\"") == NULL);
        CHECK(strstr(body, "\"role\":\"system\"") == NULL);
        free(body);
    }

    CHECK(krait_ai_set_provider(1) == 1);
    body = krait_ai_build_body(msgs, 2);
    CHECK(body != NULL);
    if(body != NULL) {
        CHECK(strstr(body, krait_ai_provider_model(1)) != NULL);
        CHECK(strstr(body, "\"thinking\":{\"type\":\"enabled\"}") != NULL);
        CHECK(strstr(body, "\"reasoning_effort\":\"max\"") != NULL);
        free(body);
    }
}

static void
test_response_parsers(void)
{
    char *text = NULL;

    CHECK(krait_ai_extract_response_text(
        "{\"output_text\":\"codex ok\"}", &text) == 1);
    if(text != NULL) {
        CHECK(strcmp(text, "codex ok") == 0);
        free(text);
        text = NULL;
    }
    CHECK(krait_ai_extract_response_text(
        "{\"choices\":[{\"message\":{\"content\":\"chat ok\"}}]}", &text) == 1);
    if(text != NULL) {
        CHECK(strcmp(text, "chat ok") == 0);
        free(text);
        text = NULL;
    }
    CHECK(krait_ai_extract_response_text(
        "{\"choices\":[{\"message\":{\"content\":[{\"type\":\"text\","
        "\"text\":\"parts ok\"}]}}]}", &text) == 1);
    if(text != NULL) {
        CHECK(strcmp(text, "parts ok") == 0);
        free(text);
        text = NULL;
    }
    CHECK(krait_ai_extract_response_text(
        "{\"content\":[{\"type\":\"text\",\"text\":\"claude ok\"}]}", &text)
          == 1);
    if(text != NULL) {
        CHECK(strcmp(text, "claude ok") == 0);
        free(text);
        text = NULL;
    }
    CHECK(krait_ai_extract_response_text(
        "data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n"
        "data: [DONE]\n", &text) == 1);
    if(text != NULL) {
        CHECK(strcmp(text, "hello") == 0);
        free(text);
        text = NULL;
    }
    CHECK(krait_ai_extract_response_text(
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hi\"}\n"
        "data: [DONE]\n", &text) == 1);
    if(text != NULL) {
        CHECK(strcmp(text, "hi") == 0);
        free(text);
        text = NULL;
    }
    CHECK(krait_ai_extract_response_text(
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"claude\"}}\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\" stream\"}}\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n", &text) == 1);
    if(text != NULL) {
        CHECK(strcmp(text, "claude stream") == 0);
        free(text);
    }
}

static void
test_account_config_persistence(const char *argv0)
{
    char path[512];
    char *text = NULL;
    long len = 0;
    struct stat st;
    const char *home = getenv("HOME");
    pid_t pid;
    int status = 1;

    CHECK(krait_ai_set_provider(2) == 1);
    CHECK(krait_ai_set_effort(1) == 1);
    krait_ai_set_provider_key(2, "test-claude-key");
    krait_ai_set_provider_model(2, "claude-test-model");
    krait_ai_set_provider_base_url(2, "https://claude.example/v1");
    snprintf(path, sizeof(path), "%s/.kryon/krait/ai.conf",
             home != NULL ? home : ".");
    CHECK(krait_read_file_alloc(path, &text, &len) == 1);
    CHECK(text != NULL);
    if(text != NULL) {
        CHECK(strstr(text, "provider=claude") != NULL);
        CHECK(strstr(text, "effort=medium") != NULL);
        CHECK(strstr(text, "key.claude=test-claude-key") != NULL);
        CHECK(strstr(text, "model.claude=claude-test-model") != NULL);
        CHECK(strstr(text, "base.claude=https://claude.example/v1") != NULL);
        free(text);
    }
    CHECK(stat(path, &st) == 0);
    if(stat(path, &st) == 0)
        CHECK((st.st_mode & 0777) == 0600);
    CHECK(krait_ai_provider_configured(2) == 1);

    pid = fork();
    CHECK(pid >= 0);
    if(pid == 0) {
        setenv("KRAIT_AGENT_CONFIG_CHILD", "1", 1);
        unsetenv("KRAIT_AI_PROVIDER");
        unsetenv("AI_PROVIDER");
        unsetenv("KRAIT_AI_REASONING_EFFORT");
        unsetenv("AI_REASONING_EFFORT");
        execl(argv0, argv0, (char *)NULL);
        _exit(127);
    }
    if(pid > 0) {
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status));
        if(WIFEXITED(status))
            CHECK(WEXITSTATUS(status) == 0);
    }
}

/* Live vision: capture the rendered screen offscreen, send it as an
 * image part, and require a non-empty description back. Opt-in: costs a
 * real (vision) API call and needs a display for the offscreen render. */
static void
test_live_vision_gated(void)
{
    const char *optin = getenv("KRAIT_AGENT_VISION_LIVE");
    char status[256];
    char *results;
    int spins = 0;

    if(optin == NULL || optin[0] == '\0' || !krait_ai_configured())
        return;
    printf("live agent vision loop... ");
    fflush(stdout);
    system("rm -rf /tmp/krait-agent-vision-proj");
    CHECK(krait_scaffold_project("/tmp/krait-agent-vision-proj", status,
                                 sizeof(status)) == 1);
    InitWindow(640, 480, "krait-agent-vision-test");
    SetTargetFPS(60);
    krait_agent_clear();
    krait_agent_bind("/tmp/krait-agent-vision-proj");
    results = krait_agent_run_tools(
        "[{\"tool\":\"screenshot\",\"path\":\"main.kry\"}]");
    CHECK(results != NULL && strstr(results, "[screenshot main.kry] attached")
          != NULL);
    free(results);
    CHECK(krait_agent_send(
        "Describe the user interface shown in the attached screenshot in "
        "one sentence.") == 1);
    while(spins++ < 1200 && krait_agent_busy()) {
        BeginDrawing();
        EndDrawing();
        krait_agent_poll();
        usleep(250 * 1000);
    }
    CHECK(!krait_agent_busy());
    CloseWindow();
    if(krait_agent_count() > 0) {
        const char *reply = krait_agent_text(krait_agent_count() - 1);

        CHECK(reply != NULL && reply[0] != '\0');
        printf("reply: %.120s\nstatus '%s'\nok\n", reply,
               krait_agent_status_text());
    } else {
        CHECK(0);
    }
    system("rm -rf /tmp/krait-agent-vision-proj");
}

/* Streaming bodies request SSE; the final text still resolves. Covered
 * hermetically by the body shape and live by every gated live test. */
static void
test_stream_body(void)
{
    KraitAiMessage msgs[2];
    char *body;

    msgs[0].role = "system";
    msgs[0].content = "sys";
    msgs[0].image_b64 = NULL;
    msgs[1].role = "user";
    msgs[1].content = "hi";
    msgs[1].image_b64 = NULL;
    CHECK(krait_ai_set_provider(1) == 1);
    body = krait_ai_build_body(msgs, 2);
    CHECK(body != NULL);
    if(body != NULL) {
        CHECK(strstr(body, "\"stream\":true") != NULL);
        free(body);
    }
}

/* Live: user message -> model -> tool actions (read/write) -> compile
 * feedback -> final text, with the file really on disk. */
static void
test_live_agent_gated(void)
{
    const char *optin = getenv("KRAIT_AGENT_LIVE");
    char status[256];
    int spins = 0;

    if(optin == NULL || optin[0] == '\0' || !krait_ai_configured())
        return;
    printf("live agent tool loop... ");
    fflush(stdout);
    system("rm -rf /tmp/krait-agent-live-proj");
    CHECK(krait_scaffold_project("/tmp/krait-agent-live-proj", status,
                                 sizeof(status)) == 1);
    krait_agent_clear();
    krait_agent_set_full_access(1);
    krait_agent_bind("/tmp/krait-agent-live-proj");
    CHECK(krait_agent_send(
        "Read main.kry, then add a new file go.kry with a Go :: () #ui "
        "entry that draws a centered Button labeled Go. Compile to verify.")
          == 1);
    while(spins++ < 1600 && krait_agent_busy()) {
        krait_agent_poll();
        usleep(250 * 1000);
    }
    CHECK(!krait_agent_busy());
    CHECK(access("/tmp/krait-agent-live-proj/go.kry", F_OK) == 0);
    CHECK(krait_compile_gate_all("/tmp/krait-agent-live-proj", NULL, NULL, 0,
                                 NULL, 0, NULL, 0) == 0);
    printf("ok (%d messages, status '%s')\n", krait_agent_count(),
           krait_agent_status_text());
}

/* ---- markdown layout (native_md.c) ---- */
static void
test_markdown_layout(void)
{
    static const char *doc = "# Title\n"
                             "plain **bold** and `code` span\n"
                             "```c\nint x = 1;\nint y = 2;\n```\n"
                             "- alpha\n"
                             "- beta\n"
                             "1. first\n"
                             "> quoted line\n";
    int rows = krait_md_rows(doc, 400, 12);
    int kind = -1, runs = 0, indent = 0, bg = 0, i;
    int saw_h1 = 0, saw_code = 0, saw_bullet = 0, saw_num = 0, saw_quote = 0;

    CHECK(rows >= 8);
    for(i = 0; i < rows; i++) {
        char buf[256];
        int x = 0, style = 0;

        if(!krait_md_row_info(doc, 400, 12, i, &kind, &runs, &indent, &bg)) {
            CHECK(0);
            continue;
        }
        if(kind == 1)
            saw_h1 = 1;
        if(kind == 4) {
            saw_code = 1;
            CHECK(bg == 1);
        }
        if(kind == 6)
            saw_bullet = 1;
        if(kind == 7)
            saw_num = 1;
        if(kind == 5)
            saw_quote = 1;
        CHECK(krait_md_run_info(doc, 400, 12, i, 0, buf, sizeof(buf),
                                &x, &style) == 1 || runs == 0);
    }
    CHECK(saw_h1);
    CHECK(saw_code);
    CHECK(saw_bullet);
    CHECK(saw_num);
    CHECK(saw_quote);

    /* style bits: the bold word in row 2+ carries style 1 */
    {
        char buf[256];
        int x = 0, style = 0, found_bold = 0, found_code = 0;

        for(i = 0; i < rows; i++) {
            int kind2, runs2, ind2, bg2, r;

            krait_md_row_info(doc, 400, 12, i, &kind2, &runs2, &ind2, &bg2);
            for(r = 0; r < runs2; r++) {
                if(krait_md_run_info(doc, 400, 12, i, r, buf, sizeof(buf),
                                     &x, &style)) {
                    if(style % 2 == 1 && strcmp(buf, "bold") == 0)
                        found_bold = 1;
                    if(style / 4 % 2 == 1 && strcmp(buf, "code") == 0)
                        found_code = 1;
                }
            }
        }
        CHECK(found_bold);
        CHECK(found_code);
    }

    /* layout is safe at tiny widths (wrapping depends on the live font,
     * which needs a window, so only sanity-check row counts here) */
    CHECK(krait_md_rows("one two three four five six seven eight", 40, 12) >= 1);

    /* empty and NULL are safe */
    CHECK(krait_md_rows("", 400, 12) == 0);
    CHECK(krait_md_rows(NULL, 400, 12) == 0);
}

/* ---- permission gate no-op + retry guards ---- */
static void
test_permission_and_retry_guards(void)
{
    CHECK(krait_agent_permission_pending() == 0);
    CHECK(krait_agent_full_access_enabled() == 0);
    krait_agent_set_full_access(1);
    CHECK(krait_agent_full_access_enabled() == 1);
    krait_agent_set_full_access(0);
    CHECK(krait_agent_full_access_enabled() == 0);
    krait_agent_permission_respond(0, 0);   /* nothing pending: no-op */
    CHECK(krait_agent_retry(-1) == 0);
    CHECK(krait_agent_retry(9999) == 0);
}

int
main(int argc, char **argv)
{
    char home[256];
    const char *tmp = getenv("TMPDIR");

    (void)argc;
    if(getenv("KRAIT_READ_SNAPSHOT_CHILD")) return read_snapshot_child(getenv("KRAIT_READ_SNAPSHOT_CHILD"));
    if(getenv("KRAIT_AGENT_CONFIG_CHILD") != NULL)
        return config_child_check();

    snprintf(home, sizeof(home), "%s/krait-agent-test-home.XXXXXX",
             tmp != NULL ? tmp : "/tmp");
    if(mkdtemp(home) == NULL)
        return 1;
    setenv("HOME", home, 1);
    system("rm -rf /tmp/krait-agent-test-proj "
           "/tmp/krait-agent-test-proj-other");

    test_history_roundtrip();
    test_task_sessions();
    test_interrupted_run_recovery();
    test_compile_gate();
    test_run_capture();
    test_command_cancellation();
    CHECK(krait_command_matches("  SAVE file ", "File: Save"));
    CHECK(krait_command_matches("term kaps", "Tools: Kapsule Terminal"));
    CHECK(krait_command_matches("", "Edit: Undo"));
    CHECK(!krait_command_matches("undo redo", "Edit: Undo"));
    CHECK(!krait_command_matches("terminal extra", "Tools: Kapsule Terminal"));
    test_workspace_search();
    test_workspace_replacement();
    test_project_validation();
    test_card_acceptance_gate();
    test_tools();
    test_change_recovery();
    test_selective_review();
    test_read_snapshot_restart(argv[0]);
    test_unsaved_write_guard();
    test_file_guards();
    test_checkpoint_history();
    test_hex_editor();
    test_provider_selection();
    test_response_parsers();
    test_account_config_persistence(argv[0]);
    test_vision_body();
    test_stream_body();
    test_markdown_layout();
    test_permission_and_retry_guards();
    test_live_agent_gated();
    test_live_vision_gated();
    krait_agent_shutdown();

    if(failures == 0)
        printf("agent tests passed\n");
    return failures == 0 ? 0 : 1;
}
