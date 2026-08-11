#ifndef KRAIT_NATIVE_INTERNAL_H
#define KRAIT_NATIVE_INTERNAL_H

#include <stddef.h>   /* size_t */
#include <stdarg.h>   /* krait_live_status is variadic */

/*
 * Shared private header for the native_*.c family (the split of the former
 * src/native.c). Declares the shared constants, the one struct that crosses
 * files (KraitProjectMeta), and every helper that more than one native_*.c
 * translation unit calls. Single-domain globals and structs stay in their
 * owning .c file.
 *
 * Generated/external types used here (IdeState, ScreenEntry, Rectangle, Color,
 * AppHost, AppRouteInfo) come from "ide/state.h" and "kryon.h", which every
 * native_*.c includes before this header.
 */

#define KRAIT_PATH_MAX 1024
#define KRAIT_MAX_RECENT 12
#define KRAIT_MAX_EXAMPLES 32
#define KRAIT_SEARCH_DEPTH 8
#define KRAIT_TREE_DEPTH 8
#define KRAIT_MTIME_DEPTH 8
#define KRAIT_SCREEN_DEPTH 8
#define KRAIT_SCENE_NODE_MAX 256

/* Owned by native_meta.c, but native_meta_field/_save/_load and the project
 * draw path reference it, so the type is visible here. The definition lives in
 * native_meta.c; callers use it as an opaque pointer except via the meta API. */
typedef struct KraitProjectMeta KraitProjectMeta;

/* Live-preview execution context. Shared between native_live_eval.c (which owns
 * the struct + the var table) and native_live.c (which drives exec/draw). */
typedef struct {
    const char *root;
    const char *rel_path;
    const char *text;
    const char *body;
    const char *body_end;
    int ok;
    int line_no;
    int render_count;
    int delegate_count;
    int call_depth;
    char status[512];
} KraitLive;

typedef struct {
    char name[64];
    int value;
} KraitLiveVar;

/* ---- native_util.c: shared string/filesystem helpers ---- */
void krait_join(char *dst, size_t dst_size, const char *a, const char *b);
const char *krait_basename(const char *path);
char *krait_trim(char *s);
int krait_path_exists(const char *path);
int krait_path_has_suffix(const char *path, const char *suffix);
int krait_ignored_dir(const char *name);
void krait_ensure_parent_dir(const char *path);
int krait_read_file_alloc(const char *path, char **out, long *out_len);
int krait_write_text_file(const char *path, const char *text);
int krait_file_is_text(const char *path);
int krait_tree_id(const char *path);
int krait_ident_start(int ch);
int krait_ident_char(int ch);
void krait_title_from_file(char *dst, size_t dst_size, const char *file);

/* ---- native_live_eval.c: pure parser/eval helpers (shared by scene + live) ---- */
const char *krait_live_skip_space(const char *p);
int krait_live_starts_word(const char *p, const char *word);
char *krait_live_trim(char *s);
int krait_live_parse_string(char **sp, char *dst, size_t dst_size);
int krait_live_split_args(const char *src, char args[][256], int cap);
int krait_live_call_args(const char *line, const char *name, char args[][256], int cap);
int krait_live_eval_int(const char *expr, int *out);
int krait_live_next_scale_arg(char **sp, int *out);
int krait_live_parse_ident(char **sp, char *dst, size_t dst_size);
int krait_live_eval_color(const char *expr, Color *out);
UIButtonStyle krait_live_eval_button_style(const char *expr);
int krait_live_find_named_body(const char *text, const char *keyword, const char *name,
                               const char **body, const char **body_end);
int krait_live_find_function_body(const char *text, const char *name,
                                  const char **body, const char **body_end);
int krait_live_line_for_ptr(const char *text, const char *ptr);
int krait_live_find_frame_name(const char *text, char *dst, size_t dst_size);
/* live var table + status: owned by native_live_eval.c, used by native_live.c */
int krait_live_var_get(const char *name, int *out);
void krait_live_var_set(const char *name, int value);
void krait_live_status(KraitLive *live, const char *fmt, ...);
void krait_live_vars_clear(void);

/* ---- cross-file entry points called between domain files ---- */
int krait_project_has_make_target(const char *root, const char *target);
int krait_project_preview_config(const char *root, char *live_path,
                                 size_t live_path_size, char *build_command,
                                 size_t build_command_size);
void krait_meta_load(KraitProjectMeta *meta, const char *root);
int krait_meta_save(KraitProjectMeta *meta, char *status, int status_size);
void krait_meta_field(Rectangle rect, const char *label, char *text,
                      size_t text_size, int *cursor, int *focused, int id);
int krait_add_screen(ScreenEntry *out, int count, int cap, const char *name,
                     const char *rel_path, int line, int insert_offset);
const char *krait_scan_project_quoted(const char *p, char *out, size_t out_size);
int krait_scan_project_scenes(const char *root, ScreenEntry *out, int count, int cap);
int krait_live_draw_source(const char *root, const char *rel_path, int w, int h,
                           char *status, int status_size);
int krait_preview_build(IdeState *st, char *status, int status_size);
int krait_preview_draw(IdeState *st, const char *rel_path, Rectangle viewport,
                       char *status, int status_size);
void krait_preview_unload(void);
int krait_live_draw_canvas(const char *root, const char *rel_path, int w, int h,
                           char *status, int status_size);
int krait_game_node_args(const char *q, char args[8][256], char *type,
                         size_t type_size, char *label, size_t label_size,
                         int *x, int *y, int *w, int *h);
void krait_draw_game_node(const char *type, const char *label, int x, int y, int w, int h);

#endif /* KRAIT_NATIVE_INTERNAL_H */
