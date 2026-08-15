/*
 * native_krbhex.c - hex editor backend for binary files (.krb).
 *
 * The editor opens binaries as bytes, not mangled text: load keeps the
 * raw buffer, the UI reads and mutates through byte accessors, and save
 * backs up the original as .bak before writing - same discipline as the
 * agent's writes. One buffer at a time, capped; .kry gets index-based
 * accessors, never the array.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KRAIT_HEX_CAP (1024 * 1024)

static unsigned char *hex_buf;
static size_t hex_size;
static char hex_path[KRAIT_PATH_MAX * 2];
static int hex_dirty;
static size_t hex_changed_count;

int
krait_hex_open(const char *path)
{
    FILE *file;
    long len;

    if(path == NULL || path[0] == '\0')
        return 0;
    file = fopen(path, "rb");

    if(file == NULL)
        return 0;
    if(fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    len = ftell(file);
    if(len < 0 || len > KRAIT_HEX_CAP) {
        fclose(file);
        return 0;
    }
    if(fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    free(hex_buf);
    hex_buf = malloc((size_t)len + 1);
    if(hex_buf == NULL) {
        fclose(file);
        return 0;
    }
    if(len > 0 && fread(hex_buf, 1, (size_t)len, file) != (size_t)len) {
        free(hex_buf);
        hex_buf = NULL;
        fclose(file);
        return 0;
    }
    fclose(file);
    hex_size = (size_t)len;
    snprintf(hex_path, sizeof(hex_path), "%s", path);
    hex_dirty = 0;
    hex_changed_count = 0;

    return 1;
}

int
krait_hex_size(void)
{
    return (int)hex_size;
}

int
krait_hex_byte(int index)
{
    if(hex_buf == NULL || index < 0 || (size_t)index >= hex_size)
        return -1;
    return hex_buf[index];
}

int
krait_hex_set_byte(int index, int value)
{
    if(hex_buf == NULL || index < 0 || (size_t)index >= hex_size)
        return 0;
    if(hex_buf[index] != (unsigned char)value)
        hex_changed_count++;
    hex_buf[index] = (unsigned char)value;
    hex_dirty = 1;
    return 1;
}

int
krait_hex_dirty(void)
{
    return hex_dirty;
}

int
krait_hex_changed_count(void)
{
    return (int)hex_changed_count;
}

const char *
krait_hex_path(void)
{
    return hex_path;
}

int
krait_hex_save(void)
{
    char bak[KRAIT_PATH_MAX * 2];
    FILE *file;

    if(hex_buf == NULL || hex_path[0] == '\0')
        return 0;
    snprintf(bak, sizeof(bak), "%s.bak", hex_path);
    {
        char *orig = NULL;
        long len;

        if(krait_read_file_alloc(hex_path, &orig, &len))
            krait_write_text_file(bak, orig);
        free(orig);
    }
    file = fopen(hex_path, "wb");
    if(file == NULL)
        return 0;
    if(hex_size > 0 && fwrite(hex_buf, 1, hex_size, file) != hex_size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    hex_dirty = 0;
    return 1;
}
