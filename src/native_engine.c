/*
 * Game Engine mode backend.
 *
 * Krait's game engine is a document editor around the Kryon Game2D scene
 * tree (scene_tree.h): the authored scene lives in an EngineNode document
 * that is serialized to <project>/game.scene, and Play instantiates it into
 * a runtime Scene with physics, behaviors, cameras and audio. Stop rebuilds
 * from the document, so editing never fights the simulation. The whole mode
 * UI (toolbar, scene tree, node palette, viewport, inspector) is drawn here
 * with Kryon widgets; ide/game.kry is a one-call shim.
 */

#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"
#include "native_engine_internal.h"
#include "native_level.h"
#include "native_script.h"

static EngineState g_engine;

#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static EngineNode *
engine_node_by_id(int id)
{
    int i;

    if(id <= 0)
        return NULL;
    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used && g_engine.nodes[i].id == id)
            return &g_engine.nodes[i];
    }
    return NULL;
}

/* A scene renders with the 3D pass when it has any 3D node. */
static int
engine_scene_is_3d(void)
{
    int i;

    for(i = 0; i < g_engine.node_count; i++) {
        int k = (int)g_engine.nodes[i].kind;

        if(g_engine.nodes[i].used &&
           (k == ENGINE_KIND_NODE3D || k == ENGINE_KIND_MESH3D ||
            k == ENGINE_KIND_CAMERA3D))
            return 1;
    }
    return 0;
}

static EngineNode *
engine_node_by_name(const char *name)
{
    int i;

    if(name == NULL || name[0] == '\0')
        return NULL;
    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used &&
           strcmp(g_engine.nodes[i].name, name) == 0)
            return &g_engine.nodes[i];
    }
    return NULL;
}

static int
engine_kind_index(NodeKind kind)
{
    int i;

    for(i = 0; i < ENGINE_KIND_COUNT; i++) {
        if(g_kinds[i].kind == kind)
            return i;
    }
    return -1;
}

static const char *
engine_kind_name(NodeKind kind)
{
    int i;

    if((int)kind == ENGINE_KIND_TIMER)
        return "Timer";
    if((int)kind == ENGINE_KIND_PARTICLES)
        return "Particles2D";
    if((int)kind == ENGINE_KIND_NODE3D)
        return "Node3D";
    if((int)kind == ENGINE_KIND_MESH3D)
        return "MeshInstance3D";
    if((int)kind == ENGINE_KIND_CAMERA3D)
        return "Camera3D";
    i = engine_kind_index(kind);
    return i >= 0 ? g_kinds[i].name : "Node2D";
}

static void
engine_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g_engine.status, sizeof(g_engine.status), fmt, ap);
    va_end(ap);
}

/* Resolved asset path for runtime props: absolute paths pass through,
 * relative paths resolve against the project root. The returned pointer
 * stays valid until the next call (rotating static buffers), which is safe
 * because the runtime scene is rebuilt wholesale whenever the document
 * changes, and the document outlives any single build. */
static const char *
engine_asset_abs(const char *asset)
{
    static char buf[4][KRAIT_PATH_MAX];
    static int rot;

    if(asset == NULL || asset[0] == '\0')
        return "";
    rot = (rot + 1) & 3;
    if(asset[0] == '/' || strstr(asset, "://") != NULL)
        snprintf(buf[rot], sizeof(buf[rot]), "%s", asset);
    else if(g_engine.project_root[0] != '\0')
        krait_join(buf[rot], sizeof(buf[rot]), g_engine.project_root, asset);
    else
        snprintf(buf[rot], sizeof(buf[rot]), "%s", asset);
    return buf[rot];
}

static Color
engine_node_color(const EngineNode *n)
{
    if(n->tint.a != 0)
        return n->tint;
    switch(n->kind) {
    case NODE_SPRITE2D:          return (Color){ 70, 130, 220, 255 };
    case NODE_ANIMATED_SPRITE2D: return (Color){ 60, 190, 180, 255 };
    case NODE_CAMERA2D:          return (Color){ 170, 110, 220, 255 };
    case NODE_BODY2D:            return (Color){ 110, 170, 120, 255 };
    case NODE_AREA2D:            return (Color){ 220, 190, 70, 255 };
    case NODE_TILEMAP:           return (Color){ 130, 170, 120, 255 };
    case NODE_TIMER:             return (Color){ 150, 170, 235, 255 };
    case NODE_AUDIO_SOURCE:      return (Color){ 220, 120, 170, 255 };
    default:
        if((int)n->kind == ENGINE_KIND_PARTICLES)
            return (Color){ 250, 220, 120, 255 };
        if((int)n->kind == ENGINE_KIND_MESH3D)
            return (Color){ 130, 170, 220, 255 };
        if((int)n->kind == ENGINE_KIND_NODE3D)
            return (Color){ 190, 190, 210, 255 };
        if((int)n->kind == ENGINE_KIND_CAMERA3D)
            return (Color){ 120, 200, 210, 255 };
        return (Color){ 240, 150, 60, 255 };
    }
}

/* ------------------------------------------------------------------ */
/* document                                                            */
/* ------------------------------------------------------------------ */

static void
engine_doc_clear(void)
{
    memset(g_engine.nodes, 0, sizeof(g_engine.nodes));
    g_engine.node_count = 0;
    g_engine.next_id = 1;
    g_engine.selected = 0;
    g_engine.dirty = 1;
}

static EngineNode *
engine_doc_add(NodeKind kind, const char *name, int parent_id)
{
    EngineNode *n;
    char base[48];
    int i, same = 0, slot = -1;

    if(g_engine.node_count >= ENGINE_NODES_MAX)
        return NULL;
    if(parent_id != 0 && engine_node_by_id(parent_id) == NULL)
        return NULL;
    for(i = 0; i < g_engine.node_count; i++) {
        if(!g_engine.nodes[i].used) {
            if(slot < 0)
                slot = i;
        } else if(g_engine.nodes[i].kind == kind) {
            same++;
        }
    }
    if(slot < 0)
        slot = g_engine.node_count;
    if(slot >= g_engine.node_count)
        g_engine.node_count = slot + 1;
    n = &g_engine.nodes[slot];
    memset(n, 0, sizeof(*n));
    n->used = 1;
    n->id = g_engine.next_id++;
    n->parent = parent_id;
    n->kind = kind;
    snprintf(base, sizeof(base), "%s", engine_kind_name(kind));
    if(same > 0)
        snprintf(n->name, sizeof(n->name), "%s%d", base, same + 1);
    else
        snprintf(n->name, sizeof(n->name), "%s", base);
    if(name != NULL && name[0] != '\0')
        snprintf(n->name, sizeof(n->name), "%s", name);
    n->x = g_engine.cam_x;
    n->y = g_engine.cam_y;
    n->sx = 1.0f;
    n->sy = 1.0f;
    n->w = 32.0f;
    n->h = 32.0f;
    n->gravity_scale = 1.0f;
    n->volume = 1.0f;
    n->pitch = 1.0f;
    n->cam_zoom = 1.0f;
    n->cam_active = 1;
    n->runtime = -1;
    n->shape_runtime = -1;
    switch(kind) {
    case NODE_SPRITE2D:
        n->w = 48.0f;
        n->h = 48.0f;
        break;
    case NODE_CAMERA2D:
        n->w = 0.0f;
        n->h = 0.0f;
        break;
    case NODE_BODY2D:
        n->w = 64.0f;
        n->h = 32.0f;
        break;
    case NODE_ANIMATED_SPRITE2D:
        n->frame_count = 4;
        n->frames_per_row = 4;
        n->frame_w = 16;
        n->frame_h = 16;
        n->fps = 8.0f;
        break;
    case NODE_TILEMAP: {
        int tx;

        n->map_w = 12;
        n->map_h = 6;
        n->tile_w = 16;
        n->tile_h = 16;
        n->tiles_per_row = 4;
        n->tile_px_w = 32;
        n->tile_px_h = 32;
        n->w = 384.0f;
        n->h = 192.0f;
        for(tx = 0; tx < n->map_w; tx++)
            n->tiles[(n->map_h - 1) * n->map_w + tx] = 1; /* ground row */
        n->tint = (Color){ 120, 160, 120, 255 };
        break;
    }
    default:
        if((int)kind == ENGINE_KIND_TIMER) {
            n->wait_time = 1.0f;
            n->autostart = 1;
        } else if((int)kind == ENGINE_KIND_NODE3D ||
                  (int)kind == ENGINE_KIND_MESH3D) {
            n->w = 32.0f;
            n->h = 32.0f;
            n->z = 0.0f;
            n->rot_y3 = 0.0f;
            n->scale3 = 1.0f;
            n->mesh_kind = ENGINE_MESH_CUBE;
            if((int)kind == ENGINE_KIND_MESH3D)
                n->tint = (Color){ 130, 170, 220, 255 };
        } else if((int)kind == ENGINE_KIND_CAMERA3D) {
            n->w = 0.0f;
            n->h = 0.0f;
            n->x = 4.0f;
            n->y = 3.0f;
            n->z = 4.0f;
            n->tx = 0.0f;
            n->ty = 0.0f;
            n->tz = 0.0f;
        } else if((int)kind == ENGINE_KIND_PARTICLES) {
            n->w = 0.0f;
            n->h = 0.0f;
            n->p_rate = 20.0f;
            n->p_lifetime = 1.0f;
            n->p_speed = 80.0f;
            n->p_spread = 360.0f;
            n->p_col_start = (Color){ 255, 210, 90, 255 };
            n->p_col_end = (Color){ 255, 90, 40, 0 };
            n->p_emitting = 1;
        } else if(kind == NODE_TIMER) {
            /* AnimationPlayer: animate the selection (or Player) on X */
            const char *target = "Player";
            EngineNode *sel = engine_node_by_id(g_engine.selected);

            if(sel != NULL && sel->kind != NODE_TIMER)
                target = sel->name;
            n->w = 0.0f;
            n->h = 0.0f;
            n->anim_autoplay = 1;
            n->anim_track_count = 1;
            snprintf(n->anim_tracks[0].target,
                     sizeof(n->anim_tracks[0].target), "%s", target);
            n->anim_tracks[0].property = ENGINE_ANIM_POS_X;
            n->anim_tracks[0].key_count = 2;
            n->anim_tracks[0].keys[0][0] = 0.0f;
            n->anim_tracks[0].keys[0][1] = sel != NULL ? sel->x : 0.0f;
            n->anim_tracks[0].keys[1][0] = 1.0f;
            n->anim_tracks[0].keys[1][1] =
                (sel != NULL ? sel->x : 0.0f) + 200.0f;
        }
        break;
    }
    g_engine.dirty = 1;
    return n;
}

static void
engine_doc_remove(EngineNode *n)
{
    int i;

    /* remove descendants first */
    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used && g_engine.nodes[i].parent == n->id)
            engine_doc_remove(&g_engine.nodes[i]);
    }
    if(g_engine.selected == n->id)
        g_engine.selected = 0;
    n->used = 0;
    g_engine.dirty = 1;
}

static void
engine_doc_compact(void)
{
    int i, w = 0;

    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used) {
            if(w != i)
                g_engine.nodes[w] = g_engine.nodes[i];
            w++;
        }
    }
    for(i = w; i < g_engine.node_count; i++)
        memset(&g_engine.nodes[i], 0, sizeof(g_engine.nodes[i]));
    g_engine.node_count = w;
}

/* The default scene a fresh engine view starts from: a small physics
 * platformer that shows gravity, static/dynamic bodies and the player
 * behavior without needing any assets. */
static void
engine_doc_starter(void)
{
    EngineNode *n;
    int player_id = 0;

    engine_doc_clear();
    snprintf(g_engine.name, sizeof(g_engine.name), "Main");
    g_engine.gravity_x = 0.0f;
    g_engine.gravity_y = 980.0f;
    g_engine.view_w = 640;
    g_engine.view_h = 360;

    n = engine_doc_add(NODE_BODY2D, "Ground", 0);
    n->x = 320; n->y = 332; n->w = 640; n->h = 56; n->body_type = 0;

    n = engine_doc_add(NODE_BODY2D, "Platform", 0);
    n->x = 180; n->y = 232; n->w = 160; n->h = 16; n->body_type = 0;

    n = engine_doc_add(NODE_BODY2D, "Crate", 0);
    n->x = 420; n->y = 60; n->w = 36; n->h = 36; n->body_type = 2;

    n = engine_doc_add(NODE_BODY2D, "Crate2", 0);
    n->x = 436; n->y = 8; n->w = 28; n->h = 28; n->body_type = 2;

    n = engine_doc_add(NODE_BODY2D, "Player", 0);
    n->x = 80; n->y = 280; n->w = 28; n->h = 44;
    n->body_type = 2;   /* dynamic: physics-driven player */
    n->fixed_rotation = 1;
    snprintf(n->behavior_id, sizeof(n->behavior_id), "player");
    n->behavior_params[0] = 240.0f;   /* speed */
    n->behavior_params[1] = 380.0f;   /* jump */
    n->tint = (Color){ 240, 150, 60, 255 };
    player_id = n->id;

    n = engine_doc_add(NODE_TILEMAP, "Terrain", 0);
    n->x = 96; n->y = 136;
    n->tiles[(n->map_h - 3) * n->map_w + 2] = 2;
    n->tiles[(n->map_h - 3) * n->map_w + 3] = 2;

    n = engine_doc_add(NODE_AREA2D, "Coin", 0);
    n->x = 420; n->y = 220; n->w = 48; n->h = 48;
    n->trigger = ENGINE_TRIGGER_COLLECT;
    n->tint = (Color){ 240, 200, 60, 255 };

    n = engine_doc_add((NodeKind)ENGINE_KIND_PARTICLES, "Sparkles", 0);
    n->x = 420; n->y = 220;

    n = engine_doc_add(NODE_CAMERA2D, "Camera", 0);
    n->x = 320; n->y = 180; n->cam_zoom = 1.0f; n->cam_active = 1;

    g_engine.selected = player_id;   /* inspector opens on the player */
}

/* ------------------------------------------------------------------ */
/* scene file format                                                   */
/* ------------------------------------------------------------------ */
/*
 * # krait game scene 1
 * name "Main"
 * gravity 0 980
 * view 640 360
 * node <id> <parent> <Kind> "<name>"
 * x 100
 * ...
 */

static void
engine_write_node(FILE *f, const EngineNode *n)
{
    fprintf(f, "node %d %d %s \"%s\"\n", n->id, n->parent,
            engine_kind_name(n->kind), n->name);
    fprintf(f, "x %g\ny %g\nrot %g\nscale %g %g\n",
            n->x, n->y, n->rot, n->sx, n->sy);
    if(n->behavior_id[0] != '\0') {
        const KraitBehaviorDef *bdef = engine_behavior_by_id(n->behavior_id);
        int pi;

        fprintf(f, "behavior %s\n", n->behavior_id);
        if(bdef != NULL) {
            for(pi = 0; pi < bdef->param_count; pi++)
                fprintf(f, "bparam %s %g\n", bdef->param_names[pi],
                        n->behavior_params[pi]);
        }
    }
    if(n->asset[0] != '\0')
        fprintf(f, "asset %s\n", n->asset);
    if(n->w != 0.0f || n->h != 0.0f)
        fprintf(f, "size %g %g\n", n->w, n->h);
    if(n->tint.a != 0)
        fprintf(f, "color %d %d %d %d\n", n->tint.r, n->tint.g, n->tint.b,
                n->tint.a);
    if(n->kind == NODE_BODY2D) {
        static const char *const body_names[3] = { "static", "kinematic", "dynamic" };
        fprintf(f, "body %s\nfixed_rot %d\ngrav %g\nshape %s\n",
                body_names[n->body_type >= 0 && n->body_type <= 2 ? n->body_type : 0],
                n->fixed_rotation, n->gravity_scale,
                n->shape_circle ? "circle" : "box");
    }
    if(n->kind == NODE_AREA2D)
        fprintf(f, "shape %s\n", n->shape_circle ? "circle" : "box");
    if(n->kind == NODE_CAMERA2D)
        fprintf(f, "camera %g %d\n", n->cam_zoom, n->cam_active);
    if(n->kind == NODE_ANIMATED_SPRITE2D)
        fprintf(f, "frames %d %d %d %d %g\n",
                n->frame_count, n->frames_per_row, n->frame_w, n->frame_h, n->fps);
    if(n->kind == NODE_AUDIO_SOURCE)
        fprintf(f, "audio %s %g %g %d\n",
                n->audio_kind ? "music" : "sound", n->volume, n->pitch, n->loop);
    if((int)n->kind == ENGINE_KIND_NODE3D || (int)n->kind == ENGINE_KIND_MESH3D ||
       (int)n->kind == ENGINE_KIND_CAMERA3D) {
        fprintf(f, "z %g\nroty3 %g\nscale3 %g\n", n->z, n->rot_y3,
                n->scale3);
        if((int)n->kind == ENGINE_KIND_MESH3D) {
            fprintf(f, "mesh %d\n", n->mesh_kind);
            if(n->tint.a != 0)
                fprintf(f, "color %d %d %d %d\n", n->tint.r, n->tint.g,
                        n->tint.b, n->tint.a);
        }
        if((int)n->kind == ENGINE_KIND_CAMERA3D)
            fprintf(f, "target3 %g %g %g\n", n->tx, n->ty, n->tz);
    }
    if((int)n->kind == ENGINE_KIND_PARTICLES) {
        fprintf(f, "particles %g %g %g %g\n", n->p_rate, n->p_lifetime,
                n->p_speed, n->p_spread);
        fprintf(f, "pcol %d %d %d %d %d %d %d %d\n",
                n->p_col_start.r, n->p_col_start.g, n->p_col_start.b,
                n->p_col_start.a, n->p_col_end.r, n->p_col_end.g,
                n->p_col_end.b, n->p_col_end.a);
        if(!n->p_emitting)
            fprintf(f, "pemit 0\n");
    }
    if(n->kind == NODE_TILEMAP) {
        int count = n->map_w * n->map_h;
        int any = 0;
        int t;

        for(t = 0; t < count && t < ENGINE_TILES_MAX; t++) {
            if(n->tiles[t] != 0) {
                any = 1;
                break;
            }
        }
        fprintf(f, "tmap %d %d %d %d %d %d %d\n",
                n->map_w, n->map_h, n->tile_w, n->tile_h, n->tiles_per_row,
                n->tile_px_w, n->tile_px_h);
        if(any) {
            fprintf(f, "tiles");
            for(t = 0; t < count && t < ENGINE_TILES_MAX; t++)
                fprintf(f, " %d", n->tiles[t]);
            fprintf(f, "\n");
        }
    }
    if((int)n->kind == ENGINE_KIND_TIMER)
        fprintf(f, "timer %g %d %d\n", n->wait_time, n->autostart, n->loop);
    if(n->kind == NODE_TIMER) {
        int t, k;

        fprintf(f, "anim %d %d %d\n", n->anim_loop, n->anim_autoplay,
                n->anim_track_count);
        for(t = 0; t < n->anim_track_count && t < ENGINE_ANIM_TRACKS; t++) {
            fprintf(f, "atrack \"%s\" %s %d\n", n->anim_tracks[t].target,
                    g_anim_prop_ids[n->anim_tracks[t].property],
                    n->anim_tracks[t].key_count);
            for(k = 0; k < n->anim_tracks[t].key_count &&
                       k < ENGINE_ANIM_KEYS; k++)
                fprintf(f, "akey %g %g\n", n->anim_tracks[t].keys[k][0],
                        n->anim_tracks[t].keys[k][1]);
        }
    }
    if(n->trigger != ENGINE_TRIGGER_NONE)
        fprintf(f, "trigger %s\n", g_trigger_ids[n->trigger]);
    if(n->script[0] != '\0') {
        const char *sp = n->script;

        while(*sp != '\0') {
            char line[256];
            size_t len = 0;

            while(*sp != '\0' && *sp != '\n' && len < sizeof(line) - 1)
                line[len++] = *sp++;
            line[len] = '\0';
            while(*sp == '\n' || *sp == '\r')
                sp++;
            fprintf(f, "script %s\n", line);
        }
    }
    if((int)n->kind == ENGINE_KIND_MESH3D && n->model_path[0] != '\0')
        fprintf(f, "model %s\n", n->model_path);
    fprintf(f, "\n");
}

int
krait_engine_save(const char *path)
{
    FILE *f;
    int i;

    if(path == NULL || path[0] == '\0')
        return 0;
    krait_ensure_parent_dir(path);
    f = fopen(path, "w");
    if(f == NULL)
        return 0;
    fprintf(f, "# krait game scene 1\n");
    fprintf(f, "name \"%s\"\n", g_engine.name);
    fprintf(f, "gravity %g %g\n", g_engine.gravity_x, g_engine.gravity_y);
    fprintf(f, "view %d %d\n", g_engine.view_w, g_engine.view_h);
    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used)
            engine_write_node(f, &g_engine.nodes[i]);
    }
    fclose(f);
    g_engine.dirty = 0;
    return 1;
}

