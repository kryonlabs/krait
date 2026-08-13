/*
 * Cartridge (krb) explorer backend. Compiles the current .kry source to a .krb
 * cartridge (kc --emit-krb), loads it with KrbLoadFile, and exposes the node
 * tree, string table, program, and host imports to ide/cartridge.kry.
 * krait_krb_draw renders the cartridge through the raylib-backed KryBackend for
 * an in-panel preview.
 *
 * The bridge is link-time FFI by name, matching native_assets.c / native_scene.c:
 * cartridge.kry declares each function `#extern` and this file defines it.
 */

#include "kryon.h"
#include "krb.h"
#include "ide/state.h"
#include "native_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KRAIT_KRB_NODE_MAX 512
#define KRAIT_KRB_STR_MAX 1024
#define KRAIT_KRB_OP_MAX 256

static KrbImage g_krb;
static int g_krb_loaded;
static char g_krb_status[256];
static char g_krb_last_source[1024];

/* Derived tables, rebuilt on every successful load. */
static int g_krb_depth[KRAIT_KRB_NODE_MAX];
static unsigned g_krb_str_off[KRAIT_KRB_STR_MAX];
static int g_krb_str_count;
static char g_krb_op_disasm[KRAIT_KRB_OP_MAX][48];
static int g_krb_op_count;

/* Resolve the kryon source tree (KRYON_DIR env, then the vendored checkout). */
static const char *
krait_krb_dir(void)
{
    const char *dir = getenv("KRYON_DIR");
    if(dir != NULL && dir[0] != '\0')
        return dir;
    return "vendor/kryon";
}

/* Wrap a path in single quotes for /bin/sh, escaping embedded quotes. */
static int
krait_krb_quote(char *dst, size_t dst_size, const char *src)
{
    size_t n = 0;

    if(dst_size < 3)
        return 0;
    if(src == NULL)
        src = "";
    dst[n++] = '\'';
    for(const char *p = src; *p != '\0'; p++) {
        if(*p == '\'') {
            const char *esc = "'\\''";
            for(int i = 0; esc[i] != '\0'; i++) {
                if(n + 1 >= dst_size)
                    return 0;
                dst[n++] = esc[i];
            }
        } else {
            if(n + 1 >= dst_size)
                return 0;
            dst[n++] = *p;
        }
    }
    if(n + 2 >= dst_size)
        return 0;
    dst[n++] = '\'';
    dst[n] = '\0';
    return 1;
}

static void
krait_krb_set_status(const char *msg)
{
    snprintf(g_krb_status, sizeof(g_krb_status), "%s", msg);
}

static int
krait_krb_compute_depth(int index)
{
    KrbNode n;
    int parent;

    if(index < 0 || index >= KRAIT_KRB_NODE_MAX)
        return 0;
    if(g_krb_depth[index] >= 0)
        return g_krb_depth[index];
    if(KrbReadNode(&g_krb, (unsigned)index, &n) != 0)
        return 0;
    if(n.parent < 0) {
        g_krb_depth[index] = 0;
        return 0;
    }
    parent = (int)n.parent;
    if(parent >= index) {            /* malformed: cycle / forward ref */
        g_krb_depth[index] = 0;
        return 0;
    }
    g_krb_depth[index] = krait_krb_compute_depth(parent) + 1;
    return g_krb_depth[index];
}

