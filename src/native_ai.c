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

KraitAiRequest *
krait_ai_chat(const KraitAiMessage *messages, int count, int timeout_s)
{
    const char *key = krait_ai_key();
    const char *base = getenv("ZAI_BASE_URL");
    KraitAiRequest *r;
    KryJsonBuf body = {0};
    char url[1024];
    int i;

    if(key == NULL || key[0] == '\0' || messages == NULL || count <= 0)
        return NULL;
    if(base == NULL || base[0] == '\0')
        base = KRAIT_AI_DEFAULT_BASE;
    kry_json_buf_raw(&body, "{\"model\":");
    kry_json_buf_str(&body, krait_ai_model());
    kry_json_buf_raw(&body, ",\"messages\":[");
    for(i = 0; i < count; i++) {
        if(i > 0)
            kry_json_buf_raw(&body, ",");
        kry_json_buf_raw(&body, "{\"role\":");
        kry_json_buf_str(&body, messages[i].role);
        kry_json_buf_raw(&body, ",\"content\":");
        kry_json_buf_str(&body, messages[i].content);
        kry_json_buf_raw(&body, "}");
    }
    kry_json_buf_raw(&body, "],\"thinking\":{\"type\":\"disabled\"}}");
    r = calloc(1, sizeof(*r));
    if(r == NULL) {
        kry_json_buf_free(&body);
        return NULL;
    }
    r->request_body = strdup(kry_json_buf_finish(&body));
    kry_json_buf_free(&body);
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

static int
krait_ai_extract_text(const char *response, char **out)
{
    KryJson *root = kry_json_parse(response);
    KryJson *content;
    const char *text;

    *out = NULL;
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
