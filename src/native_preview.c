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
    void *dylib;
    AppHost *host;
    DestroyAppHostCallback destroy_host;
    int defer_unload;
    char project_path[KRAIT_PATH_MAX];
    char host_path[KRAIT_PATH_MAX];
} KraitPreviewHost;

typedef struct {
    KryProcess proc;
    int running;
    char project_path[KRAIT_PATH_MAX];
    char host_path[KRAIT_PATH_MAX];
    char output[8192];
} KraitPreviewBuild;

static KraitPreviewHost g_preview_host;
static KraitPreviewBuild g_preview_build;
static unsigned long g_preview_host_generation;

/* Used by krait_preview_build_poll before its own definition later in this file. */
static int krait_preview_load_host(const char *project_path,
                                   const char *host_path,
                                   char *status, int status_size);

static int
krait_preview_env_begin(char *old_value, size_t old_value_size)
{
    const char *old = getenv("KRYON_INSPECT");
    int had = old != NULL;

    if(old_value != NULL && old_value_size > 0)
        snprintf(old_value, old_value_size, "%s", old != NULL ? old : "");
    setenv("KRYON_INSPECT", "1", 1);
    return had;
}

static void
krait_preview_env_end(int had, const char *old_value)
{
    if(had)
        setenv("KRYON_INSPECT", old_value != NULL ? old_value : "", 1);
    else
        unsetenv("KRYON_INSPECT");
}

static int
krait_shell_quote(char *dst, size_t dst_size, const char *src)
{
    size_t n = 0;

    if(dst == NULL || dst_size == 0)
        return 0;
    if(src == NULL)
        src = "";
    if(n + 1 >= dst_size)
        return 0;
    dst[n++] = '\'';
    for(const char *p = src; *p != '\0'; p++) {
        if(*p == '\'') {
            const char *q = "'\\''";
            for(int i = 0; q[i] != '\0'; i++) {
                if(n + 1 >= dst_size)
                    return 0;
                dst[n++] = q[i];
            }
        } else {
            if(n + 1 >= dst_size)
                return 0;
            dst[n++] = *p;
        }
    }
    if(n + 1 >= dst_size)
        return 0;
    dst[n++] = '\'';
    dst[n] = '\0';
    return 1;
}

static int
krait_copy_file(const char *src, const char *dst)
{
    FILE *in;
    FILE *out;
    char buf[16384];
    size_t n;

    if(src == NULL || dst == NULL)
        return 0;
    in = fopen(src, "rb");
    if(in == NULL)
        return 0;
    out = fopen(dst, "wb");
    if(out == NULL) {
        fclose(in);
        return 0;
    }
    while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if(fwrite(buf, 1, n, out) != n) {
            fclose(out);
            fclose(in);
            return 0;
        }
    }
    if(ferror(in)) {
        fclose(out);
        fclose(in);
        return 0;
    }
    fclose(out);
    fclose(in);
    return 1;
}

static int
krait_write_preview_shim(const char *path)
{
    FILE *file = fopen(path, "w");

    if(file == NULL)
        return 0;
    fputs("#include \"kryon.h\"\n"
          "#include \"app_host.h\"\n"
          "\n"
          "void *\n"
          "CreateApp(const char *project_path)\n"
          "{\n"
          "    (void)project_path;\n"
          "    return (void *)1;\n"
          "}\n"
          "\n"
          "void\n"
          "DestroyApp(void *app)\n"
          "{\n"
          "    (void)app;\n"
          "}\n"
          "\n"
          "void\n"
          "ApplyRoute(void *app, const AppRouteInfo *route)\n"
          "{\n"
          "    (void)app;\n"
          "    (void)route;\n"
          "}\n"
          "\n"
          "void\n"
          "BeginScreenDraw(void *app, Rectangle viewport)\n"
          "{\n"
          "    (void)app;\n"
          "    SetUIViewSize((int)viewport.width, (int)viewport.height);\n"
          "}\n",
          file);
    fclose(file);
    return 1;
}

