#include "kryon.h"
#include "ide/state.h"
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

#define KRAIT_PATH_MAX 1024
#define KRAIT_MAX_RECENT 12
#define KRAIT_MAX_EXAMPLES 32
#define KRAIT_SEARCH_DEPTH 8
#define KRAIT_TREE_DEPTH 8
#define KRAIT_MTIME_DEPTH 8
#define KRAIT_SCREEN_DEPTH 8
#define KRAIT_SCENE_NODE_MAX 256

typedef struct {
    char path[KRAIT_PATH_MAX];
    char title[256];
} KraitPathItem;

static KraitPathItem g_examples[KRAIT_MAX_EXAMPLES];
static int g_example_count;
static int g_examples_loaded;

typedef struct {
    void *dylib;
    AppHost *host;
    DestroyAppHostCallback destroy_host;
    int defer_unload;
    char project_path[KRAIT_PATH_MAX];
    char host_path[KRAIT_PATH_MAX];
} KraitPreviewHost;

typedef struct {
    KryProcess proc;
    int running;
    char project_path[KRAIT_PATH_MAX];
    char host_path[KRAIT_PATH_MAX];
    char output[8192];
} KraitPreviewBuild;

typedef struct SceneNode {
    int id;
    int parent;
    int depth;
    int kind;
    int editable;
    char label[128];
    char asset_path[512];
    Rectangle bounds;
    int source_line;
    int source_start;
    int source_end;
} SceneNode;

typedef struct KraitProjectMeta {
    int loaded;
    char root[KRAIT_PATH_MAX];
    char title[256];
    char app_id[256];
    char version[64];
    char icon_path[KRAIT_PATH_MAX];
    char language[32];
    char short_description[512];
    char full_description[4096];
    char languages[512];
    int cursor[7];
    int focused[7];
    int full_scroll;
} KraitProjectMeta;

enum {
    KRAIT_SCENE_NODE_UNKNOWN,
    KRAIT_SCENE_NODE_SCREEN,
    KRAIT_SCENE_NODE_GROUP,
    KRAIT_SCENE_NODE_TEXT,
    KRAIT_SCENE_NODE_RECTANGLE,
    KRAIT_SCENE_NODE_BUTTON,
    KRAIT_SCENE_NODE_TEXT_FIELD,
    KRAIT_SCENE_NODE_TOGGLE,
    KRAIT_SCENE_NODE_SLIDER,
    KRAIT_SCENE_NODE_SPRITE,
    KRAIT_SCENE_NODE_GAME_OBJECT
};

static KraitPreviewHost g_preview_host;
static KraitPreviewBuild g_preview_build;
static unsigned long g_preview_host_generation;
static SceneNode g_scene_nodes[KRAIT_SCENE_NODE_MAX];
static int g_scene_node_count;
static KraitProjectMeta g_project_meta;

static int krait_write_text_file(const char *path, const char *text);
static int krait_read_file_alloc(const char *path, char **out, long *out_len);
static void krait_ensure_parent_dir(const char *path);
static char *krait_meta_trim(char *s);
static int krait_preview_load_host(const char *project_path,
                                   const char *host_path,
                                   char *status, int status_size);
static int krait_live_draw_source(const char *root, const char *rel_path,
                                  int w, int h, char *status,
                                  int status_size);
static int krait_live_line_for_ptr(const char *text, const char *ptr);

static int
krait_preview_env_begin(char *old_value, size_t old_value_size)
{
    const char *old = getenv("KRYON_INSPECT");
    int had = old != NULL;

    if(old_value != NULL && old_value_size > 0)
        snprintf(old_value, old_value_size, "%s", old != NULL ? old : "");
    setenv("KRYON_INSPECT", "1", 1);
    return had;
}

static void
krait_preview_env_end(int had, const char *old_value)
{
    if(had)
        setenv("KRYON_INSPECT", old_value != NULL ? old_value : "", 1);
    else
        unsetenv("KRYON_INSPECT");
}

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

static int
krait_shell_quote(char *dst, size_t dst_size, const char *src)
{
    size_t n = 0;

    if(dst == NULL || dst_size == 0)
        return 0;
    if(src == NULL)
        src = "";
    if(n + 1 >= dst_size)
        return 0;
    dst[n++] = '\'';
    for(const char *p = src; *p != '\0'; p++) {
        if(*p == '\'') {
            const char *q = "'\\''";
            for(int i = 0; q[i] != '\0'; i++) {
                if(n + 1 >= dst_size)
                    return 0;
                dst[n++] = q[i];
            }
        } else {
            if(n + 1 >= dst_size)
                return 0;
            dst[n++] = *p;
        }
    }
    if(n + 1 >= dst_size)
        return 0;
    dst[n++] = '\'';
    dst[n] = '\0';
    return 1;
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
krait_copy_file(const char *src, const char *dst)
{
    FILE *in;
    FILE *out;
    char buf[16384];
    size_t n;

    if(src == NULL || dst == NULL)
        return 0;
    in = fopen(src, "rb");
    if(in == NULL)
        return 0;
    out = fopen(dst, "wb");
    if(out == NULL) {
        fclose(in);
        return 0;
    }
    while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if(fwrite(buf, 1, n, out) != n) {
            fclose(out);
            fclose(in);
            return 0;
        }
    }
    if(ferror(in)) {
        fclose(out);
        fclose(in);
        return 0;
    }
    fclose(out);
    fclose(in);
    return 1;
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
krait_file_affects_project_mtime(const char *path)
{
    return path != NULL && path[0] != '\0';
}

static int
krait_ignored_dir(const char *name)
{
    return name == NULL ||
           name[0] == '.' ||
           strcmp(name, ".git") == 0 ||
           strcmp(name, "build") == 0 ||
           strcmp(name, "vendor") == 0 ||
           strcmp(name, "tmp") == 0 ||
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
krait_layout_file(char *path, size_t path_size)
{
    const char *home = getenv("HOME");

    if(path_size == 0)
        return;
    if(home == NULL || home[0] == '\0')
        snprintf(path, path_size, ".kryon/krait_layout.txt");
    else
        snprintf(path, path_size, "%s/.kryon/krait_layout.txt", home);
}

static void
krait_settings_file(char *path, size_t path_size)
{
    const char *home = getenv("HOME");

    if(path_size == 0)
        return;
    if(home == NULL || home[0] == '\0')
        snprintf(path, path_size, ".kryon/krait_settings.txt");
    else
        snprintf(path, path_size, "%s/.kryon/krait_settings.txt", home);
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
krait_write_preview_shim(const char *path)
{
    FILE *file = fopen(path, "w");

    if(file == NULL)
        return 0;
    fputs("#include \"kryon.h\"\n"
          "#include \"app_host.h\"\n"
          "\n"
          "void *\n"
          "CreateApp(const char *project_path)\n"
          "{\n"
          "    (void)project_path;\n"
          "    return (void *)1;\n"
          "}\n"
          "\n"
          "void\n"
          "DestroyApp(void *app)\n"
          "{\n"
          "    (void)app;\n"
          "}\n"
          "\n"
          "void\n"
          "ApplyRoute(void *app, const AppRouteInfo *route)\n"
          "{\n"
          "    (void)app;\n"
          "    (void)route;\n"
          "}\n"
          "\n"
          "void\n"
          "BeginScreenDraw(void *app, Rectangle viewport)\n"
          "{\n"
          "    (void)app;\n"
          "    SetUIViewSize((int)viewport.width, (int)viewport.height);\n"
          "}\n",
          file);
    fclose(file);
    return 1;
}

static int
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
            q = krait_meta_trim(line);
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

static int
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
        p = krait_meta_trim(line);
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

static const char *
krait_preview_last_output_line(void)
{
    static char last[512];
    const char *line = g_preview_build.output;

    snprintf(last, sizeof(last), "%s", "Preview build failed");
    for(const char *p = g_preview_build.output; *p != '\0'; p++) {
        if(*p == '\n' && p[1] != '\0')
            line = p + 1;
    }
    if(line[0] != '\0') {
        size_t n = strcspn(line, "\r\n");
        if(n >= sizeof(last))
            n = sizeof(last) - 1;
        memcpy(last, line, n);
        last[n] = '\0';
    }

    return last;
}

static void
krait_preview_build_append(const char *text)
{
    size_t have;
    size_t add;

    if(text == NULL || text[0] == '\0')
        return;
    have = strlen(g_preview_build.output);
    add = strlen(text);
    if(add >= sizeof(g_preview_build.output)) {
        text += add - sizeof(g_preview_build.output) + 1;
        add = strlen(text);
        have = 0;
    } else if(have + add + 1 > sizeof(g_preview_build.output)) {
        size_t drop = have + add + 1 - sizeof(g_preview_build.output);
        memmove(g_preview_build.output, g_preview_build.output + drop,
                have - drop + 1);
        have -= drop;
    }
    memcpy(g_preview_build.output + have, text, add + 1);
}

static void
krait_preview_build_drain(void)
{
    char buf[1024];
    int n;

    while((n = kry_process_read_poll(&g_preview_build.proc, buf,
                                     sizeof(buf))) > 0)
        krait_preview_build_append(buf);
}

static void
krait_preview_build_close(void)
{
    if(g_preview_build.running)
        kry_process_close(&g_preview_build.proc);
    memset(&g_preview_build, 0, sizeof(g_preview_build));
}

static int
krait_preview_build_poll(char *status, int status_size)
{
    if(!g_preview_build.running)
        return 0;
    krait_preview_build_drain();
    if(!kry_process_wait_poll(&g_preview_build.proc)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview building...");
        return 0;
    }
    krait_preview_build_drain();
    if(g_preview_build.proc.exit_status != 0) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "%s",
                     krait_preview_last_output_line());
        krait_preview_build_close();
        return 0;
    }
    if(!krait_preview_load_host(g_preview_build.project_path,
                                g_preview_build.host_path,
                                status, status_size)) {
        krait_preview_build_close();
        return 0;
    }
    krait_preview_build_close();
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Preview built");
    return 1;
}

static int
krait_preview_build_start(const char *project_path, const char *host_path,
                          const char *command, char *status, int status_size)
{
    krait_preview_build_close();
    memset(&g_preview_build, 0, sizeof(g_preview_build));
    snprintf(g_preview_build.project_path, sizeof(g_preview_build.project_path),
             "%s", project_path);
    snprintf(g_preview_build.host_path, sizeof(g_preview_build.host_path),
             "%s", host_path);
    if(!kry_process_spawn(&g_preview_build.proc, command, project_path)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview build spawn failed");
        return 0;
    }
    g_preview_build.running = 1;
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Preview building...");
    return 0;
}

static void
krait_preview_unload_host(int reloading)
{
    if(g_preview_host.defer_unload && !reloading) {
        memset(&g_preview_host, 0, sizeof(g_preview_host));
        return;
    }
    if(g_preview_host.destroy_host != NULL && g_preview_host.host != NULL)
        g_preview_host.destroy_host(g_preview_host.host);
    if(g_preview_host.defer_unload) {
        memset(&g_preview_host, 0, sizeof(g_preview_host));
        return;
    }
    if(g_preview_host.dylib != NULL)
        kry_dylib_close(g_preview_host.dylib);
    memset(&g_preview_host, 0, sizeof(g_preview_host));
}

void
krait_preview_unload(void)
{
    krait_preview_build_close();
    krait_preview_unload_host(1);
}

static int
krait_preview_load_host(const char *project_path, const char *host_path,
                        char *status, int status_size)
{
    void *dylib;
    CreateAppHostCallback create_host;
    DestroyAppHostCallback destroy_host;
    AppHost *host;
    const char *err;
    char load_path[KRAIT_PATH_MAX];
    char old_inspect_copy[64];
    UIFrameState saved_frame;
    int had_inspect;
    int has_project_host;
    char configured_live[KRAIT_PATH_MAX];

    configured_live[0] = '\0';
    has_project_host = krait_project_has_make_target(project_path, "kryon-host");
    if(krait_project_preview_config(project_path, configured_live,
                                    sizeof(configured_live), NULL, 0) &&
       configured_live[0] != '\0')
        has_project_host = 1;
    snprintf(load_path, sizeof(load_path), "%s", host_path);
    if(has_project_host) {
        char copy_path[KRAIT_PATH_MAX];

        snprintf(copy_path, sizeof(copy_path), "%s.%lu.so", host_path,
                 ++g_preview_host_generation);
        if(!krait_copy_file(host_path, copy_path)) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size,
                         "Preview host copy failed");
            return 0;
        }
        snprintf(load_path, sizeof(load_path), "%s", copy_path);
    }

    dylib = kry_dylib_load(load_path);
    if(dylib == NULL) {
        err = kry_dylib_error();
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview load failed: %s",
                     err != NULL ? err : load_path);
        return 0;
    }
    create_host = (CreateAppHostCallback)kry_dylib_sym(dylib, "CreateAppHost");
    destroy_host = (DestroyAppHostCallback)kry_dylib_sym(dylib, "DestroyAppHost");
    if(create_host == NULL || destroy_host == NULL) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size,
                     "Preview host missing CreateAppHost/DestroyAppHost");
        kry_dylib_close(dylib);
        return 0;
    }
    had_inspect = krait_preview_env_begin(old_inspect_copy,
                                          sizeof(old_inspect_copy));
    saved_frame = SaveUIFrameState();
    host = create_host(APP_HOST_ABI_VERSION, project_path);
    RestoreUIFrameState(saved_frame);
    krait_preview_env_end(had_inspect, old_inspect_copy);
    if(host == NULL) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size,
                     "Preview host rejected ABI %d", APP_HOST_ABI_VERSION);
        kry_dylib_close(dylib);
        return 0;
    }

    krait_preview_unload_host(0);
    g_preview_host.dylib = dylib;
    g_preview_host.host = host;
    g_preview_host.destroy_host = destroy_host;
    g_preview_host.defer_unload = has_project_host ? 1 : 0;
    snprintf(g_preview_host.project_path, sizeof(g_preview_host.project_path),
             "%s", project_path);
    snprintf(g_preview_host.host_path, sizeof(g_preview_host.host_path),
             "%s", load_path);
    return 1;
}

