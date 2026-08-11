/*
 * Asset browser backend. Scans the project directory for image and audio
 * assets and exposes them as a flat list for the IDE's Assets pane. The pane
 * (ide/assets.kry) renders thumbnails and, on click, bridges the selected
 * asset into the inspector's asset_path property.
 */

#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define KRAIT_ASSET_MAX 512
#define KRAIT_ASSET_PATH_MAX 512

static char g_asset_paths[KRAIT_ASSET_MAX][KRAIT_ASSET_PATH_MAX];
static int g_asset_count;

static int
krait_asset_name_is_image(const char *name)
{
    const char *dot = strrchr(name, '.');
    if(dot == NULL)
        return 0;
    return strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
           strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".bmp") == 0 ||
           strcasecmp(dot, ".webp") == 0 || strcasecmp(dot, ".gif") == 0 ||
           strcasecmp(dot, ".qoi") == 0;
}

static int
krait_asset_is_audio(const char *name)
{
    const char *dot = strrchr(name, '.');
    if(dot == NULL)
        return 0;
    return strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".ogg") == 0 ||
           strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".flac") == 0 ||
           strcasecmp(dot, ".xm") == 0 || strcasecmp(dot, ".mod") == 0;
}

static int
krait_asset_is_supported(const char *name)
{
    return krait_asset_name_is_image(name) || krait_asset_is_audio(name);
}

static int
krait_asset_ignored_dir(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
           strcmp(name, "build") == 0 || strcmp(name, "vendor") == 0 ||
           strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0;
}

static void
krait_asset_scan_dir(const char *root, const char *rel, int depth)
{
    char dirpath[KRAIT_PATH_MAX];
    DIR *dir;
    struct dirent *ent;

    if(depth > KRAIT_SEARCH_DEPTH || g_asset_count >= KRAIT_ASSET_MAX)
        return;
    if(rel[0] != '\0')
        snprintf(dirpath, sizeof(dirpath), "%s/%s", root, rel);
    else
        snprintf(dirpath, sizeof(dirpath), "%s", root);
    dir = opendir(dirpath);
    if(dir == NULL)
        return;
    while((ent = readdir(dir)) != NULL && g_asset_count < KRAIT_ASSET_MAX) {
        char child_rel[KRAIT_ASSET_PATH_MAX];
        char child_full[KRAIT_PATH_MAX];
        struct stat st;

        if(krait_asset_ignored_dir(ent->d_name))
            continue;
        if(rel[0] != '\0')
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        snprintf(child_full, sizeof(child_full), "%s/%s", root, child_rel);
        if(stat(child_full, &st) != 0)
            continue;
        if(S_ISDIR(st.st_mode)) {
            krait_asset_scan_dir(root, child_rel, depth + 1);
        } else if(S_ISREG(st.st_mode) && krait_asset_is_supported(ent->d_name)) {
            snprintf(g_asset_paths[g_asset_count],
                     sizeof(g_asset_paths[g_asset_count]), "%s", child_rel);
            g_asset_count++;
        }
    }
    closedir(dir);
}

int
krait_asset_scan(const char *root)
{
    g_asset_count = 0;
    if(root == NULL)
        return 0;
    krait_asset_scan_dir(root, "", 0);
    return g_asset_count;
}

int
krait_asset_count(void)
{
    return g_asset_count;
}

const char *
krait_asset_path(int index)
{
    if(index < 0 || index >= g_asset_count)
        return "";
    return g_asset_paths[index];
}

int
krait_asset_is_image(int index)
{
    if(index < 0 || index >= g_asset_count)
        return 0;
    return krait_asset_name_is_image(g_asset_paths[index]);
}
