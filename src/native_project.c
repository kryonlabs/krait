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

static int
krait_file_affects_project_mtime(const char *path)
{
    return path != NULL && path[0] != '\0';
}

static long
krait_project_source_mtime_dir(const char *dir, int depth)
{
    DIR *handle;
    struct dirent *entry;
    long newest = 0;

    if(dir == NULL || depth > KRAIT_MTIME_DEPTH)
        return 0;
    handle = opendir(dir);
    if(handle == NULL)
        return 0;

    while((entry = readdir(handle)) != NULL) {
        char path[KRAIT_PATH_MAX];
        struct stat st;

        if(strcmp(entry->d_name, ".") == 0 ||
           strcmp(entry->d_name, "..") == 0)
            continue;
        krait_join(path, sizeof(path), dir, entry->d_name);
        if(stat(path, &st) != 0)
            continue;
        if(S_ISDIR(st.st_mode)) {
            long child_mtime;

            if(krait_ignored_dir(entry->d_name))
                continue;
            child_mtime = krait_project_source_mtime_dir(path, depth + 1);
            if(child_mtime > newest)
                newest = child_mtime;
        } else if(S_ISREG(st.st_mode) && krait_file_affects_project_mtime(path)) {
            if((long)st.st_mtime > newest)
                newest = (long)st.st_mtime;
        }
    }
    closedir(handle);
    return newest;
}

long
krait_project_source_mtime(const char *path)
{
    struct stat st;

    if(path == NULL || path[0] == '\0' || stat(path, &st) != 0)
        return 0;
    if(S_ISDIR(st.st_mode))
        return krait_project_source_mtime_dir(path, 0);
    if(S_ISREG(st.st_mode) && krait_file_affects_project_mtime(path))
        return (long)st.st_mtime;
    return 0;
}

int
krait_project_has_make_target(const char *root, const char *target)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    const char *files[] = {"GNUmakefile", "Makefile", "makefile"};
    int found = 0;

    if(root == NULL || target == NULL)
        return 0;
    for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        krait_join(path, sizeof(path), root, files[i]);
        if(!krait_read_file_alloc(path, &text, &len))
            continue;
        for(char *p = text; *p != '\0';) {
            char *line = p;
            char *end = strchr(p, '\n');
            char *q;
            int at_end = 0;

            if(end == NULL) {
                end = p + strlen(p);
                at_end = 1;
            }
            *end = '\0';
            q = krait_trim(line);
            if(strncmp(q, target, strlen(target)) == 0 &&
               q[strlen(target)] == ':') {
                found = 1;
                break;
            }
            if(at_end)
                break;
            p = end + 1;
        }
        free(text);
        text = NULL;
        if(found)
            return 1;
    }
    return 0;
}

static int
krait_project_parse_quoted_value(const char *line, const char *key,
                                 char *out, size_t out_size)
{
    const char *p;
    const char *q;
    size_t n;

    if(line == NULL || key == NULL || out == NULL || out_size == 0)
        return 0;
    p = line;
    while(isspace((unsigned char)*p))
        p++;
    n = strlen(key);
    if(strncmp(p, key, n) != 0 ||
       (p[n] != '\0' && !isspace((unsigned char)p[n])))
        return 0;
    p += n;
    while(isspace((unsigned char)*p))
        p++;
    if(*p != '"')
        return 0;
    p++;
    q = strchr(p, '"');
    if(q == NULL)
        return 0;
    n = (size_t)(q - p);
    if(n >= out_size)
        n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

int
krait_project_preview_config(const char *root, char *live_path,
                             size_t live_path_size, char *build_command,
                             size_t build_command_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    int found = 0;

    if(live_path != NULL && live_path_size > 0)
        live_path[0] = '\0';
    if(build_command != NULL && build_command_size > 0)
        build_command[0] = '\0';
    if(root == NULL)
        return 0;
    krait_join(path, sizeof(path), root, "project.kryon");
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    for(char *line = text; *line != '\0';) {
        char *end = strchr(line, '\n');
        int at_end = 0;

        if(end == NULL) {
            end = line + strlen(line);
            at_end = 1;
        }
        *end = '\0';
        if(live_path != NULL &&
           krait_project_parse_quoted_value(line, "live", live_path,
                                            live_path_size))
            found = 1;
        if(build_command != NULL &&
           krait_project_parse_quoted_value(line, "build_live", build_command,
                                            build_command_size))
            found = 1;
        if(at_end)
            break;
        line = end + 1;
    }
    free(text);
    return found;
}

int
krait_project_preview_size(const char *root, int *out_w, int *out_h)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    int found = 0;

    if(out_w != NULL)
        *out_w = 0;
    if(out_h != NULL)
        *out_h = 0;
    if(root == NULL || out_w == NULL || out_h == NULL)
        return 0;
    krait_join(path, sizeof(path), root, "project.kryon");
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    for(char *line = text; *line != '\0';) {
        char *end = strchr(line, '\n');
        char *p;
        int at_end = 0;
        int w = 0;
        int h = 0;

        if(end == NULL) {
            end = line + strlen(line);
            at_end = 1;
        }
        *end = '\0';
        p = krait_trim(line);
        if(sscanf(p, "preview_size %d %d", &w, &h) == 2 &&
           w > 0 && h > 0) {
            *out_w = w;
            *out_h = h;
            found = 1;
            break;
        }
        if(at_end)
            break;
        line = end + 1;
    }
    free(text);
    return found;
}

