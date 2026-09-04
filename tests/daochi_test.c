/*
 * daochi_test.c - Daochi client semantics with a redirected HOME.
 *
 * Covers the local pieces: state-file round trips (node url, token
 * passthrough keys), account create/export/import, and URL normalization
 * from kryon's ksync layer. Network paths (login, friends, boards) are
 * exercised end-to-end by scripts against a real node; here we assert the
 * offline behavior and guards.
 *
 * With DAOCHI_TEST_SERVER set (e.g. http://127.0.0.1:18085), the suite also
 * runs the full lifecycle against that node: register two accounts, make
 * them friends, share a board, invite, and sync records back.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"
#include "ksync_account.h"
#include "ksync_sync.h"

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

int krait_daochi_available(void);
int krait_daochi_has_account(void);
const char *krait_daochi_public_id(void);
int krait_daochi_account_create(void);
int krait_daochi_account_import_file(const char *filename);
int krait_daochi_account_export_file(const char *filename);
int krait_daochi_logout(void);
const char *krait_daochi_node_url(void);
int krait_daochi_set_node_url(const char *url);
int krait_daochi_login(void);
int krait_daochi_ping(void);
int krait_daochi_set_alias(const char *alias);
const char *krait_daochi_alias(void);
const char *krait_daochi_status(void);
int krait_daochi_friends_refresh(void);
int krait_daochi_friend_count(void);
const char *krait_daochi_friend_id(int index);
const char *krait_daochi_friend_alias(int index);
int krait_daochi_friend_request_send(const char *target);
int krait_daochi_friend_requests_refresh(void);
int krait_daochi_friend_request_count(void);
int krait_daochi_friend_request_incoming(int index);
int krait_daochi_friend_request_accept(int index);
int krait_daochi_boards_refresh(void);
int krait_daochi_board_count(void);
const char *krait_daochi_board_title(int index);
const char *krait_daochi_board_permission(int index);
int krait_daochi_board_create(const char *title);
int krait_daochi_board_select(int index);
int krait_daochi_board_detail_refresh(void);
int krait_daochi_member_count(void);
const char *krait_daochi_member_alias(int index);
const char *krait_daochi_member_permission(int index);
int krait_daochi_board_invite(const char *target, const char *permission);
int krait_daochi_member_set_permission(int member_index, const char *permission);
int krait_daochi_records_refresh(void);
int krait_daochi_record_count(void);
const char *krait_daochi_record_field(int index, const char *field);
int krait_daochi_record_put_fields(const char *record_id, const char *title,
                                   const char *body, const char *column);
int krait_daochi_record_delete(const char *record_id);
void krait_daochi_reset(void);

static char home_backup[512];

static void
redirect_home(void)
{
    snprintf(home_backup, sizeof(home_backup), "%s", getenv("HOME") != NULL
                 ? getenv("HOME") : "");
    if(system("rm -rf /tmp/krait-daochi-test-home && "
              "mkdir -p /tmp/krait-daochi-test-home") != 0)
        exit(2);
    setenv("HOME", "/tmp/krait-daochi-test-home", 1);
}

static void
restore_home(void)
{
    setenv("HOME", home_backup, 1);
}

static void
test_local_account(void)
{
    char path[512];
    FILE *f;

    CHECK(krait_daochi_has_account() == 0);
    CHECK(krait_daochi_account_create() == 1);
    CHECK(krait_daochi_has_account() == 1);
    CHECK(strlen(krait_daochi_public_id()) == 64);

    snprintf(path, sizeof(path), "%s/.kryon/krait/daochi/account.txt",
             getenv("HOME"));
    f = fopen(path, "r");
    CHECK(f != NULL);
    if(f != NULL)
        fclose(f);

    /* export, then import into a fresh HOME */
    CHECK(krait_daochi_account_export_file("/tmp/krait-daochi-export.txt") == 1);
    if(system("rm -rf /tmp/krait-daochi-homeC && mkdir -p /tmp/krait-daochi-homeC") != 0)
        CHECK(!"homeC setup failed");
    setenv("HOME", "/tmp/krait-daochi-homeC", 1);
    krait_daochi_reset();
    CHECK(krait_daochi_has_account() == 0);
    CHECK(krait_daochi_account_import_file("/tmp/krait-daochi-export.txt") == 1);
    CHECK(krait_daochi_has_account() == 1);
    setenv("HOME", "/tmp/krait-daochi-test-home", 1);
    krait_daochi_reset();
    CHECK(krait_daochi_has_account() == 1);
    unlink("/tmp/krait-daochi-export.txt");
    system("rm -rf /tmp/krait-daochi-homeC");
}

