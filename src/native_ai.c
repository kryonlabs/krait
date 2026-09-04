/*
 * native_ai.c - krait's provider-selectable coding chat client.
 *
 * Krait is the harness, so provider specifics live here.
 * Codex/OpenAI uses Responses.
 * Z.ai uses OpenAI-compatible chat completions.
 * Claude uses Anthropic's native Messages API.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include "kry_http.h"
#include "kry_json.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(HAS_LIBCURL) && !defined(__EMSCRIPTEN__)
#include <curl/curl.h>
#endif

#define KRAIT_AI_PROVIDER_CODEX 0
#define KRAIT_AI_PROVIDER_ZAI 1
#define KRAIT_AI_PROVIDER_CLAUDE 2

#define KRAIT_AI_API_CHAT 0
#define KRAIT_AI_API_RESPONSES 1
#define KRAIT_AI_API_CLAUDE_MESSAGES 2

typedef struct {
    const char *id;
    const char *name;
    const char *key_env;
    const char *base_env;
    const char *model_env;
    const char *vision_model_env;
    const char *default_base;
    const char *default_model;
    const char *default_vision_model;
    int api_kind;
} KraitAiProvider;

static const KraitAiProvider krait_ai_providers[] = {
    {
        "codex", "Codex", "OPENAI_API_KEY", "OPENAI_BASE_URL",
        "CODEX_MODEL", "CODEX_VISION_MODEL",
        "https://api.openai.com/v1", "gpt-5.3-codex",
        "gpt-5.3-codex", KRAIT_AI_API_RESPONSES
    },
    {
        "zai", "Z.ai", "ZAI_API_KEY", "ZAI_BASE_URL",
        "ZAI_MODEL", "ZAI_VISION_MODEL",
        "https://api.z.ai/api/coding/paas/v4", "glm-5.3",
        "glm-5v-turbo",
        KRAIT_AI_API_CHAT
    },
    {
        "claude", "Claude", "ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL",
        "CLAUDE_MODEL", "CLAUDE_VISION_MODEL",
        "https://api.anthropic.com/v1", "claude-sonnet-4-6",
        "claude-sonnet-4-6", KRAIT_AI_API_CLAUDE_MESSAGES
    },
};

#define KRAIT_AI_PROVIDER_COUNT \
    ((int)(sizeof(krait_ai_providers) / sizeof(krait_ai_providers[0])))

struct KraitAiRequest {
    KryHttpRequest *http;
    char *request_body;
    char *text;
#if defined(HAS_LIBCURL) && !defined(__EMSCRIPTEN__)
    KryThread thread;
    KryMutex mutex;
    int native_started;
    int native_finished;
    KraitAiStatus native_state;
    int native_status_code;
    char *native_response;
    char *native_url;
    char *native_key;
    long native_timeout_s;
#endif
};

static char g_key_override[KRAIT_AI_PROVIDER_COUNT][256];
static char g_base_override[KRAIT_AI_PROVIDER_COUNT][512];
static char g_model_override[KRAIT_AI_PROVIDER_COUNT][128];
static int g_active_provider = -1;
static int g_active_effort = -1;
static int g_config_loaded;

static const char *const krait_ai_effort_ids[] = {
    "low", "medium", "high", "xhigh"
};

static const char *const krait_ai_effort_names[] = {
    "Low", "Medium", "High", "Max"
};

#define KRAIT_AI_EFFORT_COUNT \
    ((int)(sizeof(krait_ai_effort_ids) / sizeof(krait_ai_effort_ids[0])))

static int krait_ai_provider_index_from_name(const char *name);
static const char *krait_ai_env_nonempty(const char *name);
static int krait_ai_effort_index_from_name(const char *name);
static int krait_ai_effort_index(void);
static int krait_ai_active_index(void);
static const char *krait_ai_zai_effort(void);
static int krait_ai_stream_enabled(void);

static void
krait_ai_config_path(char *dst, size_t dst_size)
{
    const char *home = getenv("HOME");

    if(home == NULL || home[0] == '\0')
        snprintf(dst, dst_size, ".kryon/krait/ai.conf");
    else
        snprintf(dst, dst_size, "%s/.kryon/krait/ai.conf", home);
}

static void
krait_ai_config_dir(char *dst, size_t dst_size)
{
    const char *home = getenv("HOME");

    if(home == NULL || home[0] == '\0')
        snprintf(dst, dst_size, ".kryon/krait");
    else
        snprintf(dst, dst_size, "%s/.kryon/krait", home);
}

static char *
krait_ai_trim(char *s)
{
    char *end;

    if(s == NULL)
        return s;
    while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + strlen(s);
    while(end > s &&
          (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
           end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static void
krait_ai_load_config(void)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;
    char *line;
    const char *env_provider;
    const char *env_effort;

    if(g_config_loaded)
        return;
    g_config_loaded = 1;
    krait_ai_config_path(path, sizeof(path));
    if(!krait_read_file_alloc(path, &text, &len) || text == NULL)
        return;

    env_provider = krait_ai_env_nonempty("KRAIT_AI_PROVIDER");
    if(env_provider == NULL)
        env_provider = krait_ai_env_nonempty("AI_PROVIDER");
    env_effort = krait_ai_env_nonempty("KRAIT_AI_REASONING_EFFORT");
    if(env_effort == NULL)
        env_effort = krait_ai_env_nonempty("AI_REASONING_EFFORT");

    line = strtok(text, "\n");
    while(line != NULL) {
        char *eq = strchr(line, '=');

        if(eq != NULL) {
            char *key;
            char *value;
            int index;

            *eq = '\0';
            key = krait_ai_trim(line);
            value = krait_ai_trim(eq + 1);
            if(strcmp(key, "provider") == 0 && env_provider == NULL) {
                index = krait_ai_provider_index_from_name(value);
                if(index >= 0)
                    g_active_provider = index;
            } else if(strcmp(key, "effort") == 0 && env_effort == NULL) {
                index = krait_ai_effort_index_from_name(value);
                if(index >= 0)
                    g_active_effort = index;
            } else if(strncmp(key, "key.", 4) == 0) {
                index = krait_ai_provider_index_from_name(key + 4);
                if(index >= 0)
                    snprintf(g_key_override[index],
                             sizeof(g_key_override[index]), "%s", value);
            } else if(strncmp(key, "base.", 5) == 0) {
                index = krait_ai_provider_index_from_name(key + 5);
                if(index >= 0)
                    snprintf(g_base_override[index],
                             sizeof(g_base_override[index]), "%s", value);
            } else if(strncmp(key, "model.", 6) == 0) {
                index = krait_ai_provider_index_from_name(key + 6);
                if(index >= 0)
                    snprintf(g_model_override[index],
                             sizeof(g_model_override[index]), "%s", value);
            }
        }
        line = strtok(NULL, "\n");
    }
    free(text);
}

static void
krait_ai_save_config(void)
{
    char dir[KRAIT_PATH_MAX];
    char path[KRAIT_PATH_MAX];
    FILE *file;
    int i;

    krait_ai_load_config();
    krait_ai_config_dir(dir, sizeof(dir));
    if(!krait_mkdir_p(dir))
        return;
    krait_ai_config_path(path, sizeof(path));
    file = fopen(path, "wb");
    if(file == NULL)
        return;
    fprintf(file, "provider=%s\n",
            krait_ai_providers[krait_ai_active_index()].id);
    fprintf(file, "effort=%s\n", krait_ai_effort_ids[krait_ai_effort_index()]);
    for(i = 0; i < KRAIT_AI_PROVIDER_COUNT; i++)
        if(g_key_override[i][0] != '\0')
            fprintf(file, "key.%s=%s\n", krait_ai_providers[i].id,
                    g_key_override[i]);
    for(i = 0; i < KRAIT_AI_PROVIDER_COUNT; i++)
        if(g_base_override[i][0] != '\0')
            fprintf(file, "base.%s=%s\n", krait_ai_providers[i].id,
                    g_base_override[i]);
    for(i = 0; i < KRAIT_AI_PROVIDER_COUNT; i++)
        if(g_model_override[i][0] != '\0')
            fprintf(file, "model.%s=%s\n", krait_ai_providers[i].id,
                    g_model_override[i]);
    fclose(file);
    (void)chmod(path, 0600);
}

static int
krait_ai_provider_index_from_name(const char *name)
{
    int i;

    if(name == NULL || name[0] == '\0')
        return -1;
    for(i = 0; i < KRAIT_AI_PROVIDER_COUNT; i++) {
        if(strcmp(name, krait_ai_providers[i].id) == 0 ||
           strcmp(name, krait_ai_providers[i].name) == 0)
            return i;
    }
    if(strcmp(name, "openai") == 0 || strcmp(name, "gpt") == 0)
        return KRAIT_AI_PROVIDER_CODEX;
    if(strcmp(name, "z.ai") == 0 || strcmp(name, "glm") == 0)
        return KRAIT_AI_PROVIDER_ZAI;
    if(strcmp(name, "anthropic") == 0)
        return KRAIT_AI_PROVIDER_CLAUDE;
    return -1;
}

static const char *
krait_ai_env_nonempty(const char *name)
{
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0' ? value : NULL;
}

static int
krait_ai_default_provider(void)
{
    const char *configured = krait_ai_env_nonempty("KRAIT_AI_PROVIDER");
    int index;

    if(configured == NULL)
        configured = krait_ai_env_nonempty("AI_PROVIDER");
    index = krait_ai_provider_index_from_name(configured);
    if(index >= 0)
        return index;
    if(krait_ai_env_nonempty("OPENAI_API_KEY") != NULL)
        return KRAIT_AI_PROVIDER_CODEX;
    if(krait_ai_env_nonempty("ZAI_API_KEY") != NULL)
        return KRAIT_AI_PROVIDER_ZAI;
    if(krait_ai_env_nonempty("ANTHROPIC_API_KEY") != NULL)
        return KRAIT_AI_PROVIDER_CLAUDE;
    return KRAIT_AI_PROVIDER_ZAI;
}

static int
krait_ai_effort_index_from_name(const char *name)
{
    int i;

    if(name == NULL || name[0] == '\0')
        return -1;
    for(i = 0; i < KRAIT_AI_EFFORT_COUNT; i++)
        if(strcmp(name, krait_ai_effort_ids[i]) == 0 ||
           strcmp(name, krait_ai_effort_names[i]) == 0)
            return i;
    if(strcmp(name, "max") == 0)
        return KRAIT_AI_EFFORT_COUNT - 1;
    return -1;
}

static int
krait_ai_default_effort(void)
{
    const char *configured = krait_ai_env_nonempty("KRAIT_AI_REASONING_EFFORT");
    int index;

    if(configured == NULL)
        configured = krait_ai_env_nonempty("AI_REASONING_EFFORT");
    index = krait_ai_effort_index_from_name(configured);
    return index >= 0 ? index : KRAIT_AI_EFFORT_COUNT - 1;
}

static const char *
krait_ai_zai_effort(void)
{
    switch(krait_ai_effort_index()) {
    case 0: return "low";
    case 1:
    case 2: return "high";
    default: return "max";
    }
}

static int
krait_ai_stream_enabled(void)
{
    const char *stream = getenv("KRAIT_AI_STREAM");

    return stream == NULL || stream[0] != '0';
}

static int
krait_ai_effort_index(void)
{
    if(g_active_effort >= 0 && g_active_effort < KRAIT_AI_EFFORT_COUNT)
        return g_active_effort;
    return krait_ai_default_effort();
}

static int
krait_ai_active_index(void)
{
    krait_ai_load_config();
    if(g_active_provider >= 0 && g_active_provider < KRAIT_AI_PROVIDER_COUNT)
        return g_active_provider;
    return krait_ai_default_provider();
}

int
krait_ai_provider_count(void)
{
    return KRAIT_AI_PROVIDER_COUNT;
}

const char *
krait_ai_provider_id(int index)
{
    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        return "";
    return krait_ai_providers[index].id;
}

const char *
krait_ai_provider_name(int index)
{
    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        return "";
    return krait_ai_providers[index].name;
}

const char *
krait_ai_provider_key_env(int index)
{
    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        return "";
    return krait_ai_providers[index].key_env;
}

int
krait_ai_active_provider(void)
{
    return krait_ai_active_index();
}

int
krait_ai_set_provider(int index)
{
    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        return 0;
    krait_ai_load_config();
    g_active_provider = index;
    krait_ai_save_config();
    return 1;
}

int
krait_ai_effort_count(void)
{
    return KRAIT_AI_EFFORT_COUNT;
}

const char *
krait_ai_effort_name(int index)
{
    if(index < 0 || index >= KRAIT_AI_EFFORT_COUNT)
        return "";
    return krait_ai_effort_names[index];
}

int
krait_ai_active_effort(void)
{
    krait_ai_load_config();
    return krait_ai_effort_index();
}

int
krait_ai_set_effort(int index)
{
    if(index < 0 || index >= KRAIT_AI_EFFORT_COUNT)
        return 0;
    krait_ai_load_config();
    g_active_effort = index;
    krait_ai_save_config();
    return 1;
}

void
krait_ai_set_provider_key(int index, const char *api_key)
{
    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        return;
    krait_ai_load_config();
    if(api_key == NULL)
        api_key = "";
    snprintf(g_key_override[index], sizeof(g_key_override[index]), "%s",
             api_key);
    krait_ai_save_config();
}

void
krait_ai_set_provider_base_url(int index, const char *base_url)
{
    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        return;
    krait_ai_load_config();
    if(base_url == NULL)
        base_url = "";
    snprintf(g_base_override[index], sizeof(g_base_override[index]), "%s",
             base_url);
    krait_ai_save_config();
}

void
krait_ai_set_provider_model(int index, const char *model)
{
    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        return;
    krait_ai_load_config();
    if(model == NULL)
        model = "";
    snprintf(g_model_override[index], sizeof(g_model_override[index]), "%s",
             model);
    krait_ai_save_config();
}

void
krait_ai_set_key(const char *api_key)
{
    krait_ai_set_provider_key(krait_ai_active_index(), api_key);
}

static const char *
krait_ai_key_for(int index)
{
    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        return NULL;
    krait_ai_load_config();
    if(g_key_override[index][0] != '\0')
        return g_key_override[index];
    return getenv(krait_ai_providers[index].key_env);
}

int
krait_ai_provider_configured(int index)
{
    const char *key = krait_ai_key_for(index);

    return key != NULL && key[0] != '\0';
}

int
krait_ai_configured(void)
{
    return krait_ai_provider_configured(krait_ai_active_index());
}

const char *
krait_ai_config_hint(void)
{
    return "AI off: set OPENAI_API_KEY, ZAI_API_KEY, or ANTHROPIC_API_KEY";
}

static const char *
krait_ai_model_for(int index)
{
    const char *model;

    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        index = krait_ai_active_index();
    krait_ai_load_config();
    if(g_model_override[index][0] != '\0')
        return g_model_override[index];
    model = getenv(krait_ai_providers[index].model_env);
    return model != NULL && model[0] != '\0'
             ? model : krait_ai_providers[index].default_model;
}

static const char *
krait_ai_vision_model_for(int index)
{
    const char *model;

    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        index = krait_ai_active_index();
    krait_ai_load_config();
    model = getenv(krait_ai_providers[index].vision_model_env);
    if(model != NULL && model[0] != '\0')
        return model;
    if(g_model_override[index][0] != '\0')
        return g_model_override[index];
    model = getenv(krait_ai_providers[index].model_env);
    if(model != NULL && model[0] != '\0')
        return model;
    return krait_ai_providers[index].default_vision_model;
}

const char *
krait_ai_provider_model(int index)
{
    return krait_ai_model_for(index);
}

const char *
krait_ai_provider_base_url(int index)
{
    const char *base;

    if(index < 0 || index >= KRAIT_AI_PROVIDER_COUNT)
        index = krait_ai_active_index();
    krait_ai_load_config();
    if(g_base_override[index][0] != '\0')
        return g_base_override[index];
    base = getenv(krait_ai_providers[index].base_env);
    return base != NULL && base[0] != '\0'
             ? base : krait_ai_providers[index].default_base;
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

static void
krait_ai_content_part(KryJsonBuf *body, const KraitAiMessage *message,
                      int responses_api)
{
    if(message->image_b64 != NULL && message->image_b64[0] != '\0') {
        kry_json_buf_raw(body, "[{\"type\":");
        kry_json_buf_str(body, responses_api ? "input_text" : "text");
        kry_json_buf_raw(body, ",\"text\":");
        kry_json_buf_str(body, message->content);
        kry_json_buf_raw(body, "},{\"type\":");
        kry_json_buf_str(body, responses_api ? "input_image" : "image_url");
        if(responses_api) {
            kry_json_buf_raw(body, ",\"image_url\":\"data:image/png;base64,");
            kry_json_buf_raw(body, message->image_b64);
            kry_json_buf_raw(body, "\"}]");
        } else {
            kry_json_buf_raw(body, ",\"image_url\":{\"url\":\"data:image/png;base64,");
            kry_json_buf_raw(body, message->image_b64);
            kry_json_buf_raw(body, "\"}}]");
        }
    } else if(responses_api) {
        kry_json_buf_raw(body, "[{\"type\":\"input_text\",\"text\":");
        kry_json_buf_str(body, message->content);
        kry_json_buf_raw(body, "}]");
    } else {
        kry_json_buf_str(body, message->content);
    }
}

static void
krait_ai_claude_content_part(KryJsonBuf *body, const KraitAiMessage *message)
{
    if(message->image_b64 != NULL && message->image_b64[0] != '\0') {
        kry_json_buf_raw(body, "[{\"type\":\"image\",\"source\":{");
        kry_json_buf_raw(body, "\"type\":\"base64\",\"media_type\":\"image/png\",");
        kry_json_buf_raw(body, "\"data\":\"");
        kry_json_buf_raw(body, message->image_b64);
        kry_json_buf_raw(body, "\"}},{\"type\":\"text\",\"text\":");
        kry_json_buf_str(body, message->content);
        kry_json_buf_raw(body, "}]");
    } else {
        kry_json_buf_str(body, message->content);
    }
}

static char *
krait_ai_build_chat_body(const KraitAiMessage *messages, int count,
                         int provider)
{
    KryJsonBuf body = {0};
    int i;

    kry_json_buf_raw(&body, "{\"model\":");
    for(i = 0; i < count; i++)
        if(messages[i].image_b64 != NULL && messages[i].image_b64[0] != '\0')
            break;
    kry_json_buf_str(&body, i < count ? krait_ai_vision_model_for(provider)
                                      : krait_ai_model_for(provider));
    kry_json_buf_raw(&body, ",\"messages\":[");
    for(i = 0; i < count; i++) {
        if(i > 0)
            kry_json_buf_raw(&body, ",");
        kry_json_buf_raw(&body, "{\"role\":");
        kry_json_buf_str(&body, messages[i].role);
        kry_json_buf_raw(&body, ",\"content\":");
        krait_ai_content_part(&body, &messages[i], 0);
        kry_json_buf_raw(&body, "}");
    }
    if(krait_ai_stream_enabled())
        kry_json_buf_raw(&body, "],\"stream\":true");
    else
        kry_json_buf_raw(&body, "]");
    if(provider == KRAIT_AI_PROVIDER_ZAI) {
        kry_json_buf_raw(&body, ",\"thinking\":{\"type\":\"enabled\"}");
        kry_json_buf_raw(&body, ",\"reasoning_effort\":");
        kry_json_buf_str(&body, krait_ai_zai_effort());
    }
    kry_json_buf_raw(&body, "}");
    {
        char *out = strdup(kry_json_buf_finish(&body));

        kry_json_buf_free(&body);
        return out;
    }
}

static char *
krait_ai_build_claude_body(const KraitAiMessage *messages, int count,
                           int provider)
{
    KryJsonBuf body = {0};
    const char *max_tokens = getenv("CLAUDE_MAX_TOKENS");
    int i;
    int first = 1;

    kry_json_buf_raw(&body, "{\"model\":");
    for(i = 0; i < count; i++)
        if(messages[i].image_b64 != NULL && messages[i].image_b64[0] != '\0')
            break;
    kry_json_buf_str(&body, i < count ? krait_ai_vision_model_for(provider)
                                      : krait_ai_model_for(provider));
    kry_json_buf_raw(&body, ",\"max_tokens\":");
    if(max_tokens != NULL && max_tokens[0] != '\0')
        kry_json_buf_raw(&body, max_tokens);
    else
        kry_json_buf_raw(&body, "8192");
    for(i = 0; i < count; i++) {
        if(strcmp(messages[i].role, "system") == 0) {
            kry_json_buf_raw(&body, ",\"system\":");
            kry_json_buf_str(&body, messages[i].content);
            break;
        }
    }
    kry_json_buf_raw(&body, ",\"messages\":[");
    for(i = 0; i < count; i++) {
        const char *role = messages[i].role;

        if(strcmp(role, "system") == 0)
            continue;
        if(!first)
            kry_json_buf_raw(&body, ",");
        first = 0;
        kry_json_buf_raw(&body, "{\"role\":");
        kry_json_buf_str(&body, strcmp(role, "assistant") == 0 ? "assistant"
                                                               : "user");
        kry_json_buf_raw(&body, ",\"content\":");
        krait_ai_claude_content_part(&body, &messages[i]);
        kry_json_buf_raw(&body, "}");
    }
    kry_json_buf_raw(&body, "]");
    if(krait_ai_stream_enabled())
        kry_json_buf_raw(&body, ",\"stream\":true");
    kry_json_buf_raw(&body, "}");
    {
        char *out = strdup(kry_json_buf_finish(&body));

        kry_json_buf_free(&body);
        return out;
    }
}

static char *
krait_ai_build_responses_body(const KraitAiMessage *messages, int count,
                              int provider)
{
    KryJsonBuf body = {0};
    int i;
    int first_input = 1;

    kry_json_buf_raw(&body, "{\"model\":");
    for(i = 0; i < count; i++)
        if(messages[i].image_b64 != NULL && messages[i].image_b64[0] != '\0')
            break;
    kry_json_buf_str(&body, i < count ? krait_ai_vision_model_for(provider)
                                      : krait_ai_model_for(provider));
    for(i = 0; i < count; i++) {
        if(strcmp(messages[i].role, "system") == 0) {
            kry_json_buf_raw(&body, ",\"instructions\":");
            kry_json_buf_str(&body, messages[i].content);
            break;
        }
    }
    kry_json_buf_raw(&body, ",\"input\":[");
    for(i = 0; i < count; i++) {
        const char *role = messages[i].role;

        if(strcmp(role, "system") == 0)
            continue;
        if(!first_input)
            kry_json_buf_raw(&body, ",");
        first_input = 0;
        kry_json_buf_raw(&body, "{\"role\":");
        kry_json_buf_str(&body, strcmp(role, "assistant") == 0 ? "assistant"
                                                               : "user");
        kry_json_buf_raw(&body, ",\"content\":");
        krait_ai_content_part(&body, &messages[i], 1);
        kry_json_buf_raw(&body, "}");
    }
    kry_json_buf_raw(&body, "],\"reasoning\":{\"effort\":");
    kry_json_buf_str(&body, krait_ai_effort_ids[krait_ai_effort_index()]);
    kry_json_buf_raw(&body, "}}");
    {
        char *out = strdup(kry_json_buf_finish(&body));

        kry_json_buf_free(&body);
        return out;
    }
}

/* Pure request-body builder so tests can verify multimodality without
 * spending a request. malloc'd JSON, caller frees. */
