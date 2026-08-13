#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    KRAIT_ARTIFACT_KIR = 1,
    KRAIT_ARTIFACT_KRB = 2,
    KRAIT_ARTIFACT_C = 3
};

static const char *
krait_artifact_kryon_dir(void)
{
    const char *dir = getenv("KRYON_DIR");

    if(dir != NULL && dir[0] != '\0')
        return dir;
    return "vendor/kryon";
}

static int
shell_quote(char *dst, size_t dst_size, const char *src)
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
source_stem(char *dst, size_t dst_size, const char *rel_source)
{
    const char *base;
    const char *dot;
    size_t n;

    if(dst_size == 0)
        return;
    base = krait_basename(rel_source);
    dot = strrchr(base, '.');
    n = dot != NULL ? (size_t)(dot - base) : strlen(base);
    if(n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, base, n);
    dst[n] = '\0';
}

static void
rel_stem(char *dst, size_t dst_size, const char *rel_source)
{
    size_t n;

    if(dst_size == 0)
        return;
    n = strlen(rel_source);
    if(n > 4 && strcmp(rel_source + n - 4, ".kry") == 0)
        n -= 4;
    if(n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, rel_source, n);
    dst[n] = '\0';
}

static int
read_text(const char *path, char *out, int cap)
{
    FILE *file;
    size_t n;

    if(path == NULL || out == NULL || cap <= 0)
        return 0;
    out[0] = '\0';
    file = fopen(path, "rb");
    if(file == NULL)
        return 0;
    n = fread(out, 1, (size_t)cap - 1, file);
    out[n] = '\0';
    fclose(file);
    return 1;
}

static int
read_hex(const char *path, char *out, int cap)
{
    FILE *file;
    unsigned char buf[16];
    unsigned long off = 0;
    int used = 0;
    size_t n;

    if(path == NULL || out == NULL || cap <= 0)
        return 0;
    out[0] = '\0';
    file = fopen(path, "rb");
    if(file == NULL)
        return 0;
    used += snprintf(out + used, (size_t)(cap - used),
                     "KRB binary cartridge preview\n%s\n\n", path);
    while(used + 96 < cap && (n = fread(buf, 1, sizeof(buf), file)) > 0) {
        used += snprintf(out + used, (size_t)(cap - used), "%08lx  ", off);
        for(size_t i = 0; i < 16; i++) {
            if(i < n)
                used += snprintf(out + used, (size_t)(cap - used), "%02x ", buf[i]);
            else
                used += snprintf(out + used, (size_t)(cap - used), "   ");
        }
        used += snprintf(out + used, (size_t)(cap - used), " ");
        for(size_t i = 0; i < n && used + 2 < cap; i++)
            out[used++] = (buf[i] >= 32 && buf[i] <= 126) ? (char)buf[i] : '.';
        if(used + 1 < cap)
            out[used++] = '\n';
        out[used] = '\0';
        off += (unsigned long)n;
    }
    fclose(file);
    if(used + 1 >= cap)
        out[cap - 1] = '\0';
    return 1;
}

static int
copy_binary(const char *src, const char *dst)
{
    FILE *in;
    FILE *out;
    unsigned char buf[4096];
    size_t n;

    if(src == NULL || dst == NULL)
        return 0;
    in = fopen(src, "rb");
    if(in == NULL)
        return 0;
    krait_ensure_parent_dir(dst);
    out = fopen(dst, "wb");
    if(out == NULL) {
        fclose(in);
        return 0;
    }
    while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if(fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return 0;
        }
    }
    fclose(in);
    fclose(out);
    return 1;
}

int
krait_artifact_generate(const char *root, const char *rel_source, int kind,
                        char *out, int out_size, char *artifact_path,
                        int artifact_path_size, char *status, int status_size)
{
    char tool[KRAIT_PATH_MAX];
    char temp[KRAIT_PATH_MAX];
    char qtool[KRAIT_PATH_MAX * 2];
    char qroot[KRAIT_PATH_MAX * 2];
    char qsrc[KRAIT_PATH_MAX * 2];
    char qtemp[KRAIT_PATH_MAX * 2];
    char cmd[KRAIT_PATH_MAX * 8];
    char stem[KRAIT_PATH_MAX];
    char base[256];
    const char *kryon_dir;
    int rc;

    if(out != NULL && out_size > 0)
        out[0] = '\0';
    if(artifact_path != NULL && artifact_path_size > 0)
        artifact_path[0] = '\0';
    if(status != NULL && status_size > 0)
        status[0] = '\0';
    if(root == NULL || rel_source == NULL || !krait_path_has_suffix(rel_source, ".kry"))
        return 0;

    kryon_dir = krait_artifact_kryon_dir();
    snprintf(temp, sizeof(temp), "/tmp/krait-artifacts-%ld-%d",
             (long)getpid(), kind);
    if(kind == KRAIT_ARTIFACT_KIR)
        snprintf(tool, sizeof(tool), "%s/build/bin/k2ir", kryon_dir);
    else if(kind == KRAIT_ARTIFACT_KRB)
        snprintf(tool, sizeof(tool), "%s/build/bin/k2b", kryon_dir);
    else if(kind == KRAIT_ARTIFACT_C)
        snprintf(tool, sizeof(tool), "%s/build/bin/kc", kryon_dir);
    else
        return 0;
    if(!shell_quote(qtool, sizeof(qtool), tool) ||
       !shell_quote(qroot, sizeof(qroot), root) ||
       !shell_quote(qsrc, sizeof(qsrc), rel_source) ||
       !shell_quote(qtemp, sizeof(qtemp), temp)) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Artifact path too long");
        return 0;
    }
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s && %s --no-main --root %s -o %s %s",
             qtemp, qtemp, qtool, qroot, qtemp, qsrc);
    if(kind == KRAIT_ARTIFACT_KIR)
        snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s && %s --root %s -o %s %s",
                 qtemp, qtemp, qtool, qroot, qtemp, qsrc);
    rc = system(cmd);
    if(rc != 0) {
        if(status != NULL && status_size > 0)
            snprintf(status, (size_t)status_size, "Artifact generation failed");
        return 0;
    }

    rel_stem(stem, sizeof(stem), rel_source);
    source_stem(base, sizeof(base), rel_source);
    if(kind == KRAIT_ARTIFACT_KIR)
        snprintf(artifact_path, (size_t)artifact_path_size, "%s/%s.kir", temp, stem);
    else if(kind == KRAIT_ARTIFACT_C)
        snprintf(artifact_path, (size_t)artifact_path_size, "%s/%s.c", temp, stem);
    else
        snprintf(artifact_path, (size_t)artifact_path_size, "%s/%s.krb", temp, base);

    if(kind == KRAIT_ARTIFACT_KRB) {
        if(!read_hex(artifact_path, out, out_size))
            return 0;
    } else if(!read_text(artifact_path, out, out_size)) {
        return 0;
    }
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Generated");
    return 1;
}

int
krait_artifact_save_binary(const char *generated_path, const char *dest_path)
{
    return copy_binary(generated_path, dest_path);
}
