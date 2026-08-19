/*
 * native_md.c - lightweight markdown layout for the agent transcript and
 * the markdown viewer. Parses a GFM-ish subset (headings, fenced code,
 * inline code/bold/italic/links, lists, blockquotes, hr) into styled rows
 * of runs, wrapped to a pixel width with MeasureText. The layout is
 * cached against the source text so per-frame transcript drawing only
 * pays a strcmp per message.
 *
 * Kry-facing API:
 *   krait_md_rows(text, width, font)                  total wrapped rows
 *   krait_md_row_info(..., row, &kind,&runs,&indent,&bg)
 *   krait_md_run_info(..., row, run, dst, cap, &x, &style)
 * Style bits: 1 bold, 2 italic, 4 code, 8 link.
 * Row kinds: 0 text, 1..3 heading levels, 4 code, 5 quote, 6 bullet,
 * 7 numbered item, 8 horizontal rule.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MD_STYLE_BOLD 1
#define MD_STYLE_ITALIC 2
#define MD_STYLE_CODE 4
#define MD_STYLE_LINK 8

#define MD_KIND_TEXT 0
#define MD_KIND_H1 1
#define MD_KIND_H2 2
#define MD_KIND_H3 3
#define MD_KIND_CODE 4
#define MD_KIND_QUOTE 5
#define MD_KIND_BULLET 6
#define MD_KIND_NUMITEM 7
#define MD_KIND_HR 8

#define MD_MAX_ROWS 2048
#define MD_MAX_RUNS 8192
#define MD_POOL_CAP 65536
#define MD_INDENT_PX 14   /* Kry multiplies by ScaleUIPx factor */

typedef struct {
    int off;      /* offset into md_pool */
    short len;
    short style;
} MdRun;

typedef struct {
    int first_run;
    int run_count;
    char kind;
    char indent;   /* indent units */
    char bg;       /* 1 = surface background behind the row */
    char pad_top;  /* extra breathing room above the row */
} MdRow;

static char md_pool[MD_POOL_CAP];
static int md_pool_used;
static MdRun md_runs[MD_MAX_RUNS];
static int md_run_count;
static MdRow md_rows[MD_MAX_ROWS];
static int md_row_count;

static char md_cache_src[MD_POOL_CAP];
static int md_cache_len = -1;
static int md_cache_width = -1;
static int md_cache_font = -1;
static int md_cache_valid;

/* viewer file cache: one file loaded at a time, keyed by path */
static char md_file_path[KRAIT_PATH_MAX * 2];
static char md_file_text[MD_POOL_CAP];

static int
md_pool_put(const char *s, int len)
{
    int off;

    if(len < 0)
        len = (int)strlen(s);
    if(md_pool_used + len + 1 > MD_POOL_CAP)
        return -1;
    off = md_pool_used;
    memcpy(md_pool + off, s, (size_t)len);
    md_pool[off + len] = '\0';
    md_pool_used += len + 1;
    return off;
}

static void
md_reset(void)
{
    md_pool_used = 0;
    md_run_count = 0;
    md_row_count = 0;
}

static int
md_add_row(int kind, int indent, int bg, int pad_top, int first_run,
           int run_count)
{
    MdRow *r;

    if(md_row_count >= MD_MAX_ROWS || run_count < 0)
        return -1;
    r = &md_rows[md_row_count++];
    r->first_run = first_run;
    r->run_count = run_count;
    r->kind = (char)kind;
    r->indent = (char)indent;
    r->bg = (char)bg;
    r->pad_top = (char)pad_top;
    return md_row_count - 1;
}

/* ---- inline parser: turns one markdown line into styled word runs ----
 * Words carry their style; wrapping reassembles them into rows. Returns
 * the number of words appended to words[] (text copied into pool). */
typedef struct {
    int off;      /* pool offset of the word text (no trailing space) */
    short len;
    short style;
} MdWord;

#define MD_MAX_WORDS 512

