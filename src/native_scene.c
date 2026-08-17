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

typedef struct SceneNode {
    int id;
    int parent;
    int depth;
    int kind;
    int editable;
    char label[128];
    char asset_path[512];
    Rectangle bounds;
    int font_size;
    int layout_parent;
    int offset_x;
    int offset_y;
    int source_line;
    int source_start;
    int source_end;
} SceneNode;

enum {
    KRAIT_SCENE_NODE_UNKNOWN,
    KRAIT_SCENE_NODE_SCREEN,
    KRAIT_SCENE_NODE_GROUP,
    KRAIT_SCENE_NODE_TEXT,
    KRAIT_SCENE_NODE_RECTANGLE,
    KRAIT_SCENE_NODE_BUTTON,
    KRAIT_SCENE_NODE_TEXT_FIELD,
    KRAIT_SCENE_NODE_TOGGLE,
    KRAIT_SCENE_NODE_SLIDER,
    KRAIT_SCENE_NODE_SPRITE,
    KRAIT_SCENE_NODE_GAME_OBJECT
};

static SceneNode g_scene_nodes[KRAIT_SCENE_NODE_MAX];
static int g_scene_node_count;

int
krait_game_node_args(const char *q, char args[8][256], char *type,
                     size_t type_size, char *label, size_t label_size,
                     int *x, int *y, int *w, int *h)
{
    static const char *names[] = {
        "PlayerSpawn",
        "MobSpawn",
        "TerrainPlatform",
        "Pickup",
        "TriggerArea",
        "WeatherConfig",
        "StageExit",
        "CameraBounds",
        "WaterBlob",
        "Pond",
        "TreeSpawner",
        "CaveMushroom",
        "CaveSpider",
        "CaveTurtle"
    };

    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char *s;
        int argc = krait_live_call_args(q, names[i], args, 8);

        if(argc != 5)
            continue;
        s = args[0];
        if(!krait_live_parse_string(&s, label, label_size) ||
           !krait_live_eval_int(args[1], x) ||
           !krait_live_eval_int(args[2], y) ||
           !krait_live_eval_int(args[3], w) ||
           !krait_live_eval_int(args[4], h))
            return 0;
        snprintf(type, type_size, "%s", names[i]);
        return 1;
    }
    return 0;
}

static Color
krait_game_node_color(const char *type, int outline)
{
    if(strcmp(type, "PlayerSpawn") == 0)
        return outline ? (Color){80, 120, 210, 255} :
                         (Color){226, 234, 245, 255};
    if(strcmp(type, "MobSpawn") == 0 || strcmp(type, "CaveSpider") == 0 ||
       strcmp(type, "CaveTurtle") == 0)
        return outline ? (Color){230, 130, 90, 255} :
                         (Color){120, 66, 58, 255};
    if(strcmp(type, "Pickup") == 0 || strcmp(type, "WaterBlob") == 0 ||
       strcmp(type, "Pond") == 0)
        return outline ? (Color){135, 220, 245, 255} :
                         (Color){60, 120, 130, 255};
    if(strcmp(type, "TerrainPlatform") == 0)
        return outline ? (Color){145, 140, 160, 255} :
                         (Color){96, 94, 108, 255};
    if(strcmp(type, "TreeSpawner") == 0 ||
       strcmp(type, "CaveMushroom") == 0)
        return outline ? (Color){42, 94, 58, 255} :
                         (Color){91, 118, 76, 255};
    return outline ? (Color){230, 178, 83, 255} :
                     (Color){230, 178, 83, 96};
}

void
krait_draw_game_node(const char *type, const char *label,
                     int x, int y, int w, int h)
{
    Color fill = krait_game_node_color(type, 0);
    Color outline = krait_game_node_color(type, 1);

    Rect(x, y, w, h, fill, outline);
    Text(label != NULL && label[0] != '\0' ? label : type,
         x + ScaleUIPx(6), y + ScaleUIPx(6), UI_TEXT_12, outline);
}