int
krait_engine_load(const char *path)
{
    FILE *f;
    char line[8192];
    EngineNode *cur = NULL;

    if(path == NULL || path[0] == '\0')
        return 0;
    f = fopen(path, "r");
    if(f == NULL)
        return 0;
    engine_doc_clear();
    snprintf(g_engine.name, sizeof(g_engine.name), "Main");
    g_engine.gravity_x = 0.0f;
    g_engine.gravity_y = 980.0f;
    g_engine.view_w = 640;
    g_engine.view_h = 360;
    while(fgets(line, sizeof(line), f) != NULL) {
        char *p = krait_trim(line);
        char word[64];
        char nm[ENGINE_NAME_MAX];
        int id, parent, i, ival[8];
        float a, b, c;

        if(p[0] == '#' || p[0] == '\0')
            continue;
        if(sscanf(p, "name \"%63[^\"]\"", word) == 1 ||
           sscanf(p, "name %63s", word) == 1) {
            snprintf(g_engine.name, sizeof(g_engine.name), "%s", word);
            continue;
        }
        if(sscanf(p, "gravity %f %f", &a, &b) == 2) {
            g_engine.gravity_x = a;
            g_engine.gravity_y = b;
            continue;
        }
        if(sscanf(p, "view %d %d", &id, &parent) == 2) {
            if(id > 0 && parent > 0) {
                g_engine.view_w = id;
                g_engine.view_h = parent;
            }
            continue;
        }
        if(sscanf(p, "node %d %d %63s \"%63[^\"]\"", &id, &parent, word, nm) == 4) {
            NodeKind kind = NODE_NODE2D;

            for(i = 0; i < ENGINE_KIND_COUNT; i++) {
                if(strcmp(g_kinds[i].name, word) == 0)
                    kind = g_kinds[i].kind;
            }
            if(parent != 0 && engine_node_by_id(parent) == NULL)
                parent = 0;
            cur = engine_doc_add(kind, nm, parent);
            if(cur != NULL) {
                cur->id = id;   /* keep authored ids stable */
                cur->x = 0.0f;
                cur->y = 0.0f;
            }
            continue;
        }
        if(cur == NULL)
            continue;
        if(sscanf(p, "x %f", &a) == 1) { cur->x = a; continue; }
        if(sscanf(p, "y %f", &a) == 1) { cur->y = a; continue; }
        if(sscanf(p, "z %f", &a) == 1) { cur->z = a; continue; }
        if(sscanf(p, "roty3 %f", &a) == 1) { cur->rot_y3 = a; continue; }
        if(sscanf(p, "scale3 %f", &a) == 1) {
            cur->scale3 = a > 0.01f ? a : 1.0f;
            continue;
        }
        if(sscanf(p, "mesh %d", &id) == 1) {
            cur->mesh_kind = id >= 0 && id < ENGINE_MESH_COUNT ? id : 0;
            continue;
        }
        if(strncmp(p, "script ", 7) == 0) {
            const char *sp = p + 7;
            size_t used = strlen(cur->script);

            if(used > 0 && used + 1 < sizeof(cur->script)) {
                cur->script[used++] = '\n';
                cur->script[used] = '\0';
            }
            snprintf(cur->script + used, sizeof(cur->script) - used, "%s",
                     sp);
            cur->script_var_count = 0;
            continue;
        }
        if(sscanf(p, "model %255s", cur->model_path) == 1) {
            if((int)cur->kind == ENGINE_KIND_MESH3D)
                cur->mesh_kind = ENGINE_MESH_MODEL;
            continue;
        }
        if(sscanf(p, "target3 %f %f %f", &a, &b, &c) == 3) {
            cur->tx = a;
            cur->ty = b;
            cur->tz = c;
            continue;
        }
        if(sscanf(p, "rot %f", &a) == 1) { cur->rot = a; continue; }
        if(sscanf(p, "scale %f %f", &a, &b) == 2) {
            cur->sx = a;
            cur->sy = b;
            continue;
        }
        if(sscanf(p, "size %f %f", &a, &b) == 2) {
            cur->w = a;
            cur->h = b;
            continue;
        }
        if(sscanf(p, "color %d %d %d %d", &ival[0], &ival[1], &ival[2],
                  &ival[3]) == 4) {
            cur->tint.r = (unsigned char)ival[0];
            cur->tint.g = (unsigned char)ival[1];
            cur->tint.b = (unsigned char)ival[2];
            cur->tint.a = (unsigned char)ival[3];
            continue;
        }
        if(sscanf(p, "asset %255s", cur->asset) == 1)
            continue;
        if(sscanf(p, "behavior %63s", word) == 1) {
            const KraitBehaviorDef *bdef = engine_behavior_by_id(word);
            int pi;

            if(bdef == NULL && strcmp(word, "none") != 0)
                bdef = engine_behavior_by_id("none");
            if(bdef != NULL) {
                snprintf(cur->behavior_id, sizeof(cur->behavior_id), "%s",
                         bdef->id);
                for(pi = 0; pi < bdef->param_count; pi++)
                    cur->behavior_params[pi] = bdef->param_defaults[pi];
            } else {
                /* unknown id (e.g. a plugin not loaded): keep it verbatim
                 * so a round trip does not lose the authored value */
                snprintf(cur->behavior_id, sizeof(cur->behavior_id), "%s",
                         word);
            }
            continue;
        }
        if(sscanf(p, "bparam %63s %f", word, &a) == 2) {
            const KraitBehaviorDef *bdef =
                engine_behavior_by_id(cur->behavior_id);
            int pi;

            if(bdef != NULL) {
                int matched = -1;

                for(pi = 0; pi < bdef->param_count; pi++) {
                    if(strcmp(bdef->param_names[pi], word) == 0)
                        matched = pi;
                }
                if(matched < 0) {
                    /* name unknown: fill the first unset param slot */
                    for(pi = 0; pi < bdef->param_count; pi++) {
                        if(cur->behavior_params[pi] ==
                           bdef->param_defaults[pi]) {
                            matched = pi;
                            break;
                        }
                    }
                }
                if(matched >= 0)
                    cur->behavior_params[matched] = a;
            }
            continue;
        }
        if(sscanf(p, "body %63s", word) == 1) {
            if(strcmp(word, "dynamic") == 0)
                cur->body_type = 2;
            else if(strcmp(word, "kinematic") == 0)
                cur->body_type = 1;
            else
                cur->body_type = 0;
            continue;
        }
        if(sscanf(p, "fixed_rot %d", &id) == 1) { cur->fixed_rotation = id; continue; }
        if(sscanf(p, "grav %f", &a) == 1) { cur->gravity_scale = a; continue; }
        if(sscanf(p, "shape %63s", word) == 1) {
            cur->shape_circle = strcmp(word, "circle") == 0;
            continue;
        }
        if(sscanf(p, "camera %f %d", &a, &id) == 2) {
            cur->cam_zoom = a;
            cur->cam_active = id;
            continue;
        }
        if(sscanf(p, "frames %d %d %d %d %f", &cur->frame_count,
                  &cur->frames_per_row, &cur->frame_w, &cur->frame_h, &a) == 5) {
            cur->fps = a;
            continue;
        }
        if(sscanf(p, "audio %63s %f %f %d", word, &a, &b, &id) == 4) {
            cur->audio_kind = strcmp(word, "music") == 0 ? 1 : 0;
            cur->volume = a;
            cur->pitch = b;
            cur->loop = id;
            continue;
        }
        if(sscanf(p, "timer %f %d %d", &a, &id, &parent) == 3) {
            cur->wait_time = a;
            cur->autostart = id;
            cur->loop = parent;
            continue;
        }
        if(sscanf(p, "particles %f %f %f %f", &a, &b, &ival[0], &ival[1]) == 4) {
            cur->p_rate = a;
            cur->p_lifetime = b;
            cur->p_speed = (float)ival[0];
            cur->p_spread = (float)ival[1];
            continue;
        }
        if(sscanf(p, "pcol %d %d %d %d %d %d %d %d", &ival[0], &ival[1],
                  &ival[2], &ival[3], &ival[4], &ival[5], &ival[6],
                  &ival[7]) == 8) {
            cur->p_col_start = (Color){ (unsigned char)ival[0],
                                        (unsigned char)ival[1],
                                        (unsigned char)ival[2],
                                        (unsigned char)ival[3] };
            cur->p_col_end = (Color){ (unsigned char)ival[4],
                                      (unsigned char)ival[5],
                                      (unsigned char)ival[6],
                                      (unsigned char)ival[7] };
            continue;
        }
        if(sscanf(p, "pemit %d", &id) == 1) {
            cur->p_emitting = id;
            continue;
        }
        if(sscanf(p, "anim %d %d %d", &id, &parent, &i) == 3) {
            cur->anim_loop = id;
            cur->anim_autoplay = parent;
            cur->anim_track_count = i >= 0 && i <= ENGINE_ANIM_TRACKS ? i : 0;
            continue;
        }
        if(sscanf(p, "atrack \"%63[^\"]\" %63s %d", nm, word, &id) == 3) {
            /* fill the first declared-but-empty track slot, so multi-track
             * scenes load each atrack into its own row */
            int ti = 0;

            (void)id;   /* declared key count; akey lines append in order */
            while(ti < cur->anim_track_count && ti < ENGINE_ANIM_TRACKS &&
                  cur->anim_tracks[ti].target[0] != '\0')
                ti++;
            if(ti >= cur->anim_track_count)
                ti = cur->anim_track_count > 0 ? cur->anim_track_count - 1 : 0;
            if(ti >= 0 && ti < ENGINE_ANIM_TRACKS) {
                snprintf(cur->anim_tracks[ti].target,
                         sizeof(cur->anim_tracks[ti].target), "%s", nm);
                for(i = 0; i < ENGINE_ANIM_PROPERTY_COUNT; i++) {
                    if(strcmp(g_anim_prop_ids[i], word) == 0)
                        cur->anim_tracks[ti].property = i;
                }
                cur->anim_tracks[ti].key_count = 0;
            }
            continue;
        }
        if(sscanf(p, "akey %f %f", &a, &b) == 2) {
            /* keys belong to the last declared-but-emptiest track; after the
             * atrack fix that is the slot akey lines immediately follow */
            int ti = cur->anim_track_count > 0 ? cur->anim_track_count - 1 : 0;
            int probe;

            for(probe = 0; probe < cur->anim_track_count &&
                           probe < ENGINE_ANIM_TRACKS; probe++) {
                if(cur->anim_tracks[probe].target[0] != '\0' &&
                   cur->anim_tracks[probe].key_count < ENGINE_ANIM_KEYS &&
                   (probe + 1 >= cur->anim_track_count ||
                    cur->anim_tracks[probe + 1].target[0] == '\0')) {
                    ti = probe;
                    break;
                }
            }

            if(ti >= 0 && ti < ENGINE_ANIM_TRACKS &&
               cur->anim_tracks[ti].key_count < ENGINE_ANIM_KEYS) {
                int ki = cur->anim_tracks[ti].key_count;

                cur->anim_tracks[ti].keys[ki][0] = a;
                cur->anim_tracks[ti].keys[ki][1] = b;
                cur->anim_tracks[ti].key_count = ki + 1;
            }
            continue;
        }
        if(sscanf(p, "trigger %63s", word) == 1) {
            for(i = 0; i < ENGINE_TRIGGER_COUNT; i++) {
                if(strcmp(g_trigger_ids[i], word) == 0)
                    cur->trigger = i;
            }
            continue;
        }
        if(sscanf(p, "tmap %d %d %d %d %d %d %d", &id, &parent, &i, &ival[0],
                  &ival[1], &ival[2], &ival[3]) == 7) {
            int count;

            cur->map_w = id > 0 && id <= 32 ? id : 12;
            cur->map_h = parent > 0 && parent <= 32 ? parent : 6;
            cur->tile_w = i > 0 ? i : 16;
            cur->tile_h = ival[0] > 0 ? ival[0] : 16;
            cur->tiles_per_row = ival[1] > 0 ? ival[1] : 4;
            cur->tile_px_w = ival[2] > 0 ? ival[2] : 32;
            cur->tile_px_h = ival[3] > 0 ? ival[3] : 32;
            cur->w = (float)(cur->map_w * cur->tile_px_w);
            cur->h = (float)(cur->map_h * cur->tile_px_h);
            count = cur->map_w * cur->map_h;
            if(count > ENGINE_TILES_MAX)
                count = ENGINE_TILES_MAX;
            memset(cur->tiles, 0, sizeof(cur->tiles));
            continue;
        }
        if(strncmp(p, "tiles", 5) == 0 && isspace((unsigned char)p[5])) {
            const char *tp = p + 5;
            int count = cur->map_w * cur->map_h;
            int ti = 0;

            if(count > ENGINE_TILES_MAX)
                count = ENGINE_TILES_MAX;
            while(ti < count) {
                char *end;
                long v = strtol(tp, &end, 10);

                if(end == tp)
                    break;
                cur->tiles[ti++] = (int)v;
                tp = end;
            }
            continue;
        }
    }
    fclose(f);
    /* recompute the next free id from the authored ids */
    {
        int max_id = 0;
        int i2;

        for(i2 = 0; i2 < g_engine.node_count; i2++) {
            if(g_engine.nodes[i2].used && g_engine.nodes[i2].id > max_id)
                max_id = g_engine.nodes[i2].id;
        }
        g_engine.next_id = max_id + 1;
    }
    g_engine.dirty = 1;   /* runtime needs a rebuild after a load */
    return 1;
}

/* ------------------------------------------------------------------ */
/* runtime build + lifecycle                                           */
/* ------------------------------------------------------------------ */

/* ---- kscript engine hooks (called from native_script.c) ---- */

Scene *
krait_script_scene_ptr(void)
{
    return &g_engine.scene;
}

float
krait_script_time(void)
{
    return (float)g_engine.sim_time;
}

float
krait_script_score(void)
{
    return (float)g_engine.score;
}

void
krait_script_add_score(float v)
{
    g_engine.score += v - krait_script_score();   /* set score = v */
}

void
krait_script_win(void)
{
    if(!g_engine.won) {
        g_engine.won = 1;
        g_engine.paused = 1;
        engine_status("Level complete! (script)");
    }
}

void
krait_script_collect(const char *name)
{
    EngineNode *dn = engine_node_by_name(name);

    if(dn == NULL || dn->collected)
        return;
    dn->collected = 1;
    g_engine.score++;
    if(dn->carrier_runtime >= 0)
        NodeRemove(&g_engine.scene, dn->carrier_runtime);
    else
        NodeRemove(&g_engine.scene, dn->runtime);
    engine_status("Collected %s (script)", dn->name);
}

float
krait_script_node_x(const char *name)
{
    EngineNode *dn = engine_node_by_name(name);

    return dn != NULL ? dn->r_x : 0.0f;
}

float
krait_script_node_y(const char *name)
{
    EngineNode *dn = engine_node_by_name(name);

    return dn != NULL ? dn->r_y : 0.0f;
}

int
krait_script_key_down(const char *name)
{
    if(strcmp(name, "up") == 0)
        return IsKeyDown(KEY_UP) || IsKeyDown(KEY_W);
    if(strcmp(name, "down") == 0)
        return IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S);
    if(strcmp(name, "left") == 0)
        return IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A);
    if(strcmp(name, "right") == 0)
        return IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D);
    if(strcmp(name, "space") == 0)
        return IsKeyDown(KEY_SPACE);
    return 0;
}

int
krait_script_key_pressed(const char *name)
{
    if(strcmp(name, "up") == 0)
        return IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W);
    if(strcmp(name, "down") == 0)
        return IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S);
    if(strcmp(name, "left") == 0)
        return IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A);
    if(strcmp(name, "right") == 0)
        return IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D);
    if(strcmp(name, "space") == 0)
        return IsKeyPressed(KEY_SPACE);
    return 0;
}

unsigned
krait_script_rand(EngineNode *node)
{
    if(node == NULL)
        return 0;
    node->script_rng ^= node->script_rng << 13;
    node->script_rng ^= node->script_rng >> 17;
    node->script_rng ^= node->script_rng << 5;
    return node->script_rng;
}

/* public: attach a script to a node (tests, games) */
int
krait_engine_node_set_script(int id, const char *text)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || text == NULL)
        return 0;
    snprintf(n->script, sizeof(n->script), "%s", text);
    n->script_var_count = 0;
    return 1;
}

/* ---- behavior registry ---- */

static KraitBehaviorDef *
engine_behavior_by_id(const char *id)
{
    int i;

    if(id == NULL || id[0] == '\0')
        return NULL;
    for(i = 0; i < g_behavior_count; i++) {
        if(strcmp(g_behaviors[i].id, id) == 0)
            return &g_behaviors[i];
    }
    return NULL;
}

int
krait_engine_behavior_register(const char *id, const char *label,
                               const char *const *param_names,
                               const float *param_defaults, int param_count,
                               KraitBehaviorFn fn, void *user)
{
    KraitBehaviorDef *def;
    int i;

    if(id == NULL || label == NULL || fn == NULL)
        return -1;
    if(param_count < 0 || param_count > ENGINE_BEHAVIOR_PARAMS)
        return -1;
    if(engine_behavior_by_id(id) != NULL)
        return -1;   /* duplicate id */
    if(g_behavior_count >= ENGINE_BEHAVIOR_MAX)
        return -1;
    def = &g_behaviors[g_behavior_count++];
    memset(def, 0, sizeof(*def));
    snprintf(def->id, sizeof(def->id), "%s", id);
    snprintf(def->label, sizeof(def->label), "%s", label);
    def->param_count = param_count;
    def->fn = fn;
    def->user = user;
    for(i = 0; i < param_count; i++) {
        snprintf(def->param_names[i], sizeof(def->param_names[i]), "%s",
                 param_names != NULL && param_names[i] != NULL
                     ? param_names[i] : "param");
        def->param_defaults[i] = param_defaults != NULL ? param_defaults[i]
                                                        : 0.0f;
    }
    return g_behavior_count - 1;
}

int
krait_engine_behavior_count(void)
{
    return g_behavior_count;
}

const char *
krait_engine_behavior_id(int index)
{
    if(index < 0 || index >= g_behavior_count)
        return NULL;
    return g_behaviors[index].id;
}

const char *
krait_engine_behavior_label(int index)
{
    if(index < 0 || index >= g_behavior_count)
        return NULL;
    return g_behaviors[index].label;
}

int
krait_engine_behavior_param_count(int index)
{
    if(index < 0 || index >= g_behavior_count)
        return 0;
    return g_behaviors[index].param_count;
}

const char *
krait_engine_behavior_param_name(int index, int param)
{
    if(index < 0 || index >= g_behavior_count ||
       param < 0 || param >= g_behaviors[index].param_count)
        return NULL;
    return g_behaviors[index].param_names[param];
}

