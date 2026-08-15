/*
 * kanban_test.c - board semantics against the real filesystem.
 *
 * The board IS files under ~/.kryon/krait/kanban, so the test redirects
 * HOME to a throwaway directory and exercises the native store: column
 * creation, card create/read/update, move-as-rename, project scaffold
 * binding, delete, and the no-key AI state. A live GLM round trip runs
 * only when KRAIT_KANBAN_LIVE=1 and ZAI_API_KEY are both set (it spends
 * a real API call).
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
test_board_crud(void)
{
    int i;

    CHECK(krait_kanban_rescan() == 0);
    CHECK(krait_kanban_count(0) == 0);

    i = krait_kanban_create(0, "First card");
    CHECK(i >= 0);
    CHECK(strcmp(krait_kanban_card_title(0, i), "First card") == 0);

    CHECK(krait_kanban_set_body(0, i, "make the button red"));
    CHECK(strcmp(krait_kanban_card_body(0, i), "make the button red") == 0);

    CHECK(krait_kanban_set_project(0, i, "/tmp/krait-kanban-test-proj"));
    CHECK(strcmp(krait_kanban_card_project(0, i),
                 "/tmp/krait-kanban-test-proj") == 0);

    /* move is a rename across column dirs */
    CHECK(krait_kanban_move(0, i, 1));
    CHECK(krait_kanban_count(0) == 0);
    CHECK(krait_kanban_count(1) == 1);
    CHECK(strcmp(krait_kanban_card_title(1, 0), "First card") == 0);
    CHECK(krait_kanban_move(1, 0, 0));
    CHECK(krait_kanban_count(0) == 1);

    CHECK(krait_kanban_delete(0, i));
    CHECK(krait_kanban_count(0) == 0);

    /* out-of-range accessors are safe */
    CHECK(krait_kanban_card_title(0, 99) != NULL);
    CHECK(krait_kanban_card_title(0, 99)[0] == '\0');
    CHECK(krait_kanban_count(-1) == 0);
    CHECK(krait_kanban_count(4) == 0);
}

static void
test_project_scaffold_binding(void)
{
    int i = krait_kanban_create(3, "Ship it");

    CHECK(i >= 0);
    CHECK(krait_kanban_new_project(3, i, "/tmp/krait-kanban-test-proj"));
    CHECK(access("/tmp/krait-kanban-test-proj/main.kry", F_OK) == 0);
    CHECK(access("/tmp/krait-kanban-test-proj/project.kryon", F_OK) == 0);
    CHECK(strcmp(krait_kanban_card_project(3, i),
                 "/tmp/krait-kanban-test-proj") == 0);
    CHECK(krait_kanban_delete(3, i));
}

static void
test_ai_state_without_key(void)
{
    if(krait_ai_configured()) {
        printf("key configured; skipping no-key assertions\n");
        return;
    }
    CHECK(!krait_ai_configured());
    CHECK(krait_kanban_ai_run(0, 0) == 0);
}

/* Live loop: card -> GLM -> proposal -> k2c+cc gates -> apply. Opt-in
 * because it spends a real API call and needs the compiler toolchain. */
static void
test_live_ai_gated(void)
{
    const char *optin = getenv("KRAIT_KANBAN_LIVE");
    int i;
    int state = 0;
    int spins = 0;

    if(optin == NULL || optin[0] == '\0' || !krait_ai_configured())
        return;
    printf("live kanban AI round trip... ");
    fflush(stdout);
    i = krait_kanban_create(0, "Add welcome screen");
    CHECK(i >= 0);
    CHECK(krait_kanban_new_project(0, i, "/tmp/krait-kanban-live-proj"));
    CHECK(krait_kanban_set_body(0, i,
        "Add a new file welcome.kry with a screen Welcome that draws a "
        "centered Button labeled Hello and a Text title above it."));
    CHECK(krait_kanban_ai_run(0, i));
    while(spins++ < 1200) {
        state = krait_kanban_ai_poll(0, i);
        if(state >= 2)
            break;
        usleep(250 * 1000);
    }
    CHECK(state == 2);
    if(state == 2) {
        CHECK(krait_kanban_proposal_count(0, i) > 0);
        CHECK(krait_kanban_apply(0, i) > 0);
        CHECK(access("/tmp/krait-kanban-live-proj/welcome.kry", F_OK) == 0);
        printf("ok (%s)\n", krait_kanban_card_status(0, i));
    }
    krait_kanban_delete(0, krait_kanban_count(0) - 1);
}

int
main(void)
{
    char home[256];
    const char *tmp = getenv("TMPDIR");

    snprintf(home, sizeof(home), "%s/krait-kanban-test-home.%d",
             tmp != NULL ? tmp : "/tmp", (int)getpid());
    setenv("HOME", home, 1);
    system("rm -rf /tmp/krait-kanban-test-proj /tmp/krait-kanban-live-proj");

    test_board_crud();
    test_project_scaffold_binding();
    test_ai_state_without_key();
    test_live_ai_gated();
    krait_kanban_shutdown();

    if(failures == 0)
        printf("kanban tests passed\n");
    return failures == 0 ? 0 : 1;
}
