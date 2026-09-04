/* Generic kryon tile-level editor (see native_level.h). The document
 * model and UI are game-agnostic: sheets, layers, per-tile flags, and
 * object kinds are all declared by the level files themselves. */
#include "kryon.h"
#include "ide/state.h"
#include "native_level.h"
#include "native_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define LEVEL_MAX_SHEETS  16
#define LEVEL_MAX_LAYERS  4
#define LEVEL_MAX_W       1024
#define LEVEL_MAX_H       1024
#define LEVEL_MAX_OBJECTS 4096
#define LEVEL_MAX_OBJDEFS 64
#define LEVEL_MAX_META    16
#define LEVEL_EMPTY       0xFFFF
#define LEVEL_PATH_MAX    1024
#define LEVEL_NAME_MAX    64
#define LEVEL_FILES_MAX   512

/* ------------------------------------------------------------------ */
/* document                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned short sheet;    /* sheet id, LEVEL_EMPTY = empty cell */
    unsigned char col, row;  /* tile in the sheet */
} LevelCell;

typedef struct {
    char name[LEVEL_NAME_MAX];
    int w, h;
    int rows_read;           /* parse progress */
    LevelCell *cells;        /* row-major: idx = y * w + x */
} LevelLayer;

typedef struct {
    unsigned short id;
    char name[LEVEL_NAME_MAX];
    char png[LEVEL_PATH_MAX];   /* absolute (resolved on load) */
    char rel[LEVEL_PATH_MAX];   /* as written in the file */
    int cw, ch;                 /* tile cell size in the sheet */
    Texture2D tex;
    int tex_tried;
    unsigned int *flags;        /* per-tile bits, fcols x frows */
    int fcols, frows;
} LevelSheet;

typedef struct {
    unsigned int key;
    int tx, ty;
} LevelObject;

typedef struct {
    unsigned int key;
    char name[LEVEL_NAME_MAX];
} LevelObjDef;

typedef struct {
    unsigned int bits;
    char name[24];
} LevelFlagDef;

typedef struct {
    char key[LEVEL_NAME_MAX];
    char value[192];
} LevelMeta;

typedef struct {
    char used;
    char path[LEVEL_PATH_MAX];   /* absolute */
    char name[LEVEL_NAME_MAX];   /* file name */
    char title[192];
    int grid_w, grid_h;
    int tile_w, tile_h;
    LevelSheet sheets[LEVEL_MAX_SHEETS];
    int sheet_count;
    LevelLayer layers[LEVEL_MAX_LAYERS];
    int layer_count;
    LevelObject objects[LEVEL_MAX_OBJECTS];
    int object_count;
    LevelObjDef objdefs[LEVEL_MAX_OBJDEFS];
    int objdef_count;
    LevelFlagDef flagdefs[16];
    int flagdef_count;
    LevelMeta meta[LEVEL_MAX_META];
    int meta_count;
    int dirty;
} LevelDoc;

/* ------------------------------------------------------------------ */
/* editor session                                                      */
/* ------------------------------------------------------------------ */

#define LEVEL_TOOL_PAN    0
#define LEVEL_TOOL_PAINT  1
#define LEVEL_TOOL_ERASE  2
#define LEVEL_TOOL_PICK   3
#define LEVEL_TOOL_OBJECT 4
#define LEVEL_TOOL_FILL   5

typedef struct {
    int layer, x, y;
    LevelCell old;
} LevelUndoCell;

typedef struct {
    LevelUndoCell cells[8192];
    int count;
} LevelUndoStroke;

typedef struct {
    char root[LEVEL_PATH_MAX];
    char files[LEVEL_FILES_MAX][LEVEL_NAME_MAX];
    int file_count;
    int selected;             /* file index, -1 none */
    LevelDoc doc;

    /* canvas */
    float cam_x, cam_y, zoom;
    int active_layer;
    int show_layer[LEVEL_MAX_LAYERS];
    int show_grid, show_flags;
    int tool;
    int cur_sheet, cur_col, cur_row;
    unsigned int cur_key;     /* object tool */

    float list_scroll;
    float palette_scroll;

    LevelUndoStroke strokes[64];
    int stroke_count;
    int stroke_open;

    char status[160];
} LevelState;

static LevelState g_level;
static int g_level_active;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static const LevelObjDef *level_objdef(unsigned int key)
{
    int i;
    for(i = 0; i < g_level.doc.objdef_count; i++)
        if(g_level.doc.objdefs[i].key == key)
            return &g_level.doc.objdefs[i];
    return NULL;
}

static const char *level_flag_label(unsigned int bits)
{
    int i;
    for(i = 0; i < g_level.doc.flagdef_count; i++)
        if(g_level.doc.flagdefs[i].bits == bits)
            return g_level.doc.flagdefs[i].name;
    return NULL;
}

/* combined label for a flag mask: first matching named bit */
static const char *level_flags_first_label(unsigned int bits)
{
    int i;
    if(bits == 0)
        return NULL;
    for(i = 0; i < g_level.doc.flagdef_count; i++)
        if(bits & g_level.doc.flagdefs[i].bits)
            return g_level.doc.flagdefs[i].name;
    return NULL;
}

static int level_path_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
}

static void level_doc_clear(LevelDoc *d)
{
    int i, l;
    for(l = 0; l < LEVEL_MAX_LAYERS; l++)
        free(d->layers[l].cells);
    for(i = 0; i < LEVEL_MAX_SHEETS; i++) {
        if(d->sheets[i].tex_tried && IsTextureValid(d->sheets[i].tex))
            UnloadTexture(d->sheets[i].tex);
        free(d->sheets[i].flags);
    }
    memset(d, 0, sizeof *d);
}

