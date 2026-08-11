#include "ide/app.h"
#include "ide/editor.h"
#include "ide/modules.h"
#include "ide/preview.h"
#include "ide/project.h"
#include "ui_icons.h"
#include "ui_inspect.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern IdeState istate;
extern int krait_live_draw_canvas(const char *root, const char *rel_path,
                                  int w, int h,
                                  char *status, int status_size);
extern void krait_preview_unload(void);

#define KRAIT_KEY_COUNT 512

static unsigned char krait_key_down_now[KRAIT_KEY_COUNT];
static unsigned char krait_key_pressed_now[KRAIT_KEY_COUNT];
static unsigned char krait_key_pressed_pending[KRAIT_KEY_COUNT];

static void
krait_use_kryon_dir(const char *path)
{
    if(path == NULL || path[0] == '\0')
        return;
    setenv("KRYON_DIR", path, 1);
}

static int
krait_sdl_keycode_to_ui_key(SDL_Keycode keycode)
{
    if(keycode >= 'a' && keycode <= 'z')
        return (int)(keycode - ('a' - 'A'));
    if(keycode >= 'A' && keycode <= 'Z')
        return (int)keycode;
    if(keycode >= 32 && keycode <= 126)
        return (int)keycode;
    return 0;
}

static int
krait_sdl_key_event(void *userdata, SDL_Event *event)
{
    int key;

    (void)userdata;
    if(event == NULL ||
       (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP))
        return 0;
    key = krait_sdl_keycode_to_ui_key(event->key.keysym.sym);
    if(key <= 0 || key >= KRAIT_KEY_COUNT)
        return 0;
    if(event->type == SDL_KEYDOWN) {
        if(!event->key.repeat && !krait_key_down_now[key])
            krait_key_pressed_pending[key] = 1;
        krait_key_down_now[key] = 1;
    } else {
        krait_key_down_now[key] = 0;
    }
    return 0;
}

static void
krait_update_logical_keys(void)
{
    memcpy(krait_key_pressed_now, krait_key_pressed_pending,
           sizeof(krait_key_pressed_now));
    memset(krait_key_pressed_pending, 0, sizeof(krait_key_pressed_pending));
}

static void
krait_init_logical_keys(void)
{
    SDL_AddEventWatch(krait_sdl_key_event, NULL);
}

static void
krait_shutdown_logical_keys(void)
{
    SDL_DelEventWatch(krait_sdl_key_event, NULL);
}

static int
krait_logical_key_pressed(int key)
{
    if(key <= 0 || key >= KRAIT_KEY_COUNT)
        return 0;
    return krait_key_pressed_now[key];
}

static int
krait_logical_key_down(int key)
{
    if(key <= 0 || key >= KRAIT_KEY_COUNT)
        return 0;
    return krait_key_down_now[key];
}

static void
open_startup_project(const char *path)
{
    if(path == NULL || path[0] == '\0')
        return;
    ide_project_project_open(&istate, path);
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
    if(project_path != NULL)
        open_startup_project(project_path);
    (void)build_preview;
    for(int i = 0; i < 4; i++) {
        UpdateKeyPlatformState();
        ide_app_frame();
    }
    if(!save_screen_image(screenshot_path))
        return 1;
    return 0;
}

static int
run_live_smoke(const char *project_path, const char *rel_path)
{
    char status[512];

    if(project_path == NULL || rel_path == NULL)
        return 1;
    if(!krait_live_draw_canvas(project_path, rel_path, 640, 480,
                               status, sizeof(status))) {
        fprintf(stderr, "krait live smoke failed: %s\n", status);
        return 1;
    }
    if(status[0] != '\0')
        fprintf(stderr, "krait live smoke: %s\n", status);
    return 0;
}

static int
run_live_reload_smoke(const char *project_path, const char *rel_path)
{
    for(int i = 0; i < 2; i++) {
        if(run_live_smoke(project_path, rel_path) != 0)
            return 1;
        krait_preview_unload();
    }
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
    int smoke_live = 0;
    int smoke_live_reload = 0;
    const char *project_arg = NULL;
    const char *live_rel_path = NULL;
    const char *smoke_screenshot_path = "/tmp/krait-ide-smoke.png";
    int result = 0;

    while(argi < argc) {
        if(strcmp(argv[argi], "--temp-session") == 0) {
            argi++;
            continue;
        }
        if(strcmp(argv[argi], "--dev") == 0) {
            krait_use_kryon_dir("../kryon");
            argi++;
            continue;
        }
        if(strcmp(argv[argi], "--kryon-dir") == 0) {
            if(argc > argi + 1) {
                krait_use_kryon_dir(argv[argi + 1]);
                argi += 2;
                continue;
            }
            break;
        }
        if(strcmp(argv[argi], "--smoke-screens") == 0) {
            smoke_screens = 1;
            if(argc > argi + 1)
                project_arg = argv[argi + 1];
            if(argc > argi + 2)
                smoke_screenshot_path = argv[argi + 2];
            break;
        }
        if(strcmp(argv[argi], "--smoke-ide") == 0) {
            smoke_ide = 1;
            if(argc > argi + 1) {
                if(strstr(argv[argi + 1], ".png") != NULL)
                    smoke_screenshot_path = argv[argi + 1];
                else
                    project_arg = argv[argi + 1];
            }
            if(argc > argi + 2)
                smoke_screenshot_path = argv[argi + 2];
            break;
        }
        if(strcmp(argv[argi], "--smoke-live") == 0) {
            smoke_live = 1;
            if(argc > argi + 1)
                project_arg = argv[argi + 1];
            if(argc > argi + 2)
                live_rel_path = argv[argi + 2];
            break;
        }
        if(strcmp(argv[argi], "--smoke-live-reload") == 0) {
            smoke_live_reload = 1;
            if(argc > argi + 1)
                project_arg = argv[argi + 1];
            if(argc > argi + 2)
                live_rel_path = argv[argi + 2];
            break;
        }
        project_arg = argv[argi];
        break;
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
    krait_init_logical_keys();
    SetKeyPlatformCallbacks(krait_update_logical_keys,
                              krait_logical_key_pressed,
                              krait_logical_key_down);
    SetCurrentTheme(THEME_MONO, 0);
    ide_app_init();
    open_startup_project(project_arg);

    if(smoke_live_reload) {
        result = run_live_reload_smoke(project_arg, live_rel_path);
    } else if(smoke_live) {
        result = run_live_smoke(project_arg, live_rel_path);
    } else if(smoke_screens || smoke_ide) {
        result = run_smoke(project_arg, smoke_screenshot_path, smoke_ide);
    } else {
        while(!WindowShouldClose()) {
            UpdateKeyPlatformState();
            ide_app_frame();
        }
    }

    ide_app_shutdown();
    krait_shutdown_logical_keys();
    ClearUIFonts();
    CloseWindow();
    return result;
}
