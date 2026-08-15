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
    test_live_agent_gated();
    krait_agent_shutdown();

    if(failures == 0)
        printf("agent tests passed\n");
    return failures == 0 ? 0 : 1;
}
