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
#include <regex.h>
#include <fnmatch.h>
#include "kry_json.h"

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

static void
krait_trim_line(char *text)
{
    char *start;
    char *end;

    if(text == NULL)
        return;
    start = text;
    while(*start == ' ' || *start == '\t')
        start++;
    if(start != text)
        memmove(text, start, strlen(start) + 1);
    end = text + strlen(text);
    while(end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                         end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
}

static int
krait_add_search_result(SearchResult *results, int count, int cap,
                        const char *path, int line, const char *excerpt)
{
    SearchResult *result;

    if(results == NULL || count >= cap || path == NULL || strlen(path) >= sizeof(results[0].path))
        return count;
    result = &results[count++];
    snprintf(result->path, sizeof(result->path), "%s", path);
    result->line = line;
    snprintf(result->excerpt, sizeof(result->excerpt), "%s",
             excerpt != NULL ? excerpt : "");
    krait_trim_line(result->excerpt);
    return count;
}

typedef struct {
    const char *query;
    const char *exclude;
    int regex, match_case, files_only;
    regex_t compiled;
} SearchOptions;

static int
search_match_offset(const char *text, const SearchOptions *options)
{
    if(options->regex) {
        regmatch_t match;
        return regexec(&options->compiled, text, 1, &match, 0) == 0 ? (int)match.rm_so : -1;
    }
    if(options->match_case) {
        const char *found = strstr(text, options->query);
        return found ? (int)(found - text) : -1;
    }
    size_t n = strlen(options->query);
    for(const char *p = text; *p; p++) {
        size_t i = 0;
        while(i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)options->query[i])) i++;
        if(i == n) return (int)(p - text);
    }
    return -1;
}

static int
krait_search_file(const char *abs_path, const char *rel_path,
                  const SearchOptions *options, SearchResult *results, int count, int cap)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    int line_no = 1;

    if(count >= cap || abs_path == NULL || rel_path == NULL ||
       options == NULL || options->query[0] == '\0')
        return count;
    if(options->files_only && search_match_offset(rel_path, options) >= 0)
        count = krait_add_search_result(results, count, cap, rel_path, 1,
                                        "file match");
    if(options->files_only || count >= cap)
        return count;
    file = fopen(abs_path, "r");
    if(file == NULL)
        return count;
    unsigned char probe[8192];
    size_t sampled = fread(probe, 1, sizeof(probe), file);
    if(memchr(probe, 0, sampled) != NULL) { fclose(file); return count; }
    rewind(file);
    ssize_t bytes;
    while(count < cap && (bytes = getline(&line, &capacity, file)) >= 0) {
        if(memchr(line, 0, (size_t)bytes) != NULL) break;
        int offset = search_match_offset(line, options);
        if(offset >= 0)
            count = krait_add_search_result(results, count, cap, rel_path,
                                            line_no, line + (offset > 40 ? offset - 40 : 0));
        line_no++;
    }
    free(line);
    fclose(file);
    return count;
}

static int
krait_search_dir(const char *root, const char *dir, const SearchOptions *options,
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
        if(lstat(abs_path, &st) != 0)
            continue;
        if(root_len > 0 && strncmp(abs_path, root, root_len) == 0) {
            const char *rel = abs_path + root_len;
            if(*rel == '/')
                rel++;
            snprintf(rel_path, sizeof(rel_path), "%s", rel);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s", entry->d_name);
        }
        if(options->exclude && *options->exclude && fnmatch(options->exclude, rel_path, 0) == 0)
            continue;
        if(S_ISDIR(st.st_mode)) {
            if(!krait_ignored_dir(entry->d_name))
                count = krait_search_dir(root, abs_path, options, results,
                                         count, cap, depth + 1);
        } else if(S_ISREG(st.st_mode)) {
            count = krait_search_file(abs_path, rel_path, options, results,
                                      count, cap);
        }
    }
    closedir(handle);
    return count;
}

