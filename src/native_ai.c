/*
 * native_ai.c - krait's z.ai GLM client.
 *
 * Krait is the harness, so the vendor specifics live here: the GLM
 * Coding-Plan endpoint (the general API answers 'insufficient balance' for
 * Coding Plan keys), the glm-4.6 default model, and the env/override key
 * handling. Transport is kryon's provider-neutral kry_http + kry_json.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include "kry_http.h"
#include "kry_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KRAIT_AI_DEFAULT_BASE "https://api.z.ai/api/coding/paas/v4"
#define KRAIT_AI_DEFAULT_MODEL "glm-4.6"

struct KraitAiRequest {
    KryHttpRequest *http;
    char *request_body;
    char *text;
};

static char g_key_override[256];

void
krait_ai_set_key(const char *api_key)
{
    if(api_key == NULL)
        api_key = "";
    snprintf(g_key_override, sizeof(g_key_override), "%s", api_key);
}

static const char *
krait_ai_key(void)
{
    if(g_key_override[0] != '\0')
        return g_key_override;
    return getenv("ZAI_API_KEY");
}

int
krait_ai_configured(void)
{
    const char *key = krait_ai_key();

    return key != NULL && key[0] != '\0';
}

static const char *
krait_ai_model(void)
{
    const char *model = getenv("ZAI_MODEL");

    return model != NULL && model[0] != '\0' ? model : KRAIT_AI_DEFAULT_MODEL;
}

static const char *
krait_ai_vision_model(void)
{
    const char *model = getenv("ZAI_VISION_MODEL");

    return model != NULL && model[0] != '\0' ? model : "glm-4.6v";
}

/* base64 of raw bytes; malloc'd, caller frees. Used for image parts. */
static char *
krait_ai_base64(const unsigned char *data, size_t len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc(((len + 2) / 3) * 4 + 1);
    size_t i;
    size_t o = 0;

    if(out == NULL)
        return NULL;
    for(i = 0; i < len; i += 3) {
        unsigned v = (unsigned)data[i] << 16;

        if(i + 1 < len)
            v |= (unsigned)data[i + 1] << 8;
        if(i + 2 < len)
            v |= (unsigned)data[i + 2];
        out[o++] = table[(v >> 18) & 63];
        out[o++] = table[(v >> 12) & 63];
        out[o++] = i + 1 < len ? table[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? table[v & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

/* base64 of a file's contents; malloc'd, NULL when unreadable */
char *
krait_ai_base64_file(const char *path)
{
    char *data = NULL;
    long len;
    char *encoded;

    if(!krait_read_file_alloc(path, &data, &len) || data == NULL || len <= 0) {
        free(data);
        return NULL;
    }
    encoded = krait_ai_base64((const unsigned char *)data, (size_t)len);
    free(data);
    return encoded;
}

/* Pure request-body builder so tests can verify multimodality without
 * spending a request. malloc'd JSON, caller frees. */
char *
krait_ai_build_body(const KraitAiMessage *messages, int count)
{
    KryJsonBuf body = {0};
    int i;

    if(messages == NULL || count <= 0)
        return NULL;
    kry_json_buf_raw(&body, "{\"model\":");
    for(i = 0; i < count; i++)
        if(messages[i].image_b64 != NULL && messages[i].image_b64[0] != '\0')
            break;
    kry_json_buf_str(&body, i < count ? krait_ai_vision_model()
                                      : krait_ai_model());
    kry_json_buf_raw(&body, ",\"messages\":[");
    for(i = 0; i < count; i++) {
        if(i > 0)
            kry_json_buf_raw(&body, ",");
        kry_json_buf_raw(&body, "{\"role\":");
        kry_json_buf_str(&body, messages[i].role);
        kry_json_buf_raw(&body, ",\"content\":");
        if(messages[i].image_b64 != NULL && messages[i].image_b64[0] != '\0') {
            /* multimodal content parts: text + data-URL image */
            kry_json_buf_raw(&body, "[{\"type\":\"text\",\"text\":");
            kry_json_buf_str(&body, messages[i].content);
            kry_json_buf_raw(&body,
                             "},{\"type\":\"image_url\",\"image_url\":"
                             "{\"url\":\"data:image/png;base64,");
            kry_json_buf_raw(&body, messages[i].image_b64);
            kry_json_buf_raw(&body, "\"}}]");
        } else {
            kry_json_buf_str(&body, messages[i].content);
        }
        kry_json_buf_raw(&body, "}");
    }
    if(getenv("KRAIT_AI_STREAM") == NULL ||
       getenv("KRAIT_AI_STREAM")[0] != '0')
        kry_json_buf_raw(&body, "],\"stream\":true,"
                                 "\"thinking\":{\"type\":\"disabled\"}}");
    else
        kry_json_buf_raw(&body, "],\"thinking\":{\"type\":\"disabled\"}}");
    {
        char *out = strdup(kry_json_buf_finish(&body));

        kry_json_buf_free(&body);
        return out;
    }
}

KraitAiRequest *
krait_ai_chat(const KraitAiMessage *messages, int count, int timeout_s)
{
    const char *key = krait_ai_key();
    const char *base = getenv("ZAI_BASE_URL");
    KraitAiRequest *r;
    char url[1024];

    if(key == NULL || key[0] == '\0' || messages == NULL || count <= 0)
        return NULL;
    if(base == NULL || base[0] == '\0')
        base = KRAIT_AI_DEFAULT_BASE;
    r = calloc(1, sizeof(*r));
    if(r == NULL)
        return NULL;
    r->request_body = krait_ai_build_body(messages, count);
    if(r->request_body == NULL) {
        free(r);
        return NULL;
    }
    snprintf(url, sizeof(url), "%s/chat/completions", base);
    r->http = kry_http_post_json(url, key, r->request_body,
                                 timeout_s > 0 ? timeout_s : 180);
    if(r->http == NULL) {
        free(r->request_body);
        free(r);
        return NULL;
    }
    return r;
}

KraitAiStatus
krait_ai_poll(KraitAiRequest *r)
{
    if(r == NULL)
        return KRAIT_AI_FAILED;
    switch(kry_http_poll(r->http)) {
    case KRY_HTTP_DONE: return KRAIT_AI_DONE;
    case KRY_HTTP_FAILED: return KRAIT_AI_FAILED;
    default: return KRAIT_AI_RUNNING;
    }
}

/* last response's usage line, e.g. "in 1234 / out 567 tokens" */
static char g_krait_ai_usage[96];

const char *
krait_ai_last_usage(void)
{
    return g_krait_ai_usage;
}

static void
krait_ai_extract_usage(const char *response)
{
    KryJson *root = kry_json_parse(response);
    KryJson *usage;
    double in;
    double out;

    g_krait_ai_usage[0] = '\0';
    if(root == NULL)
        return;
    usage = kry_json_get(root, "usage");
    in = kry_json_number(kry_json_get(usage, "prompt_tokens"));
    out = kry_json_number(kry_json_get(usage, "completion_tokens"));
    if(in > 0 || out > 0)
        snprintf(g_krait_ai_usage, sizeof(g_krait_ai_usage),
                 "%.0f in / %.0f out tokens", in, out);
    kry_json_free(root);
}

/* Concatenate choices[0].delta.content from every complete SSE "data:"
 * line. Works on full and partial bodies alike; returns a malloc'd string
 * (empty when nothing has arrived yet). */
static char *
krait_ai_sse_text(const char *body)
{
    char *out = malloc(8192);
    size_t used = 0;
    const char *p = body;

    if(out == NULL)
        return NULL;
    out[0] = '\0';
    if(body == NULL)
        return out;
    while((p = strstr(p, "data:")) != NULL) {
        const char *line = p + 5;
        const char *nl = strchr(line, '\n');
        char chunk[1024];
        size_t len;

        p = nl != NULL ? nl + 1 : line + strlen(line);
        if(nl == NULL)
            continue;   /* incomplete line: wait for more bytes */
        len = (size_t)(nl - line);
        if(len >= sizeof(chunk))
            len = sizeof(chunk) - 1;
        memcpy(chunk, line, len);
        chunk[len] = '\0';
        {
            char *trimmed = chunk;

            while(*trimmed == ' ')
                trimmed++;
            if(strncmp(trimmed, "[DONE]", 6) == 0)
                break;
            {
                KryJson *root = kry_json_parse(trimmed);
                KryJson *delta;
                const char *piece;

                if(root == NULL)
                    continue;
                delta = kry_json_get(root, "choices");
                delta = kry_json_at(delta, 0);
                delta = kry_json_get(delta, "delta");
                piece = kry_json_string(kry_json_get(delta, "content"));
                if(piece != NULL && used + strlen(piece) < 8192 - 1) {
                    memcpy(out + used, piece, strlen(piece));
                    used += strlen(piece);
                    out[used] = '\0';
                }
                kry_json_free(root);
            }
        }
    }
    return out;
}

/* Live text so far: parses the partial body while the request runs. */
char *
krait_ai_stream_text(KraitAiRequest *r)
{
    char partial[16384];

    if(r == NULL)
        return NULL;
    if(kry_http_partial(r->http, partial, sizeof(partial)) == 0)
        return NULL;
    if(strstr(partial, "data:") == NULL)
        return NULL;   /* not an SSE body (or nothing yet) */
    return krait_ai_sse_text(partial);
}

static int
krait_ai_extract_text(const char *response, char **out)
{
    KryJson *root;
    KryJson *content;
    const char *text;

    *out = NULL;
    if(response == NULL)
        return 0;
    /* streaming responses arrive as SSE; concatenate the deltas */
    if(strstr(response, "data:") == response ||
       strstr(response, "\ndata:") != NULL) {
        char *joined = krait_ai_sse_text(response);

        if(joined == NULL)
            return 0;
        if(joined[0] == '\0') {
            free(joined);
            return 0;
        }
        *out = joined;
        return 1;
    }
    root = kry_json_parse(response);
    if(root == NULL)
        return 0;
    content = kry_json_get(root, "choices");
    content = kry_json_at(content, 0);
    content = kry_json_get(content, "message");
    content = kry_json_get(content, "content");
    text = kry_json_string(content);
    if(text == NULL) {
        kry_json_free(root);
        return 0;
    }
    *out = strdup(text);
    kry_json_free(root);
    return *out != NULL;
}

const char *
krait_ai_text(KraitAiRequest *r)
{
    char *text;

    if(r == NULL || kry_http_poll(r->http) != KRY_HTTP_DONE)
        return NULL;
    if(r->text != NULL)
        return r->text;
    krait_ai_extract_usage(kry_http_response(r->http));
    if(krait_ai_extract_text(kry_http_response(r->http), &text)) {
        r->text = text;
        return r->text;
    }
    return NULL;
}

const char *
krait_ai_error(KraitAiRequest *r)
{
    static char msg[256];
    const char *response;

    if(r == NULL || kry_http_poll(r->http) != KRY_HTTP_FAILED)
        return NULL;
    response = kry_http_response(r->http);
    snprintf(msg, sizeof(msg), "%s",
             response != NULL ? response : "request failed");
    return msg;
}

const char *
krait_ai_request_body(KraitAiRequest *r)
{
    return r != NULL ? r->request_body : NULL;
}

void
krait_ai_free(KraitAiRequest *r)
{
    if(r == NULL)
        return;
    kry_http_free(r->http);
    free(r->request_body);
    free(r->text);
    free(r);
}
