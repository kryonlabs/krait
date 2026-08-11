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

        s = args[0];
        label[0] = '\0';
        (void)krait_live_parse_string(&s, label, sizeof(label));
        if(!krait_live_eval_int(args[1], &x))
            x = 0;
        if(!krait_live_eval_int(args[2], &y))
            y = 0;
        text_w = (int)strlen(label) * ScaleUIPx(8);
        if(text_w < ScaleUIPx(24))
            text_w = ScaleUIPx(24);
        bounds = (Rectangle){x, y, text_w, ScaleUIPx(18)};
        return krait_scene_add(out, count, cap, parent, depth,
                               KRAIT_SCENE_NODE_TEXT, label, NULL, bounds,
                               source_line, source_start, source_end);
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
    if(!krait_live_find_named_body(text, "screen", NULL, &body, &body_end)) {
        free(text);
        return 0;
    }
    snprintf(screen_label, sizeof(screen_label), "%s", rel_path);
    count = krait_scene_add(out, 0, cap, -1, 0, KRAIT_SCENE_NODE_SCREEN,
                            screen_label, NULL, (Rectangle){0, 0, 0, 0},
                            krait_live_line_for_ptr(text, body),
                            (int)(body - text), (int)(body_end - text));
    stack[stack_depth++] = 0;
    line = krait_live_line_for_ptr(text, body);
    for(p = body; p < body_end && count < cap;) {
        const char *nl = memchr(p, '\n', (size_t)(body_end - p));
        char stmt[4096];
        size_t n = nl != NULL ? (size_t)(nl - p) : (size_t)(body_end - p);

        if(n >= sizeof(stmt))
            n = sizeof(stmt) - 1;
        memcpy(stmt, p, n);
        stmt[n] = '\0';
        count = krait_scene_scan_line(stmt, out, count, cap, stack,
                                      &stack_depth, line, (int)(p - text),
                                      (int)(p + n - text));
        if(nl == NULL)
            break;
        p = nl + 1;
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
            node.kind != KRAIT_SCENE_NODE_GAME_OBJECT) {
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