char *
krait_ai_build_body(const KraitAiMessage *messages, int count)
{
    int provider = krait_ai_active_index();

    if(messages == NULL || count <= 0)
        return NULL;
    if(krait_ai_providers[provider].api_kind == KRAIT_AI_API_RESPONSES)
        return krait_ai_build_responses_body(messages, count, provider);
    if(krait_ai_providers[provider].api_kind == KRAIT_AI_API_CLAUDE_MESSAGES)
        return krait_ai_build_claude_body(messages, count, provider);
    return krait_ai_build_chat_body(messages, count, provider);
}

#if defined(HAS_LIBCURL) && !defined(__EMSCRIPTEN__)
static size_t
krait_ai_native_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    KraitAiRequest *r = userdata;
    size_t n = size * nmemb;
    size_t have;
    char *next;

    KryMutexLock(&r->mutex);
    have = r->native_response != NULL ? strlen(r->native_response) : 0;
    next = realloc(r->native_response, have + n + 1);
    if(next == NULL) {
        KryMutexUnlock(&r->mutex);
        return 0;
    }
    r->native_response = next;
    memcpy(r->native_response + have, ptr, n);
    r->native_response[have + n] = '\0';
    KryMutexUnlock(&r->mutex);
    return n;
}

static void
krait_ai_native_finish(KraitAiRequest *r, KraitAiStatus state, int code,
                       const char *message)
{
    KryMutexLock(&r->mutex);
    r->native_state = state;
    r->native_status_code = code;
    if(message != NULL &&
       (r->native_response == NULL || r->native_response[0] == '\0')) {
        free(r->native_response);
        r->native_response = strdup(message);
    }
    r->native_finished = 1;
    KryMutexUnlock(&r->mutex);
}

