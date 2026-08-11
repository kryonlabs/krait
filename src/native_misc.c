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

static void
krait_widget_snippet(char *dst, size_t dst_size, int widget_type, int x, int y)
{
    if(dst == NULL || dst_size == 0)
        return;
    if(!KryonNodeTypeSnippet(widget_type, x, y, dst, (int)dst_size))
        snprintf(dst, dst_size, "\n    /* Node is not insertable yet. */\n");
}

int
krait_insert_widget(const char *root, const char *rel_path, int insert_offset,
                    int widget_type, int x, int y, char *status,
                    int status_size)
{
    char path[KRAIT_PATH_MAX];
    char *text;
    long len;
    char snippet[2048];
    FILE *file;

    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Node insert failed");
    if(root == NULL || rel_path == NULL || insert_offset < 0)
        return 0;
    krait_join(path, sizeof(path), root, rel_path);
    if(!krait_read_file_alloc(path, &text, &len))
        return 0;
    if(insert_offset > len) {
        free(text);
        return 0;
    }
    krait_widget_snippet(snippet, sizeof(snippet), widget_type, x, y);
    file = fopen(path, "wb");
    if(file == NULL) {
        free(text);
        return 0;
    }
    if(insert_offset > 0)
        fwrite(text, 1, (size_t)insert_offset, file);
    fwrite(snippet, 1, strlen(snippet), file);
    fwrite(text + insert_offset, 1, (size_t)(len - insert_offset), file);
    fclose(file);
    free(text);
    if(status != NULL && status_size > 0)
        snprintf(status, (size_t)status_size, "Node added");
    return 1;
}

