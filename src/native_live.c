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

/* Internal forward decls (definitions appear later in this file). */
static int krait_live_exec_body(KraitLive *live);

static int
krait_live_exec_assignment(char *line)
{
    char *p;
    char *op;
    char name[64];
    int value = 0;
    int current = 0;
    int add = 0;
    int sub = 0;
    size_t n;

    if(line == NULL)
        return 0;
    p = krait_live_trim(line);
    if(!krait_ident_char((unsigned char)p[0]) || isdigit((unsigned char)p[0]))
        return 0;
    n = 0;
    while(krait_ident_char((unsigned char)p[n]) && n + 1 < sizeof(name)) {
        name[n] = p[n];
        n++;
    }
    name[n] = '\0';
    p = krait_live_trim(p + n);
    if(p[0] == ':' && p[1] != ':') {
        while(*p != '\0' && *p != '=')
            p++;
    }
    if(p[0] == ':' && p[1] == '=') {
        op = p + 2;
    } else if(p[0] == '+' && p[1] == '=') {
        add = 1;
        op = p + 2;
    } else if(p[0] == '-' && p[1] == '=') {
        sub = 1;
        op = p + 2;
    } else if(p[0] == '=') {
        op = p + 1;
    } else {
        return 0;
    }
    op = krait_live_trim(op);
    if(strncmp(op, "(float)", 7) == 0)
        op = krait_live_trim(op + 7);
    if(!krait_live_eval_int(op, &value))
        return 1;
    if(add || sub)
        (void)krait_live_var_get(name, &current);
    if(add)
        value = current + value;
    else if(sub)
        value = current - value;
    krait_live_var_set(name, value);
    return 1;
}

static int
krait_live_exec_local_call(KraitLive *live, const char *line)
{
    char name[128];
    const char *body;
    const char *body_end;
    const char *saved_body;
    const char *saved_body_end;
    int saved_line;
    char *scan;
    char buf[512];

    if(live == NULL || line == NULL || live->call_depth >= 12)
        return 0;
    snprintf(buf, sizeof(buf), "%s", line);
    scan = krait_live_trim(buf);
    if(krait_live_starts_word(scan, "unused"))
        scan = krait_live_trim(scan + strlen("unused"));
    if(!krait_live_parse_ident(&scan, name, sizeof(name)))
        return 0;
    scan = krait_live_trim(scan);
    if(scan[0] != '(')
        return 0;
    if(!krait_live_find_function_body(live->text, name, &body, &body_end))
        return 0;

    saved_body = live->body;
    saved_body_end = live->body_end;
    saved_line = live->line_no;
    live->body = body;
    live->body_end = body_end;
    live->line_no = krait_live_line_for_ptr(live->text, body);
    live->call_depth++;
    krait_live_exec_body(live);
    live->call_depth--;
    live->body = saved_body;
    live->body_end = saved_body_end;
    live->line_no = saved_line;
    return live->ok;
}