static const char *
krait_preview_last_output_line(void)
{
    static char last[512];
    const char *line = g_preview_build.output;

    snprintf(last, sizeof(last), "%s", "Preview build failed");
    for(const char *p = g_preview_build.output; *p != '\0'; p++) {
        if(*p == '\n' && p[1] != '\0')
            line = p + 1;
    }
    if(line[0] != '\0') {
        size_t n = strcspn(line, "\r\n");
        if(n >= sizeof(last))
            n = sizeof(last) - 1;
        memcpy(last, line, n);
        last[n] = '\0';
    }

    return last;
}

static void
krait_preview_build_append(const char *text)
{
    size_t have;
    size_t add;

    if(text == NULL || text[0] == '\0')
        return;
    have = strlen(g_preview_build.output);
    add = strlen(text);
    if(add >= sizeof(g_preview_build.output)) {
        text += add - sizeof(g_preview_build.output) + 1;
        add = strlen(text);
        have = 0;
    } else if(have + add + 1 > sizeof(g_preview_build.output)) {
        size_t drop = have + add + 1 - sizeof(g_preview_build.output);
        memmove(g_preview_build.output, g_preview_build.output + drop,
                have - drop + 1);
        have -= drop;
    }
    memcpy(g_preview_build.output + have, text, add + 1);
}

static void
krait_preview_build_drain(void)
{
    char buf[1024];
    int n;

    while((n = kry_process_read_poll(&g_preview_build.proc, buf,
                                     sizeof(buf))) > 0)
        krait_preview_build_append(buf);
}

static void
krait_preview_build_close(void)
{
    if(g_preview_build.running)
        kry_process_close(&g_preview_build.proc);
    memset(&g_preview_build, 0, sizeof(g_preview_build));
}

static int
krait_preview_build_poll(char *status, int status_size)
{
    if(!g_preview_build.running)
        return 0;
    krait_preview_build_drain();
    if(!kry_process_wait_poll(&g_preview_build.proc)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview building...");
        return 0;
    }
    krait_preview_build_drain();
    if(g_preview_build.proc.exit_status != 0) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "%s",
                     krait_preview_last_output_line());
        krait_preview_build_close();
        return 0;
    }
    if(!krait_preview_load_host(g_preview_build.project_path,
                                g_preview_build.host_path,
                                status, status_size)) {
        krait_preview_build_close();
        return 0;
    }
    krait_preview_build_close();
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Preview built");
    return 1;
}

const char *
krait_build_output(void)
{
    return g_preview_build.output;
}

/* Output pane (ide/output.kry) "Clear" button target. */
void
krait_output_clear(void)
{
    g_preview_build.output[0] = '\0';
}

static int
krait_preview_build_start(const char *project_path, const char *host_path,
                          const char *command, char *status, int status_size)
{
    krait_preview_build_close();
    memset(&g_preview_build, 0, sizeof(g_preview_build));
    snprintf(g_preview_build.project_path, sizeof(g_preview_build.project_path),
             "%s", project_path);
    snprintf(g_preview_build.host_path, sizeof(g_preview_build.host_path),
             "%s", host_path);
    if(!kry_process_spawn(&g_preview_build.proc, command, project_path)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview build spawn failed");
        return 0;
    }
    g_preview_build.running = 1;
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Preview building...");
    return 0;
}

static void
krait_preview_unload_host(int reloading)
{
    if(g_preview_host.defer_unload && !reloading) {
        memset(&g_preview_host, 0, sizeof(g_preview_host));
        return;
    }
    if(g_preview_host.destroy_host != NULL && g_preview_host.host != NULL)
        g_preview_host.destroy_host(g_preview_host.host);
    if(g_preview_host.defer_unload) {
        memset(&g_preview_host, 0, sizeof(g_preview_host));
        return;
    }
    if(g_preview_host.dylib != NULL)
        kry_dylib_close(g_preview_host.dylib);
    memset(&g_preview_host, 0, sizeof(g_preview_host));
}

void
krait_preview_unload(void)
{
    krait_preview_build_close();
    krait_preview_unload_host(1);
}

