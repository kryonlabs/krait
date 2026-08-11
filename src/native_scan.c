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

int
krait_add_screen(ScreenEntry *out, int count, int cap, const char *name,
                 const char *rel_path, int line, int insert_offset)
{
    ScreenEntry *entry;
    char id_src[KRAIT_PATH_MAX + 160];

    if(out == NULL || count >= cap || name == NULL || rel_path == NULL)
        return count;
    entry = &out[count++];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    snprintf(entry->path, sizeof(entry->path), "%s", rel_path);
    entry->line = line;
    entry->insert_offset = insert_offset;
    snprintf(id_src, sizeof(id_src), "%s:%s", rel_path, name);
    entry->id = krait_tree_id(id_src);
    return count;
}

static int
krait_scan_screen_file(const char *abs_path, const char *rel_path,
                       ScreenEntry *out, int count, int cap)
{
    char *text;
    long len;
    int line = 1;

    if(count >= cap || abs_path == NULL || rel_path == NULL)
        return count;
    if(!krait_read_file_alloc(abs_path, &text, &len))
        return count;
    for(long i = 0; i < len && count < cap; i++) {
        if(text[i] == '\n') {
            line++;
            continue;
        }
        if((i > 0 && krait_ident_char(text[i - 1])) ||
           strncmp(text + i, "screen", 6) != 0 ||
           (i + 6 < len && krait_ident_char(text[i + 6])))
            continue;

        long p = i + 6;
        char name[128];
        int name_len = 0;
        int screen_line = line;

        while(p < len && isspace((unsigned char)text[p])) {
            if(text[p] == '\n')
                line++;
            p++;
        }
        if(p >= len || !krait_ident_start(text[p]))
            continue;
        while(p < len && krait_ident_char(text[p]) &&
              name_len < (int)sizeof(name) - 1)
            name[name_len++] = text[p++];
        name[name_len] = '\0';
        while(p < len && text[p] != '{' && text[p] != '\n')
            p++;
        if(p >= len || text[p] != '{')
            continue;

        int depth = 1;
        p++;
        while(p < len && depth > 0) {
            if(text[p] == '{')
                depth++;
            else if(text[p] == '}')
                depth--;
            if(depth == 0) {
                count = krait_add_screen(out, count, cap, name, rel_path,
                                         screen_line, (int)p);
                break;
            }
            p++;
        }
        i = p;
    }
    free(text);
    return count;
}

static int
krait_scan_screens_dir(const char *root, const char *dir, ScreenEntry *out,
                       int count, int cap, int depth)
{
    DIR *handle;
    struct dirent *entry;
    char abs_path[KRAIT_PATH_MAX];
    char rel_path[KRAIT_PATH_MAX];
    struct stat st;
    size_t root_len;

    if(count >= cap || root == NULL || dir == NULL || depth > KRAIT_SCREEN_DEPTH)
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
                count = krait_scan_screens_dir(root, abs_path, out, count,
                                               cap, depth + 1);
        } else if(S_ISREG(st.st_mode) && krait_path_has_suffix(rel_path, ".kry")) {
            count = krait_scan_screen_file(abs_path, rel_path, out, count, cap);
        }
    }
    closedir(handle);
    return count;
}

const char *
krait_scan_project_quoted(const char *p, char *out, size_t out_size)
{
    const char *q;
    size_t n;

    if(out != NULL && out_size > 0)
        out[0] = '\0';
    if(p == NULL || out == NULL || out_size == 0)
        return NULL;
    while(isspace((unsigned char)*p))
        p++;
    if(*p != '"')
        return NULL;
    p++;
    q = strchr(p, '"');
    if(q == NULL)
        return NULL;
    n = (size_t)(q - p);
    if(n >= out_size)
        n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return q + 1;
}

int
krait_scan_project_scenes(const char *root, ScreenEntry *out, int count, int cap)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;

    if(count >= cap || root == NULL)
        return count;
    krait_join(path, sizeof(path), root, "project.kryon");
    if(!krait_read_file_alloc(path, &text, &len))
        return count;
    for(char *line = text; *line != '\0' && count < cap;) {
        char *end = strchr(line, '\n');
        char *p;
        char id[128];
        char group[128];
        char title[128];
        char source[KRAIT_PATH_MAX];
        int at_end = 0;

        if(end == NULL) {
            end = line + strlen(line);
            at_end = 1;
        }
        *end = '\0';
        p = krait_trim(line);
        if(strncmp(p, "preview_scene", 13) == 0 &&
           isspace((unsigned char)p[13])) {
            p = (char *)krait_scan_project_quoted(p + 13, id, sizeof(id));
            p = (char *)krait_scan_project_quoted(p, group, sizeof(group));
            p = (char *)krait_scan_project_quoted(p, title, sizeof(title));
            p = (char *)krait_scan_project_quoted(p, source, sizeof(source));
            if(p != NULL && source[0] != '\0')
                count = krait_add_screen(out, count, cap,
                                         title[0] != '\0' ? title : id,
                                         source, 0, 0);
        }
        if(at_end)
            break;
        line = end + 1;
    }
    free(text);
    return count;
}

int
krait_scan_screens(const char *root, ScreenEntry *out, int cap)
{
    int count;

    if(out == NULL || cap <= 0)
        return 0;
    if(root == NULL || root[0] == '\0')
        return 0;
    count = krait_scan_project_scenes(root, out, 0, cap);
    if(count > 0)
        return count;
    return krait_scan_screens_dir(root, root, out, 0, cap, 0);
}