int
krait_preview_build(IdeState *st, char *status, int status_size)
{
    const char *kryon_dir;
    char build_dir[KRAIT_PATH_MAX];
    char shim_path[KRAIT_PATH_MAX];
    char host_path[KRAIT_PATH_MAX];
    char configured_live[KRAIT_PATH_MAX];
    char configured_build[KRAIT_PATH_MAX * 4];
    char q_kryon[KRAIT_PATH_MAX * 2];
    char command[KRAIT_PATH_MAX * 12];
    int has_project_host;

    if(status != NULL && status_size > 0)
        status[0] = '\0';
    if(st == NULL || !st->project.loaded || st->project.path[0] == '\0') {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "No project loaded");
        return 0;
    }
    if(g_preview_build.running &&
       strcmp(g_preview_build.project_path, st->project.path) == 0)
        return krait_preview_build_poll(status, status_size);
    if(g_preview_build.running)
        krait_preview_build_close();
    kryon_dir = getenv("KRYON_DIR");
    if(kryon_dir == NULL || kryon_dir[0] == '\0')
        kryon_dir = "vendor/kryon";
    if(!krait_shell_quote(q_kryon, sizeof(q_kryon), kryon_dir)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview path too long");
        return 0;
    }

    krait_join(build_dir, sizeof(build_dir), st->project.path, "build/kryon");
    krait_join(shim_path, sizeof(shim_path), build_dir, "preview_shim.c");
    krait_join(host_path, sizeof(host_path), build_dir, "app_host.so");
    if(kry_fs_mkdir_p(build_dir) != 0) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot create preview build dir");
        return 0;
    }
    configured_live[0] = '\0';
    configured_build[0] = '\0';
    (void)krait_project_preview_config(st->project.path, configured_live,
                                       sizeof(configured_live),
                                       configured_build,
                                       sizeof(configured_build));
    if(configured_live[0] != '\0') {
        krait_join(host_path, sizeof(host_path), st->project.path,
                   configured_live);
        if((st->preview_dirty == 0 || configured_build[0] == '\0') &&
           krait_path_exists(host_path) &&
           krait_preview_load_host(st->project.path, host_path, status,
                                   status_size)) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size, "Live host ready");
            return 1;
        }
        if(configured_build[0] != '\0')
            return krait_preview_build_start(st->project.path, host_path,
                                             configured_build, status,
                                             status_size);
        if(krait_preview_load_host(st->project.path, host_path, status,
                                   status_size)) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size, "Live host ready");
            return 1;
        }
        return 0;
    }
    has_project_host = krait_project_has_make_target(st->project.path, "kryon-host");
    if(has_project_host) {
        snprintf(command, sizeof(command),
                 "gmake -f Makefile build/kryon/generated/.fresh kryon-host KRYON_DIR=%s",
                 q_kryon);
        return krait_preview_build_start(st->project.path, host_path,
                                         command, status, status_size);
    }
    if(!krait_write_preview_shim(shim_path)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot write preview shim");
        return 0;
    }

    snprintf(command, sizeof(command),
             "mkdir -p build/kryon/gen && "
             "%s/build/bin/kc --no-main --root . -o build/kryon/gen "
             "$(find . -name '*.kry' -not -path './build/*' -not -path './vendor/*' | sort) && "
             "cc -shared -fPIC "
             "-Ibuild/kryon/gen -I%s/include -I%s/src/ui -I%s/vendor/clay "
             "-o build/kryon/app_host.so "
             "$(find build/kryon/gen -name '*.c' | sort) build/kryon/preview_shim.c",
             q_kryon, kryon_dir, kryon_dir, kryon_dir);
    return krait_preview_build_start(st->project.path, host_path,
                                     command, status, status_size);
}

int
krait_preview_draw(IdeState *st, const char *rel_path, Rectangle viewport,
                   char *status, int status_size)
{
    char old_inspect[64];
    int had_inspect;
    int ok;

    if(status != NULL && status_size > 0)
        status[0] = '\0';
    if(st == NULL || rel_path == NULL || rel_path[0] == '\0') {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "No .kry source selected");
        return 0;
    }
    ok = krait_live_draw_source(st->project.path, rel_path,
                                (int)viewport.width,
                                (int)viewport.height,
                                status, status_size);
    if(ok && (status == NULL || strstr(status, "delegates") == NULL))
        return 1;
    if(g_preview_host.host == NULL ||
       strcmp(g_preview_host.project_path, st->project.path) != 0) {
        if(!krait_preview_build(st, status, status_size)) {
            if(status != NULL && strcmp(status, "Preview building...") == 0)
                return 1;
            return 0;
        }
    }
    had_inspect = krait_preview_env_begin(old_inspect, sizeof(old_inspect));
    if(!SetAppScreenBySourcePath(g_preview_host.host, rel_path)) {
        krait_preview_env_end(had_inspect, old_inspect);
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size,
                     "Preview route not found: %s", rel_path);
        return ok;
    }
    DrawAppScreen(g_preview_host.host, viewport);
    krait_preview_env_end(had_inspect, old_inspect);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Live canvas ready");
    return 1;
}

static int
krait_valid_pane_view(int view)
{
    return view >= IDE_PANE_VIEW_EXPLORER && view <= IDE_PANE_VIEW_WIDGETS;
}

static void
krait_settings_sanitize(IdeState *st)
{
    if(st == NULL)
        return;
    if(st->theme_source != THEME_SOURCE_SYSTEM)
        st->theme_source = THEME_SOURCE_APP;
    if(st->theme_mode < THEME_MODE_SYSTEM || st->theme_mode > THEME_MODE_DARK)
        st->theme_mode = THEME_MODE_DARK;
    if(st->theme_id < 0 || st->theme_id >= THEME_COUNT)
        st->theme_id = THEME_MONO;
    if(st->theme_style < THEME_STYLE_SYSTEM || st->theme_style > THEME_STYLE_MATERIAL)
        st->theme_style = THEME_STYLE_SYSTEM;
    if(st->settings_tab < 0 || st->settings_tab > 4)
        st->settings_tab = 0;
    if(st->settings_scroll < 0)
        st->settings_scroll = 0;
    if(st->module_count < 0)
        st->module_count = 0;
    if(st->module_count > IDE_MAX_MODULES)
        st->module_count = IDE_MAX_MODULES;
    for(int i = 0; i < st->module_count; i++)
        st->modules[i].enabled = st->modules[i].enabled ? 1 : 0;
}

static int
krait_settings_set_module(IdeState *st, const char *key, int value)
{
    char expected[160];

    if(st == NULL || key == NULL)
        return 0;
    for(int i = 0; i < st->module_count; i++) {
        snprintf(expected, sizeof(expected), "module_%s_enabled",
                 st->modules[i].id);
        if(strcmp(key, expected) == 0) {
            st->modules[i].enabled = value ? 1 : 0;
            return 1;
        }
    }
    return 0;
}

static void
krait_layout_sanitize_group(PaneGroup *group)
{
    int out[IDE_MAX_PANE_TABS];
    int count = 0;

    if(group == NULL)
        return;
    for(int i = 0; i < group->tab_count && i < IDE_MAX_PANE_TABS; i++) {
        int view = group->tabs[i];
        int seen = 0;

        if(!krait_valid_pane_view(view))
            continue;
        for(int j = 0; j < count; j++) {
            if(out[j] == view) {
                seen = 1;
                break;
            }
        }
        if(!seen)
            out[count++] = view;
    }
    memset(group->tabs, 0, sizeof(group->tabs));
    for(int i = 0; i < count; i++)
        group->tabs[i] = out[i];
    group->tab_count = count;
    if(group->tab_count <= 0) {
        group->active = 0;
        return;
    }
    if(group->active < 0)
        group->active = 0;
    if(group->active >= group->tab_count)
        group->active = group->tab_count - 1;
}

static void
krait_layout_clear_group(PaneGroup *group)
{
    if(group == NULL)
        return;
    memset(group, 0, sizeof(*group));
}

static void
krait_layout_clear_node(PaneNode *node)
{
    if(node == NULL)
        return;
    memset(node, 0, sizeof(*node));
    node->kind = IDE_PANE_NODE_EMPTY;
    node->split_axis = IDE_PANE_SPLIT_HORIZONTAL;
    node->first = -1;
    node->second = -1;
    node->group = -1;
    node->ratio = 0.5f;
}

static void
krait_layout_remove_duplicates(IdeState *st)
{
    int seen[IDE_PANE_VIEW_SETTINGS + 1] = {0};

    if(st == NULL)
        return;
    for(int i = IDE_PANE_LEFT; i <= IDE_PANE_BOTTOM; i++) {
        PaneGroup *group = &st->pane_groups[i];
        int out[IDE_MAX_PANE_TABS];
        int count = 0;

        if(group->used == 0)
            continue;
        for(int j = 0; j < group->tab_count && j < IDE_MAX_PANE_TABS; j++) {
            int view = group->tabs[j];
            if(!krait_valid_pane_view(view) || seen[view])
                continue;
            seen[view] = 1;
            out[count++] = view;
        }
        memset(group->tabs, 0, sizeof(group->tabs));
        for(int j = 0; j < count; j++)
            group->tabs[j] = out[j];
        group->tab_count = count;
        if(group->active >= group->tab_count)
            group->active = group->tab_count - 1;
        if(group->active < 0)
            group->active = 0;
    }
}

static void
krait_layout_set_group(PaneGroup *group, const int *views, int count,
                       int fallback_active)
{
    int active_view = -1;

    if(group == NULL)
        return;
    if(group->used != 0 && group->active >= 0 &&
       group->active < group->tab_count)
        active_view = group->tabs[group->active];
    memset(group->tabs, 0, sizeof(group->tabs));
    group->used = 1;
    group->tab_count = 0;
    for(int i = 0; i < count && i < IDE_MAX_PANE_TABS; i++)
        group->tabs[group->tab_count++] = views[i];
    group->active = fallback_active;
    for(int i = 0; i < group->tab_count; i++) {
        if(group->tabs[i] == active_view) {
            group->active = i;
            break;
        }
    }
    if(group->active >= group->tab_count)
        group->active = group->tab_count - 1;
    if(group->active < 0)
        group->active = 0;
}

static void
krait_layout_enforce_studio(IdeState *st)
{
    static const int left_views[] = {
        IDE_PANE_VIEW_FILES,
        IDE_PANE_VIEW_EXPLORER,
        IDE_PANE_VIEW_HIERARCHY,
        IDE_PANE_VIEW_WIDGETS,
    };
    static const int center_views[] = {
        IDE_PANE_VIEW_PREVIEW,
        IDE_PANE_VIEW_EDITOR,
    };
    static const int right_views[] = {
        IDE_PANE_VIEW_INSPECTOR,
    };
    static const int bottom_views[] = {
        IDE_PANE_VIEW_PROBLEMS,
        IDE_PANE_VIEW_CONSOLE,
    };

    if(st == NULL)
        return;
    krait_layout_set_group(&st->pane_groups[IDE_PANE_LEFT],
                           left_views, (int)(sizeof(left_views) / sizeof(left_views[0])),
                           0);
    krait_layout_set_group(&st->pane_groups[IDE_PANE_CENTER],
                           center_views, (int)(sizeof(center_views) / sizeof(center_views[0])),
                           0);
    krait_layout_set_group(&st->pane_groups[IDE_PANE_RIGHT],
                           right_views, (int)(sizeof(right_views) / sizeof(right_views[0])),
                           0);
    krait_layout_set_group(&st->pane_groups[IDE_PANE_BOTTOM],
                           bottom_views, (int)(sizeof(bottom_views) / sizeof(bottom_views[0])),
                           0);
    if(st->bottom_height <= 0)
        st->bottom_height = 210;
}

