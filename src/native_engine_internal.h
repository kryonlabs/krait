#ifndef KRAIT_NATIVE_ENGINE_INTERNAL_H
#define KRAIT_NATIVE_ENGINE_INTERNAL_H

/* Private engine document model shared between native_engine.c
 * (editor, runtime, serialization) and native_script.c (kscript). */

#include "kryon.h"
#include "native_internal.h"

#define ENGINE_NODES_MAX 256
#define ENGINE_NAME_MAX 64
#define ENGINE_ASSET_MAX 256
#define ENGINE_SCENE_FILE "game.scene"
#define ENGINE_TILES_MAX (32 * 32)
#define ENGINE_ANIM_TRACKS 4
#define ENGINE_ANIM_KEYS 8
#define ENGINE_SCRIPT_VARS_MAX 8
#define ENGINE_SCRIPT_NAME_MAX 24

/* AnimationPlayer track property ids (file format order). */
enum {
    ENGINE_ANIM_POS_X,
    ENGINE_ANIM_POS_Y,
    ENGINE_ANIM_ROTATION,
    ENGINE_ANIM_SCALE_X,
    ENGINE_ANIM_SCALE_Y,
    ENGINE_ANIM_PROPERTY_COUNT
};

static const char *g_anim_prop_names[ENGINE_ANIM_PROPERTY_COUNT] = {
    "Pos X", "Pos Y", "Rotation", "Scale X", "Scale Y"
};

static const char *g_anim_prop_ids[ENGINE_ANIM_PROPERTY_COUNT] = {
    "posx", "posy", "rot", "scalex", "scaley"
};

/*
 * Behavior registry: per-node programs applied while playing. The engine
 * registers its builtins (player, spin, patrol) through the same public
 * API games and tests use, so behaviors are a plugin surface - each def
 * carries up to ENGINE_BEHAVIOR_PARAMS named float parameters that the
 * inspector edits and game.scene serializes.
 */
#define ENGINE_BEHAVIOR_MAX 24
#define ENGINE_BEHAVIOR_PARAMS 4

typedef void (*KraitBehaviorFn)(Scene *scene, NodeId node, float dt,
                                const float *params, int param_count,
                                void *user);

typedef struct {
    char id[32];                          /* file format id; "" = none */
    char label[48];
    char param_names[ENGINE_BEHAVIOR_PARAMS][24];
    float param_defaults[ENGINE_BEHAVIOR_PARAMS];
    int param_count;
    KraitBehaviorFn fn;
    void *user;
} KraitBehaviorDef;

static KraitBehaviorDef g_behaviors[ENGINE_BEHAVIOR_MAX];
static int g_behavior_count;

static KraitBehaviorDef *engine_behavior_by_id(const char *id);
static void engine_behavior_player(Scene *scene, NodeId node, float dt,
                                   const float *params, int param_count,
                                   void *user);
static void engine_behavior_spin(Scene *scene, NodeId node, float dt,
                                 const float *params, int param_count,
                                 void *user);
static void engine_behavior_patrol(Scene *scene, NodeId node, float dt,
                                   const float *params, int param_count,
                                   void *user);
static void engine_behavior_player3d(Scene *scene, NodeId node, float dt,
                                     const float *params, int param_count,
                                     void *user);
static void engine_behavior_spin3d(Scene *scene, NodeId node, float dt,
                                   const float *params, int param_count,
                                   void *user);

/* Triggers: what an Area2D body_enter or a Timer timeout does. */
enum {
    ENGINE_TRIGGER_NONE,
    ENGINE_TRIGGER_COLLECT,
    ENGINE_TRIGGER_WIN,
    ENGINE_TRIGGER_SCORE,
    ENGINE_TRIGGER_COUNT
};

static const char *g_trigger_names[ENGINE_TRIGGER_COUNT] = {
    "None", "Collect", "Win", "Score"
};

static const char *g_trigger_ids[ENGINE_TRIGGER_COUNT] = {
    "none", "collect", "win", "score"
};

/* Markers in g_kinds for runtime-registered custom kinds. */
#define ENGINE_KIND_TIMER (-2)
#define ENGINE_KIND_PARTICLES (-3)
#define ENGINE_KIND_NODE3D (-4)
#define ENGINE_KIND_MESH3D (-5)
#define ENGINE_KIND_CAMERA3D (-6)

