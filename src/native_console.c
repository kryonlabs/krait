/*
 * Krait Studio console: the Kapsule terminal emulator embedded as the bottom
 * panel "Kapsule" tab. The emulator core is compiled straight from the
 * vendored Kapsule checkout (vendor/kapsule/src/terminal*.c + friends); this
 * file only owns the IDE-side wiring: fonts, theming, focus, input routing,
 * and the cell/cursor/sixel rendering pass. The emulator itself stays in the
 * Kapsule repository; nothing here leaks into Kryon.
 */

#include "kryon.h"
#include "terminal.h" /* Kapsule emulator (vendor/kapsule/src) */
#include "session.h"
#include "selection.h"
#include "input.h"
#include "palette.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KRAIT_CONSOLE_SCROLLBACK 2000
#define KRAIT_CONSOLE_FONT "kapsule-terminal"
#define KRAIT_CONSOLE_FONT_CJK "kapsule-terminal-cjk"
#define KRAIT_CONSOLE_FONT_SYMBOLS "kapsule-terminal-symbols"
#define KRAIT_CONSOLE_FONT_EMOJI "kapsule-terminal-emoji"

typedef struct KraitConsole {
    Session session;
    Selection selection;
    Palette palette;
    Rectangle viewport;
    int first_visible_row;
    int visible_rows;
    int cell_w;
    int line_h;
    int font_size;
    int focused;
    int window_focused;
    int fonts_ready;
    int mouse_report_col;
    int mouse_report_row;
    int mouse_report_x;
    int mouse_report_y;
    int mouse_report_button;
    double selection_click_time;
    int selection_click_row;
    int selection_click_col;
    int selection_click_count;
    int paste_shortcut_down;
    int copy_shortcut_down;
    double bell_until;
    int last_cols;
    int last_rows;
    char last_cwd[1024];
} KraitConsole;

static KraitConsole g_console;

static int
max_int(int a, int b)
{
    return a > b ? a : b;
}

static int
clamp_int(int value, int low, int high)
{
    if(value < low)
        return low;
    if(value > high)
        return high;
    return value;
}

static Color
blend_color(Color a, Color b, float t)
{
    if(t < 0.0f)
        t = 0.0f;
    if(t > 1.0f)
        t = 1.0f;
    return (Color){(unsigned char)((float)a.r + ((float)b.r - (float)a.r) * t),
                   (unsigned char)((float)a.g + ((float)b.g - (float)a.g) * t),
                   (unsigned char)((float)a.b + ((float)b.b - (float)a.b) * t),
                   a.a};
}

/* ---- glyph charset for the monospace terminal font (Kapsule's set) ------- */

typedef struct CodepointBuilder {
    int *values;
    int count;
    int capacity;
} CodepointBuilder;

static int
codepoints_append(CodepointBuilder *builder, int codepoint)
{
    if(builder == NULL)
        return 0;
    if(builder->count >= builder->capacity) {
        int capacity = builder->capacity > 0 ? builder->capacity * 2 : 256;
        int *values = realloc(builder->values, (size_t)capacity * sizeof(int));

        if(values == NULL)
            return 0;
        builder->values = values;
        builder->capacity = capacity;
    }
    builder->values[builder->count++] = codepoint;
    return 1;
}

static int
codepoints_append_range(CodepointBuilder *builder, int first, int last)
{
    int codepoint;

    for(codepoint = first; codepoint <= last; codepoint++) {
        if(!codepoints_append(builder, codepoint))
            return 0;
    }
    return 1;
}

static int *
console_codepoints(int *out_count)
{
    CodepointBuilder builder = {0};
    static const int individual[] = {
        0x1f310, /* globe */
        0x1f4a1, /* light bulb */
        0x1f600, /* grinning face */
        0x6e2c, 0x8a66
    };
    int i;

    if(out_count == NULL)
        return NULL;
    *out_count = 0;
    if(!codepoints_append_range(&builder, 0x0020, 0x007e) ||
       !codepoints_append_range(&builder, 0x00a0, 0x024f) ||
       !codepoints_append_range(&builder, 0x0300, 0x036f) ||
       !codepoints_append_range(&builder, 0x0370, 0x03ff) ||
       !codepoints_append_range(&builder, 0x0400, 0x052f) ||
       !codepoints_append_range(&builder, 0x2000, 0x206f) ||
       !codepoints_append_range(&builder, 0x2070, 0x209f) ||
       !codepoints_append_range(&builder, 0x20a0, 0x20cf) ||
       !codepoints_append_range(&builder, 0x2100, 0x214f) ||
       !codepoints_append_range(&builder, 0x2150, 0x218f) ||
       !codepoints_append_range(&builder, 0x2190, 0x21ff) ||
       !codepoints_append_range(&builder, 0x2200, 0x22ff) ||
       !codepoints_append_range(&builder, 0x2300, 0x23ff) ||
       !codepoints_append_range(&builder, 0x2400, 0x243f) ||
       !codepoints_append_range(&builder, 0x2460, 0x24ff) ||
       !codepoints_append_range(&builder, 0x2500, 0x259f) ||
       !codepoints_append_range(&builder, 0x25a0, 0x25ff) ||
       !codepoints_append_range(&builder, 0x2600, 0x27bf) ||
       !codepoints_append_range(&builder, 0x2800, 0x28ff) ||
       !codepoints_append_range(&builder, 0x2b00, 0x2bff) ||
       !codepoints_append_range(&builder, 0x1f300, 0x1f5ff) ||
       !codepoints_append_range(&builder, 0x1f600, 0x1f64f) ||
       !codepoints_append_range(&builder, 0xe0a0, 0xe0ff) ||
       !codepoints_append_range(&builder, 0xe700, 0xe7c5) ||
       !codepoints_append_range(&builder, 0xf000, 0xf2ff)) {
        free(builder.values);
        return NULL;
    }
    for(i = 0; i < (int)(sizeof(individual) / sizeof(individual[0])); i++) {
        if(!codepoints_append(&builder, individual[i])) {
            free(builder.values);
            return NULL;
        }
    }
    *out_count = builder.count;
    return builder.values;
}