static int
krait_scene_add(SceneNode *out, int count, int cap, int parent, int depth,
                int kind, const char *label, const char *asset_path,
                Rectangle bounds, int line, int source_start, int source_end)
{
    SceneNode *node;
    char id_src[KRAIT_PATH_MAX + 256];

    if(out == NULL || count >= cap)
        return count;
    node = &out[count];
    memset(node, 0, sizeof(*node));
    node->parent = parent;
    node->depth = depth;
    node->kind = kind;
    node->editable = kind != KRAIT_SCENE_NODE_UNKNOWN &&
                     kind != KRAIT_SCENE_NODE_SCREEN;
    snprintf(node->label, sizeof(node->label), "%s",
             label != NULL && label[0] != '\0' ? label : "Node");
    if(asset_path != NULL)
        snprintf(node->asset_path, sizeof(node->asset_path), "%s", asset_path);
    node->bounds = bounds;
    node->layout_parent = -1;
    node->offset_x = 0;
    node->offset_y = 0;
    /* If inside a GROUP container, compute offset from parent origin */
    if(parent >= 0 && parent < count && out[parent].kind == KRAIT_SCENE_NODE_GROUP) {
        node->layout_parent = parent;
        node->offset_x = (int)bounds.x - (int)out[parent].bounds.x;
        node->offset_y = (int)bounds.y - (int)out[parent].bounds.y;
    }
    node->source_line = line;
    node->source_start = source_start;
    node->source_end = source_end;
    snprintf(id_src, sizeof(id_src), "%d:%d:%s:%s", line, source_start,
             node->label, node->asset_path);
    node->id = krait_tree_id(id_src);
    return count + 1;
}

static int
krait_scene_rect_arg(const char *expr, Rectangle *out)
{
    char buf[512];
    char *scan;
    int x, y, w, h;

    if(expr == NULL || out == NULL)
        return 0;
    snprintf(buf, sizeof(buf), "%s", expr);
    scan = buf;
    if(!krait_live_next_scale_arg(&scan, &x) ||
       !krait_live_next_scale_arg(&scan, &y) ||
       !krait_live_next_scale_arg(&scan, &w) ||
       !krait_live_next_scale_arg(&scan, &h))
        return 0;
    *out = (Rectangle){x, y, w, h};
    return 1;
}

