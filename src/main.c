#include "ide/app.h"
#include "ide/editor.h"
#include "ide/preview.h"
#include "ide/project.h"
#include "ui_icons.h"
#include "ui_inspect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern IdeState istate;

static int
path_has_suffix(const char *path, const char *suffix)
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

static void
open_first_kry_file(IdeState *st)
{
    if(st == NULL || !st->project.loaded || st->open_count > 0)
        return;
    for(int i = 0; i < st->entry_count; i++) {
        if(!st->entries[i].is_dir &&
           strcmp(st->entries[i].path, "main.kry") == 0) {
            ide_editor_editor_open(st, st->entries[i].path);
            return;
        }
    }
    for(int i = 0; i < st->entry_count; i++) {
        if(!st->entries[i].is_dir &&
           path_has_suffix(st->entries[i].path, ".kry")) {
            ide_editor_editor_open(st, st->entries[i].path);
            return;
        }
    }
}

static void
open_startup_project(const char *path)
{
    if(path == NULL || path[0] == '\0')
        return;
    ide_project_project_open(&istate, path);
    open_first_kry_file(&istate);
}

static int
save_screen_image(const char *path)
{
    Image image;
    int ok;

    if(path == NULL || path[0] == '\0')
        return 1;
    image = LoadImageFromScreen();
    if(image.data == NULL)
        return 0;
    ok = ExportImage(image, path);
    UnloadImage(image);
    if(!ok)
        fprintf(stderr, "krait: could not save screenshot: %s\n", path);
    return ok;
}

static int
run_smoke(const char *project_path, const char *screenshot_path, int build_preview)
{
    double start;

    if(project_path != NULL)
        open_startup_project(project_path);
    if(!istate.project.loaded)
        return 1;
    if(build_preview) {
        ide_preview_preview_build_start(&istate);
        start = GetTime();
        while(istate.build_running && GetTime() - start < 30.0) {
            ide_preview_preview_build_poll(&istate);
        }
        if(istate.build_running)
            return 1;
        if(istate.host == NULL)
            return 1;
    }
    for(int i = 0; i < 4; i++)
        ide_app_frame();
    if(!save_screen_image(screenshot_path))
        return 1;
    return 0;
}

int
main(int argc, char **argv)
{
    const int screen_w = 1400;
    const int screen_h = 900;
    const UIIconAsset *window_icon_asset;
    Image window_icon = {0};
    int argi = 1;
    int smoke_screens = 0;
    int smoke_ide = 0;
    const char *project_arg = NULL;
    const char *smoke_screenshot_path = "/tmp/krait-ide-smoke.png";
    int result = 0;

    if(argi < argc && strcmp(argv[argi], "--temp-session") == 0)
        argi++;
    if(argi < argc && strcmp(argv[argi], "--smoke-screens") == 0) {
        smoke_screens = 1;
        if(argc > argi + 1)
            project_arg = argv[argi + 1];
        if(argc > argi + 2)
            smoke_screenshot_path = argv[argi + 2];
    } else if(argi < argc && strcmp(argv[argi], "--smoke-ide") == 0) {
        smoke_ide = 1;
        if(argc > argi + 1)
            project_arg = argv[argi + 1];
        if(argc > argi + 2)
            smoke_screenshot_path = argv[argi + 2];
    } else if(argi < argc) {
        project_arg = argv[argi];
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(screen_w, screen_h, "Krait");
    window_icon_asset = GetUIIconAsset(UI_ICON_TYPE_KRYON);
    if(window_icon_asset != NULL) {
        window_icon = LoadImageFromMemory(".png", window_icon_asset->data,
                                          (int)window_icon_asset->size);
        if(window_icon.data != NULL) {
            SetWindowIcon(window_icon);
            UnloadImage(window_icon);
        }
    }
    SetTargetFPS(60);
    InitUI(screen_w, screen_h, GetUIScale());
    SetCurrentTheme(THEME_MONO, 0);
    SetUIInspectEnabled(1);
    setenv("KRYON_INSPECT", "1", 1);
    ide_app_init();
    open_startup_project(project_arg);

    if(smoke_screens || smoke_ide) {
        result = run_smoke(project_arg, smoke_screenshot_path, smoke_ide);
    } else {
        while(!WindowShouldClose())
            ide_app_frame();
    }

    ide_app_shutdown();
    ClearUIFonts();
    CloseWindow();
    return result;
}
