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

void
krait_join(char *dst, size_t dst_size, const char *a, const char *b)
{
    if(dst_size == 0)
        return;
    if(a == NULL || a[0] == '\0')
        snprintf(dst, dst_size, "%s", b != NULL ? b : "");
    else if(b == NULL || b[0] == '\0')
        snprintf(dst, dst_size, "%s", a);
    else
        snprintf(dst, dst_size, "%s/%s", a, b);
}

const char *
krait_basename(const char *path)
{
    const char *base = path;
    if(path == NULL)
        return "";
    for(const char *p = path; *p != '\0'; p++) {
        if(*p == '/')
            base = p + 1;
    }
    return base;
}

void
krait_title_from_file(char *dst, size_t dst_size, const char *file)
{
    const char *base = krait_basename(file);
    size_t n = strlen(base);

    if(dst_size == 0)
        return;
    if(n >= 4 && strcmp(base + n - 4, ".kry") == 0)
        n -= 4;
    if(n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, base, n);
    dst[n] = '\0';
    for(size_t i = 0; dst[i] != '\0'; i++) {
        if(dst[i] == '_')
            dst[i] = ' ';
    }
}

int
krait_path_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
}

int
krait_path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;

    if(path == NULL || suffix == NULL)
        return 0;
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if(path_len < suffix_len)
        return 0;
    return strcmp(path + path_len - suffix_len, suffix) == 0;
}

int
krait_file_is_text(const char *path)
{
    const char *base = krait_basename(path);

    return krait_path_has_suffix(path, ".kry") ||
           krait_path_has_suffix(path, ".c") ||
           krait_path_has_suffix(path, ".h") ||
           krait_path_has_suffix(path, ".md") ||
           krait_path_has_suffix(path, ".txt") ||
           krait_path_has_suffix(path, ".mk") ||
           strcmp(base, "Makefile") == 0 ||
           strcmp(base, "makefile") == 0 ||
           strcmp(base, "project.kryon") == 0;
}

int
krait_ignored_dir(const char *name)
{
    return name == NULL ||
           name[0] == '.' ||
           strcmp(name, ".git") == 0 ||
           strcmp(name, "build") == 0 ||
           strcmp(name, "vendor") == 0 ||
           strcmp(name, "tmp") == 0 ||
           strcmp(name, ".cache") == 0;
}

int
krait_tree_id(const char *path)
{
    unsigned int hash = 2166136261u;

    if(path == NULL || path[0] == '\0')
        return 1;
    for(const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    hash &= 0x7fffffffu;
    if(hash == 0)
        hash = 1;
    return (int)hash;
}

void
krait_ensure_parent_dir(const char *path)
{
    char dir[KRAIT_PATH_MAX];
    char *slash;

    if(path == NULL)
        return;
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if(slash == NULL)
        return;
    *slash = '\0';
    (void)mkdir(dir, 0755);
}

int
krait_write_text_file(const char *path, const char *text)
{
    FILE *file;
    size_t len;

    if(path == NULL || text == NULL)
        return 0;
    file = fopen(path, "wb");
    if(file == NULL)
        return 0;
    len = strlen(text);
    if(fwrite(text, 1, len, file) != len) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

int
krait_ident_start(int ch)
{
    return isalpha((unsigned char)ch) || ch == '_';
}

int
krait_ident_char(int ch)
{
    return isalnum((unsigned char)ch) || ch == '_';
}

int
krait_read_file_alloc(const char *path, char **out, long *out_len)
{
    FILE *file;
    long len;
    char *buf;

    if(out == NULL)
        return 0;
    *out = NULL;
    if(out_len != NULL)
        *out_len = 0;
    file = fopen(path, "rb");
    if(file == NULL)
        return 0;
    if(fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    len = ftell(file);
    if(len < 0) {
        fclose(file);
        return 0;
    }
    if(fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    buf = malloc((size_t)len + 1);
    if(buf == NULL) {
        fclose(file);
        return 0;
    }
    if(len > 0 && fread(buf, 1, (size_t)len, file) != (size_t)len) {
        free(buf);
        fclose(file);
        return 0;
    }
    fclose(file);
    buf[len] = '\0';
    *out = buf;
    if(out_len != NULL)
        *out_len = len;
    return 1;
}

char *
krait_trim(char *s)
{
    char *end;

    if(s == NULL)
        return NULL;
    while(*s != '\0' && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while(end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