static int
md_inline_words(const char *s, MdWord *words, int cap, int extra_style)
{
    int count = 0;
    int style = extra_style;
    char buf[512];
    int buf_len = 0;
    int i = 0;

#define MD_FLUSH() do { \
        if(buf_len > 0 && count < cap) { \
            int off = md_pool_put(buf, buf_len); \
            if(off >= 0) { words[count].off = off; \
                           words[count].len = (short)buf_len; \
                           words[count].style = (short)style; count++; } \
        } \
        buf_len = 0; \
    } while(0)

    while(s[i] != '\0') {
        char c = s[i];

        if(c == ' ' || c == '\t') {
            MD_FLUSH();
            i++;
            continue;
        }
        if(c == '\\' && s[i + 1] != '\0') {
            buf[buf_len < 512 ? buf_len++ : 511] = s[i + 1];
            i += 2;
            continue;
        }
        if(c == '`') {
            int j = i + 1;

            while(s[j] != '\0' && s[j] != '`')
                j++;
            if(j > i + 1) {
                MD_FLUSH();
                {
                    int save = style;

                    style |= MD_STYLE_CODE;
                    for(int k = i + 1; k < j && k - i - 1 < 500; k++)
                        buf[buf_len < 512 ? buf_len++ : 511] = s[k];
                    MD_FLUSH();
                    style = save;
                }
                i = j + 1;
                continue;
            }
        }
        if(c == '*' && s[i + 1] == '*') {
            MD_FLUSH();
            style ^= MD_STYLE_BOLD;
            i += 2;
            continue;
        }
        if((c == '*' || c == '_') && s[i + 1] != ' ' && s[i + 1] != '\0') {
            char close = c;
            int j = i + 1;

            while(s[j] != '\0' && s[j] != close && s[j] != ' ')
                j++;
            if(s[j] == close && j > i + 1) {
                MD_FLUSH();
                {
                    int save = style;

                    style |= MD_STYLE_ITALIC;
                    for(int k = i + 1; k < j && k - i - 1 < 500; k++)
                        buf[buf_len < 512 ? buf_len++ : 511] = s[k];
                    MD_FLUSH();
                    style = save;
                }
                i = j + 1;
                continue;
            }
        }
        if(c == '[') {
            const char *cb = strchr(s + i, ']');

            if(cb != NULL && cb[1] == '(') {
                const char *close = strchr(cb + 2, ')');

                if(close != NULL) {
                    MD_FLUSH();
                    {
                        int save = style;

                        style |= MD_STYLE_LINK;
                        for(const char *k = s + i; k < cb && buf_len < 511; k++)
                            buf[buf_len++] = *k;
                        MD_FLUSH();
                        style = save;
                    }
                    i = (int)(close - s) + 1;
                    continue;
                }
            }
        }
        buf[buf_len < 512 ? buf_len++ : 511] = c;
        i++;
    }
    MD_FLUSH();
#undef MD_FLUSH
    return count;
}

/* Greedy word wrap into rows; each row's runs carry x offsets. */
static void
md_wrap_words(MdWord *words, int count, int width, int font, int kind,
              int indent, int bg, int pad_top)
{
    int i = 0;
    int first = 1;

    while(i < count) {
        int base_run = md_run_count;
        int row_words = 0;
        char plain[2048];
        int plain_len = 0;

        while(i < count) {
            int wlen = words[i].len;
            int add = wlen + (row_words > 0 ? 1 : 0);

            if(plain_len + add >= (int)sizeof(plain) - 1)
                break;
            if(row_words > 0) {
                plain[plain_len++] = ' ';
            }
            memcpy(plain + plain_len, md_pool + words[i].off, (size_t)wlen);
            plain_len += wlen;
            plain[plain_len] = '\0';
            if(MeasureText(plain, font) > width && row_words > 0) {
                plain_len -= add;
                plain[plain_len] = '\0';
                break;
            }
            if(md_run_count < MD_MAX_RUNS) {
                md_runs[md_run_count].off = words[i].off;
                md_runs[md_run_count].len = words[i].len;
                md_runs[md_run_count].style = words[i].style;
                md_run_count++;
            }
            row_words++;
            i++;
        }
        if(row_words > 0)
            md_add_row(kind, indent, bg, first ? pad_top : 0, base_run,
                       row_words);
        first = 0;
    }
}