static int
krait_scene_scan_line(const char *line, SceneNode *out, int count, int cap,
                      int *stack, int *stack_depth, int source_line,
                      int source_start, int source_end)
{
    char buf[4096];
    char args[8][256];
    char label[128];
    char asset[512];
    char node_type[64];
    char *q;
    char *s;
    int argc;
    int x, y, w, h;
    int parent = *stack_depth > 0 ? stack[*stack_depth - 1] : -1;
    int depth = *stack_depth;
    Rectangle bounds = {0};

    if(line == NULL)
        return count;
    snprintf(buf, sizeof(buf), "%s", line);
    q = krait_live_trim(buf);
    if(q[0] == '\0' || q[0] == '#')
        return count;
    if(krait_live_starts_word(q, "if"))
        q = krait_live_trim(q + 2);
    argc = krait_live_call_args(q, "BeginNodeGroup", args, 8);
    if(argc >= 2) {
        if(!krait_scene_rect_arg(args[1], &bounds))
            bounds = (Rectangle){0, 0, 0, 0};
        count = krait_scene_add(out, count, cap, parent, depth,
                                KRAIT_SCENE_NODE_GROUP, "Group", NULL, bounds,
                                source_line, source_start, source_end);
        if(count > 0 && *stack_depth < 64)
            stack[(*stack_depth)++] = count - 1;
        return count;
    }
    if(strncmp(q, "EndNodeGroup", 14) == 0) {
        if(*stack_depth > 1)
            (*stack_depth)--;
        return count;
    }
    argc = krait_live_call_args(q, "Text", args, 8);
    if(argc == 5) {
        int text_w;
        int font_size = 0;

        s = args[0];
        label[0] = '\0';
        (void)krait_live_parse_string(&s, label, sizeof(label));
        if(!krait_live_eval_int(args[1], &x))
            x = 0;
        if(!krait_live_eval_int(args[2], &y))
            y = 0;
        (void)krait_live_eval_int(args[3], &font_size);
        text_w = (int)strlen(label) * ScaleUIPx(8);
        if(text_w < ScaleUIPx(24))
            text_w = ScaleUIPx(24);
        bounds = (Rectangle){x, y, text_w, ScaleUIPx(18)};
        count = krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_TEXT, label, NULL, bounds,
                               source_line, source_start, source_end);
        if(count > 0)
            out[count - 1].font_size = font_size;
        return count;
    }
    argc = krait_live_call_args(q, "DrawText", args, 8);
    if(argc == 5) {
        int text_w;
        int font_size = 0;

        s = args[0];
        label[0] = '\0';
        (void)krait_live_parse_string(&s, label, sizeof(label));
        if(!krait_live_eval_int(args[1], &x))
            x = 0;
        if(!krait_live_eval_int(args[2], &y))
            y = 0;
        (void)krait_live_eval_int(args[3], &font_size);
        text_w = (int)strlen(label) * ScaleUIPx(8);
        if(text_w < ScaleUIPx(24))
            text_w = ScaleUIPx(24);
        bounds = (Rectangle){x, y, text_w, ScaleUIPx(18)};
        count = krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_TEXT, label, NULL, bounds,
                               source_line, source_start, source_end);
        if(count > 0)
            out[count - 1].font_size = font_size;
        return count;
    }
    argc = krait_live_call_args(q, "Rect", args, 8);
    if(argc == 6) {
        if(!krait_live_eval_int(args[0], &x))
            x = 0;
        if(!krait_live_eval_int(args[1], &y))
            y = 0;
        if(!krait_live_eval_int(args[2], &w))
            w = 0;
        if(!krait_live_eval_int(args[3], &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Rectangle", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Button", args, 8);
    if(argc == 1) {
        char *scan = args[0];
        s = strchr(args[0], '"');
        label[0] = '\0';
        if(s != NULL)
            (void)krait_live_parse_string(&s, label, sizeof(label));
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_BUTTON, label, NULL, bounds,
                               source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Picture", args, 8);
    if(argc == 1) {
        char *scan = args[0];
        s = strchr(args[0], '"');
        asset[0] = '\0';
        if(s != NULL)
            (void)krait_live_parse_string(&s, asset, sizeof(asset));
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_SPRITE,
                               asset[0] != '\0' ? asset : "Picture", asset,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "TextField", args, 8);
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
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_TEXT_FIELD,
                               "Text Field", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Toggle", args, 8);
    if(argc >= 4) {
        if(!krait_live_eval_int(args[0], &x))
            x = 0;
        if(!krait_live_eval_int(args[1], &y))
            y = 0;
        if(!krait_live_eval_int(args[2], &w))
            w = 0;
        if(!krait_live_eval_int(args[3], &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_TOGGLE,
                               "Toggle", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Slider", args, 8);
    if(argc >= 4) {
        if(!krait_live_eval_int(args[1], &x))
            x = 0;
        if(!krait_live_eval_int(args[2], &y))
            y = 0;
        if(!krait_live_eval_int(args[3], &w))
            w = 0;
        h = ScaleUIPx(56);
        bounds = (Rectangle){x, y, w, (float)h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_SLIDER, "Slider", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Line", args, 8);
    if(argc == 5) {
        int x2 = 0, y2 = 0;
        if(!krait_live_eval_int(args[0], &x))
            x = 0;
        if(!krait_live_eval_int(args[1], &y))
            y = 0;
        if(!krait_live_eval_int(args[2], &x2))
            x2 = 0;
        if(!krait_live_eval_int(args[3], &y2))
            y2 = 0;
        int minx = x < x2 ? x : x2;
        int miny = y < y2 ? y : y2;
        bounds = (Rectangle){(float)minx, (float)miny,
                             (float)(x2 > x ? x2 - x : x - x2),
                             (float)(y2 > y ? y2 - y : y - y2)};
        if(bounds.width < 2) bounds.width = 2;
        if(bounds.height < 2) bounds.height = 2;
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Line", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "TextInRect", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        s = strchr(args[0], '"');
        label[0] = '\0';
        if(s != NULL)
            (void)krait_live_parse_string(&s, label, sizeof(label));
        x = 0; y = 0; w = 0; h = 0;
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_TEXT,
                               label[0] != '\0' ? label : "Text", NULL,
                               bounds, source_line, source_start, source_end);
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
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Progress", NULL,
                               bounds, source_line, source_start, source_end);
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
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_SPRITE, "Image", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "LabelFrame", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        s = strchr(args[0], '"');
        label[0] = '\0';
        if(s != NULL)
            (void)krait_live_parse_string(&s, label, sizeof(label));
        x = 0; y = 0; w = 0; h = 0;
        if(!krait_live_next_scale_arg(&scan, &x))
            x = 0;
        if(!krait_live_next_scale_arg(&scan, &y))
            y = 0;
        if(!krait_live_next_scale_arg(&scan, &w))
            w = 0;
        if(!krait_live_next_scale_arg(&scan, &h))
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_GROUP,
                               label[0] != '\0' ? label : "Frame", NULL,
                               bounds, source_line, source_start, source_end);
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
            h = 0;
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Separator", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "TabBar", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        h = ScaleUIPx(36);
        bounds = (Rectangle){x, y, w, (float)h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Tab Bar", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "BottomNav", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        krait_live_next_scale_arg(&scan, &h);
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Bottom Nav", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "TopNav", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        krait_live_next_scale_arg(&scan, &h);
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Top Nav", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Combobox", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        krait_live_next_scale_arg(&scan, &h);
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Combobox", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Spinbox", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        krait_live_next_scale_arg(&scan, &h);
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Spinbox", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Checkbox", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        krait_live_next_scale_arg(&scan, &h);
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Checkbox", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "Radio", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        krait_live_next_scale_arg(&scan, &h);
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "Radio", NULL,
                               bounds, source_line, source_start, source_end);
    }
    argc = krait_live_call_args(q, "ListBox", args, 8);
    if(argc >= 1) {
        char *scan = args[0];
        x = 0; y = 0; w = 0; h = 0;
        krait_live_next_scale_arg(&scan, &x);
        krait_live_next_scale_arg(&scan, &y);
        krait_live_next_scale_arg(&scan, &w);
        krait_live_next_scale_arg(&scan, &h);
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_RECTANGLE, "List Box", NULL,
                               bounds, source_line, source_start, source_end);
    }
    if(krait_game_node_args(q, args, node_type, sizeof(node_type),
                            label, sizeof(label), &x, &y, &w, &h)) {
        bounds = (Rectangle){x, y, w, h};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_GAME_OBJECT,
                               label[0] != '\0' ? label : node_type,
                               node_type, bounds, source_line, source_start,
                               source_end);
    }
    return count;
}

int
krait_scene_scan(const char *root, const char *rel_path)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    const char *body;
    const char *body_end;
    const char *p;
    int count;
    int line;
    int stack[64];
    int stack_depth = 0;
    char screen_label[128];
    SceneNode *out = g_scene_nodes;
    int cap = KRAIT_SCENE_NODE_MAX;

    g_scene_node_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)cap);
    if(root == NULL || rel_path == NULL)
        return 0;
    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    /* Scan the screen body, or fall back to the frame function body,
     * or fall back to the entire file if neither exists. */
    {
        int found_screen = krait_live_find_named_body(text, "screen", NULL,
                                                       &body, &body_end);
        if(found_screen) {
            snprintf(screen_label, sizeof(screen_label), "Screen");
        } else {
            char frame_name[128];
            frame_name[0] = '\0';
            if(krait_live_find_frame_name(text, frame_name, sizeof(frame_name)) &&
               krait_live_find_named_body(text, frame_name, NULL, &body, &body_end)) {
                const char *base = krait_basename(rel_path);
                snprintf(screen_label, sizeof(screen_label), "%s", base);
            } else {
                body = text;
                body_end = text + len;
                const char *base = krait_basename(rel_path);
                snprintf(screen_label, sizeof(screen_label), "%s", base);
            }
        }
    }
    count = krait_scene_add(out, 0, cap, -1, 0, KRAIT_SCENE_NODE_SCREEN,
                            screen_label, NULL, (Rectangle){0, 0, 0, 0},
                            krait_live_line_for_ptr(text, body),
                            (int)(body - text), (int)(body_end - text));
    stack[stack_depth++] = 0;
    line = krait_live_line_for_ptr(text, body);
    for(p = body; p < body_end && count < cap;) {
        const char *stmt_start = p;
        const char *stmt_end = body_end;
        int paren_depth = 0;
        int brace_depth = 0;
        int in_string = 0;
        int line_count = 0;

        for(const char *pp = p; pp < body_end; pp++) {
            if(in_string) {
                if(*pp == '\\' && pp + 1 < body_end) {
                    pp++;
                    continue;
                }
                if(*pp == '"')
                    in_string = 0;
            } else if(*pp == '"') {
                in_string = 1;
            } else if(*pp == '(' || *pp == '[') {
                paren_depth++;
            } else if(*pp == ')' || *pp == ']') {
                paren_depth--;
            } else if(*pp == '{') {
                brace_depth++;
            } else if(*pp == '}') {
                brace_depth--;
            } else if(*pp == '\n' && paren_depth <= 0 && brace_depth <= 0) {
                stmt_end = pp;
                break;
            }
        }
        {
            char stmt[4096];
            size_t n = (size_t)(stmt_end - stmt_start);
            if(n >= sizeof(stmt))
                n = sizeof(stmt) - 1;
            memcpy(stmt, stmt_start, n);
            stmt[n] = '\0';
            count = krait_scene_scan_line(stmt, out, count, cap, stack,
                                          &stack_depth, line,
                                          (int)(stmt_start - text),
                                          (int)(stmt_end - text));
        }
        for(const char *pp = stmt_start; pp < stmt_end; pp++)
            if(*pp == '\n')
                line_count++;
        line += line_count + 1;
        if(stmt_end >= body_end)
            break;
        p = stmt_end + 1;
        line++;
    }
    free(text);
    g_scene_node_count = count;
    return count;
}

