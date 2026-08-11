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

typedef struct KraitProjectMeta {
    int loaded;
    char root[KRAIT_PATH_MAX];
    char title[256];
    char app_id[256];
    char version[64];
    char icon_path[KRAIT_PATH_MAX];
    char language[32];
    char short_description[512];
    char full_description[4096];
    char languages[512];
    int cursor[7];
    int focused[7];
    int full_scroll;
} KraitProjectMeta;

static KraitProjectMeta g_project_meta;

static int
krait_meta_parse_quoted(const char **sp, char *dst, size_t dst_size)
{
    const char *p;
    size_t n = 0;

    if(sp == NULL || *sp == NULL || dst == NULL || dst_size == 0)
        return 0;
    dst[0] = '\0';
    p = *sp;
    while(*p != '\0' && isspace((unsigned char)*p))
        p++;
    if(*p != '"')
        return 0;
    p++;
    while(*p != '\0' && *p != '"') {
        int ch = *p++;
        if(ch == '\\' && *p != '\0') {
            ch = *p++;
            if(ch == 'n')
                ch = '\n';
            else if(ch == 't')
                ch = '\t';
        }
        if(n + 1 < dst_size)
            dst[n++] = (char)ch;
    }
    if(*p != '"')
        return 0;
    dst[n] = '\0';
    *sp = p + 1;
    return 1;
}

static void
krait_meta_write_quoted(FILE *file, const char *s)
{
    fputc('"', file);
    if(s != NULL) {
        for(const char *p = s; *p != '\0'; p++) {
            if(*p == '"' || *p == '\\')
                fputc('\\', file);
            if(*p == '\n')
                fputs("\\n", file);
            else if(*p == '\t')
                fputs("\\t", file);
            else
                fputc(*p, file);
        }
    }
    fputc('"', file);
}

static void
krait_meta_set_field(KraitProjectMeta *meta, const char *key, const char *value)
{
    if(strcmp(key, "app_id") == 0)
        snprintf(meta->app_id, sizeof(meta->app_id), "%s", value);
    else if(strcmp(key, "version") == 0)
        snprintf(meta->version, sizeof(meta->version), "%s", value);
    else if(strcmp(key, "icon") == 0 || strcmp(key, "logo") == 0)
        snprintf(meta->icon_path, sizeof(meta->icon_path), "%s", value);
    else if(strcmp(key, "language") == 0)
        snprintf(meta->language, sizeof(meta->language), "%s", value);
    else if(strcmp(key, "short_description") == 0)
        snprintf(meta->short_description, sizeof(meta->short_description), "%s", value);
    else if(strcmp(key, "full_description") == 0)
        snprintf(meta->full_description, sizeof(meta->full_description), "%s", value);
}

static int
krait_meta_managed_key(const char *key)
{
    return strcmp(key, "app_id") == 0 ||
           strcmp(key, "version") == 0 ||
           strcmp(key, "icon") == 0 ||
           strcmp(key, "logo") == 0 ||
           strcmp(key, "language") == 0 ||
           strcmp(key, "short_description") == 0 ||
           strcmp(key, "full_description") == 0;
}

static int
krait_meta_line_managed(char *line)
{
    char *p = krait_trim(line);
    char key[128];
    const char *scan;

    if(p == NULL)
        return 0;
    if(strncmp(p, "name", 4) == 0 &&
       (p[4] == '\0' || isspace((unsigned char)p[4])))
        return 1;
    if(strncmp(p, "metadata", 8) != 0 ||
       !isspace((unsigned char)p[8]))
        return 0;
    scan = p + 8;
    if(!krait_meta_parse_quoted(&scan, key, sizeof(key)))
        return 0;
    return krait_meta_managed_key(key);
}

static int
krait_meta_read_text_file(const char *root, const char *rel,
                          char *dst, size_t dst_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;

    krait_join(path, sizeof(path), root, rel);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    snprintf(dst, dst_size, "%s", text);
    free(text);
    return 1;
}