/* MeshInstance3D generated meshes (no assets needed). */
enum {
    ENGINE_MESH_CUBE,
    ENGINE_MESH_SPHERE,
    ENGINE_MESH_PLANE,
    ENGINE_MESH_CYLINDER,
    ENGINE_MESH_CONE,
    ENGINE_MESH_TORUS,
    ENGINE_MESH_MODEL,   /* external .obj/.gltf via model_path */
    ENGINE_MESH_COUNT
};

static const char *g_mesh3d_names[ENGINE_MESH_COUNT] = {
    "Cube", "Sphere", "Plane", "Cylinder", "Cone", "Torus", "Model"
};

/* Per-instance Particles2D runtime state (node->state). The emitter config
 * is baked in from the document at build time; the simulation is
 * deterministic (xorshift directions) so tests can assert counts. */
#define ENGINE_PARTICLES_MAX 256

typedef struct {
    float x, y;        /* world position */
    float vx, vy;
    float life;        /* seconds remaining */
    float max_life;
} EngineParticle;

typedef struct {
    /* emitter config (documented values) */
    float rate, lifetime, speed, spread;
    Color col_start, col_end;
    int emitting;
    /* simulation */
    EngineParticle parts[ENGINE_PARTICLES_MAX];
    int count;
    float spawn_acc;
    unsigned rng;
} ParticlesState;

/* Per-instance Timer runtime state (node->state); self-contained so the
 * process op never needs the document. */
typedef struct {
    float wait_time;
    int loop;
    int running;
    double elapsed;
    int fired_count;
} EngineTimerState;



static void engine_render(void);   /* editor + player viewport painter */

/* ------------------------------------------------------------------ */
typedef struct {
    const char *name;    /* canonical kind name (file format) */
    NodeKind kind;       /* kryon NodeKind value */
    const char *label;   /* palette label */
} EngineKindInfo;

static const EngineKindInfo g_kinds[] = {
    { "Node2D",           NODE_NODE2D,            "Node2D" },
    { "Sprite2D",         NODE_SPRITE2D,          "Sprite2D" },
    { "AnimatedSprite2D", NODE_ANIMATED_SPRITE2D, "AnimatedSprite2D" },
    { "Camera2D",         NODE_CAMERA2D,          "Camera2D" },
    { "Body2D",           NODE_BODY2D,            "Body2D" },
    { "Area2D",           NODE_AREA2D,            "Area2D" },
    { "TileMap",          NODE_TILEMAP,           "TileMap" },
    { "Timer",            (NodeKind)ENGINE_KIND_TIMER, "Timer" },
    { "AnimationPlayer",  NODE_TIMER,             "AnimationPlayer" },
    { "AudioSource",      NODE_AUDIO_SOURCE,      "AudioSource" },
    { "Particles2D",      (NodeKind)ENGINE_KIND_PARTICLES, "Particles2D" },
    { "Node3D",           (NodeKind)ENGINE_KIND_NODE3D,    "Node3D" },
    { "MeshInstance3D",   (NodeKind)ENGINE_KIND_MESH3D,    "MeshInstance3D" },
    { "Camera3D",         (NodeKind)ENGINE_KIND_CAMERA3D,  "Camera3D" },
};
#define ENGINE_KIND_COUNT ((int)(sizeof(g_kinds) / sizeof(g_kinds[0])))