static int
krait_add_search_result(SearchResult *results, int count, int cap,
                        const char *path, int line, const char *excerpt)
{
    SearchResult *result;

    if(results == NULL || count >= cap || path == NULL)
        return count;
    result = &results[count++];
    snprintf(result->path, sizeof(result->path), "%s", path);
    result->line = line;
    snprintf(result->excerpt, sizeof(result->excerpt), "%s",
             excerpt != NULL ? excerpt : "");
    krait_trim_line(result->excerpt);
    return count;
}

static int
krait_search_file(const char *abs_path, const char *rel_path,
                  const char *query, SearchResult *results, int count, int cap)
{
    FILE *file;
    char line[512];
    int line_no = 1;

    if(count >= cap || abs_path == NULL || rel_path == NULL ||
       query == NULL || query[0] == '\0')
        return count;
    if(strstr(rel_path, query) != NULL)
        count = krait_add_search_result(results, count, cap, rel_path, 1,
                                        "file match");
    if(count >= cap || !krait_file_is_text(rel_path))
        return count;
    file = fopen(abs_path, "r");
    if(file == NULL)
        return count;
    while(count < cap && fgets(line, sizeof(line), file) != NULL) {
        if(strstr(line, query) != NULL)
            count = krait_add_search_result(results, count, cap, rel_path,
                                            line_no, line);
        line_no++;
    }
    fclose(file);
    return count;
}

static int
krait_search_dir(const char *root, const char *dir, const char *query,
                 SearchResult *results, int count, int cap, int depth)
{
    DIR *handle;
    struct dirent *entry;
    char abs_path[KRAIT_PATH_MAX];
    char rel_path[KRAIT_PATH_MAX];
    struct stat st;
    size_t root_len;

    if(count >= cap || root == NULL || dir == NULL || depth > KRAIT_SEARCH_DEPTH)
        return count;
    handle = opendir(dir);
    if(handle == NULL)
        return count;
    root_len = strlen(root);
    while(count < cap && (entry = readdir(handle)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        krait_join(abs_path, sizeof(abs_path), dir, entry->d_name);
        if(stat(abs_path, &st) != 0)
            continue;
        if(root_len > 0 && strncmp(abs_path, root, root_len) == 0) {
            const char *rel = abs_path + root_len;
            if(*rel == '/')
                rel++;
            snprintf(rel_path, sizeof(rel_path), "%s", rel);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s", entry->d_name);
        }
        if(S_ISDIR(st.st_mode)) {
            if(!krait_ignored_dir(entry->d_name))
                count = krait_search_dir(root, abs_path, query, results,
                                         count, cap, depth + 1);
        } else if(S_ISREG(st.st_mode)) {
            count = krait_search_file(abs_path, rel_path, query, results,
                                      count, cap);
        }
    }
    closedir(handle);
    return count;
}

int
krait_search_project(const char *root, const char *query,
                     SearchResult *results, int cap)
{
    if(results == NULL || cap <= 0)
        return 0;
    if(root == NULL || root[0] == '\0' || query == NULL || query[0] == '\0')
        return 0;
    return krait_search_dir(root, root, query, results, 0, cap, 0);
}