static int
krait_meta_write_text_rel(const char *root, const char *rel, const char *text)
{
    char path[KRAIT_PATH_MAX];
    char dir[KRAIT_PATH_MAX];
    char *slash;

    krait_join(path, sizeof(path), root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if(slash != NULL) {
        *slash = '\0';
        (void)kry_fs_mkdir_p(dir);
    }
    return krait_write_text_file(path, text != NULL ? text : "");
}

static void
krait_meta_detect_languages(KraitProjectMeta *meta)
{
    char dir_path[KRAIT_PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    size_t n = 0;

    if(meta == NULL)
        return;
    meta->languages[0] = '\0';
    krait_join(dir_path, sizeof(dir_path), meta->root,
               "fastlane/metadata/android");
    dir = opendir(dir_path);
    if(dir == NULL)
        return;
    while((entry = readdir(dir)) != NULL) {
        char child[KRAIT_PATH_MAX];
        struct stat st;

        if(entry->d_name[0] == '.')
            continue;
        krait_join(child, sizeof(child), dir_path, entry->d_name);
        if(stat(child, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if(n > 0 && n + 2 < sizeof(meta->languages)) {
            meta->languages[n++] = ',';
            meta->languages[n++] = ' ';
        }
        for(const char *p = entry->d_name;
            *p != '\0' && n + 1 < sizeof(meta->languages); p++)
            meta->languages[n++] = *p;
        meta->languages[n] = '\0';
    }
    closedir(dir);
}

static void
krait_meta_import_fastlane(KraitProjectMeta *meta)
{
    char rel[KRAIT_PATH_MAX];
    char path[KRAIT_PATH_MAX];

    if(meta == NULL || meta->root[0] == '\0')
        return;
    if(meta->language[0] == '\0')
        snprintf(meta->language, sizeof(meta->language), "%s", "en-US");
    snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/short_description.txt",
             meta->language);
    (void)krait_meta_read_text_file(meta->root, rel, meta->short_description,
                                    sizeof(meta->short_description));
    snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/full_description.txt",
             meta->language);
    (void)krait_meta_read_text_file(meta->root, rel, meta->full_description,
                                    sizeof(meta->full_description));
    if(meta->icon_path[0] == '\0') {
        snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/images/icon.png",
                 meta->language);
        krait_join(path, sizeof(path), meta->root, rel);
        if(krait_path_exists(path))
            snprintf(meta->icon_path, sizeof(meta->icon_path), "%s", rel);
    }
}

void
krait_meta_load(KraitProjectMeta *meta, const char *root)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;

    if(meta == NULL || root == NULL)
        return;
    memset(meta, 0, sizeof(*meta));
    snprintf(meta->root, sizeof(meta->root), "%s", root);
    snprintf(meta->language, sizeof(meta->language), "%s", "en-US");
    krait_join(path, sizeof(path), root, "project.kryon");
    if(krait_read_file_alloc(path, &text, &len)) {
        for(char *line = text; *line != '\0';) {
            char *end = strchr(line, '\n');
            char *p;
            int at_end = 0;

            if(end == NULL) {
                end = line + strlen(line);
                at_end = 1;
            }
            *end = '\0';
            p = krait_trim(line);
            if(strncmp(p, "name", 4) == 0 &&
               (p[4] == '\0' || isspace((unsigned char)p[4]))) {
                const char *scan = p + 4;
                (void)krait_meta_parse_quoted(&scan, meta->title,
                                              sizeof(meta->title));
            } else if(strncmp(p, "metadata", 8) == 0 &&
                      isspace((unsigned char)p[8])) {
                const char *scan = p + 8;
                char key[128];
                char value[4096];

                if(krait_meta_parse_quoted(&scan, key, sizeof(key)) &&
                   krait_meta_parse_quoted(&scan, value, sizeof(value)))
                    krait_meta_set_field(meta, key, value);
            }
            if(at_end)
                break;
            line = end + 1;
        }
        free(text);
    }
    if(meta->title[0] == '\0')
        snprintf(meta->title, sizeof(meta->title), "%s", krait_basename(root));
    krait_meta_detect_languages(meta);
    if(meta->short_description[0] == '\0' && meta->full_description[0] == '\0')
        krait_meta_import_fastlane(meta);
    meta->loaded = 1;
}