int
krait_engine_set_behavior_id(int node_id, const char *behavior_id)
{
    EngineNode *n = engine_node_by_id(node_id);
    const KraitBehaviorDef *def;
    int i;

    if(n == NULL)
        return 0;
    def = behavior_id != NULL ? engine_behavior_by_id(behavior_id) : NULL;
    if(behavior_id != NULL && behavior_id[0] != '\0' && def == NULL)
        return 0;   /* unknown behavior */
    if(def == NULL)
        n->behavior_id[0] = '\0';
    else
        snprintf(n->behavior_id, sizeof(n->behavior_id), "%s", def->id);
    /* params reset to the def defaults when the behavior changes */
    for(i = 0; i < ENGINE_BEHAVIOR_PARAMS; i++)
        n->behavior_params[i] = def != NULL && i < def->param_count
                                    ? def->param_defaults[i] : 0.0f;
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_set_behavior_param(int node_id, int param, float value)
{
    EngineNode *n = engine_node_by_id(node_id);

    if(n == NULL || param < 0 || param >= ENGINE_BEHAVIOR_PARAMS)
        return 0;
    n->behavior_params[param] = value;
    return 1;
}

/* ---- built-in behaviors (registered through the public API) ---- */

/* Player: run left/right and jump. Physics bodies steer with velocity;
 * plain nodes move kinematically. */
static void
engine_behavior_player(Scene *scene, NodeId node, float dt,
                       const float *params, int param_count, void *user)
{
    Node *n = NodeGet(scene, node);
    float speed = param_count > 0 ? params[0] : 240.0f;
    float jump = param_count > 1 ? params[1] : 380.0f;
    int dx = 0;
    int jump_pressed;

    (void)dt;
    (void)user;
    if(n == NULL)
        return;
    if(g_engine.name_focused || g_engine.asset_focused)
        return;
    if(IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
        dx -= 1;
    if(IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
        dx += 1;
    jump_pressed = IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W) ||
                   IsKeyPressed(KEY_SPACE);
    if(n->kind == NODE_BODY2D) {
        float vx = 0.0f, vy = 0.0f;

        KryBody2DGetVelocity(scene, node, &vx, &vy);
        vx = (float)dx * speed;
        if(jump_pressed)
            vy = -jump;
        KryBody2DSetVelocity(scene, node, vx, vy);
    } else {
        int dy = 0;
        float len = 1.0f;

        if(IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            dy -= 1;
        if(IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            dy += 1;
        if(dx != 0 && dy != 0)
            len = 0.7071f;
        NodeSetPosition(scene, node,
                        n->local.position.x + (float)dx * speed * len * dt,
                        n->local.position.y + (float)dy * speed * len * dt);
    }
}

/* Spin: constant rotation, radians per second. */
static void
engine_behavior_spin(Scene *scene, NodeId node, float dt,
                     const float *params, int param_count, void *user)
{
    Node *n = NodeGet(scene, node);
    float rate = param_count > 0 ? params[0] : 1.6f;

    (void)user;
    if(n == NULL)
        return;
    NodeSetRotation(scene, node, n->local.rotation + rate * dt);
}

/* Patrol: oscillate on X around the position captured at Play. */
static void
engine_behavior_patrol(Scene *scene, NodeId node, float dt,
                       const float *params, int param_count, void *user)
{
    Node *n = NodeGet(scene, node);
    EngineNode *dn = NULL;
    float range = param_count > 0 ? params[0] : 64.0f;
    float speed = param_count > 1 ? params[1] : 1.5f;
    int i;

    (void)dt;
    (void)user;
    if(n == NULL)
        return;
    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used && g_engine.nodes[i].runtime == node) {
            dn = &g_engine.nodes[i];
            break;
        }
    }
    if(dn == NULL)
        return;
    NodeSetPosition(scene, node,
                    dn->base_x + sinf((float)g_engine.sim_time * speed) * range,
                    dn->base_y);
}

/* --- Particles2D: a krait-registered custom kind ------------------
 *
 * A deterministic CPU particle emitter: the process op spawns particles
 * from an accumulator (rate/second) with a random direction inside the
 * spread cone around -Y, moves them linearly, and compacts the dead; the
 * draw op renders each particle as a small quad whose color lerps from
 * col_start to col_end over its lifetime. */

static unsigned
engine_prng(ParticlesState *ps)
{
    ps->rng ^= ps->rng << 13;
    ps->rng ^= ps->rng >> 17;
    ps->rng ^= ps->rng << 5;
    return ps->rng;
}

static void
engine_particles_process(Scene *scene, NodeId node, float dt)
{
    Node *n = NodeGet(scene, node);
    ParticlesState *ps;

    if(n == NULL)
        return;
    ps = (ParticlesState *)n->state;
    if(ps == NULL || dt <= 0.0f)
        return;
    if(ps->emitting && ps->rate > 0.0f) {
        ps->spawn_acc += ps->rate * dt;
        while(ps->spawn_acc >= 1.0f) {
            ps->spawn_acc -= 1.0f;
            if(ps->count >= ENGINE_PARTICLES_MAX)
                break;
            {
                EngineParticle *pt = &ps->parts[ps->count++];
                float a = ((float)(engine_prng(ps) % 36001) / 36000.0f -
                           0.5f) * (ps->spread * 3.14159265f / 180.0f);
                float base = -3.14159265f * 0.5f;   /* up */

                pt->x = n->world.position.x;
                pt->y = n->world.position.y;
                pt->vx = cosf(base + a) * ps->speed;
                pt->vy = sinf(base + a) * ps->speed;
                pt->max_life = ps->lifetime > 0.0f ? ps->lifetime : 0.1f;
                pt->life = pt->max_life;
            }
        }
    } else {
        ps->spawn_acc = 0.0f;
    }
    {
        int i = 0;

        while(i < ps->count) {
            EngineParticle *pt = &ps->parts[i];

            pt->x += pt->vx * dt;
            pt->y += pt->vy * dt;
            pt->life -= dt;
            if(pt->life <= 0.0f)
                ps->parts[i] = ps->parts[--ps->count];  /* swap-remove */
            else
                i++;
        }
    }
}

static void
engine_particles_draw(Scene *scene, NodeId node)
{
    Node *n = NodeGet(scene, node);
    ParticlesState *ps;
    int i;

    if(n == NULL)
        return;
    ps = (ParticlesState *)n->state;
    if(ps == NULL)
        return;
    for(i = 0; i < ps->count; i++) {
        const EngineParticle *pt = &ps->parts[i];
        float t = 1.0f - pt->life / pt->max_life;   /* 0..1 over life */
        Color c;
        float size = 4.0f * (1.0f - t * 0.6f);

        if(t < 0.0f)
            t = 0.0f;
        if(t > 1.0f)
            t = 1.0f;
        c.r = (unsigned char)(ps->col_start.r +
                              (float)(ps->col_end.r - ps->col_start.r) * t);
        c.g = (unsigned char)(ps->col_start.g +
                              (float)(ps->col_end.g - ps->col_start.g) * t);
        c.b = (unsigned char)(ps->col_start.b +
                              (float)(ps->col_end.b - ps->col_start.b) * t);
        c.a = (unsigned char)(ps->col_start.a +
                              (float)(ps->col_end.a - ps->col_start.a) * t);
        DrawRectangleRec((Rectangle){ pt->x - size * 0.5f, pt->y - size * 0.5f,
                                      size, size }, c);
    }
}

static void
engine_particles_destroy(Scene *scene, Node *node)
{
    (void)scene;
    if(node->state != NULL) {
        free(node->state);
        node->state = NULL;
    }
}

static const NodeOps g_engine_particles_ops = {
    NULL,                    /* ready */
    engine_particles_process,
    NULL,                    /* physics_process */
    engine_particles_draw
};

/* Player3D: WASD/arrows move on the XZ plane, Space jumps; simple
 * gravity with a ground clamp. Behaviors mutate the RUNTIME pose, never
 * the authored document. Params: speed, jump, ground height. */
static void
engine_behavior_player3d(Scene *scene, NodeId node, float dt,
                         const float *params, int param_count, void *user)
{
    EngineNode *dn = NULL;
    int i;
    float speed = param_count > 0 ? params[0] : 4.0f;
    float jump = param_count > 1 ? params[1] : 5.0f;
    float ground = param_count > 2 ? params[2] : 0.0f;

    (void)scene;
    (void)node;
    (void)user;
    if(g_engine.name_focused || g_engine.asset_focused)
        return;
    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used && g_engine.nodes[i].runtime == node) {
            dn = &g_engine.nodes[i];
            break;
        }
    }
    if(dn == NULL)
        return;
    {
        int dx = 0, dz = 0;

        if(IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
            dx -= 1;
        if(IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
            dx += 1;
        if(IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            dz -= 1;
        if(IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            dz += 1;
        if(dx != 0 && dz != 0) {
            dx = (int)((float)dx * 0.7071f);
            dz = (int)((float)dz * 0.7071f);
        }
        dn->r_x += (float)dx * speed * dt;
        dn->r_z += (float)dz * speed * dt;
        /* jump only from the ground */
        if((IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_W) ||
            IsKeyPressed(KEY_UP)) && dn->r_y <= ground + 0.001f)
            dn->b3_vy = jump;
    }
    dn->b3_vy -= 9.8f * dt;
    dn->r_y += dn->b3_vy * dt;
    if(dn->r_y < ground) {
        dn->r_y = ground;
        dn->b3_vy = 0.0f;
    }
}

/* Spin3D: constant Y rotation, radians per second (runtime pose only). */
static void
engine_behavior_spin3d(Scene *scene, NodeId node, float dt,
                       const float *params, int param_count, void *user)
{
    EngineNode *dn = NULL;
    int i;
    float rate = param_count > 0 ? params[0] : 1.0f;

    (void)scene;
    (void)node;
    (void)user;
    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used && g_engine.nodes[i].runtime == node) {
            dn = &g_engine.nodes[i];
            break;
        }
    }
    if(dn == NULL)
        return;
    dn->r_rot_y += rate * dt;
}

/* --- Timer: a krait-registered custom kind -------------------------
 *
 * kryon's extension path (NodeRegisterCustomKind + NodeRegisterOps) is used
 * for the Timer so no kryon enum change is needed: the process op counts
 * down wait_time and emits a Godot-style "timeout" signal on the node. */

static void
engine_timer_process(Scene *scene, NodeId node, float dt)
{
    Node *n = NodeGet(scene, node);
    EngineTimerState *t;

    if(n == NULL)
        return;
    t = (EngineTimerState *)n->state;
    if(t == NULL || !t->running || t->wait_time <= 0.0f)
        return;
    t->elapsed += dt;
    while(t->elapsed >= (double)t->wait_time) {
        t->elapsed -= (double)t->wait_time;
        t->fired_count++;
        SignalEmit(scene, node, "timeout", PropertyInt(t->fired_count));
        if(!t->loop) {
            t->running = 0;
            t->elapsed = 0.0;
            break;
        }
    }
}

static void
engine_timer_destroy(Scene *scene, Node *node)
{
    (void)scene;
    if(node->state != NULL) {
        free(node->state);
        node->state = NULL;
    }
    if(node->props != NULL) {
        free(node->props);
        node->props = NULL;
    }
}

static const NodeOps g_engine_timer_ops = {
    NULL,                 /* ready */
    engine_timer_process,
    NULL,                 /* physics_process */
    NULL                  /* draw: invisible */
};

/* Trigger actions run through the kryon signal bus: triggers connect the
 * emitter's body_enter/timeout signal to the hidden EngineEvents node whose
 * kind (the Timer kind) carries this handler. */
static void
engine_signal_handler(Scene *scene, NodeId target, NodeId emitter,
                       const char *handler, PropertyValue arg)
{
    EngineState *e = &g_engine;
    EngineNode *dn = NULL;
    int i;

    (void)target;
    (void)arg;
    if(scene != &e->scene)
        return;
    for(i = 0; i < e->node_count; i++) {
        if(e->nodes[i].used && e->nodes[i].runtime == emitter) {
            dn = &e->nodes[i];
            break;
        }
    }
    if(dn == NULL)
        return;
    if(strcmp(handler, "collect") == 0 && !dn->collected) {
        dn->collected = 1;
        e->score++;
        if(dn->carrier_runtime >= 0)
            NodeRemove(scene, dn->carrier_runtime);
        else
            NodeRemove(scene, emitter);
        engine_status("Collected %s (score %d)", dn->name, e->score);
    } else if(strcmp(handler, "win") == 0 && !e->won) {
        e->won = 1;
        e->paused = 1;
        engine_status("Level complete! (score %d)", e->score);
    } else if(strcmp(handler, "score") == 0) {
        e->score++;
    }
}

static void
engine_register_once(void)
{
    /* Kinds and behaviors are process-global: register exactly once
     * (re-registering after every reset would exhaust kryon's 64-kind
     * table in long sessions). The ids are re-bound to g_engine on
     * every reset because the reset clears it. */
    static NodeKind s_timer_kind, s_particles_kind;
    static NodeKind s_node3d_kind, s_mesh3d_kind, s_camera3d_kind;
    static int registered;

    if(registered) {
        g_engine.timer_kind = s_timer_kind;
        g_engine.particles_kind = s_particles_kind;
        g_engine.node3d_kind = s_node3d_kind;
        g_engine.mesh3d_kind = s_mesh3d_kind;
        g_engine.camera3d_kind = s_camera3d_kind;
        return;
    }
    registered = 1;
    SceneRegisterBuiltins();
    SceneRegisterBuiltinProperties();
    {
        static const char *const player_params[] = { "speed", "jump" };
        static const float player_defaults[] = { 240.0f, 380.0f };
        static const char *const spin_params[] = { "rate" };
        static const float spin_defaults[] = { 1.6f };
        static const char *const patrol_params[] = { "range", "speed" };
        static const float patrol_defaults[] = { 64.0f, 1.5f };

        krait_engine_behavior_register("player", "Player", player_params,
                                       player_defaults, 2,
                                       engine_behavior_player, NULL);
        krait_engine_behavior_register("spin", "Spin", spin_params,
                                       spin_defaults, 1,
                                       engine_behavior_spin, NULL);
        krait_engine_behavior_register("patrol", "Patrol", patrol_params,
                                       patrol_defaults, 2,
                                       engine_behavior_patrol, NULL);
    }
    {
        static const char *const p3d_params[] = { "speed", "jump", "ground" };
        static const float p3d_defaults[] = { 4.0f, 5.0f, 0.0f };
        static const char *const spin3d_params[] = { "rate" };
        static const float spin3d_defaults[] = { 1.0f };

        krait_engine_behavior_register("player3d", "Player 3D", p3d_params,
                                       p3d_defaults, 3,
                                       engine_behavior_player3d, NULL);
        krait_engine_behavior_register("spin3d", "Spin 3D", spin3d_params,
                                       spin3d_defaults, 1,
                                       engine_behavior_spin3d, NULL);
    }
    g_engine.timer_kind = NodeRegisterCustomKind("Timer");
    if((int)g_engine.timer_kind >= 0) {
        NodeRegisterOps(g_engine.timer_kind, &g_engine_timer_ops);
        NodeRegisterDestroy(g_engine.timer_kind, engine_timer_destroy);
        NodeKindRegisterSignalHandler(g_engine.timer_kind, engine_signal_handler);
    }
    g_engine.particles_kind = NodeRegisterCustomKind("Particles2D");
    if((int)g_engine.particles_kind >= 0) {
        NodeRegisterOps(g_engine.particles_kind, &g_engine_particles_ops);
        NodeRegisterDestroy(g_engine.particles_kind, engine_particles_destroy);
    }
    /* 3D kinds: presence markers in the scene tree; the 3D render pass
     * in engine_render draws them with kryon's 3D tier. */
    g_engine.node3d_kind = NodeRegisterCustomKind("Node3D");
    g_engine.mesh3d_kind = NodeRegisterCustomKind("MeshInstance3D");
    g_engine.camera3d_kind = NodeRegisterCustomKind("Camera3D");
    s_timer_kind = g_engine.timer_kind;
    s_particles_kind = g_engine.particles_kind;
    s_node3d_kind = g_engine.node3d_kind;
    s_mesh3d_kind = g_engine.mesh3d_kind;
    s_camera3d_kind = g_engine.camera3d_kind;
    g_engine.builtins_registered = 1;
}

static void
engine_teardown_runtime(void)
{
    if(!g_engine.runtime_ready)
        return;
    if(g_engine.physics_created) {
        ScenePhysicsDestroy(&g_engine.scene);
        g_engine.physics_created = 0;
    }
    SceneDestroy(&g_engine.scene);
    g_engine.runtime_ready = 0;
    memset(&g_engine.scene, 0, sizeof(g_engine.scene));
}

static void
engine_build(void)
{
    int i;
    EngineState *e = &g_engine;

    engine_teardown_runtime();
    engine_register_once();
    SceneInit(&e->scene);
    for(i = 0; i < e->node_count; i++) {
        if(!e->nodes[i].used)
            continue;
        if(e->nodes[i].kind == NODE_BODY2D || e->nodes[i].kind == NODE_AREA2D) {
            if(!e->physics_created)
                e->physics_created =
                    ScenePhysicsCreate(&e->scene, e->gravity_x, e->gravity_y);
        }
    }
    /* Hidden signal target: trigger handlers run on this node's kind. */
    e->events_runtime = -1;
    if((int)e->timer_kind >= 0) {
        e->events_runtime = NodeCreate(&e->scene, e->scene.root,
                                       e->timer_kind, "EngineEvents");
        if(e->events_runtime >= 0) {
            Node *en = NodeGet(&e->scene, e->events_runtime);

            if(en != NULL)
                en->state = calloc(1, sizeof(EngineTimerState));
        }
    }
    for(i = 0; i < e->node_count; i++) {
        EngineNode *dn = &e->nodes[i];
        NodeId parent_rt = e->scene.root;
        NodeKind kind = dn->kind;

        if(!dn->used)
            continue;
        dn->r_x = dn->x;
        dn->r_y = dn->y;
        dn->r_z = dn->z;
        dn->r_rot_y = dn->rot_y3;
        if((int)kind == ENGINE_KIND_TIMER)
            kind = e->timer_kind;
        else if((int)kind == ENGINE_KIND_PARTICLES)
            kind = e->particles_kind;
        else if((int)kind == ENGINE_KIND_NODE3D)
            kind = e->node3d_kind;
        else if((int)kind == ENGINE_KIND_MESH3D)
            kind = e->mesh3d_kind;
        else if((int)kind == ENGINE_KIND_CAMERA3D)
            kind = e->camera3d_kind;
        if(dn->parent != 0) {
            EngineNode *pn = engine_node_by_id(dn->parent);
            if(pn != NULL && pn->runtime >= 0)
                parent_rt = pn->runtime;
        }
        /* A standalone Area2D has no Body2D ancestor, but sensor shapes
         * attach to bodies: give it an implicit static carrier. */
        dn->carrier_runtime = -1;
        if(dn->kind == NODE_AREA2D) {
            int has_body_ancestor = 0;
            EngineNode *pn = dn->parent != 0 ? engine_node_by_id(dn->parent) : NULL;

            while(pn != NULL) {
                if(pn->kind == NODE_BODY2D) {
                    has_body_ancestor = 1;
                    break;
                }
                pn = pn->parent != 0 ? engine_node_by_id(pn->parent) : NULL;
            }
            if(!has_body_ancestor && parent_rt >= 0) {
                float wx = 0.0f, wy = 0.0f;
                EngineNode *walk = dn;
                Body2DProps *carrier;

                while(walk != NULL) {
                    wx += walk->x;
                    wy += walk->y;
                    walk = walk->parent != 0 ? engine_node_by_id(walk->parent) : NULL;
                }
                dn->carrier_runtime = NodeCreate(&e->scene, parent_rt,
                                                 NODE_BODY2D, "AreaBody");
                if(dn->carrier_runtime >= 0) {
                    NodeSetPosition(&e->scene, dn->carrier_runtime, wx, wy);
                    carrier = KryBody2DPropsAlloc(KRY_BODY2D_STATIC);
                    if(carrier != NULL)
                        NodeSetProps(&e->scene, dn->carrier_runtime, carrier);
                    parent_rt = dn->carrier_runtime;
                }
            }
        }
        dn->runtime = NodeCreate(&e->scene, parent_rt, kind, dn->name);
        if(dn->runtime < 0)
            continue;
        NodeSetPosition(&e->scene, dn->runtime, dn->x, dn->y);
        NodeSetRotation(&e->scene, dn->runtime, dn->rot * 0.017453293f);
        NodeSetScale(&e->scene, dn->runtime, dn->sx, dn->sy);
        switch(dn->kind) {
        case NODE_SPRITE2D: {
            Sprite2DProps *p = KrySprite2DPropsAlloc(engine_asset_abs(dn->asset),
                                                     dn->w, dn->h);
            if(p != NULL) {
                if(dn->tint.a != 0)
                    p->tint = dn->tint;
                NodeSetProps(&e->scene, dn->runtime, p);
            }
            break;
        }
        case NODE_ANIMATED_SPRITE2D: {
            AnimatedSprite2DProps *p = KryAnimatedSprite2DPropsAlloc(
                engine_asset_abs(dn->asset), dn->frame_count, dn->frames_per_row,
                dn->frame_w, dn->frame_h, dn->fps);
            if(p != NULL) {
                p->size.x = dn->w;
                p->size.y = dn->h;
                if(dn->tint.a != 0)
                    p->tint = dn->tint;
                NodeSetProps(&e->scene, dn->runtime, p);
            }
            break;
        }
        case NODE_CAMERA2D: {
            Camera2DProps *p = KryCamera2DPropsAlloc(dn->cam_zoom, dn->cam_active);
            if(p != NULL)
                NodeSetProps(&e->scene, dn->runtime, p);
            break;
        }
        case NODE_BODY2D: {
            Body2DProps *p = KryBody2DPropsAlloc(
                dn->body_type == 2 ? KRY_BODY2D_DYNAMIC :
                dn->body_type == 1 ? KRY_BODY2D_KINEMATIC : KRY_BODY2D_STATIC);
            if(p != NULL) {
                p->fixed_rotation = dn->fixed_rotation;
                p->gravity_scale = dn->gravity_scale;
                NodeSetProps(&e->scene, dn->runtime, p);
            }
            {
                CollisionShape2DProps *sp = KryCollisionShape2DPropsAlloc(
                    dn->shape_circle ? KRY_SHAPE2D_CIRCLE : KRY_SHAPE2D_BOX,
                    dn->w, dn->h);
                if(sp != NULL) {
                    dn->shape_runtime = NodeCreate(&e->scene, dn->runtime,
                                                   NODE_COLLISION_SHAPE2D, "Shape");
                    NodeSetProps(&e->scene, dn->shape_runtime, sp);
                }
            }
            break;
        }
        case NODE_AREA2D: {
            Area2DProps *p = KryArea2DPropsAlloc();
            if(p != NULL) {
                p->monitoring = 1;
                NodeSetProps(&e->scene, dn->runtime, p);
            }
            {
                CollisionShape2DProps *sp = KryCollisionShape2DPropsAlloc(
                    dn->shape_circle ? KRY_SHAPE2D_CIRCLE : KRY_SHAPE2D_BOX,
                    dn->w, dn->h);
                if(sp != NULL) {
                    sp->is_sensor = 1;
                    dn->shape_runtime = NodeCreate(&e->scene, dn->runtime,
                                                   NODE_COLLISION_SHAPE2D, "Shape");
                    NodeSetProps(&e->scene, dn->shape_runtime, sp);
                }
            }
            break;
        }
        case NODE_AUDIO_SOURCE: {
            AudioSourceProps *p = KryAudioSourcePropsAlloc(
                engine_asset_abs(dn->asset),
                dn->audio_kind ? KRY_AUDIO_MUSIC : KRY_AUDIO_SOUND);
            if(p != NULL) {
                p->volume = dn->volume;
                p->pitch = dn->pitch;
                p->loop = dn->loop;
                NodeSetProps(&e->scene, dn->runtime, p);
            }
            break;
        }
        case NODE_TIMER: {
            /* AnimationPlayer (kryon's NODE_TIMER kind); tracks are filled
             * in a second pass below, once every target exists. */
            AnimationPlayerProps *p = KryAnimationPlayerPropsAlloc();

            if(p != NULL)
                NodeSetProps(&e->scene, dn->runtime, p);
            break;
        }
        case NODE_TILEMAP: {
            TileMapProps *p = KryTileMapPropsAlloc(engine_asset_abs(dn->asset),
                                                   dn->tile_w, dn->tile_h,
                                                   dn->tiles_per_row,
                                                   dn->map_w, dn->map_h);
            if(p != NULL) {
                p->tiles = dn->tiles;   /* doc-owned, outlives the build */
                p->tile_px_w = dn->tile_px_w;
                p->tile_px_h = dn->tile_px_h;
                if(dn->tint.a != 0)
                    p->tint = dn->tint;
                NodeSetProps(&e->scene, dn->runtime, p);
            }
            break;
        }
        default:
            if((int)dn->kind == ENGINE_KIND_TIMER) {
                Node *rn = NodeGet(&e->scene, dn->runtime);

                if(rn != NULL) {
                    EngineTimerState *t = calloc(1, sizeof(*t));

                    if(t != NULL) {
                        t->wait_time = dn->wait_time > 0.0f ? dn->wait_time : 1.0f;
                        t->loop = dn->loop;
                        t->running = dn->autostart;
                        rn->state = t;
                    }
                }
            } else if((int)dn->kind == ENGINE_KIND_PARTICLES) {
                Node *rn = NodeGet(&e->scene, dn->runtime);

                if(rn != NULL) {
                    ParticlesState *ps = calloc(1, sizeof(*ps));

                    if(ps != NULL) {
                        ps->rate = dn->p_rate;
                        ps->lifetime = dn->p_lifetime;
                        ps->speed = dn->p_speed;
                        ps->spread = dn->p_spread;
                        ps->col_start = dn->p_col_start;
                        ps->col_end = dn->p_col_end;
                        ps->emitting = dn->p_emitting;
                        ps->rng = 0x9e3779b9u;
                        rn->state = ps;
                    }
                }
            }
            break;
        }
    }
    /* fill AnimationPlayer tracks now that every target node exists */
    for(i = 0; i < e->node_count; i++) {
        EngineNode *dn = &e->nodes[i];
        AnimationPlayerProps *p;
        KryAnimation *anim;
        int t, k;

        if(!dn->used || dn->kind != NODE_TIMER || dn->runtime < 0)
            continue;
        p = (AnimationPlayerProps *)NodeGet(&e->scene, dn->runtime)->props;
        if(p == NULL)
            continue;
        anim = &p->anims[0];
        memset(anim, 0, sizeof(*anim));
        snprintf(anim->name, sizeof(anim->name), "%s", dn->name);
        anim->loop = dn->anim_loop;
        for(t = 0; t < dn->anim_track_count && t < ENGINE_ANIM_TRACKS &&
                    anim->track_count < KRY_ANIM_TRACKS_MAX; t++) {
            EngineNode *target = engine_node_by_name(dn->anim_tracks[t].target);
            KryAnimTrack *track = &anim->tracks[anim->track_count];

            if(target == NULL || target->runtime < 0)
                continue;
            memset(track, 0, sizeof(*track));
            track->target = target->runtime;
            switch(dn->anim_tracks[t].property) {
            case ENGINE_ANIM_POS_X:
                snprintf(track->property, sizeof(track->property), "position");
                track->component = 0;
                break;
            case ENGINE_ANIM_POS_Y:
                snprintf(track->property, sizeof(track->property), "position");
                track->component = 1;
                break;
            case ENGINE_ANIM_ROTATION:
                snprintf(track->property, sizeof(track->property), "rotation");
                track->component = -1;
                break;
            case ENGINE_ANIM_SCALE_X:
                snprintf(track->property, sizeof(track->property), "scale");
                track->component = 0;
                break;
            default:
                snprintf(track->property, sizeof(track->property), "scale");
                track->component = 1;
                break;
            }
            track->interp = KRY_ANIM_INTERP_LINEAR;
            for(k = 0; k < dn->anim_tracks[t].key_count &&
                       k < ENGINE_ANIM_KEYS &&
                       track->keyframe_count < KRY_ANIM_KEYS_MAX; k++) {
                track->keyframes[track->keyframe_count].time =
                    dn->anim_tracks[t].keys[k][0];
                track->keyframes[track->keyframe_count].value =
                    dn->anim_tracks[t].keys[k][1];
                track->keyframe_count++;
            }
            /* keyframes must be time-ordered for sampling */
            for(k = 1; k < track->keyframe_count; k++) {
                KryKeyframe kf = track->keyframes[k];
                int j = k - 1;

                while(j >= 0 && track->keyframes[j].time > kf.time) {
                    track->keyframes[j + 1] = track->keyframes[j];
                    j--;
                }
                track->keyframes[j + 1] = kf;
            }
            if(track->keyframe_count > 0 &&
               track->keyframes[track->keyframe_count - 1].time > anim->duration)
                anim->duration =
                    track->keyframes[track->keyframe_count - 1].time;
            anim->track_count++;
        }
        p->anim_count = anim->track_count > 0 ? 1 : 0;
        p->current = p->anim_count > 0 ? 0 : -1;
        p->time = 0.0f;
        /* autoplay engages on Play, not during edit-mode ticks: the
         * timeline scrub owns the pose while editing */
        p->playing = p->anim_count > 0 && dn->anim_autoplay && e->playing;
    }
    /* wire triggers through the signal bus */
    if(e->events_runtime >= 0) {
        for(i = 0; i < e->node_count; i++) {
            EngineNode *dn = &e->nodes[i];

            if(!dn->used || dn->trigger == ENGINE_TRIGGER_NONE ||
               dn->runtime < 0)
                continue;
            if(dn->kind == NODE_AREA2D)
                SignalConnect(&e->scene, dn->runtime, "body_enter",
                              e->events_runtime, g_trigger_ids[dn->trigger]);
            else if((int)dn->kind == ENGINE_KIND_TIMER)
                SignalConnect(&e->scene, dn->runtime, "timeout",
                              e->events_runtime, g_trigger_ids[dn->trigger]);
            else if(dn->kind == NODE_TIMER)
                SignalConnect(&e->scene, dn->runtime, "animation_finished",
                              e->events_runtime, g_trigger_ids[dn->trigger]);
        }
    }
    for(i = 0; i < e->node_count; i++)
        e->nodes[i].collected = 0;
    SceneTick(&e->scene, 0.0f);   /* fire ready hooks + first transforms */
    e->runtime_camera = e->scene.active_camera;
    e->runtime_ready = 1;
    e->physics_acc = 0.0f;
    e->sim_time = 0.0;
    e->dirty = 0;
    if(!e->playing)
        e->scene.active_camera = -1;  /* edit mode always uses the editor camera */
}

static void
engine_capture_behavior_bases(void)
{
    int i;

    for(i = 0; i < g_engine.node_count; i++) {
        EngineNode *dn = &g_engine.nodes[i];
        Node *rn;

        if(!dn->used || dn->runtime < 0)
            continue;
        rn = NodeGet(&g_engine.scene, dn->runtime);
        if(rn == NULL)
            continue;
        dn->base_x = rn->local.position.x;
        dn->base_y = rn->local.position.y;
        dn->base_rot = rn->local.rotation;
        dn->b3_vy = 0.0f;
        dn->script_var_count = 0;
        dn->script_rng = 0x9e3779b9u;
    }
}

static void
engine_start_audio(void)
{
    int i;

    for(i = 0; i < g_engine.node_count; i++) {
        EngineNode *dn = &g_engine.nodes[i];

        if(!dn->used || dn->kind != NODE_AUDIO_SOURCE)
            continue;
        if(dn->asset[0] == '\0' || dn->runtime < 0)
            continue;
        if(!g_engine.audio_opened) {
            if(!IsAudioDeviceReady())
                InitAudioDevice();
            g_engine.audio_opened = 1;
        }
        KryAudioSourcePlay(&g_engine.scene, dn->runtime);
    }
}

void
krait_engine_play(void)
{
    if(g_engine.dirty || !g_engine.runtime_ready)
        engine_build();
    g_engine.score = 0;
    g_engine.won = 0;
    {
        int i;

        for(i = 0; i < g_engine.node_count; i++) {
            EngineNode *dn = &g_engine.nodes[i];

            if(dn->used && dn->kind == NODE_TIMER && dn->runtime >= 0) {
                Node *rn = NodeGet(&g_engine.scene, dn->runtime);
                AnimationPlayerProps *p = rn != NULL
                    ? (AnimationPlayerProps *)rn->props : NULL;

                if(p != NULL && p->anim_count > 0 && dn->anim_autoplay) {
                    p->time = 0.0f;
                    p->playing = 1;
                }
            }
        }
    }
    g_engine.scene.active_camera = g_engine.runtime_camera;
    g_engine.playing = 1;
    g_engine.paused = 0;
    engine_capture_behavior_bases();
    engine_start_audio();
    engine_status("Playing %s", g_engine.name);
}

void
krait_engine_pause(void)
{
    if(!g_engine.playing)
        return;
    g_engine.paused = !g_engine.paused;
    engine_status("%s", g_engine.paused ? "Paused" : "Playing");
}

void
krait_engine_stop(void)
{
    g_engine.playing = 0;
    g_engine.paused = 0;
    engine_build();               /* rebuild restores the authored scene */
    engine_status("Stopped");
}

static void
engine_apply_behaviors(float dt)
{
    int i;

    for(i = 0; i < g_engine.node_count; i++) {
        EngineNode *dn = &g_engine.nodes[i];
        const KraitBehaviorDef *def;

        if(!dn->used || dn->runtime < 0 || dn->behavior_id[0] == '\0')
            continue;
        def = engine_behavior_by_id(dn->behavior_id);
        if(def == NULL || def->fn == NULL)
            continue;
        def->fn(&g_engine.scene, dn->runtime, dt, dn->behavior_params,
                def->param_count, def->user);
    }
}

void
krait_engine_advance(float dt)
{
    const float step = 1.0f / 60.0f;
    int i;
    int steps = 0;

    if(!g_engine.runtime_ready || g_engine.dirty)
        engine_build();
    if(g_engine.playing && !g_engine.paused) {
        engine_apply_behaviors(dt);
        for(i = 0; i < g_engine.node_count; i++) {
            if(g_engine.nodes[i].used)
                krait_script_run(&g_engine.nodes[i], dt);
        }
        SceneTick(&g_engine.scene, dt);
        g_engine.physics_acc += dt;
        while(g_engine.physics_acc >= step && steps < 4) {
            ScenePhysicsTick(&g_engine.scene, step);
            g_engine.physics_acc -= step;
            steps++;
        }
        if(steps == 4)
            g_engine.physics_acc = 0.0f;   /* drop backlog after a stall */
        g_engine.sim_time += dt;
    } else {
        SceneTick(&g_engine.scene, 0.0f);
        if(!g_engine.playing)
            g_engine.scene.active_camera = -1;
    }
}

/* ------------------------------------------------------------------ */
/* public document API (shared by the UI and the tests)                */
/* ------------------------------------------------------------------ */

void
krait_engine_reset(const char *project_dir)
{
    engine_teardown_runtime();
    memset(&g_engine, 0, sizeof(g_engine));
    engine_register_once();
    g_engine.cam_x = 320.0f;
    g_engine.cam_y = 180.0f;
    g_engine.cam_zoom = 1.0f;
    g_engine.orbit_yaw = 45.0f;
    g_engine.orbit_pitch = 30.0f;
    g_engine.orbit_dist = 10.0f;
    if(project_dir != NULL && project_dir[0] != '\0') {
        snprintf(g_engine.project_root, sizeof(g_engine.project_root), "%s",
                 project_dir);
        krait_join(g_engine.scene_path, sizeof(g_engine.scene_path),
                   project_dir, ENGINE_SCENE_FILE);
        g_engine.has_scene_path = 1;
        if(!krait_engine_load(g_engine.scene_path))
            engine_doc_starter();
    } else {
        engine_doc_starter();
    }
    engine_status("Ready");
}

int
krait_engine_add_node(int kind_index, const char *name)
{
    EngineNode *n;

    if(kind_index < 0 || kind_index >= ENGINE_KIND_COUNT)
        return 0;
    n = engine_doc_add(g_kinds[kind_index].kind, name, g_engine.selected);
    if(n == NULL)
        return 0;
    g_engine.selected = n->id;
    engine_build();
    return n->id;
}

int
krait_engine_delete_selected(void)
{
    EngineNode *n = engine_node_by_id(g_engine.selected);

    if(n == NULL)
        return 0;
    engine_doc_remove(n);
    engine_doc_compact();
    engine_build();
    return 1;
}

int
krait_engine_node_count(void)
{
    int i, count = 0;

    for(i = 0; i < g_engine.node_count; i++) {
        if(g_engine.nodes[i].used)
            count++;
    }
    return count;
}

const char *
krait_engine_node_name(int id)
{
    EngineNode *n = engine_node_by_id(id);

    return n != NULL ? n->name : NULL;
}

int
krait_engine_node_pos(int id, float *x, float *y)
{
    EngineNode *n = engine_node_by_id(id);
    Node *rn;

    if(n == NULL || n->runtime < 0 || x == NULL || y == NULL)
        return 0;
    rn = NodeGet(&g_engine.scene, n->runtime);
    if(rn == NULL)
        return 0;
    *x = rn->local.position.x;
    *y = rn->local.position.y;
    return 1;
}

int
krait_engine_set_trigger(int id, int trigger)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || trigger < 0 || trigger >= ENGINE_TRIGGER_COUNT)
        return 0;
    if(n->kind != NODE_AREA2D && (int)n->kind != ENGINE_KIND_TIMER)
        return 0;
    n->trigger = trigger;
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_playing(void)
{
    return g_engine.playing;
}

