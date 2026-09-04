#include "ide/app.h"
#include "ide/agent.h"
#include "ide/editor.h"
#include "ide/game.h"
#include "ide/modules.h"
#include "ide/preview.h"
#include "ide/project.h"
#include "native_internal.h"
#include "native_level.h"
#include "ui_icons.h"
#include "ui_inspect.h"
#include "theme_style.h"
#include "version.h"

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#include <SDL2/SDL.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern IdeState istate;

#define KRAIT_KEY_COUNT 512

static unsigned char krait_key_down_now[KRAIT_KEY_COUNT];
static unsigned char krait_key_pressed_now[KRAIT_KEY_COUNT];
static unsigned char krait_key_pressed_pending[KRAIT_KEY_COUNT];

/* Mobile preview / DPI smoke overrides. Desktop-only knobs; on Android the UI
 * scale comes from GetWindowScaleDPI() (device density) instead. */
static int g_phone_mode = 0;
static int g_phone_w = 420;
static int g_phone_h = 880;
static float g_ui_scale_override = 0.0f;

static void
krait_use_kryon_dir(const char *path)
{
    if(path == NULL || path[0] == '\0')
        return;
    setenv("KRYON_DIR", path, 1);
}

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
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
#endif

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
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    SDL_AddEventWatch(krait_sdl_key_event, NULL);
#endif
}