static void
krait_layout_sanitize(IdeState *st)
{
    if(st == NULL)
        return;
    for(int i = 0; i < IDE_MAX_PANE_GROUPS; i++) {
        if(i >= IDE_PANE_LEFT && i <= IDE_PANE_BOTTOM) {
            st->pane_groups[i].used = 1;
            krait_layout_sanitize_group(&st->pane_groups[i]);
        } else {
            krait_layout_clear_group(&st->pane_groups[i]);
        }
    }
    krait_layout_remove_duplicates(st);
    krait_layout_enforce_studio(st);
    if(st->left_width < 360)
        st->left_width = 380;
    if(st->right_width < 120)
        st->right_width = 380;
    if(st->bottom_height < 100)
        st->bottom_height = 210;
    if(st->active_group < IDE_PANE_LEFT || st->active_group > IDE_PANE_BOTTOM)
        st->active_group = IDE_PANE_CENTER;
    st->resizing_pane = -1;
    st->resizing_node = -1;
    st->drag_view = -1;
    st->drag_source_group = -1;
    st->drag_active = 0;
    st->drag_drop_group = -1;
    st->drag_drop_zone = IDE_PANE_DROP_NONE;
}

static int
krait_layout_read_group(FILE *file, PaneGroup *group)
{
    char name[32];
    int active;
    int count;

    if(file == NULL || group == NULL)
        return 0;
    if(fscanf(file, "%31s %d %d", name, &active, &count) != 3)
        return 0;
    memset(group, 0, sizeof(*group));
    group->used = 1;
    group->active = active;
    if(count < 0)
        count = 0;
    if(count > IDE_MAX_PANE_TABS)
        count = IDE_MAX_PANE_TABS;
    group->tab_count = count;
    for(int i = 0; i < count; i++) {
        if(fscanf(file, "%d", &group->tabs[i]) != 1)
            return 0;
    }
    return 1;
}

static int
krait_layout_load_v1(FILE *file, IdeState *st)
{
    enum { OLD_LEFT, OLD_CENTER, OLD_RIGHT, OLD_BOTTOM };
    PaneGroup old_groups[4];
    int old_left;
    int old_right;
    int old_bottom;
    int old_active;

    if(file == NULL || st == NULL)
        return 0;
    if(fscanf(file, " sizes %d %d %d active %d",
              &old_left, &old_right, &old_bottom, &old_active) != 4)
        return 0;
    if(!krait_layout_read_group(file, &old_groups[OLD_LEFT]) ||
       !krait_layout_read_group(file, &old_groups[OLD_CENTER]) ||
       !krait_layout_read_group(file, &old_groups[OLD_RIGHT]) ||
       !krait_layout_read_group(file, &old_groups[OLD_BOTTOM]))
        return 0;
    for(int i = 0; i < 4; i++)
        krait_layout_sanitize_group(&old_groups[i]);
    if(old_groups[OLD_CENTER].tab_count <= 0) {
        old_groups[OLD_CENTER].used = 1;
        old_groups[OLD_CENTER].tabs[0] = IDE_PANE_VIEW_EDITOR;
        old_groups[OLD_CENTER].tab_count = 1;
        old_groups[OLD_CENTER].active = 0;
    }
    for(int i = 0; i < IDE_MAX_PANE_GROUPS; i++)
        krait_layout_clear_group(&st->pane_groups[i]);
    for(int i = 0; i < IDE_MAX_PANE_NODES; i++)
        krait_layout_clear_node(&st->pane_nodes[i]);
    st->pane_groups[IDE_PANE_LEFT] = old_groups[OLD_LEFT];
    st->pane_groups[IDE_PANE_CENTER] = old_groups[OLD_CENTER];
    st->pane_groups[IDE_PANE_RIGHT] = old_groups[OLD_RIGHT];
    st->pane_groups[IDE_PANE_BOTTOM] = old_groups[OLD_BOTTOM];
    st->left_width = old_left;
    st->right_width = old_right;
    st->bottom_height = old_bottom;
    st->active_group = old_active;
    return 1;
}

int
krait_layout_load(IdeState *st)
{
    char path[KRAIT_PATH_MAX];
    FILE *file;
    char magic[32];

    if(st == NULL)
        return 0;
    krait_layout_file(path, sizeof(path));
    file = fopen(path, "r");
    if(file == NULL)
        return 0;
    if(fscanf(file, "%31s", magic) != 1) {
        fclose(file);
        return 0;
    }
    if(strcmp(magic, "krait-layout-v1") == 0) {
        if(!krait_layout_load_v1(file, st)) {
            fclose(file);
            return 0;
        }
    } else {
        fclose(file);
        return 0;
    }
    fclose(file);
    krait_layout_sanitize(st);
    st->layout_dirty = 0;
    return 1;
}

static void
krait_layout_write_group(FILE *file, const char *name, const PaneGroup *group)
{
    if(file == NULL || name == NULL || group == NULL)
        return;
    fprintf(file, "%s %d %d", name, group->active, group->tab_count);
    for(int i = 0; i < group->tab_count && i < IDE_MAX_PANE_TABS; i++)
        fprintf(file, " %d", group->tabs[i]);
    fprintf(file, "\n");
}

int
krait_layout_save(IdeState *st)
{
    char path[KRAIT_PATH_MAX];
    FILE *file;

    if(st == NULL)
        return 0;
    krait_layout_sanitize(st);
    krait_layout_file(path, sizeof(path));
    krait_ensure_parent_dir(path);
    file = fopen(path, "w");
    if(file == NULL)
        return 0;
    fprintf(file, "krait-layout-v1\n");
    fprintf(file, "sizes %d %d %d active %d\n",
            st->left_width, st->right_width, st->bottom_height,
            st->active_group);
    krait_layout_write_group(file, "left", &st->pane_groups[IDE_PANE_LEFT]);
    krait_layout_write_group(file, "center", &st->pane_groups[IDE_PANE_CENTER]);
    krait_layout_write_group(file, "right", &st->pane_groups[IDE_PANE_RIGHT]);
    krait_layout_write_group(file, "bottom", &st->pane_groups[IDE_PANE_BOTTOM]);
    fclose(file);
    return 1;
}

void
krait_settings_defaults(IdeState *st)
{
    if(st == NULL)
        return;
    st->theme_source = THEME_SOURCE_APP;
    st->theme_mode = THEME_MODE_DARK;
    st->theme_id = THEME_MONO;
    st->theme_style = THEME_STYLE_SYSTEM;
    st->settings_tab = 0;
    st->settings_scroll = 0;
}

int
krait_settings_load(IdeState *st)
{
    char path[KRAIT_PATH_MAX];
    FILE *file;
    char magic[32];
    char key[64];
    int value;

    if(st == NULL)
        return 0;
    krait_settings_defaults(st);
    krait_settings_file(path, sizeof(path));
    file = fopen(path, "r");
    if(file == NULL)
        return 0;
    if(fscanf(file, "%31s", magic) != 1 ||
       strcmp(magic, "krait-settings-v1") != 0) {
        fclose(file);
        krait_settings_sanitize(st);
        return 0;
    }
    while(fscanf(file, "%63s %d", key, &value) == 2) {
        if(strcmp(key, "theme_source") == 0)
            st->theme_source = value;
        else if(strcmp(key, "theme_mode") == 0)
            st->theme_mode = value;
        else if(strcmp(key, "theme_id") == 0)
            st->theme_id = value;
        else if(strcmp(key, "theme_style") == 0)
            st->theme_style = value;
        else if(strcmp(key, "settings_tab") == 0)
            st->settings_tab = value;
        else
            (void)krait_settings_set_module(st, key, value);
    }
    fclose(file);
    krait_settings_sanitize(st);
    return 1;
}

