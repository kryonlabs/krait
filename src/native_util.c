#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"
#include "app_host.h"
#include "kry_dylib.h"
#include "kry_sha256.h"

#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
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

void
krait_kryon_dir(char *out, size_t out_size)
{
    /* $KRYON_DIR wins when set. Otherwise derive from the binary location
     * (<krait>/build/<plat>-<arch>/bin/krait -> <krait>/vendor/kryon) so a
     * launch from outside the checkout still finds the vendored runtime;
     * last resort is the cwd-relative path. */
    const char *dir = getenv("KRYON_DIR");
    char exe[KRAIT_PATH_MAX];
    char root[KRAIT_PATH_MAX];
    char candidate[KRAIT_PATH_MAX];
    ssize_t n;

    if(dir != NULL && dir[0] != '\0') {
        snprintf(out, out_size, "%s", dir);
        return;
    }
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if(n <= 0)
        n = readlink("/proc/curproc/file", exe, sizeof(exe) - 1);
    if(n > 0) {
        char *p;
        int up;

        exe[n] = '\0';
        snprintf(root, sizeof(root), "%s", exe);
        for(up = 0; up < 4; up++) {
            p = strrchr(root, '/');
            if(p == NULL)
                break;
            *p = '\0';
        }
        if(p != NULL) {
            snprintf(candidate, sizeof(candidate), "%s/vendor/kryon", root);
            if(krait_path_exists(candidate)) {
                snprintf(out, out_size, "%s", candidate);
                return;
            }
        }
    }
    snprintf(out, out_size, "%s", "vendor/kryon");
}