static int
krait_preview_load_host(const char *project_path, const char *host_path,
                        char *status, int status_size)
{
    void *dylib;
    CreateAppHostCallback create_host;
    DestroyAppHostCallback destroy_host;
    AppHost *host;
    const char *err;
    char load_path[KRAIT_PATH_MAX];
    char old_inspect_copy[64];
    UIFrameState saved_frame;
    int had_inspect;
    int has_project_host;
    char configured_live[KRAIT_PATH_MAX];

    configured_live[0] = '\0';
    has_project_host = krait_project_has_make_target(project_path, "kryon-host");
    if(krait_project_preview_config(project_path, configured_live,
                                    sizeof(configured_live), NULL, 0) &&
       configured_live[0] != '\0')
        has_project_host = 1;
    snprintf(load_path, sizeof(load_path), "%s", host_path);
    if(has_project_host) {
        char copy_path[KRAIT_PATH_MAX];

        snprintf(copy_path, sizeof(copy_path), "%s.%lu.so", host_path,
                 ++g_preview_host_generation);
        if(!krait_copy_file(host_path, copy_path)) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size,
                         "Preview host copy failed");
            return 0;
        }
        snprintf(load_path, sizeof(load_path), "%s", copy_path);
    }

    dylib = kry_dylib_load(load_path);
    if(dylib == NULL) {
        err = kry_dylib_error();
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview load failed: %s",
                     err != NULL ? err : load_path);
        return 0;
    }
    create_host = (CreateAppHostCallback)kry_dylib_sym(dylib, "CreateAppHost");
    destroy_host = (DestroyAppHostCallback)kry_dylib_sym(dylib, "DestroyAppHost");
    if(create_host == NULL || destroy_host == NULL) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size,
                     "Preview host missing CreateAppHost/DestroyAppHost");
        kry_dylib_close(dylib);
        return 0;
    }
    had_inspect = krait_preview_env_begin(old_inspect_copy,
                                          sizeof(old_inspect_copy));
    saved_frame = SaveUIFrameState();
    host = create_host(APP_HOST_ABI_VERSION, project_path);
    RestoreUIFrameState(saved_frame);
    krait_preview_env_end(had_inspect, old_inspect_copy);
    if(host == NULL) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size,
                     "Preview host rejected ABI %d", APP_HOST_ABI_VERSION);
        kry_dylib_close(dylib);
        return 0;
    }

    krait_preview_unload_host(0);
    g_preview_host.dylib = dylib;
    g_preview_host.host = host;
    g_preview_host.destroy_host = destroy_host;
    g_preview_host.defer_unload = has_project_host ? 1 : 0;
    snprintf(g_preview_host.project_path, sizeof(g_preview_host.project_path),
             "%s", project_path);
    snprintf(g_preview_host.host_path, sizeof(g_preview_host.host_path),
             "%s", load_path);
    return 1;
}