static int *
console_cjk_codepoints(int *out_count)
{
    CodepointBuilder builder = {0};

    if(out_count == NULL)
        return NULL;
    *out_count = 0;
    if(!codepoints_append_range(&builder, 0x3040, 0x309f) ||
       !codepoints_append_range(&builder, 0x30a0, 0x30ff) ||
       !codepoints_append_range(&builder, 0x3400, 0x34ff) ||
       !codepoints_append_range(&builder, 0x4e00, 0x52ff) ||
       !codepoints_append_range(&builder, 0x6e00, 0x6eff) ||
       !codepoints_append_range(&builder, 0x8a00, 0x8aff)) {
        free(builder.values);
        return NULL;
    }
    *out_count = builder.count;
    return builder.values;
}

static int
fontconfig_match_charset(unsigned int codepoint, char *out, int out_size)
{
#if !defined(_WIN32) && !defined(PLATFORM_WEB)
    char command[96];
    char line[512];
    FILE *pipe;
    size_t len;

    if(out == NULL || out_size <= 0 || codepoint == 0)
        return 0;
    snprintf(command, sizeof(command),
             "fc-match -f '%%{file}\\n' ':charset=%x'", codepoint);
    pipe = popen(command, "r");
    if(pipe == NULL)
        return 0;
    if(fgets(line, sizeof(line), pipe) == NULL) {
        pclose(pipe);
        return 0;
    }
    pclose(pipe);
    len = strlen(line);
    while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                      line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';
    if(len == 0 || len >= (size_t)out_size || access(line, R_OK) != 0)
        return 0;
    snprintf(out, (size_t)out_size, "%s", line);
    return 1;
#else
    (void)codepoint;
    (void)out;
    (void)out_size;
    return 0;
#endif
}

static int
fontconfig_match_monospace(char *out, int out_size)
{
#if !defined(_WIN32) && !defined(PLATFORM_WEB)
    char line[512];
    FILE *pipe;
    size_t len;

    if(out == NULL || out_size <= 0)
        return 0;
    pipe = popen("fc-match -f '%{file}\\n' :mono", "r");
    if(pipe == NULL)
        return 0;
    if(fgets(line, sizeof(line), pipe) == NULL) {
        pclose(pipe);
        return 0;
    }
    pclose(pipe);
    len = strlen(line);
    while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                      line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';
    if(len == 0 || len >= (size_t)out_size || access(line, R_OK) != 0)
        return 0;
    snprintf(out, (size_t)out_size, "%s", line);
    return 1;
#else
    (void)out;
    (void)out_size;
    return 0;
#endif
}

static int
path_in_list(const char *const *paths, int count, const char *path)
{
    if(path == NULL || path[0] == '\0')
        return 1;
    for(int i = 0; i < count; i++) {
        if(paths[i] != NULL && strcmp(paths[i], path) == 0)
            return 1;
    }
    return 0;
}

static void
register_console_fallback(const char *name, const char *const *static_paths,
                          int static_count, unsigned int match_codepoint,
                          const int *codepoints, int codepoint_count)
{
    const char *tried[16];
    int tried_count = 0;
    char matched[512];
    int i;

    for(i = 0; i < static_count && static_paths[i] != NULL; i++) {
        const char *path = static_paths[i];

        if(path_in_list(tried, tried_count, path))
            continue;
        if(tried_count < (int)(sizeof(tried) / sizeof(tried[0])))
            tried[tried_count++] = path;
        if(RegisterUIFontFileSource(name, path, codepoints, codepoint_count))
            return;
    }
    if(fontconfig_match_charset(match_codepoint, matched, sizeof(matched)) &&
       !path_in_list(tried, tried_count, matched))
        (void)RegisterUIFontFileSource(name, matched, codepoints,
                                       codepoint_count);
}