static SceneNode *
krait_scene_node(int index)
{
    if(index < 0 || index >= g_scene_node_count)
        return NULL;
    return &g_scene_nodes[index];
}

int
krait_scene_kind(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->kind : KRAIT_SCENE_NODE_UNKNOWN;
}

int
krait_scene_depth(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->depth : 0;
}

const char *
krait_scene_label(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->label : "";
}

const char *
krait_scene_asset_path(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->asset_path : "";
}

Rectangle
krait_scene_bounds(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->bounds : (Rectangle){0};
}

int
krait_scene_source_line(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->source_line : 0;
}

int
krait_scene_editable(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->editable : 0;
}

int
krait_scene_font_size(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->font_size : 0;
}

int
krait_scene_layout_parent(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->layout_parent : -1;
}

int
krait_scene_offset_x(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->offset_x : 0;
}

int
krait_scene_offset_y(int index)
{
    SceneNode *node = krait_scene_node(index);
    return node != NULL ? node->offset_y : 0;
}

static int
krait_scene_replace_scale_delta(char *text, size_t text_size, int scale_index,
                                int delta)
{
    const char *needle = "ScaleUIPx";
    char *p = text;
    int seen = 0;

    while((p = strstr(p, needle)) != NULL) {
        char *open = p + strlen(needle);
        char *num;
        char *end;
        int value;
        char repl[32];
        size_t old_len;
        size_t new_len;
        size_t tail_len;

        while(*open != '\0' && isspace((unsigned char)*open))
            open++;
        if(*open != '(') {
            p += strlen(needle);
            continue;
        }
        if(seen++ != scale_index) {
            p = open + 1;
            continue;
        }
        num = open + 1;
        while(*num != '\0' && isspace((unsigned char)*num))
            num++;
        end = num;
        if(*end == '-' || *end == '+')
            end++;
        if(!isdigit((unsigned char)*end))
            return 0;
        while(isdigit((unsigned char)*end))
            end++;
        value = atoi(num) + delta;
        if(value < 0)
            value = 0;
        snprintf(repl, sizeof(repl), "%d", value);
        old_len = (size_t)(end - num);
        new_len = strlen(repl);
        tail_len = strlen(end);
        if(strlen(text) - old_len + new_len + 1 > text_size)
            return 0;
        memmove(num + new_len, end, tail_len + 1);
        memcpy(num, repl, new_len);
        return 1;
    }
    return 0;
}

