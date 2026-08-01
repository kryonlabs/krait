#include "kryon.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define KRAIT_PATH_MAX 1024
#define KRAIT_MAX_RECENT 12
#define KRAIT_MAX_EXAMPLES 32

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

static int
krait_path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;

    if(path == NULL || suffix == NULL)
        return 0;
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
           strcmp(path + path_len - suffix_len, suffix) == 0;
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
        kryon_dir = "../kryon";
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
        (void)RegisterUIFontFileSource("noto", "../kryon/fonts/noto/NotoSans-Regular.ttf",
                                       NULL, 0, 1);
    }
    return ok;
}