int
krait_preview_build(IdeState *st, char *status, int status_size)
{
    const char *kryon_dir;
    char build_dir[KRAIT_PATH_MAX];
    char shim_path[KRAIT_PATH_MAX];
    char host_path[KRAIT_PATH_MAX];
    char configured_live[KRAIT_PATH_MAX];
    char configured_build[KRAIT_PATH_MAX * 4];
    char q_kryon[KRAIT_PATH_MAX * 2];
    char kc_tool[KRAIT_PATH_MAX];
    char q_kc[KRAIT_PATH_MAX * 2];
    char command[KRAIT_PATH_MAX * 12];
    int has_project_host;

    if(status != NULL && status_size > 0)
        status[0] = '\0';
    if(st == NULL || !st->project.loaded || st->project.path[0] == '\0') {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "No project loaded");
        return 0;
    }
    if(g_preview_build.running &&
       strcmp(g_preview_build.project_path, st->project.path) == 0)
        return krait_preview_build_poll(status, status_size);
    if(g_preview_build.running)
        krait_preview_build_close();
    kryon_dir = getenv("KRYON_DIR");
    if(kryon_dir == NULL || kryon_dir[0] == '\0')
        kryon_dir = "vendor/kryon";
    if(!krait_shell_quote(q_kryon, sizeof(q_kryon), kryon_dir)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview path too long");
        return 0;
    }
    krait_kryon_tool_path(kc_tool, sizeof(kc_tool), "k2c");
    if(!krait_shell_quote(q_kc, sizeof(q_kc), kc_tool)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Preview path too long");
        return 0;
    }

    krait_join(build_dir, sizeof(build_dir), st->project.path, "build/kryon");
    krait_join(shim_path, sizeof(shim_path), build_dir, "preview_shim.c");
    krait_join(host_path, sizeof(host_path), build_dir, "app_host.so");
    if(kry_fs_mkdir_p(build_dir) != 0) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot create preview build dir");
        return 0;
    }
    configured_live[0] = '\0';
    configured_build[0] = '\0';
    (void)krait_project_preview_config(st->project.path, configured_live,
                                       sizeof(configured_live),
                                       configured_build,
                                       sizeof(configured_build));
    if(configured_live[0] != '\0') {
        krait_join(host_path, sizeof(host_path), st->project.path,
                   configured_live);
        if((st->preview_dirty == 0 || configured_build[0] == '\0') &&
           krait_path_exists(host_path) &&
           krait_preview_load_host(st->project.path, host_path, status,
                                   status_size)) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size, "Live host ready");
            return 1;
        }
        if(configured_build[0] != '\0')
            return krait_preview_build_start(st->project.path, host_path,
                                             configured_build, status,
                                             status_size);
        if(krait_preview_load_host(st->project.path, host_path, status,
                                   status_size)) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size, "Live host ready");
            return 1;
        }
        return 0;
    }
    has_project_host = krait_project_has_make_target(st->project.path, "kryon-host");
    if(has_project_host) {
        snprintf(command, sizeof(command),
                 "gmake -f Makefile build/kryon/generated/.fresh kryon-host KRYON_DIR=%s",
                 q_kryon);
        return krait_preview_build_start(st->project.path, host_path,
                                         command, status, status_size);
    }
    if(!krait_write_preview_shim(shim_path)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot write preview shim");
        return 0;
    }

    snprintf(command, sizeof(command),
             "mkdir -p build/kryon/gen && "
             "%s --no-main --root . -o build/kryon/gen "
             "$(find . -name '*.kry' -not -path './build/*' -not -path './vendor/*' | sort) && "
             "cc -shared -fPIC "
             "-Ibuild/kryon/gen -I%s/include -I%s/src/ui -I%s/vendor/clay "
             "-o build/kryon/app_host.so "
             "$(find build/kryon/gen -name '*.c' | sort) build/kryon/preview_shim.c",
             q_kc, kryon_dir, kryon_dir, kryon_dir);
    return krait_preview_build_start(st->project.path, host_path,
                                     command, status, status_size);
}

int
krait_preview_draw(IdeState *st, const char *rel_path, Rectangle viewport,
                   char *status, int status_size)
{
    char old_inspect[64];
    int had_inspect;
    int ok;

    if(status != NULL && status_size > 0)
        status[0] = '\0';
    if(st == NULL || rel_path == NULL || rel_path[0] == '\0') {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "No .kry source selected");
        return 0;
    }
    ok = krait_live_draw_source(st->project.path, rel_path,
                                (int)viewport.width,
                                (int)viewport.height,
                                status, status_size);
    if(ok && (status == NULL || strstr(status, "delegates") == NULL))
        return 1;
    if(g_preview_host.host == NULL ||
       strcmp(g_preview_host.project_path, st->project.path) != 0) {
        if(!krait_preview_build(st, status, status_size)) {
            if(status != NULL && strcmp(status, "Preview building...") == 0)
                return 1;
            return 0;
        }
    }
    had_inspect = krait_preview_env_begin(old_inspect, sizeof(old_inspect));
    if(!SetAppScreenBySourcePath(g_preview_host.host, rel_path)) {
        krait_preview_env_end(had_inspect, old_inspect);
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size,
                     "Preview route not found: %s", rel_path);
        return ok;
    }
    DrawAppScreen(g_preview_host.host, viewport);
    krait_preview_env_end(had_inspect, old_inspect);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Live canvas ready");
    return 1;
}

/* Live-interpreter render into an offscreen texture, exported as PNG.
 * Pixels are read AFTER EndTextureMode (reading inside texture mode
 * poisons the framebuffer on llvmpipe). */
static int
live_render_captured(const char *root, const char *rel_path, int w, int h,
                     const char *png_path, char *status, int status_size)
{
    RenderTexture2D target;
    int ok;

    target = LoadRenderTexture(w, h);
    BeginTextureMode(target);
    ClearBackground(GetThemeBackground());
    ok = krait_live_draw_source(root, rel_path, w, h, status, status_size);
    EndTextureMode();
    if(ok) {
        Image shot = LoadImageFromTexture(target.texture);

        ExportImage(shot, png_path);
        UnloadImage(shot);
    }
    UnloadRenderTexture(target);
    return ok;
}