int
krait_scene_nudge(const char *root, const char *rel_path, int index,
                  int dx, int dy, int dw, int dh,
                  char *status, int status_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    char *slice = NULL;
    long len = 0;
    long span;
    int deltas[4];
    int scale_count = 4;
    SceneNode node;
    FILE *file;

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Node edit failed");
    if(root == NULL || rel_path == NULL)
        return 0;
    if(index < 0 || index >= g_scene_node_count)
        return 0;
    node = g_scene_nodes[index];
    fprintf(stderr, "[nudge] index=%d kind=%d editable=%d dx=%d dy=%d dw=%d dh=%d span=%ld..%ld\n",
            index, node.kind, node.editable, dx, dy, dw, dh,
            (long)node.source_start, (long)node.source_end);
    if(!node.editable) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Node is read-only");
        return 0;
    }
    if(node.kind == KRAIT_SCENE_NODE_TEXT)
        scale_count = 2;
    else if(node.kind != KRAIT_SCENE_NODE_GROUP &&
            node.kind != KRAIT_SCENE_NODE_RECTANGLE &&
            node.kind != KRAIT_SCENE_NODE_BUTTON &&
            node.kind != KRAIT_SCENE_NODE_SPRITE &&
            node.kind != KRAIT_SCENE_NODE_GAME_OBJECT &&
            node.kind != KRAIT_SCENE_NODE_TEXT_FIELD &&
            node.kind != KRAIT_SCENE_NODE_TOGGLE &&
            node.kind != KRAIT_SCENE_NODE_SLIDER) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Node kind is not editable yet");
        return 0;
    }
    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    if(node.source_start < 0 || node.source_end < node.source_start ||
       node.source_end > len) {
        free(text);
        return 0;
    }
    span = node.source_end - node.source_start;
    slice = malloc((size_t)span + 128);
    if(slice == NULL) {
        free(text);
        return 0;
    }
    memcpy(slice, text + node.source_start, (size_t)span);
    slice[span] = '\0';
    deltas[0] = dx;
    deltas[1] = dy;
    deltas[2] = dw;
    deltas[3] = dh;
    for(int i = 0; i < scale_count; i++) {
        if(deltas[i] == 0)
            continue;
        if(!krait_scene_replace_scale_delta(slice, (size_t)span + 128,
                                           i, deltas[i])) {
            free(slice);
            free(text);
            if(status != NULL && status_size > 0)
                snprintf(status, (size_t)status_size,
                         "Node uses unsupported bounds expression");
            return 0;
        }
    }
    file = fopen(path, "wb");
    if(file == NULL) {
        free(slice);
        free(text);
        return 0;
    }
    if(node.source_start > 0)
        fwrite(text, 1, (size_t)node.source_start, file);
    fwrite(slice, 1, strlen(slice), file);
    fwrite(text + node.source_end, 1, (size_t)(len - node.source_end), file);
    fclose(file);
    free(slice);
    free(text);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Node updated");
    return 1;
}

