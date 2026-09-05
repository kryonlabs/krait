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
    char source[1024];
    snprintf(source, sizeof(source), "%s/source.txt", project);
    CHECK(krait_write_text_file_atomic(source, "changed after validation"));
    CHECK(!krait_agent_validation_current());
    unlink(source);
    CHECK(krait_agent_validation_current());
    free(report); report = NULL;
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
    test_project_validation();
    test_card_acceptance_gate();
    test_tools();
    test_change_recovery();
    test_selective_review();
    test_file_guards();
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