static void
console_load_fonts(void)
{
    static const char *mono_paths[] = {
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-Regular.otf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/terminus/TerminusTTF-4.49.3.ttf",
        "../kryon/fonts/noto/NotoSans-Regular.ttf",
        "vendor/kryon/fonts/noto/NotoSans-Regular.ttf",
        NULL
    };
    static const char *cjk_paths[] = {
        "/usr/share/fonts/truetype/fonts-ukij-uyghur/UKIJCJK.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        NULL
    };
    static const char *symbol_paths[] = {
        "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/opentype/urw-base35/StandardSymbolsPS.otf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    static const char *emoji_paths[] = {
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    int codepoint_count = 0;
    int *codepoints = console_codepoints(&codepoint_count);
    int cjk_codepoint_count = 0;
    int *cjk_codepoints = console_cjk_codepoints(&cjk_codepoint_count);
    const char *kryon_dir = getenv("KRYON_DIR");
    char mono[1024];
    char app_font[1024];
    const char *app_dir = GetApplicationDirectory();
    int i;
    int loaded = 0;

    for(i = 0; mono_paths[i] != NULL; i++) {
        if(RegisterUIFontFileSource(KRAIT_CONSOLE_FONT, mono_paths[i],
                                    codepoints, codepoint_count)) {
            loaded = 1;
            break;
        }
    }
    if(!loaded && app_dir != NULL && app_dir[0] != '\0') {
        /* Packaged builds (AppImage) ship Noto next to the binary. */
        snprintf(app_font, sizeof(app_font),
                 "%s../share/krait/fonts/noto/NotoSans-Regular.ttf", app_dir);
        if(RegisterUIFontFileSource(KRAIT_CONSOLE_FONT, app_font, codepoints,
                                    codepoint_count))
            loaded = 1;
    }
    if(!loaded && kryon_dir != NULL && kryon_dir[0] != '\0') {
        snprintf(mono, sizeof(mono), "%s/fonts/noto/NotoSans-Regular.ttf",
                 kryon_dir);
        if(RegisterUIFontFileSource(KRAIT_CONSOLE_FONT, mono, codepoints,
                                    codepoint_count))
            loaded = 1;
    }
    if(!loaded && fontconfig_match_monospace(mono, sizeof(mono)))
        (void)RegisterUIFontFileSource(KRAIT_CONSOLE_FONT, mono, codepoints,
                                       codepoint_count);
    register_console_fallback(KRAIT_CONSOLE_FONT_CJK, cjk_paths,
                              (int)(sizeof(cjk_paths) / sizeof(cjk_paths[0])),
                              0x6e2c, cjk_codepoints, cjk_codepoint_count);
    register_console_fallback(
        KRAIT_CONSOLE_FONT_SYMBOLS, symbol_paths,
        (int)(sizeof(symbol_paths) / sizeof(symbol_paths[0])), 0x2800,
        codepoints, codepoint_count);
    register_console_fallback(
        KRAIT_CONSOLE_FONT_EMOJI, emoji_paths,
        (int)(sizeof(emoji_paths) / sizeof(emoji_paths[0])), 0x1f600,
        codepoints, codepoint_count);
    free(cjk_codepoints);
    free(codepoints);
}

/* ---- theming: follow the active Krait theme into the terminal colors ----- */

static void
seed_theme_defaults(TerminalState *terminal)
{
    TerminalPaneProfileState state;
    TerminalPaneProfileColors colors;
    TerminalPaneColors theme =
        ResolveTerminalPaneThemeColors(GetTerminalPaneThemeColors());

    if(terminal == NULL)
        return;
    colors = TerminalPaneProfileColorsFromTheme(theme);
    state.base_foreground = terminal->base_fg;
    state.base_background = terminal->base_bg;
    state.base_cursor = terminal->base_cursor_color;
    state.base_selection_foreground = terminal->base_selection_fg;
    state.base_selection_background = terminal->base_selection_bg;
    state.foreground = terminal->default_fg;
    state.background = terminal->default_bg;
    state.cursor = terminal->cursor_color;
    state.selection_foreground = terminal->selection_fg;
    state.selection_background = terminal->selection_bg;
    TerminalPaneProfileStateSeedMissing(&state, colors);
    terminal->base_fg = state.base_foreground;
    terminal->base_bg = state.base_background;
    terminal->base_cursor_color = state.base_cursor;
    terminal->base_selection_fg = state.base_selection_foreground;
    terminal->base_selection_bg = state.base_selection_background;
    terminal->default_fg = state.foreground;
    terminal->default_bg = state.background;
    terminal->cursor_color = state.cursor;
    terminal->selection_fg = state.selection_foreground;
    terminal->selection_bg = state.selection_background;
}

static Color
resolve_terminal_color(const TerminalState *terminal, int value,
                       Color fallback)
{
    return ResolveTerminalPaneColorWithOverrides(
        &g_console.palette.terminal_palette,
        terminal != NULL ? terminal->palette_overrides : NULL, value,
        fallback);
}

static TerminalPaneViewColors
terminal_view_colors(const TerminalState *terminal,
                     TerminalPaneColors theme_colors)
{
    TerminalPaneProfileColors colors = {
        COLOR_DEFAULT, COLOR_DEFAULT, COLOR_DEFAULT, COLOR_DEFAULT,
        COLOR_DEFAULT
    };

    if(terminal != NULL) {
        colors.foreground = terminal->default_fg;
        colors.background = terminal->default_bg;
        colors.cursor = terminal->cursor_color;
        colors.selection_foreground = terminal->selection_fg;
        colors.selection_background = terminal->selection_bg;
    }
    return ResolveTerminalPaneViewColors(
        &g_console.palette.terminal_palette,
        terminal != NULL ? terminal->palette_overrides : NULL, colors,
        theme_colors);
}

/* ---- clipboard: selection + terminal OSC 52 buffers ---------------------- */

static TerminalPaneClipboardActions
console_clipboard_actions(void)
{
    TerminalPaneClipboardActions actions = {0};
    TerminalPaneClipboardController selection = {0};
    TerminalPaneClipboardController terminal = {0};

    selection = selection_clipboard_controller(&g_console.selection,
                                               &g_console.session.terminal);
    terminal = MakeTerminalPaneClipboardController(
        terminal_clipboard(&g_console.session.terminal), NULL, NULL, NULL,
        NULL, 0, 0, &g_console.session.scroll_offset);
    return MakeTerminalPaneClipboardActions(selection, terminal);
}

static void
console_copy_selection(void)
{
    if(!g_console.selection.active)
        return;
    (void)TerminalPaneClipboardCopy(console_clipboard_actions());
}

static void
console_paste_preferred(void)
{
    (void)TerminalPaneClipboardPasteActionsPreferred(
        console_clipboard_actions());
}

/* ---- rendering (Kapsule's per-cell pass, Krait-themed) ------------------- */

static int
cell_text(const Cell *cell, char *text, int text_size)
{
    int len = 0;
    unsigned int codepoint;

    if(cell == NULL)
        return 0;
    codepoint = cell->codepoint;
    if(text == NULL || text_size < 2 || codepoint == 0 || codepoint == ' ') {
        if(text != NULL && text_size > 0)
            text[0] = '\0';
        return 0;
    }
    if(!AppendTerminalPaneUTF8Codepoint(text, text_size, &len, codepoint))
        return 0;
    if(cell->combining != 0)
        AppendTerminalPaneUTF8Codepoint(text, text_size, &len,
                                        cell->combining);
    text[len] = '\0';
    return len;
}

static void
draw_line_cells(const TerminalState *terminal,
                TerminalPaneViewColors view_colors,
                TerminalPaneColors theme_colors, int visible_row, int y)
{
    const KraitConsole *con = &g_console;
    int col;

    if(terminal == NULL || visible_row < 0 ||
       visible_row >= terminal_visible_line_count(terminal))
        return;
    for(col = 0; col < terminal->cols; col++) {
        const Cell *cell = terminal_visible_cell(terminal, col, visible_row);
        char text[16];
        int len = 0;
        Color fg;
        Color bg;
        Color underline;
        int linked;
        int selected =
            selection_contains(&g_console.selection, visible_row, col);

        if(cell == NULL)
            continue;
        if((cell->style & STYLE_WIDE_CONT) != 0)
            continue;
        fg = resolve_terminal_color(terminal, cell->fg,
                                    view_colors.foreground);
        bg = resolve_terminal_color(terminal, cell->bg,
                                    view_colors.background);
        underline = resolve_terminal_color(terminal, cell->underline, fg);
        linked = cell->hyperlink > 0;
        if(linked)
            fg = theme_colors.link;
        if((cell->style & STYLE_INVERSE) != 0) {
            Color tmp = fg;

            fg = bg;
            bg = tmp;
        }
        if((cell->style & STYLE_FAINT) != 0)
            fg = blend_color(fg, bg, 0.45f);
        if(selected) {
            DrawRectangle((int)con->viewport.x + col * con->cell_w, y,
                          con->cell_w, con->line_h,
                          view_colors.selection_background);
            fg = view_colors.selection_foreground;
            underline = view_colors.selection_foreground;
        } else if(cell->bg != COLOR_DEFAULT ||
                  (cell->style & STYLE_INVERSE) != 0) {
            DrawRectangle((int)con->viewport.x + col * con->cell_w, y,
                          con->cell_w, con->line_h, bg);
        }
        if(cell->codepoint == 0 || cell->codepoint == ' ')
            continue;
        if((cell->style & STYLE_CONCEAL) != 0)
            continue;
        if((cell->style & STYLE_BLINK) != 0 &&
           ((int)(GetTime() * 2.0) & 1) != 0)
            continue;
        len = cell_text(cell, text, sizeof(text));
        if(len <= 0)
            continue;
        Text(text, (int)con->viewport.x + col * con->cell_w, y,
                   con->font_size, fg);
        if(linked && !selected)
            underline = theme_colors.link;
        if((cell->style & STYLE_UNDERLINE) != 0 || linked)
            DrawRectangle((int)con->viewport.x + col * con->cell_w,
                          y + con->line_h - 3, con->cell_w, 1, underline);
        if((cell->style & STYLE_OVERLINE) != 0)
            DrawRectangle((int)con->viewport.x + col * con->cell_w,
                          y + 1, con->cell_w, 1, underline);
    }
}

static void
draw_sixel_images(const TerminalState *terminal,
                  TerminalPaneViewColors view_colors)
{
    const KraitConsole *con = &g_console;
    int i;

    if(terminal == NULL)
        return;
    for(i = 0; i < terminal_sixel_count(terminal); i++) {
        const SixelImage *image = terminal_sixel_image(terminal, i);
        int visible_row;
        int origin_x;
        int origin_y;
        int y;
        float pixel_w;
        float pixel_h;

        if(image == NULL || image->pixels == NULL ||
           image->alternate_screen != terminal->alternate_screen)
            continue;
        pixel_h = (float)con->line_h / 6.0f;
        pixel_w = (float)con->cell_w / 6.0f;
        if(image->pixel_aspect_num > 0 && image->pixel_aspect_den > 0)
            pixel_w *= (float)image->pixel_aspect_num /
                       (float)image->pixel_aspect_den;
        if(pixel_w <= 0.0f)
            pixel_w = (float)con->cell_w / 6.0f;
        if(pixel_h <= 0.0f)
            pixel_h = 1.0f;
        visible_row =
            terminal->alternate_screen ? image->row
                                       : terminal->scrollback_count + image->row;
        origin_x = (int)con->viewport.x + image->col * con->cell_w;
        origin_y = (int)con->viewport.y +
                   (visible_row - con->first_visible_row) * con->line_h;
        for(y = 0; y < image->height; y++) {
            float dest_y = (float)origin_y + (float)y * pixel_h;
            int x = 0;

            if(dest_y + pixel_h <= con->viewport.y ||
               dest_y >= con->viewport.y + con->viewport.height)
                continue;
            while(x < image->width) {
                int pixel = image->pixels[y * image->width + x];
                int run = 1;
                Color color;

                if(pixel == COLOR_DEFAULT) {
                    x++;
                    continue;
                }
                while(x + run < image->width &&
                      image->pixels[y * image->width + x + run] == pixel)
                    run++;
                if((float)origin_x + ((float)x + (float)run) * pixel_w >
                       con->viewport.x &&
                   (float)origin_x + (float)x * pixel_w <
                       con->viewport.x + con->viewport.width) {
                    Rectangle pixel_run;

                    color = resolve_terminal_color(terminal, pixel,
                                                   view_colors.foreground);
                    pixel_run = (Rectangle){
                        (float)origin_x + (float)x * pixel_w,
                        dest_y,
                        (float)run * pixel_w,
                        pixel_h
                    };
                    DrawRectangleRec(pixel_run, color);
                }
                x += run;
            }
        }
    }
}

/* ---- input: mouse -> grid mapping ---------------------------------------- */

static int
visible_row_from_mouse(Vector2 mouse)
{
    const KraitConsole *con = &g_console;
    int row;

    if(!CheckCollisionPointRec(mouse, con->viewport))
        return -1;
    row = (int)((mouse.y - con->viewport.y) / (float)con->line_h);
    if(row < 0 || row >= con->visible_rows)
        return -1;
    return con->first_visible_row + row;
}

static int
visible_col_from_mouse(Vector2 mouse)
{
    const KraitConsole *con = &g_console;
    int col;

    if(!CheckCollisionPointRec(mouse, con->viewport))
        return -1;
    col = (int)((mouse.x - con->viewport.x) / (float)con->cell_w);
    return max_int(0, col);
}

static int
viewport_row_from_mouse(Vector2 mouse)
{
    const KraitConsole *con = &g_console;
    int row;

    if(!CheckCollisionPointRec(mouse, con->viewport))
        return -1;
    row = (int)((mouse.y - con->viewport.y) / (float)con->line_h);
    if(row < 0 || row >= con->visible_rows)
        return -1;
    return row;
}

static int
viewport_col_from_mouse(const TerminalState *terminal, Vector2 mouse)
{
    const KraitConsole *con = &g_console;
    int col;

    if(terminal == NULL || !CheckCollisionPointRec(mouse, con->viewport))
        return -1;
    col = (int)((mouse.x - con->viewport.x) / (float)con->cell_w);
    return clamp_int(col, 0, max_int(0, terminal->cols - 1));
}

static int
open_hyperlink_at_mouse(const TerminalState *terminal, Vector2 mouse)
{
    const KraitConsole *con = &g_console;
    int viewport_row;
    int visible_row;
    int col;
    const Cell *cell;
    const char *url;

    if(!CheckCollisionPointRec(mouse, con->viewport))
        return 0;
    viewport_row = viewport_row_from_mouse(mouse);
    col = viewport_col_from_mouse(terminal, mouse);
    if(viewport_row < 0 || col < 0)
        return 0;
    visible_row = con->first_visible_row + viewport_row;
    cell = terminal_visible_cell(terminal, col, visible_row);
    if(cell == NULL || cell->hyperlink <= 0)
        return 0;
    url = terminal_hyperlink(terminal, cell->hyperlink);
    if(url == NULL || url[0] == '\0')
        return 0;
    OpenURL(url);
    return 1;
}

static void
handle_console_selection(TerminalState *terminal)
{
    KraitConsole *con = &g_console;
    Vector2 mouse = GetMousePosition();
    int row = visible_row_from_mouse(mouse);
    int col = visible_col_from_mouse(mouse);

    if(terminal == NULL)
        return;
    if(row >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        double now = GetTime();

        if(row == con->selection_click_row &&
           col == con->selection_click_col &&
           now - con->selection_click_time < 0.45) {
            con->selection_click_count++;
        } else {
            con->selection_click_count = 1;
        }
        con->selection_click_time = now;
        con->selection_click_row = row;
        con->selection_click_col = col;
        if(con->selection_click_count >= 3) {
            selection_select_line(&con->selection, terminal, row);
            return;
        }
        if(con->selection_click_count == 2) {
            selection_select_word(&con->selection, terminal, row, col);
            return;
        }
        selection_begin_char(&con->selection, row, col);
    }
    if(con->selection.dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        int scroll_delta = selection_edge_scroll_delta(
            mouse.y, con->viewport.y, con->viewport.height, ScaleUIPx(24));

        if(row < 0 || scroll_delta != 0) {
            int total = terminal_visible_line_count(terminal);
            int max_scroll = max_int(0, total - con->visible_rows);
            int first_visible_row = con->first_visible_row;

            if(scroll_delta != 0) {
                con->session.scroll_offset = clamp_int(
                    con->session.scroll_offset + scroll_delta, 0, max_scroll);
                first_visible_row = selection_first_visible_row(
                    total, con->visible_rows, con->session.scroll_offset);
                con->first_visible_row = first_visible_row;
            }
            row = selection_edge_scroll_row(first_visible_row,
                                            con->visible_rows, scroll_delta);
            if(row < 0)
                row = mouse.y < con->viewport.y
                         ? first_visible_row
                         : first_visible_row + con->visible_rows - 1;
            col = visible_col_from_mouse((Vector2){
                (float)clamp_int((int)mouse.x, (int)con->viewport.x,
                                 (int)(con->viewport.x +
                                       con->viewport.width - 1)),
                (float)clamp_int((int)mouse.y, (int)con->viewport.y,
                                 (int)(con->viewport.y +
                                       con->viewport.height - 1))
            });
        }
        if(row >= 0)
            selection_update_end(&con->selection, terminal, row, col);
    }
    if(con->selection.dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        con->selection.dragging = 0;
        (void)TerminalPaneClipboardUpdatePrimary(console_clipboard_actions());
    }
}

static int
send_mouse_button(TerminalState *terminal, int ray_button, int terminal_button,
                  int col, int row, int pixel_x, int pixel_y, int mods)
{
    KraitConsole *con = &g_console;

    if(IsMouseButtonPressed(ray_button)) {
        con->mouse_report_button = terminal_button;
        con->mouse_report_col = col;
        con->mouse_report_row = row;
        con->mouse_report_x = pixel_x;
        con->mouse_report_y = pixel_y;
        return terminal_send_mouse_pixels(terminal, terminal_button, col, row,
                                          pixel_x, pixel_y, 1, 0, mods);
    }
    if(IsMouseButtonReleased(ray_button)) {
        int result = terminal_send_mouse_pixels(
            terminal, terminal_button, col, row, pixel_x, pixel_y, 0, 0, mods);

        if(con->mouse_report_button == terminal_button)
            con->mouse_report_button = TERMINAL_MOUSE_RELEASE;
        con->mouse_report_col = col;
        con->mouse_report_row = row;
        con->mouse_report_x = pixel_x;
        con->mouse_report_y = pixel_y;
        return result;
    }
    return 0;
}

static int
handle_console_mouse(TerminalState *terminal, float wheel)
{
    KraitConsole *con = &g_console;
    Vector2 mouse;
    int row;
    int col;
    int pixel_x;
    int pixel_y;
    int mods;
    int consumed = 0;
    int steps;
    int i;
    int shift;

    shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    mouse = GetMousePosition();
    if(terminal->mouse_mode == 0 || shift ||
       !CheckCollisionPointRec(mouse, con->viewport))
        return 0;
    row = viewport_row_from_mouse(mouse);
    col = viewport_col_from_mouse(terminal, mouse);
    if(row < 0 || col < 0)
        return 0;
    row = clamp_int(row, 0, max_int(0, terminal->rows - 1));
    pixel_x = clamp_int((int)(mouse.x - con->viewport.x), 0,
                        max_int(0, (int)con->viewport.width - 1));
    pixel_y = clamp_int((int)(mouse.y - con->viewport.y), 0,
                        max_int(0, (int)con->viewport.height - 1));
    mods = input_mods();

    if(wheel != 0.0f) {
        int button = wheel > 0.0f ? TERMINAL_MOUSE_WHEEL_UP
                                  : TERMINAL_MOUSE_WHEEL_DOWN;

        steps = wheel > 0.0f ? (int)(wheel + 0.5f) : (int)((-wheel) + 0.5f);
        if(steps < 1)
            steps = 1;
        for(i = 0; i < steps; i++) {
            if(terminal_send_mouse_pixels(terminal, button, col, row, pixel_x,
                                          pixel_y, 1, 0, mods))
                consumed = 1;
        }
    }
    if(send_mouse_button(terminal, MOUSE_BUTTON_LEFT, TERMINAL_MOUSE_LEFT, col,
                         row, pixel_x, pixel_y, mods))
        consumed = 1;
    if(send_mouse_button(terminal, MOUSE_BUTTON_MIDDLE, TERMINAL_MOUSE_MIDDLE,
                         col, row, pixel_x, pixel_y, mods))
        consumed = 1;
    if(send_mouse_button(terminal, MOUSE_BUTTON_RIGHT, TERMINAL_MOUSE_RIGHT,
                         col, row, pixel_x, pixel_y, mods))
        consumed = 1;

    if((terminal->mouse_mode == 1002 &&
        con->mouse_report_button != TERMINAL_MOUSE_RELEASE) ||
       terminal->mouse_mode == 1003) {
        if(col != con->mouse_report_col || row != con->mouse_report_row ||
           (terminal->mouse_pixels &&
            (pixel_x != con->mouse_report_x ||
             pixel_y != con->mouse_report_y))) {
            int button = con->mouse_report_button;

            if(terminal_send_mouse_pixels(terminal, button, col, row, pixel_x,
                                          pixel_y, 1, 1, mods))
                consumed = 1;
            con->mouse_report_col = col;
            con->mouse_report_row = row;
            con->mouse_report_x = pixel_x;
            con->mouse_report_y = pixel_y;
        }
    }
    return consumed;
}

static int
console_write_shortcut(void *userdata, const char *text, int length)
{
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    (void)userdata;
    if(text == NULL)
        return 0;
    if((length == 6 && memcmp(text, "\x1b[2;2~", 6) == 0) ||
       (length == 4 && shift && memcmp(text, "\x1b[2~", 4) == 0)) {
        console_paste_preferred();
        return 1;
    }
    if(length == 6 && memcmp(text, "\x1b[2;5~", 6) == 0) {
        console_copy_selection();
        return 1;
    }
    return 0;
}

static int
console_key_shortcut(void *userdata, int platform_key, int mods)
{
    int ctrl = (mods & TERMINAL_PANE_MOD_CTRL) != 0;
    int shift = (mods & TERMINAL_PANE_MOD_SHIFT) != 0;

    (void)userdata;
    if(ctrl && shift && platform_key == KEY_V) {
        console_paste_preferred();
        return 1;
    }
    if((ctrl && shift && platform_key == KEY_C) ||
       (ctrl && platform_key == KEY_INSERT)) {
        console_copy_selection();
        return 1;
    }
    return 0;
}

static void
handle_console_input(KraitConsole *con)
{
    TerminalState *terminal = &con->session.terminal;
    float wheel;
    int ctrl;
    int shift;
    int opened_link = 0;

    if(!con->session.used)
        return;
    ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    wheel = GetMouseWheelMove();
    /* Clicking the terminal viewport takes focus from the editor. */
    if(!con->focused &&
       CheckCollisionPointRec(GetMousePosition(), con->viewport) &&
       IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        con->focused = 1;
    if(!handle_console_mouse(terminal, wheel)) {
        if(CheckCollisionPointRec(GetMousePosition(), con->viewport) &&
           wheel != 0.0f) {
            int direction = wheel > 0.0f ? 1 : -1;
            int steps = wheel > 0.0f ? (int)(wheel + 0.5f)
                                     : (int)((-wheel) + 0.5f);
            int consumed = 0;
            int i;

            if(steps < 1)
                steps = 1;
            for(i = 0; i < steps; i++) {
                if(terminal_send_alternate_scroll(terminal, direction,
                                                  input_mods()))
                    consumed = 1;
            }
            if(consumed) {
                con->session.scroll_offset = 0;
            } else {
                con->session.scroll_offset += (int)(wheel * 3.0f);
                if(con->session.scroll_offset < 0)
                    con->session.scroll_offset = 0;
            }
        }
        if(terminal->mouse_mode == 0 && ctrl &&
           CheckCollisionPointRec(GetMousePosition(), con->viewport) &&
           IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
           open_hyperlink_at_mouse(terminal, GetMousePosition())) {
            con->selection.active = 0;
            con->session.scroll_offset = 0;
            opened_link = 1;
        } else if(terminal->mouse_mode == 0 &&
                  CheckCollisionPointRec(GetMousePosition(), con->viewport) &&
                  IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
            console_paste_preferred();
        }
        if(!opened_link)
            handle_console_selection(terminal);
    } else {
        con->selection.active = 0;
        con->session.scroll_offset = 0;
    }
    /* Keyboard only flows to the terminal while the console holds focus. */
    if(!con->focused || !IsWindowFocused())
        return;
    /* Krait's global Ctrl+Shift+F / Ctrl+P tool shortcuts keep priority. */
    if(ctrl && shift && IsKeyPressed(KEY_F))
        return;
    if(ctrl && !shift && IsKeyPressed(KEY_P))
        return;
    if(IsKeyPressed(KEY_PAGE_UP) && shift) {
        con->session.scroll_offset += con->visible_rows;
        return;
    }
    if(IsKeyPressed(KEY_PAGE_DOWN) && shift) {
        con->session.scroll_offset -= con->visible_rows;
        if(con->session.scroll_offset < 0)
            con->session.scroll_offset = 0;
        return;
    }
    input_send_keyboard_filtered(terminal, console_key_shortcut,
                                 console_write_shortcut, NULL);
    if(IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_BACKSPACE) ||
       IsKeyPressed(KEY_TAB))
        con->session.scroll_offset = 0;
}

/* ---- public API (ide/console.kry is the thin UI on top) ------------------ */

void
krait_console_init(void)
{
    KraitConsole *con = &g_console;

    memset(con, 0, sizeof(*con));
    session_init(&con->session);
    con->mouse_report_button = TERMINAL_MOUSE_RELEASE;
    con->selection_click_row = -1;
    con->selection_click_col = -1;
}

void
krait_console_shutdown(void)
{
    session_close(&g_console.session);
    memset(&g_console, 0, sizeof(g_console));
}

int
krait_console_running(void)
{
    return g_console.session.terminal.running != 0;
}

const char *
krait_console_title(void)
{
    return session_title(&g_console.session);
}

int krait_console_restart(const char *cwd);

int
krait_console_ensure(const char *cwd, int cols, int rows)
{
    KraitConsole *con = &g_console;
    TerminalState *terminal = &con->session.terminal;

    if(con->session.used && terminal->running) {
        terminal_resize(terminal, cols, rows);
        return 1;
    }
    if(cwd == NULL || cwd[0] == '\0')
        cwd = GetWorkingDirectory();
    snprintf(con->last_cwd, sizeof(con->last_cwd), "%s", cwd);
    con->last_cols = cols;
    con->last_rows = rows;
    selection_clear(&con->selection);
    if(!session_open(&con->session, cwd, NULL, NULL, cols, rows,
                     KRAIT_CONSOLE_SCROLLBACK))
        return 0;
    seed_theme_defaults(&con->session.terminal);
    return 1;
}

int
krait_console_ensure_cwd(const char *cwd, int cols, int rows)
{
    KraitConsole *con = &g_console;
    char current[1024];

    if(cwd == NULL || cwd[0] == '\0')
        cwd = GetWorkingDirectory();
    if(con->session.used && con->session.terminal.running) {
        session_current_cwd(&con->session, current, sizeof(current));
        if(strcmp(current, cwd) == 0) {
            terminal_resize(&con->session.terminal, cols, rows);
            return 1;
        }
        return krait_console_restart(cwd);
    }
    return krait_console_ensure(cwd, cols, rows);
}

void
krait_console_stop(void)
{
    session_close(&g_console.session);
    selection_clear(&g_console.selection);
    g_console.focused = 0;
}

int
krait_console_restart(const char *cwd)
{
    KraitConsole *con = &g_console;
    int cols = con->last_cols > 0 ? con->last_cols : 80;
    int rows = con->last_rows > 0 ? con->last_rows : 24;

    if(cwd == NULL || cwd[0] == '\0')
        cwd = con->last_cwd[0] != '\0' ? con->last_cwd : NULL;
    session_close(&con->session);
    session_init(&con->session);
    selection_clear(&con->selection);
    return krait_console_ensure(cwd, cols, rows);
}

void
krait_console_send(const char *text)
{
    if(text == NULL || text[0] == '\0')
        return;
    (void)krait_console_ensure(NULL, 80, 24);
    terminal_write_text(&g_console.session.terminal, text);
}

void
krait_console_feed(const char *text)
{
    if(text == NULL || text[0] == '\0')
        return;
    (void)krait_console_ensure(NULL, 80, 24);
    terminal_feed(&g_console.session.terminal, text, (int)strlen(text));
}

void
krait_console_poll(void)
{
    KraitConsole *con = &g_console;

    if(!con->session.used)
        return;
    (void)TerminalPaneClipboardActionsSyncFromHost(console_clipboard_actions());
    (void)terminal_poll_bytes(&con->session.terminal);
    if(terminal_consume_bell(&con->session.terminal))
        con->bell_until = GetTime() + 0.18;
    {
        int focused = IsWindowFocused() ? 1 : 0;

        if(focused != con->window_focused) {
            terminal_send_focus(&con->session.terminal, focused);
            con->window_focused = focused;
        }
    }
    (void)TerminalPaneClipboardActionsFlushToHost(console_clipboard_actions());
}

void
krait_console_copy(void)
{
    console_copy_selection();
}

void
krait_console_paste(void)
{
    console_paste_preferred();
}

int
krait_console_draw(Rectangle bounds, int focused, int font_size)
{
    KraitConsole *con = &g_console;
    TerminalState *terminal = &con->session.terminal;
    TerminalPaneMetrics metrics;
    TerminalPaneColors theme_colors;
    TerminalPaneViewColors view_colors;
    int total_rows;
    int max_scroll;
    int row;
    int font_token;

    if(!con->fonts_ready) {
        console_load_fonts();
        con->fonts_ready = 1;
    }
    con->focused = focused;
    con->font_size = font_size;
    seed_theme_defaults(terminal);
    palette_default(&con->palette);
    font_token = PushUIFont(KRAIT_CONSOLE_FONT);
    metrics = MeasureTerminalPaneContent(bounds, font_size);
    PopUIFont(font_token);
    con->cell_w = metrics.cell_width;
    con->line_h = metrics.line_height;
    con->viewport = metrics.content;
    con->visible_rows = metrics.rows;
    terminal_resize(terminal, metrics.cols, metrics.rows);
    con->last_cols = metrics.cols;
    con->last_rows = metrics.rows;

    total_rows = terminal_visible_line_count(terminal);
    max_scroll = max_int(0, total_rows - con->visible_rows);
    con->session.scroll_offset =
        clamp_int(con->session.scroll_offset, 0, max_scroll);
    con->first_visible_row = selection_first_visible_row(
        total_rows, con->visible_rows, con->session.scroll_offset);
    theme_colors = ResolveTerminalPaneThemeColors(GetTerminalPaneThemeColors());
    view_colors = terminal_view_colors(terminal, theme_colors);

    DrawRectangleRec(con->viewport, view_colors.background);
    BeginScissorMode((int)con->viewport.x, (int)con->viewport.y,
                     (int)con->viewport.width, (int)con->viewport.height);
    draw_sixel_images(terminal, view_colors);
    font_token = PushUIFont(KRAIT_CONSOLE_FONT);
    for(row = 0; row < con->visible_rows; row++) {
        int visible_row = con->first_visible_row + row;
        int y = (int)con->viewport.y + row * con->line_h;

        draw_line_cells(terminal, view_colors, theme_colors, visible_row, y);
    }
    if(terminal->cursor_visible && con->session.scroll_offset == 0 &&
       (!terminal->cursor_blink || ((int)(GetTime() * 2.0) & 1) == 0)) {
        int cursor_visible_row =
            terminal->alternate_screen
                ? terminal->cursor_row
                : terminal->scrollback_count + terminal->cursor_row;
        int cursor_y = cursor_visible_row - con->first_visible_row;

        if(cursor_y >= 0 && cursor_y < con->visible_rows) {
            int x = (int)con->viewport.x + terminal->cursor_col * con->cell_w;
            int y = (int)con->viewport.y + cursor_y * con->line_h;
            int style =
                terminal->cursor_style != TERMINAL_CURSOR_DEFAULT
                    ? terminal->cursor_style
                    : TERMINAL_CURSOR_BLOCK;

            if(style == TERMINAL_CURSOR_BAR) {
                DrawRectangle(x, y, 2, con->line_h, view_colors.cursor);
            } else if(style == TERMINAL_CURSOR_UNDERLINE) {
                DrawRectangle(x, y + con->line_h - 3, con->cell_w, 2,
                              view_colors.cursor);
            } else {
                const Cell *cell = terminal_cell(terminal, terminal->cursor_col,
                                                 terminal->cursor_row);
                char text[16];

                DrawRectangle(x, y, con->cell_w, con->line_h,
                              view_colors.cursor);
                if(cell != NULL && cell_text(cell, text, sizeof(text)) > 0)
                    Text(text, x, y, con->font_size,
                               view_colors.background);
            }
        }
    }
    PopUIFont(font_token);
    EndScissorMode();

    if(con->bell_until > GetTime()) {
        float alpha = (float)((con->bell_until - GetTime()) / 0.18);
        Color overlay = theme_colors.bell_overlay;
        Color border = theme_colors.bell_border;

        alpha = alpha < 0.0f ? 0.0f : alpha;
        alpha = alpha > 1.0f ? 1.0f : alpha;
        overlay.a = (unsigned char)((float)overlay.a * alpha);
        border.a = (unsigned char)((float)border.a * alpha);
        DrawRectangleRec(con->viewport, overlay);
        DrawRectangleLines((int)con->viewport.x, (int)con->viewport.y,
                           (int)con->viewport.width,
                           (int)con->viewport.height, border);
    }
    if(con->session.scroll_offset > 0) {
        DrawTerminalPaneScrollIndicator((TerminalPaneScrollIndicator){
            con->viewport,
            con->session.scroll_offset,
            ScaleUIPx(13),
            theme_colors
        });
    }
    handle_console_input(con);
    return con->focused;
}