typedef struct {
    int used;
    int id;              /* stable document id; 0 is never used */
    int parent;          /* parent document id, 0 = scene root */
    NodeKind kind;
    char name[ENGINE_NAME_MAX];
    /* transform (authored; rotation in degrees) */
    float x, y, rot, sx, sy;
    char behavior_id[32];                 /* registry id; "" = none */
    float behavior_params[ENGINE_BEHAVIOR_PARAMS];
    /* visual */
    char asset[ENGINE_ASSET_MAX];   /* relative to project root, or absolute */
    float w, h;                     /* sprite size / shape size */
    Color tint;                     /* .a == 0 -> kind default */
    /* Body2D / Area2D */
    int body_type;        /* 0 static, 1 kinematic, 2 dynamic */
    int fixed_rotation;
    float gravity_scale;
    int shape_circle;     /* 0 box, 1 circle */
    /* Camera2D */
    float cam_zoom;
    int cam_active;
    /* AnimatedSprite2D */
    int frame_count, frames_per_row, frame_w, frame_h;
    float fps;
    /* AudioSource */
    int audio_kind;       /* 0 sound, 1 music */
    float volume, pitch;
    int loop;
    /* TileMap */
    int map_w, map_h;         /* grid size in tiles (<= 32) */
    int tile_w, tile_h;       /* source tile size in the tileset */
    int tiles_per_row;
    int tile_px_w, tile_px_h; /* world-space draw size per tile */
    int tiles[ENGINE_TILES_MAX];
    /* Timer */
    float wait_time;
    int autostart;
    /* 3D nodes (Node3D family; x/y double as world x/y, z adds depth) */
    float z, rot_y3, scale3;
    int mesh_kind;         /* MeshInstance3D */
    float tx, ty, tz;      /* Camera3D look-at target */
    /* Particles2D */
    float p_rate, p_lifetime, p_speed, p_spread;
    Color p_col_start, p_col_end;
    int p_emitting;
    /* AnimationPlayer (kryon NODE_TIMER kind: kry_animation.h) */
    int anim_loop;
    int anim_autoplay;
    int anim_track_count;               /* <= ENGINE_ANIM_TRACKS */
    struct {
        char target[ENGINE_NAME_MAX];   /* document node name */
        int property;                   /* ENGINE_ANIM_POS_X .. */
        int key_count;                  /* <= ENGINE_ANIM_KEYS */
        float keys[ENGINE_ANIM_KEYS][2];/* { time, value } */
    } anim_tracks[ENGINE_ANIM_TRACKS];
    /* Area2D / Timer / AnimationPlayer */
    int trigger;
    /* runtime */
    NodeId runtime;       /* NodeId in the built Scene, -1 when absent */
    NodeId shape_runtime; /* auto child CollisionShape2D, -1 when absent */
    NodeId carrier_runtime; /* implicit static Body2D for standalone areas */
    int collected;        /* runtime-collected by a Collect trigger */
    /* behavior bases captured at Play */
    float base_x, base_y, base_rot;
    float b3_vy;            /* 3D behavior vertical velocity */
    /* runtime 3D pose: behaviors mutate this, never the document; a
     * rebuild (Stop) restores the authored transform */
    float r_x, r_y, r_z, r_rot_y;
    /* scene-authored script (kscript; see native_script.c) */
    char script[2048];
    char script_var_names[ENGINE_SCRIPT_VARS_MAX][ENGINE_SCRIPT_NAME_MAX];
    float script_vars[ENGINE_SCRIPT_VARS_MAX];
    int script_var_count;
    unsigned script_rng;
    /* MeshInstance3D external model asset (.obj/.gltf) */
    char model_path[ENGINE_ASSET_MAX];
} EngineNode;

typedef struct {
    int initialized;
    int builtins_registered;
    NodeKind particles_kind;  /* krait-registered custom kind */
    NodeKind node3d_kind, mesh3d_kind, camera3d_kind;
    /* 3D editor orbit camera */
    float orbit_yaw, orbit_pitch, orbit_dist;
    /* timeline editor (selected AnimationPlayer) */
    Rectangle timeline_rect;
    float tl_time;            /* scrubber position, seconds */
    int tl_sel_track, tl_sel_key;
    int tl_drag;              /* 0 none, 1 scrub, 2 key */
    float tl_key_grab_t;
    char project_root[KRAIT_PATH_MAX];
    char scene_path[KRAIT_PATH_MAX];
    int has_scene_path;
    /* document */
    char name[ENGINE_NAME_MAX];
    float gravity_x, gravity_y;
    int view_w, view_h;
    EngineNode nodes[ENGINE_NODES_MAX];
    int node_count;
    int next_id;
    int dirty;
    /* runtime scene */
    Scene scene;
    int runtime_ready;
    int physics_created;
    NodeId runtime_camera;
    int playing, paused;
    int level_checked;         /* level scan done for this project */
    int level_available;      /* project has levels/*.level */
    float physics_acc;
    double sim_time;
    int score, won;
    NodeKind timer_kind;    /* krait-registered custom kind */
    NodeId events_runtime;  /* hidden signal target under the root */
    /* editor */
    int selected;         /* document id, 0 = none */
    float cam_x, cam_y, cam_zoom;
    int dragging;
    int painting;
    float drag_grab_x, drag_grab_y;
    Rectangle view_dst;   /* letterboxed viewport rect in window space */
    int view_valid;
    /* render target */
    RenderTexture2D rt;
    int rt_w, rt_h;
    /* audio */
    int audio_opened;
    /* UI transient */
    char status[256];
    int name_cursor, name_focused;
    int asset_cursor, asset_focused;
    int tree_scroll, tree_max_scroll;
    int script_scroll_dummy;
    int insp_scroll, insp_max_scroll;
} EngineState;

static EngineState g_engine;

#endif /* KRAIT_NATIVE_ENGINE_INTERNAL_H */
