/*
 * Live scene inspector data source. Polls a running kryon app's
 * SceneInspectServe JSON endpoint (http://127.0.0.1:<port>/scene) and keeps
 * a flat node list plus the selected node's property table for the IDE's
 * Inspect navigator tab. See ide/inspect.kry for the drawing side.
 */

#include "kryon.h"
#include "kry_http.h"
#include "kry_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KRAIT_INSPECT_MAX_NODES 512
#define KRAIT_INSPECT_MAX_PROPS 48
#define KRAIT_INSPECT_REFRESH_SECONDS 0.5f

typedef struct KraitInspectNode {
    int id;
    int parent;
    char name[64];
    char kind[32];
    float x;
    float y;
} KraitInspectNode;

typedef struct KraitInspectProp {
    char id[64];
    char label[96];
    char value[160];
} KraitInspectProp;

static KraitInspectNode g_nodes[KRAIT_INSPECT_MAX_NODES];
static int g_node_count;
static KraitInspectProp g_props[KRAIT_INSPECT_MAX_PROPS];
static int g_prop_count;
static int g_selected = -1;
static int g_connected;
static int g_port = 8642;
static float g_refresh_timer;
static KryHttpRequest *g_request;
static float g_last_time;

static void
copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

static void
json_value_to_text(const KryJson *v, char *out, size_t cap)
{
    switch(kry_json_type(v)) {
    case KRY_JSON_STRING:
        copy_str(out, cap, kry_json_string(v));
        break;
    case KRY_JSON_NUMBER:
        snprintf(out, cap, "%g", kry_json_number(v));
        break;
    case KRY_JSON_BOOL:
        copy_str(out, cap, kry_json_bool(v) ? "true" : "false");
        break;
    case KRY_JSON_ARRAY: {
        int n = kry_json_count(v);
        int i;
        size_t used = 0;

        used += (size_t)snprintf(out + used, cap - used, "[");
        for(i = 0; i < n && used + 2 < cap; i++) {
            char item[48];

            json_value_to_text(kry_json_at(v, i), item, sizeof(item));
            used += (size_t)snprintf(out + used, cap - used, "%s%s",
                                     i > 0 ? ", " : "", item);
        }
        snprintf(out + used, cap - used, "]");
        break;
    }
    default:
        copy_str(out, cap, "null");
        break;
    }
}

static void
parse_scene(const char *text)
{
    KryJson *root = kry_json_parse(text);
    const KryJson *nodes;
    int count;
    int i;

    if(root == NULL)
        return;
    nodes = kry_json_get(root, "nodes");
    count = nodes ? kry_json_count(nodes) : 0;
    if(count < 0)
        count = 0;
    if(count > KRAIT_INSPECT_MAX_NODES)
        count = KRAIT_INSPECT_MAX_NODES;
    g_node_count = 0;
    for(i = 0; i < count; i++) {
        const KryJson *jn = kry_json_at(nodes, i);
        const KryJson *field;
        KraitInspectNode *n = &g_nodes[g_node_count];

        if(jn == NULL)
            continue;
        field = kry_json_get(jn, "id");
        n->id = field ? (int)kry_json_number(field) : -1;
        field = kry_json_get(jn, "parent");
        n->parent = field ? (int)kry_json_number(field) : -1;
        copy_str(n->name, sizeof(n->name),
                 kry_json_string(kry_json_get(jn, "name")));
        copy_str(n->kind, sizeof(n->kind),
                 kry_json_string(kry_json_get(jn, "kind")));
        field = kry_json_get(jn, "position");
        if(field != NULL && kry_json_count(field) >= 2) {
            n->x = (float)kry_json_number(kry_json_at(field, 0));
            n->y = (float)kry_json_number(kry_json_at(field, 1));
        }
        g_node_count++;
    }
    kry_json_free(root);
    g_connected = 1;
}

int
krait_inspect_node_count(void)
{
    return g_node_count;
}

int
krait_inspect_connected(void)
{
    return g_connected;
}

int
krait_inspect_port(void)
{
    return g_port;
}

void
krait_inspect_set_port(int port)
{
    if(port > 0 && port < 65536 && port != g_port) {
        g_port = port;
        g_connected = 0;
        g_node_count = 0;
        g_prop_count = 0;
        g_refresh_timer = KRAIT_INSPECT_REFRESH_SECONDS;
    }
}