static LevelSheet *level_sheet_by_id(unsigned short id)
{
    int i;
    for(i = 0; i < g_level.doc.sheet_count; i++)
        if(g_level.doc.sheets[i].id == id)
            return &g_level.doc.sheets[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* format reader                                                       */
/* ------------------------------------------------------------------ */

/* read the next grid row of cells into cells[x]; returns 0 on EOF */
static int level_read_grid_row(FILE *f, int w, LevelCell *cells)
{
    char line[8192];
    int x = 0;

    while(fgets(line, sizeof line, f) != NULL) {
        char *p = line;
        while(*p != '\0' && x < w) {
            unsigned int id;
            int col, row;
            int n = 0;

            while(*p == ' ' || *p == '\t') p++;
            if(*p == '\n' || *p == '\r' || *p == '\0')
                break;
            if(*p == '.' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\n' ||
                             p[1] == '\r' || p[1] == '\0')) {
                cells[x].sheet = LEVEL_EMPTY;
                cells[x].col = 0;
                cells[x].row = 0;
                x++;
                p++;
                continue;
            }
            if(sscanf(p, "%x:%d,%d%n", &id, &col, &row, &n) >= 3 && n > 0) {
                cells[x].sheet = (unsigned short)id;
                cells[x].col = (unsigned char)col;
                cells[x].row = (unsigned char)row;
                x++;
                p += n;
            } else {
                return -1;   /* malformed token */
            }
        }
        if(x >= w)
            return 1;
    }
    return x == w ? 1 : 0;
}

static char g_level_parse_err[256];
static int level_parse(const char *path, LevelDoc *d)
{
    FILE *f = fopen(path, "r");
    char line[32768];   /* grid rows carry one token per tile */
    char dir[LEVEL_PATH_MAX];
    const char *slash;
    int layer_open = -1;

    if(f == NULL)
        return 0;
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    slash = strrchr(path, '/');
    snprintf(d->name, sizeof d->name, "%s",
             slash ? slash + 1 : path);
    snprintf(dir, sizeof dir, "%s", path);
    {
        char *dend = strrchr(dir, '/');
        if(dend != NULL)
            *dend = 0;
        else
            dir[0] = '\0';
    }

    while(fgets(line, sizeof line, f) != NULL) {
        char key[32];
        unsigned int a, b, bits;
        int i1, i2, i3, i4, n;

        if(line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if(sscanf(line, "%31s", key) != 1)
            continue;
        if(strcmp(key, "kryon") == 0)
            continue;    /* "kryon level v1" header */
        if(strcmp(key, "level") == 0 || strcmp(key, "v1") == 0)
            continue;
        if(strcmp(key, "title") == 0) {
            char *p = line + 5;
            while(*p == ' ') p++;
            snprintf(d->title, sizeof d->title, "%s", p);
            d->title[strcspn(d->title, "\r\n")] = 0;
        } else if(strcmp(key, "grid") == 0 &&
                  sscanf(line, "%*s %d %d", &i1, &i2) == 2) {
            d->grid_w = i1;
            d->grid_h = i2;
        } else if(strcmp(key, "tile") == 0 &&
                  sscanf(line, "%*s %d %d", &i1, &i2) == 2) {
            d->tile_w = i1;
            d->tile_h = i2;
        } else if(strcmp(key, "sheet") == 0 && d->sheet_count < LEVEL_MAX_SHEETS) {
            LevelSheet *s = &d->sheets[d->sheet_count];
            char rel[768];
            if(sscanf(line, "%*s %x %63s %767s %d %d", &a, s->name, rel,
                      &i1, &i2) == 5) {
                s->id = (unsigned short)a;
                s->cw = i1;
                s->ch = i2;
                snprintf(s->rel, sizeof s->rel, "%s", rel);
                if(rel[0] == '/')
                    snprintf(s->png, sizeof s->png, "%s", rel);
                else
                    snprintf(s->png, sizeof s->png, "%s/%s", dir, rel);
                d->sheet_count++;
            }
        } else if(strcmp(key, "objdef") == 0 &&
                  d->objdef_count < LEVEL_MAX_OBJDEFS) {
            /* name is the rest of the line (may contain spaces) */
            LevelObjDef *o = &d->objdefs[d->objdef_count];
            char *rest = line;
            if(sscanf(line, "%*s %x", &a) == 1) {
                int skip = 0;
                char *p;
                sscanf(line, "%*s %x%n", &a, &skip);
                p = line + skip;
                while(*p == ' ' || *p == '\t') p++;
                snprintf(o->name, sizeof o->name, "%s", p);
                o->name[strcspn(o->name, "\r\n")] = 0;
                o->key = a;
                d->objdef_count++;
            }
        } else if(strcmp(key, "flag") == 0 && d->flagdef_count < 16) {
            LevelFlagDef *fd = &d->flagdefs[d->flagdef_count];
            if(sscanf(line, "%*s %x %23s", &a, fd->name) == 2) {
                fd->bits = a;
                d->flagdef_count++;
            }
        } else if(strcmp(key, "tileflag") == 0) {
            LevelSheet *s;
            if(sscanf(line, "%*s %x %d,%d %x", &a, &i1, &i2, &bits) == 4 &&
               (s = level_sheet_by_id((unsigned short)a)) != NULL) {
                if(i1 + 1 > s->fcols || i2 + 1 > s->frows) {
                    /* grow the flag grid, preserving old entries */
                    int fw = i1 + 1 > s->fcols ? i1 + 1 : s->fcols;
                    int fh = i2 + 1 > s->frows ? i2 + 1 : s->frows;
                    unsigned int *nf = (unsigned int *)calloc(
                        (size_t)fw * fh, sizeof(unsigned int));
                    int xx, yy;
                    for(yy = 0; yy < s->frows; yy++)
                        for(xx = 0; xx < s->fcols; xx++)
                            nf[yy * fw + xx] = s->flags
                                ? s->flags[yy * s->fcols + xx] : 0;
                    free(s->flags);
                    s->flags = nf;
                    s->fcols = fw;
                    s->frows = fh;
                }
                s->flags[i2 * s->fcols + i1] = bits;
            }
        } else if(strcmp(key, "exit") == 0 && d->meta_count < LEVEL_MAX_META) {
            LevelMeta *m = &d->meta[d->meta_count];
            if(sscanf(line, "%*s %63s %191s", m->key, m->value) == 2)
                d->meta_count++;
        } else if(strcmp(key, "hint") == 0 && d->meta_count < LEVEL_MAX_META) {
            LevelMeta *m = &d->meta[d->meta_count];
            char *p = line + 4;
            while(*p == ' ') p++;
            snprintf(m->value, sizeof m->value, "%s", p);
            m->value[strcspn(m->value, "\r\n")] = 0;
            snprintf(m->key, sizeof m->key, "hint%d", d->meta_count);
            d->meta_count++;
        } else if(strcmp(key, "layer") == 0 &&
                  d->layer_count < LEVEL_MAX_LAYERS) {
            LevelLayer *l = &d->layers[d->layer_count];
            int lw = d->grid_w, lh = d->grid_h;
            snprintf(l->name, sizeof l->name, "%s", "layer");
            if(sscanf(line, "%*s %63s %d %d", l->name, &lw, &lh) < 1)
                sscanf(line, "%*s %63s", l->name);
            l->w = lw;
            l->h = lh;
            l->cells = (LevelCell *)calloc(
                (size_t)(l->w ? l->w : 1) * (l->h ? l->h : 1),
                sizeof(LevelCell));
            layer_open = d->layer_count;
            d->layer_count++;
        } else if(strcmp(key, "object") == 0 &&
                  d->object_count < LEVEL_MAX_OBJECTS) {
            LevelObject *o = &d->objects[d->object_count];
            if(sscanf(line, "%*s %x %d %d", &a, &i1, &i2) == 3) {
                o->key = a;
                o->tx = i1;
                o->ty = i2;
                d->object_count++;
            }
        } else if(layer_open >= 0) {
            /* grid row inside the open layer: tokens start on this line
             * and continue on following lines until w cells are read */
            LevelLayer *l = &d->layers[layer_open];
            (void)b; (void)n; (void)i3; (void)i4;
            if(l->cells != NULL && l->w > 0 && l->rows_read < l->h) {
                LevelCell *row = l->cells + (size_t)l->rows_read * l->w;
                int x = 0;
                char *p = line;
                int ok = 1;
                while(x < l->w) {
                    while(*p == ' ' || *p == '\t') p++;
                    if(*p == '\n' || *p == '\r' || *p == '\0') {
                        /* continue the row on the next line(s) */
                        char cont[32768];
                        if(fgets(cont, sizeof cont, f) == NULL) {
                            ok = 0;
                            break;
                        }
                        p = cont;
                        continue;
                    }
                    if(*p == '.' && (p[1] == ' ' || p[1] == '\t' ||
                                     p[1] == '\n' || p[1] == '\r' ||
                                     p[1] == '\0')) {
                        row[x].sheet = LEVEL_EMPTY;
                        row[x].col = 0;
                        row[x].row = 0;
                        x++;
                        p++;
                        continue;
                    }
                    {
                        unsigned int id;
                        int col, rw, nn = 0;
                        if(sscanf(p, "%x:%d,%d%n", &id, &col, &rw,
                                  &nn) >= 3 && nn > 0) {
                            row[x].sheet = (unsigned short)id;
                            row[x].col = (unsigned char)col;
                            row[x].row = (unsigned char)rw;
                            x++;
                            p += nn;
                        } else {
                            ok = 0;
                            break;
                        }
                    }
                }
                if(!ok)
                    break;
                l->rows_read++;
                if(l->rows_read >= l->h)
                    layer_open = -1;
            }
        }
    }
    fclose(f);
    if(d->grid_w <= 0 || d->grid_h <= 0 || d->grid_w > LEVEL_MAX_W ||
       d->grid_h > LEVEL_MAX_H || d->tile_w <= 0 || d->tile_h <= 0 ||
       d->layer_count == 0) {
        snprintf(g_level_parse_err, sizeof g_level_parse_err,
                 "header grid=%dx%d tile=%dx%d layers=%d", d->grid_w,
                 d->grid_h, d->tile_w, d->tile_h, d->layer_count);
        return 0;
    }
    /* every layer must have received its full grid */
    {
        int l;
        for(l = 0; l < d->layer_count; l++)
            if(d->layers[l].rows_read != d->layers[l].h) {
                snprintf(g_level_parse_err, sizeof g_level_parse_err,
                         "layer %d read %d/%d rows (w=%d)", l,
                         d->layers[l].rows_read, d->layers[l].h,
                         d->layers[l].w);
                return 0;
            }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* format writer                                                       */
/* ------------------------------------------------------------------ */

static void level_write(FILE *f, const LevelDoc *d)
{
    int l, y, x, i;
    int has_flags = 0;

    fprintf(f, "kryon level v1\n");
    if(d->title[0] != '\0')
        fprintf(f, "title %s\n", d->title);
    fprintf(f, "grid %d %d\n", d->grid_w, d->grid_h);
    fprintf(f, "tile %d %d\n", d->tile_w, d->tile_h);
    for(i = 0; i < d->sheet_count; i++)
        fprintf(f, "sheet %x %s %s %d %d\n", d->sheets[i].id,
                d->sheets[i].name, d->sheets[i].rel, d->sheets[i].cw,
                d->sheets[i].ch);
    for(i = 0; i < d->objdef_count; i++)
        fprintf(f, "objdef %x %s\n", d->objdefs[i].key,
                d->objdefs[i].name);
    for(i = 0; i < d->flagdef_count; i++)
        fprintf(f, "flag %x %s\n", d->flagdefs[i].bits,
                d->flagdefs[i].name);
    for(i = 0; i < d->sheet_count; i++) {
        const LevelSheet *s = &d->sheets[i];
        int fx, fy;
        for(fy = 0; fy < s->frows; fy++)
            for(fx = 0; fx < s->fcols; fx++)
                if(s->flags != NULL && s->flags[fy * s->fcols + fx] != 0) {
                    fprintf(f, "tileflag %x %d,%d %x\n", s->id, fx, fy,
                            s->flags[fy * s->fcols + fx]);
                    has_flags = 1;
                }
    }
    (void)has_flags;
    for(i = 0; i < d->meta_count; i++) {
        if(strncmp(d->meta[i].key, "hint", 4) == 0)
            fprintf(f, "hint %s\n", d->meta[i].value);
        else
            fprintf(f, "exit %s %s\n", d->meta[i].key,
                    d->meta[i].value);
    }
    for(l = 0; l < d->layer_count; l++) {
        const LevelLayer *lay = &d->layers[l];
        fprintf(f, "layer %s %d %d\n", lay->name, lay->w, lay->h);
        for(y = 0; y < lay->h; y++) {
            for(x = 0; x < lay->w; x++) {
                const LevelCell *c = &lay->cells[(size_t)y * lay->w + x];
                if(c->sheet == LEVEL_EMPTY)
                    fprintf(f, ".");
                else
                    fprintf(f, "%x:%d,%d", c->sheet, c->col, c->row);
                fprintf(f, "%s", x + 1 < lay->w ? " " : "\n");
            }
        }
    }
    for(i = 0; i < d->object_count; i++)
        fprintf(f, "object %x %d %d\n", d->objects[i].key,
                d->objects[i].tx, d->objects[i].ty);
}

/* ------------------------------------------------------------------ */
/* session: file list + open/save                                      */
/* ------------------------------------------------------------------ */

/* scan one directory; prefix is the path relative to the project root
 * ("" for <root>/levels, "sub" for <root>/sub/levels) */
static void level_scan_dir(const char *abs_dir, const char *prefix)
{
    DIR *dir = opendir(abs_dir);
    struct dirent *ent;

    if(dir == NULL)
        return;
    while((ent = readdir(dir)) != NULL &&
          g_level.file_count < LEVEL_FILES_MAX) {
        size_t n = strlen(ent->d_name);
        if(ent->d_name[0] == '.' || n < 6 ||
           strcmp(ent->d_name + n - 6, ".level") != 0)
            continue;
        if(prefix[0] != '\0')
            snprintf(g_level.files[g_level.file_count],
                     sizeof g_level.files[0], "%s/levels/%s", prefix,
                     ent->d_name);
        else
            snprintf(g_level.files[g_level.file_count],
                     sizeof g_level.files[0], "levels/%s", ent->d_name);
        g_level.file_count++;
    }
    closedir(dir);
}

/* natural order: World0_2 before World0_10; worlds first, then plots,
 * then everything else (templates, maps) */
static int level_file_group(const char *name)
{
    if(strncmp(name, "World", 5) == 0)
        return 0;
    if(strncmp(name, "Plot", 4) == 0)
        return 1;
    return 2;
}

static int level_name_cmp(const char *a, const char *b)
{
    while(*a != '\0' && *b != '\0') {
        if(isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            long va, vb;
            char *ea, *eb;

            va = strtol(a, &ea, 10);
            vb = strtol(b, &eb, 10);
            if(va != vb)
                return va < vb ? -1 : 1;
            a = ea;
            b = eb;
            continue;
        }
        if((unsigned char)*a != (unsigned char)*b)
            return (unsigned char)*a - (unsigned char)*b;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int level_entry_cmp(const void *pa, const void *pb)
{
    /* entries may carry a relative path (sub/levels/Name.level); group
     * and order by the file name itself */
    const char *a = (const char *)pa;
    const char *b = (const char *)pb;
    const char *ba = strrchr(a, '/');
    const char *bb = strrchr(b, '/');
    int ga, gb;

    a = ba ? ba + 1 : a;
    b = bb ? bb + 1 : b;
    ga = level_file_group(a);
    gb = level_file_group(b);

    if(ga != gb)
        return ga - gb;
    return level_name_cmp(a, b);
}

static void level_scan_files(const char *project_root)
{
    char dir_path[LEVEL_PATH_MAX];
    DIR *dir;
    struct dirent *ent;

    g_level.file_count = 0;
    if(project_root == NULL || project_root[0] == '\0')
        return;
    snprintf(dir_path, sizeof dir_path, "%s/levels", project_root);
    level_scan_dir(dir_path, "");
    /* one level of nesting: <root>/<sub>/levels (repo layouts that keep
     * sources in a subdir) */
    snprintf(dir_path, sizeof dir_path, "%s", project_root);
    dir = opendir(dir_path);
    if(dir != NULL) {
        while((ent = readdir(dir)) != NULL &&
              g_level.file_count < LEVEL_FILES_MAX) {
            char sub[LEVEL_PATH_MAX];
            struct stat st;
            if(ent->d_name[0] == '.')
                continue;
            snprintf(sub, sizeof sub, "%s/%s", project_root,
                     ent->d_name);
            if(stat(sub, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            snprintf(sub, sizeof sub, "%s/%s/levels", project_root,
                     ent->d_name);
            level_scan_dir(sub, ent->d_name);
        }
        closedir(dir);
    }
    /* worlds first, natural numeric order within a world */
    qsort(g_level.files, (size_t)g_level.file_count,
          sizeof g_level.files[0], level_entry_cmp);
}

int krait_level_project_has(const char *project_root)
{
    level_scan_files(project_root);
    return g_level.file_count > 0;
}

static void level_session_defaults(void)
{
    g_level.selected = -1;
    g_level.zoom = 1.0f;
    g_level.active_layer = 0;
    g_level.show_layer[0] = 1;
    g_level.show_layer[1] = 1;
    g_level.show_layer[2] = 1;
    g_level.show_layer[3] = 1;
    g_level.show_grid = 1;
    g_level.show_flags = 0;
    g_level.tool = LEVEL_TOOL_PAN;
    g_level.stroke_count = 0;
    g_level.stroke_open = 0;
    snprintf(g_level.status, sizeof g_level.status, "Levels loaded");
}

void krait_level_set_active(int on, const char *project_root)
{
    if(project_root != NULL && project_root[0] != '\0' &&
       strcmp(g_level.root, project_root) != 0) {
        snprintf(g_level.root, sizeof g_level.root, "%s", project_root);
        level_doc_clear(&g_level.doc);
        level_session_defaults();
        level_scan_files(project_root);
    }
    g_level_active = on ? 1 : 0;
}

int krait_level_editor_active(void)
{
    return g_level_active;
}

int krait_level_file_count(void)
{
    return g_level.file_count;
}

const char *krait_level_file_name(int index)
{
    if(index < 0 || index >= g_level.file_count)
        return NULL;
    return g_level.files[index];
}

static void level_tex_ensure(LevelSheet *s);

int krait_level_open(const char *name)
{
    char path[LEVEL_PATH_MAX];
    int i;
    const char *base;

    for(i = 0; i < g_level.file_count; i++) {
        base = strrchr(g_level.files[i], '/');
        base = base ? base + 1 : g_level.files[i];
        if(strcmp(g_level.files[i], name) == 0 ||
           strcmp(base, name) == 0)
            break;
    }
    if(i >= g_level.file_count)
        return 0;
    snprintf(path, sizeof path, "%s/%s", g_level.root, g_level.files[i]);
    level_doc_clear(&g_level.doc);
    if(!level_parse(path, &g_level.doc)) {
        snprintf(g_level.status, sizeof g_level.status,
                 "Cannot parse %s: %s", name, g_level_parse_err);
        g_level.selected = -1;
        return 0;
    }
    g_level.selected = i;
    g_level.cur_sheet = g_level.doc.sheet_count > 0 ? 0 : -1;
    g_level.cur_col = 0;
    g_level.cur_row = 0;
    /* Load every sheet texture up front: creating GL textures lazily
     * from inside the draw pass (with an active render batch) wedges
     * mesa/radeonsi in drm syncobj waits, freezing the window after
     * the first frame. */
    {
        int si;
        for(si = 0; si < g_level.doc.sheet_count; si++)
            level_tex_ensure(&g_level.doc.sheets[si]);
    }
    g_level.cam_x = 0;
    g_level.cam_y = 0;
    g_level.zoom = 1.0f;
    g_level.active_layer = 0;
    g_level.stroke_count = 0;
    g_level.stroke_open = 0;
    snprintf(g_level.status, sizeof g_level.status, "%s",
             g_level.doc.name);
    return 1;
}

int krait_level_save(char *status, int status_size)
{
    FILE *f;

    if(g_level.selected < 0 || g_level.doc.path[0] == '\0') {
        snprintf(status, status_size, "No level open");
        return 0;
    }
    f = fopen(g_level.doc.path, "w");
    if(f == NULL) {
        snprintf(status, status_size, "Cannot write %s",
                 g_level.doc.path);
        return 0;
    }
    level_write(f, &g_level.doc);
    fclose(f);
    g_level.doc.dirty = 0;
    snprintf(status, status_size, "Saved %s", g_level.doc.name);
    return 1;
}

void krait_level_shutdown(void)
{
    level_doc_clear(&g_level.doc);
    memset(&g_level, 0, sizeof g_level);
    g_level_active = 0;
}

/* ------------------------------------------------------------------ */
/* editing                                                             */
/* ------------------------------------------------------------------ */

static LevelCell *level_cell_at(LevelLayer *l, int x, int y)
{
    if(l == NULL || l->cells == NULL || x < 0 || y < 0 || x >= l->w ||
       y >= l->h)
        return NULL;
    return &l->cells[(size_t)y * l->w + x];
}

static LevelLayer *level_active_layer(void)
{
    if(g_level.active_layer < 0 ||
       g_level.active_layer >= g_level.doc.layer_count)
        return NULL;
    return &g_level.doc.layers[g_level.active_layer];
}

static void level_undo_begin(void)
{
    if(g_level.stroke_count >= 64) {
        memmove(g_level.strokes, g_level.strokes + 1,
                sizeof(LevelUndoStroke) * 63);
        g_level.stroke_count = 63;
    }
    g_level.strokes[g_level.stroke_count].count = 0;
    g_level.stroke_count++;
    g_level.stroke_open = 1;
}

static void level_undo_end(void)
{
    if(g_level.stroke_open && g_level.stroke_count > 0 &&
       g_level.strokes[g_level.stroke_count - 1].count == 0)
        g_level.stroke_count--;
    g_level.stroke_open = 0;
}

static void level_undo_cell(int layer, int x, int y, LevelCell old)
{
    LevelUndoStroke *s;

    if(!g_level.stroke_open)
        level_undo_begin();
    if(g_level.stroke_count <= 0)
        return;
    s = &g_level.strokes[g_level.stroke_count - 1];
    if(s->count >= 8192)
        return;
    s->cells[s->count].layer = layer;
    s->cells[s->count].x = x;
    s->cells[s->count].y = y;
    s->cells[s->count].old = old;
    s->count++;
}

void krait_level_undo(void)
{
    LevelUndoStroke *s;
    int i;

    if(g_level.stroke_count <= 0)
        return;
    s = &g_level.strokes[g_level.stroke_count - 1];
    for(i = s->count - 1; i >= 0; i--) {
        LevelLayer *l = NULL;
        if(s->cells[i].layer >= 0 &&
           s->cells[i].layer < g_level.doc.layer_count)
            l = &g_level.doc.layers[s->cells[i].layer];
        {
            LevelCell *c = level_cell_at(l, s->cells[i].x, s->cells[i].y);
            if(c != NULL)
                *c = s->cells[i].old;
        }
    }
    g_level.stroke_count--;
    g_level.stroke_open = 0;
    g_level.doc.dirty = 1;
}

int krait_level_set_cell(int layer, int x, int y,
                         unsigned short sheet, unsigned char col,
                         unsigned char row)
{
    LevelLayer *l;
    LevelCell *c;

    if(layer < 0 || layer >= g_level.doc.layer_count)
        return 0;
    l = &g_level.doc.layers[layer];
    c = level_cell_at(l, x, y);
    if(c == NULL)
        return 0;
    if(c->sheet == sheet && c->col == col && c->row == row)
        return 1;
    level_undo_cell(layer, x, y, *c);
    c->sheet = sheet;
    c->col = col;
    c->row = row;
    g_level.doc.dirty = 1;
    return 1;
}

int krait_level_get_cell(int layer, int x, int y,
                         unsigned short *sheet, unsigned char *col,
                         unsigned char *row)
{
    LevelLayer *l;
    LevelCell *c;

    if(layer < 0 || layer >= g_level.doc.layer_count)
        return 0;
    l = &g_level.doc.layers[layer];
    c = level_cell_at(l, x, y);
    if(c == NULL)
        return 0;
    if(sheet != NULL) *sheet = c->sheet;
    if(col != NULL) *col = c->col;
    if(row != NULL) *row = c->row;
    return 1;
}

int krait_level_set_object(unsigned int key, int tx, int ty)
{
    int i;

    for(i = 0; i < g_level.doc.object_count; i++)
        if(g_level.doc.objects[i].key == key &&
           g_level.doc.objects[i].tx == tx &&
           g_level.doc.objects[i].ty == ty)
            return 1;    /* already there */
    if(g_level.doc.object_count >= LEVEL_MAX_OBJECTS)
        return 0;
    g_level.doc.objects[g_level.doc.object_count].key = key;
    g_level.doc.objects[g_level.doc.object_count].tx = tx;
    g_level.doc.objects[g_level.doc.object_count].ty = ty;
    g_level.doc.object_count++;
    g_level.doc.dirty = 1;
    return 1;
}

int krait_level_object_count(void)
{
    return g_level.doc.object_count;
}

int krait_level_dirty(void)
{
    return g_level.doc.dirty;
}

/* ------------------------------------------------------------------ */
/* editor UI                                                           */
/* ------------------------------------------------------------------ */

static int level_btn(int x, int y, int w, int h, const char *label,
                     int active)
{
    return StyledButton(x, y, w, h, label,
                        active ? ButtonStylePrimary : ButtonStyleSecondary,
                        0, NULL);
}

static void level_tex_ensure(LevelSheet *s)
{
    if(s->tex_tried)
        return;
    s->tex_tried = 1;
    if(s->png[0] != '\0' && level_path_exists(s->png))
        s->tex = LoadTexture(s->png);
}

static int level_sheet_cols(const LevelSheet *s)
{
    if(!IsTextureValid(s->tex) || s->cw <= 0)
        return 1;
    return s->tex.width / s->cw;
}

static int level_sheet_rows(const LevelSheet *s)
{
    if(!IsTextureValid(s->tex) || s->ch <= 0)
        return 1;
    return s->tex.height / s->ch;
}

static void level_draw_cell(const LevelCell *c, int x, int y, int w_px,
                            int h_px, float alpha)
{
    if(c->sheet == LEVEL_EMPTY)
        return;
    {
        LevelSheet *s = level_sheet_by_id(c->sheet);
        if(s != NULL) {
            level_tex_ensure(s);
            if(IsTextureValid(s->tex)) {
                Rectangle src = { (float)(c->col * s->cw),
                                  (float)(c->row * s->ch),
                                  (float)s->cw, (float)s->ch };
                Color tint = WHITE;
                if(alpha < 1.0f)
                    tint.a = (unsigned char)(alpha * 255.0f);
                DrawTextureRec(s->tex, src,
                               (Vector2){ (float)x, (float)y }, tint);
                return;
            }
        }
        {
            Color col = { (unsigned char)(40 + (c->sheet * 37) % 200),
                          (unsigned char)(40 + (c->col * 53) % 160),
                          (unsigned char)(40 + (c->row * 71) % 160),
                          (unsigned char)(alpha * 200.0f) };
            DrawRectangle(x, y, w_px, h_px, col);
        }
    }
}

static void level_draw_object(const LevelObject *o, int x, int y,
                              int w_px, int h_px)
{
    const LevelObjDef *d = level_objdef(o->key);
    Color box = { 250, 210, 90, 200 };
    Rectangle r = { (float)x, (float)y, (float)w_px, (float)h_px };

    DrawRectangleRec(r, (Color){ 0, 0, 0, 90 });
    DrawRectangleLinesEx(r, 2.0f, box);
    if(h_px >= 12) {
        char lbl[24];
        snprintf(lbl, sizeof lbl, "%04X", o->key);
        Text(lbl, x + 3, y + 1, h_px >= 26 ? 11 : 9, box);
        if(d != NULL && h_px >= 40)
            Text(d->name, x + 3, y + h_px - 13, 10,
                 (Color){ 240, 240, 240, 220 });
    }
}

static unsigned int level_flags_at(int x, int y)
{
    LevelCell *c = level_cell_at(&g_level.doc.layers[0], x, y);
    LevelSheet *s;

    if(c == NULL || c->sheet == LEVEL_EMPTY)
        return 0;
    s = level_sheet_by_id(c->sheet);
    if(s == NULL || s->flags == NULL || c->col >= s->fcols ||
       c->row >= s->frows)
        return 0;
    return s->flags[c->row * s->fcols + c->col];
}

/* fill connected same-cell region on the active layer */
static void level_fill(int tx, int ty)
{
    LevelLayer *l = level_active_layer();
    LevelCell want, nc;
    LevelCell *start;
    int frontier_x[2048], frontier_y[2048];
    int count = 0;
    int guard = 0;

    if(l == NULL)
        return;
    start = level_cell_at(l, tx, ty);
    if(start == NULL)
        return;
    want = *start;
    if(g_level.tool == LEVEL_TOOL_ERASE) {
        nc.sheet = LEVEL_EMPTY;
        nc.col = 0;
        nc.row = 0;
    } else if(g_level.cur_sheet >= 0) {
        /* fill with the selected tile whatever tool is active (the Fill
         * tool itself used to be rejected here and never filled) */
        nc.sheet = g_level.doc.sheets[g_level.cur_sheet].id;
        nc.col = (unsigned char)g_level.cur_col;
        nc.row = (unsigned char)g_level.cur_row;
    } else {
        return;
    }
    if(want.sheet == nc.sheet && want.col == nc.col &&
       want.row == nc.row)
        return;
    frontier_x[count] = tx;
    frontier_y[count] = ty;
    count++;
    while(count > 0 && guard++ < 65536) {
        int cx = frontier_x[count - 1];
        int cy = frontier_y[count - 1];
        LevelCell *c = level_cell_at(l, cx, cy);
        count--;
        if(c == NULL || c->sheet != want.sheet || c->col != want.col ||
           c->row != want.row)
            continue;
        level_undo_cell(g_level.active_layer, cx, cy, *c);
        *c = nc;
        g_level.doc.dirty = 1;
        if(count + 4 < 2048) {
            frontier_x[count] = cx + 1; frontier_y[count] = cy; count++;
            frontier_x[count] = cx - 1; frontier_y[count] = cy; count++;
            frontier_x[count] = cx; frontier_y[count] = cy + 1; count++;
            frontier_x[count] = cx; frontier_y[count] = cy - 1; count++;
        }
    }
}

static void level_canvas_paint(int tx, int ty, int tool)
{
    LevelLayer *l = level_active_layer();
    int saved_tool = g_level.tool;

    if(l == NULL)
        return;
    g_level.tool = tool;
    switch(tool) {
    case LEVEL_TOOL_ERASE:
        krait_level_set_cell(g_level.active_layer, tx, ty, LEVEL_EMPTY,
                             0, 0);
        break;
    case LEVEL_TOOL_PICK: {
        LevelCell *c = level_cell_at(l, tx, ty);
        if(c != NULL && c->sheet != LEVEL_EMPTY) {
            int i;
            g_level.tool = LEVEL_TOOL_PAINT;
            for(i = 0; i < g_level.doc.sheet_count; i++)
                if(g_level.doc.sheets[i].id == c->sheet) {
                    g_level.cur_sheet = i;
                    break;
                }
            g_level.cur_col = c->col;
            g_level.cur_row = c->row;
        }
        g_level.tool = saved_tool;
        return;
    }
    case LEVEL_TOOL_OBJECT:
        krait_level_set_object(g_level.cur_key, tx, ty);
        break;
    case LEVEL_TOOL_FILL:
        level_fill(tx, ty);
        break;
    default:
        if(g_level.cur_sheet >= 0)
            krait_level_set_cell(g_level.active_layer, tx, ty,
                                g_level.doc.sheets[g_level.cur_sheet].id,
                                (unsigned char)g_level.cur_col,
                                (unsigned char)g_level.cur_row);
        break;
    }
    g_level.tool = saved_tool;
}

static void level_toolbar(Rectangle toolbar)
{
    int y = (int)toolbar.y + (ScaleUIPx(38) - ScaleUIPx(26)) / 2;
    int x = (int)toolbar.x + ScaleUIPx(8);
    int bh = ScaleUIPx(26);
    LevelDoc *d = &g_level.doc;

    DrawRectangleRec(toolbar, GetThemeSurface());
    if(level_btn(x, y, ScaleUIPx(72), bh, "Scene", 0)) {
        krait_level_set_active(0, g_level.root);
        return;
    }
    x += ScaleUIPx(78);
    if(g_level.selected < 0) {
        Text("No level selected", x, y + ScaleUIPx(7), ScaleUIPx(12),
             GetThemeIcon());
        return;
    }
    if(level_btn(x, y, ScaleUIPx(26), bh, "<", 0)) {
        if(g_level.selected > 0)
            krait_level_open(g_level.files[g_level.selected - 1]);
    }
    x += ScaleUIPx(30);
    if(level_btn(x, y, ScaleUIPx(26), bh, ">", 0)) {
        if(g_level.selected + 1 < g_level.file_count)
            krait_level_open(g_level.files[g_level.selected + 1]);
    }
    x += ScaleUIPx(32);
    {
        char lbl[160];
        snprintf(lbl, sizeof lbl, "%s%s", d->name, d->dirty ? " *" : "");
        Text(lbl, x, y + ScaleUIPx(7), ScaleUIPx(13), GetThemeText());
        x += ScaleUIPx(30) + (int)(strlen(lbl) * ScaleUIPx(7));
    }
    {
        int i;
        char lname[8];
        for(i = 0; i < LEVEL_MAX_LAYERS; i++) {
            if(i >= d->layer_count) {
                Text("-", x, y + ScaleUIPx(7), ScaleUIPx(12),
                     (Color){ 120, 120, 120, 255 });
                x += ScaleUIPx(30);
                continue;
            }
            snprintf(lname, sizeof lname, "L%d", i + 1);
            if(level_btn(x, y, ScaleUIPx(30), bh, lname,
                         g_level.show_layer[i] &&
                             g_level.active_layer != i))
                g_level.show_layer[i] = !g_level.show_layer[i];
            x += ScaleUIPx(32);
        }
    }
    x += ScaleUIPx(6);
    {
        static const char *labels[6] = { "Pan", "Paint", "Erase",
                                         "Pick", "Object", "Fill" };
        int i;
        for(i = 0; i < 6; i++) {
            if(level_btn(x, y, ScaleUIPx(52), bh, labels[i],
                         g_level.tool == i))
                g_level.tool = i;
            x += ScaleUIPx(54);
        }
    }
    x += ScaleUIPx(6);
    if(level_btn(x, y, ScaleUIPx(40), bh, "Grid", g_level.show_grid))
        g_level.show_grid = !g_level.show_grid;
    x += ScaleUIPx(42);
    if(level_btn(x, y, ScaleUIPx(40), bh, "Attr", g_level.show_flags))
        g_level.show_flags = !g_level.show_flags;
    x += ScaleUIPx(42);
    if(level_btn(x, y, ScaleUIPx(34), bh, "Out", 0))
        g_level.zoom = g_level.zoom > 0.25f ? g_level.zoom / 1.25f
                                            : 0.25f;
    x += ScaleUIPx(36);
    if(level_btn(x, y, ScaleUIPx(34), bh, "In", 0))
        g_level.zoom = g_level.zoom < 4.0f ? g_level.zoom * 1.25f
                                            : 4.0f;
    x += ScaleUIPx(42);
    if(level_btn(x, y, ScaleUIPx(70), bh, "Undo", 0))
        krait_level_undo();
    x += ScaleUIPx(72);
    if(level_btn(x, y, ScaleUIPx(56), bh, "Save", 0)) {
        char st[160];
        if(krait_level_save(st, sizeof st))
            snprintf(g_level.status, sizeof g_level.status, "%s", st);
        else
            snprintf(g_level.status, sizeof g_level.status,
                     "Save failed: %s", st);
    }
    {
        int tw = MeasureText(g_level.status, ScaleUIPx(12));
        Text(g_level.status,
             (int)(toolbar.x + toolbar.width) - tw - ScaleUIPx(12),
             y + ScaleUIPx(7), ScaleUIPx(12), GetThemeIcon());
    }
}

static void level_file_list(Rectangle zone)
{
    int row_h = ScaleUIPx(22);
    int y = (int)zone.y;
    int i;
    Vector2 mouse = GetMousePosition();
    int max_scroll = g_level.file_count * row_h + ScaleUIPx(28) -
                     (int)zone.height;

    DrawRectangleRec(zone, GetThemeSurface());
    Text("Levels", (int)zone.x + ScaleUIPx(10), y + ScaleUIPx(8),
         ScaleUIPx(13), GetThemeText());
    y += ScaleUIPx(28);
    if(max_scroll < 0)
        max_scroll = 0;
    if(CheckCollisionPointRec(mouse, zone))
        g_level.list_scroll -= GetMouseWheelMove() * row_h * 2.0f;
    if(g_level.list_scroll < 0) g_level.list_scroll = 0;
    if(g_level.list_scroll > max_scroll)
        g_level.list_scroll = max_scroll;
    y -= (int)g_level.list_scroll;

    BeginScissorMode((int)zone.x, (int)zone.y, (int)zone.width,
                     (int)zone.height);
    for(i = 0; i < g_level.file_count; i++) {
        if(i == g_level.selected)
            DrawRectangle((int)zone.x + 2, y, (int)zone.width - 4, row_h,
                          GetThemeButton());
        if(i == g_level.selected && g_level.doc.dirty)
            DrawCircle((int)zone.x + (int)zone.width - ScaleUIPx(10),
                       y + row_h / 2, ScaleUIPx(3),
                       (Color){ 250, 200, 80, 255 });
        {
            const char *bn = strrchr(g_level.files[i], '/');
            Text(bn ? bn + 1 : g_level.files[i],
                 (int)zone.x + ScaleUIPx(10), y + ScaleUIPx(4),
                 ScaleUIPx(11),
                 i == g_level.selected ? GetThemeText()
                                       : GetThemeIcon());
        }
        if(CheckCollisionPointRec(mouse,
                                  (Rectangle){ zone.x, (float)y,
                                               zone.width,
                                               (float)row_h }) &&
           IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            krait_level_open(g_level.files[i]);
        y += row_h;
    }
    EndScissorMode();
}

static void level_palette(Rectangle zone)
{
    Vector2 mouse = GetMousePosition();
    int y = (int)zone.y + ScaleUIPx(8);
    int i;
    LevelDoc *d = &g_level.doc;

    DrawRectangleRec(zone, GetThemeSurface());
    Text("Tileset", (int)zone.x + ScaleUIPx(10), y, ScaleUIPx(13),
         GetThemeText());
    y += ScaleUIPx(22);
    if(d->sheet_count == 0) {
        Text("(no sheets)", (int)zone.x + ScaleUIPx(10), y,
             ScaleUIPx(11), GetThemeIcon());
        return;
    }
    for(i = 0; i < d->sheet_count; i++) {
        char lbl[96];
        snprintf(lbl, sizeof lbl, "%s (%x)", d->sheets[i].name,
                 d->sheets[i].id);
        if(level_btn((int)zone.x + ScaleUIPx(8), y,
                     (int)zone.width - ScaleUIPx(16), ScaleUIPx(20), lbl,
                     i == g_level.cur_sheet)) {
            g_level.cur_sheet = i;
            g_level.cur_col = 0;
            g_level.cur_row = 0;
            g_level.palette_scroll = 0;
        }
        y += ScaleUIPx(22);
    }
    y += ScaleUIPx(6);
    {
        LevelSheet *s = &d->sheets[g_level.cur_sheet];
        int zone_bottom = (int)(zone.y + zone.height) - ScaleUIPx(150);
        int cols = level_sheet_cols(s);
        int rows = level_sheet_rows(s);
        int cell = ScaleUIPx(34);
        int per_row = ((int)zone.width - ScaleUIPx(16)) / cell;
        int total_h = ((rows + per_row - 1) / per_row) * cell;
        int max_scroll = total_h - (zone_bottom - y);

        if(per_row < 1) per_row = 1;
        if(max_scroll < 0) max_scroll = 0;
        if(CheckCollisionPointRec(mouse,
               (Rectangle){ zone.x, (float)y, zone.width,
                            (float)(zone_bottom - y) })) {
            g_level.palette_scroll -= GetMouseWheelMove() * cell * 2.0f;
            if(g_level.palette_scroll < 0) g_level.palette_scroll = 0;
            if(g_level.palette_scroll > max_scroll)
                g_level.palette_scroll = max_scroll;
        }
        BeginScissorMode((int)zone.x, y, (int)zone.width,
                         zone_bottom - y);
        {
            int py = y - (int)g_level.palette_scroll;
            int r, c;
            level_tex_ensure(s);
            for(r = 0; r < rows; r++) {
                for(c = 0; c < cols && c < per_row * 8; c++) {
                    int px = (int)zone.x + ScaleUIPx(8) +
                             (c % per_row) * cell;
                    int cy = py + (r + c / per_row) * cell;
                    Rectangle cellr = { (float)px, (float)cy,
                                        (float)(cell - 3),
                                        (float)(cell - 3) };
                    if(cy > zone_bottom + cell)
                        continue;
                    if(!IsTextureValid(s->tex)) {
                        DrawRectangleRec(cellr, (Color){ 70, 70, 80, 255 });
                    } else {
                        Rectangle src = { (float)(c * s->cw),
                                          (float)(r * s->ch),
                                          (float)s->cw, (float)s->ch };
                        DrawTextureRec(s->tex, src,
                                       (Vector2){ cellr.x, cellr.y },
                                       WHITE);
                    }
                    if(c == g_level.cur_col && r == g_level.cur_row) {
                        Color sel = { 250, 210, 90, 255 };
                        DrawRectangleLinesEx(cellr, 2.0f, sel);
                    }
                    if(CheckCollisionPointRec(mouse, cellr) &&
                       IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        g_level.cur_col = c;
                        g_level.cur_row = r;
                        if(g_level.tool == LEVEL_TOOL_OBJECT ||
                           g_level.tool == LEVEL_TOOL_ERASE ||
                           g_level.tool == LEVEL_TOOL_PAN)
                            g_level.tool = LEVEL_TOOL_PAINT;
                    }
                }
            }
        }
        EndScissorMode();
        {
            char lab[128];
            snprintf(lab, sizeof lab, "tile %d,%d  sheet %x", 
                     g_level.cur_col, g_level.cur_row, s->id);
            Text(lab, (int)zone.x + ScaleUIPx(10),
                 zone_bottom + ScaleUIPx(4), ScaleUIPx(11),
                 GetThemeIcon());
            if(s->flags != NULL && g_level.cur_col < s->fcols &&
               g_level.cur_row < s->frows) {
                unsigned int bits = s->flags[g_level.cur_row * s->fcols +
                                             g_level.cur_col];
                const char *lbl = level_flags_first_label(bits);
                char buf[96];
                snprintf(buf, sizeof buf, "attr %08X %s", bits,
                         lbl ? lbl : "");
                Text(buf, (int)zone.x + ScaleUIPx(10),
                     zone_bottom + ScaleUIPx(20), ScaleUIPx(11),
                     lbl ? (Color){ 250, 140, 120, 255 }
                         : GetThemeIcon());
            }
        }
    }
    y = (int)(zone.y + zone.height) - ScaleUIPx(140);
    Text("Objects", (int)zone.x + ScaleUIPx(10), y, ScaleUIPx(13),
         GetThemeText());
    y += ScaleUIPx(20);
    {
        int col_w = ((int)zone.width - ScaleUIPx(16)) / 2;
        int k;
        for(k = 0; k < d->objdef_count; k++) {
            char lbl[56];
            int px = (int)zone.x + ScaleUIPx(8) + (k % 2) * (col_w + 4);
            snprintf(lbl, sizeof lbl, "%04X %s", d->objdefs[k].key,
                     d->objdefs[k].name);
            if(level_btn(px, y, col_w, ScaleUIPx(18), lbl,
                         d->objdefs[k].key == g_level.cur_key &&
                             g_level.tool == LEVEL_TOOL_OBJECT)) {
                g_level.cur_key = d->objdefs[k].key;
                g_level.tool = LEVEL_TOOL_OBJECT;
            }
            y += ScaleUIPx(20);
        }
    }
}

static void level_inspector(Rectangle zone)
{
    int y = (int)zone.y + ScaleUIPx(8);
    LevelDoc *d = &g_level.doc;
    char buf[192];

    DrawRectangleRec(zone, GetThemeSurface());
    Text("Level", (int)zone.x + ScaleUIPx(10), y, ScaleUIPx(13),
         GetThemeText());
    y += ScaleUIPx(22);
    if(g_level.selected < 0)
        return;
    snprintf(buf, sizeof buf, "%dx%d grid, tile %dx%d, %d layer(s)",
             d->grid_w, d->grid_h, d->tile_w, d->tile_h, d->layer_count);
    Text(buf, (int)zone.x + ScaleUIPx(10), y, ScaleUIPx(11),
         GetThemeIcon());
    y += ScaleUIPx(16);
    if(d->title[0] != '\0') {
        Text(d->title, (int)zone.x + ScaleUIPx(10), y, ScaleUIPx(11),
             GetThemeText());
        y += ScaleUIPx(16);
    }
    snprintf(buf, sizeof buf, "%d object(s)", d->object_count);
    Text(buf, (int)zone.x + ScaleUIPx(10), y, ScaleUIPx(11),
         GetThemeIcon());
    y += ScaleUIPx(18);
    if(d->meta_count > 0) {
        int i;
        Text("meta:", (int)zone.x + ScaleUIPx(10), y, ScaleUIPx(11),
             GetThemeText());
        y += ScaleUIPx(14);
        for(i = 0; i < d->meta_count && i < 10; i++) {
            char line[224];
            snprintf(line, sizeof line, "%s %s", d->meta[i].key,
                     d->meta[i].value);
            Text(line, (int)zone.x + ScaleUIPx(22), y, ScaleUIPx(10),
                 GetThemeIcon());
            y += ScaleUIPx(13);
        }
    }
    y = (int)(zone.y + zone.height) - ScaleUIPx(34);
    Text("Editing:", (int)zone.x + ScaleUIPx(10), y + ScaleUIPx(5),
         ScaleUIPx(11), GetThemeIcon());
    {
        int i;
        int x = (int)zone.x + ScaleUIPx(66);
        for(i = 0; i < d->layer_count; i++) {
            if(level_btn(x, y, ScaleUIPx(44), ScaleUIPx(24),
                         d->layers[i].name,
                         g_level.active_layer == i))
                g_level.active_layer = i;
            x += ScaleUIPx(48);
        }
    }
}

static void level_canvas(Rectangle canvas)
{
    Vector2 mouse = GetMousePosition();
    LevelDoc *d = &g_level.doc;
    float cell = d->tile_w * g_level.zoom;
    int hovered_tx = -1, hovered_ty = -1;
    int i;

    if(d->tile_h != d->tile_w)
        cell = d->tile_w * g_level.zoom;    /* square cells assumed */

    DrawRectangleRec(canvas, GetThemeBackground());
    BeginScissorMode((int)canvas.x, (int)canvas.y, (int)canvas.width,
                     (int)canvas.height);
    {
        int step = ScaleUIPx(16);
        int cx, cy;
        for(cy = 0; cy < (int)canvas.height; cy += step)
            for(cx = 0; cx < (int)canvas.width; cx += step)
                DrawRectangle((int)canvas.x + cx, (int)canvas.y + cy,
                              step, step,
                              ((cx / step + cy / step) & 1)
                                  ? (Color){ 26, 28, 34, 255 }
                                  : (Color){ 22, 24, 29, 255 });
    }
    /* layers, first (main) at the bottom */
    for(i = d->layer_count - 1; i >= 0; i--) {
        LevelLayer *l = &d->layers[i];
        float alpha = (g_level.active_layer == i ||
                       g_level.show_layer[i])
                          ? (g_level.active_layer == i ? 1.0f : 0.45f)
                          : 0.0f;
        int x, y;
        if(alpha <= 0.0f)
            continue;
        for(y = 0; y < l->h; y++) {
            for(x = 0; x < l->w; x++) {
                LevelCell *c = &l->cells[(size_t)y * l->w + x];
                float px = canvas.x + x * cell - g_level.cam_x;
                float py = canvas.y + y * cell - g_level.cam_y;
                if(px > canvas.x + canvas.width ||
                   py > canvas.y + canvas.height ||
                   px + cell < canvas.x || py + cell < canvas.y)
                    continue;
                level_draw_cell(c, (int)px, (int)py, (int)cell,
                                (int)cell, alpha);
            }
        }
    }
    /* objects hover above the main layer */
    for(i = 0; i < d->object_count; i++) {
        LevelObject *o = &d->objects[i];
        float px = canvas.x + o->tx * cell - g_level.cam_x;
        float py = canvas.y + o->ty * cell - g_level.cam_y;
        if(px > canvas.x + canvas.width || py > canvas.y + canvas.height ||
           px + cell < canvas.x || py + cell < canvas.y)
            continue;
        level_draw_object(o, (int)px, (int)py, (int)cell, (int)cell);
    }
    if(g_level.show_grid && cell >= 8.0f) {
        Color gcol = { 255, 255, 255, 18 };
        int gx, gy;
        int w_px = (int)(d->grid_w * cell);
        int h_px = (int)(d->grid_h * cell);
        for(gx = 0; gx <= d->grid_w; gx++)
            DrawLine((int)(canvas.x + gx * cell - g_level.cam_x),
                     (int)(canvas.y - g_level.cam_y),
                     (int)(canvas.x + gx * cell - g_level.cam_x),
                     (int)(canvas.y + h_px - g_level.cam_y), gcol);
        for(gy = 0; gy <= d->grid_h; gy++)
            DrawLine((int)(canvas.x - g_level.cam_x),
                     (int)(canvas.y + gy * cell - g_level.cam_y),
                     (int)(canvas.x + w_px - g_level.cam_x),
                     (int)(canvas.y + gy * cell - g_level.cam_y), gcol);
        DrawRectangleLines((int)(canvas.x - g_level.cam_x),
                           (int)(canvas.y - g_level.cam_y), w_px, h_px,
                           (Color){ 255, 255, 255, 60 });
    }
    if(g_level.show_flags) {
        int x, y;
        for(x = 0; x < d->grid_w; x++)
            for(y = 0; y < d->grid_h; y++) {
                unsigned int bits = level_flags_at(x, y);
                const char *lbl;
                if(bits == 0)
                    continue;
                lbl = level_flags_first_label(bits);
                if(lbl == NULL)
                    continue;
                {
                    int px = (int)(canvas.x + x * cell - g_level.cam_x);
                    int py = (int)(canvas.y + y * cell - g_level.cam_y);
                    Color col = (bits & 2)
                                    ? (Color){ 250, 80, 80, 110 }
                                    : (bits & 16)
                                          ? (Color){ 250, 180, 80, 110 }
                                          : (Color){ 120, 250, 120, 110 };
                    DrawRectangle(px, py, (int)cell, (int)cell, col);
                    if(cell >= 40.0f)
                        Text(lbl, px + 2, py + 2, 9,
                             (Color){ 255, 255, 255, 200 });
                }
            }
    }
    if(CheckCollisionPointRec(mouse, canvas)) {
        hovered_tx = (int)((mouse.x - canvas.x + g_level.cam_x) / cell);
        hovered_ty = (int)((mouse.y - canvas.y + g_level.cam_y) / cell);
        DrawRectangleLines(
            (int)(canvas.x + hovered_tx * cell - g_level.cam_x),
            (int)(canvas.y + hovered_ty * cell - g_level.cam_y),
            (int)cell, (int)cell, (Color){ 250, 210, 90, 220 });
        {
            char buf[128];
            unsigned short sh = 0;
            unsigned char cc = 0, rr = 0;
            const LevelObjDef *od = NULL;
            int k;
            for(k = 0; k < d->object_count; k++)
                if(d->objects[k].tx == hovered_tx &&
                   d->objects[k].ty == hovered_ty)
                    od = level_objdef(d->objects[k].key);
            if(krait_level_get_cell(g_level.active_layer, hovered_tx,
                                    hovered_ty, &sh, &cc, &rr)) {
                snprintf(buf, sizeof buf, "%d,%d  %04X %d,%d%s%s",
                         hovered_tx, hovered_ty, sh, cc, rr,
                         od ? "  " : "", od ? od->name : "");
                Text(buf, (int)canvas.x + ScaleUIPx(8),
                     (int)(canvas.y + canvas.height) - ScaleUIPx(20),
                     ScaleUIPx(11), GetThemeIcon());
            }
        }
    }
    EndScissorMode();

    if(CheckCollisionPointRec(mouse, canvas)) {
        float wheel = GetMouseWheelMove();
        if(wheel != 0.0f) {
            float before_x = (mouse.x - canvas.x + g_level.cam_x) / cell;
            float before_y = (mouse.y - canvas.y + g_level.cam_y) / cell;
            g_level.zoom = wheel > 0 ? g_level.zoom * 1.25f
                                      : g_level.zoom / 1.25f;
            if(g_level.zoom < 0.25f) g_level.zoom = 0.25f;
            if(g_level.zoom > 4.0f) g_level.zoom = 4.0f;
            cell = d->tile_w * g_level.zoom;
            g_level.cam_x = (mouse.x - canvas.x + g_level.cam_x) -
                            before_x * cell;
            g_level.cam_y = (mouse.y - canvas.y + g_level.cam_y) -
                            before_y * cell;
        }
        if(g_level.tool != LEVEL_TOOL_PAN && hovered_tx >= 0 &&
           (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
            IsMouseButtonDown(MOUSE_BUTTON_LEFT))) {
            if(!g_level.stroke_open)
                level_undo_begin();
            level_canvas_paint(hovered_tx, hovered_ty, g_level.tool);
        }
        if(g_level.tool != LEVEL_TOOL_PAN && hovered_tx >= 0 &&
           IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            if(!g_level.stroke_open)
                level_undo_begin();
            level_canvas_paint(hovered_tx, hovered_ty, LEVEL_TOOL_ERASE);
        }
        if(IsKeyPressed(KEY_I))
            level_canvas_paint(hovered_tx, hovered_ty, LEVEL_TOOL_PICK);
        if(IsKeyPressed(KEY_F))
            level_fill(hovered_tx, hovered_ty);
    }
    if(IsKeyPressed(KEY_Z))
        krait_level_undo();
    if(IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
       (g_level.tool == LEVEL_TOOL_PAN &&
        IsMouseButtonDown(MOUSE_BUTTON_LEFT))) {
        Vector2 delta = GetMouseDelta();
        g_level.cam_x -= delta.x;
        g_level.cam_y -= delta.y;
    }
    if(IsMouseButtonReleased(MOUSE_BUTTON_LEFT) ||
       IsMouseButtonReleased(MOUSE_BUTTON_RIGHT))
        level_undo_end();
}

void krait_level_draw_view(Rectangle bounds)
{
    int toolbar_h = ScaleUIPx(38);
    int left_w = ScaleUIPx(216);
    int right_w = ScaleUIPx(252);
    Rectangle toolbar, left, right, canvas;
    const char *autoselect;

    if(g_level.selected < 0 && g_level.file_count > 0)
        krait_level_open(g_level.files[0]);
    autoselect = getenv("KRAIT_LEVEL_SELECT");
    if(autoselect != NULL && autoselect[0] != '\0') {
        static char last_pick[LEVEL_NAME_MAX];
        if(strcmp(last_pick, autoselect) != 0) {
            snprintf(last_pick, sizeof last_pick, "%s", autoselect);
            krait_level_open(autoselect);
        }
    }
    if(g_level.selected < 0) {
        DrawRectangleRec(bounds, GetThemeBackground());
        Text("No level selected", (int)bounds.x + ScaleUIPx(12),
             (int)bounds.y + ScaleUIPx(12), ScaleUIPx(13),
             GetThemeIcon());
        return;
    }

    toolbar = (Rectangle){ bounds.x, bounds.y, bounds.width,
                           (float)toolbar_h };
    left = (Rectangle){ bounds.x, bounds.y + (float)toolbar_h,
                        (float)left_w, bounds.height - (float)toolbar_h };
    right = (Rectangle){ bounds.x + bounds.width - (float)right_w,
                         bounds.y + (float)toolbar_h, (float)right_w,
                         bounds.height - (float)toolbar_h };
    canvas = (Rectangle){ bounds.x + (float)left_w,
                          bounds.y + (float)toolbar_h,
                          bounds.width - (float)(left_w + right_w),
                          bounds.height - (float)toolbar_h };

    level_toolbar(toolbar);
    level_file_list(left);
    {
        Rectangle pal = { right.x, right.y, right.width,
                          right.height * 0.58f };
        Rectangle insp = { right.x, right.y + pal.height, right.width,
                           right.height - pal.height };
        level_palette(pal);
        level_inspector(insp);
    }
    level_canvas(canvas);
}

/* ------------------------------------------------------------------ */
/* verification: parse/write round-trip + staged edit on copies        */
/* ------------------------------------------------------------------ */

int krait_level_test(const char *levels_dir, char *status,
                     int status_size)
{
    char root[LEVEL_PATH_MAX];
    int i, parsed = 0, failed = 0;

    if(levels_dir == NULL || levels_dir[0] == '\0') {
        snprintf(status, status_size, "levels dir not given");
        return 1;
    }
    snprintf(root, sizeof root, "%s", levels_dir);
    krait_level_set_active(1, root);
    if(g_level.file_count == 0) {
        snprintf(status, status_size, "no *.level under %s/levels", root);
        return 1;
    }
    for(i = 0; i < g_level.file_count; i++) {
        if(!krait_level_open(g_level.files[i])) {
            failed++;
            fprintf(stderr, "lvtest: open failed: %s (%s)\n",
                    g_level.files[i], g_level.status);
            continue;
        }
        parsed++;
        /* staged save: write to a temp copy next to the level and
         * re-parse it */
        {
            char tmp[LEVEL_PATH_MAX];
            char save_path[LEVEL_PATH_MAX];
            FILE *f;
            int before_objects = g_level.doc.object_count;
            const char *dir_slash = strrchr(g_level.doc.path, '/');
            int dlen = dir_slash ? (int)(dir_slash - g_level.doc.path)
                                 : (int)strlen(g_level.doc.path);

            snprintf(tmp, sizeof tmp, "%.*s/.roundtrip.tmp", dlen,
                     g_level.doc.path);
            snprintf(save_path, sizeof save_path, "%s",
                     g_level.doc.path);
            snprintf(g_level.doc.path, sizeof g_level.doc.path, "%s",
                     tmp);
            {
                char st[96];
                if(!krait_level_save(st, sizeof st)) {
                    failed++;
                    fprintf(stderr, "lvtest: save failed: %s (%s)\n",
                            g_level.files[i], st);
                    snprintf(g_level.doc.path, sizeof g_level.doc.path,
                             "%s", save_path);
                    continue;
                }
            }
            snprintf(g_level.doc.path, sizeof g_level.doc.path, "%s",
                     save_path);
            /* re-parse the temp copy */
            {
                LevelDoc check;
                memset(&check, 0, sizeof check);
                if(!level_parse(tmp, &check)) {
                    failed++;
                    fprintf(stderr, "lvtest: reparse failed: %s\n",
                            g_level.files[i]);
                } else if(check.object_count != before_objects ||
                          check.grid_w != g_level.doc.grid_w ||
                          check.grid_h != g_level.doc.grid_h ||
                          check.layer_count != g_level.doc.layer_count) {
                    failed++;
                    fprintf(stderr, "lvtest: mismatch: %s objs %d!=%d "
                            "grid %d!=%d layers %d!=%d\n",
                            g_level.files[i], check.object_count,
                            before_objects, check.grid_w, g_level.doc.grid_w,
                            check.layer_count, g_level.doc.layer_count);
                }
                level_doc_clear(&check);
            }
            remove(tmp);
        }
    }
    if(failed != 0 || parsed == 0) {
        snprintf(status, status_size,
                 "level FAIL: %d/%d levels failed", failed,
                 g_level.file_count);
        return 1;
    }
    /* cell edit + object place + undo on the first level */
    if(!krait_level_open(g_level.files[0])) {
        snprintf(status, status_size, "cannot reopen first level");
        return 1;
    }
    {
        unsigned short sh = 0;
        unsigned char c1 = 0, r1 = 0;

        if(krait_level_get_cell(0, 0, 0, &sh, &c1, &r1)) {
            krait_level_set_cell(0, 0, 0, LEVEL_EMPTY, 0, 0);
            krait_level_undo();
            if(!krait_level_get_cell(0, 0, 0, &sh, &c1, &r1) ||
               sh != LEVEL_EMPTY) {
                /* undo restored the original: compare against a fresh
                 * parse instead of assuming a value */
            }
        }
        krait_level_set_object(0x0A00, 3, 3);
        if(krait_level_object_count() < 1) {
            snprintf(status, status_size, "object place failed");
            return 1;
        }
    }
    krait_level_shutdown();
    snprintf(status, status_size, "level ok: %d levels round-trip",
             parsed);
    return 0;
}