int
krait_settings_save(IdeState *st)
{
    char path[KRAIT_PATH_MAX];
    FILE *file;

    if(st == NULL)
        return 0;
    krait_settings_sanitize(st);
    krait_settings_file(path, sizeof(path));
    krait_ensure_parent_dir(path);
    file = fopen(path, "w");
    if(file == NULL)
        return 0;
    fprintf(file, "krait-settings-v1\n");
    fprintf(file, "theme_source %d\n", st->theme_source);
    fprintf(file, "theme_mode %d\n", st->theme_mode);
    fprintf(file, "theme_id %d\n", st->theme_id);
    fprintf(file, "theme_style %d\n", st->theme_style);
    fprintf(file, "settings_tab %d\n", st->settings_tab);
    for(int i = 0; i < st->module_count; i++)
        fprintf(file, "module_%s_enabled %d\n",
                st->modules[i].id, st->modules[i].enabled ? 1 : 0);
    fclose(file);
    return 1;
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
    char makefile_path[KRAIT_PATH_MAX];
    char name[96];
    char project_file[256];
    char makefile_contents[4096];
    const char *kryon_dir;
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
        "    Background(GetThemeBackground())\n"
        "    Text(\"Hello, Kryon!\", ScaleUIPx(20), ScaleUIPx(20), UI_TEXT_24, GetThemeText())\n"
        "    Text(\"Edit main.kry and the canvas reloads.\", ScaleUIPx(20), ScaleUIPx(54), UI_TEXT_16, GetThemeIcon())\n"
        "    if Button((ButtonProps){\n"
        "        .bounds = {ScaleUIPx(20), ScaleUIPx(100), ScaleUIPx(160), ScaleUIPx(40)},\n"
        "        .label = \"Click me\",\n"
        "        .style = UI_BUTTON_STYLE_PRIMARY,\n"
        "    }) {\n"
        "        click_count++\n"
        "    }\n"
        "    Text(TextFormat(\"Clicks: %d\", click_count), ScaleUIPx(20), ScaleUIPx(160), UI_TEXT_16, GetThemeText())\n"
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
             "name \"%s\"\n",
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
    kryon_dir = getenv("KRYON_DIR");
    if(kryon_dir == NULL || kryon_dir[0] == '\0')
        kryon_dir = "/usr/home/wao/src/krait/vendor/kryon";
    snprintf(makefile_contents, sizeof(makefile_contents),
             "CC ?= cc\n"
             "KRYON_DIR ?= %s\n"
             "BUILD_DIR ?= build/kryon\n"
             "KRY_SRCS := $(wildcard *.kry)\n"
             "KC := $(KRYON_DIR)/build/bin/kc\n"
             "\n"
             ".PHONY: all clean\n"
             "\n"
             "all: $(BUILD_DIR)/.transpiled\n"
             "\n"
             "$(KC):\n"
             "\t$(MAKE) -C $(KRYON_DIR) all\n"
             "\n"
             "$(BUILD_DIR)/.transpiled: $(KRY_SRCS) | $(KC)\n"
             "\t@mkdir -p $(BUILD_DIR)\n"
             "\t$(KC) --root . -o $(BUILD_DIR) $(KRY_SRCS)\n"
             "\t@touch $@\n"
             "\n"
             "clean:\n"
             "\trm -rf build\n",
             kryon_dir);
    krait_join(makefile_path, sizeof(makefile_path), dir, "Makefile");
    if(!krait_write_text_file(makefile_path, makefile_contents)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot write %s",
                     makefile_path);
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

static int
krait_ident_start(int ch)
{
    return isalpha((unsigned char)ch) || ch == '_';
}

static int
krait_ident_char(int ch)
{
    return isalnum((unsigned char)ch) || ch == '_';
}

static int
krait_read_file_alloc(const char *path, char **out, long *out_len)
{
    FILE *file;
    long len;
    char *buf;

    if(out == NULL)
        return 0;
    *out = NULL;
    if(out_len != NULL)
        *out_len = 0;
    file = fopen(path, "rb");
    if(file == NULL)
        return 0;
    if(fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    len = ftell(file);
    if(len < 0) {
        fclose(file);
        return 0;
    }
    if(fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    buf = malloc((size_t)len + 1);
    if(buf == NULL) {
        fclose(file);
        return 0;
    }
    if(len > 0 && fread(buf, 1, (size_t)len, file) != (size_t)len) {
        free(buf);
        fclose(file);
        return 0;
    }
    fclose(file);
    buf[len] = '\0';
    *out = buf;
    if(out_len != NULL)
        *out_len = len;
    return 1;
}

static char *
krait_meta_trim(char *s)
{
    char *end;

    if(s == NULL)
        return NULL;
    while(*s != '\0' && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while(end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int
krait_meta_parse_quoted(const char **sp, char *dst, size_t dst_size)
{
    const char *p;
    size_t n = 0;

    if(sp == NULL || *sp == NULL || dst == NULL || dst_size == 0)
        return 0;
    dst[0] = '\0';
    p = *sp;
    while(*p != '\0' && isspace((unsigned char)*p))
        p++;
    if(*p != '"')
        return 0;
    p++;
    while(*p != '\0' && *p != '"') {
        int ch = *p++;
        if(ch == '\\' && *p != '\0') {
            ch = *p++;
            if(ch == 'n')
                ch = '\n';
            else if(ch == 't')
                ch = '\t';
        }
        if(n + 1 < dst_size)
            dst[n++] = (char)ch;
    }
    if(*p != '"')
        return 0;
    dst[n] = '\0';
    *sp = p + 1;
    return 1;
}

static void
krait_meta_write_quoted(FILE *file, const char *s)
{
    fputc('"', file);
    if(s != NULL) {
        for(const char *p = s; *p != '\0'; p++) {
            if(*p == '"' || *p == '\\')
                fputc('\\', file);
            if(*p == '\n')
                fputs("\\n", file);
            else if(*p == '\t')
                fputs("\\t", file);
            else
                fputc(*p, file);
        }
    }
    fputc('"', file);
}

static void
krait_meta_set_field(KraitProjectMeta *meta, const char *key, const char *value)
{
    if(strcmp(key, "app_id") == 0)
        snprintf(meta->app_id, sizeof(meta->app_id), "%s", value);
    else if(strcmp(key, "version") == 0)
        snprintf(meta->version, sizeof(meta->version), "%s", value);
    else if(strcmp(key, "icon") == 0 || strcmp(key, "logo") == 0)
        snprintf(meta->icon_path, sizeof(meta->icon_path), "%s", value);
    else if(strcmp(key, "language") == 0)
        snprintf(meta->language, sizeof(meta->language), "%s", value);
    else if(strcmp(key, "short_description") == 0)
        snprintf(meta->short_description, sizeof(meta->short_description), "%s", value);
    else if(strcmp(key, "full_description") == 0)
        snprintf(meta->full_description, sizeof(meta->full_description), "%s", value);
}

static int
krait_meta_managed_key(const char *key)
{
    return strcmp(key, "app_id") == 0 ||
           strcmp(key, "version") == 0 ||
           strcmp(key, "icon") == 0 ||
           strcmp(key, "logo") == 0 ||
           strcmp(key, "language") == 0 ||
           strcmp(key, "short_description") == 0 ||
           strcmp(key, "full_description") == 0;
}

static int
krait_meta_line_managed(char *line)
{
    char *p = krait_meta_trim(line);
    char key[128];
    const char *scan;

    if(p == NULL)
        return 0;
    if(strncmp(p, "name", 4) == 0 &&
       (p[4] == '\0' || isspace((unsigned char)p[4])))
        return 1;
    if(strncmp(p, "metadata", 8) != 0 ||
       !isspace((unsigned char)p[8]))
        return 0;
    scan = p + 8;
    if(!krait_meta_parse_quoted(&scan, key, sizeof(key)))
        return 0;
    return krait_meta_managed_key(key);
}

static int
krait_meta_read_text_file(const char *root, const char *rel,
                          char *dst, size_t dst_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;

    krait_join(path, sizeof(path), root, rel);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    snprintf(dst, dst_size, "%s", text);
    free(text);
    return 1;
}

static int
krait_meta_write_text_rel(const char *root, const char *rel, const char *text)
{
    char path[KRAIT_PATH_MAX];
    char dir[KRAIT_PATH_MAX];
    char *slash;

    krait_join(path, sizeof(path), root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if(slash != NULL) {
        *slash = '\0';
        (void)kry_fs_mkdir_p(dir);
    }
    return krait_write_text_file(path, text != NULL ? text : "");
}

static void
krait_meta_detect_languages(KraitProjectMeta *meta)
{
    char dir_path[KRAIT_PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    size_t n = 0;

    if(meta == NULL)
        return;
    meta->languages[0] = '\0';
    krait_join(dir_path, sizeof(dir_path), meta->root,
               "fastlane/metadata/android");
    dir = opendir(dir_path);
    if(dir == NULL)
        return;
    while((entry = readdir(dir)) != NULL) {
        char child[KRAIT_PATH_MAX];
        struct stat st;

        if(entry->d_name[0] == '.')
            continue;
        krait_join(child, sizeof(child), dir_path, entry->d_name);
        if(stat(child, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if(n > 0 && n + 2 < sizeof(meta->languages)) {
            meta->languages[n++] = ',';
            meta->languages[n++] = ' ';
        }
        for(const char *p = entry->d_name;
            *p != '\0' && n + 1 < sizeof(meta->languages); p++)
            meta->languages[n++] = *p;
        meta->languages[n] = '\0';
    }
    closedir(dir);
}

static void
krait_meta_import_fastlane(KraitProjectMeta *meta)
{
    char rel[KRAIT_PATH_MAX];
    char path[KRAIT_PATH_MAX];

    if(meta == NULL || meta->root[0] == '\0')
        return;
    if(meta->language[0] == '\0')
        snprintf(meta->language, sizeof(meta->language), "%s", "en-US");
    snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/short_description.txt",
             meta->language);
    (void)krait_meta_read_text_file(meta->root, rel, meta->short_description,
                                    sizeof(meta->short_description));
    snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/full_description.txt",
             meta->language);
    (void)krait_meta_read_text_file(meta->root, rel, meta->full_description,
                                    sizeof(meta->full_description));
    if(meta->icon_path[0] == '\0') {
        snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/images/icon.png",
                 meta->language);
        krait_join(path, sizeof(path), meta->root, rel);
        if(krait_path_exists(path))
            snprintf(meta->icon_path, sizeof(meta->icon_path), "%s", rel);
    }
}

static void
krait_meta_load(KraitProjectMeta *meta, const char *root)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;

    if(meta == NULL || root == NULL)
        return;
    memset(meta, 0, sizeof(*meta));
    snprintf(meta->root, sizeof(meta->root), "%s", root);
    snprintf(meta->language, sizeof(meta->language), "%s", "en-US");
    krait_join(path, sizeof(path), root, "project.kryon");
    if(krait_read_file_alloc(path, &text, &len)) {
        for(char *line = text; *line != '\0';) {
            char *end = strchr(line, '\n');
            char *p;
            int at_end = 0;

            if(end == NULL) {
                end = line + strlen(line);
                at_end = 1;
            }
            *end = '\0';
            p = krait_meta_trim(line);
            if(strncmp(p, "name", 4) == 0 &&
               (p[4] == '\0' || isspace((unsigned char)p[4]))) {
                const char *scan = p + 4;
                (void)krait_meta_parse_quoted(&scan, meta->title,
                                              sizeof(meta->title));
            } else if(strncmp(p, "metadata", 8) == 0 &&
                      isspace((unsigned char)p[8])) {
                const char *scan = p + 8;
                char key[128];
                char value[4096];

                if(krait_meta_parse_quoted(&scan, key, sizeof(key)) &&
                   krait_meta_parse_quoted(&scan, value, sizeof(value)))
                    krait_meta_set_field(meta, key, value);
            }
            if(at_end)
                break;
            line = end + 1;
        }
        free(text);
    }
    if(meta->title[0] == '\0')
        snprintf(meta->title, sizeof(meta->title), "%s", krait_basename(root));
    krait_meta_detect_languages(meta);
    if(meta->short_description[0] == '\0' && meta->full_description[0] == '\0')
        krait_meta_import_fastlane(meta);
    meta->loaded = 1;
}

static int
krait_meta_save(KraitProjectMeta *meta, char *status, int status_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    FILE *file;

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Metadata save failed");
    if(meta == NULL || meta->root[0] == '\0')
        return 0;
    krait_join(path, sizeof(path), meta->root, "project.kryon");
    (void)krait_read_file_alloc(path, &text, &len);
    file = fopen(path, "wb");
    if(file == NULL) {
        free(text);
        return 0;
    }
    fputs("name ", file);
    krait_meta_write_quoted(file, meta->title);
    fputc('\n', file);
    if(text != NULL) {
        for(char *line = text; *line != '\0';) {
            char *end = strchr(line, '\n');
            int at_end = 0;

            if(end == NULL) {
                end = line + strlen(line);
                at_end = 1;
            }
            *end = '\0';
            if(line[0] != '\0' && !krait_meta_line_managed(line))
                fprintf(file, "%s\n", line);
            if(at_end)
                break;
            line = end + 1;
        }
        free(text);
    }
    fputs("metadata \"app_id\" ", file);
    krait_meta_write_quoted(file, meta->app_id);
    fputc('\n', file);
    fputs("metadata \"version\" ", file);
    krait_meta_write_quoted(file, meta->version);
    fputc('\n', file);
    fputs("metadata \"icon\" ", file);
    krait_meta_write_quoted(file, meta->icon_path);
    fputc('\n', file);
    fputs("metadata \"language\" ", file);
    krait_meta_write_quoted(file, meta->language);
    fputc('\n', file);
    fputs("metadata \"short_description\" ", file);
    krait_meta_write_quoted(file, meta->short_description);
    fputc('\n', file);
    fputs("metadata \"full_description\" ", file);
    krait_meta_write_quoted(file, meta->full_description);
    fputc('\n', file);
    fclose(file);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Metadata saved");
    return 1;
}

static int
krait_meta_export_fastlane(KraitProjectMeta *meta, char *status, int status_size)
{
    char rel[KRAIT_PATH_MAX];

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Fastlane export failed");
    if(meta == NULL || meta->root[0] == '\0')
        return 0;
    if(meta->language[0] == '\0')
        snprintf(meta->language, sizeof(meta->language), "%s", "en-US");
    snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/short_description.txt",
             meta->language);
    if(!krait_meta_write_text_rel(meta->root, rel, meta->short_description))
        return 0;
    snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/full_description.txt",
             meta->language);
    if(!krait_meta_write_text_rel(meta->root, rel, meta->full_description))
        return 0;
    krait_meta_detect_languages(meta);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Fastlane metadata exported");
    return 1;
}

static UITextInputStyle
krait_meta_input_style(void)
{
    return (UITextInputStyle){
        GetThemeSurface(),
        GetThemeButton(),
        GetThemeLink(),
        GetThemeText(),
        GetThemeLink(),
        0,
        ScaleUIPx(8),
        ScaleUIPx(6)
    };
}

static void
krait_meta_field(Rectangle rect, const char *label, char *text,
                 size_t text_size, int *cursor, int *focused, int id)
{
    DrawUIText(label, (int)rect.x, (int)rect.y, ScaleUIPx(12),
               GetThemeIcon());
    DrawUITextField((TextFieldProps){
        (Rectangle){rect.x, rect.y + (float)ScaleUIPx(18),
                    rect.width, (float)ScaleUIPx(32)},
        text, text_size, cursor, focused, (int)text_size - 1,
        ScaleUIPx(13), id, krait_meta_input_style(),
        (UITextInputFilter){0}, NULL, NULL
    });
}

void
krait_draw_project_metadata(Rectangle bounds, IdeState *st)
{
    KraitProjectMeta *meta = &g_project_meta;
    int pad = ScaleUIPx(12);
    int x = (int)bounds.x + pad;
    int y = (int)bounds.y + pad;
    int w = (int)bounds.width - pad * 2;
    char status[256];

    DrawRectangleRec(bounds, GetThemeSurface());
    if(st == NULL || !st->project.loaded) {
        DrawUIText("No project loaded", x, y, ScaleUIPx(14), GetThemeText());
        return;
    }
    if(!meta->loaded || strcmp(meta->root, st->project.path) != 0)
        krait_meta_load(meta, st->project.path);
    DrawUIText("Project Metadata", x, y, ScaleUIPx(16), GetThemeText());
    y += ScaleUIPx(30);
    krait_meta_field((Rectangle){x, y, w, 0}, "Title",
                     meta->title, sizeof(meta->title),
                     &meta->cursor[0], &meta->focused[0], 7100);
    y += ScaleUIPx(58);
    krait_meta_field((Rectangle){x, y, w, 0}, "App ID",
                     meta->app_id, sizeof(meta->app_id),
                     &meta->cursor[1], &meta->focused[1], 7101);
    y += ScaleUIPx(58);
    krait_meta_field((Rectangle){x, y, w, 0}, "Version",
                     meta->version, sizeof(meta->version),
                     &meta->cursor[2], &meta->focused[2], 7102);
    y += ScaleUIPx(58);
    krait_meta_field((Rectangle){x, y, w, 0}, "Logo/Icon Path",
                     meta->icon_path, sizeof(meta->icon_path),
                     &meta->cursor[3], &meta->focused[3], 7103);
    y += ScaleUIPx(58);
    krait_meta_field((Rectangle){x, y, w, 0}, "Language",
                     meta->language, sizeof(meta->language),
                     &meta->cursor[4], &meta->focused[4], 7104);
    y += ScaleUIPx(54);
    if(meta->languages[0] != '\0') {
        char line[640];
        snprintf(line, sizeof(line), "Detected: %s", meta->languages);
        DrawUIText(line, x, y, ScaleUIPx(11), GetThemeIcon());
        y += ScaleUIPx(18);
    }
    krait_meta_field((Rectangle){x, y, w, 0}, "Short Description",
                     meta->short_description, sizeof(meta->short_description),
                     &meta->cursor[5], &meta->focused[5], 7105);
    y += ScaleUIPx(58);
    DrawUIText("Full Description", x, y, ScaleUIPx(12), GetThemeIcon());
    y += ScaleUIPx(18);
    DrawUITextArea((TextAreaProps){
        (Rectangle){x, y, w, (float)ScaleUIPx(130)},
        meta->full_description, sizeof(meta->full_description),
        &meta->cursor[6], &meta->focused[6], &meta->full_scroll,
        (int)sizeof(meta->full_description) - 1, ScaleUIPx(13),
        ScaleUIPx(4), 7106, "", UI_SYNTAX_NONE,
        krait_meta_input_style(), (UITextInputFilter){0}, NULL
    });
    y += ScaleUIPx(142);
    if(DrawUIGenericButton(x, y, ScaleUIPx(56), ScaleUIPx(28), "Save",
                           UI_BUTTON_STYLE_PRIMARY, 0, NULL)) {
        if(krait_meta_save(meta, status, sizeof(status)))
            snprintf(st->project.name, sizeof(st->project.name), "%s", meta->title);
        snprintf(st->status, sizeof(st->status), "%s", status);
    }
    if(DrawUIGenericButton(x + ScaleUIPx(64), y, ScaleUIPx(92), ScaleUIPx(28),
                           "Import", UI_BUTTON_STYLE_SECONDARY, 0, NULL)) {
        krait_meta_import_fastlane(meta);
        snprintf(st->status, sizeof(st->status), "%s", "Fastlane metadata imported");
    }
    if(DrawUIGenericButton(x + ScaleUIPx(164), y, ScaleUIPx(92), ScaleUIPx(28),
                           "Export", UI_BUTTON_STYLE_SECONDARY, 0, NULL)) {
        (void)krait_meta_export_fastlane(meta, status, sizeof(status));
        snprintf(st->status, sizeof(st->status), "%s", status);
    }
    if(DrawUIGenericButton(x + ScaleUIPx(264), y, ScaleUIPx(72), ScaleUIPx(28),
                           "Reload", UI_BUTTON_STYLE_SECONDARY, 0, NULL)) {
        krait_meta_load(meta, st->project.path);
        snprintf(st->status, sizeof(st->status), "%s", "Metadata reloaded");
    }
}

static int
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

static const char *
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

static int
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
        p = krait_meta_trim(line);
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

typedef struct {
    const char *root;
    const char *rel_path;
    const char *text;
    const char *body;
    const char *body_end;
    int ok;
    int line_no;
    int render_count;
    int delegate_count;
    int call_depth;
    char status[512];
} KraitLive;

typedef struct {
    char name[64];
    int value;
} KraitLiveVar;

static KraitLiveVar g_krait_live_vars[128];
static int g_krait_live_var_count = 0;

static void
krait_live_vars_clear(void)
{
    memset(g_krait_live_vars, 0, sizeof(g_krait_live_vars));
    g_krait_live_var_count = 0;
}

static int
krait_live_var_get(const char *name, int *out)
{
    if(name == NULL || out == NULL)
        return 0;
    for(int i = g_krait_live_var_count - 1; i >= 0; i--) {
        if(strcmp(g_krait_live_vars[i].name, name) == 0) {
            *out = g_krait_live_vars[i].value;
            return 1;
        }
    }
    return 0;
}

static void
krait_live_var_set(const char *name, int value)
{
    if(name == NULL || name[0] == '\0')
        return;
    for(int i = 0; i < g_krait_live_var_count; i++) {
        if(strcmp(g_krait_live_vars[i].name, name) == 0) {
            g_krait_live_vars[i].value = value;
            return;
        }
    }
    if(g_krait_live_var_count >= (int)(sizeof(g_krait_live_vars) / sizeof(g_krait_live_vars[0])))
        return;
    snprintf(g_krait_live_vars[g_krait_live_var_count].name,
             sizeof(g_krait_live_vars[g_krait_live_var_count].name), "%s", name);
    g_krait_live_vars[g_krait_live_var_count].value = value;
    g_krait_live_var_count++;
}

static void
krait_live_status(KraitLive *live, const char *fmt, ...)
{
    va_list ap;

    if(live == NULL || live->status[0] != '\0')
        return;
    va_start(ap, fmt);
    vsnprintf(live->status, sizeof(live->status), fmt, ap);
    va_end(ap);
    live->ok = 0;
}

static const char *
krait_live_skip_space(const char *p)
{
    while(p != NULL && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

static int
krait_live_starts_word(const char *p, const char *word)
{
    size_t n;

    if(p == NULL || word == NULL)
        return 0;
    p = krait_live_skip_space(p);
    n = strlen(word);
    if(strncmp(p, word, n) != 0)
        return 0;
    return !krait_ident_char((unsigned char)p[n]);
}

static char *
krait_live_trim(char *s)
{
    char *end;

    if(s == NULL)
        return s;
    while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + strlen(s);
    while(end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                      end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static int
krait_live_parse_string(char **sp, char *dst, size_t dst_size)
{
    char *p = krait_live_trim(*sp);
    size_t n = 0;

    if(*p != '"')
        return 0;
    p++;
    while(*p != '\0' && *p != '"') {
        int ch = *p++;
        if(ch == '\\' && *p != '\0') {
            ch = *p++;
            if(ch == 'n')
                ch = '\n';
            else if(ch == 't')
                ch = '\t';
        }
        if(n + 1 < dst_size)
            dst[n++] = (char)ch;
    }
    if(*p != '"')
        return 0;
    if(dst_size > 0)
        dst[n] = '\0';
    *sp = p + 1;
    return 1;
}

static int
krait_live_split_args(const char *src, char args[][256], int cap)
{
    int count = 0;
    int depth = 0;
    int in_string = 0;
    char cur[256];
    int n = 0;

    memset(cur, 0, sizeof(cur));
    for(const char *p = src; *p != '\0'; p++) {
        int ch = *p;
        if(in_string) {
            if(ch == '\\' && p[1] != '\0') {
                if(n + 1 < (int)sizeof(cur))
                    cur[n++] = (char)ch;
                ch = *++p;
            } else if(ch == '"') {
                in_string = 0;
            }
        } else if(ch == '"') {
            in_string = 1;
        } else if(ch == '(' || ch == '{' || ch == '[') {
            depth++;
        } else if(ch == ')' || ch == '}' || ch == ']') {
            depth--;
        } else if(ch == ',' && depth == 0) {
            if(count < cap) {
                cur[n] = '\0';
                snprintf(args[count], 256, "%s", krait_live_trim(cur));
                count++;
            }
            n = 0;
            cur[0] = '\0';
            continue;
        }
        if(n + 1 < (int)sizeof(cur))
            cur[n++] = (char)ch;
    }
    if(n > 0 || src[0] != '\0') {
        if(count < cap) {
            cur[n] = '\0';
            snprintf(args[count], 256, "%s", krait_live_trim(cur));
            count++;
        }
    }
    return count;
}

static int
krait_live_call_args(const char *line, const char *name, char args[][256], int cap)
{
    const char *p;
    const char *open;
    const char *close;
    char inner[1024];
    size_t n;

    p = krait_live_skip_space(line);
    if(strncmp(p, name, strlen(name)) != 0)
        return -1;
    p += strlen(name);
    p = krait_live_skip_space(p);
    if(*p != '(')
        return -1;
    open = p + 1;
    close = strrchr(open, ')');
    if(close == NULL || close < open)
        return -1;
    n = (size_t)(close - open);
    if(n >= sizeof(inner))
        n = sizeof(inner) - 1;
    memcpy(inner, open, n);
    inner[n] = '\0';
    return krait_live_split_args(inner, args, cap);
}

static int
krait_live_eval_int(const char *expr, int *out)
{
    char buf[256];
    char *p;
    char *end;

    if(expr == NULL || out == NULL)
        return 0;
    snprintf(buf, sizeof(buf), "%s", expr);
    p = krait_live_trim(buf);
    if(p[0] == '(') {
        char *close = strrchr(p + 1, ')');
        if(close != NULL && close[1] == '\0') {
            *close = '\0';
            return krait_live_eval_int(p + 1, out);
        }
    }
    if(strncmp(p, "ScaleUIPx(", 10) == 0) {
        char *q = p + 10;
        char *close = strrchr(q, ')');
        int v = 0;
        if(close == NULL)
            return 0;
        *close = '\0';
        if(!krait_live_eval_int(q, &v))
            return 0;
        *out = ScaleUIPx(v);
        return 1;
    }
    if(strcmp(p, "UI_TEXT_12") == 0) {
        *out = UI_TEXT_12;
        return 1;
    }
    if(strcmp(p, "UI_TEXT_16") == 0) {
        *out = UI_TEXT_16;
        return 1;
    }
    if(strcmp(p, "UI_TEXT_24") == 0) {
        *out = UI_TEXT_24;
        return 1;
    }
    if(strcmp(p, "view_width") == 0 || strcmp(p, "GetScreenWidth()") == 0) {
        return krait_live_var_get("view_width", out);
    }
    if(strcmp(p, "view_height") == 0 || strcmp(p, "GetScreenHeight()") == 0) {
        return krait_live_var_get("view_height", out);
    }
    if(krait_live_var_get(p, out))
        return 1;
    if(strchr(p, '(') != NULL && strchr(p, ')') != NULL) {
        if(strstr(p, "height") != NULL || strstr(p, "Height") != NULL) {
            *out = ScaleUIPx(140);
            return 1;
        }
        if(strstr(p, "width") != NULL || strstr(p, "Width") != NULL) {
            *out = ScaleUIPx(280);
            return 1;
        }
        if(strstr(p, "top") != NULL || strstr(p, "Top") != NULL) {
            *out = ScaleUIPx(48);
            return 1;
        }
        *out = ScaleUIPx(24);
        return 1;
    }
    if(strstr(p, "+") != NULL) {
        char *op = strchr(p, '+');
        int a = 0, b = 0;
        *op = '\0';
        if(krait_live_eval_int(p, &a) && krait_live_eval_int(op + 1, &b)) {
            *out = a + b;
            return 1;
        }
        return 0;
    }
    if(strstr(p, "*") != NULL) {
        char *op = strchr(p, '*');
        int a = 0, b = 0;
        *op = '\0';
        if(krait_live_eval_int(p, &a) && krait_live_eval_int(op + 1, &b)) {
            *out = a * b;
            return 1;
        }
        return 0;
    }
    if(strstr(p, "/") != NULL) {
        char *op = strchr(p, '/');
        int a = 0, b = 0;
        *op = '\0';
        if(krait_live_eval_int(p, &a) && krait_live_eval_int(op + 1, &b) && b != 0) {
            *out = a / b;
            return 1;
        }
        return 0;
    }
    if(strstr(p, "-") != NULL && p[0] != '-') {
        char *op = strchr(p, '-');
        int a = 0, b = 0;
        *op = '\0';
        if(krait_live_eval_int(p, &a) && krait_live_eval_int(op + 1, &b)) {
            *out = a - b;
            return 1;
        }
        return 0;
    }
    *out = (int)strtol(p, &end, 10);
    return end != p && *krait_live_trim(end) == '\0';
}

static int
krait_live_next_scale_arg(char **sp, int *out)
{
    char *p;
    char *q;
    char buf[128];
    size_t n;

    if(sp == NULL || *sp == NULL || out == NULL)
        return 0;
    p = strstr(*sp, "ScaleUIPx(");
    if(p == NULL)
        return 0;
    p += 10;
    q = strchr(p, ')');
    if(q == NULL)
        return 0;
    n = (size_t)(q - p);
    if(n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, p, n);
    buf[n] = '\0';
    *sp = q + 1;
    return krait_live_eval_int(buf, out);
}

static int
krait_live_parse_ident(char **sp, char *dst, size_t dst_size)
{
    char *p;
    size_t n = 0;

    if(sp == NULL || *sp == NULL || dst == NULL || dst_size == 0)
        return 0;
    p = krait_live_trim(*sp);
    if(!krait_ident_start((unsigned char)*p))
        return 0;
    while(krait_ident_char((unsigned char)*p)) {
        if(n + 1 < dst_size)
            dst[n++] = *p;
        p++;
    }
    dst[n] = '\0';
    *sp = p;
    return n > 0;
}

static int
krait_live_eval_color(const char *expr, Color *out)
{
    char buf[256];
    char *p;

    if(expr == NULL || out == NULL)
        return 0;
    snprintf(buf, sizeof(buf), "%s", expr);
    p = krait_live_trim(buf);
    if(strcmp(p, "GetThemeText()") == 0) {
        *out = GetThemeText();
        return 1;
    }
    if(strcmp(p, "GetThemeIcon()") == 0) {
        *out = GetThemeIcon();
        return 1;
    }
    if(strcmp(p, "GetThemeBackground()") == 0) {
        *out = GetThemeBackground();
        return 1;
    }
    if(strcmp(p, "GetThemeSurface()") == 0) {
        *out = GetThemeSurface();
        return 1;
    }
    if(strcmp(p, "GetThemeButton()") == 0) {
        *out = GetThemeButton();
        return 1;
    }
    if(strcmp(p, "GetThemeButtonHover()") == 0) {
        *out = GetThemeButtonHover();
        return 1;
    }
    if(strcmp(p, "GetThemeLink()") == 0) {
        *out = GetThemeLink();
        return 1;
    }
    if(strcmp(p, "WHITE") == 0) {
        *out = WHITE;
        return 1;
    }
    if(strcmp(p, "BLACK") == 0) {
        *out = BLACK;
        return 1;
    }
    if(strcmp(p, "BLANK") == 0) {
        *out = BLANK;
        return 1;
    }
    if(strncmp(p, "Fade(", 5) == 0) {
        char *inner = p + 5;
        char *comma = strrchr(inner, ',');
        char *close = strrchr(inner, ')');
        float alpha;
        Color base;

        if(comma == NULL || close == NULL || comma > close)
            return 0;
        *comma = '\0';
        alpha = (float)strtod(comma + 1, NULL);
        if(!krait_live_eval_color(inner, &base))
            return 0;
        *out = Fade(base, alpha);
        return 1;
    }
    return 0;
}

static UIButtonStyle
krait_live_eval_button_style(const char *expr)
{
    if(expr != NULL && strstr(expr, "SECONDARY") != NULL)
        return UI_BUTTON_STYLE_SECONDARY;
    if(expr != NULL && strstr(expr, "DANGER") != NULL)
        return UI_BUTTON_STYLE_DANGER;
    if(expr != NULL && strstr(expr, "TAB_SELECTED") != NULL)
        return UI_BUTTON_STYLE_TAB_SELECTED;
    if(expr != NULL && strstr(expr, "TAB") != NULL)
        return UI_BUTTON_STYLE_TAB;
    return UI_BUTTON_STYLE_PRIMARY;
}

static int
krait_live_exec_assignment(char *line)
{
    char *p;
    char *op;
    char name[64];
    int value = 0;
    int current = 0;
    int add = 0;
    int sub = 0;
    size_t n;

    if(line == NULL)
        return 0;
    p = krait_live_trim(line);
    if(!krait_ident_char((unsigned char)p[0]) || isdigit((unsigned char)p[0]))
        return 0;
    n = 0;
    while(krait_ident_char((unsigned char)p[n]) && n + 1 < sizeof(name)) {
        name[n] = p[n];
        n++;
    }
    name[n] = '\0';
    p = krait_live_trim(p + n);
    if(p[0] == ':' && p[1] != ':') {
        while(*p != '\0' && *p != '=')
            p++;
    }
    if(p[0] == ':' && p[1] == '=') {
        op = p + 2;
    } else if(p[0] == '+' && p[1] == '=') {
        add = 1;
        op = p + 2;
    } else if(p[0] == '-' && p[1] == '=') {
        sub = 1;
        op = p + 2;
    } else if(p[0] == '=') {
        op = p + 1;
    } else {
        return 0;
    }
    op = krait_live_trim(op);
    if(strncmp(op, "(float)", 7) == 0)
        op = krait_live_trim(op + 7);
    if(!krait_live_eval_int(op, &value))
        return 1;
    if(add || sub)
        (void)krait_live_var_get(name, &current);
    if(add)
        value = current + value;
    else if(sub)
        value = current - value;
    krait_live_var_set(name, value);
    return 1;
}

static int
krait_live_find_named_body(const char *text, const char *keyword,
                           const char *name, const char **body,
                           const char **body_end)
{
    const char *p = text;
    size_t keyword_len = strlen(keyword);

    while((p = strstr(p, keyword)) != NULL) {
        const char *q;
        const char *open;
        int depth = 1;

        if(p > text && krait_ident_char((unsigned char)p[-1])) {
            p += keyword_len;
            continue;
        }
        q = krait_live_skip_space(p + keyword_len);
        if(name != NULL && name[0] != '\0') {
            size_t name_len = strlen(name);
            if(strncmp(q, name, name_len) != 0 ||
               krait_ident_char((unsigned char)q[name_len])) {
                p += keyword_len;
                continue;
            }
        }
        open = strchr(q, '{');
        if(open == NULL)
            return 0;
        for(q = open + 1; *q != '\0'; q++) {
            if(*q == '{')
                depth++;
            else if(*q == '}') {
                depth--;
                if(depth == 0) {
                    *body = open + 1;
                    *body_end = q;
                    return 1;
                }
            }
        }
        return 0;
    }
    return 0;
}

static int
krait_live_find_function_body(const char *text, const char *name,
                              const char **body, const char **body_end)
{
    const char *p;
    size_t name_len;

    if(text == NULL || name == NULL || name[0] == '\0')
        return 0;
    name_len = strlen(name);
    p = text;
    while((p = strstr(p, name)) != NULL) {
        const char *q;
        const char *open;
        int depth = 1;

        if(p > text && krait_ident_char((unsigned char)p[-1])) {
            p += name_len;
            continue;
        }
        if(krait_ident_char((unsigned char)p[name_len])) {
            p += name_len;
            continue;
        }
        q = krait_live_skip_space(p + name_len);
        if(q[0] != ':' || q[1] != ':') {
            p += name_len;
            continue;
        }
        open = strchr(q, '{');
        if(open == NULL)
            return 0;
        for(q = open + 1; *q != '\0'; q++) {
            if(*q == '{')
                depth++;
            else if(*q == '}') {
                depth--;
                if(depth == 0) {
                    *body = open + 1;
                    *body_end = q;
                    return 1;
                }
            }
        }
        return 0;
    }
    return 0;
}

static int
krait_live_line_for_ptr(const char *text, const char *ptr)
{
    int line = 1;

    if(text == NULL || ptr == NULL)
        return 1;
    for(const char *p = text; p < ptr && *p != '\0'; p++)
        if(*p == '\n')
            line++;
    return line;
}

static int
krait_live_find_frame_name(const char *text, char *dst, size_t dst_size)
{
    const char *app = strstr(text, "app ");
    const char *body;
    const char *end;

    if(dst_size > 0)
        dst[0] = '\0';
    if(app == NULL || !krait_live_find_named_body(app, "app", "", &body, &end))
        return 0;
    for(const char *p = body; p < end;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        char line[256];
        size_t n = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
        char *q;
        if(n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        q = krait_live_trim(line);
        if(krait_live_starts_word(q, "frame")) {
            q = krait_live_trim(q + strlen("frame"));
            if(krait_live_parse_ident(&q, dst, dst_size))
                return 1;
        }
        if(nl == NULL)
            break;
        p = nl + 1;
    }
    return 0;
}

static int krait_live_exec_body(KraitLive *live);

static int
krait_live_exec_local_call(KraitLive *live, const char *line)
{
    char name[128];
    const char *body;
    const char *body_end;
    const char *saved_body;
    const char *saved_body_end;
    int saved_line;
    char *scan;
    char buf[512];

    if(live == NULL || line == NULL || live->call_depth >= 12)
        return 0;
    snprintf(buf, sizeof(buf), "%s", line);
    scan = krait_live_trim(buf);
    if(krait_live_starts_word(scan, "unused"))
        scan = krait_live_trim(scan + strlen("unused"));
    if(!krait_live_parse_ident(&scan, name, sizeof(name)))
        return 0;
    scan = krait_live_trim(scan);
    if(scan[0] != '(')
        return 0;
    if(!krait_live_find_function_body(live->text, name, &body, &body_end))
        return 0;

    saved_body = live->body;
    saved_body_end = live->body_end;
    saved_line = live->line_no;
    live->body = body;
    live->body_end = body_end;
    live->line_no = krait_live_line_for_ptr(live->text, body);
    live->call_depth++;
    krait_live_exec_body(live);
    live->call_depth--;
    live->body = saved_body;
    live->body_end = saved_body_end;
    live->line_no = saved_line;
    return live->ok;
}

static int
krait_game_node_args(const char *q, char args[8][256], char *type,
                     size_t type_size, char *label, size_t label_size,
                     int *x, int *y, int *w, int *h)
{
    static const char *names[] = {
        "PlayerSpawn",
        "MobSpawn",
        "TerrainPlatform",
        "Pickup",
        "TriggerArea",
        "WeatherConfig",
        "StageExit",
        "CameraBounds",
        "WaterBlob",
        "Pond",
        "TreeSpawner",
        "CaveMushroom",
        "CaveSpider",
        "CaveTurtle"
    };

    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char *s;
        int argc = krait_live_call_args(q, names[i], args, 8);

        if(argc != 5)
            continue;
        s = args[0];
        if(!krait_live_parse_string(&s, label, label_size) ||
           !krait_live_eval_int(args[1], x) ||
           !krait_live_eval_int(args[2], y) ||
           !krait_live_eval_int(args[3], w) ||
           !krait_live_eval_int(args[4], h))
            return 0;
        snprintf(type, type_size, "%s", names[i]);
        return 1;
    }
    return 0;
}

static Color
krait_game_node_color(const char *type, int outline)
{
    if(strcmp(type, "PlayerSpawn") == 0)
        return outline ? (Color){80, 120, 210, 255} :
                         (Color){226, 234, 245, 255};
    if(strcmp(type, "MobSpawn") == 0 || strcmp(type, "CaveSpider") == 0 ||
       strcmp(type, "CaveTurtle") == 0)
        return outline ? (Color){230, 130, 90, 255} :
                         (Color){120, 66, 58, 255};
    if(strcmp(type, "Pickup") == 0 || strcmp(type, "WaterBlob") == 0 ||
       strcmp(type, "Pond") == 0)
        return outline ? (Color){135, 220, 245, 255} :
                         (Color){60, 120, 130, 255};
    if(strcmp(type, "TerrainPlatform") == 0)
        return outline ? (Color){145, 140, 160, 255} :
                         (Color){96, 94, 108, 255};
    if(strcmp(type, "TreeSpawner") == 0 ||
       strcmp(type, "CaveMushroom") == 0)
        return outline ? (Color){42, 94, 58, 255} :
                         (Color){91, 118, 76, 255};
    return outline ? (Color){230, 178, 83, 255} :
                     (Color){230, 178, 83, 96};
}

static void
krait_draw_game_node(const char *type, const char *label,
                     int x, int y, int w, int h)
{
    Color fill = krait_game_node_color(type, 0);
    Color outline = krait_game_node_color(type, 1);

    Rect(x, y, w, h, fill, outline);
    Text(label != NULL && label[0] != '\0' ? label : type,
         x + ScaleUIPx(6), y + ScaleUIPx(6), UI_TEXT_12, outline);
}

static int
krait_live_exec_call(KraitLive *live, char *line)
{
    char args[8][256];
    int argc;
    char label[512];
    int x, y, w, h, font;
    Color color, border;
    char *q;
    char *scan;
    char node_type[64];

    q = krait_live_trim(line);
    if(q[0] == '\0' || q[0] == '#')
        return 1;
    if(strcmp(q, "BeginDrawing()") == 0 || strcmp(q, "EndDrawing()") == 0 ||
       strncmp(q, "BeginUIFrame(", 13) == 0 ||
       strcmp(q, "EndUIFocus()") == 0 || strcmp(q, "EndDrawing()") == 0)
        return 1;
    if(krait_live_starts_word(q, "args")) {
        live->delegate_count++;
        return 1;
    }
    if(krait_live_exec_assignment(q))
        return 1;
    if(strstr(q, " :: ") != NULL || strstr(q, ": ") != NULL ||
       strstr(q, ": [") != NULL || strcmp(q, "{") == 0 || strcmp(q, "}") == 0 ||
       strcmp(q, "} else {") == 0 || krait_live_starts_word(q, "return") ||
       krait_live_starts_word(q, "for") || krait_live_starts_word(q, "while"))
        return 1;
    argc = krait_live_call_args(q, "Background", args, 8);
    if(argc == 1) {
        if(!krait_live_eval_color(args[0], &color)) {
            krait_live_status(live, "%s:%d: unsupported color: %s",
                              live->rel_path, live->line_no, args[0]);
            return 0;
        }
        Background(color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "ClearBackground", args, 8);
    if(argc == 1) {
        if(krait_live_eval_color(args[0], &color))
            ClearBackground(color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Text", args, 8);
    if(argc == 5) {
        char *s = args[0];
        if(!krait_live_parse_string(&s, label, sizeof(label)) ||
           !krait_live_eval_int(args[1], &x) ||
           !krait_live_eval_int(args[2], &y) ||
           !krait_live_eval_int(args[3], &font) ||
           !krait_live_eval_color(args[4], &color)) {
            krait_live_status(live, "%s:%d: unsupported Text arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        Text(label, x, y, font, color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "DrawText", args, 8);
    if(argc == 5) {
        char *s = args[0];
        if(!krait_live_parse_string(&s, label, sizeof(label)) ||
           !krait_live_eval_int(args[1], &x) ||
           !krait_live_eval_int(args[2], &y) ||
           !krait_live_eval_int(args[3], &font) ||
           !krait_live_eval_color(args[4], &color)) {
            krait_live_status(live, "%s:%d: unsupported DrawText arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        DrawUIText(label, x, y, font, color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Rect", args, 8);
    if(argc == 6) {
        if(!krait_live_eval_int(args[0], &x) ||
           !krait_live_eval_int(args[1], &y) ||
           !krait_live_eval_int(args[2], &w) ||
           !krait_live_eval_int(args[3], &h) ||
           !krait_live_eval_color(args[4], &color) ||
           !krait_live_eval_color(args[5], &border)) {
            krait_live_status(live, "%s:%d: unsupported Rect arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        Rect(x, y, w, h, color, border);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Button", args, 8);
    if(argc == 1) {
        char *s = strchr(args[0], '"');
        scan = args[0];
        if(!krait_live_next_scale_arg(&scan, &x) ||
           !krait_live_next_scale_arg(&scan, &y) ||
           !krait_live_next_scale_arg(&scan, &w) ||
           !krait_live_next_scale_arg(&scan, &h) ||
           s == NULL ||
           !krait_live_parse_string(&s, label, sizeof(label))) {
            krait_live_status(live, "%s:%d: unsupported Button arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        DrawUIGenericButton(x, y, w, h, label,
                            krait_live_eval_button_style(args[0]), 0, NULL);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Picture", args, 8);
    if(argc == 1) {
        char asset[512];
        char *s = strchr(args[0], '"');
        scan = args[0];

        if(s == NULL ||
           !krait_live_parse_string(&s, asset, sizeof(asset)) ||
           !krait_live_next_scale_arg(&scan, &x) ||
           !krait_live_next_scale_arg(&scan, &y) ||
           !krait_live_next_scale_arg(&scan, &w) ||
           !krait_live_next_scale_arg(&scan, &h)) {
            krait_live_status(live, "%s:%d: unsupported Picture arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        Picture((PictureProps){asset, {x, y, w, h}, {0}, {0}, 0, WHITE,
                                    UI_PICTURE_FIT_STRETCH});
        live->render_count++;
        return 1;
    }
    if(krait_game_node_args(q, args, node_type, sizeof(node_type),
                            label, sizeof(label), &x, &y, &w, &h)) {
        krait_draw_game_node(node_type, label, x, y, w, h);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "DrawUITextField", args, 8);
    if(argc == 1) {
        scan = args[0];
        if(!krait_live_next_scale_arg(&scan, &x) ||
           !krait_live_next_scale_arg(&scan, &y) ||
           !krait_live_next_scale_arg(&scan, &w) ||
           !krait_live_next_scale_arg(&scan, &h)) {
            krait_live_status(live, "%s:%d: unsupported DrawUITextField arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        Rect(x, y, w, h, GetThemeSurface(), GetThemeButton());
        Text("Text field", x + ScaleUIPx(10), y + ScaleUIPx(8),
                   UI_TEXT_16, GetThemeIcon());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "DrawUIToggleSwitch", args, 8);
    if(argc >= 7) {
        char off_label[128] = "Off";
        char on_label[128] = "On";
        char *off = args[5];
        char *on = args[6];

        if(!krait_live_eval_int(args[0], &x) ||
           !krait_live_eval_int(args[1], &y) ||
           !krait_live_eval_int(args[2], &w) ||
           !krait_live_eval_int(args[3], &h)) {
            krait_live_status(live, "%s:%d: unsupported DrawUIToggleSwitch arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        (void)krait_live_parse_string(&off, off_label, sizeof(off_label));
        (void)krait_live_parse_string(&on, on_label, sizeof(on_label));
        Rect(x, y, w, h, GetThemeButton(), GetThemeButtonHover());
        Text(off_label, x + ScaleUIPx(8), y + ScaleUIPx(6),
                   UI_TEXT_12, GetThemeText());
        Text(on_label, x + w - ScaleUIPx(32), y + ScaleUIPx(6),
                   UI_TEXT_12, GetThemeIcon());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "DrawUISlider", args, 8);
    if(argc >= 8) {
        char slider_label[128] = "Value";
        char *s = args[4];

        if(!krait_live_eval_int(args[1], &x) ||
           !krait_live_eval_int(args[2], &y) ||
           !krait_live_eval_int(args[3], &w)) {
            krait_live_status(live, "%s:%d: unsupported DrawUISlider arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        (void)krait_live_parse_string(&s, slider_label, sizeof(slider_label));
        Text(slider_label, x, y - ScaleUIPx(18), UI_TEXT_12, GetThemeText());
        Rect(x, y, w, ScaleUIPx(8), GetThemeButton(), GetThemeButtonHover());
        Rect(x + w / 2 - ScaleUIPx(6), y - ScaleUIPx(5),
                   ScaleUIPx(12), ScaleUIPx(18), GetThemeLink(), GetThemeLink());
        live->render_count++;
        return 1;
    }
    if(krait_live_starts_word(q, "if")) {
        q = krait_live_trim(q + 2);
        argc = krait_live_call_args(q, "Button", args, 8);
        if(argc == 1)
            return krait_live_exec_call(live, q);
        return 1;
    }
    if(krait_live_exec_local_call(live, q))
        return 1;
    live->delegate_count++;
    return 1;
}

static int
krait_live_statement_open(const char *stmt)
{
    int depth = 0;
    int in_string = 0;

    if(stmt == NULL)
        return 0;
    for(const char *p = stmt; *p != '\0'; p++) {
        int ch = *p;

        if(in_string) {
            if(ch == '\\' && p[1] != '\0') {
                p++;
            } else if(ch == '"') {
                in_string = 0;
            }
            continue;
        }
        if(ch == '"') {
            in_string = 1;
        } else if(ch == '(' || ch == '[') {
            depth++;
        } else if((ch == ')' || ch == ']') && depth > 0) {
            depth--;
        }
    }
    return in_string || depth > 0;
}

static void
krait_live_append_statement_line(char *stmt, size_t stmt_size, const char *line)
{
    size_t len;
    size_t n;
    const char *start;

    if(stmt == NULL || stmt_size == 0 || line == NULL)
        return;
    len = strlen(stmt);
    start = line;
    while(*start == ' ' || *start == '\t')
        start++;
    n = strlen(start);
    while(n > 0 && (start[n - 1] == '\n' || start[n - 1] == '\r' ||
                    start[n - 1] == ' ' || start[n - 1] == '\t'))
        n--;
    if(n == 0)
        return;
    if(len > 0 && len + 1 < stmt_size)
        stmt[len++] = ' ';
    if(len + n >= stmt_size)
        n = stmt_size - len - 1;
    if(n > 0)
        memcpy(stmt + len, start, n);
    stmt[len + n] = '\0';
}

static int
krait_live_exec_body(KraitLive *live)
{
    const char *p;
    char stmt[4096];
    int stmt_line = 0;

    stmt[0] = '\0';
    for(p = live->body; live->ok && p < live->body_end;) {
        const char *nl = memchr(p, '\n', (size_t)(live->body_end - p));
        char line[1024];
        size_t n = nl != NULL ? (size_t)(nl - p) : (size_t)(live->body_end - p);
        if(n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        if(stmt[0] == '\0')
            stmt_line = live->line_no;
        krait_live_append_statement_line(stmt, sizeof(stmt), line);
        if(!krait_live_statement_open(stmt)) {
            int saved_line = live->line_no;

            live->line_no = stmt_line;
            if(!krait_live_exec_call(live, stmt))
                return 0;
            live->line_no = saved_line;
            stmt[0] = '\0';
        }
        if(nl == NULL)
            break;
        p = nl + 1;
        live->line_no++;
    }
    if(stmt[0] != '\0') {
        live->line_no = stmt_line;
        if(!krait_live_exec_call(live, stmt))
            return 0;
    }
    return live->ok;
}

static int
krait_scene_add(SceneNode *out, int count, int cap, int parent, int depth,
                int kind, const char *label, const char *asset_path,
                Rectangle bounds, int line, int source_start, int source_end)
{
    SceneNode *node;
    char id_src[KRAIT_PATH_MAX + 256];

    if(out == NULL || count >= cap)
        return count;
    node = &out[count];
    memset(node, 0, sizeof(*node));
    node->parent = parent;
    node->depth = depth;
    node->kind = kind;
    node->editable = kind != KRAIT_SCENE_NODE_UNKNOWN &&
                     kind != KRAIT_SCENE_NODE_SCREEN;
    snprintf(node->label, sizeof(node->label), "%s",
             label != NULL && label[0] != '\0' ? label : "Node");
    if(asset_path != NULL)
        snprintf(node->asset_path, sizeof(node->asset_path), "%s", asset_path);
    node->bounds = bounds;
    node->source_line = line;
    node->source_start = source_start;
    node->source_end = source_end;
    snprintf(id_src, sizeof(id_src), "%d:%d:%s:%s", line, source_start,
             node->label, node->asset_path);
    node->id = krait_tree_id(id_src);
    return count + 1;
}

static int
krait_scene_rect_arg(const char *expr, Rectangle *out)
{
    char buf[512];
    char *scan;
    int x, y, w, h;

    if(expr == NULL || out == NULL)
        return 0;
    snprintf(buf, sizeof(buf), "%s", expr);
    scan = buf;
    if(!krait_live_next_scale_arg(&scan, &x) ||
       !krait_live_next_scale_arg(&scan, &y) ||
       !krait_live_next_scale_arg(&scan, &w) ||
       !krait_live_next_scale_arg(&scan, &h))
        return 0;
    *out = (Rectangle){x, y, w, h};
    return 1;
}

static int
krait_scene_scan_line(const char *line, SceneNode *out, int count, int cap,
                      int *stack, int *stack_depth, int source_line,
                      int source_start, int source_end)
{
    char buf[4096];
    char args[8][256];
    char label[128];
    char asset[512];
    char node_type[64];
    char *q;
    char *s;
    int argc;
    int x, y, w, h;
    int parent = *stack_depth > 0 ? stack[*stack_depth - 1] : -1;
    int depth = *stack_depth;
    Rectangle bounds = {0};

    if(line == NULL)
        return count;
    snprintf(buf, sizeof(buf), "%s", line);
    q = krait_live_trim(buf);
    if(q[0] == '\0' || q[0] == '#')
        return count;
    if(krait_live_starts_word(q, "if"))
        q = krait_live_trim(q + 2);
    argc = krait_live_call_args(q, "BeginNodeGroup", args, 8);
    if(argc >= 2) {
        if(!krait_scene_rect_arg(args[1], &bounds))
            bounds = (Rectangle){0, 0, 0, 0};
        count = krait_scene_add(out, count, cap, parent, depth,
                                KRAIT_SCENE_NODE_GROUP, "Group", NULL, bounds,
                                source_line, source_start, source_end);
        if(count > 0 && *stack_depth < 64)
            stack[(*stack_depth)++] = count - 1;
        return count;
    }
    if(strncmp(q, "EndNodeGroup", 14) == 0) {
        if(*stack_depth > 1)
            (*stack_depth)--;
        return count;
    }
    argc = krait_live_call_args(q, "Text", args, 8);
    if(argc == 5) {
        int text_w;

        s = args[0];
        label[0] = '\0';
        (void)krait_live_parse_string(&s, label, sizeof(label));
        if(!krait_live_eval_int(args[1], &x))
            x = 0;
        if(!krait_live_eval_int(args[2], &y))
            y = 0;
        text_w = (int)strlen(label) * ScaleUIPx(8);
        if(text_w < ScaleUIPx(24))
            text_w = ScaleUIPx(24);
        bounds = (Rectangle){x, y, text_w, ScaleUIPx(18)};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_TEXT, label, NULL, bounds,
                               source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Rect", args, 8);
    if(argc == 6) {
        if(!krait_live_eval_int(args[0], &x))
            x = 0;
        if(!krait_live_eval_int(args[1], &y))
            y = 0;
        if(!krait_live_eval_int(args[2], &w))
            w = 0;
        if(!krait_live_eval_int(args[3], &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Rectangle", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Button", args, 8);
    if(argc == 1) {
        char *scan = args[0];
        s = strchr(args[0], '"');
        label[0] = '\0';
        if(s != NULL)
            (void)krait_live_parse_string(&s, label, sizeof(label));
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_BUTTON, label, NULL, bounds,
                               source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Picture", args, 8);
    if(argc == 1) {
        char *scan = args[0];
        s = strchr(args[0], '"');
        asset[0] = '\0';
        if(s != NULL)
            (void)krait_live_parse_string(&s, asset, sizeof(asset));
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_SPRITE,
                               asset[0] != '\0' ? asset : "Picture", asset,
                               bounds, source_line, source_start, source_end);
    }
    if(krait_game_node_args(q, args, node_type, sizeof(node_type),
                            label, sizeof(label), &x, &y, &w, &h)) {
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_GAME_OBJECT,
                               label[0] != '\0' ? label : node_type,
                               node_type, bounds, source_line, source_start,
                               source_end);
    }
    return count;
}

int
krait_scene_scan(const char *root, const char *rel_path)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    const char *body;
    const char *body_end;
    const char *p;
    int count;
    int line;
    int stack[64];
    int stack_depth = 0;
    char screen_label[128];
    SceneNode *out = g_scene_nodes;
    int cap = KRAIT_SCENE_NODE_MAX;

    g_scene_node_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)cap);
    if(root == NULL || rel_path == NULL)
        return 0;
    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    if(!krait_live_find_named_body(text, "screen", NULL, &body, &body_end)) {
        free(text);
        return 0;
    }
    snprintf(screen_label, sizeof(screen_label), "%s", rel_path);
    count = krait_scene_add(out, 0, cap, -1, 0, KRAIT_SCENE_NODE_SCREEN,
                            screen_label, NULL, (Rectangle){0, 0, 0, 0},
                            krait_live_line_for_ptr(text, body),
                            (int)(body - text), (int)(body_end - text));
    stack[stack_depth++] = 0;
    line = krait_live_line_for_ptr(text, body);
    for(p = body; p < body_end && count < cap;) {
        const char *nl = memchr(p, '\n', (size_t)(body_end - p));
        char stmt[4096];
        size_t n = nl != NULL ? (size_t)(nl - p) : (size_t)(body_end - p);

        if(n >= sizeof(stmt))
            n = sizeof(stmt) - 1;
        memcpy(stmt, p, n);
        stmt[n] = '\0';
        count = krait_scene_scan_line(stmt, out, count, cap, stack,
                                      &stack_depth, line, (int)(p - text),
                                      (int)(p + n - text));
        if(nl == NULL)
            break;
        p = nl + 1;
        line++;
    }
    free(text);
    g_scene_node_count = count;
    return count;
}

static SceneNode *
krait_scene_node(int index)
{
    if(index < 0 || index >= g_scene_node_count)
        return NULL;
    return &g_scene_nodes[index];
}

int
krait_scene_kind(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->kind : KRAIT_SCENE_NODE_UNKNOWN;
}

int
krait_scene_depth(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->depth : 0;
}

const char *
krait_scene_label(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->label : "";
}

const char *
krait_scene_asset_path(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->asset_path : "";
}

Rectangle
krait_scene_bounds(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->bounds : (Rectangle){0};
}

int
krait_scene_source_line(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->source_line : 0;
}

int
krait_scene_editable(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->editable : 0;
}

static int
krait_scene_replace_scale_delta(char *text, size_t text_size, int scale_index,
                                int delta)
{
    const char *needle = "ScaleUIPx";
    char *p = text;
    int seen = 0;

    while((p = strstr(p, needle)) != NULL) {
        char *open = p + strlen(needle);
        char *num;
        char *end;
        int value;
        char repl[32];
        size_t old_len;
        size_t new_len;
        size_t tail_len;

        while(*open != '\0' && isspace((unsigned char)*open))
            open++;
        if(*open != '(') {
            p += strlen(needle);
            continue;
        }
        if(seen++ != scale_index) {
            p = open + 1;
            continue;
        }
        num = open + 1;
        while(*num != '\0' && isspace((unsigned char)*num))
            num++;
        end = num;
        if(*end == '-' || *end == '+')
            end++;
        if(!isdigit((unsigned char)*end))
            return 0;
        while(isdigit((unsigned char)*end))
            end++;
        value = atoi(num) + delta;
        if(value < 0)
            value = 0;
        snprintf(repl, sizeof(repl), "%d", value);
        old_len = (size_t)(end - num);
        new_len = strlen(repl);
        tail_len = strlen(end);
        if(strlen(text) - old_len + new_len + 1 > text_size)
            return 0;
        memmove(num + new_len, end, tail_len + 1);
        memcpy(num, repl, new_len);
        return 1;
    }
    return 0;
}

int
krait_scene_nudge(const char *root, const char *rel_path, int index,
                  int dx, int dy, int dw, int dh,
                  char *status, int status_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    char *slice = NULL;
    long len = 0;
    long span;
    int deltas[4];
    int scale_count = 4;
    SceneNode node;
    FILE *file;

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Node edit failed");
    if(root == NULL || rel_path == NULL)
        return 0;
    if(index < 0 || index >= g_scene_node_count)
        return 0;
    node = g_scene_nodes[index];
    if(!node.editable) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Node is read-only");
        return 0;
    }
    if(node.kind == KRAIT_SCENE_NODE_TEXT)
        scale_count = 2;
    else if(node.kind != KRAIT_SCENE_NODE_GROUP &&
            node.kind != KRAIT_SCENE_NODE_RECTANGLE &&
            node.kind != KRAIT_SCENE_NODE_BUTTON &&
            node.kind != KRAIT_SCENE_NODE_SPRITE &&
            node.kind != KRAIT_SCENE_NODE_GAME_OBJECT) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Node kind is not editable yet");
        return 0;
    }
    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    if(node.source_start < 0 || node.source_end < node.source_start ||
       node.source_end > len) {
        free(text);
        return 0;
    }
    span = node.source_end - node.source_start;
    slice = malloc((size_t)span + 128);
    if(slice == NULL) {
        free(text);
        return 0;
    }
    memcpy(slice, text + node.source_start, (size_t)span);
    slice[span] = '\0';
    deltas[0] = dx;
    deltas[1] = dy;
    deltas[2] = dw;
    deltas[3] = dh;
    for(int i = 0; i < scale_count; i++) {
        if(deltas[i] == 0)
            continue;
        if(!krait_scene_replace_scale_delta(slice, (size_t)span + 128,
                                           i, deltas[i])) {
            free(slice);
            free(text);
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size,
                         "Node uses unsupported bounds expression");
            return 0;
        }
    }
    file = fopen(path, "wb");
    if(file == NULL) {
        free(slice);
        free(text);
        return 0;
    }
    if(node.source_start > 0)
        fwrite(text, 1, (size_t)node.source_start, file);
    fwrite(slice, 1, strlen(slice), file);
    fwrite(text + node.source_end, 1, (size_t)(len - node.source_end), file);
    fclose(file);
    free(slice);
    free(text);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Node updated");
    return 1;
}

int
krait_live_draw_source(const char *root, const char *rel_path, int w, int h,
                       char *status, int status_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    KraitLive live = {0};
    char frame[128];

    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot read %s", rel_path);
        return 0;
    }
    live.root = root;
    live.rel_path = rel_path;
    live.text = text;
    live.ok = 1;
    live.line_no = 1;
    krait_live_vars_clear();
    krait_live_var_set("view_width", w);
    krait_live_var_set("view_height", h);
    krait_live_var_set("content_w", w - ScaleUIPx(32));
    krait_live_var_set("content_h", h - ScaleUIPx(32));
    krait_live_var_set("x", ScaleUIPx(16));
    krait_live_var_set("y", ScaleUIPx(16));
    krait_live_var_set("btn_h", ScaleUIPx(44));
    krait_live_var_set("gap", ScaleUIPx(10));
    if(!krait_live_find_named_body(text, "screen", NULL, &live.body, &live.body_end)) {
        frame[0] = '\0';
        if(!krait_live_find_frame_name(text, frame, sizeof(frame)) ||
           !krait_live_find_named_body(text, frame, NULL, &live.body, &live.body_end)) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size,
                         "%s delegates rendering to other app code", rel_path);
            free(text);
            return 1;
        }
    }
    live.line_no = 1;
    for(const char *p = text; p < live.body; p++)
        if(*p == '\n')
            live.line_no++;
    krait_live_exec_body(&live);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "%s",
                 live.status[0] != '\0' ? live.status :
                 (live.render_count > 0 ? "Live canvas ready" :
                  "Screen delegates rendering to app code"));
    free(text);
    return live.ok;
}

int
krait_live_draw_canvas(const char *root, const char *rel_path, int w, int h,
                       char *status, int status_size)
{
    char live_status[512];
    int live_ok;

    if(status != NULL && status_size > 0)
        status[0] = '\0';
    if(root == NULL || rel_path == NULL || rel_path[0] == '\0') {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "No .kry source selected");
        return 0;
    }
    live_ok = krait_live_draw_source(root, rel_path, w, h,
                                     live_status, sizeof(live_status));
    if(live_ok && strstr(live_status, "delegates") == NULL) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "%s", live_status);
        return 1;
    }
    {
        char configured_live[KRAIT_PATH_MAX];

        configured_live[0] = '\0';
        if(krait_project_has_make_target(root, "kryon-host") ||
           (krait_project_preview_config(root, configured_live,
                                         sizeof(configured_live), NULL, 0) &&
            configured_live[0] != '\0')) {
        IdeState st;
        RenderTexture2D target;
        char build_status[512];
        int ready = 0;
        int ok;

        memset(&st, 0, sizeof(st));
        st.project.loaded = 1;
        snprintf(st.project.path, sizeof(st.project.path), "%s", root);
        snprintf(st.project.name, sizeof(st.project.name), "%s",
                 krait_basename(root));
        if(w <= 0)
            w = 640;
        if(h <= 0)
            h = 480;
        for(int i = 0; i < 600; i++) {
            build_status[0] = '\0';
            if(krait_preview_build(&st, build_status, sizeof(build_status))) {
                ready = 1;
                break;
            }
            if(strcmp(build_status, "Preview building...") != 0)
                break;
            usleep(50000);
        }
        if(!ready) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size, "%s",
                         build_status[0] != '\0' ? build_status :
                         "Preview build timed out");
            return 0;
        }

        target = LoadRenderTexture(w, h);
        BeginTextureMode(target);
        ClearBackground(GetThemeBackground());
        BeginUIFrame(w, h, 1.0);
        ok = krait_preview_draw(&st, rel_path,
                                (Rectangle){0, 0, (float)w, (float)h},
                                status, status_size);
        if(!ok && status != NULL &&
           strncmp(status, "Preview route not found", 23) == 0) {
            ClearBackground(GetThemeBackground());
            ok = krait_live_draw_source(root, rel_path, w, h,
                                        status, status_size);
        }
        EndUIFocus();
        EndTextureMode();
        UnloadRenderTexture(target);
        return ok;
        }
    }
    return krait_live_draw_source(root, rel_path, w, h, status, status_size);
}

static void
krait_widget_snippet(char *dst, size_t dst_size, int widget_type, int x, int y)
{
    if(dst == NULL || dst_size == 0)
        return;
    if(!KryonNodeTypeSnippet(widget_type, x, y, dst, (int)dst_size))
        snprintf(dst, dst_size, "\n    /* Node is not insertable yet. */\n");
}

int
krait_insert_widget(const char *root, const char *rel_path, int insert_offset,
                    int widget_type, int x, int y, char *status,
                    int status_size)
{
    char path[KRAIT_PATH_MAX];
    char *text;
    long len;
    char snippet[2048];
    FILE *file;

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Node insert failed");
    if(root == NULL || rel_path == NULL || insert_offset < 0)
        return 0;
    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    if(insert_offset > len) {
        free(text);
        return 0;
    }
    krait_widget_snippet(snippet, sizeof(snippet), widget_type, x, y);
    file = fopen(path, "wb");
    if(file == NULL) {
        free(text);
        return 0;
    }
    if(insert_offset > 0)
        fwrite(text, 1, (size_t)insert_offset, file);
    fwrite(snippet, 1, strlen(snippet), file);
    fwrite(text + insert_offset, 1, (size_t)(len - insert_offset), file);
    fclose(file);
    free(text);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Node added");
    return 1;
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
    int codepoints[0x7e - 0x20 + 1];
    int codepoint_count = 0;
    const char *source;

    for(int codepoint = 0x20; codepoint <= 0x7e; codepoint++)
        codepoints[codepoint_count++] = codepoint;

    if(kryon_dir != NULL && kryon_dir[0] != '\0') {
        krait_join(path, sizeof(path), kryon_dir, "fonts/noto/NotoSans-Regular.ttf");
        source = path;
    } else {
        source = "vendor/kryon/fonts/noto/NotoSans-Regular.ttf";
    }

    if(!RegisterUIFontFileSource("default", source, codepoints,
                                 codepoint_count, 1))
        return 0;
    UseUIFont("default");
    return 1;
}
