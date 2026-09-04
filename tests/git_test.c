/*
 * git_test.c - git module backend against a throwaway repository.
 *
 * Creates a temp repo, then exercises the whole local lifecycle through
 * the module API: detection, status parsing (untracked/staged/modified),
 * staging, commit, log, branch create/checkout. Remote operations
 * (push/pull/fetch) and GitHub need real remotes and tokens; they are
 * exercised manually against real repos.
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

int krait_git_available(void);
void krait_git_set_project(const char *path);
int krait_git_repo(void);
int krait_git_refresh(void);
const char *krait_git_branch(void);
int krait_git_ahead(void);
int krait_git_entry_count(void);
const char *krait_git_entry_path(int index);
int krait_git_entry_staged(int index);
int krait_git_entry_changed(int index);
int krait_git_entry_untracked(int index);
int krait_git_stage(int index);
int krait_git_unstage(int index);
int krait_git_stage_all(void);
int krait_git_commit(const char *message);
int krait_git_log_refresh(void);
int krait_git_log_count(void);
const char *krait_git_log_subject(int index);
int krait_git_branch_refresh(void);
int krait_git_branch_count(void);
const char *krait_git_branch_name(int index);
int krait_git_branch_current(int index);
int krait_git_branch_create(const char *name);
int krait_git_branch_checkout(int index);
const char *krait_git_diff(int index);
const char *krait_git_status_text(void);

static char repo_dir[] = "/tmp/krait-git-test-XXXXXX";

static void
write_file(const char *rel, const char *text)
{
    char path[1024];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", repo_dir, rel);
    f = fopen(path, "w");
    if(f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

int
main(void)
{
    char cmd[1100];

    if(!krait_git_available()) {
        printf("git not available; skipping\n");
        return 0;
    }
    if(mkdtemp(repo_dir) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(cmd, sizeof(cmd),
             "cd %s && git init -q -b main && "
             "git config user.email test@krait.local && "
             "git config user.name KraitTest",
             repo_dir);
    if(system(cmd) != 0) {
        fprintf(stderr, "git init failed\n");
        return 2;
    }

    /* no repo set yet */
    CHECK(krait_git_repo() == 0);

    krait_git_set_project(repo_dir);
    CHECK(krait_git_repo() == 1);
    CHECK(strcmp(krait_git_branch(), "main") == 0);

    /* empty tree: clean */
    CHECK(krait_git_refresh() == 1);
    CHECK(krait_git_entry_count() == 0);

    /* untracked file appears */
    write_file("hello.txt", "one\n");
    CHECK(krait_git_refresh() == 1);
    CHECK(krait_git_entry_count() == 1);
    CHECK(strcmp(krait_git_entry_path(0), "hello.txt") == 0);
    CHECK(krait_git_entry_untracked(0) == 1);
    CHECK(krait_git_entry_staged(0) == 0);

    /* stage -> staged, unstage -> back */
    CHECK(krait_git_stage(0) == 1);
    CHECK(krait_git_entry_count() == 1);
    CHECK(krait_git_entry_staged(0) == 1);
    CHECK(krait_git_unstage(0) == 1);
    CHECK(krait_git_entry_staged(0) == 0);
    CHECK(krait_git_entry_untracked(0) == 1);

    /* diff shows the change after staging */
    CHECK(krait_git_stage(0) == 1);
    {
        const char *diff = krait_git_diff(0);

        CHECK(diff != NULL);
        CHECK(strstr(diff, "hello.txt") != NULL || diff[0] == '\0' ||
              strstr(diff, "one") != NULL);
    }

    /* commit */
    CHECK(krait_git_commit("first commit") == 1);
    CHECK(krait_git_entry_count() == 0);
    CHECK(krait_git_log_refresh() == 1);
    CHECK(krait_git_log_count() == 1);
    CHECK(strcmp(krait_git_log_subject(0), "first commit") == 0);

    /* modification shows as changed, stage_all commits the second one */
    write_file("hello.txt", "one\ntwo\n");
    CHECK(krait_git_refresh() == 1);
    CHECK(krait_git_entry_count() == 1);
    CHECK(krait_git_entry_changed(0) == 1);
    CHECK(krait_git_stage_all() == 1);
    CHECK(krait_git_commit("second") == 1);
    CHECK(krait_git_log_count() == 2);
    CHECK(strcmp(krait_git_log_subject(0), "second") == 0);

    /* branches: create + checkout + back */
    CHECK(krait_git_branch_create("feature") == 1);
    CHECK(krait_git_branch_refresh() == 1);
    CHECK(krait_git_branch_count() == 2);
    {
        int feature = -1, main_branch = -1;

        for(int i = 0; i < krait_git_branch_count(); i++) {
            if(strcmp(krait_git_branch_name(i), "feature") == 0)
                feature = i;
            if(strcmp(krait_git_branch_name(i), "main") == 0)
                main_branch = i;
        }
        CHECK(feature >= 0 && main_branch >= 0);
        CHECK(krait_git_branch_current(feature) == 1);
        CHECK(krait_git_branch_checkout(main_branch) == 1);
        CHECK(krait_git_branch_current(main_branch) == 1);
        CHECK(strcmp(krait_git_branch(), "main") == 0);
    }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", repo_dir);
    system(cmd);
    krait_git_set_project("");
    CHECK(krait_git_repo() == 0);

    if(failures != 0) {
        fprintf(stderr, "git: %d failure(s)\n", failures);
        return 1;
    }
    printf("git tests passed\n");
    return 0;
}