static void *
krait_ai_native_worker(void *userdata)
{
    KraitAiRequest *r = userdata;
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    CURLcode rc;

    if(curl == NULL) {
        krait_ai_native_finish(r, KRAIT_AI_FAILED, 0,
                               "curl_easy_init failed");
        return NULL;
    }
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    {
        char key_header[768];

        snprintf(key_header, sizeof(key_header), "x-api-key: %s",
                 r->native_key != NULL ? r->native_key : "");
        headers = curl_slist_append(headers, key_header);
    }
    curl_easy_setopt(curl, CURLOPT_URL, r->native_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r->request_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     (long)strlen(r->request_body));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, r->native_timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, krait_ai_native_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "krait/1");
    rc = curl_easy_perform(curl);
    if(rc == CURLE_OK) {
        long code = 0;

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if(code >= 200 && code < 300) {
            krait_ai_native_finish(r, KRAIT_AI_DONE, (int)code, NULL);
        } else {
            char msg[64];

            snprintf(msg, sizeof(msg), "HTTP %ld", code);
            krait_ai_native_finish(r, KRAIT_AI_FAILED, (int)code, msg);
        }
    } else {
        char msg[128];

        snprintf(msg, sizeof(msg), "curl error %d: %s", (int)rc,
                 curl_easy_strerror(rc));
        krait_ai_native_finish(r, KRAIT_AI_FAILED, 0, msg);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return NULL;
}

