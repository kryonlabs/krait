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

static int
krait_valid_pane_view(int view)
{
    return view >= IDE_PANE_VIEW_EXPLORER && view <= IDE_PANE_VIEW_ASSETS;
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