/* ---- property model: per-kind spec tables + typed source rewriter ----
 *
 * The Inspector renders these specs instead of the fixed X/Y/W/H grid. Each
 * kind publishes its editable properties (label/asset_path/position/size); the
 * getter reads the parsed SceneNode fields, the setter rewrites the matching
 * token in the .kry source span. Bounds keep using the ScaleUIPx delta path;
 * strings rewrite the first "..." literal in the span.
 */

#define KRAIT_PROP_MAX 6

typedef struct KraitPropSpec {
    char id[32];
    char label[48];
    int kind; /* PROPERTY_* */
} KraitPropSpec;

static const KraitPropSpec kry_props_text[] = {
    {"label", "Text", PROPERTY_STRING},
    {"position", "Position", PROPERTY_RECTANGLE},
    {"font_size", "Font Size", PROPERTY_INT},
};

static const KraitPropSpec kry_props_rect[] = {
    {"position", "Position", PROPERTY_RECTANGLE},
};

static const KraitPropSpec kry_props_button[] = {
    {"label", "Label", PROPERTY_STRING},
    {"position", "Position", PROPERTY_RECTANGLE},
};

static const KraitPropSpec kry_props_sprite[] = {
    {"asset_path", "Asset", PROPERTY_ASSET_PATH},
    {"position", "Position", PROPERTY_RECTANGLE},
};

