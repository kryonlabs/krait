#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"
#include "app_host.h"
#include "kry_dylib.h"

#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
krait_recent_file(char *path, size_t path_size)
{
    const char *home = getenv("HOME");

    if(path_size == 0)
        return;
    if(home == NULL || home[0] == '\0')
        snprintf(path, path_size, ".kryon/recent_projects.txt");
    else
        snprintf(path, path_size, "%s/.kryon/recent_projects.txt", home);
}

int
krait_recent_count(void)
{
    FILE *file;
    char path[KRAIT_PATH_MAX];
    char line[KRAIT_PATH_MAX];
    int count = 0;

    krait_recent_file(path, sizeof(path));
    file = fopen(path, "r");
    if(file == NULL)
        return 0;
    while(count < KRAIT_MAX_RECENT && fgets(line, sizeof(line), file) != NULL) {
        size_t len = strlen(line);
        while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if(line[0] != '\0' && krait_path_exists(line))
            count++;
    }
    fclose(file);
    return count;
}

int
krait_recent_path(int index, char *out, int cap)
{
    FILE *file;
    char path[KRAIT_PATH_MAX];
    char line[KRAIT_PATH_MAX];
    int current = 0;

    if(out == NULL || cap <= 0)
        return 0;
    out[0] = '\0';
    if(index < 0)
        return 0;
    krait_recent_file(path, sizeof(path));
    file = fopen(path, "r");
    if(file == NULL)
        return 0;
    while(fgets(line, sizeof(line), file) != NULL) {
        size_t len = strlen(line);
        while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if(line[0] == '\0' || !krait_path_exists(line))
            continue;
        if(current == index) {
            snprintf(out, (size_t)cap, "%s", line);
            fclose(file);
            return 1;
        }
        current++;
    }
    fclose(file);
    return 0;
}

void
krait_recent_add(const char *path)
{
    char resolved[KRAIT_PATH_MAX];
    char items[KRAIT_MAX_RECENT][KRAIT_PATH_MAX];
    int count = 0;
    int found = -1;
    char recent_path[KRAIT_PATH_MAX];
    FILE *file;

    if(path == NULL || path[0] == '\0')
        return;
    if(realpath(path, resolved) == NULL)
        snprintf(resolved, sizeof(resolved), "%s", path);
    for(int i = 0; i < KRAIT_MAX_RECENT; i++) {
        if(!krait_recent_path(i, items[count], KRAIT_PATH_MAX))
            break;
        if(strcmp(items[count], resolved) == 0)
            found = count;
        count++;
    }
    if(found >= 0) {
        for(int i = found; i > 0; i--)
            snprintf(items[i], sizeof(items[i]), "%s", items[i - 1]);
    } else {
        if(count < KRAIT_MAX_RECENT)
            count++;
        for(int i = count - 1; i > 0; i--)
            snprintf(items[i], sizeof(items[i]), "%s", items[i - 1]);
    }
    snprintf(items[0], sizeof(items[0]), "%s", resolved);

    krait_recent_file(recent_path, sizeof(recent_path));
    krait_ensure_parent_dir(recent_path);
    file = fopen(recent_path, "w");
    if(file == NULL)
        return;
    for(int i = 0; i < count; i++)
        fprintf(file, "%s\n", items[i]);
    fclose(file);
}

void
krait_recent_remove(int index)
{
    char items[KRAIT_MAX_RECENT][KRAIT_PATH_MAX];
    int count = 0;
    char recent_path[KRAIT_PATH_MAX];
    FILE *file;

    if(index < 0)
        return;
    for(int i = 0; i < KRAIT_MAX_RECENT; i++) {
        if(!krait_recent_path(i, items[count], KRAIT_PATH_MAX))
            break;
        count++;
    }
    if(index >= count)
        return;
    for(int i = index; i + 1 < count; i++)
        snprintf(items[i], sizeof(items[i]), "%s", items[i + 1]);
    count--;

    krait_recent_file(recent_path, sizeof(recent_path));
    krait_ensure_parent_dir(recent_path);
    file = fopen(recent_path, "w");
    if(file == NULL)
        return;
    for(int i = 0; i < count; i++)
        fprintf(file, "%s\n", items[i]);
    fclose(file);
}

