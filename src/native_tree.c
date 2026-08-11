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