static void
krait_krb_build_tables(void)
{
    unsigned i;
    unsigned node_count;
    unsigned off;
    unsigned prog_len;
    const unsigned char *p;

    node_count = KrbNodeCount(&g_krb);
    for(i = 0; i < KRAIT_KRB_NODE_MAX; i++)
        g_krb_depth[i] = -1;
    for(i = 0; i < node_count && i < KRAIT_KRB_NODE_MAX; i++)
        (void)krait_krb_compute_depth((int)i);

    g_krb_str_count = 0;
    off = 0;
    while(off < g_krb.header->string_bytes &&
          g_krb_str_count < KRAIT_KRB_STR_MAX) {
        g_krb_str_off[g_krb_str_count++] = off;
        off += (unsigned)strlen(g_krb.strings + off) + 1;
    }

    g_krb_op_count = 0;
    p = g_krb.prog;
    prog_len = g_krb.header->prog_bytes;
    off = 0;
    while(off < prog_len && g_krb_op_count < KRAIT_KRB_OP_MAX) {
        char buf[48];
        unsigned char op = p[off];

        buf[0] = '\0';
        if(op == KRB_OP_DRAW_TREE) {
            snprintf(buf, sizeof(buf), "OP_DRAW_TREE");
            off += 1;
        } else if(op == KRB_OP_CALL_HOST && off + 1 < prog_len) {
            snprintf(buf, sizeof(buf), "OP_CALL_HOST slot=%u", p[off + 1]);
            off += 2;
        } else if(op == KRB_OP_SET_I32 && off + 6 < prog_len) {
            unsigned path = (unsigned)p[off + 1] | ((unsigned)p[off + 2] << 8);
            int value = (int)((unsigned)p[off + 3] | ((unsigned)p[off + 4] << 8) |
                              ((unsigned)p[off + 5] << 16) | ((unsigned)p[off + 6] << 24));
            snprintf(buf, sizeof(buf), "OP_SET_I32 path=%u %d", path, value);
            off += 7;
        } else {
            snprintf(buf, sizeof(buf), "OP_0x%02x (?)", op);
            off += 1;
        }
        snprintf(g_krb_op_disasm[g_krb_op_count],
                 sizeof(g_krb_op_disasm[g_krb_op_count]), "%s", buf);
        g_krb_op_count++;
    }
}

int
krait_krb_open(const char *root, const char *rel_source, char *status,
               int status_size)
{
    char kc[KRAIT_PATH_MAX];
    char qkc[KRAIT_PATH_MAX * 2];
    char qroot[KRAIT_PATH_MAX * 2];
    char qsrc[KRAIT_PATH_MAX * 2];
    char command[KRAIT_PATH_MAX * 8];
    char base[256];
    char krb_path[KRAIT_PATH_MAX];
    const char *dir;
    const char *dot;
    int rc;

    if(status != NULL && status_size > 0)
        status[0] = '\0';
    if(g_krb_loaded)
        KrbFree(&g_krb);
    g_krb_loaded = 0;
    g_krb_status[0] = '\0';
    g_krb_str_count = 0;
    g_krb_op_count = 0;

    if(root == NULL || rel_source == NULL || rel_source[0] == '\0') {
        krait_krb_set_status("No .kry source selected");
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "%s", g_krb_status);
        return -1;
    }

    dir = krait_krb_dir();
    snprintf(kc, sizeof(kc), "%s/build/bin/k2b", dir);
    if(!krait_krb_quote(qkc, sizeof(qkc), kc) ||
       !krait_krb_quote(qroot, sizeof(qroot), root) ||
       !krait_krb_quote(qsrc, sizeof(qsrc), rel_source)) {
        krait_krb_set_status("Path too long");
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "%s", g_krb_status);
        return -1;
    }

    /* kc writes <outdir>/<sourcebase>.krb for each input .kry. */
    snprintf(command, sizeof(command),
             "mkdir -p /tmp/krait-krb && %s --no-main --root %s -o /tmp/krait-krb %s",
             qkc, qroot, qsrc);
    rc = system(command);
    if(rc != 0) {
        krait_krb_set_status("k2b failed (is KRYON_DIR set?)");
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "%s", g_krb_status);
        return -1;
    }

    snprintf(base, sizeof(base), "%s", krait_basename(rel_source));
    dot = strrchr(base, '.');
    if(dot != NULL)
        *(char *)(void *)dot = '\0';
    snprintf(krb_path, sizeof(krb_path), "/tmp/krait-krb/%s.krb", base);

    memset(&g_krb, 0, sizeof(g_krb));
    if(KrbLoadFile(&g_krb, krb_path) != 0) {
        krait_krb_set_status("No cartridge emitted");
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "%s", g_krb_status);
        return -1;
    }
    g_krb_loaded = 1;
    snprintf(g_krb_last_source, sizeof(g_krb_last_source), "%s", rel_source);
    snprintf(g_krb_status, sizeof(g_krb_status), "loaded %u nodes",
             KrbNodeCount(&g_krb));
    krait_krb_build_tables();
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "%s", g_krb_status);
    return (int)KrbNodeCount(&g_krb);
}

void
krait_krb_close(void)
{
    if(g_krb_loaded)
        KrbFree(&g_krb);
    g_krb_loaded = 0;
    g_krb_status[0] = '\0';
    g_krb_last_source[0] = '\0';
    g_krb_str_count = 0;
    g_krb_op_count = 0;
}

int
krait_krb_loaded(void)
{
    return g_krb_loaded;
}

const char *
krait_krb_status(void)
{
    return g_krb_status;
}