void
krait_kryon_tool_path(char *out, size_t out_size, const char *tool)
{
    /* kryon builds its tools (k2ir/k2b/k2c) into the platform-tagged dir
     * <kryon_dir>/build/<platform>-<arch>/bin/, never the legacy flat
     * build/bin/, so derive the host platform/arch the same way kryon's
     * Makefile does (amd64 -> x86_64; Linux/FreeBSD/Darwin -> the lower
     * kryon name). */
    const char *plat = "linux";
    const char *arch = "x86_64";
    struct utsname u;
    char dir[KRAIT_PATH_MAX];

    krait_kryon_dir(dir, sizeof(dir));
    if(uname(&u) == 0) {
        if(strcmp(u.sysname, "FreeBSD") == 0)
            plat = "freebsd";
        else if(strcmp(u.sysname, "Darwin") == 0)
            plat = "macos";
        else if(strcmp(u.sysname, "Linux") == 0)
            plat = "linux";
        else
            plat = u.sysname;
        if(strcmp(u.machine, "amd64") == 0)
            arch = "x86_64";
        else
            arch = u.machine;
    }
    snprintf(out, out_size, "%s/build/%s-%s/bin/%s", dir, plat, arch, tool);
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

/* Replace only after the complete file is flushed. Preserve permissions
 * on existing documents; private defaults for new task/session records. */
int
krait_write_text_file_atomic(const char *path, const char *text)
{
    char temp[KRAIT_PATH_MAX * 3];
    struct stat st;
    int fd, ok;
    FILE *file;
    if(path == NULL || text == NULL ||
       snprintf(temp, sizeof(temp), "%s.tmp-XXXXXX", path) >= (int)sizeof(temp))
        return 0;
    fd = mkstemp(temp);
    if(fd < 0)
        return 0;
    if(stat(path, &st) == 0 && fchmod(fd, st.st_mode & 0777) != 0) {
        close(fd);
        unlink(temp);
        return 0;
    }
    file = fdopen(fd, "wb");
    if(file == NULL) {
        close(fd);
        unlink(temp);
        return 0;
    }
    ok = fwrite(text, 1, strlen(text), file) == strlen(text);
    if(fflush(file) != 0 || fsync(fd) != 0)
        ok = 0;
    if(fclose(file) != 0)
        ok = 0;
    if(ok)
        ok = rename(temp, path) == 0;
    if(!ok)
        unlink(temp);
    return ok;
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



/* Word-wrap with a cache: the transcript re-wraps every visible message
 * every frame, and the old .kry walker re-measured every word per row.
 * The line table is built once per (text, width, font) and reused until
 * any of them changes. */
#define WRAP_CACHE_LINES 4096
typedef struct {
    const char *text;   /* pointers are stable between frames: identity
                         * check first, hash only on a new pointer */
    unsigned long hash;
    int width;
    int font;
    int count;
    int starts[WRAP_CACHE_LINES];
    int lens[WRAP_CACHE_LINES];
} WrapCache;
static WrapCache wrap_cache;

static unsigned long
wrap_hash(const char *s)
{
    unsigned long h = 2166136261ul;

    while(s != NULL && *s != '\0')
        h = (h ^ (unsigned char)*s++) * 16777619ul;
    return h;
}

static int
wrap_build(const char *text, int width, int font)
{
    const char *p = text;
    int line_w = 0;
    int count = 0;
    int start = 0;

    wrap_cache.text = text;
    wrap_cache.hash = wrap_hash(text);
    wrap_cache.width = width;
    wrap_cache.font = font;
    wrap_cache.count = 0;
    if(text == NULL || width <= 0) {
        wrap_cache.count = text != NULL ? 1 : 0;
        if(wrap_cache.count == 1) {
            wrap_cache.starts[0] = 0;
            wrap_cache.lens[0] = (int)strlen(text);
        }
        return wrap_cache.count;
    }
    while(*p != '\0') {
        const char *word = p;
        char seg[512];
        size_t seg_len;
        int seg_w;

        while(*p != '\0' && *p != ' ' && *p != '\n')
            p++;
        seg_len = (size_t)(p - word);
        if(seg_len > sizeof(seg) - 1)
            seg_len = sizeof(seg) - 1;
        memcpy(seg, word, seg_len);
        seg[seg_len] = '\0';
        seg_w = TextWidth(seg, font);
        if(line_w > 0 && line_w + seg_w + font > width) {
            if(count >= WRAP_CACHE_LINES)
                break;
            wrap_cache.starts[count] = start;
            wrap_cache.lens[count] = (int)(word - text) - start;
            while(wrap_cache.lens[count] > 0 &&
                  text[start + wrap_cache.lens[count] - 1] == ' ')
                wrap_cache.lens[count]--;
            count++;
            start = (int)(word - text);
            line_w = 0;
        }
        line_w += seg_w + font;
        if(*p == '\n') {
            if(count >= WRAP_CACHE_LINES)
                break;
            wrap_cache.starts[count] = start;
            wrap_cache.lens[count] = (int)(p - text) - start;
            count++;
            start = (int)(p - text) + 1;
            line_w = 0;
            p++;
        } else if(*p != '\0') {
            p++;
        }
    }
    if(count < WRAP_CACHE_LINES) {
        wrap_cache.starts[count] = start;
        wrap_cache.lens[count] = (int)strlen(text + start);
        while(wrap_cache.lens[count] > 0 &&
              text[start + wrap_cache.lens[count] - 1] == ' ')
            wrap_cache.lens[count]--;
        if(wrap_cache.lens[count] > 0 || count == 0)
            count++;
    }
    wrap_cache.count = count;
    return count;
}

int
krait_wrap_lines(const char *text, int width, int font, int max_lines)
{
    int n;

    if(text == NULL)
        return 0;
    if(wrap_cache.text == text && wrap_cache.width == width &&
       wrap_cache.font == font && wrap_cache.count > 0)
        n = wrap_cache.count;
    else
        n = wrap_build(text, width, font);
    return n < max_lines ? n : max_lines;
}

int
krait_wrap_line(const char *text, int width, int font, int max_lines, int row,
                char *dst, int dst_size)
{
    int n;

    if(dst == NULL || dst_size <= 0)
        return 0;
    dst[0] = '\0';
    if(text == NULL || row < 0)
        return 0;
    n = krait_wrap_lines(text, width, font, max_lines);
    if(row >= n)
        return 0;
    {
        int len = wrap_cache.lens[row];

        if(len > dst_size - 1)
            len = dst_size - 1;
        memcpy(dst, text + wrap_cache.starts[row], (size_t)len);
        dst[len] = '\0';
    }
    return 1;
}

int
krait_scratch_path(char *out, size_t out_size)
{
    const char *home = getenv("HOME");

    if(out == NULL || out_size == 0)
        return 0;
    if(home == NULL || home[0] == '\0')
        snprintf(out, out_size, ".kryon/scratch.txt");
    else
        snprintf(out, out_size, "%s/.kryon/scratch.txt", home);
    return 1;
}

/* Stable source-tree fingerprint. Generated output/dependency directories
 * are excluded consistently; source names, modes and bytes are included. */
static int
snapshot_dir(const char *path, KrySha256 *hash, int depth)
{
    struct dirent **entries = NULL;
    int count, ok = 1;
    if(depth > 64)
        return 0;
    count = scandir(path, &entries, NULL, alphasort);
    if(count < 0)
        return 0;
    for(int i = 0; i < count; i++) {
        const char *name = entries[i]->d_name;
        char full[KRAIT_PATH_MAX * 3];
        struct stat st;
        if(!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git")) {
            free(entries[i]);
            continue;
        }
        if(snprintf(full, sizeof(full), "%s/%s", path, name) >= (int)sizeof(full) ||
           lstat(full, &st) != 0) { ok = 0; free(entries[i]); continue; }
        if(S_ISDIR(st.st_mode) && (!strcmp(name, "build") || !strcmp(name, "out") ||
           !strcmp(name, "node_modules") || !strcmp(name, ".cache"))) {
            free(entries[i]);
            continue;
        }
        kry_sha256_update(hash, name, strlen(name) + 1);
        char mode[64];
        snprintf(mode, sizeof(mode), "%o:%lld", (unsigned)(st.st_mode & (S_IFMT | 0777)),
                 (long long)(S_ISDIR(st.st_mode) ? 0 : st.st_size));
        kry_sha256_update(hash, mode, strlen(mode) + 1);
        if(S_ISDIR(st.st_mode)) {
            if(!snapshot_dir(full, hash, depth + 1)) ok = 0;
        } else if(S_ISREG(st.st_mode)) {
            FILE *f = fopen(full, "rb");
            if(f == NULL) ok = 0;
            else {
                unsigned char buf[16384];
                size_t n;
                while((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    kry_sha256_update(hash, buf, n);
                if(ferror(f)) ok = 0;
                fclose(f);
            }
        } else if(S_ISLNK(st.st_mode)) {
            char target[KRAIT_PATH_MAX * 3];
            ssize_t n = readlink(full, target, sizeof(target));
            if(n < 0 || n == sizeof(target)) ok = 0;
            else kry_sha256_update(hash, target, (size_t)n);
        } else ok = 0;
        kry_sha256_update(hash, "\0END\0", 5);
        free(entries[i]);
    }
    free(entries);
    return ok;
}

int
krait_project_snapshot(const char *project, char digest[65])
{
    KrySha256 hash;
    unsigned char raw[32];
    if(project == NULL || project[0] == 0 || digest == NULL)
        return 0;
    digest[0] = 0;
    kry_sha256_init(&hash);
    if(!snapshot_dir(project, &hash, 0))
        return 0;
    kry_sha256_final(&hash, raw);
    kry_sha256_hex(raw, digest);
    return 1;
}