static const KraitPropSpec kry_props_group[] = {
    {"position", "Position", PROPERTY_RECTANGLE},
};

static const KraitPropSpec kry_props_game[] = {
    {"label", "Label", PROPERTY_STRING},
    {"position", "Position", PROPERTY_RECTANGLE},
};

static const KraitPropSpec kry_props_text_field[] = {
    {"label", "Text", PROPERTY_STRING},
    {"position", "Position", PROPERTY_RECTANGLE},
};

static const KraitPropSpec kry_props_toggle[] = {
    {"label", "Label", PROPERTY_STRING},
    {"position", "Position", PROPERTY_RECTANGLE},
};

static const KraitPropSpec kry_props_slider[] = {
    {"position", "Position", PROPERTY_RECTANGLE},
};

static const KraitPropSpec *
krait_prop_specs(int kind, int *out_count)
{
    switch(kind) {
    case KRAIT_SCENE_NODE_TEXT:
        *out_count = 3; return kry_props_text;
    case KRAIT_SCENE_NODE_RECTANGLE:
        *out_count = 1; return kry_props_rect;
    case KRAIT_SCENE_NODE_BUTTON:
        *out_count = 2; return kry_props_button;
    case KRAIT_SCENE_NODE_SPRITE:
        *out_count = 2; return kry_props_sprite;
    case KRAIT_SCENE_NODE_GROUP:
        *out_count = 1; return kry_props_group;
    case KRAIT_SCENE_NODE_GAME_OBJECT:
        *out_count = 2; return kry_props_game;
    case KRAIT_SCENE_NODE_TEXT_FIELD:
        *out_count = 2; return kry_props_text_field;
    case KRAIT_SCENE_NODE_TOGGLE:
        *out_count = 2; return kry_props_toggle;
    case KRAIT_SCENE_NODE_SLIDER:
        *out_count = 1; return kry_props_slider;
    default:
        *out_count = 0; return NULL;
    }
}

int
krait_scene_property_count(int index)
{
    SceneNode *node = krait_scene_node(index);
    int count;
    if(node == NULL)
        return 0;
    krait_prop_specs(node->kind, &count);
    return count;
}

int
krait_scene_property_spec(int index, int prop_index, char *id, int id_size,
                          char *label, int label_size, int *out_kind)
{
    SceneNode *node = krait_scene_node(index);
    const KraitPropSpec *specs;
    int count;
    if(node == NULL)
        return 0;
    specs = krait_prop_specs(node->kind, &count);
    if(specs == NULL || prop_index < 0 || prop_index >= count)
        return 0;
    snprintf(id, (size_t)id_size, "%s", specs[prop_index].id);
    snprintf(label, (size_t)label_size, "%s", specs[prop_index].label);
    if(out_kind != NULL)
        *out_kind = specs[prop_index].kind;
    return 1;
}

/* Read a property into the provided buffer. For strings/asset_path the buffer
 * receives the value; for rectangle kinds it receives "x y w h" integers. */
int
krait_scene_property_get(int index, const char *property_id, char *buf,
                         int buf_size)
{
    SceneNode *node = krait_scene_node(index);
    if(node == NULL || property_id == NULL || buf == NULL || buf_size <= 0)
        return 0;
    buf[0] = '\0';
    if(strcmp(property_id, "label") == 0) {
        snprintf(buf, (size_t)buf_size, "%s", node->label);
        return 1;
    }
    if(strcmp(property_id, "asset_path") == 0) {
        snprintf(buf, (size_t)buf_size, "%s", node->asset_path);
        return 1;
    }
    if(strcmp(property_id, "position") == 0) {
        if(node->kind == KRAIT_SCENE_NODE_TEXT)
            snprintf(buf, (size_t)buf_size, "%d %d",
                     (int)node->bounds.x, (int)node->bounds.y);
        else
            snprintf(buf, (size_t)buf_size, "%d %d %d %d",
                     (int)node->bounds.x, (int)node->bounds.y,
                     (int)node->bounds.width, (int)node->bounds.height);
        return 1;
    }
    if(strcmp(property_id, "font_size") == 0) {
        snprintf(buf, (size_t)buf_size, "%d", node->font_size);
        return 1;
    }
    return 0;
}