int
krait_engine_paused(void)
{
    return g_engine.paused;
}

/* ------------------------------------------------------------------ */
/* standalone player mode (krait --play-game)                          */
/* ------------------------------------------------------------------ */

int
krait_engine_play_scene(const char *path_or_dir)
{
    char full[KRAIT_PATH_MAX];
    char dir[KRAIT_PATH_MAX];
    const char *base;
    size_t len;

    if(path_or_dir == NULL || path_or_dir[0] == '\0')
        return 0;
    snprintf(full, sizeof(full), "%s", path_or_dir);
    len = strlen(full);
    if(len > 5 && strcmp(full + len - 6, ".scene") == 0) {
        snprintf(dir, sizeof(dir), "%s", full);
        base = strrchr(dir, '/');
        if(base != NULL) {
            if(base == dir)
                dir[1] = '\0';
            else
                dir[(size_t)(base - dir)] = '\0';
        } else {
            snprintf(dir, sizeof(dir), ".");
        }
        krait_engine_reset(dir);
        if(strcmp(base != NULL ? base + 1 : full, ENGINE_SCENE_FILE) != 0 &&
           !krait_engine_load(full)) {
            fprintf(stderr, "krait: cannot load scene %s\n", full);
            return 0;
        }
    } else {
        /* a directory: <dir>/game.scene */
        char scene[KRAIT_PATH_MAX];

        krait_join(scene, sizeof(scene), path_or_dir, ENGINE_SCENE_FILE);
        krait_engine_reset(path_or_dir);
        if(!krait_engine_load(scene)) {
            fprintf(stderr, "krait: no %s in %s\n", ENGINE_SCENE_FILE,
                    path_or_dir);
            return 0;
        }
    }
    g_engine.initialized = 1;
    engine_build();
    krait_engine_play();
    return 1;
}

void
krait_engine_draw_play(Rectangle bounds)
{
    float dt = GetFrameTime();
    float scale, vh;
    Rectangle dst;

    if(dt > 0.05f)
        dt = 0.05f;
    krait_engine_advance(dt);
    DrawRectangleRec(bounds, GetThemeBackground());
    scale = bounds.width / (float)g_engine.view_w;
    vh = (float)g_engine.view_h * scale;
    if(vh > bounds.height) {
        vh = bounds.height;
        scale = vh / (float)g_engine.view_h;
    }
    dst = (Rectangle){
        bounds.x + (bounds.width - (float)g_engine.view_w * scale) * 0.5f,
        bounds.y + (bounds.height - vh) * 0.5f,
        (float)g_engine.view_w * scale, vh
    };
    g_engine.view_dst = dst;
    g_engine.view_valid = 1;
    engine_render();
    if(g_engine.rt_w > 0) {
        DrawTexturePro(g_engine.rt.texture,
                       (Rectangle){ 0, 0, (float)g_engine.rt_w, -(float)g_engine.rt_h },
                       dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
    }
}

void
krait_engine_view_size(int *w, int *h)
{
    if(w != NULL)
        *w = g_engine.view_w;
    if(h != NULL)
        *h = g_engine.view_h;
}

const char *
krait_engine_scene_name(void)
{
    return g_engine.name;
}

/* Launch the standalone player on the current scene in a new process.
 * The scene is saved first so the player sees the authored document. */
int
krait_engine_run_game(void)
{
    char self[KRAIT_PATH_MAX];
    ssize_t n;
    pid_t pid;

    if(!g_engine.has_scene_path)
        return 0;
    krait_engine_save(g_engine.scene_path);
    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if(n <= 0)
        return 0;
    self[n] = '\0';
    pid = fork();
    if(pid < 0)
        return 0;
    if(pid == 0) {
        setsid();
        execl(self, self, "--play-game", g_engine.scene_path, (char *)NULL);
        _exit(127);
    }
    engine_status("Player launched (%s)", ENGINE_SCENE_FILE);
    return 1;
}

/* Export a launcher next to the scene: <project>/game-export/run.sh plus a
 * .desktop entry and a copy of the scene. The launcher runs the standalone
 * player outside the editor. */
int
krait_engine_export_game(char *status, int status_size)
{
    char dir[KRAIT_PATH_MAX];
    char path[KRAIT_PATH_MAX];
    char self[KRAIT_PATH_MAX];
    char script[2 * KRAIT_PATH_MAX];
    ssize_t n;
    FILE *f;

    if(!g_engine.has_scene_path) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Open a project first");
        return 0;
    }
    krait_engine_save(g_engine.scene_path);
    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if(n <= 0) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot locate krait");
        return 0;
    }
    self[n] = '\0';
    krait_join(dir, sizeof(dir), g_engine.project_root, "game-export");
    if(!krait_mkdir_p(dir)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot create %s", dir);
        return 0;
    }
    krait_join(path, sizeof(path), dir, ENGINE_SCENE_FILE);
    f = fopen(path, "w");
    if(f == NULL) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot write %s", path);
        return 0;
    }
    {
        FILE *src = fopen(g_engine.scene_path, "r");

        if(src != NULL) {
            char buf[4096];
            size_t got;

            while((got = fread(buf, 1, sizeof(buf), src)) > 0)
                fwrite(buf, 1, got, f);
            fclose(src);
        }
    }
    fclose(f);
    krait_join(path, sizeof(path), dir, "run.sh");
    {
        static const char run_sh[] =
            "#!/bin/sh\n"
            "# Standalone game launcher generated by the Krait Game Engine.\n"
            "# Self-contained: runs the bundled player (own window, no\n"
            "# editor and no krait installation needed).\n"
            "DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
            "exec \"$DIR/bin/krait-player\" --play-game \"$DIR/%s\" \"$@\"\n";

        snprintf(script, sizeof(script), run_sh, ENGINE_SCENE_FILE);
        (void)self;
    }
    if(!krait_write_text_file(path, script)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot write %s", path);
        return 0;
    }
    chmod(path, 0755);
    /* bundle the player binary so the export folder is self-contained:
     * it runs with no krait installation present */
    {
        char bin_dir[KRAIT_PATH_MAX];
        char bin_path[KRAIT_PATH_MAX];
        char dst_path[KRAIT_PATH_MAX];
        FILE *src_f, *dst_f;
        char buf[8192];
        size_t got;

        krait_join(bin_dir, sizeof(bin_dir), dir, "bin");
        krait_mkdir_p(bin_dir);
        krait_join(bin_path, sizeof(bin_path), bin_dir, "krait-player");
        snprintf(dst_path, sizeof(dst_path), "%s", self);
        /* avoid copying onto itself */
        if(strcmp(bin_path, dst_path) != 0) {
            src_f = fopen(dst_path, "rb");
            dst_f = src_f != NULL ? fopen(bin_path, "wb") : NULL;
            if(src_f != NULL && dst_f != NULL) {
                while((got = fread(buf, 1, sizeof(buf), src_f)) > 0)
                    fwrite(buf, 1, got, dst_f);
            }
            if(src_f != NULL)
                fclose(src_f);
            if(dst_f != NULL)
                fclose(dst_f);
            if(src_f == NULL || dst_f == NULL) {
                if(status != NULL && status_size > 0)
                    snprintf(status, (size_t)status_size,
                             "Cannot bundle player binary");
                return 0;
            }
            chmod(bin_path, 0755);
        }
    }
    krait_join(path, sizeof(path), dir, "play.desktop");
    snprintf(script, sizeof(script),
             "[Desktop Entry]\n"
             "Type=Application\n"
             "Name=%s\n"
             "Exec=%s/bin/krait-player --play-game %s/%s\n"
             "Terminal=false\n"
             "Categories=Game;\n",
             g_engine.name, dir, dir, ENGINE_SCENE_FILE);
    if(!krait_write_text_file(path, script)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Cannot write %s", path);
        return 0;
    }
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Exported to %s", dir);
    return 1;
}

/* ------------------------------------------------------------------ */
/* timeline editing: key manipulation over the AnimationPlayer doc     */
/* ------------------------------------------------------------------ */

static void
engine_anim_sort_keys(EngineNode *dn, int track)
{
    int k;

    if(track < 0 || track >= ENGINE_ANIM_TRACKS)
        return;
    for(k = 1; k < dn->anim_tracks[track].key_count; k++) {
        float kt = dn->anim_tracks[track].keys[k][0];
        float kv = dn->anim_tracks[track].keys[k][1];
        int j = k - 1;

        while(j >= 0 && dn->anim_tracks[track].keys[j][0] > kt) {
            dn->anim_tracks[track].keys[j + 1][0] =
                dn->anim_tracks[track].keys[j][0];
            dn->anim_tracks[track].keys[j + 1][1] =
                dn->anim_tracks[track].keys[j][1];
            j--;
        }
        dn->anim_tracks[track].keys[j + 1][0] = kt;
        dn->anim_tracks[track].keys[j + 1][1] = kv;
    }
}

int
krait_engine_anim_add_key(int id, int track, float time, float value)
{
    EngineNode *n = engine_node_by_id(id);
    int k;

    if(n == NULL || n->kind != NODE_TIMER)
        return 0;
    if(track < 0 || track >= n->anim_track_count ||
       track >= ENGINE_ANIM_TRACKS)
        return 0;
    k = n->anim_tracks[track].key_count;
    if(k >= ENGINE_ANIM_KEYS)
        return 0;
    if(time < 0.0f)
        time = 0.0f;
    n->anim_tracks[track].keys[k][0] = time;
    n->anim_tracks[track].keys[k][1] = value;
    n->anim_tracks[track].key_count = k + 1;
    engine_anim_sort_keys(n, track);
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_anim_move_key(int id, int track, int key, float time)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || n->kind != NODE_TIMER)
        return 0;
    if(track < 0 || track >= n->anim_track_count ||
       track >= ENGINE_ANIM_TRACKS)
        return 0;
    if(key < 0 || key >= n->anim_tracks[track].key_count)
        return 0;
    if(time < 0.0f)
        time = 0.0f;
    n->anim_tracks[track].keys[key][0] = time;
    engine_anim_sort_keys(n, track);
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_anim_delete_key(int id, int track, int key)
{
    EngineNode *n = engine_node_by_id(id);
    int i;

    if(n == NULL || n->kind != NODE_TIMER)
        return 0;
    if(track < 0 || track >= n->anim_track_count ||
       track >= ENGINE_ANIM_TRACKS)
        return 0;
    if(key < 0 || key >= n->anim_tracks[track].key_count)
        return 0;
    for(i = key; i + 1 < n->anim_tracks[track].key_count; i++) {
        n->anim_tracks[track].keys[i][0] =
            n->anim_tracks[track].keys[i + 1][0];
        n->anim_tracks[track].keys[i][1] =
            n->anim_tracks[track].keys[i + 1][1];
    }
    n->anim_tracks[track].key_count--;
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_anim_key_count(int id, int track)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || n->kind != NODE_TIMER || track < 0 ||
       track >= ENGINE_ANIM_TRACKS)
        return -1;
    return n->anim_tracks[track].key_count;
}

int
krait_engine_anim_key_get(int id, int track, int key, float *time,
                          float *value)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || n->kind != NODE_TIMER || track < 0 ||
       track >= ENGINE_ANIM_TRACKS)
        return 0;
    if(key < 0 || key >= n->anim_tracks[track].key_count)
        return 0;
    if(time != NULL)
        *time = n->anim_tracks[track].keys[key][0];
    if(value != NULL)
        *value = n->anim_tracks[track].keys[key][1];
    return 1;
}