/* Render any pending code-block line as its own row. */
static void
md_code_row(const char *line, int lang_marker)
{
    int base = md_run_count;

    (void)lang_marker;
    if(md_run_count < MD_MAX_RUNS) {
        md_runs[md_run_count].off = md_pool_put(line, -1);
        md_runs[md_run_count].len = (short)strlen(line);
        md_runs[md_run_count].style = MD_STYLE_CODE;
        md_run_count++;
    }
    if(md_run_count > base)
        md_add_row(MD_KIND_CODE, 0, 1, 0, base, 1);
}

static void
md_build(const char *text, int width, int font)
{
    const char *p = text;
    int in_code = 0;
    MdWord words[MD_MAX_WORDS];

    md_reset();
    if(text == NULL)
        return;
    while(*p != '\0') {
        const char *nl = strchr(p, '\n');
        int len = nl != NULL ? (int)(nl - p) : (int)strlen(p);
        char line[1024];

        if(len > (int)sizeof(line) - 1)
            len = (int)sizeof(line) - 1;
        memcpy(line, p, (size_t)len);
        line[len] = '\0';
        /* strip \r */
        {
            char *cr = strchr(line, '\r');

            if(cr != NULL)
                *cr = '\0';
        }
        if(in_code) {
            if(line[0] == '`' && line[1] == '`' && line[2] == '`')
                in_code = 0;
            else
                md_code_row(line, 0);
        } else if(line[0] == '`' && line[1] == '`' && line[2] == '`') {
            in_code = 1;
        } else if(line[0] == '#') {
            int level = 0;

            while(line[level] == '#' && level < 3)
                level++;
            if(line[level] == ' ') {
                int n = md_inline_words(line + level + 1, words,
                                        MD_MAX_WORDS, MD_STYLE_BOLD);

                md_wrap_words(words, n, width, font + (level == 1 ? 4 :
                              level == 2 ? 2 : 0),
                              MD_KIND_H1 - 1 + level, 0, 0, 1);
            } else {
                int n = md_inline_words(line, words, MD_MAX_WORDS, 0);

                md_wrap_words(words, n, width, font, MD_KIND_TEXT, 0, 0, 0);
            }
        } else if(strlen(line) >= 3 &&
                  strspn(line, "-*_ \t") == strlen(line)) {
            md_add_row(MD_KIND_HR, 0, 0, 1, -1, 0);
        } else if(line[0] == '>') {
            int n = md_inline_words(line[1] == ' ' ? line + 2 : line + 1,
                                    words, MD_MAX_WORDS, 0);

            md_wrap_words(words, n, width, font, MD_KIND_QUOTE, 1, 0, 0);
        } else if((line[0] == '-' || line[0] == '*' || line[0] == '+') &&
                  line[1] == ' ') {
            int n = md_inline_words(line + 2, words, MD_MAX_WORDS, 0);

            md_wrap_words(words, n, width - MD_INDENT_PX, font,
                          MD_KIND_BULLET, 1, 0, 0);
        } else if(line[0] >= '0' && line[0] <= '9') {
            const char *dot = strchr(line, '.');

            if(dot != NULL && dot[1] == ' ') {
                int n = md_inline_words(dot + 2, words, MD_MAX_WORDS, 0);

                md_wrap_words(words, n, width - MD_INDENT_PX * 3, font,
                              MD_KIND_NUMITEM, 3, 0, 0);
            } else {
                int n = md_inline_words(line, words, MD_MAX_WORDS, 0);

                md_wrap_words(words, n, width, font, MD_KIND_TEXT, 0, 0, 0);
            }
        } else if(line[0] == '\0') {
            /* blank line: pad the next row via a zero-height spacer flag */
            if(md_row_count > 0 && md_row_count < MD_MAX_ROWS)
                md_rows[md_row_count - 1].pad_top = 2;
        } else {
            int n = md_inline_words(line, words, MD_MAX_WORDS, 0);

            md_wrap_words(words, n, width, font, MD_KIND_TEXT, 0, 0, 0);
        }
        if(nl == NULL)
            break;
        p = nl + 1;
    }
}

