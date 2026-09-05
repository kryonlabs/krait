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

/* ---- native_ai.c: provider-selectable coding chat client (krait is the
 * harness; kryon supplies only the provider-neutral kry_http + kry_json
 * transport) ---- */
typedef struct KraitAiRequest KraitAiRequest;

typedef enum {
    KRAIT_AI_PENDING,
    KRAIT_AI_RUNNING,
    KRAIT_AI_DONE,
    KRAIT_AI_FAILED,
} KraitAiStatus;

typedef struct {
    const char *role;     /* "system" | "user" | "assistant" */
    const char *content;
    const char *image_b64;   /* optional PNG base64 -> multimodal turn */
} KraitAiMessage;

int krait_ai_provider_count(void);
const char *krait_ai_provider_id(int index);
const char *krait_ai_provider_name(int index);
const char *krait_ai_provider_key_env(int index);
const char *krait_ai_provider_model(int index);
const char *krait_ai_provider_base_url(int index);
int krait_ai_provider_configured(int index);
int krait_ai_active_provider(void);
int krait_ai_set_provider(int index);
void krait_ai_set_provider_key(int index, const char *api_key);
void krait_ai_set_provider_base_url(int index, const char *base_url);
void krait_ai_set_provider_model(int index, const char *model);
int krait_ai_effort_count(void);
const char *krait_ai_effort_name(int index);
int krait_ai_active_effort(void);
int krait_ai_set_effort(int index);
const char *krait_ai_config_hint(void);
void krait_ai_set_key(const char *api_key);
char *krait_ai_base64_file(const char *path);
char *krait_ai_build_body(const KraitAiMessage *messages, int count);
int krait_ai_extract_response_text(const char *response, char **out);
const char *krait_ai_last_usage(void);
char *krait_ai_stream_text(KraitAiRequest *request);
int krait_ai_configured(void);
KraitAiRequest *krait_ai_chat(const KraitAiMessage *messages, int count,
                              int timeout_s);
KraitAiStatus krait_ai_poll(KraitAiRequest *request);
const char *krait_ai_text(KraitAiRequest *request);
const char *krait_ai_error(KraitAiRequest *request);
const char *krait_ai_request_body(KraitAiRequest *request);
void krait_ai_free(KraitAiRequest *request);

/* ---- native_kanban.c: agentic board (files under ~/.kryon/krait/kanban) ---- */
int krait_kanban_rescan(void);
int krait_kanban_count(int col);
const char *krait_kanban_field(int col, int index, int field);
int krait_kanban_set_field(int col, int index, int field, const char *value);
int krait_kanban_spec_snapshot(const char *id, char digest[65]);
const char *krait_kanban_column_name(int col);
const char *krait_kanban_card_id(int col, int index);
const char *krait_kanban_card_title(int col, int index);
const char *krait_kanban_card_project(int col, int index);
const char *krait_kanban_card_body(int col, int index);
const char *krait_kanban_card_path(int col, int index);
const char *krait_kanban_card_status(int col, int index);
int krait_kanban_create(int col, const char *title);
int krait_kanban_set_title(int col, int index, const char *title);
int krait_kanban_set_body(int col, int index, const char *body);
int krait_kanban_set_project(int col, int index, const char *project);
int krait_kanban_move(int col, int index, int to_col);
int krait_kanban_delete(int col, int index);
int krait_kanban_new_project(int col, int index, const char *path);
int krait_kanban_ai_configured(void);
int krait_kanban_ai_run(int col, int index);
int krait_kanban_ai_poll(int col, int index);
int krait_kanban_proposal_count(int col, int index);
const char *krait_kanban_proposal_path(int col, int index, int file_index);
const char *krait_kanban_proposal_content(int col, int index, int file_index);
int krait_kanban_apply(int col, int index);
int krait_kanban_reject(int col, int index);
void krait_kanban_shutdown(void);

/* ---- native_scaffold.c ---- */
int krait_scaffold_project(const char *dir, char *status, int status_size);
int krait_mkdir_p(const char *dir);

/* ---- native_util.c: shared string/filesystem helpers ---- */
void krait_join(char *dst, size_t dst_size, const char *a, const char *b);
const char *krait_basename(const char *path);
char *krait_trim(char *s);
int krait_path_exists(const char *path);
int krait_path_has_suffix(const char *path, const char *suffix);
int krait_ignored_dir(const char *name);
void krait_ensure_parent_dir(const char *path);
int krait_mkdir_p(const char *dir);
int krait_wrap_lines(const char *text, int width, int font, int max_lines);
int krait_wrap_line(const char *text, int width, int font, int max_lines,
                    int row, char *dst, int dst_size);