int
krait_search_project_options(const char *root, const char *query, int regex,
    int match_case, int files_only, const char *exclude, SearchResult *results, int cap)
{
    if(results == NULL || cap <= 0 || root == NULL || !*root || query == NULL || !*query)
        return 0;
    SearchOptions options = {0};
    options.query = query; options.exclude = exclude;
    options.regex = regex; options.match_case = match_case; options.files_only = files_only;
    if(regex && regcomp(&options.compiled, query, REG_EXTENDED | REG_NEWLINE | (match_case ? 0 : REG_ICASE)) != 0)
        return -1;
    int count = krait_search_dir(root, root, &options, results, 0, cap, 0);
    if(regex) regfree(&options.compiled);
    return count;
}

int
krait_search_project(const char *root, const char *query, SearchResult *results, int cap)
{
    return krait_search_project_options(root, query, 0, 1, 0, NULL, results, cap);
}

/* Replacement previews retain complete before-images until explicitly applied. */
typedef struct { char path[512]; char *before, *after; int matches; } Replacement;
static Replacement replacements[64];
static int replacement_count;
static char replacement_root[KRAIT_PATH_MAX];
static char replacement_status[512];

void
krait_replace_clear(void)
{
    for(int i = 0; i < replacement_count; i++) {
        free(replacements[i].before); free(replacements[i].after);
    }
    memset(replacements, 0, sizeof(replacements));
    replacement_count = 0;
    replacement_root[0] = 0;
}

const char *krait_replace_status(void) { return replacement_status; }
int krait_replace_count(void) { return replacement_count; }
const char *krait_replace_path(int i) { return i >= 0 && i < replacement_count ? replacements[i].path : ""; }
const char *krait_replace_content(int i, int after) {
    return i >= 0 && i < replacement_count ? (after ? replacements[i].after : replacements[i].before) : "";
}

static int
replacement_append(char **out, size_t *used, const char *text, size_t len)
{
    if(len > 16 * 1024 * 1024 || *used > 16 * 1024 * 1024 - len) return 0;
    char *next = realloc(*out, *used + len + 1);
    if(next == NULL) return 0;
    *out = next;
    memcpy(next + *used, text, len); *used += len; next[*used] = 0;
    return 1;
}

char *
krait_replace_text(const char *text, const char *query, const char *replacement,
                    int regex, int match_case, int *matches)
{
    SearchOptions options = {0};
    char *out = NULL;
    size_t used = 0, offset = 0, length;
    int ok = 1;
    int ignored_matches;
    if(!matches) matches = &ignored_matches;
    *matches = 0;
    if(text == NULL || query == NULL || !*query || replacement == NULL) return NULL;
    options.query = query; options.regex = regex; options.match_case = match_case;
    if(regex && regcomp(&options.compiled, query, REG_EXTENDED | REG_NEWLINE | (match_case ? 0 : REG_ICASE))) return NULL;
    length = strlen(text);
    while(offset <= length && ok) {
        regmatch_t groups[10];
        int start, end;
        if(regex) {
            int flags = offset && text[offset - 1] != '\n' ? REG_NOTBOL : 0;
            if(regexec(&options.compiled, text + offset, 10, groups, flags)) break;
            start = (int)groups[0].rm_so; end = (int)groups[0].rm_eo;
        } else {
            start = search_match_offset(text + offset, &options);
            if(start < 0) break;
            end = start + (int)strlen(query);
        }
        ok = replacement_append(&out, &used, text + offset, (size_t)start);
        for(size_t i = 0; replacement[i] && ok; i++) {
            if(regex && replacement[i] == '$' && replacement[i + 1] == '$') {
                ok = replacement_append(&out, &used, "$", 1); i++;
            } else if(regex && replacement[i] == '$' && replacement[i + 1] >= '0' && replacement[i + 1] <= '9') {
                int group = replacement[++i] - '0';
                if(groups[group].rm_so >= 0)
                    ok = replacement_append(&out, &used, text + offset + groups[group].rm_so,
                        (size_t)(groups[group].rm_eo - groups[group].rm_so));
            } else ok = replacement_append(&out, &used, replacement + i, 1);
        }
        (*matches)++;
        offset += (size_t)end;
        if(start == end) {
            if(offset == length) break;
            size_t bytes = 1;
            while(offset + bytes < length && ((unsigned char)text[offset + bytes] & 0xc0) == 0x80) bytes++;
            ok = ok && replacement_append(&out, &used, text + offset, bytes);
            offset += bytes;
        }
    }
    ok = ok && replacement_append(&out, &used, text + offset, length - offset);
    if(regex) regfree(&options.compiled);
    if(!ok) { free(out); return NULL; }
    return out;
}

