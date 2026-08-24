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
krait_settings_sanitize(IdeState *st)
{
    if(st == NULL)
        return;
    if(st->theme_source != THEME_SOURCE_SYSTEM)
        st->theme_source = THEME_SOURCE_APP;
    if(st->theme_mode < THEME_MODE_SYSTEM || st->theme_mode > THEME_MODE_DARK)
        st->theme_mode = GetDefaultPlatformThemeMode();
    if(st->theme_id < 0 || st->theme_id >= THEME_COUNT)
        st->theme_id = GetDefaultThemeForThemeStyle(
            st->theme_style == THEME_STYLE_SYSTEM ? GetDefaultPlatformThemeStyle()
                                                  : (ThemeStyle)st->theme_style);
    if(st->theme_style < THEME_STYLE_SYSTEM || st->theme_style > THEME_STYLE_MATERIAL)
        st->theme_style = THEME_STYLE_SYSTEM;
    if(st->settings_tab < 0 || st->settings_tab > 4)
        st->settings_tab = 0;
    if(st->settings_scroll < 0)
        st->settings_scroll = 0;
    st->force_mobile_layout = st->force_mobile_layout ? 1 : 0;
    if(st->module_count < 0)
        st->module_count = 0;
    if(st->module_count > IDE_MAX_MODULES)
        st->module_count = IDE_MAX_MODULES;
    for(int i = 0; i < st->module_count; i++)
        st->modules[i].enabled = st->modules[i].enabled ? 1 : 0;
}

static void
krait_settings_migrate_legacy_defaults(IdeState *st)
{
    if(st == NULL)
        return;
    if(st->theme_source == THEME_SOURCE_APP &&
       st->theme_mode == THEME_MODE_DARK &&
       st->theme_id == THEME_MONO &&
       st->theme_style == THEME_STYLE_RETRO) {
        st->theme_source = GetDefaultPlatformThemeSource();
        st->theme_mode = GetDefaultPlatformThemeMode();
        st->theme_style = THEME_STYLE_SYSTEM;
        st->theme_id = GetDefaultThemeForThemeStyle(GetDefaultPlatformThemeStyle());
    }
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

void
krait_settings_defaults(IdeState *st)
{
    if(st == NULL)
        return;
    st->theme_source = GetDefaultPlatformThemeSource();
    st->theme_mode = GetDefaultPlatformThemeMode();
    st->theme_style = THEME_STYLE_SYSTEM;
    st->theme_id = GetDefaultThemeForThemeStyle(GetDefaultPlatformThemeStyle());
    st->settings_tab = 0;
    st->settings_scroll = 0;
    st->force_mobile_layout = 0;
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
        else if(strcmp(key, "force_mobile") == 0)
            st->force_mobile_layout = value ? 1 : 0;
        else
            (void)krait_settings_set_module(st, key, value);
    }
    fclose(file);
    krait_settings_migrate_legacy_defaults(st);
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
    fprintf(file, "force_mobile %d\n", st->force_mobile_layout ? 1 : 0);
    for(int i = 0; i < st->module_count; i++)
        fprintf(file, "module_%s_enabled %d\n",
                st->modules[i].id, st->modules[i].enabled ? 1 : 0);
    fclose(file);
    return 1;
}
