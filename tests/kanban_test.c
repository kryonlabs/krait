/*
 * kanban_test.c - board semantics against the real filesystem.
 *
 * The board IS files under ~/.kryon/krait/kanban, so the test redirects
 * HOME to a throwaway directory and exercises the native store: column
 * creation, card create/read/update, move-as-rename, project scaffold
 * binding, delete, and the no-key AI state. A live provider round trip runs
 * only when KRAIT_KANBAN_LIVE=1 and the selected provider has an API key.
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
test_card_identity_and_long_body(void)
{
    char ids[4][128];
    char original[4096], collision[4096];
    char *body = malloc(20001);
    char *loaded = NULL;
    long length;
    int col, i;

    CHECK(body != NULL);
    if(body == NULL)
        return;
    memset(body, 'x', 20000);
    body[20000] = 0;
    for(col = 0; col < 4; col++) {
        i = krait_kanban_create(col, "Independent task");
        CHECK(i >= 0);
        snprintf(ids[col], sizeof(ids[col]), "%s", krait_kanban_card_id(col, i));
        for(int j = 0; j < col; j++)
            CHECK(strcmp(ids[j], ids[col]) != 0);
    }
    CHECK(krait_kanban_set_body(0, 0, body));
    krait_kanban_rescan();
    CHECK(strcmp(krait_kanban_card_body(0, 0), body) == 0);
    free(body);

    /* A duplicate ID from an older store cannot overwrite its neighbour. */
    snprintf(original, sizeof(original), "%s", krait_kanban_card_path(0, 0));
    snprintf(collision, sizeof(collision), "%s/.kryon/krait/kanban/doing/%s.txt",
             getenv("HOME"), ids[0]);
    CHECK(krait_write_text_file(collision, "Keep this task\n\n"));
    CHECK(!krait_kanban_move(0, 0, 1));
    CHECK(access(original, F_OK) == 0);
    CHECK(krait_read_file_alloc(collision, &loaded, &length));
    CHECK(loaded != NULL && strcmp(loaded, "Keep this task\n\n") == 0);
    free(loaded);
    unlink(collision);
    krait_kanban_rescan();
    for(col = 0; col < 4; col++)
        CHECK(krait_kanban_delete(col, 0));
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

/* Live loop: card -> model -> proposal -> k2c+cc gates -> apply. Opt-in
 * because it spends a real API call and needs the compiler toolchain.
 * The second card asks for constructs Kry rejects (let, comments) so a
 * first attempt that complies fails validation and the retry loop has
 * to correct it; either way the loop must end on compilable code. */
static int
live_run_card(const char *title, const char *body, int spins_max)
{
    int i = krait_kanban_create(0, title);
    int state = 0;
    int spins = 0;

    CHECK(i >= 0);
    CHECK(krait_kanban_new_project(0, i, "/tmp/krait-kanban-live-proj"));
    CHECK(krait_kanban_set_body(0, i, body));
    CHECK(krait_kanban_ai_run(0, i));
    while(spins++ < spins_max) {
        state = krait_kanban_ai_poll(0, i);
        if(state >= 2)
            break;
        usleep(250 * 1000);
    }
    if(state != 2)
        fprintf(stderr, "live loop '%s' ended in state %d: %s\n", title,
                state, krait_kanban_card_status(0, i));
    return state;
}

static void
test_live_ai_gated(void)
{
    const char *optin = getenv("KRAIT_KANBAN_LIVE");
    int state;

    if(optin == NULL || optin[0] == '\0' || !krait_ai_configured())
        return;
    printf("live kanban AI round trip... ");
    fflush(stdout);
    state = live_run_card("Add welcome screen",
        "Add a new file welcome.kry with a screen Welcome that draws a "
        "centered Button labeled Hello and a Text title above it.", 1600);
    CHECK(state == 2);
    if(state == 2) {
        CHECK(krait_kanban_proposal_count(0, krait_kanban_count(0) - 1) > 0);
        CHECK(krait_kanban_apply(0, krait_kanban_count(0) - 1) > 0);
        CHECK(access("/tmp/krait-kanban-live-proj/welcome.kry", F_OK) == 0);
        printf("ok (%s)\n",
               krait_kanban_card_status(0, krait_kanban_count(0) - 1));
    }
    krait_kanban_delete(0, krait_kanban_count(0) - 1);

    printf("live kanban AI retry loop... ");
    fflush(stdout);
    state = live_run_card("Force invalid Kry",
        "In a new file broken.kry write a screen Broken that uses "
        "`let x := 5` and a `// comment` line and draws a Button labeled "
        "Broken. The let and the comment must appear in the file.", 2400);
    CHECK(state == 2);
    if(state == 2) {
        int last = krait_kanban_count(0) - 1;

        CHECK(krait_kanban_apply(0, last) > 0);
        CHECK(access("/tmp/krait-kanban-live-proj/broken.kry", F_OK) == 0);
        printf("ok (%s)\n", krait_kanban_card_status(0, last));
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
    test_card_identity_and_long_body();
    test_project_scaffold_binding();
    test_ai_state_without_key();
    test_live_ai_gated();
    krait_kanban_shutdown();

    if(failures == 0)
        printf("kanban tests passed\n");
    return failures == 0 ? 0 : 1;
}