static int
md_layout(const char *text, int width, int font)
{
    int len;

    if(text == NULL || width <= 0)
        return 0;
    len = (int)strlen(text);
    if(len >= MD_POOL_CAP)
        len = MD_POOL_CAP - 1;
    if(md_cache_valid && md_cache_len == len && md_cache_width == width &&
       md_cache_font == font && memcmp(md_cache_src, text, (size_t)len) == 0)
        return md_row_count;
    snprintf(md_cache_src, sizeof(md_cache_src), "%s", text);
    md_cache_len = len;
    md_cache_width = width;
    md_cache_font = font;
    md_build(text, width, font);
    md_cache_valid = 1;
    return md_row_count;
}

int
krait_md_rows(const char *text, int width, int font)
{
    return md_layout(text, width, font);
}

int
krait_md_row_info(const char *text, int width, int font, int row, int *kind,
                  int *runs, int *indent, int *bg)
{
    if(kind != NULL)
        *kind = MD_KIND_TEXT;
    if(runs != NULL)
        *runs = 0;
    if(indent != NULL)
        *indent = 0;
    if(bg != NULL)
        *bg = 0;
    if(row < 0 || row >= md_layout(text, width, font))
        return 0;
    if(kind != NULL)
        *kind = md_rows[row].kind;
    if(runs != NULL)
        *runs = md_rows[row].run_count;
    if(indent != NULL)
        *indent = md_rows[row].indent;
    if(bg != NULL)
        *bg = md_rows[row].bg;
    return 1;
}

int
krait_md_run_info(const char *text, int width, int font, int row, int run,
                  char *dst, int dst_size, int *x, int *style)
{
    const MdRow *r;
    const MdRun *g;
    int px = 0;
    int i;

    if(dst != NULL && dst_size > 0)
        dst[0] = '\0';
    if(x != NULL)
        *x = 0;
    if(style != NULL)
        *style = 0;
    if(row < 0 || row >= md_layout(text, width, font))
        return 0;
    r = &md_rows[row];
    if(run < 0 || run >= r->run_count)
        return 0;
    g = &md_runs[r->first_run + run];
    for(i = 0; i < run; i++)
        px += MeasureText(md_pool + md_runs[r->first_run + i].off, font) +
              MeasureText(" ", font);
    if(x != NULL)
        *x = px;
    if(style != NULL)
        *style = g->style;
    if(dst != NULL && dst_size > 0) {
        int len = g->len;

        if(len > dst_size - 1)
            len = dst_size - 1;
        memcpy(dst, md_pool + g->off, (size_t)len);
        dst[len] = '\0';
    }
    return 1;
}

/* indent unit in scaled pixels, for the Kry drawing side */
int
krait_md_indent_px(void)
{
    return MD_INDENT_PX;
}

/* Load a markdown file once for the viewer; returns NULL on failure.
 * The returned pointer is a static buffer valid until the next load. */
const char *
krait_md_file_text(const char *path)
{
    char *data = NULL;
    long len;

    if(path == NULL || path[0] == '\0')
        return NULL;
    if(strcmp(md_file_path, path) == 0)
        return md_file_text;
    if(!krait_read_file_alloc(path, &data, &len) || data == NULL) {
        free(data);
        return NULL;
    }
    snprintf(md_file_path, sizeof(md_file_path), "%s", path);
    md_file_text[0] = '\0';
    strncat(md_file_text, data, sizeof(md_file_text) - 1);
    free(data);
    return md_file_text;
}

/* ---- markdown viewer source (static copy, safe across agent clears) ---- */
static char md_view_text[MD_POOL_CAP];
static char md_view_title[256];

void
krait_md_view_set(const char *text, const char *title)
{
    snprintf(md_view_title, sizeof(md_view_title), "%s",
             title != NULL ? title : "markdown");
    md_view_text[0] = '\0';
    if(text != NULL)
        strncat(md_view_text, text, sizeof(md_view_text) - 1);
}

int
krait_md_view_open_file(const char *path)
{
    const char *text = krait_md_file_text(path);

    if(text == NULL)
        return 0;
    krait_md_view_set(text, path);
    return 1;
}

const char *
krait_md_view_text(void)
{
    return md_view_text;
}

const char *
krait_md_view_title(void)
{
    return md_view_title;
}