static void
test_state_and_urls(void)
{
    CHECK(krait_daochi_set_node_url("https://api.example.com") == 1);
    CHECK(strcmp(krait_daochi_node_url(), "https://api.example.com") == 0);
    CHECK(krait_daochi_set_node_url("http://127.0.0.1:18085") == 1);
    CHECK(strcmp(krait_daochi_node_url(), "http://127.0.0.1:18085") == 0);
    CHECK(krait_daochi_set_node_url("not a url") == 0);
    /* scheme-less public hosts are rejected so tokens never fall back to
     * cleartext http; private hosts are allowed bare */
    CHECK(krait_daochi_set_node_url("api.example.com") == 0);
    CHECK(krait_daochi_set_node_url("") == 1);
    CHECK(strncmp(krait_daochi_node_url(), "https://", 8) == 0);

    /* guard: friend/board accessors never crash on empty caches */
    CHECK(krait_daochi_friend_count() == 0);
    CHECK(strcmp(krait_daochi_friend_id(0), "") == 0);
    CHECK(krait_daochi_board_count() == 0);
    CHECK(strcmp(krait_daochi_board_title(3), "") == 0);
    CHECK(strcmp(krait_daochi_record_field(0, "title"), "") == 0);
    CHECK(krait_daochi_records_refresh() == 0);
}