/* Sample one track at time t (linear, hold at both ends). */
static float
engine_track_sample(const EngineNode *anim, int track, float t)
{
    const float(*keys)[2] = anim->anim_tracks[track].keys;
    int count = anim->anim_tracks[track].key_count;
    int i;

    if(count <= 0)
        return 0.0f;
    if(t <= keys[0][0])
        return keys[0][1];
    if(t >= keys[count - 1][0])
        return keys[count - 1][1];
    for(i = 1; i < count; i++) {
        if(t <= keys[i][0]) {
            float span = keys[i][0] - keys[i - 1][0];
            float f = span > 0.0f ? (t - keys[i - 1][0]) / span : 0.0f;

            return keys[i - 1][1] + (keys[i][1] - keys[i - 1][1]) * f;
        }
    }
    return keys[count - 1][1];
}

/* Scrub preview: apply the animation to the RUNTIME target nodes at time
 * t without touching the authored document (a rebuild restores it). */
int
krait_engine_timeline_scrub(int id, float t)
{
    EngineNode *anim = engine_node_by_id(id);
    int tr;

    if(anim == NULL || anim->kind != NODE_TIMER)
        return 0;
    if(!g_engine.runtime_ready)
        krait_engine_advance(0.0f);
    for(tr = 0; tr < anim->anim_track_count && tr < ENGINE_ANIM_TRACKS;
        tr++) {
        EngineNode *target = engine_node_by_name(anim->anim_tracks[tr].target);
        float v;
        Node *rn;

        if(target == NULL || target->runtime < 0)
            continue;
        rn = NodeGet(&g_engine.scene, target->runtime);
        if(rn == NULL)
            continue;
        v = engine_track_sample(anim, tr, t);
        switch(anim->anim_tracks[tr].property) {
        case ENGINE_ANIM_POS_X: rn->local.position.x = v; break;
        case ENGINE_ANIM_POS_Y: rn->local.position.y = v; break;
        case ENGINE_ANIM_ROTATION: rn->local.rotation = v; break;
        case ENGINE_ANIM_SCALE_X: rn->local.scale.x = v; break;
        default: rn->local.scale.y = v; break;
        }
        rn->flags |= NODE_FLAG_DIRTY;
    }
    SceneTick(&g_engine.scene, 0.0f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* smoke test (also the engine-test binary body)                       */
/* ------------------------------------------------------------------ */

static int
engine_check_float(const char *what, float got, float want, float eps)
{
    if(fabsf(got - want) <= eps)
        return 0;
    fprintf(stderr, "engine FAIL: %s got %g want %g\n", what, got, want);
    return 1;
}

int
krait_engine_smoke_test(void)
{
    char tmp[512];
    int fails = 0;
    int crate = 0, ground = 0, spinner = 0, coin = 0, terrain = 0, timer = 0;
    int marker = 0, mover = 0, sparkles = 0;
    int count, i;
    float x0, y0, x1, y1, ground_y0, ground_y1, scratch_x;
    float marker_x0, marker_x1, scratch_y;
    double t;

    snprintf(tmp, sizeof(tmp), "%s/krait-engine-smoke.scene",
             getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
    remove(tmp);

    krait_engine_reset(NULL);
    count = krait_engine_node_count();
    if(count != 9) {
        fprintf(stderr, "engine FAIL: starter node count %d want 9\n", count);
        fails++;
    }

    /* address starter nodes by name */
    for(i = 1; i <= 16; i++) {
        const char *nm = krait_engine_node_name(i);

        if(nm == NULL)
            continue;
        if(strcmp(nm, "Crate") == 0)
            crate = i;
        else if(strcmp(nm, "Ground") == 0)
            ground = i;
        else if(strcmp(nm, "Coin") == 0)
            coin = i;
        else if(strcmp(nm, "Terrain") == 0)
            terrain = i;
        else if(strcmp(nm, "Sparkles") == 0)
            sparkles = i;
    }
    if(crate == 0 || ground == 0 || coin == 0 || terrain == 0 ||
       sparkles == 0) {
        fprintf(stderr, "engine FAIL: starter nodes not addressable\n");
        fails++;
    }

    /* behaviors attach before play */
    spinner = krait_engine_add_node(0, "Spinner");
    if(spinner <= 0 || !krait_engine_set_behavior_id(spinner, "spin")) {
        fprintf(stderr, "engine FAIL: could not attach spin behavior\n");
        return 1;
    }

    /* a Timer scores on timeout via the signal bus */
    timer = krait_engine_add_node(7, "Ticker");
    if(timer <= 0 || !krait_engine_set_trigger(timer, ENGINE_TRIGGER_SCORE)) {
        fprintf(stderr, "engine FAIL: could not wire timer trigger\n");
        return 1;
    }

    /* an AnimationPlayer tweens a plain node (selected when added, so the
     * default track targets it) from its position to +200 on X */
    marker = krait_engine_add_node(0, "Marker");
    mover = krait_engine_add_node(8, "Mover");
    if(marker <= 0 || mover <= 0) {
        fprintf(stderr, "engine FAIL: could not add animation nodes\n");
        return 1;
    }
    if(!krait_engine_node_pos(marker, &marker_x0, &scratch_y) ||
       marker_x0 < 300.0f || marker_x0 > 340.0f) {
        fprintf(stderr, "engine FAIL: marker start position %.1f\n",
                marker_x0);
        return 1;
    }

    /* TileMap data survives the round trip */
    if(krait_engine_node_tile(terrain, 0, 5) != 1) {
        fprintf(stderr, "engine FAIL: starter tilemap ground row missing\n");
        fails++;
    }

    /* save/load round trip keeps nodes and fields */
    if(!krait_engine_save(tmp)) {
        fprintf(stderr, "engine FAIL: save %s\n", tmp);
        return 1;
    }
    krait_engine_reset(NULL);
    if(!krait_engine_load(tmp)) {
        fprintf(stderr, "engine FAIL: load %s\n", tmp);
        return 1;
    }
    count = krait_engine_node_count();
    if(count != 13) {
        fprintf(stderr, "engine FAIL: round-trip node count %d want 13\n", count);
        fails++;
    }
    if(crate == 0 || krait_engine_node_name(crate) == NULL ||
       strcmp(krait_engine_node_name(crate), "Crate") != 0) {
        fprintf(stderr, "engine FAIL: round-trip lost Crate\n");
        fails++;
    }
    if(krait_engine_node_tile(terrain, 0, 5) != 1 ||
       krait_engine_node_tile(terrain, 5, 0) != 0) {
        fprintf(stderr, "engine FAIL: round-trip lost tile data\n");
        fails++;
    }
    krait_engine_advance(0.0f);   /* force a runtime build */

    /* play: gravity pulls the dynamic crate down, the ground stays put */
    if(!krait_engine_node_pos(crate, &x0, &y0) ||
       !krait_engine_node_pos(ground, &scratch_x, &ground_y0)) {
        fprintf(stderr, "engine FAIL: runtime nodes missing\n");
        return 1;
    }
    krait_engine_play();
    for(t = 0.0; t < 1.2; t += 1.0 / 60.0)
        krait_engine_advance(1.0f / 60.0f);
    if(!krait_engine_node_pos(crate, &x1, &y1) ||
       !krait_engine_node_pos(ground, &scratch_x, &ground_y1)) {
        fprintf(stderr, "engine FAIL: runtime nodes missing while playing\n");
        return 1;
    }
    if(y1 <= y0 + 40.0f) {
        fprintf(stderr, "engine FAIL: crate did not fall (%.1f -> %.1f)\n",
                y0, y1);
        fails++;
    }
    fails += engine_check_float("ground stays", ground_y1, ground_y0, 0.01f);

    /* the Coin's collect trigger fired when the crate fell through it */
    if(krait_engine_score() < 1) {
        fprintf(stderr, "engine FAIL: coin not collected (score %d)\n",
                krait_engine_score());
        fails++;
    }
    /* the looping Ticker fired at least once (wait 1s, played 1.2s) */
    if(krait_engine_timer_fired(timer) < 1) {
        fprintf(stderr, "engine FAIL: timer never fired (%d)\n",
                krait_engine_timer_fired(timer));
        fails++;
    }
    /* the Sparkles emitter reached its steady state (~rate*lifetime) */
    {
        int alive = krait_engine_particle_count(sparkles);

        if(alive < 10 || alive > 30) {
            fprintf(stderr, "engine FAIL: particles steady state %d\n",
                    alive);
            fails++;
        }
    }
    /* the AnimationPlayer tweened the Marker to its end key (1s, no loop,
     * so the value clamps at the last keyframe) */
    if(!krait_engine_node_pos(marker, &marker_x1, &scratch_y) ||
       marker_x1 < marker_x0 + 195.0f || marker_x1 > marker_x0 + 205.0f) {
        fprintf(stderr, "engine FAIL: marker not animated (%.1f -> %.1f)\n",
                marker_x0, marker_x1);
        fails++;
    }
    if(krait_engine_score() < 2) {
        fprintf(stderr, "engine FAIL: timer score trigger not delivered\n");
        fails++;
    }

    /* the spin behavior advanced the runtime node's rotation */
    {
        EngineNode *sn = engine_node_by_id(spinner);
        Node *rn = sn != NULL && sn->runtime >= 0
                       ? NodeGet(&g_engine.scene, sn->runtime) : NULL;

        if(rn == NULL || rn->local.rotation <= 0.05f) {
            fprintf(stderr, "engine FAIL: spinner did not rotate\n");
            fails++;
        }
    }

    /* stop restores authored positions */
    krait_engine_stop();
    if(!krait_engine_node_pos(crate, &x1, &y1)) {
        fprintf(stderr, "engine FAIL: crate missing after stop\n");
        return 1;
    }
    fails += engine_check_float("stop restores crate", y1, y0, 0.01f);

    remove(tmp);
    if(fails == 0)
        fprintf(stderr, "krait: engine smoke ok (%d nodes)\n",
                krait_engine_node_count());
    return fails == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* editor drawing                                                      */
/* ------------------------------------------------------------------ */

static TextInputStyle
engine_input_style(void)
{
    return (TextInputStyle){
        GetThemeSurface(),
        GetThemeButton(),
        GetThemeLink(),
        GetThemeText(),
        GetThemeLink(),
        0,
        ScaleUIPx(8),
        ScaleUIPx(6)
    };
}

static Camera2D
engine_editor_camera(void)
{
    Camera2D cam;

    cam.target = (Vector2){ g_engine.cam_x, g_engine.cam_y };
    cam.offset = (Vector2){ g_engine.view_w * 0.5f, g_engine.view_h * 0.5f };
    cam.rotation = 0.0f;
    cam.zoom = g_engine.cam_zoom;
    return cam;
}

static void
engine_screen_to_world(float mx, float my, float *wx, float *wy)
{
    float rtx, rty;

    if(!g_engine.view_valid) {
        *wx = 0.0f;
        *wy = 0.0f;
        return;
    }
    rtx = (mx - g_engine.view_dst.x) / g_engine.view_dst.width *
          (float)g_engine.view_w;
    rty = (my - g_engine.view_dst.y) / g_engine.view_dst.height *
          (float)g_engine.view_h;
    *wx = g_engine.cam_x + (rtx - g_engine.view_w * 0.5f) / g_engine.cam_zoom;
    *wy = g_engine.cam_y + (rty - g_engine.view_h * 0.5f) / g_engine.cam_zoom;
}

/* world-space rect a node occupies (doc size, runtime transform). Sprites
 * and bodies are centered on the node origin; TileMaps draw from the origin
 * toward +X/+Y like the runtime tile renderer. */
static int
engine_world_rect(const EngineNode *dn, Rectangle *out)
{
    Node *rn;

    if(dn == NULL || dn->runtime < 0)
        return 0;
    rn = NodeGet(&g_engine.scene, dn->runtime);
    if(rn == NULL || (rn->flags & NODE_FLAG_ALIVE) == 0)
        return 0;
    if(dn->kind == NODE_TILEMAP) {
        out->x = rn->world.position.x;
        out->y = rn->world.position.y;
        out->width = (float)(dn->map_w * dn->tile_px_w) * rn->world.scale.x;
        out->height = (float)(dn->map_h * dn->tile_px_h) * rn->world.scale.y;
        return 1;
    }
    out->x = rn->world.position.x - dn->w * 0.5f * rn->world.scale.x;
    out->y = rn->world.position.y - dn->h * 0.5f * rn->world.scale.y;
    out->width = dn->w * rn->world.scale.x;
    out->height = dn->h * rn->world.scale.y;
    return 1;
}

static EngineNode *
engine_pick(float wx, float wy)
{
    int i;

    for(i = g_engine.node_count - 1; i >= 0; i--) {
        EngineNode *dn = &g_engine.nodes[i];
        Rectangle r;

        if(!dn->used || dn->collected)
            continue;
        if(dn->kind != NODE_TILEMAP && (dn->w <= 0.0f || dn->h <= 0.0f))
            continue;
        if(!engine_world_rect(dn, &r) || r.width <= 0.0f || r.height <= 0.0f)
            continue;
        if(wx >= r.x && wx <= r.x + r.width && wy >= r.y && wy <= r.y + r.height)
            return dn;
    }
    return NULL;
}

static void
engine_draw_placeholder(const EngineNode *dn)
{
    Rectangle r;
    Color c = engine_node_color(dn);
    Node *rn;

    if(dn->runtime < 0 || dn->collected)
        return;
    rn = NodeGet(&g_engine.scene, dn->runtime);
    if(rn == NULL || (rn->flags & NODE_FLAG_ALIVE) == 0)
        return;
    if(dn->kind == NODE_TILEMAP) {
        /* draw filled cells when there is no tileset texture */
        if(dn->asset[0] != '\0')
            return;
        {
            int tx, ty;

            for(ty = 0; ty < dn->map_h; ty++) {
                for(tx = 0; tx < dn->map_w; tx++) {
                    int id = dn->tiles[ty * dn->map_w + tx];
                    Rectangle cell;

                    if(id <= 0)
                        continue;
                    cell.x = rn->world.position.x + (float)(tx * dn->tile_px_w);
                    cell.y = rn->world.position.y + (float)(ty * dn->tile_px_h);
                    cell.width = (float)dn->tile_px_w;
                    cell.height = (float)dn->tile_px_h;
                    DrawRectangleRec(cell, (Color){ c.r, c.g, c.b, 150 });
                    DrawRectangleLinesEx(cell, 1.0f,
                                         (Color){ c.r, c.g, c.b, 220 });
                }
            }
        }
        return;
    }
    if((int)dn->kind == ENGINE_KIND_PARTICLES) {
        /* marker in edit mode; while playing the kind's own draw op
         * renders the live particles */
        if(g_engine.playing)
            return;
    }
    if(dn->kind == NODE_CAMERA2D || dn->kind == NODE_AUDIO_SOURCE ||
       dn->kind == NODE_TIMER ||
       (int)dn->kind == ENGINE_KIND_TIMER ||
       (int)dn->kind == ENGINE_KIND_PARTICLES) {
        /* marker: small diamond at the node origin */
        Vector2 p = rn->world.position;

        DrawLine((int)p.x - 8, (int)p.y, (int)p.x, (int)p.y - 8, c);
        DrawLine((int)p.x, (int)p.y - 8, (int)p.x + 8, (int)p.y, c);
        DrawLine((int)p.x + 8, (int)p.y, (int)p.x, (int)p.y + 8, c);
        DrawLine((int)p.x, (int)p.y + 8, (int)p.x - 8, (int)p.y, c);
        return;
    }
    if(dn->w <= 0.0f || dn->h <= 0.0f)
        return;
    if((dn->kind == NODE_SPRITE2D || dn->kind == NODE_ANIMATED_SPRITE2D) &&
       dn->asset[0] != '\0')
        return;  /* the real texture is drawn by SceneDraw */
    if(!engine_world_rect(dn, &r))
        return;
    DrawRectangleRec(r, (Color){ c.r, c.g, c.b, 110 });
    DrawRectangleLinesEx(r, 1.5f, c);
}

static void
engine_draw_grid(Camera2D cam)
{
    const float step = 32.0f;
    float half_w = (float)g_engine.view_w * 0.5f / cam.zoom;
    float half_h = (float)g_engine.view_h * 0.5f / cam.zoom;
    float x0 = cam.target.x - half_w, x1 = cam.target.x + half_w;
    float y0 = cam.target.y - half_h, y1 = cam.target.y + half_h;
    Color grid = GetThemeButton();
    Color axis = GetThemeLink();
    float x, y;

    for(x = floorf(x0 / step) * step; x <= x1; x += step)
        DrawLine((int)x, (int)y0, (int)x, (int)y1, grid);
    for(y = floorf(y0 / step) * step; y <= y1; y += step)
        DrawLine((int)x0, (int)y, (int)x1, (int)y, grid);
    DrawLine((int)x0, 0, (int)x1, 0, axis);
    DrawLine(0, (int)y0, 0, (int)y1, axis);
}

/* ---- 3D pass: kryon's 3D tier over the same scene document ---- */

static Model g_mesh_models[ENGINE_MESH_COUNT];
static int g_mesh_loaded[ENGINE_MESH_COUNT];

static Model engine_mesh_model(int kind);

/* external model cache (.obj/.gltf), keyed by resolved path */
#define ENGINE_MODEL_CACHE 8
static struct {
    char path[KRAIT_PATH_MAX];
    Model model;
    int loaded;
} g_model_cache[ENGINE_MODEL_CACHE];

static Model
engine_model_load(const char *path)
{
    int i;
    int slot = -1;

    for(i = 0; i < ENGINE_MODEL_CACHE; i++) {
        if(g_model_cache[i].loaded &&
           strcmp(g_model_cache[i].path, path) == 0)
            return g_model_cache[i].model;
        if(!g_model_cache[i].loaded && slot < 0)
            slot = i;
    }
    if(slot < 0) {
        UnloadModel(g_model_cache[0].model);
        slot = 0;
    }
    g_model_cache[slot].model = LoadModel(path);
    if(g_model_cache[slot].model.meshCount <= 0)
        return g_model_cache[slot].model;
    snprintf(g_model_cache[slot].path, sizeof(g_model_cache[slot].path),
             "%s", path);
    g_model_cache[slot].loaded = 1;
    return g_model_cache[slot].model;
}

static Model
engine_mesh_model_for(const EngineNode *dn)
{
    if(dn->mesh_kind == ENGINE_MESH_MODEL && dn->model_path[0] != '\0')
        return engine_model_load(engine_asset_abs(dn->model_path));
    return engine_mesh_model(dn->mesh_kind);
}

static Model
engine_mesh_model(int kind)
{
    Mesh mesh;

    if(kind < 0 || kind >= ENGINE_MESH_COUNT)
        kind = ENGINE_MESH_CUBE;
    if(kind != ENGINE_MESH_MODEL && g_mesh_loaded[kind])
        return g_mesh_models[kind];
    switch(kind) {
    case ENGINE_MESH_SPHERE:   mesh = GenMeshSphere(0.5f, 16, 16); break;
    case ENGINE_MESH_PLANE:    mesh = GenMeshPlane(1.0f, 1.0f, 1, 1); break;
    case ENGINE_MESH_CYLINDER: mesh = GenMeshCylinder(0.5f, 1.0f, 16); break;
    case ENGINE_MESH_CONE:     mesh = GenMeshCone(0.5f, 1.0f, 16); break;
    case ENGINE_MESH_TORUS:    mesh = GenMeshTorus(0.3f, 0.15f, 12, 16); break;
    default:                   mesh = GenMeshCube(1.0f, 1.0f, 1.0f); break;
    }
    g_mesh_models[kind] = LoadModelFromMesh(mesh);
    g_mesh_loaded[kind] = 1;
    return g_mesh_models[kind];
}

/* The 3D camera: the scene's Camera3D node while playing, the editor
 * orbit camera otherwise. */
static Camera3D
engine_camera3d(void)
{
    Camera3D cam;
    EngineNode *cam_node = NULL;
    int i;

    for(i = 0; i < g_engine.node_count; i++) {
        EngineNode *dn = &g_engine.nodes[i];

        if(dn->used && (int)dn->kind == ENGINE_KIND_CAMERA3D) {
            cam_node = dn;
            break;
        }
    }
    memset(&cam, 0, sizeof(cam));
    cam.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    cam.fovy = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    if(g_engine.playing && cam_node != NULL) {
        cam.position = (Vector3){ cam_node->x, cam_node->y, cam_node->z };
        cam.target = (Vector3){ cam_node->tx, cam_node->ty, cam_node->tz };
        return cam;
    }
    {
        float yaw = g_engine.orbit_yaw * 0.017453293f;
        float pitch = g_engine.orbit_pitch * 0.017453293f;
        Vector3 t = cam_node != NULL
            ? (Vector3){ cam_node->tx, cam_node->ty, cam_node->tz }
            : (Vector3){ 0.0f, 0.0f, 0.0f };

        cam.target = t;
        cam.position.x = t.x + g_engine.orbit_dist * cosf(pitch) * sinf(yaw);
        cam.position.y = t.y + g_engine.orbit_dist * sinf(pitch);
        cam.position.z = t.z + g_engine.orbit_dist * cosf(pitch) * cosf(yaw);
    }
    return cam;
}

static void
engine_render_3d(void)
{
    Camera3D cam = engine_camera3d();
    int i;

    BeginMode3D(cam);
    DrawGrid(20, 1.0f);
    for(i = 0; i < g_engine.node_count; i++) {
        EngineNode *dn = &g_engine.nodes[i];

        if(!dn->used || (int)dn->kind != ENGINE_KIND_MESH3D)
            continue;
        DrawModel(engine_mesh_model_for(dn),
                  (Vector3){ dn->r_x, dn->r_y, dn->r_z }, dn->scale3,
                  dn->tint.a != 0 ? dn->tint : WHITE);
    }
    EndMode3D();
}

static void
engine_ensure_rt(void)
{
    if(g_engine.rt_w == g_engine.view_w && g_engine.rt_h == g_engine.view_h)
        return;
    if(g_engine.rt_w > 0)
        UnloadRenderTexture(g_engine.rt);
    g_engine.rt = LoadRenderTexture(g_engine.view_w, g_engine.view_h);
    g_engine.rt_w = g_engine.view_w;
    g_engine.rt_h = g_engine.view_h;
}

static void
engine_render(void)
{
    int i;

    engine_ensure_rt();
    if(g_engine.rt_w <= 0)
        return;
    {
        UIFrameState saved = SaveUIFrameState();

        BeginTextureMode(g_engine.rt);
        ClearBackground(GetThemeBackground());
        if(engine_scene_is_3d()) {
            engine_render_3d();
        } else if(g_engine.playing && g_engine.scene.active_camera >= 0) {
            SceneDraw(&g_engine.scene);  /* the game camera drives rendering */
        } else {
            Camera2D cam = engine_editor_camera();

            BeginMode2D(cam);
            SceneDraw(&g_engine.scene);
            if(!g_engine.playing)
                engine_draw_grid(cam);
            for(i = 0; i < g_engine.node_count; i++) {
                if(g_engine.nodes[i].used)
                    engine_draw_placeholder(&g_engine.nodes[i]);
            }
            if(!g_engine.playing) {
                EngineNode *sel = engine_node_by_id(g_engine.selected);
                Rectangle r;

                if(sel != NULL && sel->w > 0.0f && sel->h > 0.0f &&
                   engine_world_rect(sel, &r)) {
                    DrawRectangleLinesEx((Rectangle){ r.x - 2, r.y - 2,
                                                     r.width + 4, r.height + 4 },
                                         2.0f, GetThemeLink());
                }
            }
            EndMode2D();
        }
        /* HUD */
        {
            const char *mode = g_engine.playing
                ? (g_engine.paused ? "PAUSED" : "PLAYING") : "EDIT";
            char hud[128];
            int y = g_engine.view_h - ScaleUIPx(20);

            snprintf(hud, sizeof(hud), "%s  %02d:%04.1f  score %d  %d nodes  %s",
                     mode, (int)(g_engine.sim_time / 60.0),
                     fmod(g_engine.sim_time, 60.0), g_engine.score,
                     krait_engine_node_count(),
                     g_engine.has_scene_path ? ENGINE_SCENE_FILE : "unsaved scene");
            {
                int tw = MeasureText(hud, ScaleUIPx(11));

                DrawRectangleRec((Rectangle){ 0, (float)y - ScaleUIPx(3),
                                              (float)(tw + ScaleUIPx(16)),
                                              (float)(ScaleUIPx(17)) },
                                 (Color){ 0, 0, 0, 140 });
            }
            Text(hud, ScaleUIPx(8), y, ScaleUIPx(11), GetThemeIcon());
            if(g_engine.won) {
                const char *msg = "LEVEL COMPLETE";
                int mw = MeasureText(msg, ScaleUIPx(22));

                Text(msg, (g_engine.view_w - mw) / 2,
                     g_engine.view_h / 2 - ScaleUIPx(16), ScaleUIPx(22),
                     GetThemeText());
            }
        }
        EndTextureMode();
        RestoreUIFrameState(saved);
    }
}

/* ---- inspector write-back helpers (edit the built scene in place) ---- */

static void
engine_write_back_transform(EngineNode *dn)
{
    if(dn == NULL || dn->runtime < 0)
        return;
    NodeSetPosition(&g_engine.scene, dn->runtime, dn->x, dn->y);
    NodeSetRotation(&g_engine.scene, dn->runtime, dn->rot * 0.017453293f);
    NodeSetScale(&g_engine.scene, dn->runtime, dn->sx, dn->sy);
}

static void
engine_write_back_shape(EngineNode *dn)
{
    Node *rn;
    CollisionShape2DProps *sp;

    if(dn == NULL || dn->shape_runtime < 0)
        return;
    rn = NodeGet(&g_engine.scene, dn->shape_runtime);
    if(rn == NULL)
        return;
    sp = (CollisionShape2DProps *)rn->props;
    if(sp == NULL)
        return;
    sp->size.x = dn->w;
    sp->size.y = dn->h;
    sp->shape_kind = dn->shape_circle ? KRY_SHAPE2D_CIRCLE : KRY_SHAPE2D_BOX;
}

static void
engine_write_back_sprite(EngineNode *dn)
{
    Node *rn;
    Sprite2DProps *p;

    if(dn == NULL || dn->runtime < 0)
        return;
    rn = NodeGet(&g_engine.scene, dn->runtime);
    if(rn == NULL)
        return;
    p = (Sprite2DProps *)rn->props;
    if(p == NULL)
        return;
    p->size.x = dn->w;
    p->size.y = dn->h;
    if(dn->tint.a != 0)
        p->tint = dn->tint;
}

static void
engine_write_back_anim(EngineNode *dn)
{
    Node *rn;
    AnimatedSprite2DProps *p;

    if(dn == NULL || dn->runtime < 0)
        return;
    rn = NodeGet(&g_engine.scene, dn->runtime);
    if(rn == NULL)
        return;
    p = (AnimatedSprite2DProps *)rn->props;
    if(p == NULL)
        return;
    p->size.x = dn->w;
    p->size.y = dn->h;
    p->fps = dn->fps;
}

/* ---- panels ---- */

/* Resize a TileMap grid, preserving the overlapping tile data. */
static void
engine_tilemap_resize(EngineNode *dn, int new_w, int new_h)
{
    int old_tiles[ENGINE_TILES_MAX];
    int ox, oy;

    if(new_w < 1)
        new_w = 1;
    if(new_h < 1)
        new_h = 1;
    if(new_w > 32)
        new_w = 32;
    if(new_h > 32)
        new_h = 32;
    if(new_w == dn->map_w && new_h == dn->map_h)
        return;
    memcpy(old_tiles, dn->tiles, sizeof(old_tiles));
    memset(dn->tiles, 0, sizeof(dn->tiles));
    for(oy = 0; oy < dn->map_h && oy < new_h; oy++) {
        for(ox = 0; ox < dn->map_w && ox < new_w; ox++)
            dn->tiles[oy * new_w + ox] = old_tiles[oy * dn->map_w + ox];
    }
    dn->map_w = new_w;
    dn->map_h = new_h;
    dn->w = (float)(new_w * dn->tile_px_w);
    dn->h = (float)(new_h * dn->tile_px_h);
    g_engine.dirty = 1;
}

int
krait_engine_timer_fired(int id)
{
    EngineNode *n = engine_node_by_id(id);
    Node *rn;
    EngineTimerState *t;

    if(n == NULL || n->runtime < 0)
        return -1;
    rn = NodeGet(&g_engine.scene, n->runtime);
    if(rn == NULL)
        return -1;
    t = (EngineTimerState *)rn->state;
    return t != NULL ? t->fired_count : -1;
}

int
krait_engine_node_tile(int id, int tx, int ty)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || n->kind != NODE_TILEMAP || tx < 0 || ty < 0 ||
       tx >= n->map_w || ty >= n->map_h)
        return -1;
    return n->tiles[ty * n->map_w + tx];
}

int
krait_engine_set_particles(int id, float rate, float lifetime, float speed,
                           float spread)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || (int)n->kind != ENGINE_KIND_PARTICLES)
        return 0;
    n->p_rate = rate;
    n->p_lifetime = lifetime;
    n->p_speed = speed;
    n->p_spread = spread;
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_particle_count(int id)
{
    EngineNode *n = engine_node_by_id(id);
    Node *rn;
    ParticlesState *ps;

    if(n == NULL || n->runtime < 0)
        return -1;
    rn = NodeGet(&g_engine.scene, n->runtime);
    if(rn == NULL)
        return -1;
    ps = (ParticlesState *)rn->state;
    return ps != NULL ? ps->count : -1;
}

int
krait_engine_set_3d(int id, float z, float rot_y, float scale)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || ((int)n->kind != ENGINE_KIND_NODE3D &&
                     (int)n->kind != ENGINE_KIND_MESH3D &&
                     (int)n->kind != ENGINE_KIND_CAMERA3D))
        return 0;
    n->z = z;
    n->rot_y3 = rot_y;
    n->scale3 = scale > 0.01f ? scale : 1.0f;
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_set_mesh(int id, int mesh_kind)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || (int)n->kind != ENGINE_KIND_MESH3D)
        return 0;
    if(mesh_kind < 0 || mesh_kind >= ENGINE_MESH_COUNT)
        return 0;
    n->mesh_kind = mesh_kind;
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_set_3d_target(int id, float tx, float ty, float tz)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || (int)n->kind != ENGINE_KIND_CAMERA3D)
        return 0;
    n->tx = tx;
    n->ty = ty;
    n->tz = tz;
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_get_3d(int id, float *z, float *rot_y, float *scale)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL)
        return 0;
    if(g_engine.runtime_ready)
        krait_engine_advance(0.0f);
    /* report the runtime pose (what the viewport renders) */
    if(z != NULL)
        *z = g_engine.runtime_ready ? n->r_z : n->z;
    if(rot_y != NULL)
        *rot_y = g_engine.runtime_ready ? n->r_rot_y : n->rot_y3;
    if(scale != NULL)
        *scale = n->scale3;
    return 1;
}