/* Rewrite the first "..." string literal within [slice]. Returns 1 on
 * success. The caller must ensure buf_size is large enough for the new span. */
static int
krait_scene_replace_string_literal(char *slice, size_t slice_size,
                                   const char *new_value)
{
    char *open = strchr(slice, '"');
    char *close;
    size_t old_len;
    size_t new_len;
    size_t tail_len;
    char literal[768];

    if(open == NULL)
        return 0;
    close = strchr(open + 1, '"');
    if(close == NULL)
        return 0;
    /* escape nothing for now; asset paths and labels are plain */
    snprintf(literal, sizeof(literal), "\"%s\"", new_value);
    new_len = strlen(literal);
    old_len = (size_t)(close + 1 - open);
    tail_len = strlen(close + 1);
    if(strlen(slice) - old_len + new_len + 1 > slice_size)
        return 0;
    memmove(open + new_len, close + 1, tail_len + 1);
    memcpy(open, literal, new_len);
    return 1;
}

/* Parse "x y [w h]" integers from buf into deltas relative to current bounds,
 * then reuse the ScaleUIPx delta path to rewrite the source. */
static int
krait_scene_apply_position(const char *root, const char *rel_path,
                           SceneNode *node, const char *buf,
                           char *status, int status_size)
{
    int nx, ny, nw, nh;
    int fields;
    int dx, dy, dw, dh;

    fields = sscanf(buf, "%d %d %d %d", &nx, &ny, &nw, &nh);
    if(fields < 2)
        return 0;
    dx = nx - (int)node->bounds.x;
    dy = ny - (int)node->bounds.y;
    dw = fields >= 4 ? nw - (int)node->bounds.width : 0;
    dh = fields >= 4 ? nh - (int)node->bounds.height : 0;
    return krait_scene_nudge(root, rel_path, node->id, dx, dy, dw, dh,
                             status, status_size);
}

int
krait_scene_property_set(int index, const char *property_id, const char *value,
                         const char *root, const char *rel_path,
                         char *status, int status_size)
{
    SceneNode *node = krait_scene_node(index);
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    char *slice = NULL;
    long len = 0;
    long span;
    FILE *file;

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Property not set");
    if(node == NULL || property_id == NULL || value == NULL)
        return 0;
    if(!node->editable)
        return 0;

    /* position delegates to the existing ScaleUIPx delta rewriter */
    if(strcmp(property_id, "position") == 0)
        return krait_scene_apply_position(root, rel_path, node, value,
                                          status, status_size);

    /* label / asset_path rewrite the first string literal in the source span */
    if(strcmp(property_id, "label") != 0 && strcmp(property_id, "asset_path") != 0)
        return 0;

    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    if(node->source_start < 0 || node->source_end < node->source_start ||
       node->source_end > len) {
        free(text);
        return 0;
    }
    span = node->source_end - node->source_start;
    slice = malloc((size_t)span + 1024);
    if(slice == NULL) {
        free(text);
        return 0;
    }
    memcpy(slice, text + node->source_start, (size_t)span);
    slice[span] = '\0';
    if(!krait_scene_replace_string_literal(slice, (size_t)span + 1024, value)) {
        free(slice);
        free(text);
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size,
                     "Property uses unsupported expression");
        return 0;
    }
    file = fopen(path, "wb");
    if(file == NULL) {
        free(slice);
        free(text);
        return 0;
    }
    if(node->source_start > 0)
        fwrite(text, 1, (size_t)node->source_start, file);
    fwrite(slice, 1, strlen(slice), file);
    fwrite(text + node->source_end, 1, (size_t)(len - node->source_end), file);
    fclose(file);
    free(slice);
    free(text);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Property updated");
    return 1;
}