static void
krait_shutdown_logical_keys(void)
{
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    SDL_DelEventWatch(krait_sdl_key_event, NULL);
#endif
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

int krait_load_ui_font(void);   /* native_examples.c */

/* Standalone player: run a game.scene in its own window without the
 * editor. ESC quits, F5 restarts from the file. The frame cap and the
 * screenshot path come from KRAIT_PLAY_FRAMES / KRAIT_PLAY_SHOT so the
 * same path serves the smoke test. */
static int
run_player(const char *scene_arg, const char *shot_path)
{
    char title[160];
    int vw = 640, vh = 360;
    int frame_cap = 0;
    int frames = 0;

    if(!krait_engine_play_scene(scene_arg)) {
        fprintf(stderr, "krait: cannot play %s\n", scene_arg);
        return 2;
    }
    if(getenv("KRAIT_PLAY_FRAMES") != NULL)
        frame_cap = atoi(getenv("KRAIT_PLAY_FRAMES"));
    krait_engine_view_size(&vw, &vh);
    snprintf(title, sizeof(title), "%s - Krait Player",
             krait_engine_scene_name());
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(vw, vh, title);
    if(InstanceRejected()) {
        fprintf(stderr, "krait: another krait instance is already running\n");
        if(IsNotificationSupported())
            SendNotification("Krait",
                             "Another krait instance is already running.");
        return 1;
    }
    SetTargetFPS(60);
    InitUI(vw, vh, 1.0f);
    krait_load_ui_font();
    SetThemeSource(GetDefaultPlatformThemeSource());
    SetThemeMode(GetDefaultPlatformThemeMode());
    SetThemeStyle(THEME_STYLE_SYSTEM);
    SetCurrentTheme(GetDefaultThemeForThemeStyle(GetEffectiveThemeStyle()),
                    GetEffectiveThemeDarkMode() ? 1 : 0);
    while(!WindowShouldClose()) {
        if(IsKeyPressed(KEY_ESCAPE))
            break;
        if(IsKeyPressed(KEY_F5)) {
            if(!krait_engine_play_scene(scene_arg))
                break;
        }
        UpdateKeyPlatformState();
        BeginFrame();
        BeginUIFrame(GetScreenWidth(), GetScreenHeight(), GetUIScale());
        BeginUI(Key("ide_game_PlayGame"));
        ide_game_PlayGame();
        EndUI();
        EndUIFrame();
        EndFrame();
        frames++;
        if(frame_cap > 0 && frames >= frame_cap)
            break;
    }
    if(shot_path != NULL && shot_path[0] != '\0') {
        Image shot = LoadImageFromScreen();

        if(shot.data != NULL) {
            ExportImage(shot, shot_path);
            UnloadImage(shot);
            fprintf(stderr, "krait: screenshot saved to %s\n", shot_path);
        } else {
            fprintf(stderr,
                    "krait: screenshot capture unavailable on this backend\n");
            return 1;
        }
    }
    krait_engine_shutdown();
    CloseWindow();
    return 0;
}

static int
krait_view_from_name(const char *name)
{    if(strcmp(name, "start") == 0)
        return IDE_VIEW_START;
    if(strcmp(name, "studio") == 0 || strcmp(name, "project") == 0)
        return IDE_VIEW_PROJECT;
    if(strcmp(name, "text") == 0)
        return IDE_VIEW_TEXT;
    if(strcmp(name, "kanban") == 0)
        return IDE_VIEW_KANBAN;
    if(strcmp(name, "agent") == 0)
        return IDE_VIEW_AGENT;
    if(strcmp(name, "game") == 0)
        return IDE_VIEW_GAME;
    if(strcmp(name, "settings") == 0)
        return IDE_VIEW_SETTINGS;
    return -1;
}

static int
krait_ui_text_seen(const UIWidgetNode *nodes, int count, const char *text)
{
    int i;

    if(nodes == NULL || text == NULL)
        return 0;
    for(i = 0; i < count; i++) {
        if(nodes[i].kind == UI_WIDGET_TEXT_NODE &&
           nodes[i].owned_text != NULL &&
           strcmp(nodes[i].owned_text, text) == 0)
            return 1;
    }
    return 0;
}

static int
krait_ui_control_seen(const UIWidgetNode *nodes, int count, UIWidgetKind kind,
                      int id)
{
    int i;

    if(nodes == NULL)
        return 0;
    for(i = 0; i < count; i++) {
        if(nodes[i].kind == kind && nodes[i].id == id)
            return 1;
    }
    return 0;
}

static int
krait_smoke_assert_agent_ui(void)
{
    int count = 0;
    const UIWidgetNode *nodes = UIGetTreeNodes(&count);
    const char *required_text[] = {
        "Krait",
        "Projects",
        "Conversations",
        "Tasks",
        "Configure accounts",
        "Provider",
        "Model",
        "Base URL",
        "API key",
        NULL
    };
    int i;

    if(count <= 0 || nodes == NULL) {
        fprintf(stderr, "krait agent smoke: UI tree is empty\n");
        return 0;
    }
    for(i = 0; required_text[i] != NULL; i++) {
        if(!krait_ui_text_seen(nodes, count, required_text[i])) {
            fprintf(stderr, "krait agent smoke: missing text '%s'\n",
                    required_text[i]);
            return 0;
        }
    }
    if(!krait_ui_control_seen(nodes, count, UI_WIDGET_TEXT_FIELD_NODE, 7601)) {
        fprintf(stderr, "krait agent smoke: missing composer text field\n");
        return 0;
    }
    if(!krait_ui_control_seen(nodes, count, UI_WIDGET_TEXT_FIELD_NODE, 7905) ||
       !krait_ui_control_seen(nodes, count, UI_WIDGET_TEXT_FIELD_NODE, 7906) ||
       !krait_ui_control_seen(nodes, count, UI_WIDGET_TEXT_FIELD_NODE, 7902)) {
        fprintf(stderr, "krait agent smoke: missing account text fields\n");
        return 0;
    }
    if(!krait_ui_control_seen(nodes, count, UI_WIDGET_DROPDOWN_NODE, 7901) ||
       !krait_ui_control_seen(nodes, count, UI_WIDGET_DROPDOWN_NODE, 7903) ||
       !krait_ui_control_seen(nodes, count, UI_WIDGET_DROPDOWN_NODE, 7904)) {
        fprintf(stderr, "krait agent smoke: missing provider/effort dropdowns\n");
        return 0;
    }
    fprintf(stderr, "krait agent smoke: semantic UI ok (%d nodes)\n", count);
    return 1;
}

static int
run_smoke(const char *project_path, const char *screenshot_path, int build_preview)
{
    int assert_agent = getenv("KRAIT_SMOKE_ASSERT_AGENT") != NULL &&
                       getenv("KRAIT_SMOKE_ASSERT_AGENT")[0] != '\0';

    (void)project_path;
    if(getenv("KRAIT_SMOKE_OPEN_FILE") != NULL)
        ide_editor_editor_open(&istate, getenv("KRAIT_SMOKE_OPEN_FILE"));
    if(getenv("KRAIT_SMOKE_VIEW") != NULL &&
       getenv("KRAIT_SMOKE_VIEW")[0] != '\0') {
        int view = krait_view_from_name(getenv("KRAIT_SMOKE_VIEW"));

        if(view >= 0)
            istate.view = view;
    }
    if(getenv("KRAIT_SMOKE_LEVEL") != NULL &&
       getenv("KRAIT_SMOKE_LEVEL")[0] != '\0')
        krait_level_set_active(1, NULL);   /* root resolved at first frame */
    (void)build_preview;
    if(assert_agent) {
        istate.view = IDE_VIEW_AGENT;
        agent_accounts_open = 1;
    }
    for(int i = 0; i < 4; i++) {
        UpdateKeyPlatformState();
        BeginFrame();
        BeginUIFrame(GetScreenWidth(), GetScreenHeight(), GetUIScale());
        BeginUI(Key("ide_app_App"));
        ide_app_App();
        EndUI();
        EndUIFrame();
        EndFrame();
    }
    if(assert_agent && !krait_smoke_assert_agent_ui())
        return 1;
    if(screenshot_path != NULL && screenshot_path[0] != '\0') {
        Image shot = LoadImageFromScreen();

        if(shot.data != NULL) {
            ExportImage(shot, screenshot_path);
            UnloadImage(shot);
            fprintf(stderr, "krait: screenshot saved to %s\n", screenshot_path);
        } else {
            fprintf(stderr,
                    "krait: screenshot capture unavailable on this backend\n");
            return 1;
        }
    }
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

/* Headless check of the artifact-generation path (the kir/krb/c tabs). Drives
 * the same krait_artifact_generate() the editor calls, so it exercises the
 * resolved tool path without needing a window or display. */
static int
run_artifact_smoke(const char *kind_str, const char *project, const char *rel)
{
    /* kind matches the native artifact enum in native_artifacts.c: 1=kir 2=krb 3=c. */
    int kind;
    static char out[131072];
    char artifact_path[1024];
    char status[512];

    if(kind_str == NULL || project == NULL || rel == NULL) {
        fprintf(stderr, "usage: --smoke-artifact <kir|krb|c> <project-dir> <rel.kry>\n");
        return 1;
    }
    if(strcmp(kind_str, "kir") == 0)
        kind = 1;
    else if(strcmp(kind_str, "krb") == 0)
        kind = 2;
    else if(strcmp(kind_str, "c") == 0)
        kind = 3;
    else {
        fprintf(stderr, "krait artifact smoke: unknown kind '%s' (kir/krb/c)\n", kind_str);
        return 1;
    }
    if(!krait_artifact_generate(project, rel, kind, out, (int)sizeof(out),
                                artifact_path, (int)sizeof(artifact_path),
                                status, (int)sizeof(status))) {
        fprintf(stderr, "krait artifact smoke FAILED (%s): %s\n", kind_str,
                status[0] != '\0' ? status : "Artifact generation failed");
        return 1;
    }
    fprintf(stderr, "krait artifact smoke OK (%s): %zu bytes <- %s\n",
            kind_str, strlen(out), artifact_path);
    if(status[0] != '\0' && strstr(status, "Generated (krb") != NULL)
        fprintf(stderr, "krait artifact smoke status: %s\n", status);
    return 0;
}

int
main(int argc, char **argv)
{
    int screen_w = 1400;
    int screen_h = 900;
    const UIIconAsset *window_icon_asset;
    Image window_icon = {0};
    int argi = 1;
    int smoke_screens = 0;
    int smoke_ide = 0;
    int smoke_live = 0;
    int smoke_live_reload = 0;
    int smoke_artifact = 0;
    int smoke_engine = 0;
    const char *play_scene = NULL;
    const char *smoke_play_scene = NULL;
    const char *artifact_kind = NULL;
    const char *artifact_project = NULL;
    const char *artifact_rel = NULL;
    const char *project_arg = NULL;
    const char *view_arg = NULL;
    const char *live_rel_path = NULL;
    const char *smoke_screenshot_path = "/tmp/krait-ide-smoke.png";
    int result = 0;

    while(argi < argc) {
        if(strcmp(argv[argi], "--temp-session") == 0) {
            argi++;
            continue;
        }
        if(strcmp(argv[argi], "--version") == 0 ||
           strcmp(argv[argi], "-v") == 0) {
            printf("krait %s\n", KRAIT_VERSION_STRING);
            return 0;
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
        if(strcmp(argv[argi], "--smoke-artifact") == 0) {
            smoke_artifact = 1;
            if(argc > argi + 1)
                artifact_kind = argv[argi + 1];
            if(argc > argi + 2)
                artifact_project = argv[argi + 2];
            if(argc > argi + 3)
                artifact_rel = argv[argi + 3];
            break;
        }
        if(strcmp(argv[argi], "--smoke-engine") == 0) {
            smoke_engine = 1;
            break;
        }
        if(strcmp(argv[argi], "--play-game") == 0) {
            if(argc <= argi + 1) {
                fprintf(stderr, "krait: --play-game requires a game.scene "
                        "path or a project directory\n");
                return 2;
            }
            play_scene = argv[argi + 1];
            argi += 2;
            continue;
        }
        if(strcmp(argv[argi], "--smoke-play") == 0) {
            if(argc <= argi + 1) {
                fprintf(stderr, "krait: --smoke-play requires a game.scene "
                        "path or a project directory\n");
                return 2;
            }
            smoke_play_scene = argv[argi + 1];
            argi += 2;
            continue;
        }
        if(strcmp(argv[argi], "--phone") == 0) {
            g_phone_mode = 1;
            argi++;
            /* Optional "--phone W H" to pick exact phone dimensions. */
            if(argc >= argi + 2 && argv[argi][0] >= '0' && argv[argi][0] <= '9') {
                int pw = atoi(argv[argi]);
                int ph = atoi(argv[argi + 1]);
                if(pw >= 200 && ph >= 400) {
                    g_phone_w = pw;
                    g_phone_h = ph;
                    argi += 2;
                }
            }
            continue;
        }
        if(strcmp(argv[argi], "--dpi") == 0) {
            if(argc > argi + 1) {
                g_ui_scale_override = (float)atof(argv[argi + 1]);
                argi += 2;
                continue;
            }
            break;
        }
        if(strcmp(argv[argi], "--view") == 0) {
            if(argc <= argi + 1) {
                fprintf(stderr, "krait: --view requires a view name "
                        "(start|studio|text|kanban|agent|game|settings)\n");
                return 2;
            }
            view_arg = argv[argi + 1];
            argi += 2;
            continue;
        }
        project_arg = argv[argi];
        break;
    }

    if(g_phone_mode) {
        screen_w = g_phone_w;
        screen_h = g_phone_h;
    }

    if(smoke_artifact)
        return run_artifact_smoke(artifact_kind, artifact_project, artifact_rel);
    if(smoke_engine)
        return krait_engine_smoke_test();
    if(play_scene != NULL)
        return run_player(play_scene, NULL);
    if(smoke_play_scene != NULL)
        return run_player(smoke_play_scene, getenv("KRAIT_PLAY_SHOT"));

    /* Screenshot capture reads the pre-swap back buffer, which kryon only
     * keeps when armed; smoke runs that capture a frame arm it up front
     * (the arm state caches at the first EndDrawing). */
    if(smoke_screenshot_path != NULL && smoke_screenshot_path[0] != '\0')
        setenv("KRYON_SHOT_ARM", "1", 1);
    /* This machine's radeonsi (hardware GL) path stalls Kryon apps in a
     * Mesa syncobj wait: the window freezes after a few frames and input
     * stops. Render with llvmpipe unless the caller picked a driver
     * (Neon's run scripts do the same). */
    setenv("GALLIUM_DRIVER", "llvmpipe", 0);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(screen_w, screen_h, "Krait " KRAIT_VERSION_STRING);
    if(InstanceRejected()) {
        /* another krait holds the instance lock (same window title) and
         * could not be replaced; exit cleanly instead of rendering with
         * no GPU context. A desktop notification makes the failure
         * visible for launches without a terminal. */
        fprintf(stderr, "krait: another krait instance is already running\n");
        if(IsNotificationSupported())
            SendNotification("Krait",
                             "Another krait instance is already running.");
        return 1;
    }
    window_icon_asset = GetUIIconAsset(UI_ICON_TYPE_PROJ_KRYON);
    if(window_icon_asset != NULL) {
        window_icon = LoadImageFromMemory(".png", window_icon_asset->data,
                                          (int)window_icon_asset->size);
        if(window_icon.data != NULL) {
            SetWindowIcon(window_icon);
            UnloadImage(window_icon);
        }
    }
    SetTargetFPS(60);
    /* UI scale: real device density on Android, overridable on desktop for
     * smoke testing. Drives every ScaleUIPx() call site and the
     * IsUIDesktopMode() mobile breakpoint. */
    float ui_scale = 1.0f;
#if defined(PLATFORM_ANDROID) || defined(__ANDROID__) || defined(ANDROID)
    {
        Vector2 dpid = GetWindowScaleDPI();
        if(dpid.x > 1.0f)
            ui_scale = dpid.x;
    }
#endif
    if(g_ui_scale_override > 0.0f)
        ui_scale = g_ui_scale_override;
    if(ui_scale < 1.0f)
        ui_scale = 1.0f;
    if(ui_scale > 4.0f)
        ui_scale = 4.0f;
    InitUI(screen_w, screen_h, ui_scale);
    krait_init_logical_keys();
    SetKeyPlatformCallbacks(krait_update_logical_keys,
                              krait_logical_key_pressed,
                              krait_logical_key_down);
    SetThemeSource(GetDefaultPlatformThemeSource());
    SetThemeMode(GetDefaultPlatformThemeMode());
    SetThemeStyle(THEME_STYLE_SYSTEM);
    SetCurrentTheme(GetDefaultThemeForThemeStyle(GetEffectiveThemeStyle()),
                    GetEffectiveThemeDarkMode() ? 1 : 0);
    ide_app_init();
    open_startup_project(project_arg);
    if(view_arg != NULL) {
        int view = krait_view_from_name(view_arg);
        if(view < 0) {
            fprintf(stderr, "krait: unknown view '%s' (expected "
                    "start|studio|text|kanban|agent|game|settings)\n", view_arg);
            return 2;
        }
        if(view == IDE_VIEW_PROJECT && istate.project.loaded == 0) {
            /* studio without a project has nothing to show: open the
             * project picker modal instead */
            istate.picker_open = 1;
        } else {
            istate.view = view;
        }
    }

    /* optional auto-open of the generic Level editor (KRAIT_LEVEL=1);
     * the project root is resolved on the first Game-view frame */
    if(getenv("KRAIT_LEVEL") != NULL && getenv("KRAIT_LEVEL")[0] != '\0')
        krait_level_set_active(1, NULL);

    if(smoke_live_reload) {
        result = run_live_reload_smoke(project_arg, live_rel_path);
    } else if(smoke_live) {
        result = run_live_smoke(project_arg, live_rel_path);
    } else if(smoke_screens || smoke_ide) {
        result = run_smoke(project_arg, smoke_screenshot_path, smoke_ide);
    } else {
        while(!WindowShouldClose()) {
            UpdateKeyPlatformState();
            /* The declarative App is the frame body only: the caller owns
             * Begin/End frame + UI scope (same wrapper run_smoke uses).
             * Without EndFrame nothing swaps, waits, or polls events: the
             * window stays black with dead input at 100% CPU. */
            BeginFrame();
            BeginUIFrame(GetScreenWidth(), GetScreenHeight(), GetUIScale());
            BeginUI(Key("ide_app_App"));
            ide_app_App();
            EndUI();
            EndUIFrame();
            EndFrame();
        }
    }

    /* drop the instance lock before the slow shutdown path so a fresh
     * launch takes over instantly instead of racing our cleanup */
    KryonReleaseInstanceLock();
    ide_app_shutdown();
    krait_shutdown_logical_keys();
    ClearUIFonts();
    CloseWindow();
    return result;
}