int krait_read_file_alloc(const char *path, char **out, long *out_len);
int krait_write_text_file(const char *path, const char *text);
int krait_write_text_file_atomic(const char *path, const char *text);
int krait_file_is_text(const char *path);
int krait_tree_id(const char *path);
int krait_ident_start(int ch);
int krait_ident_char(int ch);
void krait_title_from_file(char *dst, size_t dst_size, const char *file);
void krait_kryon_tool_path(char *out, size_t out_size, const char *tool);
void krait_kryon_dir(char *out, size_t out_size);

/* ---- native_project.c: in-project content search ---- */
int krait_search_project(const char *root, const char *query,
                         SearchResult *results, int cap);

/* ---- native_compile.c: shared compile gate + bounded command runner ---- */
int krait_compile_gate_all(const char *project_dir,
                           const char *const *overlay_paths,
                           const char *const *overlay_bodies, int overlay_count,
                           char *first_error, size_t error_size,
                           char *all_errors, size_t all_size);
int krait_run_capture_cancel(const char *dir, const char *cmdline, int timeout_s,
    char *out, size_t out_size, int (*cancelled)(void *), void *userdata);
int krait_run_capture(const char *dir, const char *cmdline, int timeout_s,
                      char *out, size_t out_size);
int krait_gate_cc_runs(void);
int krait_gate_cache_hits(void);
int krait_gate_copies(void);

/* ---- native_krbhex.c: hex editor backend for binary (.krb) files ---- */
int krait_hex_open(const char *path);
int krait_hex_size(void);
int krait_hex_byte(int index);
int krait_hex_set_byte(int index, int value);
int krait_hex_dirty(void);
int krait_hex_changed_count(void);
const char *krait_hex_path(void);
int krait_hex_save(void);

/* ---- native_agent.c: agent-view conversation engine (ZCode-style loop) ---- */
void krait_agent_bind(const char *project_dir);
int krait_agent_count(void);
int krait_agent_kind(int index);
const char *krait_agent_text(int index);
const char *krait_agent_status_text(void);
int krait_agent_busy(void);
int krait_agent_files_changed(void);
int krait_agent_send(const char *text);
void krait_agent_stop(void);
void krait_agent_clear(void);
int krait_agent_poll(void);
void krait_agent_shutdown(void);
char *krait_agent_run_tools(const char *json);
const char *krait_agent_consume_console(void);
void krait_agent_clear_console(void);
int krait_agent_bridge_card(int col, int index);
int krait_agent_bind_task(const char *project_dir, const char *task);
const char *krait_agent_task(void);
const char *krait_agent_run_state(void);
int krait_agent_can_resume(void);
int krait_agent_resume(void);
int krait_agent_validate(void);
int krait_agent_validation_current(void);
int krait_agent_validation_for(const char *project, const char *task);
int krait_project_snapshot(const char *project, char digest[65]);
char *krait_agent_instructions(const char *project);
int krait_agent_written_count(void);
const char *krait_agent_written_path(int index);
int krait_agent_can_revert(void);
int krait_agent_revert(void);
const char *krait_agent_tool_name(int index);
const char *krait_agent_tool_arg(int index);
int krait_agent_tool_status(int index);
int krait_agent_tool_dur(int index);
int krait_agent_permission_pending(void);
int krait_agent_permission_count(void);
const char *krait_agent_permission_line(int index);
void krait_agent_permission_respond(int allow, int always);
int krait_agent_full_access_enabled(void);
void krait_agent_set_full_access(int enabled);
int krait_agent_retry(int index);
int krait_agent_session_count(void);
const char *krait_agent_session_name(int index);
const char *krait_agent_session_project(int index);
long krait_agent_session_mtime(int index);
int krait_agent_open_session(int index);

/* ---- native_md.c: markdown layout for the transcript + viewer ----
 * Style bits: 1 bold, 2 italic, 4 code, 8 link. Row kinds: 0 text,
 * 1..3 heading levels, 4 code, 5 quote, 6 bullet, 7 numbered, 8 hr. */
int krait_md_rows(const char *text, int width, int font);
int krait_md_row_info(const char *text, int width, int font, int row,
                      int *kind, int *runs, int *indent, int *bg);
int krait_md_run_info(const char *text, int width, int font, int row, int run,
                      char *dst, int dst_size, int *x, int *style);