int
krait_meta_save(KraitProjectMeta *meta, char *status, int status_size)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    FILE *file;

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Metadata save failed");
    if(meta == NULL || meta->root[0] == '\0')
        return 0;
    krait_join(path, sizeof(path), meta->root, "project.kryon");
    (void)krait_read_file_alloc(path, &text, &len);
    file = fopen(path, "wb");
    if(file == NULL) {
        free(text);
        return 0;
    }
    fputs("name ", file);
    krait_meta_write_quoted(file, meta->title);
    fputc('\n', file);
    if(text != NULL) {
        for(char *line = text; *line != '\0';) {
            char *end = strchr(line, '\n');
            int at_end = 0;

            if(end == NULL) {
                end = line + strlen(line);
                at_end = 1;
            }
            *end = '\0';
            if(line[0] != '\0' && !krait_meta_line_managed(line))
                fprintf(file, "%s\n", line);
            if(at_end)
                break;
            line = end + 1;
        }
        free(text);
    }
    fputs("metadata \"app_id\" ", file);
    krait_meta_write_quoted(file, meta->app_id);
    fputc('\n', file);
    fputs("metadata \"version\" ", file);
    krait_meta_write_quoted(file, meta->version);
    fputc('\n', file);
    fputs("metadata \"icon\" ", file);
    krait_meta_write_quoted(file, meta->icon_path);
    fputc('\n', file);
    fputs("metadata \"language\" ", file);
    krait_meta_write_quoted(file, meta->language);
    fputc('\n', file);
    fputs("metadata \"short_description\" ", file);
    krait_meta_write_quoted(file, meta->short_description);
    fputc('\n', file);
    fputs("metadata \"full_description\" ", file);
    krait_meta_write_quoted(file, meta->full_description);
    fputc('\n', file);
    fclose(file);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Metadata saved");
    return 1;
}

static int
krait_meta_export_fastlane(KraitProjectMeta *meta, char *status, int status_size)
{
    char rel[KRAIT_PATH_MAX];

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Fastlane export failed");
    if(meta == NULL || meta->root[0] == '\0')
        return 0;
    if(meta->language[0] == '\0')
        snprintf(meta->language, sizeof(meta->language), "%s", "en-US");
    snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/short_description.txt",
             meta->language);
    if(!krait_meta_write_text_rel(meta->root, rel, meta->short_description))
        return 0;
    snprintf(rel, sizeof(rel), "fastlane/metadata/android/%s/full_description.txt",
             meta->language);
    if(!krait_meta_write_text_rel(meta->root, rel, meta->full_description))
        return 0;
    krait_meta_detect_languages(meta);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Fastlane metadata exported");
    return 1;
}

static UITextInputStyle
krait_meta_input_style(void)
{
    return (UITextInputStyle){
        GetThemeSurface(),
        GetThemeButton(),
        GetThemeLink(),
        GetThemeText(),
        GetThemeLink(),
        0,
        ScaleUIPx(8),
        ScaleUIPx(6)
    };
}

void
krait_meta_field(Rectangle rect, const char *label, char *text,
                 size_t text_size, int *cursor, int *focused, int id)
{
    DrawUIText(label, (int)rect.x, (int)rect.y, ScaleUIPx(12),
               GetThemeIcon());
    DrawUITextField((TextFieldProps){
        (Rectangle){rect.x, rect.y + (float)ScaleUIPx(18),
                    rect.width, (float)ScaleUIPx(32)},
        text, text_size, cursor, focused, (int)text_size - 1,
        ScaleUIPx(13), id, krait_meta_input_style(),
        (UITextInputFilter){0}, NULL, NULL
    });
}