static int
krait_live_exec_call(KraitLive *live, char *line)
{
    char args[8][256];
    int argc;
    char label[512];
    int x, y, w, h, font;
    Color color, border;
    char *q;
    char *scan;
    char node_type[64];

    q = krait_live_trim(line);
    if(q[0] == '\0' || q[0] == '#')
        return 1;
    if(strcmp(q, "BeginDrawing()") == 0 || strcmp(q, "EndDrawing()") == 0 ||
       strncmp(q, "BeginUIFrame(", 13) == 0 ||
       strcmp(q, "EndUIFocus()") == 0 || strcmp(q, "EndDrawing()") == 0)
        return 1;
    if(krait_live_starts_word(q, "args")) {
        live->delegate_count++;
        return 1;
    }
    if(krait_live_exec_assignment(q))
        return 1;
    if(strstr(q, " :: ") != NULL || strstr(q, ": ") != NULL ||
       strstr(q, ": [") != NULL || strcmp(q, "{") == 0 || strcmp(q, "}") == 0 ||
       strcmp(q, "} else {") == 0 || krait_live_starts_word(q, "return") ||
       krait_live_starts_word(q, "for") || krait_live_starts_word(q, "while"))
        return 1;
    argc = krait_live_call_args(q, "Background", args, 8);
    if(argc == 1) {
        if(!krait_live_eval_color(args[0], &color)) {
            krait_live_status(live, "%s:%d: unsupported color: %s",
                              live->rel_path, live->line_no, args[0]);
            return 0;
        }
        Background(color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "ClearBackground", args, 8);
    if(argc == 1) {
        if(krait_live_eval_color(args[0], &color))
            ClearBackground(color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Text", args, 8);
    if(argc == 5) {
        char *s = args[0];
        if(!krait_live_parse_string(&s, label, sizeof(label)) ||
           !krait_live_eval_int(args[1], &x) ||
           !krait_live_eval_int(args[2], &y) ||
           !krait_live_eval_int(args[3], &font) ||
           !krait_live_eval_color(args[4], &color)) {
            krait_live_status(live, "%s:%d: unsupported Text arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        Text(label, x, y, font, color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "DrawText", args, 8);
    if(argc == 5) {
        char *s = args[0];
        if(!krait_live_parse_string(&s, label, sizeof(label)) ||
           !krait_live_eval_int(args[1], &x) ||
           !krait_live_eval_int(args[2], &y) ||
           !krait_live_eval_int(args[3], &font) ||
           !krait_live_eval_color(args[4], &color)) {
            krait_live_status(live, "%s:%d: unsupported DrawText arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        DrawUIText(label, x, y, font, color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Rect", args, 8);
    if(argc == 6) {
        if(!krait_live_eval_int(args[0], &x) ||
           !krait_live_eval_int(args[1], &y) ||
           !krait_live_eval_int(args[2], &w) ||
           !krait_live_eval_int(args[3], &h) ||
           !krait_live_eval_color(args[4], &color) ||
           !krait_live_eval_color(args[5], &border)) {
            krait_live_status(live, "%s:%d: unsupported Rect arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        Rect(x, y, w, h, color, border);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Button", args, 8);
    if(argc == 1) {
        char *s = strchr(args[0], '"');
        scan = args[0];
        if(!krait_live_next_scale_arg(&scan, &x) ||
           !krait_live_next_scale_arg(&scan, &y) ||
           !krait_live_next_scale_arg(&scan, &w) ||
           !krait_live_next_scale_arg(&scan, &h) ||
           s == NULL ||
           !krait_live_parse_string(&s, label, sizeof(label))) {
            krait_live_status(live, "%s:%d: unsupported Button arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        DrawUIGenericButton(x, y, w, h, label,
                            krait_live_eval_button_style(args[0]), 0, NULL);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Picture", args, 8);
    if(argc == 1) {
        char asset[512];
        char *s = strchr(args[0], '"');
        scan = args[0];

        if(s == NULL ||
           !krait_live_parse_string(&s, asset, sizeof(asset)) ||
           !krait_live_next_scale_arg(&scan, &x) ||
           !krait_live_next_scale_arg(&scan, &y) ||
           !krait_live_next_scale_arg(&scan, &w) ||
           !krait_live_next_scale_arg(&scan, &h)) {
            krait_live_status(live, "%s:%d: unsupported Picture arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        Picture((PictureProps){asset, {x, y, w, h}, {0}, {0}, 0, WHITE,
                                    UI_PICTURE_FIT_STRETCH});
        live->render_count++;
        return 1;
    }
    if(krait_game_node_args(q, args, node_type, sizeof(node_type),
                            label, sizeof(label), &x, &y, &w, &h)) {
        krait_draw_game_node(node_type, label, x, y, w, h);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "TextField", args, 8);
    if(argc == 1) {
        scan = args[0];
        if(!krait_live_next_scale_arg(&scan, &x) ||
           !krait_live_next_scale_arg(&scan, &y) ||
           !krait_live_next_scale_arg(&scan, &w) ||
           !krait_live_next_scale_arg(&scan, &h)) {
            krait_live_status(live, "%s:%d: unsupported DrawUITextField arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        Rect(x, y, w, h, GetThemeSurface(), GetThemeButton());
        Text("Text field", x + ScaleUIPx(10), y + ScaleUIPx(8),
                   UI_TEXT_16, GetThemeIcon());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Toggle", args, 8);
    if(argc >= 7) {
        char off_label[128] = "Off";
        char on_label[128] = "On";
        char *off = args[5];
        char *on = args[6];

        if(!krait_live_eval_int(args[0], &x) ||
           !krait_live_eval_int(args[1], &y) ||
           !krait_live_eval_int(args[2], &w) ||
           !krait_live_eval_int(args[3], &h)) {
            krait_live_status(live, "%s:%d: unsupported DrawUIToggleSwitch arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        (void)krait_live_parse_string(&off, off_label, sizeof(off_label));
        (void)krait_live_parse_string(&on, on_label, sizeof(on_label));
        Rect(x, y, w, h, GetThemeButton(), GetThemeButtonHover());
        Text(off_label, x + ScaleUIPx(8), y + ScaleUIPx(6),
                   UI_TEXT_12, GetThemeText());
        Text(on_label, x + w - ScaleUIPx(32), y + ScaleUIPx(6),
                   UI_TEXT_12, GetThemeIcon());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Slider", args, 8);
    if(argc >= 8) {
        char slider_label[128] = "Value";
        char *s = args[4];

        if(!krait_live_eval_int(args[1], &x) ||
           !krait_live_eval_int(args[2], &y) ||
           !krait_live_eval_int(args[3], &w)) {
            krait_live_status(live, "%s:%d: unsupported DrawUISlider arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        (void)krait_live_parse_string(&s, slider_label, sizeof(slider_label));
        /* Match DrawUISlider's editor_bounds {x,y,w,56}: label row at y, track at y+28. */
        Text(slider_label, x, y, UI_TEXT_12, GetThemeText());
        Rect(x, y + ScaleUIPx(28), w, ScaleUIPx(8), GetThemeButton(), GetThemeButtonHover());
        Rect(x + w / 2 - ScaleUIPx(6), y + ScaleUIPx(21),
                   ScaleUIPx(12), ScaleUIPx(22), GetThemeLink(), GetThemeLink());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Line", args, 8);
    if(argc == 5) {
        int x2 = 0, y2 = 0;
        if(!krait_live_eval_int(args[0], &x) ||
           !krait_live_eval_int(args[1], &y) ||
           !krait_live_eval_int(args[2], &x2) ||
           !krait_live_eval_int(args[3], &y2) ||
           !krait_live_eval_color(args[4], &color)) {
            krait_live_status(live, "%s:%d: unsupported Line arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        DrawLine(x, y, x2, y2, color);
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "TextInRect", args, 8);
    if(argc >= 1) {
        char *s = args[0];
        char *scan = args[1];
        if(!krait_live_parse_string(&s, label, sizeof(label))) {
            krait_live_status(live, "%s:%d: unsupported TextInRect arguments",
                              live->rel_path, live->line_no);
            return 0;
        }
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        DrawUIText(label, x + ScaleUIPx(4), y + ScaleUIPx(4), UI_TEXT_12,
                   GetThemeText());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Progress", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = ScaleUIPx(20);
        Rect(x, y, w, h, GetThemeButton(), GetThemeButtonHover());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "ImageBox", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        Rect(x, y, w, h, GetThemeSurface(), GetThemeButton());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "LabelFrame", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        char *ls = strchr(args[0], '"');
        label[0] = '\0';
        if(ls != NULL)
            (void)krait_live_parse_string(&ls, label, sizeof(label));
        x = 0; y = 0; w = 0; h = 0;
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        Rect(x, y, w, h, (Color){0}, GetThemeButton());
        if(label[0] != '\0')
            DrawUIText(label, x + ScaleUIPx(8), y, UI_TEXT_12, GetThemeText());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "Separator", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 2;
        DrawLine(x, y, x + w, y + h, GetThemeButton());
        live->render_count++;
        return 1;
    }
    argc = krait_live_call_args(q, "BeginNodeGroup", args, 8);
    if(argc >= 2) {
        char *scan = args[1];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        krait_live_next_scale_arg(&scan, &h);
        DrawRectangleLines(x, y, w, h, GetThemeButton());
        live->render_count++;
        return 1;
    }
    if(strncmp(q, "EndNodeGroup", 12) == 0)
        return 1;
    if(krait_live_starts_word(q, "if")) {
        q = krait_live_trim(q + 2);
        argc = krait_live_call_args(q, "Button", args, 8);
        if(argc == 1)
            return krait_live_exec_call(live, q);
        return 1;
    }
    if(krait_live_exec_local_call(live, q))
        return 1;
    live->delegate_count++;
    return 1;
}

static int
krait_live_statement_open(const char *stmt)
{
    int depth = 0;
    int in_string = 0;

    if(stmt == NULL)
        return 0;
    for(const char *p = stmt; *p != '\0'; p++) {
        int ch = *p;

        if(in_string) {
            if(ch == '\\' && p[1] != '\0') {
                p++;
            } else if(ch == '"') {
                in_string = 0;
            }
            continue;
        }
        if(ch == '"') {
            in_string = 1;
        } else if(ch == '(' || ch == '[') {
            depth++;
        } else if((ch == ')' || ch == ']') && depth > 0) {
            depth--;
        }
    }
    return in_string || depth > 0;
}

static void
krait_live_append_statement_line(char *stmt, size_t stmt_size, const char *line)
{
    size_t len;
    size_t n;
    const char *start;

    if(stmt == NULL || stmt_size == 0 || line == NULL)
        return;
    len = strlen(stmt);
    start = line;
    while(*start == ' ' || *start == '\t')
        start++;
    n = strlen(start);
    while(n > 0 && (start[n - 1] == '\n' || start[n - 1] == '\r' ||
                    start[n - 1] == ' ' || start[n - 1] == '\t'))
        n--;
    if(n == 0)
        return;
    if(len > 0 && len + 1 < stmt_size)
        stmt[len++] = ' ';
    if(len + n >= stmt_size)
        n = stmt_size - len - 1;
    if(n > 0)
        memcpy(stmt + len, start, n);
    stmt[len + n] = '\0';
}

static int
krait_live_exec_body(KraitLive *live)
{
    const char *p;
    char stmt[4096];
    int stmt_line = 0;

    stmt[0] = '\0';
    for(p = live->body; live->ok && p < live->body_end;) {
        const char *nl = memchr(p, '\n', (size_t)(live->body_end - p));
        char line[1024];
        size_t n = nl != NULL ? (size_t)(nl - p) : (size_t)(live->body_end - p);
        if(n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        if(stmt[0] == '\0')
            stmt_line = live->line_no;
        krait_live_append_statement_line(stmt, sizeof(stmt), line);
        if(!krait_live_statement_open(stmt)) {
            int saved_line = live->line_no;

            live->line_no = stmt_line;
            if(!krait_live_exec_call(live, stmt))
                return 0;
            live->line_no = saved_line;
            stmt[0] = '\0';
        }
        if(nl == NULL)
            break;
        p = nl + 1;
        live->line_no++;
    }
    if(stmt[0] != '\0') {
        live->line_no = stmt_line;
        if(!krait_live_exec_call(live, stmt))
            return 0;
    }
    return live->ok;
}

int
krait_live_draw_source(const char *root, const char *rel_path, int w, int h,
                       char *status, int status_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    KraitLive live = {0};
    char frame[128];

    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot read %s", rel_path);
        return 0;
    }
    live.root = root;
    live.rel_path = rel_path;
    live.text = text;
    live.ok = 1;
    live.line_no = 1;
    krait_live_vars_clear();
    krait_live_var_set("view_width", w);
    krait_live_var_set("view_height", h);
    krait_live_var_set("content_w", w - ScaleUIPx(32));
    krait_live_var_set("content_h", h - ScaleUIPx(32));
    krait_live_var_set("x", ScaleUIPx(16));
    krait_live_var_set("y", ScaleUIPx(16));
    krait_live_var_set("btn_h", ScaleUIPx(44));
    krait_live_var_set("gap", ScaleUIPx(10));
    if(!krait_live_find_named_body(text, "screen", NULL, &live.body, &live.body_end)) {
        frame[0] = '\0';
        if(!krait_live_find_frame_name(text, frame, sizeof(frame)) ||
           !krait_live_find_named_body(text, frame, NULL, &live.body, &live.body_end)) {
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size,
                         "%s delegates rendering to other app code", rel_path);
            free(text);
            return 1;
        }
    }
    live.line_no = 1;
    for(const char *p = text; p < live.body; p++)
        if(*p == '\n')
            live.line_no++;
    krait_live_exec_body(&live);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "%s",
                 live.status[0] != '\0' ? live.status :
                 (live.render_count > 0 ? "Live canvas ready" :
                  "Screen delegates rendering to app code"));
    free(text);
    return live.ok;
}