static int
krait_ai_start_native_claude(KraitAiRequest *r, const char *url,
                             const char *key, int timeout_s)
{
    r->native_url = strdup(url);
    r->native_key = strdup(key);
    if(r->native_url == NULL || r->native_key == NULL)
        return 0;
    r->native_timeout_s = timeout_s > 0 ? timeout_s : 180;
    r->native_state = KRAIT_AI_PENDING;
    KryMutexInit(&r->mutex);
    if(!KryThreadStart(&r->thread, krait_ai_native_worker, r))
        return 0;
    r->native_started = 1;
    return 1;
}
#endif

KraitAiRequest *
krait_ai_chat(const KraitAiMessage *messages, int count, int timeout_s)
{
    int provider = krait_ai_active_index();
    const char *key = krait_ai_key_for(provider);
    const char *base = krait_ai_provider_base_url(provider);
    KraitAiRequest *r;
    char url[1024];

    if(key == NULL || key[0] == '\0' || messages == NULL || count <= 0)
        return NULL;
    r = calloc(1, sizeof(*r));
    if(r == NULL)
        return NULL;
    r->request_body = krait_ai_build_body(messages, count);
    if(r->request_body == NULL) {
        free(r);
        return NULL;
    }
    snprintf(url, sizeof(url), "%s/%s", base,
             krait_ai_providers[provider].api_kind == KRAIT_AI_API_RESPONSES
               ? "responses" :
             krait_ai_providers[provider].api_kind ==
               KRAIT_AI_API_CLAUDE_MESSAGES ? "messages" :
               "chat/completions");
    if(krait_ai_providers[provider].api_kind ==
       KRAIT_AI_API_CLAUDE_MESSAGES) {
#if defined(HAS_LIBCURL) && !defined(__EMSCRIPTEN__)
        if(!krait_ai_start_native_claude(r, url, key, timeout_s)) {
            free(r->native_url);
            free(r->native_key);
            free(r->request_body);
            free(r);
            return NULL;
        }
        return r;
#else
        free(r->request_body);
        free(r);
        return NULL;
#endif
    }
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
#if defined(HAS_LIBCURL) && !defined(__EMSCRIPTEN__)
    if(r->native_started) {
        KraitAiStatus state;

        KryMutexLock(&r->mutex);
        state = r->native_finished ? r->native_state : KRAIT_AI_RUNNING;
        KryMutexUnlock(&r->mutex);
        return state;
    }
#endif
    switch(kry_http_poll(r->http)) {
    case KRY_HTTP_DONE: return KRAIT_AI_DONE;
    case KRY_HTTP_FAILED: return KRAIT_AI_FAILED;
    default: return KRAIT_AI_RUNNING;
    }
}

