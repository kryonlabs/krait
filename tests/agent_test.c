/*
 * agent_test.c - agent engine + shared compile gate.
 *
 * Hermetic: history persist/load round-trip against a redirected HOME,
 * compile-gate verdicts on a scaffold project (clean and deliberately
 * broken overlays), and the bounded command runner. A live GLM round
 * trip with real tool use runs only when KRAIT_AGENT_LIVE=1 and
 * ZAI_API_KEY are both set.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { \
    if(!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while(0)

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
    CHECK(strstr(krait_agent_text(0), "ZAI_API_KEY") != NULL);

    krait_agent_clear();
    CHECK(krait_agent_count() == 0);
    krait_agent_bind("/tmp/krait-agent-test-proj");
    CHECK(krait_agent_count() == 0);   /* clear wiped the history file */
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
    CHECK(krait_compile_gate("/tmp/krait-agent-test-proj", NULL, NULL, 0,
                             err, sizeof(err)) == 0);

    paths[0] = "broken.kry";
    bodies[0] = "screen Bad(viewport: Rectangle) {\n    let x := 5\n}\n";
    err[0] = '\0';
    CHECK(krait_compile_gate("/tmp/krait-agent-test-proj", paths, bodies, 1,
                             err, sizeof(err)) == 1);
    CHECK(err[0] != '\0');
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
        "[{\"tool\":\"search\",\"query\":\"screen\"}]");
    CHECK(results != NULL);
    CHECK(strstr(results, "[search 'screen']") != NULL);
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

/* Multimodal plumbing: base64 encoding and request-body shape. */
static void
test_vision_body(void)
{
    char png[KRAIT_PATH_MAX];
    char *b64;
    char *body;
    KraitAiMessage msgs[2];

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
        CHECK(strstr(body, "glm-4.6v") != NULL);   /* vision model for image turns */
        free(body);
    }

    msgs[1].image_b64 = NULL;
    body = krait_ai_build_body(msgs, 2);
    CHECK(body != NULL);
    if(body != NULL) {
        CHECK(strstr(body, "image_url") == NULL);
        CHECK(strstr(body, "glm-4.6v") == NULL);
        free(body);
    }
    unlink(png);
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

/* Live: user message -> GLM -> tool actions (read/write) -> compile
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
    krait_agent_bind("/tmp/krait-agent-live-proj");
    CHECK(krait_agent_send(
        "Read main.kry, then add a new file go.kry with a screen Go that "
        "draws a centered Button labeled Go. Compile to verify.") == 1);
    while(spins++ < 1600 && krait_agent_busy()) {
        krait_agent_poll();
        usleep(250 * 1000);
    }
    CHECK(!krait_agent_busy());
    CHECK(access("/tmp/krait-agent-live-proj/go.kry", F_OK) == 0);
    CHECK(krait_compile_gate("/tmp/krait-agent-live-proj", NULL, NULL, 0,
                             NULL, 0) == 0);
    printf("ok (%d messages, status '%s')\n", krait_agent_count(),
           krait_agent_status_text());
}

int
main(void)
{
    char home[256];
    const char *tmp = getenv("TMPDIR");

    snprintf(home, sizeof(home), "%s/krait-agent-test-home.%d",
             tmp != NULL ? tmp : "/tmp", (int)getpid());
    setenv("HOME", home, 1);
    system("rm -rf /tmp/krait-agent-test-proj "
           "/tmp/krait-agent-test-proj-other");

    test_history_roundtrip();
    test_compile_gate();
    test_run_capture();
    test_tools();
    test_vision_body();
    test_live_agent_gated();
    test_live_vision_gated();
    krait_agent_shutdown();

    if(failures == 0)
        printf("agent tests passed\n");
    return failures == 0 ? 0 : 1;
}
