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

const char *
krait_live_skip_space(const char *p)
{
    while(p != NULL && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

int
krait_live_starts_word(const char *p, const char *word)
{
    size_t n;

    if(p == NULL || word == NULL)
        return 0;
    p = krait_live_skip_space(p);
    n = strlen(word);
    if(strncmp(p, word, n) != 0)
        return 0;
    return !krait_ident_char((unsigned char)p[n]);
}

char *
krait_live_trim(char *s)
{
    char *end;

    if(s == NULL)
        return s;
    while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + strlen(s);
    while(end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                      end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

int
krait_live_parse_string(char **sp, char *dst, size_t dst_size)
{
    char *p = krait_live_trim(*sp);
    size_t n = 0;

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
    if(dst_size > 0)
        dst[n] = '\0';
    *sp = p + 1;
    return 1;
}

int
krait_live_split_args(const char *src, char args[][256], int cap)
{
    int count = 0;
    int depth = 0;
    int in_string = 0;
    char cur[256];
    int n = 0;

    memset(cur, 0, sizeof(cur));
    for(const char *p = src; *p != '\0'; p++) {
        int ch = *p;
        if(in_string) {
            if(ch == '\\' && p[1] != '\0') {
                if(n + 1 < (int)sizeof(cur))
                    cur[n++] = (char)ch;
                ch = *++p;
            } else if(ch == '"') {
                in_string = 0;
            }
        } else if(ch == '"') {
            in_string = 1;
        } else if(ch == '(' || ch == '{' || ch == '[') {
            depth++;
        } else if(ch == ')' || ch == '}' || ch == ']') {
            depth--;
        } else if(ch == ',' && depth == 0) {
            if(count < cap) {
                cur[n] = '\0';
                snprintf(args[count], 256, "%s", krait_live_trim(cur));
                count++;
            }
            n = 0;
            cur[0] = '\0';
            continue;
        }
        if(n + 1 < (int)sizeof(cur))
            cur[n++] = (char)ch;
    }
    if(n > 0 || src[0] != '\0') {
        if(count < cap) {
            cur[n] = '\0';
            snprintf(args[count], 256, "%s", krait_live_trim(cur));
            count++;
        }
    }
    return count;
}

int
krait_live_call_args(const char *line, const char *name, char args[][256], int cap)
{
    const char *p;
    const char *open;
    const char *close;
    char inner[1024];
    size_t n;

    p = krait_live_skip_space(line);
    if(strncmp(p, name, strlen(name)) != 0)
        return -1;
    p += strlen(name);
    p = krait_live_skip_space(p);
    if(*p != '(')
        return -1;
    open = p + 1;
    close = strrchr(open, ')');
    if(close == NULL || close < open)
        return -1;
    n = (size_t)(close - open);
    if(n >= sizeof(inner))
        n = sizeof(inner) - 1;
    memcpy(inner, open, n);
    inner[n] = '\0';
    return krait_live_split_args(inner, args, cap);
}

int
krait_live_eval_int(const char *expr, int *out)
{
    char buf[256];
    char *p;
    char *end;

    if(expr == NULL || out == NULL)
        return 0;
    snprintf(buf, sizeof(buf), "%s", expr);
    p = krait_live_trim(buf);
    if(p[0] == '(') {
        char *close = strrchr(p + 1, ')');
        if(close != NULL && close[1] == '\0') {
            *close = '\0';
            return krait_live_eval_int(p + 1, out);
        }
    }
    if(strncmp(p, "ScaleUIPx(", 10) == 0) {
        char *q = p + 10;
        char *close = strrchr(q, ')');
        int v = 0;
        if(close == NULL)
            return 0;
        *close = '\0';
        if(!krait_live_eval_int(q, &v))
            return 0;
        *out = ScaleUIPx(v);
        return 1;
    }
    if(strcmp(p, "UI_TEXT_12") == 0) {
        *out = UI_TEXT_12;
        return 1;
    }
    if(strcmp(p, "UI_TEXT_16") == 0) {
        *out = UI_TEXT_16;
        return 1;
    }
    if(strcmp(p, "UI_TEXT_24") == 0) {
        *out = UI_TEXT_24;
        return 1;
    }
    if(strcmp(p, "view_width") == 0 || strcmp(p, "GetScreenWidth()") == 0) {
        return krait_live_var_get("view_width", out);
    }
    if(strcmp(p, "view_height") == 0 || strcmp(p, "GetScreenHeight()") == 0) {
        return krait_live_var_get("view_height", out);
    }
    if(krait_live_var_get(p, out))
        return 1;
    if(strchr(p, '(') != NULL && strchr(p, ')') != NULL) {
        if(strstr(p, "height") != NULL || strstr(p, "Height") != NULL) {
            *out = ScaleUIPx(140);
            return 1;
        }
        if(strstr(p, "width") != NULL || strstr(p, "Width") != NULL) {
            *out = ScaleUIPx(280);
            return 1;
        }
        if(strstr(p, "top") != NULL || strstr(p, "Top") != NULL) {
            *out = ScaleUIPx(48);
            return 1;
        }
        *out = ScaleUIPx(24);
        return 1;
    }
    if(strstr(p, "+") != NULL) {
        char *op = strchr(p, '+');
        int a = 0, b = 0;
        *op = '\0';
        if(krait_live_eval_int(p, &a) && krait_live_eval_int(op + 1, &b)) {
            *out = a + b;
            return 1;
        }
        return 0;
    }
    if(strstr(p, "*") != NULL) {
        char *op = strchr(p, '*');
        int a = 0, b = 0;
        *op = '\0';
        if(krait_live_eval_int(p, &a) && krait_live_eval_int(op + 1, &b)) {
            *out = a * b;
            return 1;
        }
        return 0;
    }
    if(strstr(p, "/") != NULL) {
        char *op = strchr(p, '/');
        int a = 0, b = 0;
        *op = '\0';
        if(krait_live_eval_int(p, &a) && krait_live_eval_int(op + 1, &b) && b != 0) {
            *out = a / b;
            return 1;
        }
        return 0;
    }
    if(strstr(p, "-") != NULL && p[0] != '-') {
        char *op = strchr(p, '-');
        int a = 0, b = 0;
        *op = '\0';
        if(krait_live_eval_int(p, &a) && krait_live_eval_int(op + 1, &b)) {
            *out = a - b;
            return 1;
        }
        return 0;
    }
    *out = (int)strtol(p, &end, 10);
    return end != p && *krait_live_trim(end) == '\0';
}

int
krait_live_next_scale_arg(char **sp, int *out)
{
    char *p;
    char *q;
    char buf[128];
    size_t n;

    if(sp == NULL || *sp == NULL || out == NULL)
        return 0;
    p = strstr(*sp, "ScaleUIPx(");
    if(p == NULL)
        return 0;
    p += 10;
    q = strchr(p, ')');
    if(q == NULL)
        return 0;
    n = (size_t)(q - p);
    if(n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, p, n);
    buf[n] = '\0';
    *sp = q + 1;
    return krait_live_eval_int(buf, out);
}

int
krait_live_parse_ident(char **sp, char *dst, size_t dst_size)
{
    char *p;
    size_t n = 0;

    if(sp == NULL || *sp == NULL || dst == NULL || dst_size == 0)
        return 0;
    p = krait_live_trim(*sp);
    if(!krait_ident_start((unsigned char)*p))
        return 0;
    while(krait_ident_char((unsigned char)*p)) {
        if(n + 1 < dst_size)
            dst[n++] = *p;
        p++;
    }
    dst[n] = '\0';
    *sp = p;
    return n > 0;
}

int
krait_live_eval_color(const char *expr, Color *out)
{
    char buf[256];
    char *p;

    if(expr == NULL || out == NULL)
        return 0;
    snprintf(buf, sizeof(buf), "%s", expr);
    p = krait_live_trim(buf);
    if(strcmp(p, "GetThemeText()") == 0) {
        *out = GetThemeText();
        return 1;
    }
    if(strcmp(p, "GetThemeIcon()") == 0) {
        *out = GetThemeIcon();
        return 1;
    }
    if(strcmp(p, "GetThemeBackground()") == 0) {
        *out = GetThemeBackground();
        return 1;
    }
    if(strcmp(p, "GetThemeSurface()") == 0) {
        *out = GetThemeSurface();
        return 1;
    }
    if(strcmp(p, "GetThemeButton()") == 0) {
        *out = GetThemeButton();
        return 1;
    }
    if(strcmp(p, "GetThemeButtonHover()") == 0) {
        *out = GetThemeButtonHover();
        return 1;
    }
    if(strcmp(p, "GetThemeLink()") == 0) {
        *out = GetThemeLink();
        return 1;
    }
    if(strcmp(p, "WHITE") == 0) {
        *out = WHITE;
        return 1;
    }
    if(strcmp(p, "BLACK") == 0) {
        *out = BLACK;
        return 1;
    }
    if(strcmp(p, "BLANK") == 0) {
        *out = BLANK;
        return 1;
    }
    if(strncmp(p, "Fade(", 5) == 0) {
        char *inner = p + 5;
        char *comma = strrchr(inner, ',');
        char *close = strrchr(inner, ')');
        float alpha;
        Color base;

        if(comma == NULL || close == NULL || comma > close)
            return 0;
        *comma = '\0';
        alpha = (float)strtod(comma + 1, NULL);
        if(!krait_live_eval_color(inner, &base))
            return 0;
        *out = Fade(base, alpha);
        return 1;
    }
    return 0;
}

static UIButtonStyle
krait_live_eval_button_style(const char *expr)
{
    if(expr != NULL && strstr(expr, "SECONDARY") != NULL)
        return UI_BUTTON_STYLE_SECONDARY;
    if(expr != NULL && strstr(expr, "DANGER") != NULL)
        return UI_BUTTON_STYLE_DANGER;
    if(expr != NULL && strstr(expr, "TAB_SELECTED") != NULL)
        return UI_BUTTON_STYLE_TAB_SELECTED;
    if(expr != NULL && strstr(expr, "TAB") != NULL)
        return UI_BUTTON_STYLE_TAB;
    return UI_BUTTON_STYLE_PRIMARY;
}

int
krait_live_find_named_body(const char *text, const char *keyword,
                           const char *name, const char **body,
                           const char **body_end)
{
    const char *p = text;
    size_t keyword_len = strlen(keyword);

    while((p = strstr(p, keyword)) != NULL) {
        const char *q;
        const char *open;
        int depth = 1;

        if(p > text && krait_ident_char((unsigned char)p[-1])) {
            p += keyword_len;
            continue;
        }
        q = krait_live_skip_space(p + keyword_len);
        if(name != NULL && name[0] != '\0') {
            size_t name_len = strlen(name);
            if(strncmp(q, name, name_len) != 0 ||
               krait_ident_char((unsigned char)q[name_len])) {
                p += keyword_len;
                continue;
            }
        }
        open = strchr(q, '{');
        if(open == NULL)
            return 0;
        for(q = open + 1; *q != '\0'; q++) {
            if(*q == '{')
                depth++;
            else if(*q == '}') {
                depth--;
                if(depth == 0) {
                    *body = open + 1;
                    *body_end = q;
                    return 1;
                }
            }
        }
        return 0;
    }
    return 0;
}

int
krait_live_find_function_body(const char *text, const char *name,
                              const char **body, const char **body_end)
{
    const char *p;
    size_t name_len;

    if(text == NULL || name == NULL || name[0] == '\0')
        return 0;
    name_len = strlen(name);
    p = text;
    while((p = strstr(p, name)) != NULL) {
        const char *q;
        const char *open;
        int depth = 1;

        if(p > text && krait_ident_char((unsigned char)p[-1])) {
            p += name_len;
            continue;
        }
        if(krait_ident_char((unsigned char)p[name_len])) {
            p += name_len;
            continue;
        }
        q = krait_live_skip_space(p + name_len);
        if(q[0] != ':' || q[1] != ':') {
            p += name_len;
            continue;
        }
        open = strchr(q, '{');
        if(open == NULL)
            return 0;
        for(q = open + 1; *q != '\0'; q++) {
            if(*q == '{')
                depth++;
            else if(*q == '}') {
                depth--;
                if(depth == 0) {
                    *body = open + 1;
                    *body_end = q;
                    return 1;
                }
            }
        }
        return 0;
    }
    return 0;
}

int
krait_live_line_for_ptr(const char *text, const char *ptr)
{
    int line = 1;

    if(text == NULL || ptr == NULL)
        return 1;
    for(const char *p = text; p < ptr && *p != '\0'; p++)
        if(*p == '\n')
            line++;
    return line;
}

int
krait_live_find_frame_name(const char *text, char *dst, size_t dst_size)
{
    const char *app = strstr(text, "app ");
    const char *body;
    const char *end;

    if(dst_size > 0)
        dst[0] = '\0';
    if(app == NULL || !krait_live_find_named_body(app, "app", "", &body, &end))
        return 0;
    for(const char *p = body; p < end;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        char line[256];
        size_t n = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
        char *q;
        if(n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        q = krait_live_trim(line);
        if(krait_live_starts_word(q, "frame")) {
            q = krait_live_trim(q + strlen("frame"));
            if(krait_live_parse_ident(&q, dst, dst_size))
                return 1;
        }
        if(nl == NULL)
            break;
        p = nl + 1;
    }
    return 0;
}

static int krait_live_exec_body(KraitLive *live);