static const char *
krait_ai_response(KraitAiRequest *r)
{
    if(r == NULL)
        return NULL;
#if defined(HAS_LIBCURL) && !defined(__EMSCRIPTEN__)
    if(r->native_started) {
        const char *response;

        KryMutexLock(&r->mutex);
        response = r->native_finished
                     ? (r->native_response != NULL ? r->native_response : "")
                     : NULL;
        KryMutexUnlock(&r->mutex);
        return response;
    }
#endif
    return kry_http_response(r->http);
}

static size_t
krait_ai_partial(KraitAiRequest *r, char *buf, size_t size)
{
    if(r == NULL || buf == NULL || size == 0)
        return 0;
#if defined(HAS_LIBCURL) && !defined(__EMSCRIPTEN__)
    if(r->native_started) {
        size_t avail = 0;

        KryMutexLock(&r->mutex);
        if(r->native_response != NULL) {
            avail = strlen(r->native_response);
            if(size > 0) {
                size_t copy = avail < size - 1 ? avail : size - 1;

                memcpy(buf, r->native_response, copy);
                buf[copy] = '\0';
            }
        } else {
            buf[0] = '\0';
        }
        KryMutexUnlock(&r->mutex);
        return avail;
    }
#endif
    return kry_http_partial(r->http, buf, size);
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
    if(in <= 0)
        in = kry_json_number(kry_json_get(usage, "input_tokens"));
    if(out <= 0)
        out = kry_json_number(kry_json_get(usage, "output_tokens"));
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
                if(piece == NULL)
                    piece = kry_json_string(kry_json_get(root, "delta"));
                if(piece == NULL) {
                    delta = kry_json_get(root, "delta");
                    piece = kry_json_string(kry_json_get(delta, "text"));
                }
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
    if(krait_ai_partial(r, partial, sizeof(partial)) == 0)
        return NULL;
    if(strstr(partial, "data:") == NULL)
        return NULL;   /* not an SSE body (or nothing yet) */
    return krait_ai_sse_text(partial);
}