int
krait_inspect_node_id(int index)
{
    return (index >= 0 && index < g_node_count) ? g_nodes[index].id : -1;
}

int
krait_inspect_node_parent(int index)
{
    return (index >= 0 && index < g_node_count) ? g_nodes[index].parent : -1;
}

const char *
krait_inspect_node_name(int index)
{
    return (index >= 0 && index < g_node_count) ? g_nodes[index].name : "";
}

const char *
krait_inspect_node_kind(int index)
{
    return (index >= 0 && index < g_node_count) ? g_nodes[index].kind : "";
}

float
krait_inspect_node_x(int index)
{
    return (index >= 0 && index < g_node_count) ? g_nodes[index].x : 0.0f;
}

float
krait_inspect_node_y(int index)
{
    return (index >= 0 && index < g_node_count) ? g_nodes[index].y : 0.0f;
}

int
krait_inspect_selected(void)
{
    return g_selected;
}

void
krait_inspect_select(int node_id)
{
    g_selected = node_id;
    g_prop_count = 0;
}

int
krait_inspect_prop_count(void)
{
    return g_prop_count;
}

const char *
krait_inspect_prop_id(int index)
{
    return (index >= 0 && index < g_prop_count) ? g_props[index].id : "";
}

const char *
krait_inspect_prop_label(int index)
{
    return (index >= 0 && index < g_prop_count) ? g_props[index].label : "";
}

const char *
krait_inspect_prop_value(int index)
{
    return (index >= 0 && index < g_prop_count) ? g_props[index].value : "";
}

/* Keep the raw last response so selecting a different node can rebuild its
 * property table without a new request. */
static char g_last_response[256 * 1024];

static void
parse_selected_props(void)
{
    KryJson *root = kry_json_parse(g_last_response);
    const KryJson *nodes;
    int count;
    int i;

    if(root == NULL)
        return;
    nodes = kry_json_get(root, "nodes");
    count = nodes ? kry_json_count(nodes) : 0;
    g_prop_count = 0;
    for(i = 0; i < count && g_prop_count < KRAIT_INSPECT_MAX_PROPS; i++) {
        const KryJson *jn = kry_json_at(nodes, i);
        const KryJson *props;
        const KryJson *id_field;
        int j;

        if(jn == NULL)
            continue;
        id_field = kry_json_get(jn, "id");
        if(id_field == NULL || (int)kry_json_number(id_field) != g_selected)
            continue;
        props = kry_json_get(jn, "props");
        count = props ? kry_json_count(props) : 0;
        for(j = 0; j < count && g_prop_count < KRAIT_INSPECT_MAX_PROPS; j++) {
            const KryJson *jp = kry_json_at(props, j);
            KraitInspectProp *p = &g_props[g_prop_count];

            if(jp == NULL)
                continue;
            copy_str(p->id, sizeof(p->id), kry_json_string(kry_json_get(jp, "id")));
            copy_str(p->label, sizeof(p->label),
                     kry_json_string(kry_json_get(jp, "label")));
            json_value_to_text(kry_json_get(jp, "value"), p->value,
                               sizeof(p->value));
            g_prop_count++;
        }
        break;
    }
    kry_json_free(root);
}

void
krait_inspect_tick(void)
{
    float now = (float)GetTime();
    float dt = now - g_last_time;

    if(dt < 0.0f || dt > 1.0f)
        dt = 0.016f;
    g_last_time = now;
    if(g_request != NULL) {
        KryHttpStatus status = kry_http_poll(g_request);

        if(status == KRY_HTTP_DONE) {
            const char *body = kry_http_response(g_request);

            if(body != NULL && kry_http_status_code(g_request) == 200) {
                snprintf(g_last_response, sizeof(g_last_response), "%s", body);
                parse_scene(body);
                parse_selected_props();
            } else {
                g_connected = 0;
            }
            kry_http_free(g_request);
            g_request = NULL;
            g_refresh_timer = KRAIT_INSPECT_REFRESH_SECONDS;
        } else if(status == KRY_HTTP_FAILED) {
            g_connected = 0;
            kry_http_free(g_request);
            g_request = NULL;
            g_refresh_timer = KRAIT_INSPECT_REFRESH_SECONDS;
        }
        return;
    }
    g_refresh_timer -= dt;
    if(g_refresh_timer <= 0.0f) {
        char url[96];

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/scene", g_port);
        g_request = kry_http_get(url, 2);
        g_refresh_timer = 1.0f;
    }
}