int
krait_engine_node_set_pos3(int id, float x, float y, float z)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || ((int)n->kind != ENGINE_KIND_NODE3D &&
                     (int)n->kind != ENGINE_KIND_MESH3D &&
                     (int)n->kind != ENGINE_KIND_CAMERA3D))
        return 0;
    n->x = x;
    n->y = y;
    n->z = z;
    n->r_x = x;
    n->r_y = y;
    n->r_z = z;
    g_engine.dirty = 1;
    return 1;
}

int
krait_engine_node_pos3(int id, float *x, float *y, float *z)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL)
        return 0;
    if(g_engine.runtime_ready)
        krait_engine_advance(0.0f);
    if(x != NULL)
        *x = g_engine.runtime_ready ? n->r_x : n->x;
    if(y != NULL)
        *y = g_engine.runtime_ready ? n->r_y : n->y;
    if(z != NULL)
        *z = g_engine.runtime_ready ? n->r_z : n->z;
    return 1;
}

int
krait_engine_get_mesh(int id)
{
    EngineNode *n = engine_node_by_id(id);

    if(n == NULL || (int)n->kind != ENGINE_KIND_MESH3D)
        return -1;
    return n->mesh_kind;
}

int
krait_engine_scene_is_3d(void)
{
    return engine_scene_is_3d();
}

int
krait_engine_score(void)
{
    return g_engine.score;
}

int
krait_engine_won(void)
{
    return g_engine.won;
}

static int
engine_stepper(int x, int y, const char *label, float *value, float step)
{
    int btn = ScaleUIPx(22);
    int row_h = ScaleUIPx(26);
    int base = x + ScaleUIPx(102);
    int changed = 0;
    char buf[32];

    Text(label, x, y + ScaleUIPx(6), ScaleUIPx(12), GetThemeIcon());
    snprintf(buf, sizeof(buf), "%.1f", *value);
    Text(buf, x + ScaleUIPx(58), y + ScaleUIPx(6), ScaleUIPx(12),
         GetThemeText());
    if(StyledButton(base, y, btn, row_h, "-1",
                    ButtonStyleSecondary, 0, NULL)) {
        *value -= step;
        changed = 1;
    }
    if(StyledButton(base + (btn + 2), y, btn, row_h, "+1",
                    ButtonStyleSecondary, 0, NULL)) {
        *value += step;
        changed = 1;
    }
    if(StyledButton(base + (btn + 2) * 2, y, btn, row_h, "-8",
                    ButtonStyleSecondary, 0, NULL)) {
        *value -= step * 8.0f;
        changed = 1;
    }
    if(StyledButton(base + (btn + 2) * 3, y, btn, row_h, "+8",
                    ButtonStyleSecondary, 0, NULL)) {
        *value += step * 8.0f;
        changed = 1;
    }
    return changed;
}

static int
engine_row_depth(const EngineNode *dn)
{
    int depth = 0;

    while(dn != NULL && dn->parent != 0) {
        dn = engine_node_by_id(dn->parent);
        depth++;
        if(depth > 16)
            break;
    }
    return depth;
}