int krait_md_indent_px(void);
const char *krait_md_file_text(const char *path);
void krait_md_view_set(const char *text, const char *title);
int krait_md_view_open_file(const char *path);
const char *krait_md_view_text(void);
const char *krait_md_view_title(void);

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
ButtonStyle krait_live_eval_button_style(const char *expr);
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
int krait_artifact_generate(const char *root, const char *rel_source, int kind,
                            char *out, int out_size, char *artifact_path,
                            int artifact_path_size, char *status,
                            int status_size);
int krait_artifact_save_binary(const char *generated_path,
                               const char *dest_path);
int krait_live_draw_canvas(const char *root, const char *rel_path, int w, int h,
                           char *status, int status_size);
int krait_live_capture_png(const char *root, const char *rel_path, int w, int h,
                           const char *png_path, char *status, int status_size);
int krait_game_node_args(const char *q, char args[8][256], char *type,
                         size_t type_size, char *label, size_t label_size,
                         int *x, int *y, int *w, int *h);
void krait_draw_game_node(const char *type, const char *label, int x, int y, int w, int h);

/* ---- native_engine.c: Game Engine mode (document editor around the
 * Kryon Game2D scene tree; ide/game.kry is a one-call shim) ---- */
void krait_engine_reset(const char *project_dir);
int krait_engine_add_node(int kind_index, const char *name);
int krait_engine_delete_selected(void);
/* behavior registry: builtins (player/spin/patrol) and plugin behaviors
 * share one public surface; fns run per-frame while playing */
typedef void (*KraitBehaviorFn)(Scene *scene, NodeId node, float dt,
                                const float *params, int param_count,
                                void *user);
int krait_engine_behavior_register(const char *id, const char *label,
                                   const char *const *param_names,
                                   const float *param_defaults,
                                   int param_count, KraitBehaviorFn fn,
                                   void *user);
int krait_engine_behavior_count(void);
const char *krait_engine_behavior_id(int index);
const char *krait_engine_behavior_label(int index);
int krait_engine_behavior_param_count(int index);
const char *krait_engine_behavior_param_name(int index, int param);
int krait_engine_set_behavior_id(int node_id, const char *behavior_id);
int krait_engine_set_behavior_param(int node_id, int param, float value);
int krait_engine_set_trigger(int id, int trigger);
int krait_engine_node_count(void);
const char *krait_engine_node_name(int id);
int krait_engine_node_pos(int id, float *x, float *y);
int krait_engine_node_tile(int id, int tx, int ty);
int krait_engine_timer_fired(int id);
int krait_engine_set_particles(int id, float rate, float lifetime,
                               float speed, float spread);
int krait_engine_particle_count(int id);
/* 3D nodes (Node3D / MeshInstance3D / Camera3D) */
int krait_engine_set_3d(int id, float z, float rot_y, float scale);
int krait_engine_set_mesh(int id, int mesh_kind);
int krait_engine_set_3d_target(int id, float tx, float ty, float tz);
int krait_engine_get_3d(int id, float *z, float *rot_y, float *scale);
int krait_engine_node_set_pos3(int id, float x, float y, float z);
int krait_engine_node_pos3(int id, float *x, float *y, float *z);
int krait_engine_get_mesh(int id);
int krait_engine_scene_is_3d(void);
/* timeline editing over an AnimationPlayer node */
int krait_engine_anim_add_key(int id, int track, float time, float value);
int krait_engine_anim_move_key(int id, int track, int key, float time);
int krait_engine_anim_delete_key(int id, int track, int key);
int krait_engine_anim_key_count(int id, int track);
int krait_engine_anim_key_get(int id, int track, int key, float *time,
                              float *value);
int krait_engine_timeline_scrub(int id, float t);
int krait_engine_node_set_script(int id, const char *text);
int krait_engine_score(void);
int krait_engine_won(void);
int krait_engine_playing(void);
int krait_engine_paused(void);
void krait_engine_play(void);
void krait_engine_pause(void);
void krait_engine_stop(void);
void krait_engine_advance(float dt);
int krait_engine_save(const char *path);
int krait_engine_load(const char *path);
int krait_engine_smoke_test(void);
void krait_engine_draw_view(Rectangle bounds, IdeState *st);
void krait_engine_shutdown(void);
/* standalone player mode (krait --play-game): own window, no editor UI */
int krait_engine_play_scene(const char *path_or_dir);
void krait_engine_draw_play(Rectangle bounds);
void krait_engine_view_size(int *w, int *h);
const char *krait_engine_scene_name(void);
int krait_engine_run_game(void);
int krait_engine_export_game(char *status, int status_size);

#endif /* KRAIT_NATIVE_INTERNAL_H */