int
krait_ai_extract_response_text(const char *response, char **out)
{
    KryJson *root;
    KryJson *content;
    KryJson *output;
    const char *text;
    int i;
    int j;

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
    text = kry_json_string(kry_json_get(root, "output_text"));
    if(text != NULL) {
        *out = strdup(text);
        kry_json_free(root);
        return *out != NULL;
    }
    output = kry_json_get(root, "output");
    for(i = 0; i < kry_json_count(output); i++) {
        KryJson *item = kry_json_at(output, i);
        KryJson *parts = kry_json_get(item, "content");

        for(j = 0; j < kry_json_count(parts); j++) {
            KryJson *part = kry_json_at(parts, j);

            text = kry_json_string(kry_json_get(part, "text"));
            if(text == NULL)
                text = kry_json_string(kry_json_get(part, "content"));
            if(text != NULL) {
                *out = strdup(text);
                kry_json_free(root);
                return *out != NULL;
            }
        }
    }
    content = kry_json_get(root, "choices");
    content = kry_json_at(content, 0);
    content = kry_json_get(content, "message");
    content = kry_json_get(content, "content");
    text = kry_json_string(content);
    if(text != NULL) {
        *out = strdup(text);
        kry_json_free(root);
        return *out != NULL;
    }
    for(i = 0; i < kry_json_count(content); i++) {
        KryJson *part = kry_json_at(content, i);

        text = kry_json_string(kry_json_get(part, "text"));
        if(text != NULL) {
            *out = strdup(text);
            kry_json_free(root);
            return *out != NULL;
        }
    }
    content = kry_json_get(root, "content");
    for(i = 0; i < kry_json_count(content); i++) {
        KryJson *part = kry_json_at(content, i);

        text = kry_json_string(kry_json_get(part, "text"));
        if(text != NULL) {
            *out = strdup(text);
            kry_json_free(root);
            return *out != NULL;
        }
    }
    kry_json_free(root);
    return 0;
}

const char *
krait_ai_text(KraitAiRequest *r)
{
    char *text;

    if(r == NULL || krait_ai_poll(r) != KRAIT_AI_DONE)
        return NULL;
    if(r->text != NULL)
        return r->text;
    krait_ai_extract_usage(krait_ai_response(r));
    if(krait_ai_extract_response_text(krait_ai_response(r), &text)) {
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

    if(r == NULL || krait_ai_poll(r) != KRAIT_AI_FAILED)
        return NULL;
    response = krait_ai_response(r);
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
#if defined(HAS_LIBCURL) && !defined(__EMSCRIPTEN__)
    if(r->native_started) {
        KryThreadJoin(&r->thread);
        free(r->native_response);
        free(r->native_url);
        free(r->native_key);
    } else
#endif
    {
        kry_http_free(r->http);
    }
    free(r->request_body);
    free(r->text);
    free(r);
}