static int
replacement_path(const char *root, const char *relative, char full[KRAIT_PATH_MAX * 2])
{
    if(!relative || !*relative || *relative == '/') return 0;
    if(snprintf(full, KRAIT_PATH_MAX * 2, "%s/%s", root, relative) >= KRAIT_PATH_MAX * 2) return 0;
    char walk[KRAIT_PATH_MAX * 2];
    snprintf(walk, sizeof(walk), "%s", full);
    char *part = walk + strlen(root) + 1;
    for(char *p = part;; p++) {
        if(*p == '/' || !*p) {
            size_t n = (size_t)(p - part);
            if(!n || (n == 1 && *part == '.') || (n == 2 && !memcmp(part, "..", 2)) ||
               (n == 4 && !memcmp(part, ".git", 4))) return 0;
            part = p + 1;
            char saved = *p; *p = 0;
            struct stat st;
            int ok = lstat(walk, &st) == 0 && !S_ISLNK(st.st_mode);
            if(ok && S_ISDIR(st.st_mode)) {
                char git[KRAIT_PATH_MAX * 3];
                snprintf(git, sizeof(git), "%s/.git", walk);
                if(lstat(git, &st) == 0) ok = 0;
            }
            *p = saved;
            if(!ok) return 0;
            if(!saved) break;
        }
    }
    return 1;
}

int
krait_replace_preview(const char *root, const char *query, const char *replacement,
                       int regex, int match_case, const char *exclude)
{
    SearchResult found[64];
    krait_replace_clear();
    if(!root || !*root || !query || !*query || !replacement) {
        snprintf(replacement_status, sizeof(replacement_status), "Enter a nonempty query"); return -1;
    }
    int count = krait_search_project_options(root, query, regex, match_case, 0, exclude, found, 64);
    if(count < 0 || count >= 64) {
        snprintf(replacement_status, sizeof(replacement_status), "%s", count < 0 ? "Invalid regex" : "Result limit reached; narrow the query before replacing");
        return -1;
    }
    snprintf(replacement_root, sizeof(replacement_root), "%s", root);
    for(int i = 0; i < count; i++) {
        int duplicate = 0;
        for(int j = 0; j < replacement_count; j++) if(!strcmp(replacements[j].path, found[i].path)) duplicate = 1;
        if(duplicate) continue;
        char full[KRAIT_PATH_MAX * 2], *before = NULL;
        long len;
        if(!replacement_path(root, found[i].path, full) || !krait_read_file_alloc(full, &before, &len) ||
           !before || len < 0 || len > 16 * 1024 * 1024 || memchr(before, 0, (size_t)len)) {
            free(before); krait_replace_clear();
            snprintf(replacement_status, sizeof(replacement_status), "Cannot preview file: %s", found[i].path);
            return -1;
        }
        int matches;
        char *after = krait_replace_text(before, query, replacement, regex, match_case, &matches);
        if(after == NULL) { free(before); krait_replace_clear();
            snprintf(replacement_status, sizeof(replacement_status), "Replacement exceeds size limit or cannot be allocated"); return -1; }
        if(!strcmp(before, after)) { free(before); free(after); continue; }
        Replacement *r = &replacements[replacement_count++];
        snprintf(r->path, sizeof(r->path), "%s", found[i].path);
        r->before = before; r->after = after; r->matches = matches;
    }
    snprintf(replacement_status, sizeof(replacement_status), "Preview ready: %d changed files", replacement_count);
    return replacement_count;
}

