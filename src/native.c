#include "kryon.h"
#include "ide/state.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define KRAIT_PATH_MAX 1024
#define KRAIT_MAX_RECENT 12
#define KRAIT_MAX_EXAMPLES 32
#define KRAIT_SEARCH_DEPTH 8
#define KRAIT_TREE_DEPTH 8

typedef struct {
    char path[KRAIT_PATH_MAX];
    char title[256];
} KraitPathItem;

static KraitPathItem g_examples[KRAIT_MAX_EXAMPLES];
static int g_example_count;
static int g_examples_loaded;

static void
krait_join(char *dst, size_t dst_size, const char *a, const char *b)
{
    if(dst_size == 0)
        return;
    if(a == NULL || a[0] == '\0')
        snprintf(dst, dst_size, "%s", b != NULL ? b : "");
    else if(b == NULL || b[0] == '\0')
        snprintf(dst, dst_size, "%s", a);
    else
        snprintf(dst, dst_size, "%s/%s", a, b);
}

static const char *
krait_basename(const char *path)
{
    const char *base = path;
    if(path == NULL)
        return "";
    for(const char *p = path; *p != '\0'; p++) {
        if(*p == '/')
            base = p + 1;
    }
    return base;
}

static void
krait_title_from_file(char *dst, size_t dst_size, const char *file)
{
    const char *base = krait_basename(file);
    size_t n = strlen(base);

    if(dst_size == 0)
        return;
    if(n >= 4 && strcmp(base + n - 4, ".kry") == 0)
        n -= 4;
    if(n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, base, n);
    dst[n] = '\0';
    for(size_t i = 0; dst[i] != '\0'; i++) {
        if(dst[i] == '_')
            dst[i] = ' ';
    }
}

static int
krait_path_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
}

static int
krait_path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;

    if(path == NULL || suffix == NULL)
        return 0;
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if(path_len < suffix_len)
        return 0;
    return strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int
krait_file_is_text(const char *path)
{
    const char *base = krait_basename(path);

    return krait_path_has_suffix(path, ".kry") ||
           krait_path_has_suffix(path, ".c") ||
           krait_path_has_suffix(path, ".h") ||
           krait_path_has_suffix(path, ".md") ||
           krait_path_has_suffix(path, ".txt") ||
           krait_path_has_suffix(path, ".mk") ||
           strcmp(base, "Makefile") == 0 ||
           strcmp(base, "makefile") == 0 ||
           strcmp(base, "project.kryon") == 0;
}

static int
krait_ignored_dir(const char *name)
{
    return name == NULL ||
           strcmp(name, ".git") == 0 ||
           strcmp(name, "build") == 0 ||
           strcmp(name, "vendor") == 0 ||
           strcmp(name, ".cache") == 0;
}