static int
engine_draw_scene_tree(Rectangle zone)
{
    int row_h = ScaleUIPx(24);
    int y = (int)zone.y - g_engine.tree_scroll;
    int i;
    Vector2 mouse = GetMousePosition();
    int content_h;

    Text("Scene", (int)zone.x + ScaleUIPx(10), y + ScaleUIPx(4),
         ScaleUIPx(13), GetThemeText());
    y += row_h;
    for(i = 0; i < g_engine.node_count; i++) {
        EngineNode *dn = &g_engine.nodes[i];
        Rectangle row;
        int depth;

        if(!dn->used)
            continue;
        depth = engine_row_depth(dn);
        row = (Rectangle){ zone.x + ScaleUIPx(8) + ScaleUIPx(14) * depth,
                           (float)y,
                           zone.width - ScaleUIPx(16) - ScaleUIPx(14) * depth,
                           (float)row_h };
        if(row.y + row_h > zone.y && row.y < zone.y + zone.height) {
            int is_sel = dn->id == g_engine.selected;

            if(is_sel)
                DrawRectangleRec(row, GetThemeButtonHover());
            if(CheckCollisionPointRec(mouse, row) &&
               IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                g_engine.selected = dn->id;
            DrawRectangleRec((Rectangle){ row.x + ScaleUIPx(2),
                                          row.y + row_h * 0.5f - ScaleUIPx(4),
                                          ScaleUIPx(8), ScaleUIPx(8) },
                             engine_node_color(dn));
            Text(dn->name, (int)row.x + ScaleUIPx(16), (int)row.y + ScaleUIPx(5),
                 ScaleUIPx(12), GetThemeText());
        }
        y += row_h;
    }
    content_h = y + g_engine.tree_scroll - (int)zone.y;
    g_engine.tree_max_scroll = content_h - (int)zone.height;
    if(g_engine.tree_max_scroll < 0)
        g_engine.tree_max_scroll = 0;
    if(CheckCollisionPointRec(mouse, zone)) {
        float wheel = GetMouseWheelMove();

        if(wheel != 0.0f) {
            g_engine.tree_scroll += (int)(wheel * 40.0f);
            if(g_engine.tree_scroll < 0)
                g_engine.tree_scroll = 0;
            if(g_engine.tree_scroll > g_engine.tree_max_scroll)
                g_engine.tree_scroll = g_engine.tree_max_scroll;
        }
    }
    return content_h;
}

static int
engine_draw_palette(Rectangle panel, int y)
{
    int i;
    int col_w = (int)panel.width / 2 - ScaleUIPx(10);
    int rows;

    Text("Add Node", (int)panel.x + ScaleUIPx(10), y, ScaleUIPx(13),
         GetThemeText());
    y += ScaleUIPx(22);
    for(i = 0; i < ENGINE_KIND_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;
        int bx = (int)panel.x + ScaleUIPx(8) + col * (col_w + ScaleUIPx(4));
        int by = y + row * (ScaleUIPx(28) + ScaleUIPx(4));

        if(StyledButton(bx, by, col_w, ScaleUIPx(28), g_kinds[i].label,
                        ButtonStyleSecondary, 0, NULL)) {
            krait_engine_add_node(i, NULL);
            engine_status("Added %s", g_kinds[i].label);
        }
    }
    rows = (ENGINE_KIND_COUNT + 1) / 2;
    y += rows * (ScaleUIPx(28) + ScaleUIPx(4)) + ScaleUIPx(8);
    if(StyledButton((int)panel.x + ScaleUIPx(8), y,
                    (int)panel.width - ScaleUIPx(16), ScaleUIPx(28),
                    "Delete Selected", ButtonStyleSecondary,
                    g_engine.selected == 0, NULL)) {
        if(krait_engine_delete_selected())
            engine_status("Node deleted");
    }
    y += ScaleUIPx(28) + ScaleUIPx(8);
    return y;
}

static int
engine_draw_scene_settings(Rectangle panel, int y)
{
    float tmp;

    Text("Scene Settings", (int)panel.x + ScaleUIPx(10), y, ScaleUIPx(13),
         GetThemeText());
    y += ScaleUIPx(24);
    tmp = g_engine.gravity_y;
    if(engine_stepper((int)panel.x + ScaleUIPx(10), y, "Gravity Y", &tmp, 50.0f)) {
        g_engine.gravity_y = tmp;
        g_engine.dirty = 1;
    }
    y += ScaleUIPx(30);
    tmp = (float)g_engine.view_w;
    if(engine_stepper((int)panel.x + ScaleUIPx(10), y, "View W", &tmp, 20.0f)) {
        g_engine.view_w = (int)tmp;
        if(g_engine.view_w < 160)
            g_engine.view_w = 160;
        g_engine.dirty = 1;
    }
    y += ScaleUIPx(30);
    tmp = (float)g_engine.view_h;
    if(engine_stepper((int)panel.x + ScaleUIPx(10), y, "View H", &tmp, 20.0f)) {
        g_engine.view_h = (int)tmp;
        if(g_engine.view_h < 120)
            g_engine.view_h = 120;
        g_engine.dirty = 1;
    }
    y += ScaleUIPx(34);
    return y;
}

static void
engine_draw_inspector(Rectangle panel)
{
    EngineNode *dn = engine_node_by_id(g_engine.selected);
    int x = (int)panel.x + ScaleUIPx(10);
    int y = (int)panel.y + ScaleUIPx(4) - g_engine.insp_scroll;
    int w = (int)panel.width - ScaleUIPx(20);
    Vector2 mouse = GetMousePosition();

    Text("Inspector", x, y, ScaleUIPx(13), GetThemeText());
    y += ScaleUIPx(24);
    if(dn == NULL) {
        Text("No node selected", x, y, ScaleUIPx(12), GetThemeIcon());
        Text("Click a node in the Scene", x, y + ScaleUIPx(18), ScaleUIPx(12),
             GetThemeIcon());
        Text("tree or the viewport", x, y + ScaleUIPx(36), ScaleUIPx(12),
             GetThemeIcon());
        g_engine.insp_max_scroll = 0;
        return;
    }

    /* name */
    Text("Name", x, y, ScaleUIPx(12), GetThemeIcon());
    y += ScaleUIPx(16);
    TextField((TextFieldProps){
        (Rectangle){ (float)x, (float)y, (float)w, (float)ScaleUIPx(30) },
        dn->name, sizeof(dn->name), &g_engine.name_cursor,
        &g_engine.name_focused, (int)sizeof(dn->name) - 1,
        ScaleUIPx(13), 9101, engine_input_style(),
        (TextInputFilter){0}, NULL, NULL, 0, 0
    });
    if(dn->runtime >= 0) {
        Node *rn = NodeGet(&g_engine.scene, dn->runtime);

        if(rn != NULL)
            snprintf(rn->name, sizeof(rn->name), "%s", dn->name);
    }
    y += ScaleUIPx(38);
    Text(engine_kind_name(dn->kind), x, y, ScaleUIPx(11), GetThemeIcon());
    y += ScaleUIPx(20);

    /* transform */
    if(engine_stepper(x, y, "X", &dn->x, 1.0f))
        engine_write_back_transform(dn);
    y += ScaleUIPx(30);
    if(engine_stepper(x, y, "Y", &dn->y, 1.0f))
        engine_write_back_transform(dn);
    y += ScaleUIPx(30);
    if(engine_stepper(x, y, "Rotation", &dn->rot, 5.0f))
        engine_write_back_transform(dn);
    y += ScaleUIPx(30);
    if((int)dn->kind == ENGINE_KIND_NODE3D || (int)dn->kind == ENGINE_KIND_MESH3D ||
       (int)dn->kind == ENGINE_KIND_CAMERA3D) {
        if(engine_stepper(x, y, "Z", &dn->z, 0.25f))
            g_engine.dirty = 1;
        y += ScaleUIPx(30);
        if(engine_stepper(x, y, "Rot Y 3D", &dn->rot_y3, 5.0f))
            g_engine.dirty = 1;
        y += ScaleUIPx(30);
        if(engine_stepper(x, y, "Scale 3D", &dn->scale3, 0.1f)) {
            if(dn->scale3 < 0.05f)
                dn->scale3 = 0.05f;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(30);
    }
    if((int)dn->kind == ENGINE_KIND_CAMERA3D) {
        if(engine_stepper(x, y, "Target X", &dn->tx, 0.25f))
            g_engine.dirty = 1;
        y += ScaleUIPx(30);
        if(engine_stepper(x, y, "Target Y", &dn->ty, 0.25f))
            g_engine.dirty = 1;
        y += ScaleUIPx(30);
        if(engine_stepper(x, y, "Target Z", &dn->tz, 0.25f))
            g_engine.dirty = 1;
        y += ScaleUIPx(30);
    }
    if((int)dn->kind == ENGINE_KIND_MESH3D && dn->mesh_kind == ENGINE_MESH_MODEL) {
        Text("Model (.obj/.gltf)", x, y, ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(16);
        TextField((TextFieldProps){
            (Rectangle){ (float)x, (float)y, (float)w, (float)ScaleUIPx(30) },
            dn->model_path, sizeof(dn->model_path), &g_engine.asset_cursor,
            &g_engine.asset_focused, (int)sizeof(dn->model_path) - 1,
            ScaleUIPx(12), 9190, engine_input_style(),
            (TextInputFilter){0}, NULL, NULL, 0, 0
        });
        y += ScaleUIPx(38);
    }
    if((int)dn->kind == ENGINE_KIND_MESH3D) {
        int sel = dn->mesh_kind;

        Text("Mesh", x, y, ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(16);
        Dropdown(9180, x, y, w - ScaleUIPx(8), ScaleUIPx(28),
                 g_mesh3d_names, ENGINE_MESH_COUNT, &sel);
        if(sel != dn->mesh_kind) {
            dn->mesh_kind = sel;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(38);
    }
    if(engine_stepper(x, y, "Scale X", &dn->sx, 0.1f))
        engine_write_back_transform(dn);
    y += ScaleUIPx(30);
    if(engine_stepper(x, y, "Scale Y", &dn->sy, 0.1f))
        engine_write_back_transform(dn);
    y += ScaleUIPx(36);

    /* behavior (registry: builtins plus any registered plugin behaviors) */
    Text("Behavior", x, y, ScaleUIPx(12), GetThemeIcon());
    y += ScaleUIPx(16);
    {
        const char *labels[ENGINE_BEHAVIOR_MAX + 1];
        int count = 1;
        int sel = 0;
        int bi;
        const KraitBehaviorDef *cur;

        labels[0] = "None";
        for(bi = 0; bi < g_behavior_count && count <= ENGINE_BEHAVIOR_MAX;
            bi++) {
            if(g_behaviors[bi].id[0] == '\0' ||
               strcmp(g_behaviors[bi].id, "none") == 0)
                continue;
            labels[count] = g_behaviors[bi].label;
            if(strcmp(g_behaviors[bi].id, dn->behavior_id) == 0)
                sel = count;
            count++;
        }
        if(dn->behavior_id[0] != '\0' && sel == 0) {
            /* unknown behavior id: show it as an extra entry */
            labels[count] = dn->behavior_id;
            sel = count;
            count++;
        }
        Dropdown(9103, x, y, w - ScaleUIPx(8), ScaleUIPx(28), labels, count,
                 &sel);
        if(sel == 0) {
            if(dn->behavior_id[0] != '\0')
                krait_engine_set_behavior_id(dn->id, "");
        } else {
            const char *picked_id = NULL;
            int seen = 1;

            for(bi = 0; bi < g_behavior_count; bi++) {
                if(g_behaviors[bi].id[0] == '\0' ||
                   strcmp(g_behaviors[bi].id, "none") == 0)
                    continue;
                if(seen == sel) {
                    picked_id = g_behaviors[bi].id;
                    break;
                }
                seen++;
            }
            if(picked_id != NULL &&
               strcmp(picked_id, dn->behavior_id) != 0)
                krait_engine_set_behavior_id(dn->id, picked_id);
        }
        y += ScaleUIPx(30);
        cur = engine_behavior_by_id(dn->behavior_id);
        if(cur != NULL) {
            int pi;

            for(pi = 0; pi < cur->param_count; pi++) {
                if(engine_stepper(x, y, cur->param_names[pi],
                                  &dn->behavior_params[pi], 1.0f))
                    g_engine.dirty = 0;   /* runtime reads doc params live */
                y += ScaleUIPx(28);
            }
        }
    }
    y += ScaleUIPx(8);

    /* kind-specific fields */
    if(dn->kind == NODE_SPRITE2D || dn->kind == NODE_ANIMATED_SPRITE2D ||
       dn->kind == NODE_BODY2D || dn->kind == NODE_AREA2D) {
        if(engine_stepper(x, y, "Width", &dn->w, 1.0f)) {
            engine_write_back_sprite(dn);
            engine_write_back_shape(dn);
            if(dn->kind == NODE_ANIMATED_SPRITE2D)
                engine_write_back_anim(dn);
        }
        y += ScaleUIPx(30);
        if(engine_stepper(x, y, "Height", &dn->h, 1.0f)) {
            engine_write_back_sprite(dn);
            engine_write_back_shape(dn);
            if(dn->kind == NODE_ANIMATED_SPRITE2D)
                engine_write_back_anim(dn);
        }
        y += ScaleUIPx(30);
    }
    if(dn->kind == NODE_SPRITE2D || dn->kind == NODE_ANIMATED_SPRITE2D ||
       dn->kind == NODE_AUDIO_SOURCE) {
        Text("Asset (relative to project)", x, y, ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(16);
        TextField((TextFieldProps){
            (Rectangle){ (float)x, (float)y, (float)w, (float)ScaleUIPx(30) },
            dn->asset, sizeof(dn->asset), &g_engine.asset_cursor,
            &g_engine.asset_focused, (int)sizeof(dn->asset) - 1,
            ScaleUIPx(12), 9102, engine_input_style(),
            (TextInputFilter){0}, NULL, NULL, 0, 0
        });
        y += ScaleUIPx(38);
    }
    if(dn->kind == NODE_SPRITE2D || dn->kind == NODE_ANIMATED_SPRITE2D) {
        int r, g, b, a;

        if(dn->tint.a == 0)
            dn->tint = engine_node_color(dn);
        r = dn->tint.r;
        g = dn->tint.g;
        b = dn->tint.b;
        a = dn->tint.a;
        Text("Tint", x, y, ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(18);
        Slider(9111, x, y, w - ScaleUIPx(8), "R", 0, 255, &r, "", NULL);
        y += ScaleUIPx(30);
        Slider(9112, x, y, w - ScaleUIPx(8), "G", 0, 255, &g, "", NULL);
        y += ScaleUIPx(30);
        Slider(9113, x, y, w - ScaleUIPx(8), "B", 0, 255, &b, "", NULL);
        y += ScaleUIPx(30);
        Slider(9114, x, y, w - ScaleUIPx(8), "A", 0, 255, &a, "", NULL);
        y += ScaleUIPx(30);
        dn->tint = (Color){ (unsigned char)r, (unsigned char)g,
                            (unsigned char)b, (unsigned char)a };
        engine_write_back_sprite(dn);
    }
    if(dn->kind == NODE_BODY2D) {
        static const char *body_opts[3] = { "Static", "Kinematic", "Dynamic" };
        int sel = dn->body_type;
        int v;

        Text("Body Type", x, y, ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(16);
        Dropdown(9104, x, y, w - ScaleUIPx(8), ScaleUIPx(28), body_opts, 3, &sel);
        if(sel != dn->body_type) {
            dn->body_type = sel;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(38);
        v = dn->fixed_rotation;
        Checkbox(9107, x, y, "Fixed rotation", &v);
        if(v != dn->fixed_rotation) {
            dn->fixed_rotation = v;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(28);
        v = dn->shape_circle;
        Checkbox(9109, x, y, "Circle shape", &v);
        if(v != dn->shape_circle) {
            dn->shape_circle = v;
            engine_write_back_shape(dn);
        }
        y += ScaleUIPx(28);
        {
            int grav = (int)(dn->gravity_scale * 100.0f);

            Slider(9116, x, y, w - ScaleUIPx(8), "Gravity x", -100, 300, &grav,
                   "", NULL);
            dn->gravity_scale = (float)grav / 100.0f;
        }
        y += ScaleUIPx(30);
    }
    if(dn->kind == NODE_AREA2D) {
        int v = dn->shape_circle;

        Checkbox(9109, x, y, "Circle shape", &v);
        if(v != dn->shape_circle) {
            dn->shape_circle = v;
            engine_write_back_shape(dn);
        }
        y += ScaleUIPx(28);
    }
    if(dn->kind == NODE_AREA2D || (int)dn->kind == ENGINE_KIND_TIMER) {
        int sel = dn->trigger;

        Text("Trigger", x, y, ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(16);
        Dropdown(9120, x, y, w - ScaleUIPx(8), ScaleUIPx(28),
                 g_trigger_names, ENGINE_TRIGGER_COUNT, &sel);
        if(sel != dn->trigger) {
            dn->trigger = sel;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(38);
    }
    if(dn->kind == NODE_TILEMAP) {
        float tmp;

        Text("Tileset asset (paint left/erase right in viewport)",
             x, y, ScaleUIPx(11), GetThemeIcon());
        y += ScaleUIPx(16);
        TextField((TextFieldProps){
            (Rectangle){ (float)x, (float)y, (float)w, (float)ScaleUIPx(30) },
            dn->asset, sizeof(dn->asset), &g_engine.asset_cursor,
            &g_engine.asset_focused, (int)sizeof(dn->asset) - 1,
            ScaleUIPx(12), 9121, engine_input_style(),
            (TextInputFilter){0}, NULL, NULL, 0, 0
        });
        y += ScaleUIPx(38);
        tmp = (float)dn->map_w;
        if(engine_stepper(x, y, "Map W", &tmp, 1.0f))
            engine_tilemap_resize(dn, (int)tmp, dn->map_h);
        y += ScaleUIPx(30);
        tmp = (float)dn->map_h;
        if(engine_stepper(x, y, "Map H", &tmp, 1.0f))
            engine_tilemap_resize(dn, dn->map_w, (int)tmp);
        y += ScaleUIPx(30);
        tmp = (float)dn->tile_px_w;
        if(engine_stepper(x, y, "Tile Px W", &tmp, 4.0f)) {
            dn->tile_px_w = (int)tmp > 4 ? (int)tmp : 4;
            dn->w = (float)(dn->map_w * dn->tile_px_w);
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(30);
        tmp = (float)dn->tile_px_h;
        if(engine_stepper(x, y, "Tile Px H", &tmp, 4.0f)) {
            dn->tile_px_h = (int)tmp > 4 ? (int)tmp : 4;
            dn->h = (float)(dn->map_h * dn->tile_px_h);
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(30);
        if(StyledButton(x, y, ScaleUIPx(72), ScaleUIPx(26), "Fill",
                        ButtonStyleSecondary, 0, NULL)) {
            int t, count = dn->map_w * dn->map_h;

            for(t = 0; t < count && t < ENGINE_TILES_MAX; t++)
                dn->tiles[t] = 1;
        }
        if(StyledButton(x + ScaleUIPx(80), y, ScaleUIPx(72), ScaleUIPx(26),
                        "Clear", ButtonStyleSecondary, 0, NULL)) {
            memset(dn->tiles, 0, sizeof(dn->tiles));
        }
        y += ScaleUIPx(32);
    }
    if(dn->kind == NODE_TIMER) {
        const char *targets[ENGINE_NODES_MAX + 1];
        int target_count = 0;
        int i;
        int t;
        int v;
        float tmpf;

        /* track targets are document node names */
        for(i = 0; i < g_engine.node_count && target_count < ENGINE_NODES_MAX;
            i++) {
            if(g_engine.nodes[i].used &&
               g_engine.nodes[i].kind != NODE_TIMER)
                targets[target_count++] = g_engine.nodes[i].name;
        }
        Text("Animation", x, y, ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(16);
        v = dn->anim_loop;
        Checkbox(9130, x, y, "Loop", &v);
        if(v != dn->anim_loop) {
            dn->anim_loop = v;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(26);
        v = dn->anim_autoplay;
        Checkbox(9131, x, y, "Autoplay", &v);
        if(v != dn->anim_autoplay) {
            dn->anim_autoplay = v;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(30);
        tmpf = (float)dn->anim_track_count;
        if(engine_stepper(x, y, "Tracks", &tmpf, 1.0f)) {
            int want = (int)tmpf;

            if(want < 0)
                want = 0;
            if(want > ENGINE_ANIM_TRACKS)
                want = ENGINE_ANIM_TRACKS;
            if(want > dn->anim_track_count) {
                int nt = dn->anim_track_count;

                snprintf(dn->anim_tracks[nt].target,
                         sizeof(dn->anim_tracks[nt].target), "%s",
                         target_count > 0 ? targets[0] : "Player");
                dn->anim_tracks[nt].property = ENGINE_ANIM_POS_X;
                dn->anim_tracks[nt].key_count = 2;
                dn->anim_tracks[nt].keys[0][0] = 0.0f;
                dn->anim_tracks[nt].keys[0][1] = 0.0f;
                dn->anim_tracks[nt].keys[1][0] = 1.0f;
                dn->anim_tracks[nt].keys[1][1] = 100.0f;
            }
            dn->anim_track_count = want;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(32);
        for(t = 0; t < dn->anim_track_count && t < ENGINE_ANIM_TRACKS; t++) {
            char lbl[48];
            int sel = 0;
            int k;

            snprintf(lbl, sizeof(lbl), "Track %d", t + 1);
            Text(lbl, x, y, ScaleUIPx(12), GetThemeText());
            y += ScaleUIPx(16);
            if(target_count > 0) {
                int ti;

                for(ti = 0; ti < target_count; ti++) {
                    if(strcmp(targets[ti], dn->anim_tracks[t].target) == 0) {
                        sel = ti;
                        break;
                    }
                }
                Dropdown(9140 + t * 2, x, y, w - ScaleUIPx(8), ScaleUIPx(28),
                         targets, target_count, &sel);
                if(sel >= 0 && sel < target_count)
                    snprintf(dn->anim_tracks[t].target,
                             sizeof(dn->anim_tracks[t].target), "%s",
                             targets[sel]);
            }
            y += ScaleUIPx(30);
            sel = dn->anim_tracks[t].property;
            Dropdown(9141 + t * 2, x, y, w - ScaleUIPx(8), ScaleUIPx(28),
                     g_anim_prop_names, ENGINE_ANIM_PROPERTY_COUNT, &sel);
            if(sel != dn->anim_tracks[t].property) {
                dn->anim_tracks[t].property = sel;
                g_engine.dirty = 1;
            }
            y += ScaleUIPx(30);
            for(k = 0; k < dn->anim_tracks[t].key_count &&
                       k < ENGINE_ANIM_KEYS; k++) {
                snprintf(lbl, sizeof(lbl), "K%d t", k);
                if(engine_stepper(x, y, lbl, &dn->anim_tracks[t].keys[k][0],
                                  0.1f))
                    g_engine.dirty = 1;
                y += ScaleUIPx(28);
                snprintf(lbl, sizeof(lbl), "K%d v", k);
                if(engine_stepper(x, y, lbl, &dn->anim_tracks[t].keys[k][1],
                                  1.0f))
                    g_engine.dirty = 1;
                y += ScaleUIPx(28);
            }
            if(dn->anim_tracks[t].key_count < ENGINE_ANIM_KEYS) {
                if(StyledButton(x, y, ScaleUIPx(72), ScaleUIPx(24), "+ Key",
                                ButtonStyleSecondary, 0, NULL)) {
                    int kk = dn->anim_tracks[t].key_count;
                    float last_t = kk > 0 ? dn->anim_tracks[t].keys[kk - 1][0]
                                          : 0.0f;

                    dn->anim_tracks[t].keys[kk][0] = last_t + 0.5f;
                    dn->anim_tracks[t].keys[kk][1] = 0.0f;
                    dn->anim_tracks[t].key_count = kk + 1;
                    g_engine.dirty = 1;
                }
                y += ScaleUIPx(28);
            }
            if(dn->anim_tracks[t].key_count > 0) {
                if(StyledButton(x, y, ScaleUIPx(72), ScaleUIPx(24), "- Key",
                                ButtonStyleSecondary, 0, NULL)) {
                    dn->anim_tracks[t].key_count--;
                    g_engine.dirty = 1;
                }
                y += ScaleUIPx(28);
            }
            y += ScaleUIPx(8);
        }
    }
    if((int)dn->kind == ENGINE_KIND_PARTICLES) {
        int r, g, b, a;
        char buf[64];

        if(engine_stepper(x, y, "Rate /s", &dn->p_rate, 1.0f))
            g_engine.dirty = 1;
        y += ScaleUIPx(28);
        if(engine_stepper(x, y, "Lifetime", &dn->p_lifetime, 0.1f)) {
            if(dn->p_lifetime < 0.1f)
                dn->p_lifetime = 0.1f;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(28);
        if(engine_stepper(x, y, "Speed", &dn->p_speed, 4.0f))
            g_engine.dirty = 1;
        y += ScaleUIPx(28);
        if(engine_stepper(x, y, "Spread\260", &dn->p_spread, 10.0f)) {
            if(dn->p_spread < 0.0f)
                dn->p_spread = 0.0f;
            if(dn->p_spread > 360.0f)
                dn->p_spread = 360.0f;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(28);
        {
            int v = dn->p_emitting;

            Checkbox(9160, x, y, "Emitting", &v);
            if(v != dn->p_emitting) {
                dn->p_emitting = v;
                g_engine.dirty = 1;
            }
        }
        y += ScaleUIPx(28);
        snprintf(buf, sizeof(buf), "alive: %d", krait_engine_particle_count(dn->id));
        Text(buf, x, y + ScaleUIPx(4), ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(24);
        Text("Color over life", x, y, ScaleUIPx(12), GetThemeIcon());
        y += ScaleUIPx(16);
        r = dn->p_col_start.r;
        g = dn->p_col_start.g;
        b = dn->p_col_start.b;
        a = dn->p_col_start.a;
        Slider(9161, x, y, w - ScaleUIPx(8), "S R", 0, 255, &r, "", NULL);
        y += ScaleUIPx(26);
        Slider(9162, x, y, w - ScaleUIPx(8), "S G", 0, 255, &g, "", NULL);
        y += ScaleUIPx(26);
        Slider(9163, x, y, w - ScaleUIPx(8), "S B", 0, 255, &b, "", NULL);
        y += ScaleUIPx(26);
        Slider(9164, x, y, w - ScaleUIPx(8), "S A", 0, 255, &a, "", NULL);
        y += ScaleUIPx(26);
        dn->p_col_start = (Color){ (unsigned char)r, (unsigned char)g,
                                   (unsigned char)b, (unsigned char)a };
        r = dn->p_col_end.r;
        g = dn->p_col_end.g;
        b = dn->p_col_end.b;
        a = dn->p_col_end.a;
        Slider(9165, x, y, w - ScaleUIPx(8), "E R", 0, 255, &r, "", NULL);
        y += ScaleUIPx(26);
        Slider(9166, x, y, w - ScaleUIPx(8), "E G", 0, 255, &g, "", NULL);
        y += ScaleUIPx(26);
        Slider(9167, x, y, w - ScaleUIPx(8), "E B", 0, 255, &b, "", NULL);
        y += ScaleUIPx(26);
        Slider(9168, x, y, w - ScaleUIPx(8), "E A", 0, 255, &a, "", NULL);
        y += ScaleUIPx(26);
        dn->p_col_end = (Color){ (unsigned char)r, (unsigned char)g,
                                 (unsigned char)b, (unsigned char)a };
    }
    if((int)dn->kind == ENGINE_KIND_TIMER) {
        if(engine_stepper(x, y, "Wait (s)", &dn->wait_time, 0.1f))
            g_engine.dirty = 1;
        y += ScaleUIPx(30);
        {
            int v = dn->autostart;

            Checkbox(9122, x, y, "Autostart", &v);
            if(v != dn->autostart) {
                dn->autostart = v;
                g_engine.dirty = 1;
            }
        }
        y += ScaleUIPx(28);
        {
            int v = dn->loop;

            Checkbox(9123, x, y, "Loop", &v);
            if(v != dn->loop) {
                dn->loop = v;
                g_engine.dirty = 1;
            }
        }
        y += ScaleUIPx(28);
        {
            int fired = krait_engine_timer_fired(dn->id);
            char buf[64];

            snprintf(buf, sizeof(buf), "fired %d times", fired);
            Text(buf, x, y, ScaleUIPx(12), GetThemeIcon());
        }
        y += ScaleUIPx(20);
    }
    if(dn->kind == NODE_CAMERA2D) {
        int zoom = (int)(dn->cam_zoom * 100.0f);
        int v;

        Slider(9115, x, y, w - ScaleUIPx(8), "Zoom", 25, 400, &zoom, "", NULL);
        dn->cam_zoom = (float)zoom / 100.0f;
        y += ScaleUIPx(30);
        v = dn->cam_active;
        Toggle(9106, x, y, ScaleUIPx(76), ScaleUIPx(26), &v, "Off", "Active");
        if(v != dn->cam_active) {
            dn->cam_active = v;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(32);
    }
    if(dn->kind == NODE_ANIMATED_SPRITE2D) {
        if(engine_stepper(x, y, "FPS", &dn->fps, 1.0f))
            engine_write_back_anim(dn);
        y += ScaleUIPx(30);
    }
    if(dn->kind == NODE_AUDIO_SOURCE) {
        static const char *audio_opts[2] = { "Sound", "Music" };
        int sel = dn->audio_kind;
        int v;

        Dropdown(9105, x, y, w - ScaleUIPx(8), ScaleUIPx(28), audio_opts, 2, &sel);
        if(sel != dn->audio_kind) {
            dn->audio_kind = sel;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(38);
        {
            int vol = (int)(dn->volume * 100.0f);

            Slider(9117, x, y, w - ScaleUIPx(8), "Volume", 0, 100, &vol, "", NULL);
            dn->volume = (float)vol / 100.0f;
        }
        y += ScaleUIPx(30);
        v = dn->loop;
        Checkbox(9108, x, y, "Loop", &v);
        if(v != dn->loop) {
            dn->loop = v;
            g_engine.dirty = 1;
        }
        y += ScaleUIPx(28);
    }

    /* scene-authored script (kscript): runs every frame while playing */
    Text("Script (set/if/collect; runs while playing)", x, y, ScaleUIPx(12),
         GetThemeIcon());
    y += ScaleUIPx(16);
    {
        int changed;
        int script_cursor = 0;
        TextInputStyle style = engine_input_style();

            changed = TextArea((TextAreaProps){
                (Rectangle){ (float)x, (float)y, (float)w,
                             (float)ScaleUIPx(110) },
                dn->script, sizeof(dn->script), &script_cursor,
                &g_engine.asset_focused, &g_engine.script_scroll_dummy,
                (int)sizeof(dn->script) - 1, ScaleUIPx(12), ScaleUIPx(4),
                9191, "", SyntaxNone, style, (TextInputFilter){0}, NULL
            });
        (void)changed;
    }
    y += ScaleUIPx(114);
    g_engine.insp_max_scroll =
        (y + g_engine.insp_scroll) - (int)(panel.y + panel.height);
    if(g_engine.insp_max_scroll < 0)
        g_engine.insp_max_scroll = 0;
    if(CheckCollisionPointRec(mouse, panel)) {
        float wheel = GetMouseWheelMove();

        if(wheel != 0.0f) {
            g_engine.insp_scroll += (int)(wheel * 40.0f);
            if(g_engine.insp_scroll < 0)
                g_engine.insp_scroll = 0;
            if(g_engine.insp_scroll > g_engine.insp_max_scroll)
                g_engine.insp_scroll = g_engine.insp_max_scroll;
        }
    }
}

/* ---- timeline editor panel (selected AnimationPlayer) ---- */

static float
engine_anim_duration(const EngineNode *dn)
{
    float dur = 0.0f;
    int t, k;

    for(t = 0; t < dn->anim_track_count && t < ENGINE_ANIM_TRACKS; t++) {
        for(k = 0; k < dn->anim_tracks[t].key_count; k++) {
            if(dn->anim_tracks[t].keys[k][0] > dur)
                dur = dn->anim_tracks[t].keys[k][0];
        }
    }
    return dur > 0.0f ? dur : 1.0f;
}

static void
engine_draw_timeline(Rectangle panel, EngineNode *anim)
{
    Vector2 mouse = GetMousePosition();
    float dur = engine_anim_duration(anim);
    int pad = ScaleUIPx(10);
    int row_h = ScaleUIPx(26);
    int head_w = ScaleUIPx(140);
    float lane_x = panel.x + (float)head_w;
    float lane_w = panel.width - (float)head_w - (float)pad;
    float t2x = lane_x + lane_w;   /* time 0..dur -> lane_x..t2x */
    int y = (int)panel.y + ScaleUIPx(6);
    char buf[96];
    int t;

    DrawRectangleRec(panel, GetThemeSurface());
    DrawLine((int)panel.x, (int)panel.y, (int)(panel.x + panel.width),
             (int)panel.y, GetThemeButton());

    /* header: scrub time + duration + delete-key button */
    snprintf(buf, sizeof(buf), "%.2fs / %.2fs", g_engine.tl_time, dur);
    Text(buf, (int)panel.x + pad, y, ScaleUIPx(12), GetThemeText());
    if(StyledButton((int)(panel.x + panel.width) - ScaleUIPx(92), y - ScaleUIPx(2),
                    ScaleUIPx(84), ScaleUIPx(22), "Del Key",
                    ButtonStyleSecondary,
                    g_engine.tl_sel_track < 0 ? 1 : 0, NULL)) {
        krait_engine_anim_delete_key(anim->id, g_engine.tl_sel_track,
                                     g_engine.tl_sel_key);
        g_engine.tl_sel_track = -1;
        g_engine.tl_sel_key = -1;
    }
    y += ScaleUIPx(24);

    /* scrubber bar */
    {
        Rectangle bar = (Rectangle){ lane_x, (float)y, lane_w,
                                      (float)ScaleUIPx(18) };
        int i;
        float tick;

        DrawRectangleRec(bar, GetThemeBackground());
        for(i = 0; (float)i <= dur + 0.001f; i++) {
            int tx = (int)(lane_x + lane_w * ((float)i / dur));

            DrawLine(tx, (int)bar.y, tx, (int)(bar.y + bar.height),
                     GetThemeButton());
            snprintf(buf, sizeof(buf), "%d", i);
            Text(buf, tx + 2, (int)bar.y + ScaleUIPx(2), ScaleUIPx(10),
                 GetThemeIcon());
        }
        for(tick = 0.0f; tick <= dur; tick += 0.25f) {
            int tx = (int)(lane_x + lane_w * (tick / dur));

            DrawLine(tx, (int)(bar.y + bar.height * 0.6f), tx,
                     (int)(bar.y + bar.height), GetThemeButton());
        }
        if(CheckCollisionPointRec(mouse, bar)) {
            if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_engine.tl_drag = 1;
                g_engine.tl_time = (mouse.x - lane_x) / lane_w * dur;
            }
            MarkUICursor(MOUSE_CURSOR_RESIZE_EW);
        }
        if(g_engine.tl_drag == 1 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            g_engine.tl_time = (mouse.x - lane_x) / lane_w * dur;
            if(g_engine.tl_time < 0.0f)
                g_engine.tl_time = 0.0f;
            if(g_engine.tl_time > dur)
                g_engine.tl_time = dur;
            krait_engine_timeline_scrub(anim->id, g_engine.tl_time);
        }
        /* playhead */
        {
            int px = (int)(lane_x + lane_w * (g_engine.tl_time / dur));

            DrawLine(px, (int)bar.y - ScaleUIPx(4), px,
                     (int)(panel.y + panel.height) - ScaleUIPx(4),
                     GetThemeLink());
            DrawRectangleRec((Rectangle){ (float)px - 2, bar.y, 4.0f,
                                          bar.height }, GetThemeLink());
        }
    }
    y += ScaleUIPx(26);

    /* track rows */
    for(t = 0; t < anim->anim_track_count && t < ENGINE_ANIM_TRACKS; t++) {
        Rectangle row = (Rectangle){ panel.x, (float)y, panel.width,
                                      (float)row_h };
        int k;
        int hovered = CheckCollisionPointRec(mouse, row);

        snprintf(buf, sizeof(buf), "%s %s", anim->anim_tracks[t].target,
                 g_anim_prop_names[anim->anim_tracks[t].property]);
        if(hovered)
            DrawRectangleRec(row, GetThemeButton());
        Text(buf, (int)panel.x + pad, y + ScaleUIPx(6), ScaleUIPx(11),
             GetThemeText());
        /* per-track add key at the scrub time, sampling the target's
         * current authored value for this property */
        if(StyledButton((int)(panel.x + panel.width) - pad - ScaleUIPx(56),
                        y + ScaleUIPx(2), ScaleUIPx(56), ScaleUIPx(22),
                        "+ Key", ButtonStyleSecondary, 0, NULL)) {
            EngineNode *target =
                engine_node_by_name(anim->anim_tracks[t].target);
            float v = 0.0f;

            if(target != NULL) {
                switch(anim->anim_tracks[t].property) {
                case ENGINE_ANIM_POS_X: v = target->x; break;
                case ENGINE_ANIM_POS_Y: v = target->y; break;
                case ENGINE_ANIM_ROTATION: v = target->rot * 0.017453293f; break;
                case ENGINE_ANIM_SCALE_X: v = target->sx; break;
                default: v = target->sy; break;
                }
            }
            krait_engine_anim_add_key(anim->id, t, g_engine.tl_time, v);
            g_engine.tl_sel_track = t;
            g_engine.tl_sel_key =
                krait_engine_anim_key_count(anim->id, t) - 1;
        }
        /* lane background */
        DrawRectangleRec((Rectangle){ lane_x, (float)y + ScaleUIPx(2),
                                      lane_w, (float)(row_h - 4) },
                         GetThemeBackground());
        for(k = 0; k < anim->anim_tracks[t].key_count; k++) {
            float kt = anim->anim_tracks[t].keys[k][0];
            float kv = anim->anim_tracks[t].keys[k][1];
            Rectangle key = (Rectangle){
                lane_x + lane_w * (kt / dur) - ScaleUIPx(5),
                (float)y + ScaleUIPx(4), (float)ScaleUIPx(11),
                (float)(row_h - 8) };
            int is_sel = t == g_engine.tl_sel_track &&
                         k == g_engine.tl_sel_key;
            Color kc = is_sel ? GetThemeLink() : GetThemeText();

            DrawRectangleRec(key, (Color){ kc.r, kc.g, kc.b, 200 });
            if(CheckCollisionPointRec(mouse, key)) {
                if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    g_engine.tl_sel_track = t;
                    g_engine.tl_sel_key = k;
                    g_engine.tl_drag = 2;
                    g_engine.tl_key_grab_t = kt;
                    g_engine.tl_time = kt;
                    krait_engine_timeline_scrub(anim->id, kt);
                } else if(IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                    krait_engine_anim_delete_key(anim->id, t, k);
                    if(g_engine.tl_sel_track == t &&
                       g_engine.tl_sel_key == k) {
                        g_engine.tl_sel_track = -1;
                        g_engine.tl_sel_key = -1;
                    }
                }
            }
            if(is_sel && g_engine.tl_drag != 2)
                Text(TextFormat("%.1f", kv),
                     (int)(key.x + key.width) + ScaleUIPx(4),
                     (int)key.y, ScaleUIPx(10), GetThemeText());
        }
        y += row_h;
    }
    /* key drag: move the selected key in time */
    if(g_engine.tl_drag == 2 && IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
       g_engine.tl_sel_track >= 0) {
        float nt = (mouse.x - lane_x) / lane_w * dur;

        if(nt < 0.0f)
            nt = 0.0f;
        if(nt > dur + 5.0f)
            nt = dur + 5.0f;
        g_engine.tl_time = nt;
        krait_engine_anim_move_key(anim->id, g_engine.tl_sel_track,
                                   g_engine.tl_sel_key, nt);
        krait_engine_timeline_scrub(anim->id, nt);
    }
    if(!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        g_engine.tl_drag = 0;
}

/* ------------------------------------------------------------------ */
/* main entry: the whole mode UI                                       */
/* ------------------------------------------------------------------ */

void
krait_engine_draw_view(Rectangle bounds, IdeState *st)
{
    float dt = GetFrameTime();
    int toolbar_h = ScaleUIPx(38);
    int left_w = ScaleUIPx(228);
    int right_w = ScaleUIPx(268);
    Rectangle toolbar, left, right, view;
    Vector2 mouse = GetMousePosition();
    int y;
    char label[96];

    if(dt > 0.05f)
        dt = 0.05f;

    /* first use, or the user opened a different project */
    if(!g_engine.initialized) {
        krait_engine_reset(st != NULL && st->project.loaded != 0
                               ? st->project.path : NULL);
        g_engine.initialized = 1;
    } else if(st != NULL && st->project.loaded != 0 &&
              strcmp(st->project.path, g_engine.project_root) != 0) {
        krait_engine_reset(st->project.path);
    }
    /* verification hook: select a node by name (smoke screenshots) */
    {
        const char *want = getenv("KRAIT_ENGINE_SELECT");

        if(want != NULL && want[0] != '\0') {
            EngineNode *n = engine_node_by_name(want);

            if(n != NULL)
                g_engine.selected = n->id;
        }
    }

    krait_engine_advance(dt);
    /* one-time level scan for the project (toolbar button) */
    if(!g_engine.level_checked) {
        g_engine.level_checked = 1;
        g_engine.level_available =
            krait_level_project_has(g_engine.project_root);
    }
    if(krait_level_editor_active()) {
        krait_level_set_active(1, g_engine.project_root);
        krait_level_draw_view(bounds);
        return;
    }

    DrawRectangleRec(bounds, GetThemeBackground());

    {
        int timeline_h = 0;

        if(engine_node_by_id(g_engine.selected) != NULL &&
           engine_node_by_id(g_engine.selected)->kind == NODE_TIMER)
            timeline_h = ScaleUIPx(30) + ScaleUIPx(26) +
                         ScaleUIPx(26) * engine_node_by_id(g_engine.selected)->anim_track_count;
        toolbar = (Rectangle){ bounds.x, bounds.y, bounds.width,
                                (float)toolbar_h };
        left = (Rectangle){ bounds.x, bounds.y + (float)toolbar_h,
                            (float)left_w,
                            bounds.height - (float)toolbar_h };
        right = (Rectangle){ bounds.x + bounds.width - (float)right_w,
                             bounds.y + (float)toolbar_h, (float)right_w,
                             bounds.height - (float)toolbar_h };
        view = (Rectangle){ bounds.x + (float)left_w,
                            bounds.y + (float)toolbar_h,
                            bounds.width - (float)(left_w + right_w),
                            bounds.height - (float)toolbar_h -
                                (float)timeline_h };
        if(timeline_h > 0)
            g_engine.timeline_rect = (Rectangle){
                view.x, view.y + view.height, view.width,
                (float)timeline_h };
        else
            g_engine.timeline_rect = (Rectangle){ 0, 0, 0, 0 };
    }

    /* ---- toolbar ---- */
    DrawRectangleRec(toolbar, GetThemeSurface());
    y = (int)toolbar.y + (toolbar_h - ScaleUIPx(26)) / 2;
    {
        int x = (int)toolbar.x + ScaleUIPx(10);
        int bw = ScaleUIPx(56);

        snprintf(label, sizeof(label), "%s",
                 g_engine.playing && !g_engine.paused ? "Running" : "Play");
        if(StyledButton(x, y, bw, ScaleUIPx(26), label,
                        g_engine.playing && !g_engine.paused
                            ? ButtonStylePrimary : ButtonStyleSecondary,
                        0, NULL))
            krait_engine_play();
        x += bw + ScaleUIPx(6);
        if(StyledButton(x, y, bw, ScaleUIPx(26),
                        g_engine.paused ? "Resume" : "Pause",
                        ButtonStyleSecondary, !g_engine.playing, NULL))
            krait_engine_pause();
        x += bw + ScaleUIPx(6);
        if(StyledButton(x, y, bw, ScaleUIPx(26), "Stop",
                        ButtonStyleSecondary, !g_engine.playing, NULL))
            krait_engine_stop();
        x += bw + ScaleUIPx(18);
        if(StyledButton(x, y, ScaleUIPx(52), ScaleUIPx(26), "New",
                        ButtonStyleSecondary, 0, NULL)) {
            engine_doc_clear();
            snprintf(g_engine.name, sizeof(g_engine.name), "Main");
            g_engine.gravity_x = 0.0f;
            g_engine.gravity_y = 980.0f;
            g_engine.view_w = 640;
            g_engine.view_h = 360;
            engine_build();
            engine_status("New scene");
        }
        x += ScaleUIPx(52) + ScaleUIPx(6);
        if(StyledButton(x, y, ScaleUIPx(52), ScaleUIPx(26), "Save",
                        ButtonStyleSecondary, !g_engine.has_scene_path, NULL)) {
            if(krait_engine_save(g_engine.scene_path))
                engine_status("Saved %s", ENGINE_SCENE_FILE);
            else
                engine_status("Save failed");
        }
        x += ScaleUIPx(52) + ScaleUIPx(6);
        if(StyledButton(x, y, ScaleUIPx(52), ScaleUIPx(26), "Load",
                        ButtonStyleSecondary, !g_engine.has_scene_path, NULL)) {
            if(krait_engine_load(g_engine.scene_path)) {
                engine_build();
                engine_status("Loaded %s", ENGINE_SCENE_FILE);
            } else {
                engine_status("No %s in project", ENGINE_SCENE_FILE);
            }
        }
        x += ScaleUIPx(52) + ScaleUIPx(6);
        if(StyledButton(x, y, ScaleUIPx(64), ScaleUIPx(26), "Run Game",
                        ButtonStylePrimary, !g_engine.has_scene_path, NULL)) {
            if(!krait_engine_run_game())
                engine_status("Open a project to run the game");
        }
        x += ScaleUIPx(64) + ScaleUIPx(6);
        if(StyledButton(x, y, ScaleUIPx(60), ScaleUIPx(26), "Export",
                        ButtonStyleSecondary, !g_engine.has_scene_path, NULL))
            krait_engine_export_game(g_engine.status, sizeof(g_engine.status));
        x += ScaleUIPx(60) + ScaleUIPx(6);
        if(g_engine.level_available &&
           StyledButton(x, y, ScaleUIPx(64), ScaleUIPx(26), "Level",
                        krait_level_editor_active()
                            ? ButtonStylePrimary
                            : ButtonStyleSecondary,
                        0, NULL))
            krait_level_set_active(!krait_level_editor_active(),
                                   g_engine.project_root);
        if(g_engine.level_available)
            x += ScaleUIPx(64) + ScaleUIPx(6);
        x += ScaleUIPx(12);
        snprintf(label, sizeof(label), "%s%s", g_engine.name,
                 g_engine.dirty ? " *" : "");
        Text(label, x, y + ScaleUIPx(6), ScaleUIPx(13), GetThemeText());
        Text(g_engine.status,
             (int)(toolbar.x + toolbar.width) - ScaleUIPx(230),
             y + ScaleUIPx(6), ScaleUIPx(12), GetThemeIcon());
    }

    /* ---- left panel: tree (scrolling zone) + palette + scene settings ---- */
    DrawRectangleRec(left, GetThemeSurface());
    {
        int top = (int)left.y;
        int tree_zone_h = ScaleUIPx(28) + (g_engine.node_count + 1) *
                          ScaleUIPx(24) + ScaleUIPx(12);
        int max_tree_h = (int)(left.height * 45) / 100;
        Rectangle tree_zone;

        if(tree_zone_h > max_tree_h)
            tree_zone_h = max_tree_h;
        tree_zone = (Rectangle){ left.x, (float)top, left.width,
                                 (float)tree_zone_h };
        BeginScissorMode((int)tree_zone.x, (int)tree_zone.y,
                         (int)tree_zone.width, (int)tree_zone.height);
        engine_draw_scene_tree(tree_zone);
        EndScissorMode();
        y = top + tree_zone_h + ScaleUIPx(4);
        BeginScissorMode((int)left.x, y, (int)left.width,
                         (int)(left.y + left.height) - y);
        y = engine_draw_palette(left, y);
        y = engine_draw_scene_settings(left, y + ScaleUIPx(6));
        EndScissorMode();
    }

    /* ---- center: viewport ---- */
    {
        float scale = view.width / (float)g_engine.view_w;
        float vh = (float)g_engine.view_h * scale;

        if(vh > view.height) {
            vh = view.height;
            scale = vh / (float)g_engine.view_h;
        }
        g_engine.view_dst = (Rectangle){
            view.x + (view.width - (float)g_engine.view_w * scale) * 0.5f,
            view.y + (view.height - vh) * 0.5f,
            (float)g_engine.view_w * scale, vh
        };
        g_engine.view_valid = 1;
    }

    /* viewport input (edit mode: pick + drag + pan + zoom, or 3D orbit) */
    if(CheckCollisionPointRec(mouse, view)) {
        MarkUICursor(MOUSE_CURSOR_RESIZE_ALL);
        if(engine_scene_is_3d()) {
            float wheel = GetMouseWheelMove();

            if(wheel > 0.0f)
                g_engine.orbit_dist *= 0.9f;
            else if(wheel < 0.0f)
                g_engine.orbit_dist *= 1.1f;
            if(g_engine.orbit_dist < 1.0f)
                g_engine.orbit_dist = 1.0f;
            if(g_engine.orbit_dist > 60.0f)
                g_engine.orbit_dist = 60.0f;
            if(!g_engine.playing && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                Vector2 delta = GetMouseDelta();

                g_engine.orbit_yaw += delta.x * 0.4f;
                g_engine.orbit_pitch -= delta.y * 0.4f;
                if(g_engine.orbit_pitch < 5.0f)
                    g_engine.orbit_pitch = 5.0f;
                if(g_engine.orbit_pitch > 85.0f)
                    g_engine.orbit_pitch = 85.0f;
            }
        } else {
        {
            float wheel = GetMouseWheelMove();

            if(wheel > 0.0f)
                g_engine.cam_zoom *= 1.12f;
            else if(wheel < 0.0f)
                g_engine.cam_zoom *= 0.9f;
            if(g_engine.cam_zoom < 0.2f)
                g_engine.cam_zoom = 0.2f;
            if(g_engine.cam_zoom > 4.0f)
                g_engine.cam_zoom = 4.0f;
        }
        }
        if(!g_engine.playing && IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            Vector2 delta = GetMouseDelta();

            g_engine.cam_x -= delta.x / g_engine.cam_zoom;
            g_engine.cam_y -= delta.y / g_engine.cam_zoom;
        }
        if(!g_engine.playing) {
            /* Tile painting: with a TileMap selected, clicks inside it
             * paint (left) or erase (right) cells instead of reselecting. */
            {
                EngineNode *sel = engine_node_by_id(g_engine.selected);
                Rectangle tr;

                if(sel != NULL && sel->kind == NODE_TILEMAP &&
                   engine_world_rect(sel, &tr)) {
                    float wx, wy;
                    int pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
                                  IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
                    int down = IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
                               IsMouseButtonDown(MOUSE_BUTTON_RIGHT);

                    engine_screen_to_world(mouse.x, mouse.y, &wx, &wy);
                    if((pressed || (down && g_engine.painting)) &&
                       wx >= tr.x && wx <= tr.x + tr.width &&
                       wy >= tr.y && wy <= tr.y + tr.height) {
                        int cx = (int)((wx - tr.x) / (float)sel->tile_px_w);
                        int cy = (int)((wy - tr.y) / (float)sel->tile_px_h);
                        int erase = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);

                        if(cx >= 0 && cx < sel->map_w && cy >= 0 && cy < sel->map_h) {
                            sel->tiles[cy * sel->map_w + cx] = erase ? 0 : 1;
                            g_engine.painting = 1;
                            g_engine.dragging = 0;
                        }
                    }
                }
            }
            if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                float wx, wy;
                EngineNode *hit;

                engine_screen_to_world(mouse.x, mouse.y, &wx, &wy);
                hit = engine_pick(wx, wy);
                if(hit != NULL && hit->kind == NODE_TILEMAP &&
                   hit->id == g_engine.selected) {
                    /* painting handled above; keep the selection */
                } else {
                    g_engine.selected = hit != NULL ? hit->id : 0;
                }
                if(hit != NULL && hit->kind != NODE_TILEMAP) {
                    g_engine.dragging = 1;
                    g_engine.drag_grab_x = wx - hit->x;
                    g_engine.drag_grab_y = wy - hit->y;
                }
            }
            if(g_engine.dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                EngineNode *sel = engine_node_by_id(g_engine.selected);

                if(sel != NULL) {
                    float wx, wy;

                    engine_screen_to_world(mouse.x, mouse.y, &wx, &wy);
                    sel->x = wx - g_engine.drag_grab_x;
                    sel->y = wy - g_engine.drag_grab_y;
                    engine_write_back_transform(sel);
                }
            } else {
                g_engine.dragging = 0;
            }
            if(!IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
               !IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
                g_engine.painting = 0;
        }
    }

    engine_render();

    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    if(g_engine.rt_w > 0) {
        DrawTexturePro(g_engine.rt.texture,
                       (Rectangle){ 0, 0, (float)g_engine.rt_w, -(float)g_engine.rt_h },
                       g_engine.view_dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        DrawRectangleLinesEx(g_engine.view_dst, ScaleUIPx(2), GetThemeButton());
    } else {
        DrawRectangleRec(view, GetThemeBackground());
        Text("Game viewport", (int)view.x + ScaleUIPx(16),
             (int)view.y + ScaleUIPx(16), ScaleUIPx(14), GetThemeIcon());
    }
    EndScissorMode();

    /* ---- timeline panel (AnimationPlayer selected) ---- */
    if(g_engine.timeline_rect.width > 0.0f) {
        EngineNode *anim = engine_node_by_id(g_engine.selected);

        if(anim != NULL)
            engine_draw_timeline(g_engine.timeline_rect, anim);
    }

    /* ---- right panel: inspector ---- */
    DrawRectangleRec(right, GetThemeSurface());
    BeginScissorMode((int)right.x, (int)right.y, (int)right.width,
                     (int)right.height);
    engine_draw_inspector((Rectangle){ right.x, right.y + ScaleUIPx(6),
                                       right.width,
                                       right.height - ScaleUIPx(12) });
    EndScissorMode();
}

void
krait_engine_shutdown(void)
{
    int i;

    for(i = 0; i < ENGINE_MESH_COUNT; i++) {
        if(g_mesh_loaded[i]) {
            UnloadModel(g_mesh_models[i]);
            g_mesh_loaded[i] = 0;
        }
    }
    engine_teardown_runtime();
    krait_level_shutdown();
    if(g_engine.rt_w > 0) {
        UnloadRenderTexture(g_engine.rt);
        g_engine.rt_w = 0;
    }
    if(g_engine.audio_opened) {
        if(IsAudioDeviceReady())
            CloseAudioDevice();
        g_engine.audio_opened = 0;
    }
    g_engine.initialized = 0;
}