const char *
krait_krb_last_source(void)
{
    return g_krb_last_source;
}

int
krait_krb_version(void)
{
    return g_krb_loaded ? (int)g_krb.header->version : 0;
}

int
krait_krb_node_count(void)
{
    return g_krb_loaded ? (int)KrbNodeCount(&g_krb) : 0;
}

static int
krait_krb_read(int index, KrbNode *n)
{
    if(!g_krb_loaded || index < 0)
        return 0;
    return KrbReadNode(&g_krb, (unsigned)index, n) == 0;
}

int
krait_krb_node_parent(int index)
{
    KrbNode n;
    if(!krait_krb_read(index, &n))
        return -1;
    return (int)n.parent;
}

int
krait_krb_node_depth(int index)
{
    if(!g_krb_loaded || index < 0 || index >= KRAIT_KRB_NODE_MAX)
        return 0;
    return g_krb_depth[index] < 0 ? 0 : g_krb_depth[index];
}

int
krait_krb_node_type(int index)
{
    KrbNode n;
    if(!krait_krb_read(index, &n))
        return 0;
    return (int)n.type;
}

const char *
krait_krb_node_type_name(int index)
{
    KrbNode n;
    if(!krait_krb_read(index, &n))
        return "?";
    switch(n.type) {
    case KRB_NODE_BACKGROUND: return "BG";
    case KRB_NODE_TEXT:       return "TXT";
    case KRB_NODE_RECT:       return "RECT";
    case KRB_NODE_BUTTON:     return "BTN";
    case KRB_NODE_DATA:       return "DATA";
    default:                  return "?";
    }
}

const char *
krait_krb_node_name(int index)
{
    KrbNode n;
    if(!krait_krb_read(index, &n))
        return "";
    return KrbString(&g_krb, n.name_off);
}

const char *
krait_krb_node_text(int index)
{
    KrbNode n;
    if(!krait_krb_read(index, &n))
        return "";
    return KrbString(&g_krb, n.text_off);
}

void
krait_krb_node_rect(int index, int *x, int *y, int *w, int *h)
{
    KrbNode n;
    if(x != NULL) *x = 0;
    if(y != NULL) *y = 0;
    if(w != NULL) *w = 0;
    if(h != NULL) *h = 0;
    if(!krait_krb_read(index, &n))
        return;
    if(x != NULL) *x = (int)n.x;
    if(y != NULL) *y = (int)n.y;
    if(w != NULL) *w = (int)n.w;
    if(h != NULL) *h = (int)n.h;
}

int
krait_krb_node_x(int index)
{
    KrbNode n;
    return krait_krb_read(index, &n) ? (int)n.x : 0;
}

int
krait_krb_node_y(int index)
{
    KrbNode n;
    return krait_krb_read(index, &n) ? (int)n.y : 0;
}

int
krait_krb_node_w(int index)
{
    KrbNode n;
    return krait_krb_read(index, &n) ? (int)n.w : 0;
}

int
krait_krb_node_h(int index)
{
    KrbNode n;
    return krait_krb_read(index, &n) ? (int)n.h : 0;
}

int
krait_krb_node_bind_slot(int index)
{
    KrbNode n;
    if(!krait_krb_read(index, &n))
        return -1;
    return n.bind_slot == 0xffff ? -1 : (int)n.bind_slot;
}

int
krait_krb_string_count(void)
{
    return g_krb_str_count;
}

const char *
krait_krb_string(int index)
{
    if(index < 0 || index >= g_krb_str_count)
        return "";
    return KrbString(&g_krb, g_krb_str_off[index]);
}

int
krait_krb_import_count(void)
{
    return g_krb_loaded ? (int)KrbImportCount(&g_krb) : 0;
}

const char *
krait_krb_import_name(int index)
{
    if(!g_krb_loaded || index < 0 || index >= (int)KrbImportCount(&g_krb))
        return "";
    return KrbImportName(&g_krb, (unsigned)index);
}

int
krait_krb_op_count(void)
{
    return g_krb_op_count;
}

const char *
krait_krb_op_text(int index)
{
    if(index < 0 || index >= g_krb_op_count)
        return "";
    return g_krb_op_disasm[index];
}

int
krait_krb_draw(Rectangle viewport)
{
    if(!g_krb_loaded)
        return 0;
    KryBackendSelect(&KryBackendDraw);
    KrbDraw(&g_krb, (int)viewport.x, (int)viewport.y,
            (int)viewport.width, (int)viewport.height);
    return 1;
}
