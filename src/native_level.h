#ifndef KRAIT_NATIVE_LEVEL_H
#define KRAIT_NATIVE_LEVEL_H

/*
 * Generic kryon tile-level editor for the Krait game view.
 *
 * A level is a text file ("kryon level v1"): a grid of any size, up to
 * four tile layers referencing named PNG sheets, per-tile collision
 * flags, and objects placed on the grid. Projects that build games on
 * Kryon keep their levels in <project>/levels/*.level (edited here);
 * the game reads the same files. Krait knows nothing about any specific
 * game — object kinds, flag names, and sheets all come from the files.
 */

#include "kryon_compat.generated.h"

/* Level files present under a project root (levels/*.level or *.level
 * at the root)? 1 = yes. */
int krait_level_project_has(const char *project_root);

/* Editor mode (state lives in this module, not the engine). */
int krait_level_editor_active(void);
void krait_level_set_active(int on, const char *project_root);

/* Full-bounds editor UI. */
void krait_level_draw_view(Rectangle bounds);

/* ---- document API (shared by the editor, tests, automation) ---- */
int krait_level_file_count(void);
const char *krait_level_file_name(int index);

/* Open (select) a level file by entry name, e.g. "World1_1.level". */
int krait_level_open(const char *name);

/* Cell edit on the open level. Layer 0 = main (first layer). */
int krait_level_set_cell(int layer, int x, int y,
                         unsigned short sheet, unsigned char col,
                         unsigned char row);
int krait_level_get_cell(int layer, int x, int y,
                         unsigned short *sheet, unsigned char *col,
                         unsigned char *row);
/* Place / remove an object on the open level (key is project-defined). */
int krait_level_set_object(unsigned int key, int tx, int ty);
int krait_level_object_count(void);
void krait_level_undo(void);
int krait_level_dirty(void);
int krait_level_save(char *status, int status_size);

/* Verification hook (tests + smoke). Returns 0 on success. */
int krait_level_test(const char *levels_dir, char *status,
                     int status_size);

void krait_level_shutdown(void);

#endif /* KRAIT_NATIVE_LEVEL_H */