void
krait_draw_project_metadata(Rectangle bounds, IdeState *st)
{
    KraitProjectMeta *meta = &g_project_meta;
    int pad = ScaleUIPx(12);
    int x = (int)bounds.x + pad;
    int y = (int)bounds.y + pad;
    int w = (int)bounds.width - pad * 2;
    char status[256];

    DrawRectangleRec(bounds, GetThemeSurface());
    if(st == NULL || !st->project.loaded) {
        DrawUIText("No project loaded", x, y, ScaleUIPx(14), GetThemeText());
        return;
    }
    if(!meta->loaded || strcmp(meta->root, st->project.path) != 0)
        krait_meta_load(meta, st->project.path);
    DrawUIText("Project Metadata", x, y, ScaleUIPx(16), GetThemeText());
    y += ScaleUIPx(30);
    krait_meta_field((Rectangle){x, y, w, 0}, "Title",
                     meta->title, sizeof(meta->title),
                     &meta->cursor[0], &meta->focused[0], 7100);
    y += ScaleUIPx(58);
    krait_meta_field((Rectangle){x, y, w, 0}, "App ID",
                     meta->app_id, sizeof(meta->app_id),
                     &meta->cursor[1], &meta->focused[1], 7101);
    y += ScaleUIPx(58);
    krait_meta_field((Rectangle){x, y, w, 0}, "Version",
                     meta->version, sizeof(meta->version),
                     &meta->cursor[2], &meta->focused[2], 7102);
    y += ScaleUIPx(58);
    krait_meta_field((Rectangle){x, y, w, 0}, "Logo/Icon Path",
                     meta->icon_path, sizeof(meta->icon_path),
                     &meta->cursor[3], &meta->focused[3], 7103);
    y += ScaleUIPx(58);
    krait_meta_field((Rectangle){x, y, w, 0}, "Language",
                     meta->language, sizeof(meta->language),
                     &meta->cursor[4], &meta->focused[4], 7104);
    y += ScaleUIPx(54);
    if(meta->languages[0] != '\0') {
        char line[640];
        snprintf(line, sizeof(line), "Detected: %s", meta->languages);
        DrawUIText(line, x, y, ScaleUIPx(11), GetThemeIcon());
        y += ScaleUIPx(18);
    }
    krait_meta_field((Rectangle){x, y, w, 0}, "Short Description",
                     meta->short_description, sizeof(meta->short_description),
                     &meta->cursor[5], &meta->focused[5], 7105);
    y += ScaleUIPx(58);
    DrawUIText("Full Description", x, y, ScaleUIPx(12), GetThemeIcon());
    y += ScaleUIPx(18);
    DrawUITextArea((TextAreaProps){
        (Rectangle){x, y, w, (float)ScaleUIPx(130)},
        meta->full_description, sizeof(meta->full_description),
        &meta->cursor[6], &meta->focused[6], &meta->full_scroll,
        (int)sizeof(meta->full_description) - 1, ScaleUIPx(13),
        ScaleUIPx(4), 7106, "", UI_SYNTAX_NONE,
        krait_meta_input_style(), (UITextInputFilter){0}, NULL
    });
    y += ScaleUIPx(142);
    if(DrawUIGenericButton(x, y, ScaleUIPx(56), ScaleUIPx(28), "Save",
                           UI_BUTTON_STYLE_PRIMARY, 0, NULL)) {
        if(krait_meta_save(meta, status, sizeof(status)))
            snprintf(st->project.name, sizeof(st->project.name), "%s", meta->title);
        snprintf(st->status, sizeof(st->status), "%s", status);
    }
    if(DrawUIGenericButton(x + ScaleUIPx(64), y, ScaleUIPx(92), ScaleUIPx(28),
                           "Import", UI_BUTTON_STYLE_SECONDARY, 0, NULL)) {
        krait_meta_import_fastlane(meta);
        snprintf(st->status, sizeof(st->status), "%s", "Fastlane metadata imported");
    }
    if(DrawUIGenericButton(x + ScaleUIPx(164), y, ScaleUIPx(92), ScaleUIPx(28),
                           "Export", UI_BUTTON_STYLE_SECONDARY, 0, NULL)) {
        (void)krait_meta_export_fastlane(meta, status, sizeof(status));
        snprintf(st->status, sizeof(st->status), "%s", status);
    }
    if(DrawUIGenericButton(x + ScaleUIPx(264), y, ScaleUIPx(72), ScaleUIPx(28),
                           "Reload", UI_BUTTON_STYLE_SECONDARY, 0, NULL)) {
        krait_meta_load(meta, st->project.path);
        snprintf(st->status, sizeof(st->status), "%s", "Metadata reloaded");
    }
}