int
krait_live_draw_canvas_ex(const char *root, const char *rel_path, int w,
                          int h, const char *capture_png_path,
                          char *status, int status_size)
{
    char live_status[512];
    int live_ok;

    if(status != NULL && status_size > 0)
        status[0] = '\0';
    if(root == NULL || rel_path == NULL || rel_path[0] == '\0') {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "No .kry source selected");
        return 0;
    }
    if(capture_png_path != NULL && capture_png_path[0] != '\0')
        live_ok = live_render_captured(root, rel_path, w, h,
                                       capture_png_path, live_status,
                                       sizeof(live_status));
    else
        live_ok = krait_live_draw_source(root, rel_path, w, h,
                                         live_status, sizeof(live_status));
    if(live_ok && strstr(live_status, "delegates") == NULL) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "%s", live_status);
        return 1;
    }
    {
        char configured_live[KRAIT_PATH_MAX];

        configured_live[0] = '\0';
        if(krait_project_has_make_target(root, "kryon-host") ||
           (krait_project_preview_config(root, configured_live,
                                         sizeof(configured_live), NULL, 0) &&
            configured_live[0] != '\0')) {
        IdeState st;
        RenderTexture2D target;
        char build_status[512];
        int ready = 0;
        int ok;

        memset(&st, 0, sizeof(st));
        st.project.loaded = 1;
        snprintf(st.project.path, sizeof(st.project.path), "%s", root);
        snprintf(st.project.name, sizeof(st.project.name), "%s",
                 krait_basename(root));
        if(w <= 0)
            w = 640;
        if(h <= 0)
            h = 480;
        for(int i = 0; i < 600; i++) {
            build_status[0] = '\0';
            if(krait_preview_build(&st, build_status, sizeof(build_status))) {
                ready = 1;
                break;
            }
            if(strcmp(build_status, "Preview building...") != 0)
                break;
            usleep(50000);
        }
        if(!ready) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size, "%s",
                         build_status[0] != '\0' ? build_status :
                         "Preview build timed out");
            return 0;
        }

        target = LoadRenderTexture(w, h);
        BeginTextureMode(target);
        ClearBackground(GetThemeBackground());
        BeginUIFrame(w, h, 1.0);
        ok = krait_preview_draw(&st, rel_path,
                                (Rectangle){0, 0, (float)w, (float)h},
                                status, status_size);
        if(!ok && status != NULL &&
           strncmp(status, "Preview route not found", 23) == 0) {
            ClearBackground(GetThemeBackground());
            ok = krait_live_draw_source(root, rel_path, w, h,
                                        status, status_size);
        }
        EndUIFocus();
        EndTextureMode();
        if(capture_png_path != NULL && capture_png_path[0] != '\0') {
            /* read AFTER EndTextureMode (reading inside texture mode
             * poisons the framebuffer on llvmpipe) */
            Image shot = LoadImageFromTexture(target.texture);

            ExportImage(shot, capture_png_path);
            UnloadImage(shot);
        }
        UnloadRenderTexture(target);
        return ok;
        }
    }
    if(capture_png_path != NULL && capture_png_path[0] != '\0')
        return live_render_captured(root, rel_path, w, h, capture_png_path,
                                    status, status_size);
    return krait_live_draw_source(root, rel_path, w, h, status, status_size);
}


int
krait_live_draw_canvas(const char *root, const char *rel_path, int w, int h,
                       char *status, int status_size)
{
    return krait_live_draw_canvas_ex(root, rel_path, w, h, NULL, status,
                                     status_size);
}

/* Offscreen preview render exported as a PNG on disk - the agent's eyes. */
int
krait_live_capture_png(const char *root, const char *rel_path, int w, int h,
                       const char *png_path, char *status, int status_size)
{
    if(png_path == NULL || png_path[0] == '\0')
        return 0;
    return krait_live_draw_canvas_ex(root, rel_path, w, h, png_path, status,
                                     status_size);
}
