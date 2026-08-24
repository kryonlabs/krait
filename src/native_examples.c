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

typedef struct {
    char path[KRAIT_PATH_MAX];
    char title[256];
} KraitPathItem;

static KraitPathItem g_examples[KRAIT_MAX_EXAMPLES];
static int g_example_count;
static int g_examples_loaded;

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
    return EnsureUIDefaultFont();
}