int
krait_replace_apply(IdeState *st)
{
    if(!st || !replacement_count || strcmp(st->project.path, replacement_root) || krait_agent_busy()) {
        snprintf(replacement_status, sizeof(replacement_status), "Preview this project first and wait for the agent to finish"); return 0;
    }
    for(int i = 0; i < replacement_count; i++) {
        char full[KRAIT_PATH_MAX * 2], *text = NULL;
        long len;
        if(!replacement_path(replacement_root, replacements[i].path, full)) {
            snprintf(replacement_status, sizeof(replacement_status), "File path changed or is unsafe"); return 0;
        }
        for(int j = 0; j < st->open_count; j++) if(st->open_files[j].dirty && !strcmp(st->open_files[j].path, full)) {
            snprintf(replacement_status, sizeof(replacement_status), "Save or close unsaved file: %s", replacements[i].path); return 0;
        }
        int same = krait_read_file_alloc(full, &text, &len) && text && len == (long)strlen(replacements[i].before) && !memcmp(text, replacements[i].before, (size_t)len);
        free(text);
        if(!same) { snprintf(replacement_status, sizeof(replacement_status), "File changed since preview: %s", replacements[i].path); return 0; }
    }
    /* Retain all before/after contents before the first mutation. */
    char dir[KRAIT_PATH_MAX * 2], backup[KRAIT_PATH_MAX * 3];
    const char *home = getenv("HOME");
    if(!home || snprintf(dir, sizeof(dir), "%s/.local/state/krait/replacements", home) >= (int)sizeof(dir)) {
        snprintf(replacement_status, sizeof(replacement_status), "Cannot locate recovery directory"); return 0;
    }
    krait_mkdir_p(dir);
    snprintf(backup, sizeof(backup), "%s/batch-XXXXXX", dir);
    int fd = mkstemp(backup);
    if(fd < 0) { snprintf(replacement_status, sizeof(replacement_status), "Cannot create recovery record"); return 0; }
    close(fd);
    KryJsonBuf json = {0};
    kry_json_buf_raw(&json, "{\"root\":"); kry_json_buf_str(&json, replacement_root);
    kry_json_buf_raw(&json, ",\"files\":[");
    for(int i = 0; i < replacement_count; i++) {
        if(i) kry_json_buf_raw(&json, ",");
        kry_json_buf_raw(&json, "{\"path\":"); kry_json_buf_str(&json, replacements[i].path);
        kry_json_buf_raw(&json, ",\"before\":"); kry_json_buf_str(&json, replacements[i].before);
        kry_json_buf_raw(&json, ",\"after\":"); kry_json_buf_str(&json, replacements[i].after);
        kry_json_buf_raw(&json, "}");
    }
    kry_json_buf_raw(&json, "]}");
    const char *data = kry_json_buf_finish(&json);
    int saved = data && krait_write_text_file_atomic(backup, data);
    kry_json_buf_free(&json);
    if(!saved) { snprintf(replacement_status, sizeof(replacement_status), "Cannot save recovery record"); return 0; }
    int applied = 0;
    for(int i = 0; i < replacement_count; i++) {
        char full[KRAIT_PATH_MAX * 2], *text = NULL;
        long len;
        int same = replacement_path(replacement_root, replacements[i].path, full) &&
            krait_read_file_alloc(full, &text, &len) && text && len == (long)strlen(replacements[i].before) && !memcmp(text, replacements[i].before, (size_t)len);
        free(text);
        if(!same || !krait_project_file_replace(replacement_root, replacements[i].path, replacements[i].before, 1, replacements[i].after)) {
            snprintf(replacement_status, sizeof(replacement_status), "Stopped after %d files; recovery record: %.350s", applied, backup);
            return applied;
        }
        applied++;
    }
    snprintf(replacement_status, sizeof(replacement_status), "Replaced %d files; recovery record: %.350s", applied, backup);
    return applied;
}