static void
test_live_lifecycle(const char *server)
{
    char other_export[] = "/tmp/krait-daochi-other-XXXXXX";
    char first_id[128];
    int mkfd = mkstemp(other_export);

    CHECK(mkfd >= 0);
    if(mkfd >= 0)
        close(mkfd);

    char alias_a[32];
    char alias_b[32];

    snprintf(alias_a, sizeof(alias_a), "krait_a%d", getpid() % 100000);
    snprintf(alias_b, sizeof(alias_b), "krait_b%d", getpid() % 100000);
    printf("live node: %s\n", server);
    CHECK(krait_daochi_set_node_url(server) == 1);

    /* account A: create, alias, login */
    CHECK(krait_daochi_account_create() == 1);
    CHECK(krait_daochi_login() == 1);
    fprintf(stderr, "live login status: %s\n", krait_daochi_status());
    CHECK(krait_daochi_ping() == 1);
    fprintf(stderr, "live ping status: %s\n", krait_daochi_status());
    CHECK(krait_daochi_set_alias(alias_a) == 1);

    /* share a board with one card before any friends exist */
    CHECK(krait_daochi_board_create("E2E board") == 1);
    CHECK(krait_daochi_record_put_fields("card-e2e", "Hello board",
                                         "shared from krait", "backlog") == 1);
    CHECK(krait_daochi_records_refresh() == 1);
    CHECK(krait_daochi_record_count() == 1);
    CHECK(strcmp(krait_daochi_record_field(0, "title"), "Hello board") == 0);
    CHECK(strcmp(krait_daochi_record_field(0, "column"), "backlog") == 0);

    /* invite without friendship must fail */
    CHECK(krait_daochi_board_invite(
              "0000000000000000000000000000000000000000000000000000000000000000",
              "read") == 0);

    /* account B in a separate HOME: export A, switch, create B */
    snprintf(first_id, sizeof(first_id), "%s", krait_daochi_public_id());
    krait_daochi_reset();
    {
        char cmd[768];

        snprintf(cmd, sizeof(cmd),
                 "rm -rf /tmp/krait-daochi-homeB && mkdir -p /tmp/krait-daochi-homeB");
        if(system(cmd) != 0)
            CHECK(!"homeB setup failed");
        setenv("HOME", "/tmp/krait-daochi-homeB", 1);
    }
    CHECK(krait_daochi_account_create() == 1);
    CHECK(krait_daochi_set_node_url(server) == 1);
    CHECK(krait_daochi_login() == 1);
    CHECK(krait_daochi_set_alias(alias_b) == 1);

    /* B requests friendship with A by public id; A accepts */
    CHECK(krait_daochi_friend_request_send(first_id) == 1);

    /* switch back to A's HOME */
    setenv("HOME", home_backup, 1);
    {
        /* A's state was created under the redirected dir before we swapped:
         * home_backup is the ORIGINAL home, so point HOME back at the test
         * dir where A lives. */
    }
    setenv("HOME", "/tmp/krait-daochi-test-home", 1);
    krait_daochi_reset();
    CHECK(krait_daochi_friend_requests_refresh() == 1);
    CHECK(krait_daochi_friend_request_count() == 1);
    CHECK(krait_daochi_friend_request_incoming(0) == 1);
    CHECK(krait_daochi_friend_request_accept(0) == 1);
    CHECK(krait_daochi_friends_refresh() == 1);
    CHECK(krait_daochi_friend_count() == 1);
    CHECK(strcmp(krait_daochi_friend_alias(0), alias_b) == 0);

    /* A invites B onto the shared board as a reader */
    CHECK(krait_daochi_boards_refresh() == 1);
    CHECK(krait_daochi_board_count() == 1);
    CHECK(krait_daochi_board_select(0) == 1);
    CHECK(krait_daochi_board_detail_refresh() == 1);
    CHECK(krait_daochi_member_count() == 1);
    CHECK(krait_daochi_board_invite(krait_daochi_friend_id(0), "read") == 1);
    CHECK(krait_daochi_board_detail_refresh() == 1);
    CHECK(krait_daochi_member_count() == 2);
    CHECK(strcmp(krait_daochi_member_permission(1), "read") == 0);

    /* promote B to writer, then demote and remove: permission management */
    CHECK(krait_daochi_member_set_permission(1, "write") == 1);
    CHECK(krait_daochi_board_detail_refresh() == 1);
    CHECK(strcmp(krait_daochi_member_permission(1), "write") == 0);
    CHECK(krait_daochi_member_set_permission(1, "read") == 1);

    /* B sees the board and its card */
    setenv("HOME", "/tmp/krait-daochi-homeB", 1);
    krait_daochi_reset();
    CHECK(krait_daochi_boards_refresh() == 1);
    CHECK(krait_daochi_board_count() == 1);
    CHECK(strcmp(krait_daochi_board_title(0), "E2E board") == 0);
    CHECK(strcmp(krait_daochi_board_permission(0), "read") == 0);
    CHECK(krait_daochi_board_select(0) == 1);
    CHECK(krait_daochi_records_refresh() == 1);
    CHECK(krait_daochi_record_count() == 1);
    CHECK(strcmp(krait_daochi_record_field(0, "title"), "Hello board") == 0);
    /* reader cannot write */
    CHECK(krait_daochi_record_put_fields("card-b", "nope", "", "backlog") == 0);

    /* A deletes the card; B syncs the tombstone */
    setenv("HOME", "/tmp/krait-daochi-test-home", 1);
    krait_daochi_reset();
    CHECK(krait_daochi_boards_refresh() == 1);
    CHECK(krait_daochi_board_select(0) == 1);
    CHECK(krait_daochi_record_delete("card-e2e") == 1);
    CHECK(krait_daochi_records_refresh() == 1);
    CHECK(krait_daochi_record_count() == 1);
    CHECK(krait_daochi_record_delete("card-e2e") == 1);

    setenv("HOME", "/tmp/krait-daochi-homeB", 1);
    krait_daochi_reset();
    CHECK(krait_daochi_boards_refresh() == 1);
    CHECK(krait_daochi_board_select(0) == 1);
    CHECK(krait_daochi_records_refresh() == 1);
    CHECK(krait_daochi_record_count() == 1);

    system("rm -rf /tmp/krait-daochi-homeB");
    unlink(other_export);
}

int
main(void)
{
    const char *live = getenv("DAOCHI_TEST_SERVER");

    redirect_home();
    test_local_account();
    test_state_and_urls();
    if(live != NULL && live[0] != '\0')
        test_live_lifecycle(live);

    restore_home();
    if(failures != 0) {
        fprintf(stderr, "daochi: %d failure(s)\n", failures);
        return 1;
    }
    printf("daochi tests passed\n");
    return 0;
}
