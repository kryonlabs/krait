/* Level — a small, dedicated kryon tile-level editor.
 *
 * Deliberately minimal: a window, the generic level editor from
 * native_level.c, and nothing else. It opens any project that has
 * levels/*.level (or <sub>/levels/*.level) and edits every level
 * directly; games load the same files. Built as a separate binary so
 * the editor stays small and debuggable independently of the IDE.
 *
 * Usage: level <project-dir>
 */
#include "kryon.h"
#include "native_level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_root[1024];

int main(int argc, char **argv)
{
    const char *project = argc > 1 ? argv[1] : ".";
    const char *select = getenv("LEVEL_SELECT");

    snprintf(g_root, sizeof g_root, "%s", project);

    /* This machine's radeonsi GL path stalls Kryon apps (Mesa syncobj
     * wait); render with llvmpipe unless the caller chose a driver. */
    setenv("GALLIUM_DRIVER", "llvmpipe", 0);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1400, 900, "Level");
    if(InstanceRejected()) {
        fprintf(stderr, "level: another instance holds the lock\n");
        return 1;
    }
    SetTargetFPS(60);

    krait_level_set_active(1, g_root);
    if(select != NULL && select[0] != '\0')
        krait_level_open(select);

    while(!WindowShouldClose()) {
        BeginFrame();
        BeginUIFrame(GetScreenWidth(), GetScreenHeight(),
                     GetUIScale());
        BeginUI(Key("level_app"));
        {
            Rectangle bounds = { 0, 0, (float)GetScreenWidth(),
                                 (float)GetScreenHeight() };
            krait_level_draw_view(bounds);
        }
        EndUI();
        EndUIFrame();
        EndFrame();

        if(IsKeyPressed(KEY_LEFT_CONTROL) && IsKeyPressed(KEY_S)) {
            char st[160];

            if(krait_level_save(st, sizeof st))
                printf("%s\n", st);
        }
        if(IsKeyPressed(KEY_ESCAPE))
            break;
    }

    krait_level_shutdown();
    CloseWindow();
    return 0;
}