static int
krait_tree_id(const char *path)
{
    unsigned int hash = 2166136261u;

    if(path == NULL || path[0] == '\0')
        return 1;
    for(const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    hash &= 0x7fffffffu;
    if(hash == 0)
        hash = 1;
    return (int)hash;
}

static int
krait_add_tree_entry(FileTreeEntry *out, int count, int cap,
                     const char *label, const char *path, int depth,
                     int is_dir)
{
    FileTreeEntry *entry;

    if(out == NULL || count >= cap || label == NULL || path == NULL)
        return count;
    entry = &out[count++];
    snprintf(entry->label, sizeof(entry->label), "%s", label);
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->depth = depth;
    entry->is_dir = is_dir ? 1 : 0;
    entry->id = krait_tree_id(path);
    return count;
}

static int
krait_build_tree_dir(const char *root, const char *rel_dir,
                     FileTreeEntry *out, int count, int cap, int depth)
{
    char dir[KRAIT_PATH_MAX];
    KryDirEntry entries[256];
    int entry_count;

    if(root == NULL || out == NULL || count >= cap || depth > KRAIT_TREE_DEPTH)
        return count;
    if(rel_dir != NULL && rel_dir[0] != '\0')
        krait_join(dir, sizeof(dir), root, rel_dir);
    else
        snprintf(dir, sizeof(dir), "%s", root);

    entry_count = kry_fs_list_dir(dir, entries, 256);
    for(int i = 0; i < entry_count && count < cap; i++) {
        char rel_path[KRAIT_PATH_MAX];

        if(rel_dir != NULL && rel_dir[0] != '\0')
            krait_join(rel_path, sizeof(rel_path), rel_dir, entries[i].name);
        else
            snprintf(rel_path, sizeof(rel_path), "%s", entries[i].name);

        if(entries[i].is_dir) {
            if(krait_ignored_dir(entries[i].name))
                continue;
            count = krait_add_tree_entry(out, count, cap, entries[i].name,
                                         rel_path, depth, 1);
            if(depth < KRAIT_TREE_DEPTH)
                count = krait_build_tree_dir(root, rel_path, out, count, cap,
                                             depth + 1);
        } else {
            count = krait_add_tree_entry(out, count, cap, entries[i].name,
                                         rel_path, depth, 0);
        }
    }
    return count;
}

int
krait_build_tree(const char *root, FileTreeEntry *out, int cap)
{
    if(root == NULL || root[0] == '\0' || out == NULL || cap <= 0)
        return 0;
    return krait_build_tree_dir(root, "", out, 0, cap, 0);
}

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

static void
krait_ensure_parent_dir(const char *path)
{
    char dir[KRAIT_PATH_MAX];
    char *slash;

    if(path == NULL)
        return;
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if(slash == NULL)
        return;
    *slash = '\0';
    (void)mkdir(dir, 0755);
}

static int
krait_mkdir_p(const char *dir)
{
    char tmp[KRAIT_PATH_MAX];
    size_t len;

    if(dir == NULL || dir[0] == '\0')
        return 0;
    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if(len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for(char *p = tmp + 1; *p != '\0'; p++) {
        if(*p == '/') {
            *p = '\0';
            if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return 0;
            *p = '/';
        }
    }
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return 0;
    return 1;
}

static int
krait_write_text_file(const char *path, const char *text)
{
    FILE *file;
    size_t len;

    if(path == NULL || text == NULL)
        return 0;
    file = fopen(path, "wb");
    if(file == NULL)
        return 0;
    len = strlen(text);
    if(fwrite(text, 1, len, file) != len) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

int
krait_scaffold_project(const char *dir, char *status, int status_size)
{
    char project_kryon[KRAIT_PATH_MAX];
    char main_kry[KRAIT_PATH_MAX];
    char name[96];
    char project_file[256];
    const char *main_kry_contents =
        "#import \"kryon.h\"\n"
        "\n"
        "app \"New Kryon App\" {\n"
        "    size 800 600\n"
        "    fps 60\n"
        "    theme THEME_MONO light\n"
        "}\n"
        "\n"
        "state {\n"
        "    click_count: int = 0\n"
        "}\n"
        "\n"
        "screen Main(viewport: Rectangle) {\n"
        "    background GetThemeBackground()\n"
        "    text \"Hello, Kryon!\" x: ScaleUIPx(20) y: ScaleUIPx(20) size: UI_TEXT_24 color: GetThemeText()\n"
        "    text \"Edit main.kry and the preview reloads.\" x: ScaleUIPx(20) y: ScaleUIPx(54) size: UI_TEXT_16 color: GetThemeIcon()\n"
        "    button \"Click me\" x: ScaleUIPx(20) y: ScaleUIPx(100) w: ScaleUIPx(160) h: ScaleUIPx(40) style: UI_BUTTON_STYLE_PRIMARY {\n"
        "        set click_count++\n"
        "    }\n"
        "    text TextFormat(\"Clicks: %d\", click_count) x: ScaleUIPx(20) y: ScaleUIPx(160) size: UI_TEXT_16 color: GetThemeText()\n"
        "}\n";

    if(dir == NULL || dir[0] == '\0') {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot scaffold: no directory");
        return 0;
    }
    snprintf(name, sizeof(name), "%s", krait_basename(dir));
    if(!krait_mkdir_p(dir)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size,
                     "Cannot create project directory: %s", dir);
        return 0;
    }
    krait_join(project_kryon, sizeof(project_kryon), dir, "project.kryon");
    snprintf(project_file, sizeof(project_file),
             "# Generated by Krait. Edit targets and metadata here.\n"
             "name \"%s\"\n"
             "preview_size 800 600\n",
             name);
    if(!krait_write_text_file(project_kryon, project_file)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot write %s",
                     project_kryon);
        return 0;
    }
    krait_join(main_kry, sizeof(main_kry), dir, "main.kry");
    if(!krait_write_text_file(main_kry, main_kry_contents)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot write %s", main_kry);
        return 0;
    }
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Scaffolded project at %s", dir);
    return 1;
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

static void
krait_load_examples(void)
{
    const char *kryon_dir = getenv("KRYON_DIR");
    char dir_path[KRAIT_PATH_MAX];
    DIR *dir;
    struct dirent *ent;

    if(g_examples_loaded)
        return;
    g_examples_loaded = 1;
    if(kryon_dir == NULL || kryon_dir[0] == '\0')
        kryon_dir = "vendor/kryon";
    krait_join(dir_path, sizeof(dir_path), kryon_dir, "examples");
    dir = opendir(dir_path);
    if(dir == NULL)
        return;
    while(g_example_count < KRAIT_MAX_EXAMPLES &&
          (ent = readdir(dir)) != NULL) {
        if(ent->d_name[0] == '.' || !krait_path_has_suffix(ent->d_name, ".kry"))
            continue;
        krait_join(g_examples[g_example_count].path,
                   sizeof(g_examples[g_example_count].path),
                   dir_path, ent->d_name);
        krait_title_from_file(g_examples[g_example_count].title,
                              sizeof(g_examples[g_example_count].title),
                              ent->d_name);
        g_example_count++;
    }
    closedir(dir);
}

int
krait_example_count(void)
{
    krait_load_examples();
    return g_example_count;
}

int
krait_example_path(int index, char *out, int cap)
{
    if(out == NULL || cap <= 0)
        return 0;
    out[0] = '\0';
    krait_load_examples();
    if(index < 0 || index >= g_example_count)
        return 0;
    snprintf(out, (size_t)cap, "%s", g_examples[index].path);
    return 1;
}

int
krait_example_title(int index, char *out, int cap)
{
    if(out == NULL || cap <= 0)
        return 0;
    out[0] = '\0';
    krait_load_examples();
    if(index < 0 || index >= g_example_count)
        return 0;
    snprintf(out, (size_t)cap, "%s", g_examples[index].title);
    return 1;
}

int
krait_load_ui_font(void)
{
    const char *kryon_dir = getenv("KRYON_DIR");
    char path[KRAIT_PATH_MAX];
    const char *app_dir;
    int ok = 0;

    if(RegisterUIFontFileSource("default",
                                "assets/fonts/DepartureMono-Regular.otf",
                                NULL, 0, 1)) {
        UseUIFont("default");
        ok = 1;
    }
    app_dir = GetApplicationDirectory();
    if(!ok && app_dir != NULL && app_dir[0] != '\0') {
        krait_join(path, sizeof(path), app_dir,
                   "../../assets/fonts/DepartureMono-Regular.otf");
        if(RegisterUIFontFileSource("default", path, NULL, 0, 1)) {
            UseUIFont("default");
            ok = 1;
        }
    }
    if(!ok && app_dir != NULL && app_dir[0] != '\0') {
        krait_join(path, sizeof(path), app_dir,
                   "../share/krait/assets/fonts/DepartureMono-Regular.otf");
        if(RegisterUIFontFileSource("default", path, NULL, 0, 1)) {
            UseUIFont("default");
            ok = 1;
        }
    }
    if(kryon_dir != NULL && kryon_dir[0] != '\0') {
        krait_join(path, sizeof(path), kryon_dir, "fonts/noto/NotoSans-Regular.ttf");
        (void)RegisterUIFontFileSource("noto", path, NULL, 0, 1);
    } else {
        (void)RegisterUIFontFileSource("noto", "vendor/kryon/fonts/noto/NotoSans-Regular.ttf",
                                       NULL, 0, 1);
    }
    return ok;
}
